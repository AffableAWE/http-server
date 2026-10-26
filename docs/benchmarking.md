> **Historical:** this documents the v1 baseline procedure (ApacheBench against the connection-per-request MVP). v2 keep-alive results are in `data/benchmarks/v2-baseline/`.

# Benchmarking

This document defines the repeatable baseline procedure for this repository.
It is deliberately narrow: it measures the current connection-per-request
server over loopback and records enough context to tell a real improvement
from a changed test setup.

The baseline in this document was collected on **2026-10-23** from commit
`d642591ee7d868bfa616948a13fc753f443fc15e` plus the uncommitted benchmark
harness. It is a starting point, not evidence that the 100k RPS target has been
reached.

## What is measured

| Item | Baseline value |
|---|---|
| Server | unmodified C++17 binary, Release |
| Client | ApacheBench (`ab`), same host, loopback `127.0.0.1` |
| Request | `GET /` |
| Response body | `static/index.html`, 1,277 bytes |
| Connections | 100 concurrent, one request per connection |
| Warm-up | 20,000 requests |
| Measured runs | 3 runs of 100,000 requests |
| Logging | measured separately, off and on |
| Network path | loopback only; these are not LAN/WAN results |

`ab` sends an HTTP/1.0 request and the server replies with HTTP/1.1 and
`Connection: close`. This harness therefore measures the current close-per-
request behavior, not HTTP/1.1 keep-alive throughput.

## Environment

| Item | Value |
|---|---|
| CPU | Apple M1, 8 logical cores (4 performance + 4 efficiency) |
| Memory | 8 GiB |
| OS | macOS 27.0.1, Darwin 27.0.0, arm64 |
| Compiler | Apple clang 21.0.0.21000334 |
| Build | CMake Release, `-O3 -DNDEBUG -std=c++17 -arch arm64 -Wall -Wextra -Wpedantic` |
| SQLite | 3.53.1 (Homebrew) |
| Client load tool | ApacheBench 2.3 |

Do not compare results from a different CPU, OS, build type, payload, client
location, or concurrency level without recording those differences.

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
mkdir -p data
python3 scripts/benchmark.py --logging both --label baseline
```

The defaults are a 20,000-request warm-up followed by three 100,000-request
runs at concurrency 100. Each logging mode gets a fresh server process and a
fresh SQLite database. Results are written under
`data/benchmarks/<UTC timestamp>-<pid>/result.json`; each run's raw `ab` output
is kept beside it.

To run one mode only:

```bash
python3 scripts/benchmark.py --logging off
python3 scripts/benchmark.py --logging on
```

The script exits non-zero if the server dies, `ab` does not complete the
requested count, the response body or headers do not match, or a logging run
writes fewer rows than were measured.

## Baseline results

Measured on the environment above. "Successful RPS" is
`Requests/sec × (complete - failed) / complete`; all runs had zero failed
requests and zero non-2xx responses.

| Logging | Run 1 | Run 2 | Run 3 | Median | p50 | p99 | Server CPU | Client CPU |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| off | 20,886.6 | 22,041.0 | 21,176.5 | **21,176.5** | 4 ms | 8 ms | 193.7% | 68-69% |
| on | 13,520.7 | 12,361.5 | 13,331.4 | **13,331.4** | 7 ms | 20 ms | 144.2% | 53-55% |

Notes:

* CPU percentages are percentages of one logical core. Server CPU is aggregate
  process CPU from startup through `SIGTERM`; client CPU is per `ab` process.
* The logging run inserted 320,006 rows: 300,000 measured requests + 20,000
  warm-up requests + six readiness/correctness probes.
* Logging off still initializes SQLite and applies the schema. That startup
  work is part of the server's current behavior and is included in startup,
  though not in the steady-state per-request path.
* The unmodified server reproduced the reported ~20k RPS range with logging
  off. It did **not** reproduce the README's ~8k logging figure on this host;
  the measured median was ~13.3k. Neither figure should be treated as portable
  across machines.

## Correctness checks

Before warm-up and after every measured run, the harness makes a direct
`HTTPConnection` request and checks:

* status is `200`;
* `Content-Type` is present;
* `Content-Length` equals the received body length;
* `Connection: close` is present;
* the body is byte-for-byte equal to `static/index.html` and matches its
  SHA-256 (`a7e49d...bc4001`, 1,277 bytes).

`ab` separately reports complete requests, failed requests, and non-2xx
responses. The raw output is retained so latency percentiles and errors can be
re-checked without rerunning.

## Why ApacheBench rather than wrk

`wrk` is the more realistic HTTP/1.1 client and will be used for the target
measurement after the reliability and keep-alive work. It is not the baseline
tool because the current server fails under it in two ways:

1. **Process termination on peer disconnect.** A 5-second
   `wrk -t4 -c100 -d5s --latency` run reached 22,831 RPS, then the server
   process was killed by `SIGPIPE` while writing to a client that had closed.
   The response is logged as `send failed: Broken pipe`.
2. **Loopback client-port exhaustion with logging enabled.** Because logging
   delays the server's `close()`, `wrk` can become the active closer and leave
   thousands of client ports in `TIME_WAIT`. A 5-second logging-enabled run
   completed 16,907 requests, reported 100 connect errors, and then could not
   connect again until the ports aged out. That makes the logging number a
   client-port artifact, not a server throughput result.

`ab` waits for the server to finish and did not exhibit either failure in the
baseline runs. These limitations must be revisited when keep-alive is added.

## Confirmed defects observed during baseline (not fixed here)

These are recorded so later batches can address them deliberately:

* A client disconnect during a response write can terminate the whole process
  via `SIGPIPE`; `sendAll()` handles `EPIPE` but the signal is never blocked.
* Unknown routes return the 404 page with `200 OK`; the router selects a file
  but the handler has no status to propagate.
* The parser accepts a missing HTTP version, an unsupported version such as
  `HTTP/9.9`, and absolute-form request targets as valid. It also accepts a
  request with no `Host` header.
* Eight idle TCP clients (the worker-pool size) block all workers in `recv()`;
  a subsequent request times out until one idle client closes. There are no
  read/write timeouts and no bound on the accepted-fd queue.
* Logging is initialized even when `ENABLE_LOGGING=false`, so a read-only or
  invalid database path still prevents the server from starting.
* One exploratory `wrk` logging run produced repeated
  `logAccess insert failed: database is locked` messages. It was not reproduced
  in the final `ab` runs, so it remains an intermittent risk to investigate,
  not a measured baseline result.
