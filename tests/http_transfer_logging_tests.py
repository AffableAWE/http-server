#!/usr/bin/env python3
"""Transfer-outcome logging and schema-migration integration tests."""

from __future__ import annotations

import argparse
import http.client
import os
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
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
    binary: Path,
    root: Path,
    static_dir: Path,
    db_path: Path,
    extra_env: dict | None = None,
) -> tuple[subprocess.Popen, int]:
    port = free_port()
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true",
            "SHUTDOWN_GRACE_MS": "1000",
            "WRITE_TIMEOUT_MS": "10000",
            "SEND_TIMEOUT_MS": "1000",
            "MAX_ASSET_BYTES": str(16 * 1024 * 1024),
            "MAX_ASSET_CACHE_BYTES": str(64 * 1024 * 1024),
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


def make_static(root: Path, large_index: bool) -> Path:
    static_dir = root / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(
        b"x" * BIG_BODY_SIZE if large_index else b"INDEX"
    )
    (static_dir / "about.html").write_bytes(b"ABOUT")
    (static_dir / "404.html").write_bytes(b"NOT FOUND")
    return static_dir


def request(port: int, path: str) -> int:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request("GET", path, headers={"Host": "localhost"})
    response = connection.getresponse()
    response.read()
    status = response.status
    connection.close()
    return status


def query_rows(db_path: Path) -> list[tuple]:
    with sqlite3.connect(db_path) as connection:
        return connection.execute(
            "SELECT path, status_code, bytes_sent, transfer_outcome, client_ip "
            "FROM access_logs ORDER BY id"
        ).fetchall()


def latest_for_path(db_path: Path, path: str) -> tuple:
    rows = [row for row in query_rows(db_path) if row[0] == path]
    if not rows:
        raise AssertionError(f"no log row for {path!r}")
    return rows[-1]


def test_complete_transfer(binary: Path, root: Path) -> None:
    static_dir = make_static(root, large_index=False)
    db_path = root / "complete.db"
    server, port = start_server(binary, root, static_dir, db_path)
    try:
        if request(port, "/about") != 200:
            raise AssertionError("complete transfer: request failed")
        time.sleep(0.2)
    finally:
        stop_server(server)

    path, status, bytes_sent, outcome, client_ip = latest_for_path(
        db_path, "/about"
    )
    if status != 200 or outcome != "complete" or bytes_sent <= 0:
        raise AssertionError(
            f"complete transfer: wrong row {path, status, bytes_sent, outcome}"
        )
    if client_ip not in ("127.0.0.1", "::1"):
        raise AssertionError(f"complete transfer: unexpected client_ip {client_ip!r}")


def test_timeout_outcome(binary: Path, root: Path) -> None:
    static_dir = make_static(root, large_index=True)
    db_path = root / "timeout.db"
    server, port = start_server(
        binary,
        root,
        static_dir,
        db_path,
        extra_env={"WRITE_TIMEOUT_MS": 300, "SEND_TIMEOUT_MS": 1000},
    )
    client = socket.create_connection(("127.0.0.1", port), timeout=2)
    client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    time.sleep(0.8)
    client.close()
    time.sleep(0.2)
    stop_server(server)

    _path, status, bytes_sent, outcome, _client_ip = latest_for_path(db_path, "/")
    if status != 200 or outcome != "timeout":
        raise AssertionError(
            f"timeout transfer: wrong row {status, bytes_sent, outcome}"
        )
    if not 0 < bytes_sent < BIG_BODY_SIZE:
        raise AssertionError(f"timeout transfer: bytes_sent={bytes_sent}")


def test_disconnect_outcome(binary: Path, root: Path) -> None:
    static_dir = make_static(root, large_index=True)
    db_path = root / "disconnect.db"
    server, port = start_server(binary, root, static_dir, db_path)
    client = socket.create_connection(("127.0.0.1", port), timeout=2)
    client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    time.sleep(0.1)
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
    client.close()
    time.sleep(0.3)
    stop_server(server)

    _path, status, bytes_sent, outcome, _client_ip = latest_for_path(db_path, "/")
    if status != 200 or outcome != "disconnected":
        raise AssertionError(
            f"disconnect transfer: wrong row {status, bytes_sent, outcome}"
        )


def test_shutdown_cancelled_outcome(binary: Path, root: Path) -> None:
    static_dir = make_static(root, large_index=True)
    db_path = root / "shutdown.db"
    server, port = start_server(
        binary,
        root,
        static_dir,
        db_path,
        extra_env={"WRITE_TIMEOUT_MS": 10000, "SHUTDOWN_GRACE_MS": 400},
    )
    client = socket.create_connection(("127.0.0.1", port), timeout=2)
    client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    time.sleep(0.1)
    server.send_signal(signal.SIGTERM)
    server.wait(timeout=5)
    client.close()
    if server.returncode != 0:
        raise AssertionError(f"shutdown transfer: exit {server.returncode}")

    _path, status, _bytes, outcome, _client_ip = latest_for_path(db_path, "/")
    if status != 200 or outcome != "shutdown_cancelled":
        raise AssertionError(
            f"shutdown transfer: wrong row {status, _bytes, outcome}"
        )


def test_old_database_migration(binary: Path, root: Path) -> None:
    static_dir = make_static(root, large_index=False)
    db_path = root / "legacy.db"
    with sqlite3.connect(db_path) as connection:
        connection.executescript(
            """
            CREATE TABLE access_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL DEFAULT (datetime('now')),
                client_ip TEXT NOT NULL,
                method TEXT NOT NULL,
                path TEXT NOT NULL,
                status_code INTEGER NOT NULL,
                bytes_sent INTEGER NOT NULL
            );
            INSERT INTO access_logs
                (client_ip, method, path, status_code, bytes_sent)
                VALUES ('127.0.0.1', 'GET', '/legacy', 200, 123);
            """
        )
        connection.commit()

    server, port = start_server(binary, root, static_dir, db_path)
    try:
        if request(port, "/about") != 200:
            raise AssertionError("legacy migration: request failed")
        time.sleep(0.2)
    finally:
        stop_server(server)

    with sqlite3.connect(db_path) as connection:
        version = connection.execute("PRAGMA user_version").fetchone()[0]
        columns = {
            row[1] for row in connection.execute("PRAGMA table_info(access_logs)")
        }
        legacy = connection.execute(
            "SELECT status_code, bytes_sent, transfer_outcome "
            "FROM access_logs WHERE path='/legacy'"
        ).fetchone()
        modern = connection.execute(
            "SELECT status_code, transfer_outcome FROM access_logs "
            "WHERE path='/about' ORDER BY id DESC LIMIT 1"
        ).fetchone()

    if version != 1 or "transfer_outcome" not in columns:
        raise AssertionError(f"legacy migration: schema version/columns wrong")
    if legacy != (200, 123, "unknown"):
        raise AssertionError(f"legacy migration: historical row changed {legacy}")
    if modern != (200, "complete"):
        raise AssertionError(f"legacy migration: new row wrong {modern}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="http-server-transfer-tests-") as temp:
        root = Path(temp)
        tests = [
            (test_complete_transfer, root / "complete"),
            (test_timeout_outcome, root / "timeout"),
            (test_disconnect_outcome, root / "disconnect"),
            (test_shutdown_cancelled_outcome, root / "shutdown"),
            (test_old_database_migration, root / "legacy"),
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

    print("all transfer logging tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
