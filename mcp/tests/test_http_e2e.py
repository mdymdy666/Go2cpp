#!/usr/bin/env python3
"""HTTP/SSE 传输 e2e：Streamable HTTP + 多客户端旧版 SSE 会话。

覆盖：
1. POST /mcp（initialize / tools/list / tools/call / 通知 / 未知方法）；
2. GET /sse 分配唯一 sessionId，endpoint 指向 /message?sessionId=...；
3. 3 个客户端并发，各自收到自己的响应（不串会话）；
4. 通知双出口：定向（工具执行期间只投递发起会话）与广播（插件/`POST /mcp` 全收）；
5. 未知/缺失 sessionId → 404/400；/messages 兼容别名；
6. 断开清理与重连；
7. 10 客户端 × 100 请求并发压测；
8. 会话空闲超时（通过 MCP_SSE_IDLE_TIMEOUT_MS 缩短）。

用法: test_http_e2e.py <mcp_server>
"""

import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

SERVER = sys.argv[1] if len(sys.argv) > 1 else ""
failures = 0
checks = 0
_result_lock = threading.Lock()


def check(condition, label, detail=""):
    """记录一条断言；线程安全。"""
    global failures, checks
    with _result_lock:
        checks += 1
        if not condition:
            failures += 1
            print(f"FAIL: {label} {detail}")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def post(port, payload, path, timeout=30):
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, response.read().decode()


def post_status(port, payload, path, timeout=30):
    """POST 并保留 HTTP 错误码（urllib 默认对 4xx/5xx 抛异常）。"""
    try:
        return post(port, payload, path, timeout=timeout)
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def read_sse(stream):
    """读取一条完整 SSE 事件，返回 (event, data)；跳过心跳与注释。"""
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


def open_sse(port, timeout=30):
    """建立 SSE 连接并读完 endpoint 事件，返回 (stream, session_id, endpoint)。"""
    stream = urllib.request.urlopen(f"http://127.0.0.1:{port}/sse", timeout=timeout)
    event, data = read_sse(stream)
    if event != "endpoint":
        raise RuntimeError(f"首个 SSE 事件不是 endpoint: {event} {data}")
    query = urllib.parse.parse_qs(urllib.parse.urlparse(data).query)
    session_id = query.get("sessionId", [""])[0]
    if not session_id:
        raise RuntimeError(f"endpoint 缺少 sessionId: {data}")
    return stream, session_id, data


def message_uri(session_id, alias=False):
    name = "messages" if alias else "message"
    return f"/{name}?sessionId={urllib.parse.quote(session_id)}"


def ping(rid):
    return {"jsonrpc": "2.0", "id": rid, "method": "ping"}


def echo(rid, text):
    return {
        "jsonrpc": "2.0",
        "id": rid,
        "method": "tools/call",
        "params": {"name": "echo", "arguments": {"text": text}},
    }


