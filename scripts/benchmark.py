#!/usr/bin/env python3
"""Run a bounded, repeatable loopback benchmark against http_server.

The harness starts the real server binary, waits for it to become ready,
runs a short warm-up, then runs repeated ApacheBench measurements. It records
raw client output, response-correctness checks, client/server CPU, and the
resulting JSON so before/after comparisons use the same procedure.

ApacheBench is used because it is available on both Linux and macOS and does
not abort connections early. With the current connection-per-request server,
wrk can be killed by SIGPIPE and can exhaust the loopback client port range
when logging delays the server-side close. See docs/benchmarking.md.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import platform
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
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "bin" / "http_server"
DEFAULT_OUTPUT = ROOT / "data" / "benchmarks"
SCHEMA_FILE = ROOT / "scripts" / "init_db.sql"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command: List[str]) -> str:
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=10, cwd=ROOT
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return (result.stdout or result.stderr).strip()


def git_value(*args: str) -> str:
    return command_output(["git", *args])


def cpu_model() -> str:
    if sys.platform == "darwin":
        return command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as handle:
            for line in handle:
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def build_metadata(binary: Path) -> Dict[str, Any]:
    cache = ROOT / "build" / "CMakeCache.txt"
    flags_file = ROOT / "build" / "CMakeFiles" / "http_server.dir" / "flags.make"
    build_type = "unknown"
    compiler = "unknown"
    if cache.is_file():
        text = cache.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", text, re.M)
        if match:
            build_type = match.group(1)
        match = re.search(r"^CMAKE_CXX_COMPILER:FILEPATH=(.*)$", text, re.M)
        if match:
            compiler = match.group(1)
    flags = ""
    if flags_file.is_file():
        text = flags_file.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if line.startswith("CXX_FLAGS ="):
                flags = line.split("=", 1)[1].strip()
                break
    return {
        "binary": str(binary),
        "binary_sha256": sha256_file(binary) if binary.is_file() else None,
        "build_type": build_type,
        "compiler": compiler,
        "cxx_flags": flags,
        "git_commit": git_value("rev-parse", "HEAD"),
        "git_dirty": bool(git_value("status", "--porcelain")),
    }


def ab_version() -> Optional[str]:
    text = command_output(["ab", "-V"])
    for line in text.splitlines():
        if "ApacheBench" in line and "Version" in line:
            return line.strip()
    return text.splitlines()[0].strip() if text else None


def environment_metadata() -> Dict[str, Any]:
    return {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "hostname": socket.gethostname(),
        "platform": platform.system(),
        "platform_release": platform.release(),
        "platform_version": platform.version(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
        "ab_version": ab_version() if shutil.which("ab") else None,
    }


def wait_for_child(
    proc: subprocess.Popen, timeout: float
) -> Tuple[Optional[int], Optional[os.times_result]]:
    """Wait for a child without reaping it through Popen, preserving rusage."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pid, status, usage = os.wait4(proc.pid, os.WNOHANG)
        if pid:
            proc.returncode = os.waitstatus_to_exitcode(status)
            return status, usage
        time.sleep(0.02)
    return None, None


