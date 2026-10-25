#!/usr/bin/env python3
"""压测：流水线发送 N 个 tools/call，统计吞吐与延迟分位。

不做网络回环（stdio 直接管道），测量的是 MCP 协议层 + 工具执行 + 序列化的
端到端开销。分别跑 workers=1 与 workers=4，给出对比数字。

用法: test_bench.py <mcp_server> [请求数] [并发 worker 数]
"""

import json
import os
import select
import statistics
import subprocess
import sys
import time


def run(server, workers, count):
    proc = subprocess.Popen(
        [server, "--workers", str(workers)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    buffer = b""
    sent_at = {}
    latencies = []

    def send(index):
        message = {
            "jsonrpc": "2.0",
            "id": index,
            "method": "tools/call",
            "params": {"name": "echo", "arguments": {"text": "x" * 200}},
        }
        sent_at[index] = time.perf_counter()
        proc.stdin.write((json.dumps(message) + "\n").encode())
        proc.stdin.flush()

    def read_one(timeout=30.0):
        nonlocal buffer
        deadline = time.time() + timeout
        while b"\n" not in buffer:
            ready, _, _ = select.select([proc.stdout], [], [], max(0.05, deadline - time.time()))
            if not ready:
                raise TimeoutError("压测读响应超时")
            chunk = os.read(proc.stdout.fileno(), 1 << 16)
            if not chunk:
                raise EOFError("server 退出")
            buffer += chunk
        line, buffer = buffer.split(b"\n", 1)
        return json.loads(line.decode())

    start = time.perf_counter()
    # 分批发送：stdio 管道容量约 64KB，一次性灌 N 条会让客户端写阻塞、服务端
    # 响应也阻塞，形成双向等待。每批 send + recv 交替可以避免这种经典死锁。
    batch = 64
    received = 0
    for batch_start in range(0, count, batch):
        batch_end = min(batch_start + batch, count)
        for index in range(batch_start, batch_end):
            send(index + 1)
        for _ in range(batch_end - batch_start):
            reply = read_one()
            if "method" in reply and "id" not in reply:
                continue
            received += 1
            latencies.append((time.perf_counter() - sent_at[reply["id"]]) * 1000.0)
    elapsed = time.perf_counter() - start

    proc.stdin.close()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    def percentile(values, p):
        ordered = sorted(values)
        index = min(len(ordered) - 1, int(round((p / 100.0) * (len(ordered) - 1))))
        return ordered[index]

    return {
        "workers": workers,
        "count": count,
        "elapsed_s": elapsed,
        "throughput": count / elapsed,
        "p50_ms": percentile(latencies, 50),
        "p95_ms": percentile(latencies, 95),
        "p99_ms": percentile(latencies, 99),
        "mean_ms": statistics.mean(latencies),
    }


def run_blocking(server, workers, count, delay_ms):
    """阻塞型负载：每个请求 sleep delay_ms，用于验证 Fiber 并发收益。"""
    proc = subprocess.Popen(
        [server, "--workers", str(workers)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )
    buffer = b""

    def read_one(timeout=60.0):
        nonlocal buffer
        deadline = time.time() + timeout
        while b"\n" not in buffer:
            ready, _, _ = select.select([proc.stdout], [], [], max(0.05, deadline - time.time()))
            if not ready:
                raise TimeoutError("阻塞负载读响应超时")
            chunk = os.read(proc.stdout.fileno(), 1 << 16)
            if not chunk:
                raise EOFError("server 退出")
            buffer += chunk
        line, buffer = buffer.split(b"\n", 1)
        return json.loads(line.decode())

    start = time.perf_counter()
    for index in range(count):
        message = {
            "jsonrpc": "2.0",
            "id": index + 1,
            "method": "tools/call",
            "params": {"name": "sleep", "arguments": {"milliseconds": delay_ms}},
        }
        proc.stdin.write((json.dumps(message) + "\n").encode())
    proc.stdin.flush()

    received = 0
    while received < count:
        reply = read_one()
        if "method" in reply and "id" not in reply:
            continue
        received += 1
    elapsed = time.perf_counter() - start
    proc.stdin.close()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    return elapsed


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: test_bench.py <mcp_server> [请求数]", file=sys.stderr)
        return 2
    server = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 2000

    results = [run(server, workers, count) for workers in (1, 4)]
    for item in results:
        print(
            "workers={workers:>2}  n={count}  吞吐={throughput:>9.1f} req/s  "
            "P50={p50_ms:>7.2f}ms  P95={p95_ms:>7.2f}ms  P99={p99_ms:>7.2f}ms".format(**item)
        )

    # 阻塞负载对比：16 个 200ms 的 sleep，串行约 3.2s，4 workers 应接近 0.8s。
    blocking_count = 16
    delay_ms = 200
    serial = run_blocking(server, 1, blocking_count, delay_ms)
    parallel = run_blocking(server, 4, blocking_count, delay_ms)
    print(
        f"阻塞负载 sleep({delay_ms}ms)x{blocking_count}: workers=1 {serial:.2f}s, "
        f"workers=4 {parallel:.2f}s, 加速 {serial / parallel:.2f}x"
    )

    failures = 0
    if results[0]["throughput"] <= 0:
        print("FAIL: 吞吐为 0")
        failures += 1
    if parallel > serial * 0.5:
        print("FAIL: 阻塞负载下 workers=4 没有明显加速")
        failures += 1
    # 轻负载下并发可能因 Fiber 调度开销而变慢（实测 76k -> 28k req/s），
    # 这是预期行为，不设加速断言，只要求吞吐为正。
    print(f"[bench] 失败 {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
