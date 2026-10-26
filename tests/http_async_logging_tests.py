#!/usr/bin/env python3
"""Asynchronous logging behavior and accounting tests."""

from __future__ import annotations

import argparse
import concurrent.futures
import http.client
import os
import re
import signal
import socket
import sqlite3
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


def request(port: int, path: str = "/about") -> int:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    connection.request("GET", path, headers={"Host": "localhost"})
    response = connection.getresponse()
    response.read()
    status = response.status
    connection.close()
    return status


def make_static(root: Path) -> Path:
    static_dir = root / "static"
    static_dir.mkdir()
    (static_dir / "index.html").write_bytes(b"INDEX")
    (static_dir / "about.html").write_bytes(b"ABOUT")
    (static_dir / "404.html").write_bytes(b"NOT FOUND")
    return static_dir


def start_server(
    binary: Path,
    root: Path,
    db_path: Path,
    extra_env: dict | None = None,
) -> tuple[subprocess.Popen, int, Path]:
    static_dir = make_static(root)
    port = free_port()
    log_path = root / "server.log"
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true",
            "SHUTDOWN_GRACE_MS": "500",
            "LOG_QUEUE_CAPACITY": "64",
            "LOG_BATCH_SIZE": "8",
            "LOG_FLUSH_INTERVAL_MS": "20",
            "LOG_MAX_RETRIES": "1",
            "LOG_RETRY_BACKOFF_MS": "10",
            "LOG_SHUTDOWN_DRAIN_MS": "1000",
        }
    )
    if extra_env:
        env.update({key: str(value) for key, value in extra_env.items()})
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
        server.send_signal(signal.SIGTERM)
    server.wait(timeout=10)
    if server.returncode != 0:
        raise AssertionError(f"server exited with status {server.returncode}")


def parse_metrics(log_path: Path) -> dict[str, float]:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"\[INFO\] Logger stopped (.*)", text)
    if not match:
        raise AssertionError(f"logger metrics line missing:\n{text}")
    metrics = {}
    for key, value in re.findall(r"(\w+)=([\d.]+)", match.group(1)):
        metrics[key] = float(value)
    return metrics


def acquire_write_lock(db_path: Path) -> sqlite3.Connection:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        connection = sqlite3.connect(db_path, timeout=0.2)
        try:
            connection.execute("BEGIN IMMEDIATE")
            return connection
        except sqlite3.OperationalError:
            connection.close()
            time.sleep(0.02)
    raise AssertionError("could not acquire external SQLite write lock")


def count_rows(db_path: Path) -> int:
    with sqlite3.connect(db_path) as connection:
        return connection.execute("SELECT COUNT(*) FROM access_logs").fetchone()[0]


def assert_accounting(metrics: dict[str, float]) -> None:
    if int(metrics["queued"]) != 0 or int(metrics["in_flight"]) != 0:
        raise AssertionError(f"logger stopped with pending records: {metrics}")
    if int(metrics["accepted"]) != (
        int(metrics["committed"])
        + int(metrics["failed"])
        + int(metrics["discarded"])
    ):
        raise AssertionError(f"accepted-accounting mismatch: {metrics}")
    if int(metrics["offered"]) != int(metrics["accepted"]) + int(metrics["dropped"]):
        raise AssertionError(f"offered-accounting mismatch: {metrics}")


def test_timed_flush_and_batches(binary: Path, root: Path) -> None:
    db_path = root / "normal.db"
    server, port, log_path = start_server(
        binary,
        root,
        db_path,
        extra_env={
            "LOG_QUEUE_CAPACITY": "64",
            "LOG_BATCH_SIZE": "8",
            "LOG_FLUSH_INTERVAL_MS": "50",
        },
    )
    try:
        if request(port) != 200:
            raise AssertionError("normal batch: request failed")
        time.sleep(0.4)
        if count_rows(db_path) < 2:
            raise AssertionError("low-traffic timed flush did not commit promptly")

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            statuses = list(executor.map(lambda _: request(port), range(32)))
        if any(status != 200 for status in statuses):
            raise AssertionError("normal batch: concurrent request failed")
        time.sleep(0.5)
    finally:
        stop_server(server)

    metrics = parse_metrics(log_path)
    assert_accounting(metrics)
    if metrics["dropped"] != 0 or metrics["failed"] != 0:
        raise AssertionError(f"normal batch unexpectedly lost records: {metrics}")
    if int(metrics["committed"]) != count_rows(db_path):
        raise AssertionError("normal batch: committed count does not match database")
    if int(metrics["batches"]) < 1:
        raise AssertionError("normal batch: no batches recorded")


