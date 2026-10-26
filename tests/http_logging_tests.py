#!/usr/bin/env python3
"""Startup and logging-initialization integration tests."""

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

from http_test_util import HttpResponseReader

ROOT = Path(__file__).resolve().parents[1]
STATIC_DIR = ROOT / "static"


def binary_path(override: str) -> Path:
    return Path(override).resolve()


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
    db_path: Path,
    logging: bool,
    cwd: Path,
    log_path: Path,
    extra_env: dict | None = None,
) -> tuple[subprocess.Popen, int]:
    port = free_port()
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(STATIC_DIR),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true" if logging else "false",
            "SHUTDOWN_GRACE_MS": "500",
        }
    )
    if extra_env:
        env.update({key: str(value) for key, value in extra_env.items()})
    log = log_path.open("wb")
    server = subprocess.Popen(
        [str(binary)],
        cwd=cwd,
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
    try:
        server.wait(timeout=10)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=5)
        raise AssertionError("server did not stop")
    if server.returncode != 0:
        raise AssertionError(f"server exited with status {server.returncode}")


def request(port: int, path: str = "/") -> int:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request("GET", path, headers={"Host": "localhost"})
    response = connection.getresponse()
    response.read()
    status = response.status
    connection.close()
    return status


def sqlite_files(db_path: Path) -> list[Path]:
    return [
        path
        for path in (
            db_path,
            Path(str(db_path) + "-wal"),
            Path(str(db_path) + "-shm"),
        )
        if path.exists()
    ]


def count_rows(db_path: Path) -> int:
    with sqlite3.connect(db_path) as connection:
        return connection.execute("SELECT COUNT(*) FROM access_logs").fetchone()[0]


def test_disabled_with_unusable_database(binary: Path, root: Path) -> None:
    db_path = root / "missing-parent" / "logs.db"
    log_path = root / "disabled-unusable.log"
    server, port = start_server(
        binary, db_path, logging=False, cwd=root, log_path=log_path
    )
    try:
        if request(port) != 200:
            raise AssertionError("disabled logging: request failed")
        if db_path.parent.exists() or db_path.exists():
            raise AssertionError("disabled logging created database path")
    finally:
        stop_server(server)


def test_disabled_creates_no_sqlite_files(binary: Path, root: Path) -> None:
    db_dir = root / "disabled-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"
    log_path = root / "disabled-valid.log"
    server, port = start_server(
        binary, db_path, logging=False, cwd=root, log_path=log_path
    )
    try:
        if request(port) != 200:
            raise AssertionError("disabled logging: request failed")
        files = sqlite_files(db_path)
        if files:
            raise AssertionError(f"disabled logging created SQLite files: {files}")
    finally:
        stop_server(server)


def test_different_working_directory(binary: Path, root: Path) -> None:
    cwd = root / "different-cwd"
    cwd.mkdir()
    db_dir = root / "cwd-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"
    log_path = root / "different-cwd.log"
    server, port = start_server(
        binary, db_path, logging=True, cwd=cwd, log_path=log_path
    )
    try:
        if request(port) != 200:
            raise AssertionError("different cwd: request failed")
    finally:
        stop_server(server)

    if not db_path.is_file():
        raise AssertionError("different cwd: database was not created")
    if count_rows(db_path) < 1:
        raise AssertionError("different cwd: no log records were written")


def test_existing_database_preserved(binary: Path, root: Path) -> None:
    db_dir = root / "existing-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"

    log_one = root / "existing-one.log"
    server, port = start_server(
        binary, db_path, logging=True, cwd=root, log_path=log_one
    )
    try:
        if request(port, "/about") != 200:
            raise AssertionError("existing db first run: request failed")
    finally:
        stop_server(server)
    first_count = count_rows(db_path)

    log_two = root / "existing-two.log"
    server, port = start_server(
        binary, db_path, logging=True, cwd=root, log_path=log_two
    )
    try:
        if request(port, "/about") != 200:
            raise AssertionError("existing db second run: request failed")
    finally:
        stop_server(server)
    second_count = count_rows(db_path)

    if first_count < 1:
        raise AssertionError("existing db: first run wrote no records")
    if second_count < first_count + 1:
        raise AssertionError(
            f"existing db: records not preserved ({first_count} -> {second_count})"
        )


