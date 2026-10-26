# http-server, v1: from one file to eight modules

This is the story of the first version of `AffableAWE/http-server`. It covers the original single-file
server kept in `legacy/`, the refactor that broke it into modules, and what the result could and could not
do. Everything here describes the code as it stood before the event-loop rewrite.

About 85 commits. The server went from 421 lines in one file to 995 lines across 8 modules.

---

## Where it started: legacy/server.cpp

One file, 421 lines, written to learn how a web server works rather than to ship one. It did work. You could
run it, point a browser at port 8080, and get a page back.

The shape of it:

- `main()` created an IPv4 and an IPv6 socket, bound both, and listened.
- A thread pool of `hardware_concurrency()` workers pulled client descriptors off a global `queue<int>`.
- The main thread sat in `poll(fds, 2, -1)` and pushed every accepted descriptor onto that queue.
- Each worker read one request, matched the path against a hardcoded list, opened the file with `ifstream`,
  and streamed it back.
- Globals for everything: `STATIC_DIR`, `PORT`, `client_queue`, `queue_mutex`, `queue_cond`, `server_running`.
- `using namespace std;` at the top.

It also had real bugs, and finding them is most of what the refactor was about.

**`send()` return values were ignored.** All three call sites fired and forgot. TCP is allowed to accept
fewer bytes than you hand it, so under load a response could be silently truncated.

**The file-streaming loop was wrong.** `while (!file.eof()) { file.read(buf, 1024); send(fd, buf, file.gcount(), 0); }`
checks for end-of-file before the read that reaches it. On a file whose size is an exact multiple of 1024
this does one extra pass.

**Shutdown was unreachable.** `poll(fds, 2, -1)` blocks with no timeout, so the loop below it never exits
normally. The join-the-workers code after the loop was dead.

**A spurious wakeup killed a worker permanently.** `if (client_queue.empty() || !server_running) break;`
runs after the condition variable wait. Wake up with nothing in the queue and that thread exits for good.

**No limit on request size.** The read loop appended to `accumulated_data` until it saw `\r\n\r\n`. A client
that never sent one could grow that string without bound.

**No `IPV6_V6ONLY`.** On Linux the IPv6 socket binds dual-stack by default, so the IPv4 bind on the same
port would collide.

**`cout` on every connection.** Printing "IPv4 connection accepted." per request puts a locked stream write
on the hot path.

The file stays in `legacy/` on purpose. It is where the project started and it is useful to diff against.

---

## The refactor, commit by commit

The order matters, because each step was small enough to keep the server running.

| Commit | What moved |
|---|---|
| `13939ee` | `trim()` and `getContentType()` out to `utils` |
| `85ddeb1` | config loading out to `config`, globals replaced by a `Config` struct |
| `50e11f0` | request-line and header parsing out to `http_parser` |
| `b1d974e` | path-to-file mapping out to `router` |
| `820ca82` | a `Logger` interface, stdout stub first |
| `d322af0` | the thread pool wrapped in a class with RAII shutdown |
| `f2d8807` | request handling extracted, plus the `send`/`eof` fixes and a size limit |
| `2c9da98` | socket setup and the accept loop wrapped in a `Server` class |
| `435185e` | `main()` cut back to config, `Server`, and signal handlers |
| `ed95577` | SQLite logging behind the `Logger` interface, CMake build |

The logger interface landing before SQLite is the part worth keeping. A stdout stub went in first, so when
the real SQLite writer arrived nothing above it had to change.

---

## What v1 does

A static file server for a fixed set of paths, with an access log in SQLite.

| Module | Job |
|---|---|
| `config` | Reads `PORT`, `STATIC_DIR`, `DB_PATH`, `ENABLE_LOGGING` from the environment, validates them, falls back to defaults with a warning |
| `http_parser` | Splits the request line and headers into a `ParsedRequest`, sets `valid = false` on anything malformed |
| `router` | Allowlist of five paths, anything else gets the 404 page |
| `request_handler` | Reads one request, checks the method, streams the file, records the access |
| `thread_pool` | Fixed worker pool over a mutex-guarded `queue<int>`, drains and closes leftover descriptors on shutdown |
| `logger` | SQLite in WAL mode, one prepared `INSERT`, one mutex across all workers |
| `server` | Owns both listening sockets, the `poll()` accept loop, and the lifecycle |
| `utils` | `trim()` and `getContentType()` |

