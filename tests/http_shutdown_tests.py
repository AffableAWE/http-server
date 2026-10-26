#!/usr/bin/env python3
"""Integration tests for bounded shutdown and socket cancellation."""

from __future__ import annotations

import argparse
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
GRACE_MS = 400
TOLERANCE_SECONDS = 2.0
BIG_BODY_SIZE = 8 * 1024 * 1024


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=0.25)
            connection.request("GET", "/", headers={"Host": "localhost"})
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("server did not become ready")


def start_server(
    temp_root: Path,
    port: int,
    big_body: bool,
    binary: Path,
    extra_env: dict | None = None,
) -> tuple:
    static_dir = temp_root / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(
        b"x" * BIG_BODY_SIZE if big_body else b"ok"
    )
    (static_dir / "404.html").write_bytes(b"not found")

    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(temp_root / "test.db"),
            "ENABLE_LOGGING": "false",
            "MAX_CONNECTIONS": "32",
            "MAX_QUEUED_CONNECTIONS": "32",
            "RECV_TIMEOUT_MS": "5000",
            "SEND_TIMEOUT_MS": "1000",
            "HEADER_TIMEOUT_MS": "10000",
            "WRITE_TIMEOUT_MS": "10000",
            "SHUTDOWN_GRACE_MS": str(GRACE_MS),
        }
    )
    if extra_env:
        env.update({key: str(value) for key, value in extra_env.items()})

    log_path = temp_root / "server.log"
    log = log_path.open("wb")
    server = subprocess.Popen(
        [str(binary)],
        cwd=ROOT,
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_ready(port, 10)
    except Exception:
        server.kill()
        server.wait(timeout=5)
        log.close()
        raise
    return server, log, log_path


def stop_and_measure(server: subprocess.Popen, signals: int = 1) -> float:
    started = time.monotonic()
    for _ in range(signals):
        try:
            server.send_signal(signal.SIGTERM)
        except ProcessLookupError:
            break
    try:
        server.wait(timeout=(GRACE_MS / 1000.0) + TOLERANCE_SECONDS)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=5)
        raise AssertionError("server did not exit within the shutdown tolerance")
    elapsed = time.monotonic() - started
    if server.returncode != 0:
        raise AssertionError(f"server exited with status {server.returncode}")
    return elapsed


def close_log(log) -> None:
    log.close()


def scenario_no_clients(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="shutdown-none-") as temp:
        root = Path(temp)
        port = free_port()
        server, log, _ = start_server(root, port, big_body=False, binary=binary)
        try:
            elapsed = stop_and_measure(server)
            if elapsed > (GRACE_MS / 1000.0) + TOLERANCE_SECONDS:
                raise AssertionError(f"no-client shutdown took {elapsed:.2f}s")
        finally:
            close_log(log)


def scenario_partial_headers(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="shutdown-partial-") as temp:
        root = Path(temp)
        port = free_port()
        server, log, _ = start_server(root, port, big_body=False, binary=binary)
        client = socket.create_connection(("127.0.0.1", port), timeout=2)
        client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n")
        time.sleep(0.15)
        try:
            elapsed = stop_and_measure(server, signals=3)
            if elapsed > (GRACE_MS / 1000.0) + TOLERANCE_SECONDS:
                raise AssertionError(
                    f"partial-header shutdown took {elapsed:.2f}s"
                )
        finally:
            client.close()
            close_log(log)


def scenario_idle_persistent(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="shutdown-idle-persistent-") as temp:
        root = Path(temp)
        port = free_port()
        server, log, _ = start_server(root, port, big_body=False, binary=binary)
        client = socket.create_connection(("127.0.0.1", port), timeout=2)
        try:
            client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            status, _headers, _body = HttpResponseReader(client).read_response()
            if not status.startswith("HTTP/1.1 200"):
                raise AssertionError(f"idle persistent setup: status {status!r}")
            elapsed = stop_and_measure(server)
            if elapsed > (GRACE_MS / 1000.0) + TOLERANCE_SECONDS:
                raise AssertionError(f"idle persistent shutdown took {elapsed:.2f}s")
        finally:
            client.close()
            close_log(log)


def scenario_slow_reader(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="shutdown-reader-") as temp:
        root = Path(temp)
        port = free_port()
        server, log, _ = start_server(root, port, big_body=True, binary=binary)
        client = socket.create_connection(("127.0.0.1", port), timeout=2)
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
        time.sleep(0.15)
        try:
            elapsed = stop_and_measure(server)
            if elapsed > (GRACE_MS / 1000.0) + TOLERANCE_SECONDS:
                raise AssertionError(f"slow-reader shutdown took {elapsed:.2f}s")
        finally:
            client.close()
            close_log(log)


def read_status(sock: socket.socket) -> str:
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response.extend(chunk)
    return bytes(response).split(b"\r\n", 1)[0].decode("latin1")


def scenario_admission_limit_shutdown(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="shutdown-admission-") as temp:
        root = Path(temp)
        port = free_port()
        server, log, _ = start_server(
            root,
            port,
            big_body=False,
            binary=binary,
            extra_env={
                "MAX_CONNECTIONS": 2,
            },
        )
        clients = []
        try:
            # The event loop has no worker queue; MAX_CONNECTIONS is the
            # admitted-connection bound. Two idle connections fill it, so the
            # third request must receive 503 and shutdown must still be bounded.
            first = socket.create_connection(("127.0.0.1", port), timeout=2)
            clients.append(first)
            time.sleep(0.25)
            second = socket.create_connection(("127.0.0.1", port), timeout=2)
            clients.append(second)
            time.sleep(0.25)

            third = socket.create_connection(("127.0.0.1", port), timeout=2)
            third.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            status = read_status(third)
            third.close()
            if not status.startswith("HTTP/1.1 503"):
                raise AssertionError(
                    f"admission shutdown setup: expected 503, got {status!r}"
                )

            elapsed = stop_and_measure(server, signals=2)
            if elapsed > (GRACE_MS / 1000.0) + TOLERANCE_SECONDS:
                raise AssertionError(f"admission shutdown took {elapsed:.2f}s")
        finally:
            for client in clients:
                client.close()
            close_log(log)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    try:
        scenario_no_clients(binary)
        scenario_partial_headers(binary)
        scenario_idle_persistent(binary)
        scenario_slow_reader(binary)
        scenario_admission_limit_shutdown(binary)
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("all shutdown integration tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
