#!/usr/bin/env python3
"""B 站弹幕 demo：两/三连接广播、历史、屏蔽词、长度、限流、断线在线数。

用法：python3 scripts/demo_danmaku.py [--host 127.0.0.1] [--port 29203]
"""

import argparse
import json
import time
import urllib.request

from ws_min_client import WsClient, WsError, ws_connect


def stamp():
    return time.strftime("%H:%M:%S", time.localtime()) + f".{int(time.time() * 1000) % 1000:03d}"


class Client:
    def __init__(self, host, port, video_id, name):
        self.name = name
        self.video_id = video_id
        path = f"/ws/danmaku?video_id={video_id}"
        sock, rest = ws_connect(host, port, path)
        self.ws = WsClient(sock, rest)
        log(f"{name} connected (video_id={video_id})")

    def recv(self, timeout=3.0):
        try:
            return self.ws.recv_json(timeout)
        except (WsError, OSError):
            return None

    def send(self, text, time_ms=0, color=16777215):
        self.ws.send_json({"type": "send", "text": text,
                           "time_ms": time_ms, "color": color})

    def close(self):
        self.ws.close()
        log(f"{self.name} closed")


def log(message):
    print(f"[{stamp()}] {message}", flush=True)


def expect_online(client, expected, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        message = client.recv(max(0.05, deadline - time.time()))
        if message is None:
            continue
        if message.get("type") == "online":
            log(f"{client.name} recv online count={message.get('count')}")
            if message.get("count") == expected:
                return True
    log(f"{client.name} 未收到 expected online={expected}")
    return False


def expect_danmaku(client, expected_text, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        message = client.recv(max(0.05, deadline - time.time()))
        if message is None:
            continue
        if message.get("type") == "danmaku" and message.get("text") == expected_text:
            log(f'{client.name} recv danmaku id={message.get("id")} '
                f'text="{message.get("text")}" time_ms={message.get("time_ms")} '
                f'color={message.get("color")}')
            return message
    return None


def expect_error(client, expected_code, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        message = client.recv(max(0.05, deadline - time.time()))
        if message is None:
            continue
        if message.get("type") == "error":
            log(f'{client.name} recv error code={message.get("code")} '
                f'message="{message.get("message")}"')
            if message.get("code") == expected_code:
                return message
    return None


def http_get(port, path):
    url = f"http://127.0.0.1:{port}{path}"
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=29203)
    args = parser.parse_args()

    a = Client(args.host, args.port, 1, "A")
    expect_online(a, 1)
    b = Client(args.host, args.port, 1, "B")
    expect_online(a, 2)
    expect_online(b, 2)

    # 不同 video_id 不串房间：C 在房间 2，只看到自己的 online=1。
    c = Client(args.host, args.port, 2, "C")
    expect_online(c, 1)

    log("--- A 发 3 条，B 应全部收到；C 不应收到 ---")
    limit_start = time.time()
    texts = ["前方高能", "233333", "awsl"]
    for index, text in enumerate(texts):
        a.send(text, time_ms=(index + 1) * 1000)
        time.sleep(0.05)
    for text in texts:
        expect_danmaku(b, text)
    leaked = c.recv(0.5)
    if leaked is None:
        log("C 未收到房间 1 的弹幕（正确，房间隔离）")
    else:
        log(f"C 意外收到: {leaked}")

    log("--- 历史弹幕 ---")
    history = http_get(args.port, "/api/danmaku/1")
    log("GET /api/danmaku/1 -> " + json.dumps(history, ensure_ascii=False))
    filtered = http_get(args.port, "/api/danmaku/1?from=1500&to=3000")
    log("GET /api/danmaku/1?from=1500&to=3000 -> "
        + json.dumps(filtered, ensure_ascii=False))
    online = http_get(args.port, "/api/danmaku/1/online")
    log("GET /api/danmaku/1/online -> " + json.dumps(online, ensure_ascii=False))

    log("--- 屏蔽词 / 长度 101 ---")
    a.send("广告位招租")
    expect_error(a, 400)
    a.send("x" * 101)
    expect_error(a, 400)

    log("--- 限流：A 已发 3 条，再发 2 条成功，第 6 条应 429 ---")
    a.send("fill-1", time_ms=4000)
    expect_danmaku(b, "fill-1")
    a.send("fill-2", time_ms=5000)
    expect_danmaku(b, "fill-2")
    a.send("over-limit", time_ms=6000)
    expect_error(a, 429)

    log("--- 断线：B 关闭后房间 1 在线数应 2 -> 1 ---")
    b.close()
    time.sleep(0.2)
    expect_online(a, 1)
    log("A 继续发送，服务不应崩溃")
    # 等 A 的 5 条滑出 10s 窗口（从第一条算起），否则必然 429。
    time.sleep(max(0.0, limit_start + 10.3 - time.time()))
    a.send("after-b-left", time_ms=7000)
    expect_danmaku(a, "after-b-left")

    online_final = http_get(args.port, "/api/danmaku/1/online")
    log("最终 GET /api/danmaku/1/online -> "
        + json.dumps(online_final, ensure_ascii=False))

    a.close()
    c.close()
    log("demo 完成")


if __name__ == "__main__":
    main()
