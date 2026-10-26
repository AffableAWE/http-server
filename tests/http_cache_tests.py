#!/usr/bin/env python3
"""Integration tests for the immutable static-asset cache."""

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


def request(port: int, path: str) -> tuple[int, dict[str, str], bytes]:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request("GET", path, headers={"Host": "localhost"})
    response = connection.getresponse()
    body = response.read()
    headers = {key.lower(): value for key, value in response.getheaders()}
    status = response.status
    connection.close()
    return status, headers, body


def start_server(
    binary: Path, root: Path, static_dir: Path, extra_env: dict | None = None
) -> tuple[subprocess.Popen, int]:
    port = free_port()
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(root / "unused.db"),
            "ENABLE_LOGGING": "false",
            "SHUTDOWN_GRACE_MS": "500",
        }
    )
    if extra_env:
        env.update({key: str(value) for key, value in extra_env.items()})
    log = (root / "server.log").open("wb")
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
    return server, port


def stop_server(server: subprocess.Popen) -> None:
    if server.poll() is None:
        server.send_signal(signal.SIGTERM)
    server.wait(timeout=10)
    if server.returncode != 0:
        raise AssertionError(f"server exited with status {server.returncode}")


def make_static(root: Path) -> Path:
    static_dir = root / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(b"INDEX BODY")
    (static_dir / "about.html").write_bytes(b"ABOUT BODY")
    (static_dir / "aboutStyle.css").write_bytes(b"body { color: red; }")
    (static_dir / "404Style.css").write_bytes(b"body { color: black; }")
    (static_dir / "404.html").write_bytes(b"NOT FOUND BODY")
    return static_dir


def test_preloaded_content(binary: Path, root: Path) -> None:
    static_dir = make_static(root)
    server, port = start_server(binary, root, static_dir)
    try:
        status, headers, body = request(port, "/")
        if (status, headers.get("content-type"), body) != (
            200,
            "text/html",
            b"INDEX BODY",
        ):
            raise AssertionError("cache: wrong index response")
        status, headers, body = request(port, "/about")
        if (status, headers.get("content-type"), body) != (
            200,
            "text/html",
            b"ABOUT BODY",
        ):
            raise AssertionError("cache: wrong about response")
        status, headers, body = request(port, "/aboutStyle.css")
        if (status, headers.get("content-type"), body) != (
            200,
            "text/css",
            b"body { color: red; }",
        ):
            raise AssertionError("cache: wrong CSS response")
        status, _headers, body = request(port, "/missing")
        if (status, body) != (404, b"NOT FOUND BODY"):
            raise AssertionError("cache: wrong fallback response")
        status, _headers, body = request(port, "/404")
        if (status, body) != (200, b"NOT FOUND BODY"):
            raise AssertionError("cache: wrong explicit /404 response")
        status, _headers, body = request(port, "/about?x=1")
        if (status, body) != (200, b"ABOUT BODY"):
            raise AssertionError("cache: query path not separated")

        # Removing source files after startup must not affect cached responses.
        (static_dir / "index.html").rename(static_dir / "index.html.removed")
        (static_dir / "about.html").rename(static_dir / "about.html.removed")
        status, _headers, body = request(port, "/")
        if (status, body) != (200, b"INDEX BODY"):
            raise AssertionError("cache: index was reread from disk")
        status, _headers, body = request(port, "/about")
        if (status, body) != (200, b"ABOUT BODY"):
            raise AssertionError("cache: about was reread from disk")
    finally:
        stop_server(server)


def test_missing_assets_fall_back(binary: Path, root: Path) -> None:
    static_dir = root / "missing-static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(b"INDEX")
    (static_dir / "404.html").write_bytes(b"FALLBACK")
    server, port = start_server(binary, root, static_dir)
    try:
        status, headers, body = request(port, "/about")
        if (status, headers.get("content-type"), body) != (
            404,
            "text/html",
            b"FALLBACK",
        ):
            raise AssertionError("cache: missing known asset did not become 404")
    finally:
        stop_server(server)


def test_missing_fallback_uses_inline_body(binary: Path, root: Path) -> None:
    static_dir = root / "no-fallback-static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(b"INDEX")
    server, port = start_server(binary, root, static_dir)
    try:
        status, _headers, body = request(port, "/missing")
        if status != 404 or b"404 Not Found" not in body:
            raise AssertionError("cache: inline fallback was not used")
        status, _headers, body = request(port, "/")
        if (status, body) != (200, b"INDEX"):
            raise AssertionError("cache: index failed without 404 file")
    finally:
        stop_server(server)


def run_failure(binary: Path, root: Path, static_dir: Path, env_extra: dict) -> str:
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(free_port()),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(root / "unused.db"),
            "ENABLE_LOGGING": "false",
        }
    )
    env.update({key: str(value) for key, value in env_extra.items()})
    result = subprocess.run(
        [str(binary)],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode == 0:
        raise AssertionError("asset limit failure unexpectedly started")
    if "Initialization failed" not in result.stderr:
        raise AssertionError(f"asset limit failure: no diagnostic: {result.stderr!r}")
    return result.stderr


def test_individual_size_limit(binary: Path, root: Path) -> None:
    static_dir = root / "oversized-static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(b"x" * 2048)
    (static_dir / "404.html").write_bytes(b"fallback")
    stderr = run_failure(
        binary,
        root,
        static_dir,
        {"MAX_ASSET_BYTES": 1024, "MAX_ASSET_CACHE_BYTES": 1024 * 1024},
    )
    if "MAX_ASSET_BYTES" not in stderr:
        raise AssertionError(f"individual limit diagnostic missing: {stderr!r}")


def test_total_cache_limit(binary: Path, root: Path) -> None:
    static_dir = root / "total-limit-static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(b"i" * 600)
    (static_dir / "about.html").write_bytes(b"a" * 600)
    (static_dir / "404.html").write_bytes(b"f" * 20)
    stderr = run_failure(
        binary,
        root,
        static_dir,
        {"MAX_ASSET_BYTES": 1024, "MAX_ASSET_CACHE_BYTES": 1000},
    )
    if "MAX_ASSET_CACHE_BYTES" not in stderr:
        raise AssertionError(f"total cache limit diagnostic missing: {stderr!r}")


def test_restart_to_reload(binary: Path, root: Path) -> None:
    static_dir = root / "reload-static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(b"ONE")
    (static_dir / "404.html").write_bytes(b"FALLBACK")

    server, port = start_server(binary, root, static_dir)
    try:
        status, _headers, body = request(port, "/")
        if (status, body) != (200, b"ONE"):
            raise AssertionError("reload: first cached body was wrong")
    finally:
        stop_server(server)

    (static_dir / "index.html").write_bytes(b"TWO")
    server, port = start_server(binary, root, static_dir)
    try:
        status, _headers, body = request(port, "/")
        if (status, body) != (200, b"TWO"):
            raise AssertionError("reload: restart did not refresh cache")
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

    with tempfile.TemporaryDirectory(prefix="http-server-cache-tests-") as temp:
        root = Path(temp)
        tests = [
            (test_preloaded_content, root / "content"),
            (test_missing_assets_fall_back, root / "missing"),
            (test_missing_fallback_uses_inline_body, root / "inline-fallback"),
            (test_individual_size_limit, root / "individual-limit"),
            (test_total_cache_limit, root / "total-limit"),
            (test_restart_to_reload, root / "reload"),
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

    print("all asset cache tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
