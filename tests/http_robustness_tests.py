#!/usr/bin/env python3
"""Bounded mixed-load, backpressure, and shutdown robustness tests."""

from __future__ import annotations

import argparse
import concurrent.futures
import http.client
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from http_test_util import HttpResponseReader

ROOT = Path(__file__).resolve().parents[1]
BIG_BODY = 4 * 1024 * 1024


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=0.25)
            connection.request("GET", "/about", headers={"Host": "localhost"})
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def start_server(root: Path, binary: Path, extra_env: dict | None = None):
    static_dir = root / "static"
    static_dir.mkdir(exist_ok=True)
    (static_dir / "index.html").write_bytes(b"x" * BIG_BODY)
    (static_dir / "about.html").write_bytes(b"ABOUT")
    (static_dir / "404.html").write_bytes(b"NOT FOUND")
    port = free_port()
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(root / "test.db"),
            "ENABLE_LOGGING": "false",
            "MAX_CONNECTIONS": "32",
            "MAX_PIPELINED_REQUESTS": "2",
            "MAX_REQUESTS_PER_CONNECTION": "100",
            "HEADER_TIMEOUT_MS": "400",
            "WRITE_TIMEOUT_MS": "300",
            "KEEPALIVE_TIMEOUT_MS": "500",
            "SHUTDOWN_GRACE_MS": "300",
        }
    )
    if extra_env:
        env.update({key: str(value) for key, value in extra_env.items()})
    log_path = root / "server.log"
    log = log_path.open("wb")
    server = subprocess.Popen(
        [str(binary)],
        cwd=ROOT,
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    log.close()
    try:
        wait_ready(port, 10)
    except Exception:
        server.kill()
        server.wait(timeout=5)
        raise
    return server, port, log_path


def stop_server(server: subprocess.Popen) -> None:
    if server.poll() is None:
        server.terminate()
    server.wait(timeout=10)
    if server.returncode != 0:
        raise AssertionError(f"server exited with status {server.returncode}")


def request(path: str = "/about", connection: str | None = None) -> bytes:
    text = f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
    if connection:
        text += f"Connection: {connection}\r\n"
    return (text + "\r\n").encode("ascii")


def sequential_client(port: int, count: int) -> list[int]:
    statuses: list[int] = []
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        reader = HttpResponseReader(sock)
        for _ in range(count):
            sock.sendall(request())
            status, _headers, body = reader.read_response()
            if body != b"ABOUT":
                raise AssertionError(f"normal client body {body!r}")
            statuses.append(int(status.split()[1]))
    return statuses


def pipelined_client(port: int, count: int) -> list[int]:
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        reader = HttpResponseReader(sock)
        sock.sendall(request() * count)
        statuses = []
        for _ in range(count):
            status, _headers, body = reader.read_response()
            if body != b"ABOUT":
                raise AssertionError(f"pipeline body {body!r}")
            statuses.append(int(status.split()[1]))
        return statuses


def test_mixed_load_progress(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    idle_sockets = []
    partial_sockets = []
    slow_sockets = []
    try:
        for _ in range(3):
            idle_sockets.append(socket.create_connection(("127.0.0.1", port), timeout=3))
            partial = socket.create_connection(("127.0.0.1", port), timeout=3)
            partial.sendall(b"GET / HTTP/1.1\r\nHost: local")
            partial_sockets.append(partial)
            slow = socket.create_connection(("127.0.0.1", port), timeout=3)
            slow.sendall(request("/"))
            slow_sockets.append(slow)

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [
                executor.submit(sequential_client, port, 10) for _ in range(4)
            ] + [
                executor.submit(pipelined_client, port, 6) for _ in range(3)
            ] + [
                executor.submit(sequential_client, port, 5) for _ in range(3)
            ]
            results = [future.result(timeout=15) for future in futures]

        for statuses in results:
            if not all(status == 200 for status in statuses):
                raise AssertionError(f"mixed load statuses {statuses!r}")
    finally:
        for sock in idle_sockets + partial_sockets + slow_sockets:
            sock.close()
        stop_server(server)


def test_admission_recovery_after_timeout(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root, binary, extra_env={"MAX_CONNECTIONS": 1}
    )
    try:
        idle = socket.create_connection(("127.0.0.1", port), timeout=2)
        with socket.create_connection(("127.0.0.1", port), timeout=2) as probe:
            reader = HttpResponseReader(probe)
            probe.sendall(request())
            status, _headers, _body = reader.read_response()
            if not status.startswith("HTTP/1.1 503"):
                raise AssertionError(f"admission limit expected 503, got {status!r}")
        idle.settimeout(2)
        reader = HttpResponseReader(idle)
        timeout_status, _headers, _body = reader.read_response()
        if not timeout_status.startswith("HTTP/1.1 408"):
            raise AssertionError(f"idle timeout expected 408, got {timeout_status!r}")
        idle.close()
        time.sleep(0.1)
        if not all(status == 200 for status in sequential_client(port, 2)):
            raise AssertionError("admission slot did not recover after timeout")
    finally:
        stop_server(server)


def test_pipeline_backpressure_and_order(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root, binary, extra_env={"MAX_PIPELINED_REQUESTS": 2}
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/about") * 20)
            bodies = []
            for _ in range(20):
                status, _headers, body = reader.read_response()
                if not status.startswith("HTTP/1.1 200"):
                    raise AssertionError(f"backpressure status {status!r}")
                bodies.append(body)
        if bodies != [b"ABOUT"] * 20:
            raise AssertionError("pipeline backpressure changed response order")
    finally:
        stop_server(server)


def test_half_close_multiple_buffered_requests(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root, binary, extra_env={"MAX_PIPELINED_REQUESTS": 2}
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/about") * 6)
            sock.shutdown(socket.SHUT_WR)
            statuses = []
            for _ in range(6):
                status, _headers, body = reader.read_response()
                if body != b"ABOUT":
                    raise AssertionError(f"half-close body {body!r}")
                statuses.append(status)
            if not all(status.startswith("HTTP/1.1 200") for status in statuses):
                raise AssertionError(f"half-close statuses {statuses!r}")
            if reader.read_eof() != b"":
                raise AssertionError("half-close pipeline did not close")
    finally:
        stop_server(server)


def test_diagnostics_are_aggregate_and_close_aware(binary: Path, root: Path) -> None:
    server, port, log_path = start_server(
        root, binary, extra_env={"DIAGNOSTICS_INTERVAL_MS": 100}
    )
    try:
        for _ in range(2):
            with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                reader = HttpResponseReader(sock)
                sock.sendall(request("/about", connection="close"))
                status, _headers, body = reader.read_response()
                if not status.startswith("HTTP/1.1 200") or body != b"ABOUT":
                    raise AssertionError("diagnostics requests failed")
        time.sleep(0.2)
    finally:
        stop_server(server)
    text = log_path.read_text(encoding="utf-8", errors="replace")
    final_lines = [line for line in text.splitlines()
                   if "[DIAG]" in line and "final=1" in line]
    if not final_lines:
        raise AssertionError("final diagnostics line missing")
    line = final_lines[-1]
    fields = dict(
        (key, int(value))
        for key, value in __import__("re").findall(r"(\w+)=(\d+)", line)
    )
    if fields.get("accepted", 0) < 2 or fields.get("responses", 0) < 2:
        raise AssertionError(f"diagnostic counters did not advance: {line}")
    if fields.get("requested_close", 0) < 2:
        raise AssertionError(f"explicit close not counted: {line}")
    if fields.get("peer_resets", 0) != 0 or fields.get("read_errors", 0) != 0:
        raise AssertionError(f"normal close counted as reset/error: {line}")


def test_busy_pipeline_shutdown_is_bounded(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root,
        binary,
        extra_env={
            "WRITE_TIMEOUT_MS": 5000,
            "SHUTDOWN_GRACE_MS": 300,
            "MAX_PIPELINED_REQUESTS": 8,
        },
    )
    client = socket.create_connection(("127.0.0.1", port), timeout=3)
    try:
        client.sendall(request("/") * 12)
        time.sleep(0.1)
        started = time.monotonic()
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=3)
        elapsed = time.monotonic() - started
        if server.returncode != 0:
            raise AssertionError(f"busy shutdown exit {server.returncode}")
        if elapsed > 2.0:
            raise AssertionError(f"busy shutdown took {elapsed:.2f}s")
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="http-server-robustness-tests-") as temp:
        root = Path(temp)
        tests = [
            (test_mixed_load_progress, root / "mixed"),
            (test_admission_recovery_after_timeout, root / "admission"),
            (test_pipeline_backpressure_and_order, root / "backpressure"),
            (test_half_close_multiple_buffered_requests, root / "half-close"),
            (test_busy_pipeline_shutdown_is_bounded, root / "shutdown"),
            (test_diagnostics_are_aggregate_and_close_aware, root / "diagnostics"),
        ]
        for test, test_root in tests:
            test_root.mkdir(parents=True, exist_ok=True)
            try:
                test(binary, test_root)
            except Exception as error:
                print(f"FAIL: {error}", file=sys.stderr)
                log = test_root / "server.log"
                if log.is_file():
                    print(
                        log.read_text(encoding="utf-8", errors="replace"),
                        file=sys.stderr,
                    )
                return 1

    print("all robustness tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
