# http-server

> A multi-threaded HTTP/1.1 server written from scratch in modern C++17 — with SQLite-backed access logging, dual-stack IPv4/IPv6 support, and a `poll()`-driven accept loop feeding a producer–consumer thread pool.

This project began as a "learn by building" exercise. The original monolithic implementation has been preserved in [`legacy/`](./legacy) for posterity; the current codebase is a full refactor focused on **correctness, modularity, and observability**.

---

## 📋 Table of Contents

- [Highlights](#-highlights)
- [High-Level Architecture](#-high-level-architecture)
- [Technical Architecture](#-technical-architecture)
- [Project Structure](#-project-structure)
- [Module Responsibilities](#-module-responsibilities)
- [Building from Source](#%EF%B8%8F-building-from-source)
- [Configuration](#%EF%B8%8F-configuration)
- [Routes](#-routes)
- [Access Logging](#-access-logging)
- [Benchmarks](#-benchmarks)
- [What the Refactor Fixed](#-what-the-refactor-fixed)
- [Roadmap](#%EF%B8%8F-roadmap)
- [License](#-license)

---

## ✨ Highlights

- **22,000+ requests/sec** under `wrk` benchmark (logging disabled); **~8,000 req/sec** with full SQLite access logging.
- **Zero external runtime dependencies** beyond `libsqlite3` and the C++ standard library.
- **Dual-stack networking** — separate IPv4 and IPv6 listening sockets, multiplexed via `poll()`.
- **Fixed-size thread pool** with a mutex-guarded queue and condition variable, sized to `std::thread::hardware_concurrency()`.
- **SQLite WAL-mode logging** with prepared statements; one mutex serializes writes across all workers.
- **Graceful shutdown** via `SIGINT`/`SIGTERM` signal handlers and an atomic running flag.
- **Toggleable logging** — flip access logging on or off via an environment variable, no recompilation required.

---

## 🏛 High-Level Architecture

A bird's-eye view of how a request flows from a client through the server's major components.

![High-Level Architecture](docs/images/high-level-architecture.png)

The server exposes itself simultaneously on IPv4 and IPv6. The main thread is responsible only for accepting new connections; all per-request work happens on worker threads, keeping the accept path fast and predictable.

---

## ⚙️ Technical Architecture

A detailed look at the internal data flow — sockets, the accept loop, the thread pool's queue, the request pipeline, and the logging subsystem.

![Technical Architecture](docs/images/technical-architecture.png)

**Key design choices:**

- The main thread runs a `poll()` loop with a 500 ms timeout — long enough to be efficient under load, short enough to notice a shutdown signal promptly.
- The worker pool size defaults to `hardware_concurrency()` but falls back to a minimum of two threads if the runtime reports zero.
- The fd queue is the only point of contention on the hot path. Workers spend the vast majority of their time in `recv()`, `read()`, and `send()`, not waiting on the queue lock.
- Logging is serialized through a single mutex inside `Logger`. SQLite in WAL mode handles this efficiently — readers can run concurrently with the writer.

---

## 📁 Project Structure

The repository is organized to separate **public headers** (`include/`), **implementations** (`src/`), **runtime data** (`data/`), and the **original implementation** (`legacy/`).

![Project Structure](docs/images/project-structure.png)

This layout maps cleanly to CMake's expectations: a single `file(GLOB ...)` over `src/*.cpp` picks up every translation unit, and `target_include_directories(... include)` makes all module headers resolvable as `#include "http_server/<module>.hpp"`.

---

## 🧩 Module Responsibilities

Each module has a header in `include/http_server/` and an implementation in `src/`.

| Module | Responsibility |
|---|---|
| **`config`** | Loads `PORT`, `STATIC_DIR`, `DB_PATH`, `ENABLE_LOGGING` from environment variables with safe defaults and validation. |
| **`http_parser`** | Parses the HTTP request line and headers into a structured `ParsedRequest`. Returns `valid = false` on malformed input. |
| **`router`** | Maps HTTP paths to file paths under the static directory. Whitelist-based; unknown routes fall back to the 404 page. |
| **`request_handler`** | Reads one request, validates the method, serves the file via partial-write-safe `sendAll()`, and records the access. |
| **`thread_pool`** | Fixed-size worker pool with a thread-safe fd queue. RAII shutdown drains and closes any leftover descriptors. |
| **`logger`** | SQLite-backed access log. Initializes schema on first run, uses a prepared `INSERT` statement, serializes writes with a mutex. |
| **`server`** | Owns the listening sockets, dual-stack setup, the `poll()` accept loop, and the full lifecycle. |
| **`utils`** | Small standalone helpers: `trim()`, `getContentType()`. |

---

## 🛠️ Building from Source

### Prerequisites

- A C++17 compiler — Apple Clang, GCC ≥ 9, or Clang ≥ 9
- CMake ≥ 3.16
- `libsqlite3` (runtime library **and** development headers)
- `pkg-config`

### macOS (Homebrew)

```bash
brew install cmake sqlite pkg-config
export PKG_CONFIG_PATH="$(brew --prefix sqlite)/lib/pkgconfig:$PKG_CONFIG_PATH"
```

Homebrew installs SQLite as keg-only, so the `PKG_CONFIG_PATH` export is required for CMake to find it. Add the line to your `~/.zshrc` to make it permanent.

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install build-essential cmake libsqlite3-dev pkg-config
```

### Compile

```bash
cmake -S . -B build
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

The binary lands at `build/bin/http_server`.

### Run

```bash
mkdir -p data
./build/bin/http_server
```

You should see:

```
[INFO] Logger initialized (db=data/logs.db)
[INFO] Starting http-server on port 8080, static dir 'static'.
[INFO] IPv4 and IPv6 sockets created.
[INFO] Bound to port 8080.
[INFO] Listening for incoming connections.
[INFO] Thread pool started.
```

Then open `http://localhost:8080/` in a browser, or `curl` it:

```bash
curl -i http://localhost:8080/
```

---

## ⚙️ Configuration

All configuration is read from environment variables at startup. Defaults apply when a variable is unset or invalid.

| Variable | Default | Description |
|---|---|---|
| **`PORT`** | `8080` | TCP port to listen on. Must be in the range `1`–`65535`; invalid values trigger a warning and fall back to the default. |
| **`STATIC_DIR`** | `static` | Directory served as the static file root. |
| **`DB_PATH`** | `data/logs.db` | Path to the SQLite access-log database. |
| **`ENABLE_LOGGING`** | `true` | Set to `false`, `0`, or `no` to disable access logging entirely. |

### Example: run on port 9000 without logging

```bash
PORT=9000 ENABLE_LOGGING=false ./build/bin/http_server
```

---

## 🛣 Routes

The server uses an explicit whitelist for routing. There is no filesystem traversal of the static directory.

| Path | Served file | Status |
|---|---|---|
| `/` | `static/index.html` | `200` |
| `/about` | `static/about.html` | `200` |
| `/404` | `static/404.html` | `200` |
| `/aboutStyle.css` | `static/aboutStyle.css` | `200` |
| `/404Style.css` | `static/404Style.css` | `200` |
| *any other path* | `static/404.html` (fallback) | `200` |

**Method handling:**

- Only `GET` is accepted. All other methods return `405 Method Not Allowed`.
- Malformed requests return `400 Bad Request`.
- Requests with headers exceeding 64 KB return `413 Payload Too Large`.

---

## 📊 Access Logging

When `ENABLE_LOGGING` is `true` (the default), every served connection appends one row to a SQLite database.

### Schema

```sql
CREATE TABLE access_logs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp   TEXT    NOT NULL DEFAULT (datetime('now')),
    client_ip   TEXT    NOT NULL,
    method      TEXT    NOT NULL,
    path        TEXT    NOT NULL,
    status_code INTEGER NOT NULL,
    bytes_sent  INTEGER NOT NULL
);

CREATE INDEX idx_access_logs_timestamp ON access_logs(timestamp);
CREATE INDEX idx_access_logs_status    ON access_logs(status_code);
```

The schema lives in [`scripts/init_db.sql`](scripts/init_db.sql) and is applied idempotently on every server start (`CREATE TABLE IF NOT EXISTS`).

### Database settings

`Logger` opens the database with two performance-oriented pragmas:

```sql
PRAGMA journal_mode=WAL;       -- concurrent readers + one writer
PRAGMA synchronous=NORMAL;     -- durable enough for access logs
```

### Useful queries

```bash
# Total requests served
sqlite3 data/logs.db "SELECT COUNT(*) FROM access_logs;"

# Breakdown by status code
sqlite3 data/logs.db \
    "SELECT status_code, COUNT(*) FROM access_logs GROUP BY status_code;"

# Top requested paths
sqlite3 data/logs.db \
    "SELECT path, COUNT(*) AS hits FROM access_logs
     GROUP BY path ORDER BY hits DESC LIMIT 10;"

# Most recent 20 requests
sqlite3 data/logs.db \
    "SELECT timestamp, client_ip, method, path, status_code
     FROM access_logs ORDER BY id DESC LIMIT 20;"
```

### Thread safety

All writes go through a single `std::mutex` inside `Logger`. A prepared `INSERT` statement is created once and reused across all calls — only the bound parameters change per request. This is the only thread-safety boundary required for the access log.

---

## 📈 Benchmarks

Measured locally on Apple Silicon with `wrk -t4 -c100 -d10s http://localhost:8080/`.

| Build | Configuration | Requests/sec | Notes |
|---|---|---|---|
| **v1** (legacy monolith) | no logging, original bugs intact | ~20,000 | the starting point |
| **v2** (refactored) | `ENABLE_LOGGING=false` | **~23,000** | refactor did not regress throughput |
| **v2** (refactored) | logging enabled (SQLite INSERT per request) | ~8,000 | observability trade-off |

The gap between the two v2 numbers is the cost of one mutex-guarded `sqlite3_step()` per request — a deliberate trade-off in favor of structured, queryable access logs. The toggle exists so you can pick the right mode for your use case at launch time.

### Reproduce the benchmark

```bash
# Logging ON (default)
./build/bin/http_server &
wrk -t4 -c100 -d10s http://localhost:8080/

# Logging OFF
ENABLE_LOGGING=false ./build/bin/http_server &
wrk -t4 -c100 -d10s http://localhost:8080/
```

---

## 🔧 What the Refactor Fixed

Items identified during code review of the original monolithic implementation, all addressed in v2:

### Correctness

- **`file.eof()` read loop** replaced with the canonical `while (file.read(buf, n) || file.gcount() > 0)` pattern. The original could send one extra zero-length chunk at end-of-file.
- **Unchecked `send()` return values** replaced with a `sendAll()` helper that loops on partial writes, retries on `EINTR`, and detects connection drops. The original could silently truncate large responses.
- **Dead `parseHTTPHeaders()` function** now wired into the request pipeline as `parseRequest()`, returning a structured `ParsedRequest`.

### Robustness

- **Unreachable shutdown block** made reachable via an atomic running flag, a 500 ms `poll()` timeout in the accept loop, and a `SIGINT`/`SIGTERM` handler in `main()`.
- **`IPV6_V6ONLY`** now set explicitly on the IPv6 socket so the IPv4 bind does not collide on Linux dual-stack systems.
- **64 KB request size cap** added to prevent unbounded buffering from slow or malicious clients.
- **`EINTR` retry** added to `recv()`, `send()`, `accept()`, and `poll()` so signal interrupts no longer kill connections.

### Observability

- **Hardcoded `"port 8080"`** log lines now reflect the actual configured port.
- **SQLite-backed access log** records every request (or failed connection attempt) with timestamp, client IP, method, path, status code, and bytes sent.

### Maintainability

- **One 400-line file** split into 9 focused modules with clear responsibilities and public/private boundaries.
- **Globals eliminated** — the thread pool's queue, mutex, and condition variable are now members of a `ThreadPool` class; the server's sockets and lifecycle state are members of a `Server` class.
- **`using namespace std;`** removed from all headers, preventing namespace pollution downstream.

---

## 🗺️ Roadmap

- **Dockerization** with multi-stage builds and volume mounts for `static/` and `data/`.
- **HTTP keep-alive** support to amortize TCP handshake cost across multiple requests.
- **`sendfile()`** zero-copy path for large static files.
- **Per-connection `recv()` timeout** to prevent slowloris-style holds on worker threads.
- **Batched log inserts** — single transaction per N requests — to reduce the logging-on throughput penalty.
- **Path traversal protection** when routing is generalized beyond the current whitelist.

---

## 📄 License

See [`LICENSE`](./LICENSE).