Fixes that came with the split:

- `sendAll()` loops on partial writes and retries `EINTR`, so a response either goes out whole or the
  connection is reported broken.
- The file loop runs on `gcount()` instead of `eof()`.
- The accept loop polls with a 500 ms timeout, so `SIGINT` and `SIGTERM` are noticed within half a second
  and the workers actually get joined.
- Requests are capped at 64 KB of headers. Past that the client gets `413 Payload Too Large`.
- The IPv6 socket sets `IPV6_V6ONLY`, so both binds succeed on Linux.
- Per-connection `cout` logging is gone.

### Running it

```bash
# macOS
brew install cmake sqlite pkg-config
# Ubuntu / Debian
sudo apt install build-essential cmake libsqlite3-dev pkg-config

cmake -S . -B build
cmake --build build -j
mkdir -p data
./build/bin/http_server
```

```bash
curl -i http://localhost:8080/
```

| Variable | Default | Purpose |
|---|---:|---|
| `PORT` | `8080` | Listening port |
| `STATIC_DIR` | `static` | Static file directory |
| `DB_PATH` | `data/logs.db` | SQLite access log |
| `ENABLE_LOGGING` | `true` | Turn access logging off without rebuilding |

Routes: `/` is the home page, `/about` the about page, `/aboutStyle.css` and `/404Style.css` the stylesheets,
and anything else falls through to the 404 page. `GET` only.

The server must be started from the repo root. `Logger` loads its schema from the relative path
`scripts/init_db.sql`, and it throws if that file is not there.

---

## Performance, and how much to trust it

The repo README claims roughly 22,000 to 23,000 requests/sec with logging off and about 8,000 with SQLite
logging on, measured with `wrk` on an Apple M1.

The throughput figure holds up. Careful `ab` runs on the same laptop later put close-connection throughput
at about 21,400 requests/sec, and that number barely moves no matter how the server is written, because
opening and closing a TCP connection costs more than serving a 1.2 KB file from disk. v1 always sends
`Connection: close`, so every request pays for a new connection.

What the original measurement does not record: how many runs, whether there was a warm-up, which build type,
or which exact binary. Treat 22k as the right order of magnitude rather than a result you could reproduce
on demand.

The drop to 8,000 with logging on is the honest headline of this design. Every worker runs an SQLite
`INSERT` inside the request, behind one global mutex, before the connection closes. Logging does not run
beside the request. It runs in the middle of it.

---

## What v1 cannot do

Worth writing down plainly, because every one of these is what the next version went after.

**Slow clients stall the whole server.** `recv()` blocks with no timeout. The pool has as many workers as
the machine has cores. Eight clients that open a connection and send one header byte per minute occupy all
eight workers, and every other client waits. That is slowloris, and v1 has no defence against it.

**The descriptor queue has no limit.** Accepted sockets pile up in the queue with nothing pushing back on
the accept loop. Under a flood, memory grows and queued connections time out on the client side having
never been looked at.

**Every request touches the disk.** Open, seek, read, close, for every hit, including the 404 page.

**Logging is on the critical path.** One mutex, one synchronous insert per request.

**One request per connection.** No keep-alive, so browsers reopen for every asset.

**The parser accepts almost anything.** Headers are case-sensitive, `Host` is not required, duplicate
headers overwrite silently, and `Content-Length` and `Transfer-Encoding` are parsed but not acted on. Nothing
here is exploitable as a static file server, but it is not a parser you could put in front of a proxy.

**A bad database path terminates the process.** `Logger`'s constructor throws, `main()` has no try/catch,
so the exception escapes and calls `std::terminate`.

**No tests.** Not one.

---

## What v1 was for

It is a working MVP: a modular, dual-stack, thread-pooled static file server with persistent access logs
and a clean CMake build, and it does about 21k requests/sec on a laptop. The refactor from `legacy/` is
the real content. Eight modules, each with one job, each testable on its own, arrived through ten commits
that never broke the running server.

The rewrite that follows throws out the thread pool entirely. `README_V2_EVENT_LOOP.md` covers that.
