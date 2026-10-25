#!/usr/bin/env python3
"""插件主动通知端到端测试：stdio 与 HTTP/SSE。

用法: plugin_notify_e2e.py <mcp_server> <plugin_dir>

覆盖的真实语义：
- 插件在 tools/call 内调用 `NotificationSystem::SendToClient` → stdio 客户端收到
  `notifications/message` / `notifications/progress`；
- 插件从工作线程发通知 → 仍能到达 stdio 客户端（宿主写锁串行化）；
- 非法 JSON / 越权 method → 宿主拒绝，客户端收不到，服务进程存活；
- HTTP：两个 SSE 会话同时在线，插件通知**广播**给两个会话（会话 A 触发工具，
  A、B 都收到通知；工具响应只回 A）；
- HTTP：没有 SSE 会话时通知丢弃、POST 正常、服务存活。
"""

import json
import os
import select
import socket
import subprocess
import sys
import time
import urllib.parse
import urllib.request

CHECKS = 0
FAILURES = 0


def check(condition, label, detail=""):
    global CHECKS, FAILURES
    CHECKS += 1
    if not condition:
        FAILURES += 1
        print(f"FAIL: {label} {detail}")


class StdioClient:
    """stdio MCP 客户端：把无 id 的帧归到 notifications，响应按 id 匹配。"""

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
        self.notifications = []

    def _readline(self, timeout):
        while b"\n" not in self.buffer:
            ready, _, _ = select.select([self.proc.stdout], [], [], max(0.05, timeout))
            if not ready:
                raise TimeoutError("等待响应超时")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError("server 提前退出")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def request(self, method, params=None, timeout=20.0):
        self._id += 1
        message = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write((json.dumps(message) + "\n").encode())
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            reply = self._readline(max(0.05, deadline - time.time()))
            if "method" in reply and "id" not in reply:
                self.notifications.append(reply)
                continue
            if reply.get("id") == message["id"]:
                return reply
        raise TimeoutError(f"没有收到 {method} 的响应")

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        return self.proc.returncode, self.proc.stderr.read().decode("utf-8", "replace")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def post(port, payload, path="/mcp", timeout=15):
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, response.read().decode()


def read_sse(stream):
    """读一条 SSE 事件，返回 (event, data)；跳过心跳与注释。"""
    event = None
    data_lines = []
    while True:
        raw = stream.readline()
        if not raw:
            raise EOFError("SSE 流结束")
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if not line:
            if data_lines:
                return event or "message", "\n".join(data_lines)
            continue
        if line.startswith(":"):
            continue
        if line.startswith("event:"):
            event = line[len("event:"):].strip()
        elif line.startswith("data:"):
            data_lines.append(line[len("data:"):].lstrip())
        else:
            raise RuntimeError(f"无法解析的 SSE 行: {line!r}")


def open_sse(port, timeout=10):
    """建立 SSE 连接并读完 endpoint 事件，返回 (stream, session_id, endpoint)。"""
    stream = urllib.request.urlopen(f"http://127.0.0.1:{port}/sse", timeout=timeout)
    event, data = read_sse(stream)
    if event != "endpoint":
        raise RuntimeError(f"首个 SSE 事件不是 endpoint: {event} {data}")
    session_id = urllib.parse.parse_qs(urllib.parse.urlparse(data).query).get("sessionId", [""])[0]
    if not session_id:
        raise RuntimeError(f"endpoint 缺少 sessionId: {data}")
    return stream, session_id, data


def tools_call(request_id, name):
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": {}},
    }


def expect_no_notification(client, seconds=0.4):
    """确认接下来一段时间内没有新的通知帧（用于拒绝路径）。"""
    before = len(client.notifications)
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            reply = client._readline(0.05)
        except TimeoutError:
            continue
        if "method" in reply and "id" not in reply:
            client.notifications.append(reply)
    return len(client.notifications) == before