def test_notifications_do_not_flush_early(binary: Path, root: Path) -> None:
    db_path = root / "timing.db"
    server, port, log_path = start_server(
        binary,
        root,
        db_path,
        extra_env={
            "LOG_QUEUE_CAPACITY": "64",
            "LOG_BATCH_SIZE": "100",
            "LOG_FLUSH_INTERVAL_MS": "1000",
            "LOG_MAX_RETRIES": "0",
        },
    )
    try:
        if request(port) != 200:
            raise AssertionError("timing: first request failed")
        time.sleep(0.15)
        if request(port) != 200:
            raise AssertionError("timing: second request failed")
        time.sleep(0.30)
        if count_rows(db_path) != 0:
            raise AssertionError(
                "timing: records flushed before the absolute batch deadline"
            )
        time.sleep(1.0)
        if count_rows(db_path) < 3:
            raise AssertionError("timing: records were not flushed at the deadline")
    finally:
        stop_server(server)

    metrics = parse_metrics(log_path)
    assert_accounting(metrics)
    if int(metrics["committed"]) != count_rows(db_path):
        raise AssertionError("timing: committed count does not match database")


def test_size_triggered_flush(binary: Path, root: Path) -> None:
    db_path = root / "size-trigger.db"
    server, port, log_path = start_server(
        binary,
        root,
        db_path,
        extra_env={
            "LOG_QUEUE_CAPACITY": "64",
            "LOG_BATCH_SIZE": "2",
            "LOG_FLUSH_INTERVAL_MS": "1000",
            "LOG_MAX_RETRIES": "0",
        },
    )
    try:
        if request(port) != 200 or request(port) != 200:
            raise AssertionError("size trigger: request failed")
        time.sleep(0.3)
        if count_rows(db_path) < 2:
            raise AssertionError("size trigger: full batch was not flushed")
    finally:
        stop_server(server)

    metrics = parse_metrics(log_path)
    assert_accounting(metrics)
    if int(metrics["committed"]) != count_rows(db_path):
        raise AssertionError("size trigger: committed count does not match database")


def test_queue_saturation_and_retries(binary: Path, root: Path) -> None:
    db_path = root / "saturation.db"
    server, port, log_path = start_server(
        binary,
        root,
        db_path,
        extra_env={
            "LOG_QUEUE_CAPACITY": "4",
            "LOG_BATCH_SIZE": "2",
            "LOG_FLUSH_INTERVAL_MS": "5",
            "LOG_MAX_RETRIES": "1",
            "LOG_RETRY_BACKOFF_MS": "20",
        },
    )
    try:
        lock = acquire_write_lock(db_path)
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
                statuses = list(executor.map(lambda _: request(port), range(32)))
            if any(status != 200 for status in statuses):
                raise AssertionError("saturation: HTTP request failed")
            time.sleep(0.5)
        finally:
            lock.rollback()
            lock.close()
        time.sleep(0.4)
    finally:
        stop_server(server)

    metrics = parse_metrics(log_path)
    assert_accounting(metrics)
    if metrics["dropped"] <= 0:
        raise AssertionError(f"queue saturation did not drop records: {metrics}")
    if metrics["failed"] <= 0:
        raise AssertionError(f"database lock did not fail a batch: {metrics}")


def test_shutdown_with_pending_records(binary: Path, root: Path) -> None:
    db_path = root / "shutdown.db"
    server, port, log_path = start_server(
        binary,
        root,
        db_path,
        extra_env={
            "LOG_QUEUE_CAPACITY": "4",
            "LOG_BATCH_SIZE": "2",
            "LOG_FLUSH_INTERVAL_MS": "5",
            "LOG_MAX_RETRIES": "0",
            "LOG_RETRY_BACKOFF_MS": "0",
            "LOG_SHUTDOWN_DRAIN_MS": "100",
        },
    )
    lock = acquire_write_lock(db_path)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            list(executor.map(lambda _: request(port), range(24)))
        time.sleep(0.1)
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=5)
        if server.returncode != 0:
            raise AssertionError(f"shutdown: exit {server.returncode}")
    finally:
        lock.rollback()
        lock.close()

    metrics = parse_metrics(log_path)
    assert_accounting(metrics)
    if metrics["discarded"] + metrics["failed"] <= 0:
        raise AssertionError(f"shutdown did not account pending records: {metrics}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.is_file():
        print(f"server binary not found: {binary}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="http-server-async-log-tests-") as temp:
        root = Path(temp)
        tests = [
            (test_timed_flush_and_batches, root / "normal"),
            (test_notifications_do_not_flush_early, root / "timing"),
            (test_size_triggered_flush, root / "size-trigger"),
            (test_queue_saturation_and_retries, root / "saturation"),
            (test_shutdown_with_pending_records, root / "shutdown"),
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

    print("all async logging tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
