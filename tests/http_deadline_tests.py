#!/usr/bin/env python3
"""Integration tests for header deadlines and response-write deadlines."""

from __future__ import annotations

import argparse
import http.client
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER_TIMEOUT_MS = 500
WRITE_TIMEOUT_MS = 300
BIG_BODY_SIZE = 8 * 1024 * 1024


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def connect(port: int, timeout: float = 3.0) -> socket.socket:
    return socket.create_connection(("127.0.0.1", port), timeout=timeout)


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


def read_status(port: int, path: str = "/about", timeout: float = 3.0) -> str:
    with connect(port, timeout) as sock:
        sock.sendall(
            f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode()
        )
        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
    return bytes(response).split(b"\r\n", 1)[0].decode("latin1")


def read_until_closed(sock: socket.socket, limit: int = 2 * 1024 * 1024) -> bytes:
    received = bytearray()
    while len(received) < limit:
        try:
            chunk = sock.recv(65536)
        except (socket.timeout, ConnectionResetError):
            break
        if not chunk:
            break
        received.extend(chunk)
    return bytes(received)


def recover(port: int) -> None:
    time.sleep(0.4)
    status = read_status(port)
    if not status.startswith("HTTP/1.1 200"):
        raise AssertionError(f"slot recovery: expected 200, got {status!r}")


def test_idle_timeout(port: int) -> None:
    started = time.monotonic()
    with connect(port) as sock:
        response = read_until_closed(sock)
    elapsed = time.monotonic() - started
    if response and not response.startswith(b"HTTP/1.1 408"):
        raise AssertionError(f"idle timeout: unexpected response {response[:40]!r}")
    if not 0.35 <= elapsed <= 2.0:
        raise AssertionError(f"idle timeout: unexpected elapsed {elapsed:.2f}s")
    recover(port)


def test_trickled_headers(port: int) -> None:
    request = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
    started = time.monotonic()
    with connect(port) as sock:
        try:
            for offset in range(0, len(request), 4):
                sock.sendall(request[offset : offset + 4])
                time.sleep(0.1)
        except (BrokenPipeError, ConnectionResetError):
            pass
        response = read_until_closed(sock)
    elapsed = time.monotonic() - started
    if response and not response.startswith(b"HTTP/1.1 408"):
        raise AssertionError(f"trickle timeout: unexpected response {response[:40]!r}")
    if not 0.35 <= elapsed <= 2.0:
        raise AssertionError(f"trickle timeout: unexpected elapsed {elapsed:.2f}s")
    if elapsed > (HEADER_TIMEOUT_MS / 1000.0) + 1.0:
        raise AssertionError("trickled bytes extended the absolute header deadline")
    recover(port)


def test_valid_fragmented_request(port: int) -> None:
    request = b"GET /about HTTP/1.1\r\nHost: localhost\r\n\r\n"
    with connect(port) as sock:
        for offset in range(0, len(request), 8):
            sock.sendall(request[offset : offset + 8])
            time.sleep(0.01)
        response = bytearray()
        while b"\r\n\r\n" not in response:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
    status = bytes(response).split(b"\r\n", 1)[0].decode("latin1")
    if not status.startswith("HTTP/1.1 200"):
        raise AssertionError(f"fragmented request: expected 200, got {status!r}")
    recover(port)


def test_slow_reader(port: int) -> None:
    started = time.monotonic()
    with connect(port) as sock:
        sock.settimeout(0.2)
        sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")

        # Read continuously, but slowly: one 1 KiB chunk every 50 ms. This is
        # faster than SEND_TIMEOUT_MS (1000 ms), so the server's per-operation
        # timeout cannot be the cause, while the 8 MiB response cannot finish
        # inside WRITE_TIMEOUT_MS (300 ms). No buffer sizing is assumed.
        received = bytearray()
        while time.monotonic() - started < 2.0:
            try:
                chunk = sock.recv(1024)
            except socket.timeout:
                continue
            except ConnectionResetError:
                break
            if not chunk:
                break
            received.extend(chunk)
            time.sleep(0.05)

    elapsed = time.monotonic() - started
    if not 0.15 <= elapsed <= 2.5:
        raise AssertionError(
            f"slow reader: unexpected close time ({elapsed:.2f}s)"
        )
    if len(received) >= BIG_BODY_SIZE:
        raise AssertionError("slow reader: the full oversized body was delivered")
    if not received:
        raise AssertionError("slow reader: no response bytes were observed")
    recover(port)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    port = free_port()
    with tempfile.TemporaryDirectory(prefix="http-server-deadline-tests-") as temp_dir:
        static_dir = Path(temp_dir) / "static"
        static_dir.mkdir()
        (static_dir / "index.html").write_bytes(b"x" * BIG_BODY_SIZE)
        (static_dir / "about.html").write_bytes(b"about")
        (static_dir / "404.html").write_bytes(b"not found")

        env = os.environ.copy()
        env.update(
            {
                "PORT": str(port),
                "STATIC_DIR": str(static_dir),
                "DB_PATH": str(Path(temp_dir) / "test.db"),
                "ENABLE_LOGGING": "false",
                "MAX_CONNECTIONS": "1",
                "MAX_QUEUED_CONNECTIONS": "1",
                "RECV_TIMEOUT_MS": "1000",
                "SEND_TIMEOUT_MS": "1000",
                "HEADER_TIMEOUT_MS": str(HEADER_TIMEOUT_MS),
                "WRITE_TIMEOUT_MS": str(WRITE_TIMEOUT_MS),
            }
        )

        log_path = Path(temp_dir) / "server.log"
        with log_path.open("wb") as log:
            server = subprocess.Popen(
                [str(binary)],
                cwd=ROOT,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                wait_ready(port, 10)

                test_valid_fragmented_request(port)
                test_idle_timeout(port)
                test_trickled_headers(port)
                test_slow_reader(port)
            except Exception as error:
                print(f"FAIL: {error}", file=sys.stderr)
                if log_path.is_file():
                    print(
                        log_path.read_text(encoding="utf-8", errors="replace"),
                        file=sys.stderr,
                    )
                return 1
            finally:
                if server.poll() is None:
                    server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)

    print("all deadline integration tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
