#!/usr/bin/env python3
"""Unit tests for benchmark parsing and aggregation arithmetic."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import benchmark  # noqa: E402
import benchmark_matrix  # noqa: E402
import benchmark_wrk_matrix  # noqa: E402

FAILURES = 0


def check(condition: bool, message: str) -> None:
    global FAILURES
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        FAILURES += 1


def test_ab_parser() -> None:
    raw = """
Time taken for tests:   65.992 seconds
Complete requests:      7000000
Failed requests:        3
Non-2xx responses:      2
Requests per second:    106074.13 [#/sec] (mean)
Percentage of the requests served within a certain time (ms)
  50%      1
  99%      3
Socket errors: connect 0, read 4, write 0, timeout 0
"""
    parsed = benchmark.parse_ab(raw)
    check(parsed["test_seconds"] == 65.992, "ab test_seconds")
    check(parsed["complete_requests"] == 7000000, "ab complete")
    check(parsed["failed_requests"] == 3, "ab failed")
    check(parsed["non_2xx_responses"] == 2, "ab non2xx")
    check(parsed["latency_percentiles_ms"] == {50: 1.0, 99: 3.0},
          "ab percentiles")
    extras = benchmark_matrix.parse_ab_extras(raw)
    check(extras["socket_errors"] == {"connect": 0, "read": 4, "write": 0,
                                      "timeout": 0}, "ab socket errors")
    check(benchmark_matrix.ab_successful_requests(7000000, 3) == 6999997,
          "ab successful count")
    check(abs(6999997 / 65.992 - 106074.28) < 1.0, "ab successful rate")


def test_wrk_parser() -> None:
    raw = """
  100 requests in 10.00s, 1.00MB read
  Latency Distribution
     50%    1.50ms
     99%   10.00ms
Requests/sec:      10.00
Socket errors: connect 0, read 2, write 0, timeout 0
Non-2xx or 3xx responses: 1
"""
    parsed = benchmark_wrk_matrix.parse_wrk_output(raw)
    check(parsed["requests"] == 100, "wrk requests")
    check(parsed["test_seconds"] == 10.0, "wrk test duration")
    check(parsed["non_2xx_responses"] == 1, "wrk non2xx")
    check(parsed["socket_error_total"] == 2, "wrk socket errors")
    check(parsed["successful_requests"] == 99,
          "wrk socket errors must not be subtracted from completed requests")
    check(parsed["successful_rps"] == 9.9, "wrk successful RPS")
    check(parsed["latency_percentiles_ms"] == {50: 1.5, 99: 10.0},
          "wrk percentiles")


def test_wrk_minute_duration() -> None:
    raw = """
  7000000 requests in 1.00m, 9.00GB read
  Latency Distribution
     50%    1.00ms
     99%    3.00ms
Requests/sec: 116666.67
"""
    parsed = benchmark_wrk_matrix.parse_wrk_output(raw)
    check(parsed["test_seconds"] == 60.0, "wrk minute duration")
    check(parsed["requests"] == 7000000, "wrk minute requests")
    check(parsed["successful_rps"] == 116666.67, "wrk reported RPS")


def test_median_aggregation() -> None:
    check(benchmark_matrix.aggregate_median([10.0, 20.0]) == 15.0,
          "two-run median must be the mean, not the maximum")
    check(benchmark_wrk_matrix.aggregate_median([10.0, 20.0]) == 15.0,
          "wrk two-run median must be the mean")
    check(benchmark_matrix.aggregate_median([1.0, 5.0, 3.0]) == 3.0,
          "three-run median")
    check(benchmark_wrk_matrix.aggregate_median([100.0, 10.0]) == 55.0,
          "wrk median ordering")


def main() -> int:
    test_ab_parser()
    test_wrk_parser()
    test_wrk_minute_duration()
    test_median_aggregation()
    if FAILURES:
        print(f"{FAILURES} benchmark parser test(s) failed", file=sys.stderr)
        return 1
    print("all benchmark parser tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
