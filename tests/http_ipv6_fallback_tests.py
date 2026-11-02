#!/usr/bin/env python3
"""The server must keep serving on IPv4 when IPv6 is present but disabled.

Runs the server with an LD_PRELOAD shim (tests/ipv6_disabled_shim.cpp) that
makes IPv6 bind() fail with EADDRNOTAVAIL, as on a Linux host or container
with net.ipv6.conf.all.disable_ipv6=1. Before the fix the server exited
even though its IPv4 listener was ready.
"""

from __future__ import annotations

import argparse
import http.client
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def get_root(port: int) -> int | None:
    try:
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=0.5)
        connection.request("GET", "/", headers={"Host": "localhost"})
        status = connection.getresponse().status
        connection.close()
        return status
    except OSError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build" / "bin" / "http_server"))
    parser.add_argument("--shim", required=True)
    args = parser.parse_args()

    port = free_port()
    env = {
        **os.environ,
        "LD_PRELOAD": args.shim,
        "PORT": str(port),
        "ENABLE_LOGGING": "false",
        "STATIC_DIR": str(ROOT / "static"),
    }
    server = subprocess.Popen(
        [args.binary], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    try:
        deadline = time.monotonic() + 10
        status = None
        while time.monotonic() < deadline and server.poll() is None:
            status = get_root(port)
            if status is not None:
                break
            time.sleep(0.05)

        if server.poll() is not None:
            print(f"FAIL: server exited with code {server.returncode}")
            print(server.stdout.read())
            return 1
        if status != 200:
            print(f"FAIL: expected 200 over IPv4, got {status}")
            return 1
    finally:
        server.terminate()
        output, _ = server.communicate(timeout=10)

    if "IPv6 unavailable" not in output:
        print("FAIL: expected a warning that IPv6 is unavailable")
        print(output)
        return 1
    print("PASS: IPv6 disabled -> warned and served on IPv4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
