#!/usr/bin/env python3
"""回放 100 轮压测：record → play，每轮校验 checksum/executed 并记录延迟与 RSS。"""

import argparse
import os
import resource
import subprocess
import sys
import tempfile
import time


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", required=True)
    parser.add_argument("--replay", required=True)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()

    workdir = tempfile.mkdtemp(prefix="replay-stress-")
    path = os.path.join(workdir, "round.g2cr")
    latencies = []
    failures = []
    started = time.monotonic()
    for index in range(args.count):
        round_start = time.monotonic()
        try:
            record = subprocess.run(
                [args.record, path], stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=args.timeout)
            if record.returncode != 0:
                failures.append(f"#{index} record rc={record.returncode}")
                continue
            play = subprocess.run(
                [args.replay, "play", path], stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            failures.append(f"#{index} timeout")
            continue
        output = play.stdout.decode("utf-8", "replace")
        if (play.returncode != 0 or '"replay_executed":true' not in output
                or '"status":"ok"' not in output):
            failures.append(
                f"#{index} rc={play.returncode} status/executed mismatch")
            continue
        latencies.append((time.monotonic() - round_start) * 1000.0)
    elapsed_ms = (time.monotonic() - started) * 1000.0
    rss_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    print(
        f"RESULT count={args.count} failures={len(failures)} "
        f"total_ms={elapsed_ms:.1f} "
        f"p50_ms={percentile(latencies, 0.50):.1f} "
        f"p95_ms={percentile(latencies, 0.95):.1f} "
        f"p99_ms={percentile(latencies, 0.99):.1f} rss_kb={rss_kb}"
    )
    for failure in failures[:5]:
        print(f"FAILURE {failure}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
