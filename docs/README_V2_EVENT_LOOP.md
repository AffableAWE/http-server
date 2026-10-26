# http-server, v2: thread pool out, event loop in

This covers the second version of the server. v1 was a thread-pooled static file server, one request
per connection, about 21k requests/sec. v2 keeps the module layout and replaces the concurrency model
underneath it.

995 lines became 4,194. Zero tests became 14. The thread pool is still in the tree but nothing calls it.

| | v1 | v2 |
|---|---|---|
| Concurrency | `hardware_concurrency()` blocking workers | one `poll()` event loop thread, plus one log writer |
| Connection lifetime | one request, then close | keep-alive with ordered pipelining |
| Static files | `ifstream` open/seek/read/close per request | preloaded once at startup, bodies shared by pointer |
| Logging | synchronous SQLite insert inside the request, one mutex | bounded queue, batched transactions on a writer thread |
| Slow clients | block a worker forever | header, write, and idle deadlines |
| Overload | unbounded descriptor queue | admission limit, `503`, read backpressure |
| Descriptors | raw ints | `UniqueFd`, one owner each |
| Tests | none | 14 CTest targets, ASan and UBSan clean |
| Lines (`src` + `include`) | 995 | 4,194 |

---

## The order the work happened in

Fourteen batches. Each one had to leave the server working and the tests passing before the next started.

**Safety on the blocking server first.** Before touching the concurrency model, the existing design got
bounds. The first batch added an admission limit, explicit descriptor ownership, and a best-effort `503`
for connections past the cap. The next added absolute header and write deadlines on `steady_clock`, with
the header deadline starting at `accept()` so time spent waiting in the queue counts against the request.
Then startup and logging initialisation were fixed, and a bounded shutdown went in that drains in-flight
work, transfers admission permits, and cancels sockets that outlast the grace period.

Doing this first was the right call. Those bounds do not depend on the concurrency model, and they all
survived the rewrite.

**Then the two things that were obviously slow.** Every allowlisted response is now preloaded at startup
into an immutable `AssetCache`. Bodies are `shared_ptr<const string>`, lookups are const and take no lock,
and a file that exceeds `MAX_ASSET_BYTES` fails startup rather than being discovered at request time.
Logging got two batches. The first split the intended HTTP status from what actually happened on the wire,
added a `transfer_outcome` column with a schema migration, and left historical rows as `unknown`. The
second moved SQLite off the request path entirely: records are copied into a bounded queue and one writer
thread owns the connection and commits in batches.

**A repair pass.** A code review found that the writer woke on every enqueue and flushed immediately, so
the batch interval did nothing, and that batch preparation ran outside the try/catch, so a `bad_alloc` in
the writer would terminate the process. Both confirmed, both fixed.

**Then the rewrite.** The thread pool was replaced with a single `poll()` loop, deliberately keeping
close-per-request at first so the I/O change could be isolated. The next batch added keep-alive,
incremental parsing out of a buffer, and FIFO pipelining with a bound on queued responses. The third added
`tests/http_robustness_tests.py`, which runs normal clients, idle sockets, slow-header sockets, slow
readers, and pipelined clients against the server at the same time and checks the normal ones keep making
progress.

**Then three batches of being suspicious of the numbers.** An audit of the benchmark harness found three
bugs in it: the median helper returned the upper of two runs, RPS was computed from process wall time
instead of the measured test window, and a reported 7,000,000-request figure was internally inconsistent.
The next batch chased a p99 outlier and added per-reason diagnostics counters to the server. The last found
that the `wrk` parser could not read a duration printed as `1.00m` and had been recording those runs as
0 RPS.

Three of the fourteen batches produced no server code at all. They only made the measurements
trustworthy.

---

## What the event loop actually does

One thread owns both listeners and every admitted connection. Each pass:

1. Accept up to 64 connections per listener, then stop, so accepts cannot starve existing connections.
2. Parse whatever is already buffered, at most 2 requests per connection per pass.
3. Build the pollfd set. `POLLIN` only if the connection is allowed to read, `POLLOUT` only while a
   response is pending.
