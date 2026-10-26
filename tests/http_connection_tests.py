#!/usr/bin/env python3
"""Event-loop keep-alive, buffering, pipelining, and connection lifetime tests."""

from __future__ import annotations

import argparse
import concurrent.futures
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


def start_server(temp_root: Path, binary: Path, extra_env: dict | None = None):
    static_dir = temp_root / "static"
    static_dir.mkdir(exist_ok=True)
    (static_dir / "index.html").write_bytes(b"INDEX")
    (static_dir / "about.html").write_bytes(b"ABOUT")
    (static_dir / "404.html").write_bytes(b"NOT FOUND")
    port = free_port()
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(temp_root / "test.db"),
            "ENABLE_LOGGING": "false",
            "MAX_CONNECTIONS": "64",
            "HEADER_TIMEOUT_MS": "2000",
            "WRITE_TIMEOUT_MS": "2000",
            "KEEPALIVE_TIMEOUT_MS": "2000",
            "MAX_REQUESTS_PER_CONNECTION": "100",
            "MAX_PIPELINED_REQUESTS": "8",
            "SHUTDOWN_GRACE_MS": "400",
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


def request(path: str, version: str = "HTTP/1.1",
            connection: str | None = None) -> bytes:
    headers = f"GET {path} {version}\r\nHost: localhost\r\n"
    if connection:
        headers += f"Connection: {connection}\r\n"
    return (headers + "\r\n").encode("ascii")


def status_line(response: tuple[str, dict[str, str], bytes]) -> str:
    return response[0]


def assert_status(response: tuple[str, dict[str, str], bytes], expected: str) -> None:
    if not status_line(response).startswith(expected):
        raise AssertionError(f"expected {expected!r}, got {status_line(response)!r}")


def assert_connection(response: tuple[str, dict[str, str], bytes], value: str) -> None:
    actual = response[1].get("connection", "").lower()
    if value not in actual:
        raise AssertionError(f"expected Connection containing {value!r}, got {actual!r}")


