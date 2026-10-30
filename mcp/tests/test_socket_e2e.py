#!/usr/bin/env python3
"""socket_* 工具端到端测试：真实 mcp_server（stdio）+ 真实审计文件。

覆盖：
1. `tools/list` 出现 4 个工具，schema 暴露 fd / n / reason；
2. `socket_list` 返回 JSON（mcp_server 自身没有业务 socket，sockets 为数组）；
3. 未登记 / 非 socket fd：`socket_stats`、`socket_events`、`socket_close` 返回
   isError，进程不崩溃；
4. 协议层参数校验：缺 fd、fd 类型错误、n 越界返回 -32602；
5. `socket_close(fd=0)` 被白名单拒绝，并在临时工作目录写出审计 JSON Lines；
6. 审计记录包含 action/fd/result=denied，且进程正常退出。

用法: test_socket_e2e.py <mcp_server>
"""

import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time

SERVER = sys.argv[1] if len(sys.argv) > 1 else ""

FAILURES = 0
CHECKS = 0


def check(condition, label, detail=""):
    global FAILURES, CHECKS
    CHECKS += 1
    if not condition:
        FAILURES += 1
        print(f"FAIL: {label} {detail}")


class McpClient:
    """最小 stdio MCP 客户端（按行 JSON-RPC）。"""

    def __init__(self, server_path, cwd):
        self._buffer = b""
        self._id = 0
        self.proc = subprocess.Popen(
            [server_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
            bufsize=0,
        )

    def _readline(self, timeout):
        while b"\n" not in self._buffer:
            ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
            if not ready:
                raise TimeoutError("等待 mcp_server 响应超时")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError("mcp_server 提前退出")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def request(self, method, params=None, timeout=20.0):
        self._id += 1
        message = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(
            (json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            reply = self._readline(max(0.1, deadline - time.time()))
            if "method" in reply and "id" not in reply:
                continue
            if reply.get("id") == message["id"]:
                return reply
        raise TimeoutError("没有收到匹配的响应")

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            return self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait(timeout=5)


def tool_payload(reply):
    """取 tools/call 的 (is_error, text)；协议错误返回 (True, error.message)。"""
    if "error" in reply:
        return True, json.dumps(reply["error"], ensure_ascii=False)
    result = reply.get("result", {})
    content = result.get("content", [])
    text = content[0]["text"] if content else ""
    return bool(result.get("isError", False)), text


def safe_json(text):
    try:
        return json.loads(text)
    except (TypeError, ValueError):
        return {}


def call_tool(client, name, arguments, timeout=20.0):
    return client.request(
        "tools/call",
        {"name": name, "arguments": arguments},
        timeout=timeout,
    )


def check_protocol_error(reply, label):
    check("error" in reply, label + " 应返回协议错误")
    if "error" in reply:
        check(reply["error"].get("code") == -32602,
              label + " 错误码应为 -32602",
              str(reply["error"]))


def main():
    if not SERVER:
        print("用法: test_socket_e2e.py <mcp_server>")
        return 2

    workdir = tempfile.mkdtemp(prefix="mcp_socket_e2e_")
    client = None
    try:
        client = McpClient(SERVER, workdir)
        init = client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "socket-e2e", "version": "0.1.0"},
            },
        )
        check("result" in init, "initialize 成功", str(init)[:200])

        listed = client.request("tools/list")
        tools = {item["name"]: item for item in listed.get("result", {}).get("tools", [])}
        for name in ("socket_list", "socket_stats", "socket_events", "socket_close"):
            check(name in tools, "tools/list 应包含 " + name)
        if "socket_stats" in tools:
            schema = tools["socket_stats"].get("inputSchema", {})
            check("fd" in schema.get("properties", {}),
                  "socket_stats schema 应声明 fd")
            check(schema.get("required") == ["fd"], "socket_stats 应要求 fd")
        if "socket_events" in tools:
            schema = tools["socket_events"].get("inputSchema", {})
            check(schema.get("properties", {}).get("n", {}).get("maximum") == 256,
                  "socket_events n 上限应为 256")

        # 1) list：mcp_server 没有业务 socket，返回合法空/数组结构。
        is_error, text = tool_payload(
            call_tool(client, "socket_list", {}))
        check(not is_error, "socket_list 应成功", text[:200])
        payload = safe_json(text)
        check(isinstance(payload.get("sockets"), list), "socket_list.sockets 应为数组")
        check(payload.get("count") == len(payload.get("sockets", [])),
              "socket_list.count 应与数组长度一致")

        # 2) 非法 fd：stats/events/close 都返回 isError 且进程存活。
        is_error, text = tool_payload(
            call_tool(client, "socket_stats", {"fd": 999999}))
        check(is_error, "socket_stats 非法 fd 应 isError", text[:200])
        is_error, text = tool_payload(
            call_tool(client, "socket_events", {"fd": 999999}))
        check(is_error, "socket_events 非法 fd 应 isError", text[:200])

        # 3) 白名单：fd 0 不是登记 socket，拒绝关闭且写审计。
        is_error, text = tool_payload(
            call_tool(client, "socket_close", {"fd": 0, "reason": "e2e-denied"}))
        check(is_error, "socket_close(fd=0) 应被拒绝", text[:200])
        denied = safe_json(text)
        check(denied.get("closed") is False, "拒绝时 closed=false")
        check("未登记" in denied.get("error", ""), "拒绝原因应提到未登记",
              str(denied.get("error", ""))[:200])
        check(denied.get("audit", {}).get("result") == "denied",
              "拒绝结果应带 audit.result=denied")

        # 4) 协议层参数校验（-32602）。
        check_protocol_error(
            call_tool(client, "socket_stats", {}), "socket_stats 缺 fd")
        check_protocol_error(
            call_tool(client, "socket_stats", {"fd": "0"}), "socket_stats fd 类型")
        check_protocol_error(
            call_tool(client, "socket_events", {"fd": 0, "n": 0}),
            "socket_events n=0")
        check_protocol_error(
            call_tool(client, "socket_events", {"fd": 0, "n": 257}),
            "socket_events n=257")
        check_protocol_error(
            call_tool(client, "socket_close", {"fd": 0, "reason": "x" * 201}),
            "socket_close reason 超长")

        # 5) 审计文件：工作目录/log/mcp-socket-audit.log。
        audit_path = os.path.join(workdir, "log", "mcp-socket-audit.log")
        check(os.path.isfile(audit_path), "审计文件应存在", audit_path)
        records = []
        if os.path.isfile(audit_path):
            with open(audit_path, "r", encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        records.append(json.loads(line))
                    except ValueError:
                        check(False, "审计行应为合法 JSON", line[:200])
        check(len(records) >= 1, "审计至少 1 条记录", str(len(records)))
        check(any(record.get("action") == "socket_close"
                  and record.get("result") == "denied"
                  and record.get("fd") == 0 for record in records),
              "审计应记录 fd=0 的 socket_close 拒绝")
        check(any(record.get("reason") == "e2e-denied" for record in records),
              "审计应包含关闭原因")
        check(all("ts" in record and "pid" in record for record in records),
              "每条审计应含 ts/pid")

        # 6) 服务仍可用，正常退出。
        listed_again = client.request("tools/list")
        check("result" in listed_again, "拒绝后 server 仍能响应 tools/list")
        exit_code = client.close()
        client = None
        check(exit_code == 0, "mcp_server 正常退出", f"exit={exit_code}")
    except Exception as error:  # noqa: BLE001 - e2e 需要打印诊断后继续清理
        check(False, "e2e 异常", repr(error))
    finally:
        if client is not None:
            client.close()
        shutil.rmtree(workdir, ignore_errors=True)

    print(f"[socket_e2e] {CHECKS} 项断言，失败 {FAILURES}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
