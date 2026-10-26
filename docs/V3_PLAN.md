# v3 plan: cheaper per request, then more than one core

Working plan for the next stage. v2 is the event-loop tree described in `README_V2_EVENT_LOOP.md`:
179,853 RPS median keep-alive, one core at about 100%, one `poll()` loop.

v3 has two goals. Cut the work done per request, then run several loops in one process. The first makes
the second worth doing, so the order is not negotiable.

Nothing here is measured. Every number marked "expected" is a hypothesis that the phase itself has to
confirm or kill.

---

## Three things to decide before starting

**1. Merge v2 to GitHub first.**

Done. v2 landed on `main` in six commits on 27 October 2026, with every commit building and the full
suite passing. Tag it `v2` so v3 has a fixed point to measure against.

**2. Who writes the code?**

You should write the multi-loop work yourself, with me reviewing. That phase is what an interviewer will
dig into, and the cost of not being able to answer is high. The mechanical phases (poller wrapper, timer
wheel) matter less either way.

**3. Mac only, or Linux at some point?**

Everything so far is macOS loopback with the load generator on the same laptop. Mac-only is a real
constraint to work inside, and the plan below assumes it. One cheap Linux box with a separate client would
change what the numbers prove, but it is not required to do the engineering.

---

## Ground rules

One phase at a time. After each one: full suite green, ASan and UBSan green, benchmark run, numbers
recorded. If a phase does not move the number, that is a result worth keeping, not a failure to hide.

Phases 1 through 5 do not change behaviour. The existing Python suite is protocol-level, so it should pass
untouched through all of them. A test that needs editing during those phases means something changed that
was not supposed to.

Commit per phase, so any regression can be traced to one change.

---

## Phase 0: baseline and hygiene

Before touching code.

The baseline is locked: three 60-second keep-alive runs per logging mode, in
`data/benchmarks/v2-baseline/`. Every later claim is relative to those.

Add a per-request CPU metric to the harness. RPS is going to stay pinned by the load generator for the
first few phases, so throughput will not show the improvement. Server CPU seconds divided by requests
served will. Without this, phases 1 and 2 will look like they did nothing.

Repo hygiene is done: the batch reports stayed out of the repo, the README's performance section and
roadmap are current, and `sendfile()` is off the roadmap. Bodies are preloaded in memory, so `sendfile()`
would mean going back to reading files.

Delete `thread_pool` and `request_handler`. Nothing calls them. They exist to satisfy two unit tests, and
keeping dead modules that the README has to explain away costs more than the tests are worth.

---

## Phase 1: parse once

**The problem.** `processBufferedInput()` calls `parseRequestFromBuffer()` up to four times on the same
bytes to serve one request: a peek when the pipeline queue is full, a peek when no request is in progress,
the real parse, and a lookahead for the next request. The event loop then calls `processBufferedInput()`
again from `handleReadable()`, from `completeResponse()`, and from the main pass.

Each of those parses is expensive. `parseRequestFromBuffer` takes a `string_view` and then immediately
copies out of it: `request_line`, `method`, `target`, `headers_part`, then per header line a `line`, a
`name`, a `value`, a `lowerAscii(name)`, and a `trimOws(value)`, finishing with an
`unordered_map<string,string>` insert. A browser request with ten headers runs roughly forty allocations,
times four.

**The change.** Parse into a `ParsedRequestView` holding `string_view`s into the connection's input buffer,
with no owning strings and no map. Keep only the four headers that matter: `Host`, `Connection`,
`Content-Length`, `Transfer-Encoding`. Everything else gets validated and skipped, not stored.

Add a cheap `findRequestEnd()` that only looks for `\r\n\r\n` and a byte count, for the three call sites
that currently run a full parse just to answer "is there a complete request here?".

Restructure `processBufferedInput()` so one request means one parse.

**Risk.** This is the highest-risk phase for correctness. The strict-parser behaviour is the part of this
project that is actually worth something: duplicate `Host`, duplicate `Content-Length`, duplicate
`Transfer-Encoding`, obsolete folding, bad method tokens, missing `Host` on HTTP/1.1. All of it has to
survive. `tests/http_parser_tests.cpp` passes unchanged or the phase is not done.