def run_ab(
    url: str, requests: int, connections: int, timeout: int, raw_path: Path
) -> Dict[str, Any]:
    command = [
        "ab",
        "-n",
        str(requests),
        "-c",
        str(connections),
        "-s",
        str(timeout),
        url,
    ]
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
            # ab has no global watchdog; bound the wait generously based on
            # the requested count and per-request timeout.
            budget = max(120, requests // 500 + timeout + 30)
            status, usage = wait_for_child(proc, budget)
        finally:
            if proc.poll() is None:
                proc.kill()
                wait_for_child(proc, 5)
    wall = time.monotonic() - started
    if status is None:
        raise RuntimeError(f"ab exceeded its time budget; output: {raw_path}")
    text = raw_path.read_text(encoding="utf-8", errors="replace")
    return {
        "command": command,
        "exit_code": proc.returncode,
        "wall_seconds": round(wall, 3),
        "client_cpu_seconds": round(usage.ru_utime + usage.ru_stime, 3),
        "client_cpu_percent": round(
            100.0 * (usage.ru_utime + usage.ru_stime) / wall, 1
        )
        if wall > 0
        else None,
        "raw_output": text,
    }


def parse_ab(text: str) -> Dict[str, Any]:
    def number(pattern: str, default: Optional[float] = None) -> Optional[float]:
        match = re.search(pattern, text, re.M)
        if not match:
            return default
        return float(match.group(1))

    percentiles = {
        int(match.group(1)): float(match.group(2))
        for match in re.finditer(r"^\s+(\d+)%\s+([\d.]+)\s*$", text, re.M)
    }
    failure_breakdown = {}
    breakdown_match = re.search(r"^\s*\((.*)\)\s*$", text, re.M)
    if breakdown_match:
        failure_breakdown = {
            match.group(1).lower(): int(match.group(2))
            for match in re.finditer(
                r"(Connect|Receive|Length|Exceptions):\s*(\d+)",
                breakdown_match.group(1),
            )
        }
    test_seconds_match = re.search(
        r"^Time taken for tests:\s+([\d.]+)\s+seconds", text, re.M
    )
    return {
        "test_seconds": float(test_seconds_match.group(1))
        if test_seconds_match else None,
        "complete_requests": int(number(r"^Complete requests:\s+(\d+)", 0) or 0),
        "failed_requests": int(number(r"^Failed requests:\s+(\d+)", 0) or 0),
        "non_2xx_responses": int(number(r"^Non-2xx responses:\s+(\d+)", 0) or 0),
        "requests_per_second": number(r"^Requests per second:\s+([\d.]+)"),
        "mean_time_per_request_ms": number(
            r"^Time per request:\s+([\d.]+)\s+\[ms\]"
        ),
        "transfer_rate_kbps": number(r"^Transfer rate:\s+([\d.]+)"),
        "latency_percentiles_ms": percentiles,
        "failure_breakdown": failure_breakdown,
    }


def http_probe(host: str, port: int, path: str) -> Dict[str, Any]:
    connection = http.client.HTTPConnection(host, port, timeout=5)
    try:
        connection.request(
            "GET",
            path,
            headers={"Host": f"{host}:{port}", "Connection": "close"},
        )
        response = connection.getresponse()
        body = response.read()
        return {
            "status": response.status,
            "reason": response.reason,
            "content_type": response.getheader("Content-Type"),
            "content_length": response.getheader("Content-Length"),
            "connection": response.getheader("Connection"),
            "body": body,
        }
    finally:
        connection.close()


def validate_probe(
    probe: Dict[str, Any], expected_body: Optional[bytes]
) -> Dict[str, Any]:
    body = probe["body"]
    checks = {
        "status_200": probe["status"] == 200,
        "content_type_present": bool(probe["content_type"]),
        "content_length_matches": probe["content_length"] == str(len(body)),
        "connection_close": (probe["connection"] or "").lower() == "close",
        "body_sha256": sha256_bytes(body),
        "body_bytes": len(body),
    }
    if expected_body is not None:
        checks["body_matches_expected"] = body == expected_body
    checks["passed"] = all(
        value for key, value in checks.items() if key not in {"body_sha256", "body_bytes"}
    )
    return checks


def port_is_open(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=0.25):
            return True
    except OSError:
        return False


def wait_until_ready(
    proc: subprocess.Popen,
    host: str,
    port: int,
    path: str,
    expected_body: bytes,
    log_path: Path,
) -> Dict[str, Any]:
    deadline = time.monotonic() + 10
    last_error: Optional[Exception] = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            output = log_path.read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(
                f"server exited before becoming ready (code {proc.returncode})\n{output}"
            )
        try:
            probe = http_probe(host, port, path)
            checks = validate_probe(probe, expected_body)
            if checks["passed"]:
                return checks
            last_error = RuntimeError(f"readiness probe failed: {checks}")
        except OSError as error:
            last_error = error
        time.sleep(0.05)
    output = log_path.read_text(encoding="utf-8", errors="replace")
    raise RuntimeError(f"server did not become ready: {last_error}\n{output}")


def stop_server(
    proc: subprocess.Popen,
) -> Tuple[Optional[os.times_result], Optional[int], bool]:
    """Return (rusage, exit status, needed_kill)."""
    if proc.poll() is not None:
        return None, proc.returncode, False
    proc.send_signal(signal.SIGTERM)
    status, usage = wait_for_child(proc, 10)
    if status is not None:
        return usage, proc.returncode, False
    proc.kill()
    status, usage = wait_for_child(proc, 5)
    return usage, proc.returncode, True


def scenario_name(logging_enabled: bool) -> str:
    return "logging-on" if logging_enabled else "logging-off"


def run_scenario(
    args: argparse.Namespace,
    logging_enabled: bool,
    output_dir: Path,
    expected_body: Optional[bytes],
) -> Dict[str, Any]:
    name = scenario_name(logging_enabled)
    scenario_dir = output_dir / name
    scenario_dir.mkdir(parents=True, exist_ok=True)
    log_path = scenario_dir / "server.log"
    db_path = scenario_dir / "access.db"
    for suffix in ("", "-wal", "-shm"):
        try:
            Path(str(db_path) + suffix).unlink()
        except FileNotFoundError:
            pass

    if port_is_open(args.host, args.port):
        raise RuntimeError(
            f"{args.host}:{args.port} is already in use; refusing to benchmark"
        )

    static_dir = Path(args.static_dir)
    if not static_dir.is_absolute():
        static_dir = ROOT / static_dir
    env = os.environ.copy()
    env.update(
        {
            "PORT": str(args.port),
            "STATIC_DIR": str(static_dir.resolve()),
            "DB_PATH": str(db_path),
            "ENABLE_LOGGING": "true" if logging_enabled else "false",
        }
    )
    server_log = log_path.open("wb")
    proc = subprocess.Popen(
        [str(Path(args.binary).resolve())],
        cwd=ROOT,
        env=env,
        stdout=server_log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )

    url = f"http://{args.host}:{args.port}{args.path}"
    result: Dict[str, Any] = {
        "mode": name,
        "logging_enabled": logging_enabled,
        "database_path": str(db_path),
        "server_log": str(log_path),
        "runs": [],
    }
    started = time.monotonic()
    try:
        result["initial_check"] = wait_until_ready(
            proc, args.host, args.port, args.path, expected_body, log_path
        )

        if args.warmup_requests > 0:
            warmup_path = scenario_dir / "warmup.txt"
            warmup = run_ab(
                url,
                args.warmup_requests,
                args.connections,
                args.timeout,
                warmup_path,
            )
            result["warmup"] = {
                "requests": args.warmup_requests,
                "exit_code": warmup["exit_code"],
                "command": warmup["command"],
                "raw_output": warmup["raw_output"],
            }

        for index in range(1, args.runs + 1):
            raw_path = scenario_dir / f"run-{index}.txt"
            run = run_ab(
                url, args.requests, args.connections, args.timeout, raw_path
            )
            parsed = parse_ab(run["raw_output"])
            probe = http_probe(args.host, args.port, args.path)
            checks = validate_probe(probe, expected_body)
            complete = parsed["complete_requests"]
            failed = parsed["failed_requests"]
            successful = max(complete - failed, 0)
            run.update(parsed)
            run.update(
                {
                    "run": index,
                    "requests_requested": args.requests,
                    "completed_all_requests": complete == args.requests,
                    "successful_requests": successful,
                    "successful_rps": round(
                        (parsed["requests_per_second"] or 0.0)
                        * (successful / complete),
                        2,
                    )
                    if complete
                    else 0.0,
                    "correctness": checks,
                    "server_alive_after_run": proc.poll() is None,
                    "raw_output_path": str(raw_path),
                }
            )
            # Don't retain a second copy of the raw output in JSON.
            run.pop("raw_output", None)
            if run["server_alive_after_run"] is False:
                raise RuntimeError(
                    f"server exited during {name} run {index} "
                    f"(code {proc.returncode}); see {log_path}"
                )
            if run["exit_code"] != 0 or not run["completed_all_requests"]:
                raise RuntimeError(
                    f"ab did not complete {args.requests} requests in run {index} "
                    f"(exit {run['exit_code']}, complete {complete}); see {raw_path}"
                )
            result["runs"].append(run)
            if not checks["passed"]:
                raise RuntimeError(f"response correctness failed after run {index}")

        result["final_check"] = validate_probe(
            http_probe(args.host, args.port, args.path), expected_body
        )
    finally:
        usage, exit_code, needed_kill = stop_server(proc)
        server_log.close()

    window = time.monotonic() - started
    server_cpu = (
        round(usage.ru_utime + usage.ru_stime, 3) if usage is not None else None
    )
    result["server_exit_code"] = exit_code
    result["server_needed_sigkill"] = needed_kill
    result["server_cpu_seconds"] = server_cpu
    result["server_cpu_percent"] = (
        round(100.0 * server_cpu / window, 1)
        if server_cpu is not None and window > 0
        else None
    )
    result["wall_seconds"] = round(window, 3)

    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    result["server_log_sqlite_busy_errors"] = log_text.count("database is locked")
    result["server_log_broken_pipe_errors"] = log_text.count("Broken pipe")

    if logging_enabled and db_path.is_file():
        try:
            with sqlite3.connect(db_path) as connection:
                result["database_rows"] = connection.execute(
                    "SELECT COUNT(*) FROM access_logs"
                ).fetchone()[0]
        except sqlite3.Error as error:
            result["database_rows_error"] = str(error)
    if not args.keep_db:
        for suffix in ("", "-wal", "-shm"):
            try:
                Path(str(db_path) + suffix).unlink()
            except FileNotFoundError:
                pass

    runs = result["runs"]
    rps_values = [run["successful_rps"] for run in runs]
    result["summary"] = {
        "runs": len(runs),
        "successful_rps_min": min(rps_values) if rps_values else 0.0,
        "successful_rps_median": statistics.median(rps_values) if rps_values else 0.0,
        "successful_rps_max": max(rps_values) if rps_values else 0.0,
        "p50_ms_median": statistics.median(
            [run["latency_percentiles_ms"].get(50, 0.0) for run in runs]
        )
        if runs
        else 0.0,
        "p99_ms_median": statistics.median(
            [run["latency_percentiles_ms"].get(99, 0.0) for run in runs]
        )
        if runs
        else 0.0,
        "failed_requests_total": sum(run["failed_requests"] for run in runs),
        "non_2xx_total": sum(run["non_2xx_responses"] for run in runs),
        "all_runs_correct": all(run["correctness"]["passed"] for run in runs),
        "logging_rows_at_least_measured": (
            not logging_enabled
            or result.get("database_rows", 0) >= args.runs * args.requests
        ),
    }
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bounded loopback benchmark harness for http_server"
    )
    parser.add_argument("--binary", default=str(DEFAULT_BINARY))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--static-dir", default="static")
    parser.add_argument("--path", default="/")
    parser.add_argument(
        "--expected-body",
        default=str(ROOT / "static" / "index.html"),
        help="File the response body must match; pass an empty string to skip",
    )
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument(
        "--requests", type=int, default=100000, help="requests per measured run"
    )
    parser.add_argument(
        "--warmup-requests", type=int, default=20000, help="warm-up requests"
    )
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=30, help="ab request timeout")
    parser.add_argument(
        "--logging", choices=("on", "off", "both"), default="both"
    )
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--label", default="", help="optional label for the result")
    parser.add_argument("--keep-db", action="store_true")
    args = parser.parse_args()

    if (
        args.connections < 1
        or args.requests < 1
        or args.warmup_requests < 0
        or args.runs < 1
    ):
        parser.error(
            "--connections and --requests must be positive; "
            "--warmup-requests and --runs must be non-negative"
        )
    if not shutil.which("ab"):
        parser.error("ApacheBench ('ab') is required")
    if not Path(args.binary).is_file():
        parser.error(
            f"server binary not found: {args.binary}; build with "
            "'cmake -S . -B build && cmake --build build -j'"
        )
    if not SCHEMA_FILE.is_file():
        parser.error(f"schema not found: {SCHEMA_FILE}")
    return args


