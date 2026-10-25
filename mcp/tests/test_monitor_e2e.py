#!/usr/bin/env python3
"""os_monitor_* 工具端到端测试：真实 Manager + 真实 mcp_server（stdio）。

覆盖：
1. Manager 绑定内核分配端口，测试用真实 `POST /api/push` 灌两条伪快照；
2. mcp_server 通过临时 mcp.ini 的 `mcp.monitor.base_url` 找到该 Manager；
3. `tools/list` 能看到 4 个工具及参数 schema（manager_url / limit / timeout_ms）；
4. 四个工具返回预期 JSON：latest 快照、stats 健康评分、kernel 五类指标、health 计数；
5. `manager_url` 参数覆盖配置、`limit` 生效；
6. schema 校验在协议层拒绝非法 manager_url（-32602）；
7. Manager 停止后工具返回 isError + 排查提示，server 不崩溃且仍能响应 tools/list。

用法: test_monitor_e2e.py <mcp_server> <fake_monitor_manager>
"""

import json
import os
import select
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

SERVER = sys.argv[1] if len(sys.argv) > 1 else ""
MANAGER = sys.argv[2] if len(sys.argv) > 2 else ""

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


def post_json(port, path, payload, timeout=10):
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, response.read().decode("utf-8")


def snapshot(host, usage_percent, timestamp_ms):
    """构造与 go2cpp::os::monitor::Snapshot 对齐的最小快照。"""
    return {
        "host": host,
        "timestamp_ms": timestamp_ms,
        "host_info": {"hostname": host, "os": "linux", "kernel": "e2e", "arch": "x86_64"},
        "load": {"load1": 1.5, "load5": 1.2, "load15": 0.9,
                 "runnable": 2, "total_threads": 128},
        "cpus": [
            {"name": "cpu0", "usage_percent": usage_percent},
            {"name": "cpu1", "usage_percent": usage_percent},
        ],
        "mem": {"total_kb": 1048576, "available_kb": 786432, "used_percent": 25.0},
        "disks": [{"name": "sda", "util_percent": 12.5}],
        "nets": [{"name": "eth0", "rcv_rate": 1024.0, "snd_rate": 2048.0}],
        "ebpf_active": False,
        "ebpf_reason": "e2e: no eBPF",
    }


