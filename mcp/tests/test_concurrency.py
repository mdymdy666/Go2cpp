#!/usr/bin/env python3
"""并发处理测试：验证 --workers > 1 时请求被并行执行到 Go2Cpp 调度器上。

对比同一组 sleep 请求在 workers=1 与 workers=4 下的墙钟耗时，并校验每条响应
都回填了正确的 id（并发下响应可能乱序）。
"""

import json
import os
import select
import subprocess
import sys
import time


class Client:
    def __init__(self, server, workers):
        self.proc = subprocess.Popen(
            [server, "--workers", str(workers)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.buffer = b""

    def send(self, message):
        self.proc.stdin.write((json.dumps(message) + "\n").encode())
        self.proc.stdin.flush()

    def read(self, timeout=30.0):
        deadline = time.time() + timeout
        while b"\n" not in self.buffer:
            ready, _, _ = select.select([self.proc.stdout], [], [], max(0.1, deadline - time.time()))
            if not ready:
                raise TimeoutError("读响应超时")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError("server 退出")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode())

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def measure(server, workers, count=4, delay_ms=400):
    client = Client(server, workers)
    try:
        start = time.time()
        for index in range(count):
            client.send({
                "jsonrpc": "2.0",
                "id": index + 1,
                "method": "tools/call",
                "params": {"name": "sleep", "arguments": {"milliseconds": delay_ms}},
            })
        seen = set()
        while len(seen) < count:
            reply = client.read()
            if "method" in reply and "id" not in reply:
                continue
            seen.add(reply["id"])
            assert reply["result"]["isError"] is False, reply
        elapsed = time.time() - start
        return elapsed, seen
    finally:
        client.close()


def main() -> int:
    server = sys.argv[1]
    sequential, ids1 = measure(server, 1)
    parallel, ids4 = measure(server, 4)
    print(f"workers=1: {sequential:.2f}s  workers=4: {parallel:.2f}s  ids={sorted(ids4)}")

    failures = 0
    # 顺序应接近 4*0.4=1.6s；并发应接近 0.4s（留足调度/启动余量）。
    if sequential < 1.2:
        print("FAIL: workers=1 没有体现出串行耗时")
        failures += 1
    if parallel > sequential * 0.6:
        print("FAIL: workers=4 没有明显并行加速")
        failures += 1
    if ids4 != {1, 2, 3, 4}:
        print("FAIL: 并发响应 id 不完整")
        failures += 1
    print(f"[concurrency] 失败 {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
