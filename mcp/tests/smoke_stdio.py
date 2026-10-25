#!/usr/bin/env python3
"""stdio 冒烟测试：启动 mcp_server 子进程，喂固定请求，打印响应。

用途：快速验证协议帧是否可解析、方法是否连通。正式断言在 test_stdio_e2e.py。
"""

import json
import subprocess
import sys


def send_all(server_path: str, requests: list) -> list:
    process = subprocess.Popen(
        [server_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    payload = "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in requests)
    out, err = process.communicate(payload, timeout=30)
    if err.strip():
        print("[stderr]", err.strip(), file=sys.stderr)
    return [line for line in out.splitlines() if line.strip()]


def main() -> int:
    server = sys.argv[1]
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "calculator", "arguments": {"expression": "(1+2)*3.5"}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
         "params": {"name": "nope", "arguments": {}}},
        {"jsonrpc": "2.0", "id": 5, "method": "unknown/method"},
    ]
    for line in send_all(server, requests):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