def test_http11_sequential_keepalive(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/"))
            first = reader.read_response()
            assert_status(first, "HTTP/1.1 200")
            assert_connection(first, "keep-alive")
            if first[2] != b"INDEX":
                raise AssertionError(f"first body {first[2]!r}")

            sock.sendall(request("/about"))
            second = reader.read_response()
            assert_status(second, "HTTP/1.1 200")
            assert_connection(second, "keep-alive")
            if second[2] != b"ABOUT":
                raise AssertionError(f"second body {second[2]!r}")

            sock.sendall(request("/", connection="close"))
            third = reader.read_response()
            assert_status(third, "HTTP/1.1 200")
            assert_connection(third, "close")
            if reader.read_eof() != b"":
                raise AssertionError("connection stayed open after Connection: close")
    finally:
        stop_server(server)


def test_http10_persistence(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/", version="HTTP/1.0"))
            response = reader.read_response()
            assert_status(response, "HTTP/1.1 200")
            assert_connection(response, "close")
            if reader.read_eof() != b"":
                raise AssertionError("HTTP/1.0 default did not close")
    finally:
        stop_server(server)

    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/", version="HTTP/1.0", connection="keep-alive"))
            first = reader.read_response()
            assert_status(first, "HTTP/1.1 200")
            assert_connection(first, "keep-alive")
            sock.sendall(request("/about", version="HTTP/1.0", connection="keep-alive"))
            second = reader.read_response()
            assert_status(second, "HTTP/1.1 200")
            if second[2] != b"ABOUT":
                raise AssertionError("HTTP/1.0 keep-alive second response body")
            sock.sendall(request("/", version="HTTP/1.0", connection="close"))
            third = reader.read_response()
            assert_connection(third, "close")
            if reader.read_eof() != b"":
                raise AssertionError("HTTP/1.0 keep-alive did not close on request")
    finally:
        stop_server(server)


def test_pipelined_ordered_responses(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/about") + request("/") + request("/missing"))
            responses = [reader.read_response() for _ in range(3)]
        expected = [(b"ABOUT", "200"), (b"INDEX", "200"), (b"NOT FOUND", "404")]
        for response, (body, status) in zip(responses, expected):
            if not status_line(response).startswith(f"HTTP/1.1 {status}"):
                raise AssertionError(f"pipeline status {status_line(response)!r}")
            if response[2] != body:
                raise AssertionError(f"pipeline order/body {response[2]!r}")
    finally:
        stop_server(server)


def test_close_request_ignores_buffered_requests(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(
                request("/", connection="close") + request("/about")
            )
            first = reader.read_response()
            assert_status(first, "HTTP/1.1 200")
            assert_connection(first, "close")
            if reader.read_eof() != b"":
                raise AssertionError("buffered request after close was processed")
    finally:
        stop_server(server)


def test_invalid_body_mid_pipeline(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            invalid = (
                b"POST /about HTTP/1.1\r\nHost: localhost\r\n"
                b"Content-Length: 3\r\n\r\nabc"
            )
            sock.sendall(request("/") + invalid + request("/about"))
            first = reader.read_response()
            second = reader.read_response()
            assert_status(first, "HTTP/1.1 200")
            assert_status(second, "HTTP/1.1 400")
            assert_connection(second, "close")
            if reader.read_eof() != b"":
                raise AssertionError("body bytes were interpreted as a request")
    finally:
        stop_server(server)


def test_pipeline_limit_preserves_order(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root, binary, extra_env={"MAX_PIPELINED_REQUESTS": 1}
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(
                request("/about") + request("/") + request("/missing")
            )
            responses = [reader.read_response() for _ in range(3)]
        bodies = [response[2] for response in responses]
        if bodies != [b"ABOUT", b"INDEX", b"NOT FOUND"]:
            raise AssertionError(f"pipeline limit order {bodies!r}")
    finally:
        stop_server(server)


def test_input_buffer_limit(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root, binary, extra_env={"MAX_CONNECTION_INPUT_BYTES": 4096}
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(
                b"GET / HTTP/1.1\r\nHost: localhost\r\nX-Big: "
                + b"a" * 5000
                + b"\r\n\r\n"
            )
            response = reader.read_response()
            assert_status(response, "HTTP/1.1 431")
    finally:
        stop_server(server)


def test_max_requests_closes(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root, binary, extra_env={"MAX_REQUESTS_PER_CONNECTION": 2}
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/") + request("/about") + request("/"))
            first = reader.read_response()
            second = reader.read_response()
            assert_status(first, "HTTP/1.1 200")
            assert_connection(first, "keep-alive")
            assert_status(second, "HTTP/1.1 200")
            assert_connection(second, "close")
            if reader.read_eof() != b"":
                raise AssertionError("request beyond MAX_REQUESTS_PER_CONNECTION ran")
    finally:
        stop_server(server)


def test_idle_keepalive_timeout(binary: Path, root: Path) -> None:
    server, port, _ = start_server(
        root, binary, extra_env={"KEEPALIVE_TIMEOUT_MS": 150}
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/"))
            first = reader.read_response()
            assert_status(first, "HTTP/1.1 200")
            time.sleep(0.35)
            if reader.read_eof() != b"":
                raise AssertionError("idle keep-alive connection sent extra data")
    finally:
        stop_server(server)


def test_upgrade_is_not_enabled(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(
                b"GET / HTTP/1.1\r\nHost: localhost\r\n"
                b"Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n"
            )
            response = reader.read_response()
            if status_line(response).startswith("HTTP/1.1 101"):
                raise AssertionError("unexpected protocol switch")
            assert_status(response, "HTTP/1.1 200")
            if response[2] != b"INDEX":
                raise AssertionError("upgrade request did not receive normal body")
    finally:
        stop_server(server)


def test_partial_header_eof_releases_slot(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n")
            sock.shutdown(socket.SHUT_WR)
            sock.settimeout(2)
            response = b""
            while True:
                try:
                    chunk = sock.recv(4096)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                response += chunk
        if response:
            raise AssertionError(f"partial header: unexpected response {response[:32]!r}")

        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/"))
            assert_status(reader.read_response(), "HTTP/1.1 200")
    finally:
        stop_server(server)


def test_half_close_completes_request(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/about"))
            sock.shutdown(socket.SHUT_WR)
            response = reader.read_response()
            assert_status(response, "HTTP/1.1 200")
            if response[2] != b"ABOUT":
                raise AssertionError("half-close body mismatch")
            if reader.read_eof() != b"":
                raise AssertionError("half-close connection remained open")
    finally:
        stop_server(server)


def test_many_waiting_connections(binary: Path, root: Path) -> None:
    server, port, _ = start_server(root, binary)
    sockets = []
    try:
        for _ in range(16):
            sockets.append(socket.create_connection(("127.0.0.1", port), timeout=3))
        time.sleep(0.1)

        def one_request(index: int) -> str:
            sock = sockets[index]
            sock.sendall(request("/"))
            return status_line(HttpResponseReader(sock).read_response())

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
            statuses = list(executor.map(one_request, range(len(sockets))))
        if not all(status.startswith("HTTP/1.1 200") for status in statuses):
            raise AssertionError(f"many connections: statuses {statuses!r}")
    finally:
        for sock in sockets:
            sock.close()
        stop_server(server)


def test_ipv6_loopback_when_available(binary: Path, root: Path) -> None:
    if not socket.has_ipv6:
        return
    server, port, _ = start_server(root, binary)
    try:
        try:
            sock = socket.create_connection(("::1", port), timeout=2)
        except OSError:
            return
        with sock:
            reader = HttpResponseReader(sock)
            sock.sendall(request("/"))
            assert_status(reader.read_response(), "HTTP/1.1 200")
    finally:
        stop_server(server)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="http-server-connection-tests-") as temp:
        root = Path(temp)
        tests = [
            (test_http11_sequential_keepalive, root / "keepalive"),
            (test_http10_persistence, root / "http10"),
            (test_pipelined_ordered_responses, root / "pipeline"),
            (test_close_request_ignores_buffered_requests, root / "close"),
            (test_invalid_body_mid_pipeline, root / "invalid-body"),
            (test_pipeline_limit_preserves_order, root / "pipeline-limit"),
            (test_input_buffer_limit, root / "input-limit"),
            (test_max_requests_closes, root / "max-requests"),
            (test_idle_keepalive_timeout, root / "idle"),
            (test_upgrade_is_not_enabled, root / "upgrade"),
            (test_partial_header_eof_releases_slot, root / "partial-header"),
            (test_half_close_completes_request, root / "half-close"),
            (test_many_waiting_connections, root / "many-connections"),
            (test_ipv6_loopback_when_available, root / "ipv6"),
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

    print("all connection handling tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
