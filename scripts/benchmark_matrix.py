#!/usr/bin/env python3
"""Loopback benchmark matrix for close/keep-alive and logging on/off.

This is intentionally a benchmark harness, not a server optimization path.
Each scenario starts a fresh Release server, warms up with the same connection
policy, runs a small bounded concurrency sweep, then repeats the primary run.
Raw ApacheBench output, server logs, JSON snapshots, CPU, and RSS samples are
retained. Primary results are localhost-only.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import re
import shutil
import signal
import socket
import sqlite3
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from benchmark import (
    ROOT,
    build_metadata,
    command_output,
    environment_metadata,
    parse_ab,
    sha256_bytes,
    validate_probe,
    wait_for_child,
)

DEFAULT_OUTPUT = ROOT / "data" / "benchmarks"


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def max_rss_kb(usage: os.times_result | None) -> int | None:
    if usage is None:
        return None
    # macOS reports ru_maxrss in bytes; Linux reports KiB.
    if sys.platform == "darwin":
        return int(usage.ru_maxrss / 1024)
    return int(usage.ru_maxrss)


def aggregate_median(values: list[float]) -> float:
    return float(statistics.median(values)) if values else 0.0


def ab_successful_requests(complete: int, failed: int) -> int:
    # ab's completed count includes responses with length/content failures;
    # socket-level errors are not counted here.
    return max(complete - failed, 0)


def db_rows(db_path: Path) -> int | None:
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        try:
            with sqlite3.connect(db_path, timeout=0.5) as connection:
                return connection.execute(
                    "SELECT COUNT(*) FROM access_logs"
                ).fetchone()[0]
        except sqlite3.Error:
            time.sleep(0.05)
    return None


def start_server(
    binary: Path, static_dir: Path, db_path: Path, log_path: Path,
    logging_enabled: bool, port: int, extra_env: dict | None = None,
) -> subprocess.Popen:
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(port),
            "STATIC_DIR": str(static_dir),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true" if logging_enabled else "false",
            "MAX_CONNECTIONS": "1024",
            "MAX_REQUESTS_PER_CONNECTION": "1000000",
            "MAX_PIPELINED_REQUESTS": "8",
            "MAX_CONNECTION_INPUT_BYTES": str(128 * 1024),
            "KEEPALIVE_TIMEOUT_MS": "120000",
            "HEADER_TIMEOUT_MS": "30000",
            "WRITE_TIMEOUT_MS": "30000",
            "SHUTDOWN_GRACE_MS": "3000",
        }
    )
    if extra_env:
        env.update({key: str(value) for key, value in extra_env.items()})
    log = log_path.open("wb")
    proc = subprocess.Popen(
        [str(binary)],
        cwd=ROOT,
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    log.close()
    return proc


def wait_ready(proc: subprocess.Popen, port: int, path: str,
               expected_body: bytes, log_path: Path) -> None:
    deadline = time.monotonic() + 10
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"server exited before readiness: {proc.returncode}\n"
                f"{log_path.read_text(encoding='utf-8', errors='replace')}"
            )
        try:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            connection.request(
                "GET", path,
                headers={"Host": f"127.0.0.1:{port}", "Connection": "close"},
            )
            response = connection.getresponse()
            body = response.read()
            connection.close()
            if response.status == 200 and body == expected_body:
                return
            last_error = RuntimeError(f"readiness status={response.status}")
        except OSError as error:
            last_error = error
        time.sleep(0.05)
    raise RuntimeError(f"server not ready: {last_error}")


def parse_ab_extras(text: str) -> dict:
    keep_alive = re.search(r"^Keep-Alive requests:\s+(\d+)", text, re.M)
    socket_line = re.search(r"^Socket errors:.*$", text, re.M)
    socket_errors: dict[str, int] = {}
    if socket_line:
        for name, value in re.findall(r"(connect|read|write|timeout)\s+(\d+)",
                                      socket_line.group(0)):
            socket_errors[name] = int(value)
    return {
        "keepalive_requests": int(keep_alive.group(1)) if keep_alive else 0,
        "socket_errors": socket_errors,
    }


def run_ab(
    port: int, path: str, connections: int, requests: int | None,
    duration: int | None, keepalive: bool, timeout: int, raw_path: Path,
) -> dict:
    command = ["ab"]
    if keepalive:
        command.append("-k")
    if duration is not None:
        # ab's -n default is low; make -t the effective stop condition.
        command += ["-n", str(10_000_000), "-t", str(duration)]
    else:
        command += ["-n", str(requests)]
    command += ["-c", str(connections), "-s", str(timeout),
                f"http://127.0.0.1:{port}{path}"]

    started = time.monotonic()
    with raw_path.open("wb") as raw:
        proc = subprocess.Popen(
            command,
            cwd=ROOT,
            stdout=raw,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            budget = (duration + 30) if duration else max(
                120, (requests or 0) // 500 + timeout + 30
            )
            status, usage = wait_for_child(proc, budget)
        finally:
            if proc.poll() is None:
                proc.kill()
                wait_for_child(proc, 5)
    wall = time.monotonic() - started
    text = raw_path.read_text(encoding="utf-8", errors="replace")
    if status is None:
        raise RuntimeError(f"ab exceeded budget; see {raw_path}")
    parsed = parse_ab(text)
    parsed.update(parse_ab_extras(text))
    complete = parsed["complete_requests"]
    failed = parsed["failed_requests"]
    successful = ab_successful_requests(complete, failed)
    test_seconds = parsed.get("test_seconds")
    successful_rps = (
        successful / test_seconds
        if test_seconds and test_seconds > 0
        else ((parsed["requests_per_second"] or 0.0) * (successful / complete)
              if complete else 0.0)
    )
    client_cpu = round(usage.ru_utime + usage.ru_stime, 3) if usage else None
    return {
        "command": command,
        "exit_code": proc.returncode,
        "wall_seconds": round(wall, 3),
        "client_cpu_seconds": client_cpu,
        "client_max_rss_kb": max_rss_kb(usage),
        "client_cpu_percent": round(100.0 * client_cpu / wall, 1)
        if client_cpu is not None and wall > 0 else None,
        "client_cpu_near_limit": bool(
            client_cpu is not None and wall > 0 and 100.0 * client_cpu / wall >= 90
        ),
        "successful_requests": successful,
        "test_seconds": test_seconds,
        "successful_rps": round(successful_rps, 2),
        "parsed": parsed,
        "raw_output_path": str(raw_path),
    }


def parse_logger_metrics(log_text: str) -> dict:
    match = re.search(r"\[INFO\] Logger stopped (.*)", log_text)
    if not match:
        return {}
    result = {}
    for key, value in re.findall(r"(\w+)=([\d.]+)", match.group(1)):
        result[key] = float(value)
    return result


def run_scenario(
    args: argparse.Namespace, keepalive: bool, logging_enabled: bool,
    output_dir: Path, expected_body: bytes, static_dir: Path,
) -> dict:
    label = f"{'keepalive' if keepalive else 'close'}-logging-{'on' if logging_enabled else 'off'}"
    scenario_dir = output_dir / label
    scenario_dir.mkdir(parents=True, exist_ok=True)
    port = free_port()
    db_path = scenario_dir / "access.db"
    log_path = scenario_dir / "server.log"
    for suffix in ("", "-wal", "-shm"):
        try:
            Path(str(db_path) + suffix).unlink()
        except FileNotFoundError:
            pass

    proc = start_server(
        Path(args.binary), static_dir, db_path, log_path, logging_enabled, port
    )
    result = {
        "mode": label,
        "keepalive": keepalive,
        "logging_enabled": logging_enabled,
        "port": port,
        "runs": [],
        "sweep": [],
    }
    started = time.monotonic()
    try:
        wait_ready(proc, port, args.path, expected_body, log_path)
        if args.warmup_requests > 0:
            warm = run_ab(
                port, args.path, args.connections, args.warmup_requests, None,
                keepalive, args.timeout, scenario_dir / "warmup.txt",
            )
            result["warmup"] = {k: v for k, v in warm.items()
                               if k not in {"parsed"}}
            result["warmup"]["parsed"] = warm["parsed"]
            result["db_rows_after_warmup"] = (
                db_rows(db_path) if logging_enabled else None
            )

        for connections in args.sweep_connections:
            raw = scenario_dir / f"sweep-c{connections}.txt"
            run = run_ab(
                port, args.path, connections, args.sweep_requests, None,
                keepalive, args.timeout, raw,
            )
            run["connections"] = connections
            run["role"] = "sweep"
            result["sweep"].append(run)
            if logging_enabled:
                run["db_rows_after_run"] = db_rows(db_path)

        for index in range(1, args.runs + 1):
            raw = scenario_dir / f"run-{index}-c{args.connections}.txt"
            run = run_ab(
                port, args.path, args.connections, args.requests, None,
                keepalive, args.timeout, raw,
            )
            parsed = run["parsed"]
            complete = parsed["complete_requests"]
            failed = parsed["failed_requests"]
            run.update(
                {
                    "run": index,
                    "role": "primary",
                    "connections": args.connections,
                    "requests_requested": args.requests,
                    "completed_all_requests": complete == args.requests,
                    "failed_requests": failed,
                    "non_2xx_responses": parsed["non_2xx_responses"],
                    "correctness": (
                        complete == args.requests
                        and failed == 0
                        and parsed["non_2xx_responses"] == 0
                        and (not keepalive or parsed["keepalive_requests"] > 0)
                    ),
                }
            )
            if logging_enabled:
                run["db_rows_after_run"] = db_rows(db_path)
            result["runs"].append(run)
            if run["exit_code"] != 0 or not run["completed_all_requests"]:
                raise RuntimeError(
                    f"{label} run {index} incomplete: exit={run['exit_code']} "
                    f"complete={complete}; see {raw}"
                )
            if failed or parsed["non_2xx_responses"]:
                raise RuntimeError(
                    f"{label} run {index} failed={failed} "
                    f"non2xx={parsed['non_2xx_responses']}; see {raw}"
                )

        probe_conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        probe_conn.request(
            "GET", args.path,
            headers={"Host": f"127.0.0.1:{port}", "Connection": "close"},
        )
        probe = probe_conn.getresponse()
        probe_body = probe.read()
        probe_status = probe.status
        probe_length = probe.getheader("Content-Length")
        probe_conn.close()
        result["final_probe"] = {
            "status": probe_status,
            "body_matches": probe_body == expected_body,
            "content_length": probe_length,
        }
        if probe_status != 200 or probe_body != expected_body:
            raise RuntimeError("final response probe failed")
        if logging_enabled:
            result["db_rows_after_probe"] = db_rows(db_path)
    finally:
        usage = None
        needed_kill = False
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            status, usage = wait_for_child(proc, 10)
            if status is None:
                proc.kill()
                wait_for_child(proc, 5)
                needed_kill = True
        exit_code = proc.returncode
        result["server_exit_code"] = exit_code
        result["server_needed_sigkill"] = needed_kill
        result["wall_seconds"] = round(time.monotonic() - started, 3)

    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    result["server_cpu_seconds"] = (
        round(usage.ru_utime + usage.ru_stime, 3) if usage else None
    )
    result["server_max_rss_kb"] = max_rss_kb(usage)
    result["server_cpu_percent"] = (
        round(100.0 * result["server_cpu_seconds"] / result["wall_seconds"], 1)
        if result["server_cpu_seconds"] and result["wall_seconds"] else None
    )
    result["logger_metrics"] = parse_logger_metrics(log_text)
    result["server_log_sqlite_busy_errors"] = log_text.count("database is locked")
    if logging_enabled:
        result["db_rows_final"] = db_rows(db_path)
        rows_after_probe = result.get("db_rows_after_probe")
        if result.get("db_rows_final") is not None and rows_after_probe is not None:
            result["drained_after_load"] = (
                result["db_rows_final"] - rows_after_probe
            )
        if result.get("db_rows_after_warmup") is not None and result["runs"]:
            last = result["runs"][-1].get("db_rows_after_run")
            if last is not None:
                result["measured_db_rows_delta"] = (
                    last - result["db_rows_after_warmup"]
                )
        metrics = result["logger_metrics"]
        result["logging_kept_up"] = bool(
            metrics
            and metrics.get("dropped", 1) == 0
            and metrics.get("failed", 0) == 0
            and metrics.get("discarded", 0) == 0
            and metrics.get("accepted", -1) == metrics.get("committed", -2)
        )
    else:
        result["logging_kept_up"] = None
    result["server_log"] = str(log_path)
    result["database_path"] = str(db_path) if logging_enabled else None

    rps = [run["successful_rps"] for run in result["runs"]]
    result["summary"] = {
        "primary_runs": len(rps),
        "successful_rps_min": min(rps) if rps else 0.0,
        "successful_rps_median": aggregate_median(rps),
        "successful_rps_max": max(rps) if rps else 0.0,
        "p50_ms_median": aggregate_median(
            [run["parsed"]["latency_percentiles_ms"].get(50, 0.0)
             for run in result["runs"]]
        ),
        "p99_ms_median": aggregate_median(
            [run["parsed"]["latency_percentiles_ms"].get(99, 0.0)
             for run in result["runs"]]
        ),
        "failed_requests_total": sum(
            run["failed_requests"] for run in result["runs"]
        ),
        "non_2xx_total": sum(
            run["non_2xx_responses"] for run in result["runs"]
        ),
        "keepalive_requests_total": sum(
            run["parsed"]["keepalive_requests"] for run in result["runs"]
        ),
        "all_primary_correct": all(
            run["correctness"] for run in result["runs"]
        ),
        "client_cpu_near_limit": any(
            run["client_cpu_near_limit"] for run in result["runs"]
        ),
    }
    if not args.keep_db:
        for suffix in ("", "-wal", "-shm"):
            try:
                Path(str(db_path) + suffix).unlink()
            except FileNotFoundError:
                pass
    return result


def run_long(args: argparse.Namespace, output_dir: Path,
             expected_body: bytes, static_dir: Path) -> dict:
    label = "keepalive-logging-off-sustained"
    scenario_dir = output_dir / label
    scenario_dir.mkdir(parents=True, exist_ok=True)
    port = free_port()
    log_path = scenario_dir / "server.log"
    proc = start_server(
        Path(args.binary), static_dir, scenario_dir / "unused.db", log_path,
        False, port,
    )
    started = time.monotonic()
    result = {"mode": label, "keepalive": True, "logging_enabled": False,
              "duration_seconds": args.long_duration, "port": port}
    try:
        wait_ready(proc, port, args.path, expected_body, log_path)
        if args.warmup_requests:
            warm = run_ab(
                port, args.path, args.long_connections, args.warmup_requests,
                None, True, args.timeout, scenario_dir / "warmup.txt",
            )
            result["warmup"] = warm
        run = run_ab(
            port, args.path, args.long_connections, args.long_requests,
            None, True, args.timeout,
            scenario_dir / "long.txt",
        )
        result["run"] = run
        result["completed_all_requested"] = (
            run["parsed"]["complete_requests"] == args.long_requests
        )
        parsed = run["parsed"]
        result["failed_requests"] = parsed["failed_requests"]
        result["non_2xx_responses"] = parsed["non_2xx_responses"]
        result["successful_rps"] = run["successful_rps"]
        result["test_seconds"] = run.get("test_seconds")
        result["process_wall_seconds"] = run["wall_seconds"]
        result["correctness"] = (
            parsed["failed_requests"] == 0
            and parsed["non_2xx_responses"] == 0
            and parsed["keepalive_requests"] > 0
            and run["parsed"]["complete_requests"] == args.long_requests
        )
        probe_conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        probe_conn.request("GET", args.path,
                           headers={"Host": "127.0.0.1", "Connection": "close"})
        probe = probe_conn.getresponse()
        result["final_probe"] = {
            "status": probe.status,
            "body_matches": probe.read() == expected_body,
        }
        probe_conn.close()
    finally:
        proc.send_signal(signal.SIGTERM)
        wait = wait_for_child(proc, 10)
        if wait[0] is None:
            proc.kill()
            wait_for_child(proc, 5)
        usage = wait[1]
    result["server_exit_code"] = proc.returncode
    result["server_cpu_seconds"] = (
        round(usage.ru_utime + usage.ru_stime, 3) if usage else None
    )
    result["server_max_rss_kb"] = max_rss_kb(usage)
    result["wall_seconds"] = round(time.monotonic() - started, 3)
    result["server_cpu_percent"] = (
        round(100.0 * result["server_cpu_seconds"] / result["wall_seconds"], 1)
        if result.get("server_cpu_seconds") and result["wall_seconds"] else None
    )
    result["raw_output_path"] = str(scenario_dir / "long.txt")
    result["server_log"] = str(log_path)
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark matrix for http_server")
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    parser.add_argument("--static-dir", default="static")
    parser.add_argument("--path", default="/")
    parser.add_argument("--expected-body", default=str(ROOT / "static" / "index.html"))
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--requests", type=int, default=100000)
    parser.add_argument("--warmup-requests", type=int, default=20000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--sweep-connections", default="1,25,100")
    parser.add_argument("--sweep-requests", type=int, default=10000)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--long-duration", type=int, default=60)
    parser.add_argument("--long-requests", type=int, default=7_000_000)
    parser.add_argument("--long-connections", type=int, default=100)
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--label", default="")
    parser.add_argument("--keep-db", action="store_true")
    parser.add_argument("--only-long", action="store_true")
    parser.add_argument(
        "--mode",
        choices=["all", "close-off", "close-on", "keepalive-off", "keepalive-on"],
        default="all",
    )
    args = parser.parse_args()
    args.sweep_connections = [
        int(value) for value in args.sweep_connections.split(",") if value
    ]
    if not args.sweep_connections:
        parser.error("--sweep-connections must contain at least one value")
    if not all(value > 0 for value in args.sweep_connections):
        parser.error("concurrency values must be positive")
    if not shutil.which("ab"):
        parser.error("ApacheBench ('ab') is required")
    if not Path(args.binary).is_file():
        parser.error(f"binary not found: {args.binary}")
    return args


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output) / (
        f"{datetime.now().strftime('%Y%m%dT%H%M%SZ')}-{os.getpid()}"
    )
    output_dir.mkdir(parents=True, exist_ok=False)
    expected_body = Path(args.expected_body).read_bytes()
    static_dir = Path(args.static_dir)
    if not static_dir.is_absolute():
        static_dir = ROOT / static_dir
    metadata = {
        "label": args.label,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "environment": environment_metadata(),
        "build": build_metadata(Path(args.binary).resolve()),
        "workload": {
            "tool": "ab (ApacheBench)",
            "client_location": "same host, loopback 127.0.0.1",
            "payload": args.path,
            "expected_body_sha256": sha256_bytes(expected_body),
            "connections": args.connections,
            "requests_per_run": args.requests,
            "warmup_requests": args.warmup_requests,
            "runs": args.runs,
            "sweep_connections": args.sweep_connections,
            "sweep_requests": args.sweep_requests,
            "long_duration_seconds": args.long_duration,
            "protocol": "ab HTTP/1.0; -k sets Connection: keep-alive",
        },
    }
    scenarios = []
    if args.only_long:
        modes = []
    elif args.mode == "all":
        modes = [
            (keepalive, logging_enabled)
            for keepalive in (False, True)
            for logging_enabled in (False, True)
        ]
    else:
        mapping = {
            "close-off": (False, False),
            "close-on": (False, True),
            "keepalive-off": (True, False),
            "keepalive-on": (True, True),
        }
        modes = [mapping[args.mode]]
    for keepalive, logging_enabled in modes:
        print(f"[matrix] {keepalive=} {logging_enabled=} ...", flush=True)
        scenario = run_scenario(
            args, keepalive, logging_enabled, output_dir, expected_body,
            static_dir,
        )
        scenarios.append(scenario)
        summary = scenario["summary"]
        print(
            f"  {scenario['mode']}: median {summary['successful_rps_median']:.0f} "
            f"RPS p50={summary['p50_ms_median']:.1f} p99={summary['p99_ms_median']:.1f} "
            f"client_cpu={scenario['runs'][0]['client_cpu_percent'] if scenario['runs'] else None}% "
            f"server_cpu={scenario['server_cpu_percent']}%",
            flush=True,
        )
    long_result = None
    if args.long_duration > 0:
        print(f"[matrix] long keepalive logging-off {args.long_duration}s ...",
              flush=True)
        long_result = run_long(args, output_dir, expected_body, static_dir)
        print(f"  long: {long_result['successful_rps']:.0f} RPS "
              f"p99={long_result['run']['parsed']['latency_percentiles_ms'].get(99)}",
              flush=True)

    result = {
        "metadata": metadata,
        "scenarios": scenarios,
        "long_run": long_result,
        "complete": True,
    }
    result_path = output_dir / "result.json"
    result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[matrix] result: {result_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
