#!/usr/bin/env python3
"""Separate bounded pipelining probe (not sequential keep-alive RPS).

This Python client intentionally uses raw sockets and a fixed pipeline depth.
It is useful for correctness/backpressure validation, but must be treated as a
client-bound pipelining probe rather than a server maximum.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

from benchmark import ROOT, build_metadata, environment_metadata, wait_for_child

sys.path.insert(0, str(ROOT / "tests"))
from http_test_util import HttpResponseReader  # noqa: E402

DEFAULT_OUTPUT = ROOT / "data" / "benchmarks"


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_server(binary: Path, static_dir: Path, log_path: Path, port: int):
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(ROOT / "data" / "unused-pipeline.db"),
            "ENABLE_LOGGING": "false",
            "MAX_CONNECTIONS": "1024",
            "MAX_PIPELINED_REQUESTS": "8",
            "MAX_REQUESTS_PER_CONNECTION": "1000000",
            "KEEPALIVE_TIMEOUT_MS": "120000",
            "HEADER_TIMEOUT_MS": "30000",
            "WRITE_TIMEOUT_MS": "30000",
            "SHUTDOWN_GRACE_MS": "3000",
        }
    )
    log = log_path.open("wb")
    proc = subprocess.Popen(
        [str(binary)],
        cwd=ROOT,
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    log.close()
    return proc


def wait_ready(port: int, timeout: float) -> None:
    import http.client

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=0.5)
            connection.request(
                "GET", "/about",
                headers={"Host": "127.0.0.1", "Connection": "close"},
            )
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def max_rss_kb(usage) -> int | None:
    if usage is None:
        return None
    if sys.platform == "darwin":
        return int(usage.ru_maxrss / 1024)
    return int(usage.ru_maxrss)


def worker(port: int, depth: int, path: str, expected_body: bytes,
           stop: threading.Event, counts: list[int],
           errors: list[str]) -> None:
    request = (
        f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n"
    ).encode("ascii")
    batch = request * depth
    while not stop.is_set():
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                reader = HttpResponseReader(sock)
                while not stop.is_set():
                    sock.sendall(batch)
                    for _ in range(depth):
                        status, _headers, body = reader.read_response()
                        if not status.startswith("HTTP/1.1 200") or body != expected_body:
                            raise AssertionError(
                                f"pipeline response {status!r} {body!r}"
                            )
                    counts[0] += depth
        except (OSError, EOFError) as error:
            errors.append(str(error))
            return
        except AssertionError as error:
            errors.append(str(error))
            return


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    parser.add_argument("--static-dir", default="static")
    parser.add_argument("--path", default="/")
    parser.add_argument("--expected-body", default=str(ROOT / "static" / "index.html"))
    parser.add_argument("--connections", type=int, default=20)
    parser.add_argument("--depth", type=int, default=8)
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--label", default="")
    args = parser.parse_args()
    if args.depth < 1 or args.connections < 1 or args.duration < 1:
        parser.error("connections, depth, and duration must be positive")

    static_dir = Path(args.static_dir)
    if not static_dir.is_absolute():
        static_dir = ROOT / static_dir
    expected_body = Path(args.expected_body).read_bytes()
    output_dir = Path(args.output) / (
        f"{datetime.now().strftime('%Y%m%dT%H%M%SZ')}-{os.getpid()}"
    )
    output_dir.mkdir(parents=True, exist_ok=False)
    port = args.port or free_port()
    log_path = output_dir / "server.log"
    proc = start_server(Path(args.binary), static_dir, log_path, port)
    result = {
        "metadata": {
            "label": args.label,
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "environment": environment_metadata(),
            "build": build_metadata(Path(args.binary).resolve()),
            "client": "Python raw sockets; fixed-depth pipelining; client-bound",
            "client_location": "same host, loopback 127.0.0.1",
            "connections": args.connections,
            "depth": args.depth,
            "duration_seconds": args.duration,
            "warmup_seconds": args.warmup,
        },
    }
    try:
        wait_ready(port, 10)
        counts = [0]
        errors: list[str] = []
        stop = threading.Event()
        threads = [
            threading.Thread(
                target=worker,
                args=(port, args.depth, args.path, expected_body, stop, counts,
                      errors),
            )
            for _ in range(args.connections)
        ]
        for thread in threads:
            thread.start()
        time.sleep(args.warmup)
        counts[0] = 0
        client_cpu_start = time.process_time()
        started = time.monotonic()
        time.sleep(args.duration)
        elapsed = time.monotonic() - started
        client_cpu = time.process_time() - client_cpu_start
        stop.set()
        for thread in threads:
            thread.join(timeout=5)
        result["completed_requests"] = counts[0]
        result["errors"] = errors[:20]
        result["error_count"] = len(errors)
        result["wall_seconds"] = round(elapsed, 3)
        result["successful_rps"] = round(counts[0] / elapsed, 2) if elapsed else 0.0
        result["client_cpu_seconds"] = round(client_cpu, 3)
        result["client_cpu_percent"] = round(
            100.0 * client_cpu / max(elapsed, 1e-9), 1
        )
    finally:
        proc.send_signal(15)
        status, usage = wait_for_child(proc, 10)
        if status is None:
            proc.kill()
            status, usage = wait_for_child(proc, 5)
        result["server_exit_code"] = proc.returncode
        result["server_cpu_seconds"] = round(
            usage.ru_utime + usage.ru_stime, 3
        ) if usage else None
        result["server_max_rss_kb"] = max_rss_kb(usage)
    result["raw_server_log"] = str(log_path)
    result["correct"] = (
        result.get("error_count") == 0
        and result.get("completed_requests", 0) > 0
        and all(
            not error for error in result.get("errors", [])
        )
    )
    result_path = output_dir / "result.json"
    result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["correct"] else 1


if __name__ == "__main__":
    sys.exit(main())
