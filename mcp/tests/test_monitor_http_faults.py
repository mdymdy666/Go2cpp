#!/usr/bin/env python3
"""os_monitor_* HTTP 故障 e2e：真实 mcp_server（stdio）+ 假故障 HTTP 源。

覆盖（全部经过协议层 `tools/call`，验证失败不崩溃、错误可诊断）：
1. HTTP 500 → isError + 状态码 + 响应片段；
2. HTTP 404 → isError + 状态码 + 路径提示；
3. 超时（服务端挂起 1.2s，timeout_ms=100）→ isError + timeout 提示，调用快速返回；
4. 非 JSON 响应 → isError + 「不是合法 JSON」；
5. 截断响应（Content-Length 大于实际 body）→ isError，不静默成功；
6. 非 HTTP 垃圾字节 → isError；
7. 连接拒绝（Manager 未启动）→ isError + 「Manager 已启动」排查提示；
8. 每个故障之后：进程存活、ping 正常、tools/list 仍为 29 个工具。

用法: test_monitor_http_faults.py <mcp_server>
"""

import json
import os
import select
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SERVER = sys.argv[1] if len(sys.argv) > 1 else ""

FAILURES = 0
CHECKS = 0


def check(condition, label, detail=""):
    global FAILURES, CHECKS
    CHECKS += 1
    if not condition:
        FAILURES += 1
        print(f"FAIL: {label} {detail}")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class FaultHandler(BaseHTTPRequestHandler):
    """按路径前缀返回不同故障；写响应时忽略客户端已断开。"""

    protocol_version = "HTTP/1.1"

    def do_GET(self):  # noqa: N802（BaseHTTPRequestHandler 约定）
        path = urllib.parse.urlsplit(self.path).path
        try:
            if path.startswith("/s500"):
                self._respond(500, b"fault: internal error")
            elif path.startswith("/s404"):
                self._respond(404, b'{"ok":false,"error":"no route"}')
            elif path.startswith("/slow"):
                time.sleep(1.2)
                self._respond(200, b"[]")
            elif path.startswith("/bad"):
                self._respond(200, b"this is not json at all")
            elif path.startswith("/truncated"):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", "100")
                self.end_headers()
                self.wfile.write(b'{"snapshots":')
                self.wfile.flush()
                self.close_connection = True
            elif path.startswith("/garbage"):
                self.wfile.write(b"GARBAGE\r\n\r\n")
                self.wfile.flush()
                self.close_connection = True
            else:
                self._respond(200, b"[]")
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def _respond(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def log_message(self, *args):  # 静默，避免污染 ctest 输出
        pass


class QuietHTTPServer(ThreadingHTTPServer):
    """跳过 HTTPServer.server_bind 里的 getfqdn（WSL 下可能长时间阻塞）。"""

    daemon_threads = True

    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]