class ManagerProcess:
    """真实 Manager 子进程：端口从 stdout 的 PORT=<n> 读取。"""

    def __init__(self):
        self.proc = subprocess.Popen(
            [MANAGER],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        line = self.proc.stdout.readline().strip()
        if not line.startswith("PORT="):
            stderr = self.proc.stderr.read()
            raise RuntimeError(f"Manager 未报告端口: {line!r} / {stderr!r}")
        self.port = int(line.split("=", 1)[1])
        self.base_url = f"http://127.0.0.1:{self.port}"

    def push(self, payload):
        return post_json(self.port, "/api/push", payload)

    def stop(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)


class McpClient:
    """最小 stdio MCP 客户端（按行 JSON-RPC）。"""

    def __init__(self, server_path, cwd, env=None):
        merged_env = dict(os.environ)
        if env:
            merged_env.update(env)
        self._buffer = b""
        self._id = 0
        self.notifications = []
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
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            reply = self._readline(max(0.1, deadline - time.time()))
            if "method" in reply and "id" not in reply:
                self.notifications.append(reply)
                continue
            if reply.get("id") == message["id"]:
                return reply
        raise TimeoutError("没有收到匹配的响应")

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
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


def safe_json(text):
    """解析工具返回的 JSON；失败返回空对象，让后续断言继续而不是中断。"""
    try:
        return json.loads(text)
    except (TypeError, ValueError):
        return {}


def tool_payload(reply):
    """取 tools/call 的 (is_error, text)；协议错误返回 (True, error.message)。"""
    if "error" in reply:
        return True, json.dumps(reply["error"], ensure_ascii=False)
    result = reply.get("result", {})
    content = result.get("content", [])
    text = content[0]["text"] if content else ""
    return bool(result.get("isError", False)), text


def main():
    if not SERVER or not MANAGER:
        print("用法: test_monitor_e2e.py <mcp_server> <fake_monitor_manager>")
        return 2

    workdir = tempfile.mkdtemp(prefix="mcp_monitor_e2e_")
    ini_path = os.path.join(workdir, "mcp.ini")
    manager = None
    client = None

    try:
        # 1. 真实 Manager + 两条伪快照（走真实 POST /api/push）。
        manager = ManagerProcess()
        now_ms = int(time.time() * 1000)
        status, _ = manager.push(snapshot("e2e-monitor-a", 20.0, now_ms))
        check(status == 200, "POST /api/push 第一条返回 200", f"status={status}")
        status, _ = manager.push(snapshot("e2e-monitor-b", 60.0, now_ms + 10))
        check(status == 200, "POST /api/push 第二条返回 200", f"status={status}")

        # 2. 临时 ini：mcp.monitor.base_url 指向该 Manager。
        with open(ini_path, "w", encoding="utf-8") as handle:
            handle.write(
                "[mcp]\n"
                f"monitor.base_url={manager.base_url}\n"
                "monitor.timeout_ms=3000\n"
            )

        # 3. 启动真实 mcp_server（stdio），走应用层装配与配置加载。
        client = McpClient(SERVER, cwd=workdir, env={"MCP_CONFIG_FILE": ini_path})
        init = client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "monitor-e2e", "version": "0.1.0"},
            },
        )
        check("result" in init, "initialize 成功", str(init)[:200])
        client.notify("notifications/initialized")

        # 4. tools/list：四个工具与 schema 参数。
        listed = client.request("tools/list")
        tools = {item["name"]: item for item in listed["result"]["tools"]}
        for name in ("os_monitor_latest", "os_monitor_stats", "os_monitor_kernel",
                     "os_monitor_health"):
            check(name in tools, f"tools/list 包含 {name}")
        latest_schema = tools.get("os_monitor_latest", {}).get("inputSchema", {})
        properties = latest_schema.get("properties", {})
        for key in ("manager_url", "limit", "timeout_ms"):
            check(key in properties, f"os_monitor_latest schema 有 {key}")
        health_schema = tools.get("os_monitor_health", {}).get("inputSchema", {})
        check("limit" not in health_schema.get("properties", {}),
              "os_monitor_health 不接受 limit")

        # 5. latest：配置里的 base_url 生效，返回两条快照。
        is_error, text = tool_payload(
            client.request("tools/call", {"name": "os_monitor_latest", "arguments": {"limit": 10}})
        )
        check(not is_error, "os_monitor_latest 成功", text[:300])
        payload = safe_json(text)
        check(payload.get("count") == 2, "latest 返回两条快照", str(payload.get("count")))
        check(payload.get("snapshots", [{}])[0].get("host") == "e2e-monitor-a",
              "latest 第一条 host 正确", str(payload.get("snapshots", [{}])[0].get("host")))
        check(len(payload.get("snapshots", [{}])[0].get("cpus", [])) == 2,
              "latest 快照 cpus 字段透传")
        check(payload.get("snapshots", [{}])[0].get("mem", {}).get("used_percent") == 25.0,
              "latest 快照 mem.used_percent 透传")
        if os.environ.get("MCP_MONITOR_E2E_DUMP"):
            print("[dump] os_monitor_latest ->",
                  json.dumps(payload, ensure_ascii=False)[:1500])

        # 6. limit=1 生效。
        is_error, text = tool_payload(
            client.request("tools/call", {"name": "os_monitor_latest", "arguments": {"limit": 1}})
        )
        payload = safe_json(text)
        check(not is_error and payload.get("count") == 1, "latest limit=1 只返回一条",
              str(payload.get("count")))

        # 7. stats：健康评分与 CPU 百分比，按分数降序。
        is_error, text = tool_payload(
            client.request("tools/call", {"name": "os_monitor_stats", "arguments": {}})
        )
        check(not is_error, "os_monitor_stats 成功", text[:300])
        payload = safe_json(text)
        hosts = payload.get("hosts", [])
        check(payload.get("count") == 2 and len(hosts) == 2, "stats 返回两台主机")
        if len(hosts) == 2:
            check(hosts[0].get("host") == "e2e-monitor-a", "stats 按健康分数降序",
                  str(hosts[0].get("host")))
            check(0 <= hosts[0].get("score", -1) <= 100, "stats score 在 0-100",
                  str(hosts[0].get("score")))
            check(abs(hosts[0].get("cpu_percent", 0) - 20.0) < 0.01,
                  "stats cpu_percent=20", str(hosts[0].get("cpu_percent")))

        # 8. kernel：五类指标对象。
        is_error, text = tool_payload(
            client.request("tools/call", {"name": "os_monitor_kernel", "arguments": {}})
        )
        check(not is_error, "os_monitor_kernel 成功", text[:300])
        payload = safe_json(text)
        host = payload.get("hosts", [{}])[0]
        for key in ("syscall", "sched", "futex", "tcp", "fileio"):
            check(key in host and isinstance(host[key], dict), f"kernel 返回 {key} 对象")

        # 9. health：Manager 自身计数。
        is_error, text = tool_payload(
            client.request("tools/call", {"name": "os_monitor_health", "arguments": {}})
        )
        check(not is_error, "os_monitor_health 成功", text[:300])
        payload = safe_json(text)
        health = payload.get("health", {})
        check(health.get("status") == "ok", "health.status=ok", str(health))
        check(health.get("received") == 2, "health.received=2", str(health.get("received")))
        if os.environ.get("MCP_MONITOR_E2E_DUMP"):
            print("[dump] os_monitor_health ->",
                  json.dumps(payload, ensure_ascii=False)[:600])

        # 10. manager_url 参数覆盖配置：指向未监听端口。
        dead_port = free_port()
        is_error, text = tool_payload(
            client.request(
                "tools/call",
                {
                    "name": "os_monitor_health",
                    "arguments": {"manager_url": f"http://127.0.0.1:{dead_port}"},
                },
            )
        )
        check(is_error and "无法访问 Manager" in text,
              "manager_url 覆盖：未监听端口返回明确错误", text[:300])

        # 11. 协议层 schema 校验：非法 scheme 得到 -32602。
        reply = client.request(
            "tools/call",
            {"name": "os_monitor_latest", "arguments": {"manager_url": "ftp://127.0.0.1:1"}},
        )
        check(reply.get("error", {}).get("code") == -32602,
              "非法 manager_url 被 schema 拒绝（-32602）", str(reply)[:300])

        # 12. Manager 停止：工具报错但不崩，server 继续响应 tools/list。
        manager.stop()
        manager = None
        time.sleep(0.1)
        is_error, text = tool_payload(
            client.request("tools/call", {"name": "os_monitor_latest", "arguments": {}})
        )
        check(is_error and "无法访问 Manager" in text,
              "Manager 停止后工具返回明确错误", text[:300])
        check("Manager 已启动" in text, "错误带部署排查提示", text[:300])
        if os.environ.get("MCP_MONITOR_E2E_DUMP"):
            print("[dump] Manager 停止后 ->", text[:500])
        listed_after = client.request("tools/list")
        check(len(listed_after["result"]["tools"]) >= 4,
              "Manager 停止后 server 仍可用（tools/list 成功）")

        exit_code = client.close()
        client = None
        check(exit_code == 0, "mcp_server 正常退出", f"exit={exit_code}")
    finally:
        if client is not None:
            client.close()
        if manager is not None:
            manager.stop()
        try:
            os.remove(ini_path)
        except OSError:
            pass
        try:
            os.rmdir(workdir)
        except OSError:
            pass

    print(f"[monitor_e2e] {CHECKS} 项断言，失败 {FAILURES}")
    return 0 if FAILURES == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