def main() -> int:
    args = parse_args()
    expected_body = (
        Path(args.expected_body).read_bytes() if args.expected_body else None
    )
    output_dir = (
        Path(args.output)
        / f"{datetime.now().strftime('%Y%m%dT%H%M%SZ')}-{os.getpid()}"
    )
    output_dir.mkdir(parents=True, exist_ok=False)

    metadata = {
        "label": args.label,
        "environment": environment_metadata(),
        "build": build_metadata(Path(args.binary).resolve()),
        "workload": {
            "tool": "ab (ApacheBench)",
            "protocol": "HTTP/1.0 request emitted by ab; server replies HTTP/1.1",
            "url": f"http://{args.host}:{args.port}{args.path}",
            "method": "GET",
            "connections": args.connections,
            "requests_per_run": args.requests,
            "warmup_requests": args.warmup_requests,
            "runs": args.runs,
            "request_timeout_seconds": args.timeout,
            "expected_body": args.expected_body,
            "expected_body_sha256": sha256_bytes(expected_body)
            if expected_body is not None
            else None,
            "client_location": "same host, loopback (127.0.0.1)",
        },
        "server_config": {
            "port": args.port,
            "static_dir": args.static_dir,
            "logging_modes": ["off", "on"]
            if args.logging == "both"
            else [args.logging],
            "database_schema": str(SCHEMA_FILE),
        },
        "notes": [
            "Server CPU is aggregate process CPU from start until SIGTERM.",
            "Client CPU is per ab process, measured via wait4 rusage.",
            "This measures connection-per-request behavior with Connection: close.",
        ],
    }

    modes = [False, True] if args.logging == "both" else [args.logging == "on"]
    scenarios = []
    failures = []
    for logging_enabled in modes:
        print(
            f"[benchmark] mode={scenario_name(logging_enabled)} "
            f"c={args.connections} requests={args.requests} runs={args.runs}"
        )
        try:
            scenario = run_scenario(args, logging_enabled, output_dir, expected_body)
        except Exception as error:  # Keep partial artifacts for diagnosis.
            failures.append(f"{scenario_name(logging_enabled)}: {error}")
            print(f"[benchmark] FAILED: {error}", file=sys.stderr)
            continue
        scenarios.append(scenario)
        summary = scenario["summary"]
        print(
            f"  {scenario['mode']}: median {summary['successful_rps_median']:.0f} "
            f"successful RPS (min {summary['successful_rps_min']:.0f}, "
            f"max {summary['successful_rps_max']:.0f}), "
            f"p50 {summary['p50_ms_median']:.1f} ms, "
            f"p99 {summary['p99_ms_median']:.1f} ms, "
            f"failed {summary['failed_requests_total']}, "
            f"server CPU {scenario['server_cpu_percent']}%, "
            f"log rows {scenario.get('database_rows', 'n/a')}"
        )

    result = {
        "metadata": metadata,
        "scenarios": scenarios,
        "failures": failures,
        "complete": not failures and len(scenarios) == len(modes),
    }
    result_path = output_dir / "result.json"
    result_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"[benchmark] result: {result_path}")

    if failures:
        for failure in failures:
            print(f"[benchmark] failure: {failure}", file=sys.stderr)
        return 1
    if not all(scenario["summary"]["all_runs_correct"] for scenario in scenarios):
        return 1
    if not all(
        scenario["summary"]["logging_rows_at_least_measured"]
        for scenario in scenarios
    ):
        print("[benchmark] failure: logging row count is below measured requests", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
