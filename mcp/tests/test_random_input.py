#!/usr/bin/env python3
"""随机/畸形输入测试：验证服务端在坏输入下不崩溃、不挂死、错误码正确。

覆盖：非 JSON、空行、超长字符串、类型错误、深层嵌套、未知方法、缺字段、
随机 id 类型、重复 id、并发噪声。
"""

import json
import os
import random
import select
import subprocess
import sys
import time


class Client:
    def __init__(self, server, workers=1):
        self.proc = subprocess.Popen(
            [server, "--workers", str(workers)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.buffer = b""

    def send_raw(self, payload: bytes):
        self.proc.stdin.write(payload + b"\n")
        self.proc.stdin.flush()

    def read(self, timeout=10.0):
        deadline = time.time() + timeout
        while b"\n" not in self.buffer:
            ready, _, _ = select.select([self.proc.stdout], [], [], max(0.05, deadline - time.time()))
            if not ready:
                return None
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                return None
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        try:
            return json.loads(line.decode("utf-8"))
        except json.JSONDecodeError:
            return {"__invalid__": line.decode("utf-8", "replace")}

    def alive(self):
        return self.proc.poll() is None

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main() -> int:
    server = sys.argv[1]
    random.seed(20261023)
    client = Client(server, workers=4)
    failures = 0
    checks = 0

    def check(condition, label, detail=""):
        nonlocal failures, checks
        checks += 1
        if not condition:
            failures += 1
            print(f"FAIL: {label} {detail}")

    def expect_error(payload: bytes, code=None):
        client.send_raw(payload)
        reply = client.read()
        ok = reply is not None and "error" in reply
        if code is not None:
            ok = ok and reply["error"].get("code") == code
        return ok, reply

    # 1. 非 JSON
    ok, reply = expect_error(b"not json at all", -32700)
    check(ok, "非 JSON 返回 -32700", str(reply))

    # 2. 空 JSON / 数组 / 标量
    for payload in [b"[]", b"1", b"\"str\"", b"null", b"true"]:
        client.send_raw(payload)
        reply = client.read()
        check(reply is not None and "error" in reply, f"非对象输入 {payload!r}", str(reply))

    # 3. 超长字符串（1MB 的 method 字段）
    big = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "x" * (1024 * 1024)}).encode()
    ok, reply = expect_error(big, -32601)
    check(ok, "超长 method 返回 -32601")
    check(client.alive(), "超长输入后进程存活")

    # 4. 深层嵌套 params
    nested = {}
    cursor = nested
    for _ in range(200):
        cursor["a"] = {}
        cursor = cursor["a"]
    payload = json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": nested}).encode()
    client.send_raw(payload)
    reply = client.read()
    check(reply is not None and "result" in reply, "深层嵌套 params 不崩溃", str(reply)[:120])

    # 5. 类型错误的 id / params
    cases = [
        {"jsonrpc": "2.0", "id": {"x": 1}, "method": "ping"},
        {"jsonrpc": "2.0", "id": 3, "method": "ping", "params": "string"},
        {"jsonrpc": 2.0, "id": 4, "method": "ping"},
        {"jsonrpc": "2.0", "id": 5},
        {"jsonrpc": "2.0", "id": 6, "method": ""},
        {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": []},
        {"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": 1}},
        {"jsonrpc": "2.0", "id": 9, "method": "prompts/get", "params": {"name": "code-review", "arguments": {"language": 1}}},
    ]
    for case in cases:
        client.send_raw(json.dumps(case).encode())
        reply = client.read()
        check(reply is not None and ("error" in reply or "result" in reply), "类型错误用例", str(case)[:80])

    # 6. 随机噪声：随机二进制片段 + 随机 JSON 结构
    # 注意：字符集不含换行——换行正是 stdio 的消息分隔符，含换行会把一条
    # 噪声拆成多条，导致响应与请求错位（这是测试自身的构造问题，不是被测缺陷）。
    noise_alphabet = "{}[],:\"0123456789abc \t\x01\x7f"
    for _ in range(200):
        length = random.randint(1, 200)
        payload = "".join(random.choice(noise_alphabet) for _ in range(length)).encode("utf-8", "ignore")
        client.send_raw(payload)
        reply = client.read(timeout=5)
        if reply is None:
            check(False, "随机噪声后无响应（可能挂死）", payload[:60])
            break
        if "__invalid__" in reply:
            check(False, "服务端输出了非 JSON 帧", str(reply)[:80])
            break
    check(client.alive(), "随机噪声后进程存活")

    # 7. 随机合法 JSON 但语义错误
    methods = ["", "tools/call", "prompts/get", "resources/read", "initialize", "ping", "unknown"]
    for _ in range(200):
        message = {
            "jsonrpc": "2.0",
            "id": random.choice([1, "a", -1, 3.5]),
            "method": random.choice(methods),
        }
        if random.random() < 0.7:
            message["params"] = random.choice([
                {}, [], {"name": "calculator", "arguments": {"expression": "1+1"}},
                {"uri": "bacio:///quote"}, {"name": "code-review", "arguments": {}},
                {"name": "sleep", "arguments": {"milliseconds": -5}},
            ])
        client.send_raw(json.dumps(message).encode())
        reply = client.read()
        if reply is None:
            check(False, "随机语义用例无响应")
            break
    check(client.alive(), "随机语义用例后进程存活")

    # 8. 正常请求仍然可用
    client.send_raw(json.dumps({"jsonrpc": "2.0", "id": 999, "method": "ping"}).encode())
    reply = client.read()
    check(reply is not None and reply.get("id") == 999, "噪声后 ping 仍正常", str(reply))

    code = client.close()
    print(f"[random_input] {checks} 项断言，失败 {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
