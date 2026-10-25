#!/usr/bin/env python3
"""离线 agent 测试：假 OpenAI 端点 + 真实 mcp_agent 进程。

验证点：
1. agent 能把 tools/list 转成 OpenAI tools；
2. 模型返回 tool_calls 时，agent 真的执行了 MCP 工具；
3. 工具结果被回填为 role=tool 消息并进入下一轮；
4. 最终文本返回。
"""

import json
import os
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: test_agent_offline.py <mcp_agent> [python]", file=sys.stderr)
        return 2
    agent = sys.argv[1]
    python = sys.argv[2] if len(sys.argv) > 2 else "python3"

    fake = subprocess.Popen(
        [python, os.path.join(os.path.dirname(__file__), "fake_openai_server.py")],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        port_line = fake.stdout.readline().strip()
        if not port_line.isdigit():
            print("FAIL: 假服务端没有输出端口", file=sys.stderr)
            return 1
        port = int(port_line)

        env = dict(os.environ)
        env["DASHSCOPE_API_KEY"] = "sk-test-fake"
        env["MCP_RESOURCE_DIR"] = os.path.join(os.path.dirname(__file__), "..", "resources")
        completed = subprocess.run(
            [agent, "--base-url", f"http://127.0.0.1:{port}/v1", "--prompt", "算 sqrt(144) 和 1+2", "-v"],
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        failures = 0
        if completed.returncode != 0:
            print(f"FAIL: agent 退出码 {completed.returncode}\n{completed.stderr}", file=sys.stderr)
            failures += 1
        if "工具返回" not in completed.stdout:
            print(f"FAIL: 输出缺少工具结果回填\n{completed.stdout}", file=sys.stderr)
            failures += 1
        # 两个并行 tool_call 的结果都必须在最终回答里
        if "\"text\":\"12\"" not in completed.stdout:
            print(f"FAIL: 缺少 sqrt 结果 12\n{completed.stdout}", file=sys.stderr)
            failures += 1
        if "\"text\":\"3\"" not in completed.stdout:
            print(f"FAIL: 缺少 add 结果 3\n{completed.stdout}", file=sys.stderr)
            failures += 1
        if "\"isError\":false" not in completed.stdout and "isError" not in completed.stderr:
            print("WARN: 未在输出中观察到工具结果 JSON", file=sys.stderr)
        print(f"[agent_offline] stdout={completed.stdout.strip()[:200]}")
        print(f"[agent_offline] 失败 {failures}")
        return 0 if failures == 0 else 1
    finally:
        fake.kill()
        fake.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
