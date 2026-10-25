#!/usr/bin/env python3
"""插件加载测试：直接加载 mcp-extened 编译出的 .so 插件，验证 C ABI 适配。

用法: test_plugin_e2e.py <mcp_server> <plugin_dir>
"""

import json
import os
import select
import subprocess
import sys
import time


class Client:
    def __init__(self, server, plugin_dir):
        self.proc = subprocess.Popen(
            [server, "--plugins", plugin_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self.buffer = b""
        self._id = 0

    def request(self, method, params=None, timeout=20.0):
        self._id += 1
        message = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write((json.dumps(message) + "\n").encode())
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while b"\n" not in self.buffer:
            ready, _, _ = select.select([self.proc.stdout], [], [], max(0.1, deadline - time.time()))
            if not ready:
                raise TimeoutError("等待响应超时")
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
        return self.proc.stderr.read().decode("utf-8", "replace")


def main() -> int:
    server, plugin_dir = sys.argv[1], sys.argv[2]
    client = Client(server, plugin_dir)
    failures = 0
    checks = 0

    def check(condition, label, detail=""):
        nonlocal failures, checks
        checks += 1
        if not condition:
            failures += 1
            print(f"FAIL: {label} {detail}")

    try:
        tools = client.request("tools/list")["result"]["tools"]
        names = {item["name"] for item in tools}
        check("get_weather" in names, "加载 weather 插件的 get_weather", str(sorted(names)[:8]))
        check("logging_test" in names, "加载 notification 插件")
        check("calculator" in names, "内置工具仍在")

        sleep = client.request("tools/call", {"name": "sleep", "arguments": {"milliseconds": 20}})
        check(sleep["result"]["isError"] is False, "插件 sleep 调用", str(sleep)[:120])

        prompts = client.request("prompts/list")["result"]["prompts"]
        check(any(item["name"] == "code-review" for item in prompts), "加载 code-review 提示")

        resources = client.request("resources/list")["result"]["resources"]
        uris = {item["uri"] for item in resources}
        check("bacio:///quote" in uris, "加载 bacio 资源", str(uris))
        check("document:///life-choices.txt" in uris, "加载 document 资源")

        quote = client.request("resources/read", {"uri": "bacio:///quote"})
        check("contents" in quote.get("result", {}), "读取插件资源", str(quote)[:120])
    finally:
        stderr = client.close()
        if "已加载" not in stderr:
            print(f"[server stderr] {stderr[:300]}", file=sys.stderr)

    print(f"[plugin_e2e] {checks} 项断言，失败 {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