class ServerProcess:
    """启动/停止 mcp_server --http，并等待端口就绪。"""

    def __init__(self, env=None, extra_args=None):
        if not SERVER:
            raise RuntimeError("缺少 mcp_server 路径参数")
        self.port = free_port()
        environment = dict(os.environ)
        if env:
            environment.update(env)
        self.process = subprocess.Popen(
            [SERVER, "--http", str(self.port)] + list(extra_args or []),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        self.stderr = ""
        try:
            self._wait_ready()
        except Exception:
            self.stop()
            raise

    def _wait_ready(self):
        for _ in range(100):
            if self.process.poll() is not None:
                _, stderr = self.process.communicate(timeout=2)
                raise RuntimeError(f"服务提前退出: {(stderr or '')[:500]}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("等待 HTTP 端口超时")

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
        try:
            _, stderr = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            _, stderr = self.process.communicate(timeout=5)
        self.stderr = stderr or ""


def test_streamable_http(port):
    status, body = post(
        port,
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        "/mcp",
    )
    check(status == 200, "POST /mcp 状态码", str(status))
    check(json.loads(body)["result"]["protocolVersion"] == "2025-06-18", "initialize")

    status, body = post(port, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, "/mcp")
    names = {item["name"] for item in json.loads(body)["result"]["tools"]}
    check("calculator" in names and "ros2_command" in names, "tools/list 经 HTTP")

    status, body = post(
        port,
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {"name": "multiply", "arguments": {"a": 6, "b": 7}},
        },
        "/mcp",
    )
    check(json.loads(body)["result"]["content"][0]["text"] == "42", "tools/call 经 HTTP")

    status, _ = post(port, {"jsonrpc": "2.0", "method": "notifications/initialized"}, "/mcp")
    check(status == 204, "通知返回 204", str(status))

    status, body = post(port, {"jsonrpc": "2.0", "id": 4, "method": "nope"}, "/mcp")
    check(status == 200 and json.loads(body)["error"]["code"] == -32601, "未知方法经 HTTP")


def test_three_clients(port):
    """3 个客户端同时建立 SSE，各自发 10 个 echo，验证响应完全隔离。"""
    clients = 3
    per_client = 10
    barrier = threading.Barrier(clients)
    collected = {}
    errors = []

    def worker(index):
        stream = None
        try:
            stream, session_id, endpoint = open_sse(port)
            barrier.wait(timeout=20)
            responses = []
            for sequence in range(per_client):
                rid = index * 1000 + sequence
                text = f"client-{index}-request-{sequence}"
                status, _ = post(port, echo(rid, text), message_uri(session_id))
                event, payload = read_sse(stream)
                document = json.loads(payload)
                content = document.get("result", {}).get("content", [{}])
                responses.append(
                    {
                        "status": status,
                        "event": event,
                        "id": document.get("id"),
                        "text": content[0].get("text") if content else None,
                        "expected_id": rid,
                        "expected_text": text,
                    }
                )
            with _result_lock:
                collected[index] = {
                    "session": session_id,
                    "endpoint": endpoint,
                    "responses": responses,
                }
        except Exception as error:  # noqa: BLE001 - 测试里记录后统一断言
            with _result_lock:
                errors.append(f"client{index}: {error!r}")
        finally:
            if stream is not None:
                stream.close()

    threads = [threading.Thread(target=worker, args=(index,)) for index in range(clients)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(60)

    check(not errors, "3 客户端线程无异常", str(errors))
    check(len(collected) == clients, "3 客户端都完成", str(sorted(collected)))
    sessions = [item["session"] for item in collected.values()]
    check(len(set(sessions)) == clients, "3 个 sessionId 互不相同", str(sessions))

    for index in range(clients):
        entry = collected.get(index)
        if not entry:
            continue
        responses = entry["responses"]
        check(len(responses) == per_client, f"client{index} 响应数", str(len(responses)))
        check(
            all(item["status"] == 200 and item["event"] == "message" for item in responses),
            f"client{index} 全部经 SSE 返回",
        )
        check(
            [item["id"] for item in responses]
            == [item["expected_id"] for item in responses],
            f"client{index} 只收到自己的 id",
            str([item["id"] for item in responses]),
        )
        check(
            [item["text"] for item in responses]
            == [item["expected_text"] for item in responses],
            f"client{index} 回显内容不串",
        )

    if len(collected) == clients:
        print(
            "[3 clients] sessions="
            + ", ".join(item["session"] for item in collected.values())
            + "；每个客户端 10 个请求均只收到自己的响应"
        )
    return collected


def test_notification_isolation(port):
    """工具通知必须只进发起请求的会话；用另一个会话的首个事件做反证。"""
    stream_a, sid_a, _ = open_sse(port)
    stream_b, sid_b, _ = open_sse(port)
    try:
        status, _ = post(
            port,
            {
                "jsonrpc": "2.0",
                "id": 100,
                "method": "tools/call",
                "params": {"name": "logging_test", "arguments": {}},
            },
            message_uri(sid_a),
        )
        check(status == 200, "logging_test 请求被接受", str(status))
        event, payload = read_sse(stream_a)
        document = json.loads(payload)
        check(
            event == "message"
            and document.get("method") == "notifications/message"
            and document.get("params", {}).get("data") == "logging_test executed",
            "A 收到自己的 notifications/message",
            payload[:90],
        )
        event, payload = read_sse(stream_a)
        check(json.loads(payload).get("id") == 100, "A 收到自己的响应", payload[:90])

        # B 在 A 通知之后发请求：若通知被广播，B 的首个事件会是 A 的通知。
        status, _ = post(port, ping(200), message_uri(sid_b))
        check(status == 200, "B 的 ping 被接受", str(status))
        event, payload = read_sse(stream_b)
        check(event == "message" and json.loads(payload).get("id") == 200,
              "B 的首个事件是自己的响应，不是 A 的通知", payload[:90])

        # 反向：B 发通知，A 下一个事件仍应是 A 自己的响应。
        status, _ = post(
            port,
            {
                "jsonrpc": "2.0",
                "id": 201,
                "method": "tools/call",
                "params": {"name": "logging_test", "arguments": {}},
            },
            message_uri(sid_b),
        )
        check(status == 200, "B 的 logging_test 请求被接受", str(status))
        _, payload = read_sse(stream_b)
        check(json.loads(payload).get("method") == "notifications/message",
              "B 收到自己的 notifications/message", payload[:90])
        _, payload = read_sse(stream_b)
        check(json.loads(payload).get("id") == 201, "B 收到自己的响应", payload[:90])

        status, _ = post(port, ping(101), message_uri(sid_a))
        check(status == 200, "A 的 ping 被接受", str(status))
        _, payload = read_sse(stream_a)
        check(json.loads(payload).get("id") == 101,
              "A 未收到 B 的通知（首事件是自己的响应）", payload[:90])
        print("[notifications] A/B 的通知与响应均只投递到各自会话")
    finally:
        stream_a.close()
        stream_b.close()


def test_broadcast_and_targeted(port):
    """广播出口：3 个会话全收；定向出口：只进发起会话，不串。"""
    streams = []
    sessions = []
    try:
        for _ in range(3):
            stream, session_id, _ = open_sse(port)
            streams.append(stream)
            sessions.append(session_id)

        # 广播出口：POST /mcp 没有会话绑定，触发的是服务级通知。
        status, _ = post(
            port,
            {
                "jsonrpc": "2.0",
                "id": 700,
                "method": "tools/call",
                "params": {"name": "logging_test", "arguments": {}},
            },
            "/mcp",
        )
        check(status == 200, "广播触发请求成功", str(status))
        for index, stream in enumerate(streams):
            event, payload = read_sse(stream)
            document = json.loads(payload)
            check(
                event == "message" and document.get("method") == "notifications/message",
                f"广播：会话 {index} 收到通知",
                payload[:90],
            )
            check(
                document.get("params", {}).get("data") == "logging_test executed",
                f"广播：会话 {index} 通知内容正确",
            )

        # 定向出口：只让会话 0 收到 notifications/message + 响应。
        status, _ = post(
            port,
            {
                "jsonrpc": "2.0",
                "id": 701,
                "method": "tools/call",
                "params": {"name": "logging_test", "arguments": {}},
            },
            message_uri(sessions[0]),
        )
        check(status == 200, "定向触发请求成功", str(status))
        _, payload = read_sse(streams[0])
        check(json.loads(payload).get("method") == "notifications/message",
              "定向：会话 0 收到通知", payload[:90])
        _, payload = read_sse(streams[0])
        check(json.loads(payload).get("id") == 701, "定向：会话 0 收到响应", payload[:90])

        for index in (1, 2):
            status, _ = post(port, ping(702 + index), message_uri(sessions[index]))
            check(status == 200, f"定向：会话 {index} ping 成功", str(status))
            _, payload = read_sse(streams[index])
            check(
                json.loads(payload).get("id") == 702 + index,
                f"定向：会话 {index} 首事件是自己的响应，未串入会话 0 的通知",
                payload[:90],
            )
        print("[broadcast] 3 会话都收到广播；定向通知只进发起会话")
    finally:
        for stream in streams:
            stream.close()


def test_session_errors(port):
    status, body = post_status(port, ping(300), "/message?sessionId=not-a-real-session")
    check(status == 404, "未知 sessionId → 404", str(status))
    check("unknown" in body or "session" in body, "404 响应体说明 session 非法", body[:90])

    status, _ = post_status(port, ping(301), "/message")
    check(status == 400, "缺少 sessionId → 400", str(status))

    status, _ = post_status(port, ping(302), "/message?sessionId=")
    check(status == 400, "空 sessionId → 400", str(status))

    stream, sid, endpoint = open_sse(port)
    try:
        check(endpoint.startswith("/message?sessionId="), "endpoint 指向 /message", endpoint)
        status, _ = post_status(port, ping(303), message_uri(sid, alias=True))
        check(status == 200, "/messages 兼容别名返回 200", str(status))
        event, payload = read_sse(stream)
        check(event == "message" and json.loads(payload).get("id") == 303,
              "/messages 响应仍走 SSE", payload[:90])
    finally:
        stream.close()


def test_disconnect_reconnect(port):
    stream, old_session, _ = open_sse(port)
    stream.close()

    status = None
    started = time.monotonic()
    while time.monotonic() - started < 8:
        status, _ = post_status(port, ping(400), message_uri(old_session), timeout=10)
        if status == 404:
            break
        time.sleep(0.1)
    check(status == 404, "断开后旧 sessionId 被清理", str(status))
    check(time.monotonic() - started < 8, "断开清理在超时内完成", f"{time.monotonic() - started:.2f}s")

    new_stream, new_session, _ = open_sse(port)
    try:
        check(new_session != old_session, "重连分配新 sessionId", new_session)
        status, _ = post(port, ping(401), message_uri(new_session))
        check(status == 200, "重连后 POST 成功", str(status))
        event, payload = read_sse(new_stream)
        check(event == "message" and json.loads(payload).get("id") == 401,
              "重连后响应经新 SSE 返回", payload[:90])
    finally:
        new_stream.close()

    status, _ = post_status(port, ping(402), message_uri(old_session))
    check(status == 404, "旧 sessionId 重连后仍为 404", str(status))


def test_concurrency_stress(port, clients=10, requests=100):
    """10 客户端 × 100 请求：先全部 POST 再读 SSE，检查不丢、不串、不重复。"""
    barrier = threading.Barrier(clients)
    collected = {}
    errors = []

    def worker(index):
        stream = None
        try:
            stream, session_id, _ = open_sse(port, timeout=60)
            barrier.wait(timeout=30)
            for sequence in range(requests):
                rid = index * 100000 + sequence
                text = f"stress-{index}-{sequence}"
                status, _ = post(port, echo(rid, text), message_uri(session_id), timeout=60)
                if status != 200:
                    raise RuntimeError(f"请求 {rid} 返回 {status}")
            received = {}
            events = 0
            for _ in range(requests):
                event, payload = read_sse(stream)
                if event != "message":
                    raise RuntimeError(f"非 message 事件: {event}")
                document = json.loads(payload)
                rid = document.get("id")
                content = document.get("result", {}).get("content", [{}])
                received[rid] = content[0].get("text") if content else None
                events += 1
            with _result_lock:
                collected[index] = {
                    "session": session_id,
                    "received": received,
                    "events": events,
                }
        except Exception as error:  # noqa: BLE001
            with _result_lock:
                errors.append(f"client{index}: {error!r}")
        finally:
            if stream is not None:
                stream.close()

    threads = [threading.Thread(target=worker, args=(index,)) for index in range(clients)]
    started = time.monotonic()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(180)
    elapsed = max(time.monotonic() - started, 1e-9)

    check(not errors, "压测线程无异常", str(errors))
    check(len(collected) == clients, "压测客户端都完成", str(sorted(collected)))
    check(
        len({item["session"] for item in collected.values()}) == clients,
        "压测 sessionId 互不相同",
    )

    total = 0
    for index in range(clients):
        entry = collected.get(index)
        if not entry:
            continue
        received = entry["received"]
        total += len(received)
        expected_ids = {index * 100000 + sequence for sequence in range(requests)}
        expected_texts = {f"stress-{index}-{sequence}" for sequence in range(requests)}
        check(len(received) == requests, f"client{index} 收到 {requests} 条", str(len(received)))
        check(set(received) == expected_ids, f"client{index} 无跨会话/丢失 id")
        check(set(received.values()) == expected_texts, f"client{index} 回显内容正确")

    check(total == clients * requests, "压测响应总数", f"{total} != {clients * requests}")
    print(
        f"[stress] {clients} 客户端 x {requests} 请求 = {clients * requests} 响应，"
        f"{elapsed:.2f}s，约 {clients * requests / elapsed:.0f} req/s，跨会话串扰 0"
    )


def test_plugin_broadcast(port):
    """插件主动通知走广播 sink：即使从 `/message`（有绑定）触发，也发所有会话。"""
    streams = []
    sessions = []
    try:
        for _ in range(3):
            stream, session_id, _ = open_sse(port, timeout=8)
            streams.append(stream)
            sessions.append(session_id)

        status, body = post(
            port,
            {
                "jsonrpc": "2.0",
                "id": 800,
                "method": "tools/call",
                "params": {"name": "notify_test", "arguments": {}},
            },
            message_uri(sessions[0]),
        )
        check(status == 200, "插件广播触发请求成功", str(status))
        check(body.strip() == "accepted", "插件广播：POST /message 返回 accepted", body[:90])

        for index, stream in enumerate(streams):
            event, payload = read_sse(stream)
            document = json.loads(payload)
            check(
                event == "message" and document.get("method") == "notifications/message",
                f"插件广播：会话 {index} 收到 message",
                payload[:90],
            )
            check(document.get("params", {}).get("data") == "plugin says hi",
                  f"插件广播：会话 {index} message 内容")
            event, payload = read_sse(stream)
            document = json.loads(payload)
            check(
                event == "message" and document.get("method") == "notifications/progress",
                f"插件广播：会话 {index} 收到 progress",
                payload[:90],
            )

        # 发起请求的会话随后还应收到自己的 tools/call 响应。
        event, payload = read_sse(streams[0])
        check(event == "message" and json.loads(payload).get("id") == 800,
              "插件广播：发起会话仍收到自己的响应", payload[:90])
        print("[plugin broadcast] 3 会话都收到插件 message + progress")
    finally:
        for stream in streams:
            stream.close()


def test_idle_timeout(port):
    stream, session_id, _ = open_sse(port, timeout=10)
    started = time.monotonic()
    closed = False
    try:
        line = stream.readline()
        closed = line == b""
    except (EOFError, socket.timeout, TimeoutError, ConnectionResetError):
        closed = True
    finally:
        stream.close()
    elapsed = time.monotonic() - started
    check(closed, "空闲会话被服务端关闭", f"elapsed={elapsed:.2f}s")
    check(elapsed < 5, "空闲超时在 5s 内生效", f"{elapsed:.2f}s")

    status, _ = post_status(port, ping(500), message_uri(session_id))
    check(status == 404, "超时会话的 sessionId 已失效", str(status))

    # 持续有请求的会话不应在同一个空闲窗口内被误杀。
    active, active_session, _ = open_sse(port, timeout=10)
    alive = True
    try:
        for sequence in range(8):
            status, _ = post(port, ping(600 + sequence), message_uri(active_session))
            if status != 200:
                alive = False
                break
            _, payload = read_sse(active)
            if json.loads(payload).get("id") != 600 + sequence:
                alive = False
                break
            time.sleep(0.2)
    finally:
        active.close()
    check(alive, "活跃会话跨过空闲窗口仍可用")


def main():
    if not SERVER:
        print("用法: test_http_e2e.py <mcp_server>", file=sys.stderr)
        return 2

    server = ServerProcess()
    try:
        test_streamable_http(server.port)
        test_three_clients(server.port)
        test_notification_isolation(server.port)
        test_broadcast_and_targeted(server.port)
        test_session_errors(server.port)
        test_disconnect_reconnect(server.port)
        test_concurrency_stress(server.port)
    finally:
        server.stop()
    if "HTTP 服务已启动" not in server.stderr:
        check(False, "服务启动日志", (server.stderr or "")[:200])

    plugin_dir = os.path.join(
        os.path.dirname(os.path.abspath(SERVER)), "tests", "test_plugins"
    )
    if os.path.isdir(plugin_dir):
        plugin_server = ServerProcess(extra_args=["--plugins", plugin_dir])
        try:
            test_plugin_broadcast(plugin_server.port)
        finally:
            plugin_server.stop()
        check("已加载 1 个插件" in plugin_server.stderr,
              "插件广播测试服务加载插件", plugin_server.stderr[:200])
    else:
        print(f"[plugin broadcast] 跳过：未找到 {plugin_dir}")

    timeout_server = ServerProcess(
        {"MCP_SSE_IDLE_TIMEOUT_MS": "800", "MCP_SSE_POLL_MS": "100"}
    )
    try:
        test_idle_timeout(timeout_server.port)
    finally:
        timeout_server.stop()
    if "HTTP 服务已启动" not in timeout_server.stderr:
        check(False, "超时测试服务启动日志", (timeout_server.stderr or "")[:200])

    print(f"[http_e2e] {checks} 项断言，失败 {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