A view into `connection.input` dangles the moment the buffer reallocates or is erased. The view must not
outlive the parse step, and nothing may be held across a `read()`.

**Expected.** 20 to 30% less server CPU per request. Probably no RPS change yet, because the client is the
limit.

---

## Phase 2: pre-built responses and one write

**The problem.** `cachedHeader()` builds a fresh string per response: `header_prefix` plus the connection
header plus `\r\n\r\n`. `PendingResponse` then stores owning copies of `method`, `path`, and that header.
`logResponse()` copies `method`, `path`, and `peer_ip` again into a `LogRecord`. And `handleWritable()`
sends the header and the body as two separate `send()` calls, so a single response costs at least two
syscalls and a pipelined burst of eight costs sixteen.

**The change.** At startup, build two complete responses per cached asset, header and body already joined,
one with `Connection: keep-alive` and one with `Connection: close`. Store both as
`shared_ptr<const string>`. A `PendingResponse` becomes a pointer plus an offset, with no strings
constructed per request.

For `method` and `path` in the log record: these are the only reason the parsed strings need to outlive
the parse. Since the route is already resolved to a cached asset, store a small asset id and a method enum
instead of two strings, and have the logger resolve them when it writes.

Collect all pending responses into an iovec array and send with one `writev()`.

**Expected.** Another 15 to 25% off CPU per request, and roughly half the syscalls. This is the phase where
the two together might start showing up as RPS.

---

## Phase 3: one connection state

**The problem.** `Connection` carries `request_in_progress`, `input_closed`, `stop_reading`,
`close_after_responses`, and `finished`, and the loop reasons about combinations of all five.
`processBufferedInput()` sets four of them in six different places. It works, and it is already hard to
follow. With several loops it becomes impossible to review.

**The change.** One `enum class ConnState { Reading, Parsing, Writing, Draining, Closing }` plus the two
genuine edge facts (`input_closed`, `close_after_responses`). Transitions in one function with the legal
moves written down.

No behaviour change. Pure cleanup, done before the concurrency work rather than after, because every bug
in phase 6 will be debugged through this state.

---

## Phase 4: kqueue and epoll

**The problem.** The loop rebuilds the whole `pollfd` vector every pass and hands the kernel the full set,
which the kernel then scans. At 100 connections this costs nothing. The design does not survive 10,000.

**The change.** A `Poller` interface with `add`, `modify`, `remove`, and `wait`, implemented over `kqueue`
on macOS and `epoll` on Linux. Registration changes only when a connection's interest changes, which the
code already tracks in `pollEvents()`.

Level-triggered to start. Edge-triggered is a second, separate change if it is worth it, and it is easy to
get subtly wrong.

**Expected.** Little or nothing at 100 connections. The win shows up in a new benchmark worth adding: 1,000
and 10,000 idle-plus-active connections, where `poll()` falls over and `kqueue` does not. That graph is
more interesting than the peak RPS.

---

## Phase 5: timer wheel

**The problem.** Every loop pass walks all connections four times: once in `pollTimeoutMilliseconds()` to
find the nearest deadline, twice in the pre-poll maintenance block, and once in the post-poll deadline
sweep. All of it runs whether or not anything is due.

**The change.** A bucketed timer wheel at say 10 ms resolution. Each connection holds a handle to its
current deadline. Expiry walks only the due bucket. Deadline semantics stay exactly as they are: absolute,
`steady_clock`, header deadline not reset by trickled bytes, per-response write deadlines.

`tests/http_deadline_tests.py` passes unchanged.

**Expected.** Flat CPU as connection count grows, instead of linear. Another prerequisite for the
high-connection benchmark.

---

## Phase 6: several event loops

The real one.

**The change.** One acceptor thread owns both listeners and does nothing but `accept()` and hand the
descriptor to a loop, round-robin, over a per-loop pipe. N event loops, each a full instance of the current
loop logic in an `EventLoop` class. A connection belongs to one loop for life, so after the handoff there
is no sharing and no locking on the request path.

Not `SO_REUSEPORT`. On macOS it does not spread connections across listening sockets the way Linux does, so
it would work on one platform and quietly not on the other.