def test_persistent_requests_are_logged_per_request(binary: Path, root: Path) -> None:
    db_dir = root / "persistent-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"
    log_path = root / "persistent.log"
    server, port = start_server(
        binary, db_path, logging=True, cwd=root, log_path=log_path
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(
                b"GET /about HTTP/1.1\r\nHost: localhost\r\n\r\n"
                b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
            )
            for _ in range(2):
                status, _headers, _body = reader.read_response()
                if not status.startswith("HTTP/1.1 200"):
                    raise AssertionError(f"persistent logging: status {status!r}")
        time.sleep(0.2)
    finally:
        stop_server(server)

    with sqlite3.connect(db_path) as connection:
        rows = connection.execute(
            "SELECT path, status_code, transfer_outcome FROM access_logs"
        ).fetchall()
    about_rows = [row for row in rows if row[0] == "/about"]
    index_rows = [row for row in rows if row[0] == "/"]
    if len(about_rows) != 1:
        raise AssertionError(f"persistent logging: /about rows {about_rows!r}")
    if len(index_rows) < 2:
        raise AssertionError(f"persistent logging: / rows {index_rows!r}")
    if any(status == 0 for _path, status, _outcome in rows):
        raise AssertionError(f"persistent logging: invented status 0 rows {rows!r}")


def test_idle_persistent_close_does_not_log_failure(binary: Path, root: Path) -> None:
    db_dir = root / "idle-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"
    log_path = root / "idle.log"
    server, port = start_server(
        binary, db_path, logging=True, cwd=root, log_path=log_path
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            sock.sendall(b"GET /about HTTP/1.1\r\nHost: localhost\r\n\r\n")
            status, _headers, _body = reader.read_response()
            if not status.startswith("HTTP/1.1 200"):
                raise AssertionError(f"idle logging: status {status!r}")
        time.sleep(0.2)
    finally:
        stop_server(server)

    with sqlite3.connect(db_path) as connection:
        rows = connection.execute(
            "SELECT path, status_code, transfer_outcome FROM access_logs"
        ).fetchall()
    if any(status == 0 for _path, status, _outcome in rows):
        raise AssertionError(f"idle close created a failed record: {rows!r}")


def test_header_timeout_logs_timeout_outcome(binary: Path, root: Path) -> None:
    db_dir = root / "timeout-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"
    log_path = root / "timeout.log"
    server, port = start_server(
        binary,
        db_path,
        logging=True,
        cwd=root,
        log_path=log_path,
        extra_env={"HEADER_TIMEOUT_MS": 200, "KEEPALIVE_TIMEOUT_MS": 2000},
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
            reader = HttpResponseReader(sock)
            status, _headers, _body = reader.read_response()
            if not status.startswith("HTTP/1.1 408"):
                raise AssertionError(f"timeout logging: status {status!r}")
        time.sleep(0.2)
    finally:
        stop_server(server)

    with sqlite3.connect(db_path) as connection:
        rows = connection.execute(
            "SELECT status_code, transfer_outcome FROM access_logs "
            "WHERE status_code=408"
        ).fetchall()
    if rows != [(408, "timeout")]:
        raise AssertionError(f"timeout logging: rows {rows!r}")


def test_unusable_database_fails(binary: Path, root: Path) -> None:
    db_path = root / "absent-parent" / "logs.db"
    log_path = root / "enabled-unusable.log"
    port = free_port()
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(STATIC_DIR),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true",
        }
    )
    result = subprocess.run(
        [str(binary)],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode == 0:
        raise AssertionError("unusable database unexpectedly started")
    if "Initialization failed" not in result.stderr:
        raise AssertionError(
            f"unusable database: missing diagnostic: {result.stderr!r}"
        )
    if "Failed to open SQLite DB" not in result.stderr:
        raise AssertionError(
            f"unusable database: missing database distinction: {result.stderr!r}"
        )
    if db_path.exists():
        raise AssertionError("unusable database path was created")


def test_schema_failure_is_clean(binary: Path, root: Path) -> None:
    db_dir = root / "bad-schema-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"
    with sqlite3.connect(db_path) as connection:
        connection.execute("CREATE TABLE access_logs (id INTEGER PRIMARY KEY)")
        connection.execute("INSERT INTO access_logs (id) VALUES (1)")
        connection.commit()

    env = os.environ.copy()
    env.update(
        {
            "PORT": str(free_port()),
            "STATIC_DIR": str(STATIC_DIR),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true",
        }
    )
    result = subprocess.run(
        [str(binary)],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode == 0:
        raise AssertionError("bad schema unexpectedly started")
    if "Failed to apply schema" not in result.stderr:
        raise AssertionError(
            f"bad schema: missing diagnostic: {result.stderr!r}"
        )

    with sqlite3.connect(db_path) as connection:
        integrity = connection.execute("PRAGMA integrity_check").fetchone()[0]
        columns = connection.execute("PRAGMA table_info(access_logs)").fetchall()
        version = connection.execute("PRAGMA user_version").fetchone()[0]
        rows = connection.execute("SELECT id FROM access_logs").fetchall()
    if integrity != "ok":
        raise AssertionError(f"bad schema: database was damaged: {integrity!r}")
    if len(columns) != 1:
        raise AssertionError("bad schema: pre-existing database was replaced")
    if version != 0:
        raise AssertionError(f"bad schema: version changed to {version}")
    if rows != [(1,)]:
        raise AssertionError(f"bad schema: row was not preserved: {rows}")


def test_future_schema_version_rejected(binary: Path, root: Path) -> None:
    db_dir = root / "future-schema-db"
    db_dir.mkdir()
    db_path = db_dir / "logs.db"
    with sqlite3.connect(db_path) as connection:
        connection.execute(
            "CREATE TABLE access_logs (id INTEGER PRIMARY KEY, sentinel TEXT)"
        )
        connection.execute(
            "INSERT INTO access_logs (id, sentinel) VALUES (1, 'preserve-me')"
        )
        connection.execute("PRAGMA user_version = 99")
        connection.commit()

    env = os.environ.copy()
    env.update(
        {
            "PORT": str(free_port()),
            "STATIC_DIR": str(STATIC_DIR),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true",
        }
    )
    result = subprocess.run(
        [str(binary)],
        cwd=root,
        env=env,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode == 0:
        raise AssertionError("future schema unexpectedly started")
    if "Unsupported access_logs schema version 99" not in result.stderr:
        raise AssertionError(
            f"future schema: missing diagnostic: {result.stderr!r}"
        )

    with sqlite3.connect(db_path) as connection:
        version = connection.execute("PRAGMA user_version").fetchone()[0]
        columns = connection.execute("PRAGMA table_info(access_logs)").fetchall()
        rows = connection.execute(
            "SELECT id, sentinel FROM access_logs"
        ).fetchall()
    if version != 99:
        raise AssertionError(f"future schema: version changed to {version}")
    if [column[1] for column in columns] != ["id", "sentinel"]:
        raise AssertionError("future schema: schema was modified")
    if rows != [(1, "preserve-me")]:
        raise AssertionError(f"future schema: rows changed: {rows}")


def test_socket_setup_failure(binary: Path, root: Path) -> None:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(STATIC_DIR),
            "DB_PATH": str(root / "unused.db"),
            "ENABLE_LOGGING": "false",
        }
    )
    try:
        result = subprocess.run(
            [str(binary)],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            timeout=10,
        )
    finally:
        listener.close()

    if result.returncode != 1:
        raise AssertionError(
            f"socket setup failure: expected exit 1, got {result.returncode}"
        )
    if "Socket setup failed" not in result.stderr:
        raise AssertionError(
            f"socket setup failure: missing diagnostic: {result.stderr!r}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()

    binary = binary_path(args.binary)
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="http-server-logging-tests-") as temp:
        root = Path(temp)
        tests = [
            lambda: test_disabled_with_unusable_database(binary, root),
            lambda: test_disabled_creates_no_sqlite_files(binary, root),
            lambda: test_different_working_directory(binary, root),
            lambda: test_existing_database_preserved(binary, root),
            lambda: test_persistent_requests_are_logged_per_request(binary, root),
            lambda: test_idle_persistent_close_does_not_log_failure(binary, root),
            lambda: test_header_timeout_logs_timeout_outcome(binary, root),
            lambda: test_unusable_database_fails(binary, root),
            lambda: test_schema_failure_is_clean(binary, root),
            lambda: test_future_schema_version_rejected(binary, root),
            lambda: test_socket_setup_failure(binary, root),
        ]
        for test in tests:
            try:
                test()
            except Exception as error:
                print(f"FAIL: {error}", file=sys.stderr)
                for log in root.glob("*.log"):
                    print(f"--- {log} ---", file=sys.stderr)
                    print(
                        log.read_text(encoding="utf-8", errors="replace"),
                        file=sys.stderr,
                    )
                return 1

    print("all logging initialization tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