def run_stdio(server, plugin_dir):
    client = StdioClient(server, plugin_dir)
    try:
        hello = client.request("initialize", {"protocolVersion": "2025-06-18"})
        check(hello["result"]["protocolVersion"] == "2025-06-18", "stdio initialize")

        tools = client.request("tools/list")["result"]["tools"]
        names = {tool["name"] for tool in tools}
        check(
            {"notify_test", "notify_bad_json", "notify_forbidden", "notify_worker"} <= names,
            "假插件 4 个工具已注册",
            str(sorted(names)),
        )

        response = client.request("tools/call", {"name": "notify_test", "arguments": {}})
        check(response["result"]["isError"] is False, "notify_test 返回成功")
        check(len(client.notifications) == 2, "notify_test 发出 2 条通知",
              str(len(client.notifications)))
        if len(client.notifications) >= 2:
            message, progress = client.notifications[0], client.notifications[1]
            check(message["method"] == "notifications/message", "第 1 条是 message",
                  message.get("method", ""))
            check(message["params"].get("data") == "plugin says hi", "message 内容",
                  str(message["params"])[:120])
            check("id" not in message, "通知帧无 id")
            check(progress["method"] == "notifications/progress", "第 2 条是 progress",
                  progress.get("method", ""))
            check(progress["params"].get("progressToken") == "tok-1", "progressToken 原样转发")

        client.request("tools/call", {"name": "notify_worker", "arguments": {}})
        check(len(client.notifications) == 3, "工作线程通知到达客户端",
              str(len(client.notifications)))
        if len(client.notifications) >= 3:
            check(client.notifications[2]["params"].get("data") == "from worker thread",
                  "工作线程通知内容")

        client.request("tools/call", {"name": "notify_bad_json", "arguments": {}})
        client.request("tools/call", {"name": "notify_forbidden", "arguments": {}})
        check(expect_no_notification(client), "非法/越权通知未被转发")

        pong = client.request("ping")
        check(pong.get("result") == {}, "拒绝非法通知后服务仍存活")

        code, stderr = client.close()
        check(code == 0, "stdio 服务正常退出", f"code={code}")
        check("已加载 1 个插件" in stderr, "stdio 已加载假插件", stderr[:200])
    finally:
        if client.proc.poll() is None:
            client.proc.kill()


def run_http(server, plugin_dir):
    port = free_port()
    proc = subprocess.Popen(
        [server, "--http", str(port), "--plugins", plugin_dir, "-v"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.1)

        # 1) 没有任何 SSE 会话：POST /mcp 的通知被丢弃，服务不崩。
        status, body = post(port, tools_call(1, "notify_test"))
        check(status == 200, "无 SSE 会话时 tools/call 正常", str(status))
        check(json.loads(body)["result"]["isError"] is False, "无 SSE 会话时工具结果正常")
        status, body = post(port, {"jsonrpc": "2.0", "id": 2, "method": "ping"})
        check(status == 200 and json.loads(body).get("result") == {}, "无 SSE 会话后服务存活")

        # 2) 两个 SSE 会话同时在线：A 触发一次工具，通知应广播给 A 和 B。
        first, first_id, _ = open_sse(port)
        second, second_id, _ = open_sse(port)
        try:
            check(first_id != second_id, "两个 SSE 会话 id 不同")

            status, body = post(port, tools_call(10, "notify_test"),
                                f"/message?sessionId={urllib.parse.quote(first_id)}")
            check(status == 200 and body == "accepted", "会话 A 的 /message 返回 accepted",
                  f"{status} {body[:60]}")

            event, payload = read_sse(first)
            message = json.loads(payload)
            check(event == "message" and message.get("method") == "notifications/message",
                  "会话 A 收到插件 message 通知", str(message)[:120])
            check(message.get("params", {}).get("data") == "plugin says hi",
                  "会话 A 通知内容正确")

            event, payload = read_sse(first)
            check(json.loads(payload).get("method") == "notifications/progress",
                  "会话 A 收到插件 progress 通知", payload[:120])

            event, payload = read_sse(first)
            check(json.loads(payload).get("id") == 10, "工具响应只回发起会话 A", payload[:90])

            # B 没有发起任何请求，仍应收到广播的插件通知。
            event, payload = read_sse(second)
            message_b = json.loads(payload)
            check(message_b.get("method") == "notifications/message",
                  "会话 B 收到广播的插件 message", str(message_b)[:120])
            check(message_b.get("params", {}).get("data") == "plugin says hi",
                  "会话 B 广播通知内容正确")

            event, payload = read_sse(second)
            check(json.loads(payload).get("method") == "notifications/progress",
                  "会话 B 收到广播的插件 progress", payload[:120])
        finally:
            first.close()
            second.close()
    finally:
        proc.kill()
        stderr = proc.stderr.read()
        check("[plugin] fake-notify -> notifications/message" in stderr,
              "HTTP 插件通知进入宿主 sink", stderr[-300:])
        check("已加载 1 个插件" in stderr, "HTTP 已加载假插件", stderr[:200])


def main() -> int:
    if len(sys.argv) < 3:
        print("用法: plugin_notify_e2e.py <mcp_server> <plugin_dir>", file=sys.stderr)
        return 2
    server, plugin_dir = sys.argv[1], sys.argv[2]

    run_stdio(server, plugin_dir)
    run_http(server, plugin_dir)

    print(f"[plugin_notify_e2e] {CHECKS} 项断言，失败 {FAILURES}")
    return 0 if FAILURES == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
