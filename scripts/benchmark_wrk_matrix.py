#!/usr/bin/env python3
"""Same-duration wrk matrix: close/keep-alive x logging off/on.

wrk is used for the primary comparison because it supports fixed durations for
both keep-alive and Connection: close. ab results are retained separately as a
fixed-count cross-check.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import re
import shutil
import signal
import sqlite3
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from benchmark import ROOT, build_metadata, environment_metadata, sha256_bytes, wait_for_child
from benchmark_matrix import (
    db_rows,
    max_rss_kb,
    parse_logger_metrics,
    start_server,
    wait_ready,
    free_port,
)

DEFAULT_OUTPUT = ROOT / "data" / "benchmarks"


def aggregate_median(values: list[float]) -> float:
    return float(statistics.median(values)) if values else 0.0


def wrk_successful_requests(requests: int, non_2xx: int) -> int:
    # wrk counts completed responses in "requests"; socket errors are separate
    # connection-level counters and are not part of that completed count.
    return max(requests - non_2xx, 0)


def parse_wrk_output(text: str) -> dict:
    requests_match = re.search(
        r"([\d,]+) requests in ([\d.]+)(s|m|h)", text
    )
    rps_match = re.search(r"Requests/sec:\s+([\d.]+)", text)
    socket_match = re.search(
        r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)",
        text,
    )
    non2xx_match = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", text)
    latencies = {}
    for match in re.finditer(
        r"^\s+(\d+)%\s+([\d.]+)(us|ms|s)\s*$", text, re.M
    ):
        value = float(match.group(2))
        unit = match.group(3)
        if unit == "us":
            value /= 1000.0
        elif unit == "s":
            value *= 1000.0
        latencies[int(match.group(1))] = value
    requests = (
        int(requests_match.group(1).replace(",", ""))
        if requests_match else 0
    )
    socket_errors = {
        name: int(value)
        for name, value in zip(
            ("connect", "read", "write", "timeout"),
            socket_match.groups() if socket_match else ("0", "0", "0", "0"),
        )
    }
    non2xx = int(non2xx_match.group(1)) if non2xx_match else 0
    test_seconds = None
    if requests_match:
        value = float(requests_match.group(2))
        unit = requests_match.group(3)
        test_seconds = value * {"s": 1.0, "m": 60.0, "h": 3600.0}[unit]
    successful = wrk_successful_requests(requests, non2xx)
    reported_rps = float(rps_match.group(1)) if rps_match else 0.0
    return {
        "requests": requests,
        "test_seconds": test_seconds,
        "requests_per_second": float(rps_match.group(1)) if rps_match else 0.0,
        "latency_percentiles_ms": latencies,
        "socket_errors": socket_errors,
        "socket_error_total": sum(socket_errors.values()),
        "non_2xx_responses": non2xx,
        "successful_requests": successful,
        "successful_rps": round(
            reported_rps * (successful / requests), 2
        ) if requests else 0.0,
    }


def run_wrk(
    port: int, path: str, connections: int, threads: int, duration: int,
    close_mode: bool, raw_path: Path,
) -> dict:
    command = [
        "wrk", "-t", str(threads), "-c", str(connections),
        "-d", f"{duration}s", "--latency",
    ]
    if close_mode:
        command += ["-H", "Connection: close"]
    command.append(f"http://127.0.0.1:{port}{path}")
    started = time.monotonic()
    with raw_path.open("wb") as raw:
        proc = subprocess.Popen(
            command, cwd=ROOT, stdout=raw, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        status, usage = wait_for_child(proc, duration + 60)
        if status is None:
            proc.kill()
            wait_for_child(proc, 5)
            raise RuntimeError(f"wrk exceeded budget; see {raw_path}")
    wall = time.monotonic() - started
    text = raw_path.read_text(encoding="utf-8", errors="replace")
    parsed = parse_wrk_output(text)
    client_cpu = round(usage.ru_utime + usage.ru_stime, 3) if usage else None
    return {
        "command": command,
        "exit_code": proc.returncode,
        "wall_seconds": round(wall, 3),
        "client_cpu_seconds": client_cpu,
        "client_cpu_percent": round(100.0 * client_cpu / wall, 1)
        if client_cpu is not None and wall > 0 else None,
        "client_max_rss_kb": max_rss_kb(usage),
        **parsed,
        "raw_output_path": str(raw_path),
    }


def probe(port: int, path: str, expected_body: bytes) -> dict:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request(
            "GET", path,
            headers={"Host": f"127.0.0.1:{port}", "Connection": "close"},
        )
        response = connection.getresponse()
        body = response.read()
        return {
            "status": response.status,
            "content_length": response.getheader("Content-Length"),
            "body_matches": body == expected_body,
        }
    finally:
        connection.close()


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
        Path(args.binary), static_dir, db_path, log_path, logging_enabled, port,
        extra_env={"DIAGNOSTICS_INTERVAL_MS": args.diagnostics_interval},
    )
    result = {
        "mode": label,
        "keepalive": keepalive,
        "logging_enabled": logging_enabled,
        "runs": [],
        "warmup_seconds": args.warmup,
        "duration_seconds": args.duration,
        "server_pid": proc.pid,
        "server_command": [str(Path(args.binary).resolve())],
        "server_config": {
            "ENABLE_LOGGING": logging_enabled,
            "DIAGNOSTICS_INTERVAL_MS": args.diagnostics_interval,
            "MAX_CONNECTIONS": 1024,
            "KEEPALIVE_TIMEOUT_MS": 120000,
            "MAX_REQUESTS_PER_CONNECTION": 1000000,
            "MAX_PIPELINED_REQUESTS": 8,
            "MAX_CONNECTION_INPUT_BYTES": 128 * 1024,
            "HEADER_TIMEOUT_MS": 30000,
            "WRITE_TIMEOUT_MS": 30000,
            "SHUTDOWN_GRACE_MS": 3000,
        },
    }
    started = time.monotonic()
    try:
        wait_ready(proc, port, args.path, expected_body, log_path)
        result["initial_probe"] = probe(port, args.path, expected_body)
        if not result["initial_probe"]["body_matches"]:
            raise RuntimeError("initial exact-body probe failed")
        if args.warmup > 0:
            warm = run_wrk(
                port, args.path, args.connections, args.threads, args.warmup,
                not keepalive, scenario_dir / "warmup.txt",
            )
            result["warmup"] = warm
        result["db_rows_after_warmup"] = (
            db_rows(db_path) if logging_enabled else None
        )
        for index in range(1, args.runs + 1):
            raw = scenario_dir / f"run-{index}.txt"
            run = run_wrk(
                port, args.path, args.connections, args.threads, args.duration,
                not keepalive, raw,
            )
            run["run"] = index
            run["keepalive_required"] = keepalive
            run["correct"] = (
                run["exit_code"] == 0
                and run["non_2xx_responses"] == 0
                and run["socket_error_total"] == 0
            )
            run["allowed_for_benchmark"] = (
                run["exit_code"] == 0
                and run["non_2xx_responses"] == 0
                and (
                    run["socket_error_total"] == 0
                    or (args.allow_close_read_errors and not keepalive)
                )
            )
            if logging_enabled:
                run["db_rows_after_run"] = db_rows(db_path)
            result["runs"].append(run)
            if not run["allowed_for_benchmark"]:
                raise RuntimeError(
                    f"{label} run {index} errors={run['socket_errors']} "
                    f"non2xx={run['non_2xx_responses']}; see {raw}"
                )
        result["final_probe"] = probe(port, args.path, expected_body)
        if not result["final_probe"]["body_matches"] or result["final_probe"]["status"] != 200:
            raise RuntimeError(f"{label} final probe failed")
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
        result["server_exit_code"] = proc.returncode
        result["server_needed_sigkill"] = needed_kill
        result["wall_seconds"] = round(time.monotonic() - started, 3)
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    result["server_cpu_seconds"] = (
        round(usage.ru_utime + usage.ru_stime, 3) if usage else None
    )
    result["server_cpu_percent"] = (
        round(100.0 * result["server_cpu_seconds"] / result["wall_seconds"], 1)
        if result["server_cpu_seconds"] and result["wall_seconds"] else None
    )
    result["server_max_rss_kb"] = max_rss_kb(usage)
    result["logger_metrics"] = parse_logger_metrics(log_text)
    result["server_log"] = str(log_path)
    result["database_path"] = str(db_path) if logging_enabled else None
    if logging_enabled:
        result["db_rows_final"] = db_rows(db_path)
        if result.get("db_rows_after_probe") is not None and result.get(
            "db_rows_final"
        ) is not None:
            result["drained_after_load"] = (
                result["db_rows_final"] - result["db_rows_after_probe"]
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
    rps = [run["successful_rps"] for run in result["runs"]]
    result["sampled_body_checks"] = 2  # initial + final probes
    result["summary"] = {
        "runs": len(rps),
        "rps_min": min(rps) if rps else 0.0,
        "rps_median": aggregate_median(rps),
        "rps_max": max(rps) if rps else 0.0,
        "p50_ms_median": aggregate_median(
            [run["latency_percentiles_ms"].get(50, 0.0)
             for run in result["runs"]]
        ),
        "p99_ms_median": aggregate_median(
            [run["latency_percentiles_ms"].get(99, 0.0)
             for run in result["runs"]]
        ),
        "socket_errors_total": sum(
            run["socket_error_total"] for run in result["runs"]
        ),
        "non_2xx_total": sum(run["non_2xx_responses"] for run in result["runs"]),
        "all_correct": all(run["correct"] for run in result["runs"]),
        "all_allowed_for_benchmark": all(
            run["allowed_for_benchmark"] for run in result["runs"]
        ),
        "client_cpu_near_limit": any(
            run["client_cpu_percent"] is not None and run["client_cpu_percent"] >= 90
            for run in result["runs"]
        ),
    }
    if not args.keep_db:
        for suffix in ("", "-wal", "-shm"):
            try:
                Path(str(db_path) + suffix).unlink()
            except FileNotFoundError:
                pass
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    parser.add_argument("--static-dir", default="static")
    parser.add_argument("--path", default="/")
    parser.add_argument("--expected-body", default=str(ROOT / "static" / "index.html"))
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--label", default="")
    parser.add_argument("--keep-db", action="store_true")
    parser.add_argument("--allow-close-read-errors", action="store_true")
    parser.add_argument("--diagnostics-interval", type=int, default=1000)
    parser.add_argument(
        "--mode",
        choices=["all", "keepalive-off", "keepalive-on", "close-off", "close-on"],
        default="all",
    )
    args = parser.parse_args()
    if not shutil.which("wrk"):
        parser.error("wrk is required")
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
            "tool": "wrk",
            "client_location": "same host, loopback 127.0.0.1",
            "http_version": "HTTP/1.1",
            "payload": args.path,
            "expected_body_sha256": sha256_bytes(expected_body),
            "connections": args.connections,
            "threads": args.threads,
            "duration_seconds": args.duration,
            "warmup_seconds": args.warmup,
            "runs": args.runs,
        },
    }
    if args.mode == "all":
        modes = [(keepalive, logging_enabled)
                 for keepalive in (False, True)
                 for logging_enabled in (False, True)]
    else:
        mapping = {
            "keepalive-off": (True, False),
            "keepalive-on": (True, True),
            "close-off": (False, False),
            "close-on": (False, True),
        }
        modes = [mapping[args.mode]]
    scenarios = []
    for keepalive, logging_enabled in modes:
            print(f"[wrk] {keepalive=} {logging_enabled=} ...", flush=True)
            scenario = run_scenario(
                args, keepalive, logging_enabled, output_dir, expected_body,
                static_dir,
            )
            scenarios.append(scenario)
            s = scenario["summary"]
            print(f"  {scenario['mode']}: {s['rps_median']:.0f} RPS "
                  f"p50={s['p50_ms_median']:.2f} p99={s['p99_ms_median']:.2f} "
                  f"client_cpu={scenario['runs'][0]['client_cpu_percent']}% "
                  f"server_cpu={scenario['server_cpu_percent']}%", flush=True)
    result = {"metadata": metadata, "scenarios": scenarios}
    path = output_dir / "result.json"
    path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[wrk] result: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
