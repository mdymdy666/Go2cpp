#!/usr/bin/env python3
"""go2cpp-cli 的随机参数测试与压测。

用法: stress.py <go2cpp-cli> <work-dir> [<fake-socket>]

- 随机参数：500 组随机 argv，断言退出码落在契约集合 {0,1,2,3,4,5}；
- 启动开销：重复运行 `--version` 200 次，统计均值/P50/P95/P99；
- 请求延迟：对假 DebugServer 重复 `status` 200 次，统计均值/P50/P95/P99。
  假 server 需以 --max-connections 200 启动，socket 路径由第三个参数传入。
"""

import json
import os
import random
import statistics
import subprocess
import sys
import time


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def timed_run(argv, env=None):
    start = time.perf_counter()
    completed = subprocess.run(argv, capture_output=True, env=env, timeout=20)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return completed, elapsed_ms


def main():
    if len(sys.argv) < 3:
        print("usage: stress.py <go2cpp-cli> <work-dir> [<fake-socket>]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    work_dir = sys.argv[2]
    fake_socket = sys.argv[3] if len(sys.argv) > 3 else ""

    report = {"random": {}, "startup": {}, "status_latency": {}}

    # ---- 1. 随机参数：只关心「不崩溃、退出码在契约内」 ----
    random.seed(20261020)
    commands = ["dump", "status", "replay", "migrate", "debug", "bogus", ""]
    options = [
        "--pid", "--out", "--gid", "--sections", "--format",
        "--timeout-ms", "--top", "--json", "--bogus", "-h",
    ]
    values = ["1", "0", "abc", "-1", "9999999999", "/tmp/x", "", "record", "play"]
    unexpected = []
    cases = 500
    for _ in range(cases):
        argv = [binary]
        if random.random() < 0.8:
            argv.append(random.choice(commands))
        for _ in range(random.randint(0, 6)):
            argv.append(random.choice(options) if random.random() < 0.6 else random.choice(values))
        try:
            completed = subprocess.run(argv, capture_output=True, timeout=15)
        except subprocess.TimeoutExpired:
            unexpected.append({"argv": argv[1:], "reason": "timeout"})
            continue
        if completed.returncode not in (0, 1, 2, 3, 4, 5):
            unexpected.append(
                {
                    "argv": argv[1:],
                    "exit": completed.returncode,
                    "stderr": completed.stderr.decode("utf-8", "replace")[-200:],
                }
            )
    report["random"] = {"cases": cases, "unexpected": unexpected}

    # ---- 2. 启动开销 ----
    startup = []
    for _ in range(200):
        completed, elapsed = timed_run([binary, "--version"])
        if completed.returncode != 0:
            raise SystemExit("--version 失败")
        startup.append(elapsed)
    report["startup"] = {
        "runs": len(startup),
        "mean_ms": round(statistics.mean(startup), 3),
        "p50_ms": round(percentile(startup, 0.50), 3),
        "p95_ms": round(percentile(startup, 0.95), 3),
        "p99_ms": round(percentile(startup, 0.99), 3),
    }

    # ---- 3. 假 server 上的 status 延迟 ----
    if fake_socket:
        env = dict(os.environ)
        env["GO2CPP_DEBUG_SOCKET"] = fake_socket
        latencies = []
        for _ in range(200):
            completed, elapsed = timed_run([binary, "status", "--pid", "12345"], env=env)
            if completed.returncode != 0:
                raise SystemExit(
                    "status 失败: "
                    + completed.stderr.decode("utf-8", "replace")[:300]
                )
            latencies.append(elapsed)
        report["status_latency"] = {
            "runs": len(latencies),
            "mean_ms": round(statistics.mean(latencies), 3),
            "p50_ms": round(percentile(latencies, 0.50), 3),
            "p95_ms": round(percentile(latencies, 0.95), 3),
            "p99_ms": round(percentile(latencies, 0.99), 3),
            "max_ms": round(max(latencies), 3),
        }

    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 1 if report["random"]["unexpected"] else 0


if __name__ == "__main__":
    sys.exit(main())
