#!/usr/bin/env python3
"""Integration tests for bounded admission and overload rejection."""

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

from http_test_util import HttpResponseReader

ROOT = Path(__file__).resolve().parents[1]


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exchange(port: int, payload: bytes, timeout: float = 5.0) -> tuple[str, bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(payload)
        try:
            status_line, _headers, body = HttpResponseReader(sock).read_response()
        except (EOFError, socket.timeout):
            return "", b""
    return status_line, body

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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    port = free_port()
    with tempfile.TemporaryDirectory(prefix="http-server-resource-tests-") as temp_dir:
        log_path = Path(temp_dir) / "server.log"
        env = os.environ.copy()
        env.update(
            {
                "PORT": str(port),
                "STATIC_DIR": str(ROOT / "static"),
                "DB_PATH": str(Path(temp_dir) / "test.db"),
                "ENABLE_LOGGING": "false",
                "MAX_CONNECTIONS": "1",
                "MAX_QUEUED_CONNECTIONS": "1",
            }
        )

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
                time.sleep(0.1)

                # Occupy the single admitted slot with a client that never
                # sends a request. The next connection must be rejected
                # without waiting for a worker.
                idle = socket.create_connection(("127.0.0.1", port), timeout=2)
                time.sleep(0.2)

                status, body = exchange(
                    port,
                    b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
                )
                if not status.startswith("HTTP/1.1 503"):
                    raise AssertionError(f"overload: expected 503, got {status!r}")
                if b"overloaded" not in body:
                    raise AssertionError(f"overload: unexpected body {body!r}")

                idle.close()
                time.sleep(0.3)

                status, _body = exchange(
                    port,
                    b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
                )
                if not status.startswith("HTTP/1.1 200"):
                    raise AssertionError(
                        f"post-release admission: expected 200, got {status!r}"
                    )
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

    print("all resource integration tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
