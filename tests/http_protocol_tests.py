#!/usr/bin/env python3
"""Integration tests for request parsing, framing, and response statuses."""

from __future__ import annotations

import argparse
import http.client
import os
import socket
import struct
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


def exchange(
    port: int, payload: bytes, timeout: float = 5.0
) -> tuple[str, dict[str, str], bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        try:
            sock.sendall(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass
        try:
            status_line, headers, body = HttpResponseReader(sock).read_response()
        except (EOFError, socket.timeout):
            return "", {}, b""
    return status_line, headers, body

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


def run_case(port: int, name: str, payload: bytes, expected: str) -> None:
    status, _headers, body = exchange(port, payload)
    if not status.startswith(expected):
        raise AssertionError(f"{name}: expected {expected!r}, got {status!r}")
    if not body:
        raise AssertionError(f"{name}: empty response body")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    port = free_port()
    with tempfile.TemporaryDirectory(prefix="http-server-tests-") as temp_dir:
        log_path = Path(temp_dir) / "server.log"
        env = os.environ.copy()
        env.update(
            {
                "PORT": str(port),
                "STATIC_DIR": str(ROOT / "static"),
                "DB_PATH": str(Path(temp_dir) / "test.db"),
                "ENABLE_LOGGING": "false",
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
                about_body = (ROOT / "static" / "about.html").read_bytes()

                cases = [
                    (
                        "valid",
                        b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n",
                        "HTTP/1.1 200",
                    ),
                    (
                        "case-insensitive Host",
                        b"GET / HTTP/1.1\r\nhOsT: example.com\r\n\r\n",
                        "HTTP/1.1 200",
                    ),
                    (
                        "HTTP/1.0 without Host",
                        b"GET / HTTP/1.0\r\n\r\n",
                        "HTTP/1.1 200",
                    ),
                    (
                        "Content-Length zero",
                        b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 200",
                    ),
                    (
                        "missing version",
                        b"GET /\r\nHost: x\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "unsupported version",
                        b"GET / HTTP/9.9\r\nHost: x\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "missing Host",
                        b"GET / HTTP/1.1\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "duplicate Host",
                        b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "absolute target",
                        b"GET http://example.com/ HTTP/1.1\r\nHost: x\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "header without colon",
                        b"GET / HTTP/1.1\r\nHost x\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "obsolete folding",
                        b"GET / HTTP/1.1\r\nHost: x\r\n X: y\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "nonzero Content-Length",
                        b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello",
                        "HTTP/1.1 400",
                    ),
                    (
                        "duplicate Content-Length",
                        b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
                        b"Content-Length: 0\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "Transfer-Encoding",
                        b"GET / HTTP/1.1\r\nHost: x\r\n"
                        b"Transfer-Encoding: chunked\r\n\r\n",
                        "HTTP/1.1 501",
                    ),
                    (
                        "conflicting framing",
                        b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n"
                        b"Transfer-Encoding: chunked\r\n\r\n",
                        "HTTP/1.1 400",
                    ),
                    (
                        "connection close with trailing bytes",
                        b"GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                        b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
                        "HTTP/1.1 200",
                    ),
                    (
                        "POST rejected",
                        b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
                        "HTTP/1.1 405",
                    ),
                ]

                for name, payload, expected in cases:
                    run_case(port, name, payload, expected)
                not_found_body = (ROOT / "static" / "404.html").read_bytes()

                status, _headers, body = exchange(
                    port,
                    b"GET /about?x=1 HTTP/1.1\r\nHost: x\r\n\r\n",
                )
                if not status.startswith("HTTP/1.1 200"):
                    raise AssertionError(f"query route: got {status!r}")
                if body != about_body:
                    raise AssertionError("query route: wrong body")

                status, _headers, body = exchange(
                    port, b"GET /missing HTTP/1.1\r\nHost: x\r\n\r\n"
                )
                if not status.startswith("HTTP/1.1 404"):
                    raise AssertionError(f"fallback route: got {status!r}")
                if body != not_found_body:
                    raise AssertionError("fallback route: wrong body")

                status, headers, _body = exchange(
                    port,
                    b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
                )
                if not status.startswith("HTTP/1.1 405"):
                    raise AssertionError(f"method rejection: got {status!r}")
                if headers.get("allow") != "GET":
                    raise AssertionError(
                        f"method rejection: Allow={headers.get('allow')!r}"
                    )

                # Send requests and reset each connection before the server
                # can finish writing. SIGPIPE must not terminate the process.
                for _ in range(100):
                    with socket.create_connection(("127.0.0.1", port)) as sock:
                        sock.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
                        sock.setsockopt(
                            socket.SOL_SOCKET,
                            socket.SO_LINGER,
                            struct.pack("ii", 1, 0),
                        )
                time.sleep(0.2)
                run_case(
                    port,
                    "alive after abrupt closes",
                    b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
                    "HTTP/1.1 200",
                )

                too_many_headers = b"".join(
                    f"X-{index}: y\r\n".encode() for index in range(101)
                )
                run_case(
                    port,
                    "101 headers",
                    b"GET / HTTP/1.1\r\nHost: x\r\n"
                    + too_many_headers
                    + b"\r\n",
                    "HTTP/1.1 431",
                )

                run_case(
                    port,
                    "oversized header field",
                    b"GET / HTTP/1.1\r\nHost: x\r\nX-Big: "
                    + b"a" * 9000
                    + b"\r\n\r\n",
                    "HTTP/1.1 431",
                )

                run_case(
                    port,
                    "oversized header block",
                    b"GET / HTTP/1.1\r\nHost: x\r\nX-Big: "
                    + b"a" * (70 * 1024)
                    + b"\r\n\r\n",
                    "HTTP/1.1 431",
                )

                run_case(
                    port,
                    "oversized request line",
                    b"GET /" + b"a" * (9 * 1024)
                    + b" HTTP/1.1\r\nHost: x\r\n\r\n",
                    "HTTP/1.1 414",
                )
            except Exception as error:
                print(f"FAIL: {error}", file=sys.stderr)
                if log_path.is_file():
                    print(log_path.read_text(encoding="utf-8", errors="replace"),
                          file=sys.stderr)
                return 1
            finally:
                if server.poll() is None:
                    server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)

    print("all protocol tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