**What has to change to be thread-safe:**

| Component | Today | Needs |
|---|---|---|
| `AssetCache` | immutable, const lookup, no mutex | nothing, already safe to share |
| `ConnectionLimiter` | counter in one thread | atomic global cap, or a per-loop budget |
| `ActiveConnections` | registry plus cancellation flags | per-loop registry, with shutdown fanning out |
| `Server::diagnostics_` | plain counters | per-loop counters, summed when reported |
| Wake pipe | one, for shutdown | one per loop |
| Shutdown | one `beginShutdownDrain()` | all loops drain, acceptor stops first, one grace period |

Shutdown is the subtle part, not accept. The current sequence stops accepting, drains, cancels at grace
expiry, then joins the log writer. With N loops the acceptor stops first, each loop drains its own
connections against the same deadline, and the writer joins only after every loop is done. Getting this
wrong shows up as a hang on `SIGINT`, which the shutdown tests will catch.

No thread pinning on macOS. QoS class hints are the closest available, and the four efficiency cores are
roughly a third the speed of the four performance cores, so loop count needs testing rather than assuming
one per core. Start at 3 and 4.

**Risk.** Highest in the project. Build with ThreadSanitizer from the first commit of this phase, not after
it works. Run `tests/http_robustness_tests.py` under TSan.

**Expected.** The jump. Somewhere around 300,000 RPS at 3 to 4 loops. Not 4x, because `wrk` needs cores on
the same laptop and will saturate first. The scaling curve from 1 loop to N, and the point where it
flattens and the reason why, is the result worth writing up.

---

## Phase 7: logging that keeps up

**The problem.** The v2 writer tops out at about 171,000 rows a second. At 168k RPS, two of three runs
dropped 1.4% and 3.2% of records with the queue pinned at its 8,192 ceiling. With N loops feeding one mutex-guarded `std::deque`, that mutex also becomes the one piece of
shared state on the hot path, which defeats the point of phase 6.

**The change.** One lock-free single-producer single-consumer ring per loop. Each loop writes to its own
ring with no lock. The writer thread drains all N rings in turn and batches into SQLite as it does now.

Size the rings against measured peak depth rather than guessing, and keep the existing drop accounting:
dropped records stay counted and reported, never silent.

If drops persist at 300k, the honest fallback is per-second aggregate rows instead of one row per request.
8.64 billion rows a day was never a real logging design, and the README already says so.

**Expected.** Logging-on within 10% of logging-off, zero drops.

---

## Phase 8: measure it properly

Repeat the full sustained matrix: 6 runs, 60 seconds, alternating logging modes, all runs reported.
Add the connection-scaling benchmark (100 / 1,000 / 10,000) that phases 4 and 5 were really for. Add the
loop-count scaling curve from 1 to N.

Run nginx on the same laptop with the same file and the same core count. "Within X% of nginx" is the only
number in this project that means something to someone who has not read the code.

---

## Risks worth naming now

**The client is already the ceiling.** One `wrk` thread at 100% CPU produces 180k. After phase 6 the server
may be faster than this laptop can measure. That is a real possible outcome, and the answer is to say so
and report server CPU per request alongside RPS, not to inflate the number with pipelining tricks.

**Phase 1 can quietly break the parser.** The smuggling defences are the most valuable part of the
codebase. Unit tests pass unchanged, or revert.

**Phase 6 can produce races that tests do not catch.** TSan from the first commit.

**The 230 ms close-mode p99 is still unexplained.** It may well be a loopback or backlog artifact that
disappears on Linux, or it may be real. Worth one bounded investigation with `netstat -s` before phase 6,
because if it is a server bug, multiplying the loops multiplies it.

**Scope creep.** Not in v3: HTTP/2, TLS, dynamic content, `sendfile()`, `io_uring`, routing beyond the
allowlist. Each is a defensible thing to build and none of them makes this server faster or more
believable.

---

## Done when

Parse once per request, no per-request heap allocation on the hot path, `kqueue` and `epoll` behind one
interface, deadlines off the per-pass scan, N loops sharing nothing, logging with zero drops, TSan clean,
and a measured scaling curve with the conditions written next to every number.

Then one line on the CV that survives being asked about.
