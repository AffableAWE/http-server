#!/usr/bin/env python3
"""Bounded wrk thread-count comparison at fixed connections and duration."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from benchmark import ROOT, build_metadata, environment_metadata, sha256_bytes, wait_for_child
from benchmark_matrix import free_port, max_rss_kb, start_server, wait_ready
from benchmark_wrk_matrix import parse_wrk_output, probe, run_wrk


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    parser.add_argument("--static-dir", default="static")
    parser.add_argument("--path", default="/")
    parser.add_argument("--expected-body", default=str(ROOT / "static" / "index.html"))
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--threads", default="1,2,4")
    parser.add_argument("--duration", type=int, default=5)
    parser.add_argument("--output", default=str(ROOT / "data" / "benchmarks"))
    args = parser.parse_args()
    if not shutil.which("wrk"):
        parser.error("wrk is required")
    threads = [int(value) for value in args.threads.split(",") if value]
    static_dir = Path(args.static_dir)
    if not static_dir.is_absolute():
        static_dir = ROOT / static_dir
    expected_body = Path(args.expected_body).read_bytes()
    output_dir = Path(args.output) / (
        f"{datetime.now().strftime('%Y%m%dT%H%M%SZ')}-{os.getpid()}"
    )
    output_dir.mkdir(parents=True, exist_ok=False)
    port = free_port()
    log_path = output_dir / "server.log"
    proc = start_server(
        Path(args.binary), static_dir, output_dir / "unused.db", log_path,
        False, port,
        extra_env={"DIAGNOSTICS_INTERVAL_MS": 1000},
    )
    result = {
        "purpose": "wrk thread-count capacity probe at fixed total connections",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "environment": environment_metadata(),
        "build": build_metadata(Path(args.binary).resolve()),
        "server_pid": proc.pid,
        "server_command": [str(Path(args.binary).resolve())],
        "config": {
            "ENABLE_LOGGING": False,
            "DIAGNOSTICS_INTERVAL_MS": 1000,
            "MAX_CONNECTIONS": 1024,
            "KEEPALIVE_TIMEOUT_MS": 120000,
            "MAX_REQUESTS_PER_CONNECTION": 1000000,
            "MAX_PIPELINED_REQUESTS": 8,
            "HEADER_TIMEOUT_MS": 30000,
            "WRITE_TIMEOUT_MS": 30000,
        },
        "payload": args.path,
        "payload_bytes": len(expected_body),
        "payload_sha256": sha256_bytes(expected_body),
        "connections": args.connections,
        "duration_requested": args.duration,
        "runs": [],
    }
    try:
        wait_ready(proc, port, args.path, expected_body, log_path)
        result["initial_probe"] = probe(port, args.path, expected_body)
        for thread_count in threads:
            raw = output_dir / f"wrk-t{thread_count}.txt"
            run = run_wrk(
                port, args.path, args.connections, thread_count,
                args.duration, False, raw,
            )
            run["threads"] = thread_count
            result["runs"].append(run)
            time.sleep(1)
        result["final_probe"] = probe(port, args.path, expected_body)
    finally:
        proc.send_signal(signal.SIGTERM)
        status, usage = wait_for_child(proc, 10)
        if status is None:
            proc.kill()
            wait_for_child(proc, 5)
        result["server_cpu_seconds"] = round(
            usage.ru_utime + usage.ru_stime, 3
        ) if usage else None
        result["server_max_rss_kb"] = max_rss_kb(usage)
    result["raw_dir"] = str(output_dir)
    (output_dir / "result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
