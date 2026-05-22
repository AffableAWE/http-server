# http-server

A multi-threaded HTTP/1.1 server written from scratch in C++17, with SQLite-backed access logging, dual-stack IPv4/IPv6 support, and a `poll()`-driven accept loop feeding a producer–consumer thread pool.

This started as a "learn by building" project — the original monolithic implementation lives in [`legacy/`](./legacy) for reference. The current codebase is a full refactor focused on correctness, modularity, and observability.