4. `poll()` with a timeout clamped to the nearest deadline, 500 ms at most.
5. Read up to 16 KB per ready connection, parse, queue responses.
6. Write what it can, advancing an offset into the header and the shared body.
7. Sweep deadlines: header, write, idle.
8. Drop finished connections.

A connection stops being read when it has `MAX_PIPELINED_REQUESTS` responses queued or
`MAX_CONNECTION_INPUT_BYTES` buffered. That is the backpressure. A client that pipelines faster than it
drains cannot make the server buffer without limit.

Deadlines are absolute and set from `steady_clock`. The header deadline starts when the request starts and
trickled bytes do not reset it. Each queued response carries its own write deadline from the moment it was
queued, so a slow reader cannot extend it. A header timeout queues one `408` and closes. A write timeout
just closes, because part of the response may already be on the wire.

The parser is strict now. It rejects duplicate `Host`, duplicate `Content-Length`, duplicate
`Transfer-Encoding`, obsolete line folding, non-token characters in the method, anything that is not
HTTP/1.0 or HTTP/1.1, and a missing `Host` on HTTP/1.1. `Transfer-Encoding` returns `501` rather than being
guessed at. Request bodies are refused rather than being read and skipped. The point is that when
connections stay open, getting the boundary between two requests wrong is a request smuggling bug, not a
cosmetic one.

Logging never blocks the loop. The loop copies a bounded record into a queue and moves on. The writer owns
SQLite, commits in explicit transactions, retries a failed batch a bounded number of times, and when the
queue is full it drops the newest record and counts it. At shutdown it reports offered, accepted, committed,
failed, dropped, discarded, peak queue depth, batch count, and flush latency. Dropped records are visible,
not silent.

---

## Numbers

Measured on 26 October 2026. Apple M1, 8 logical cores, macOS 27.0. Release build, `-O3`. Loopback only,
load generator on the same laptop. `GET /`, 1,277-byte body, `wrk` with 1 thread and 100 connections.
Three 60-second runs per mode, each against a fresh server process after a 5-second warm-up. Every run is
reported. Raw `wrk` output and the harness records are in `data/benchmarks/v2-baseline/`.

| Mode | Median RPS | Range | p50 | p99 | Server CPU |
|---|---:|---:|---:|---:|---:|
| Keep-alive, logging off | 179,853 | 179,850 to 180,344 | 0.396 ms | 0.591 ms | 99.6 to 99.8% |
| Keep-alive, logging on | 168,544 | 167,931 to 170,698 | 0.391 ms | 0.742 ms | 152 to 159% |

Logging off: 32.4 million requests across three runs, zero socket errors, zero non-2xx responses, and the
three runs within 0.3% of each other. The harness checked the exact response body before and after every
run. Peak server memory was 3.3 MB.

Logging on: one run saw a single client-side timeout in 10.26 million requests. The harness rejects any
run with a socket error, so it wrote no `result.json` for that one; its raw `wrk` output and server log are
kept beside the others.

Server CPU at 99.6 to 99.8% means the event loop had one core fully busy. The `wrk` thread was also at
about 100%. The two meet at roughly 180k on this laptop, so this is a ceiling of the setup, not of the
server alone. With logging on, the extra half-core is the SQLite writer thread.

Close mode, where every request opens a new connection, measured by hand with
`wrk -t1 -c100 -d10s --latency -H "Connection: close"` and logging off:

| Run | RPS | p50 | p75 | p90 | p99 |
|---|---:|---:|---:|---:|---:|
| 1 | 20,609 | 1.77 ms | 64.82 ms | 182.44 ms | 287.46 ms |
| 2 | 21,456 | 1.75 ms | 53.45 ms | 180.49 ms | 292.80 ms |

**The jump from 21k to 180k is keep-alive, not an 8x speedup on the same test.** v1 always closed the
connection, so it could only ever be measured in close mode. Run v2 the same way and it does about 21k,
the same as v1. Say this before anyone asks, because the close-mode row is the first thing a reviewer will
look for.

What did get faster on its own terms is logging. v1 dropped from 22k to 8k when logging was enabled, a 64%
loss, because the insert sat inside the request. v2 drops from 179,853 to 168,544, about 6%.