class FaultServer:
    def __init__(self):
        self.server = QuietHTTPServer(("127.0.0.1", 0), FaultHandler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(
            target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def base_url(self):
        return f"http://127.0.0.1:{self.port}"

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


class McpClient:
    """最小 stdio MCP 客户端（按行 JSON-RPC）。"""

    def __init__(self, server_path, cwd, env=None):
        merged_env = dict(os.environ)
        if env:
            merged_env.update(env)
        self._buffer = b""
        self._id = 0
        self.proc = subprocess.Popen(
            [server_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
            env=merged_env,
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
                continue  # 通知（本测试不关心）
            if reply.get("id") == message["id"]:
                return reply
        raise TimeoutError("没有收到匹配的响应")

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(
            (json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

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

    def stderr_text(self):
        try:
            return self.proc.stderr.read().decode("utf-8", errors="replace")
        except Exception:
            return ""


def tool_payload(reply):
    """取 tools/call 的 (is_error, text)；协议错误返回 (True, error.message)。"""
    if "error" in reply:
        return True, json.dumps(reply["error"], ensure_ascii=False)
    result = reply.get("result", {})
    content = result.get("content", [])
    text = content[0]["text"] if content else ""
    return bool(result.get("isError", False)), text


def call_monitor(client, url, timeout_ms=300, name="os_monitor_latest"):
    reply = client.request(
        "tools/call",
        {"name": name, "arguments": {"manager_url": url,
                                     "timeout_ms": timeout_ms}},
        timeout=15.0,
    )
    return tool_payload(reply)


def ping_alive(client):
    reply = client.request("ping", timeout=5.0)
    return "result" in reply


def main():
    if not SERVER:
        print("用法: test_monitor_http_faults.py <mcp_server>")
        return 2

    workdir = tempfile.mkdtemp(prefix="mcp_monitor_faults_")
    ini_path = os.path.join(workdir, "mcp.ini")
    # 默认地址指向一个刚释放的端口：即使某个调用忘记传 manager_url，也只会
    # 得到连接失败而不是打到宿主机 9101 上的真实服务。
    dead_port = free_port()
    with open(ini_path, "w", encoding="utf-8") as handle:
        handle.write(
            "[mcp]\n"
            f"monitor.base_url=http://127.0.0.1:{dead_port}\n"
            "monitor.timeout_ms=1000\n"
        )

    faults = FaultServer()
    print(f"[monitor_http_faults] 故障源已就绪: {faults.base_url}", flush=True)
    client = None
    exit_code = 0
    try:
        client = McpClient(SERVER, cwd=workdir,
                           env={"MCP_CONFIG_FILE": ini_path})
        print("[monitor_http_faults] MCP server 已启动，initialize...",
              flush=True)
        init = client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "monitor-faults", "version": "0.1.0"},
            },
        )
        check("result" in init, "initialize 成功", str(init)[:200])
        client.notify("notifications/initialized")
        print("[monitor_http_faults] 开始故障场景", flush=True)

        listed = client.request("tools/list")
        tool_names = {item["name"] for item in listed["result"]["tools"]}
        check(len(listed["result"]["tools"]) == 29,
              "tools/list 共 29 个工具",
              str(len(listed["result"]["tools"])))
        for name in ("os_monitor_latest", "os_monitor_stats",
                     "os_monitor_kernel", "os_monitor_health"):
            check(name in tool_names, f"tools/list 包含 {name}")

        # 1) HTTP 500
        is_error, text = call_monitor(client, faults.base_url + "/s500")
        check(is_error, "HTTP 500 → isError", text[:120])
        check("Manager 返回 HTTP 500" in text, "HTTP 500 状态码可诊断", text[:200])
        check("fault: internal error" in text, "HTTP 500 响应片段", text[:200])
        check(faults.base_url + "/s500" in text, "HTTP 500 消息含 URL", text[:200])
        check(ping_alive(client), "HTTP 500 之后进程存活")
        check(len(client.request("tools/list")["result"]["tools"]) == 29,
              "HTTP 500 之后工具表完整")

        # 2) HTTP 404
        is_error, text = call_monitor(client, faults.base_url + "/s404")
        check(is_error, "HTTP 404 → isError", text[:120])
        check("Manager 返回 HTTP 404" in text, "HTTP 404 状态码可诊断", text[:200])
        check("路径不存在" in text, "HTTP 404 路径提示", text[:200])
        check("no route" in text, "HTTP 404 响应片段", text[:200])

        # 3) 超时：100ms 超时 + 服务端 1.2s 挂起
        started = time.monotonic()
        is_error, text = call_monitor(client, faults.base_url + "/slow",
                                      timeout_ms=100)
        elapsed = time.monotonic() - started
        check(is_error, "超时 → isError", text[:120])
        check("未响应" in text, "超时消息说明未响应", text[:200])
        check("100ms" in text and "timeout_ms" in text,
              "超时消息给出超时值与调整建议", text[:200])
        check(elapsed < 3.0, "超时请求快速返回（不等到服务端 1.2s）",
              f"{elapsed:.2f}s")
        check(ping_alive(client), "超时之后进程存活")

        # 4) 非 JSON
        is_error, text = call_monitor(client, faults.base_url + "/bad")
        check(is_error, "非 JSON → isError", text[:120])
        check("不是合法 JSON" in text, "非 JSON 诊断", text[:200])

        # 5) 截断响应
        is_error, text = call_monitor(client, faults.base_url + "/truncated")
        check(is_error, "截断响应 → isError", text[:120])
        check("无法访问 Manager" in text or "响应无法解析" in text,
              "截断响应诊断", text[:200])
        check(ping_alive(client), "截断响应之后进程存活")

        # 6) 非 HTTP 垃圾字节
        is_error, text = call_monitor(client, faults.base_url + "/garbage")
        check(is_error, "垃圾字节 → isError", text[:120])
        check("无法访问 Manager" in text or "响应无法解析" in text,
              "垃圾字节诊断", text[:200])

        # 7) 连接拒绝（Manager 未启动）
        refused_port = free_port()
        is_error, text = call_monitor(
            client, f"http://127.0.0.1:{refused_port}")
        check(is_error, "连接拒绝 → isError", text[:120])
        check("无法访问 Manager" in text, "连接拒绝诊断", text[:200])
        check("Manager 已启动" in text, "连接拒绝给出启动排查提示", text[:200])
        check(f"http://127.0.0.1:{refused_port}" in text,
              "连接拒绝消息含 URL", text[:200])

        # 8) 等价路径：Manager 未启动时其余三个工具也要失败但不崩溃。
        for name in ("os_monitor_stats", "os_monitor_kernel",
                     "os_monitor_health"):
            is_error, text = call_monitor(
                client, f"http://127.0.0.1:{refused_port}", name=name)
            check(is_error, f"{name} 连接拒绝 → isError", text[:120])
            check("Manager 已启动" in text, f"{name} 连接拒绝排查提示",
                  text[:200])

        # 9) 故障之后工具表与 ping 仍然正常。
        check(ping_alive(client), "全部故障之后进程存活")
        final_tools = client.request("tools/list")["result"]["tools"]
        check(len(final_tools) == 29, "全部故障之后工具表完整",
              str(len(final_tools)))
    finally:
        faults.stop()
        if client is not None:
            # 先关闭 stdin 让 server 退出，再读 stderr；否则 stderr.read()
            # 会阻塞在仍在运行的 server 上。
            exit_code = client.close()
            stderr = client.stderr_text()
            if exit_code not in (0, None):
                check(False, "mcp_server 退出码应为 0", str(exit_code))
            if "terminate called" in stderr or "Segmentation fault" in stderr:
                check(False, "mcp_server 不应崩溃", stderr[:300])

    print(f"[monitor_http_faults] {CHECKS} 项断言，失败 {FAILURES}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