The writer's limit is now measured. The server's per-second diagnostics show it committing about 171,000
rows a second. The first logging-on run stayed just under that and lost nothing: 11,118,384 records
offered, all committed. The other two went slightly over, the queue filled to its 8,192 cap, and 151,920
and 348,473 records were dropped, 1.4% and 3.2% of the total. Every record the queue accepted was
committed. The drops are counted and reported, never silent.

Earlier runs of the same code on 23 and 24 October measured 122k with logging off, with both server and
client at about 94% CPU. The laptop was likely busier. Only the 26 October runs are used here.

---

## Open problems

**Close mode has a 180 to 290 ms tail, and it looks like the kernel.** Half of requests finish in under
2 ms, then latency jumps to about 60 ms at p75 and about 290 ms at p99, the same shape every run. The
server's own counters show nothing: no timeouts, resets, read or write errors, parse errors, or admission
rejections. Kernel counters (`sudo netstat -s -p tcp`) taken around one 10-second close-mode run showed
2,068 new retransmit timeouts, 3,231 packets dropped for low memory, and about 25,800 TCP sockets still
open afterwards, with delayed ACKs unchanged. Halving connections from 100 to 50 left the tail unchanged,
which rules out the 128-entry listen queue. The leading explanation is kernel resource pressure from
about 20,000 new connections a second: packets get dropped and connections wait for TCP's retransmission
timer. Likely, not proven.

**Back-to-back close-mode runs break the test itself.** After roughly 800,000 connections in a few
minutes, new connections failed while the server kept running with no errors. That fits loopback ports
tied up in `TIME_WAIT`. Not confirmed. Close-mode runs need a pause between them.

**Logging drops records above about 171,000 rows a second.** Between 0 and 3.2% per run at 168k RPS.
Logging-on throughput is not lossless logging throughput and should never be quoted as such.

**The server counts about 150 peer resets per run while the client reports zero socket errors.** Probably
connection teardown when `wrk` stops. Unverified.

**The ceiling is unknown.** Server and client were both at about 100% CPU on the same laptop. Separating
them needs a second machine.

**Linux is untested.** Every number here is macOS loopback.

**One core.** The event loop is single-threaded. With one core on the server and one on `wrk`, six cores
sit mostly idle.

---

## What v3 is for

The ceiling is one core and a request path that allocates more than it needs to. v3 goes after both.

Per-request cost first:

- Parse once instead of up to four times, over `string_view`, pulling only `Host`, `Connection`,
  `Content-Length`, and `Transfer-Encoding` instead of building a map of every header.
- Pre-build the full response at startup, header plus body, in both keep-alive and close variants. A
  response then becomes a pointer and an offset with no string construction per request.
- Batch pipelined responses into one `writev()`.
- Replace the five booleans on `Connection` with one state enum.
- `kqueue` on macOS and `epoll` on Linux behind one interface, so the kernel stops rescanning the whole
  descriptor set every pass.
- A timer wheel, so deadline checking stops being a scan of every connection on every pass.

Then the cores:

- One acceptor thread handing descriptors to N event loops round-robin. A connection belongs to one loop
  for its whole life, so after the handoff there is nothing shared and nothing locked. The asset cache is
  read-only and shared safely.
- One lock-free single-producer ring per loop feeding the log writer, replacing the shared mutex-guarded
  queue. This is what stops the drops.
- A ThreadSanitizer build alongside the existing ASan and UBSan ones.

Rough size: 1,500 to 2,000 new lines, plus about 1,000 lines moved out of `server.cpp` into an `EventLoop`
class, plus 700 to 1,000 lines of tests. The existing Python suite is protocol-level and should keep passing
untouched, which is what makes the rewrite safe to attempt.

Expected landing point on this laptop is somewhere around 300,000 RPS with 3 to 4 loops, not 4x the
current number, because `wrk` already needs a full core to drive 180k and the M1's four efficiency cores
are much slower than its four performance cores. The scaling curve from 1 loop to N, and where it flattens and why, is a more interesting
result than the peak.

None of the v3 numbers above are measured. They are the estimate the plan is being judged against.
