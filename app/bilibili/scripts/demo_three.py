#!/usr/bin/env python3
"""三连接同房间广播补测：A 发一条，B 和 C 都应收到。"""

import argparse
import time

from ws_min_client import WsClient, WsError, ws_connect


def stamp():
    return time.strftime("%H:%M:%S", time.localtime()) + f".{int(time.time() * 1000) % 1000:03d}"


def log(message):
    print(f"[{stamp()}] {message}", flush=True)


def connect(port, video_id):
    sock, rest = ws_connect("127.0.0.1", port, f"/ws/danmaku?video_id={video_id}")
    return WsClient(sock, rest)


def wait_online(ws, expected, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        message = ws.recv_json(max(0.05, deadline - time.time()))
        if message and message.get("type") == "online":
            if message.get("count") == expected:
                return True
    return False


def recv_danmaku(ws, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        message = ws.recv_json(max(0.05, deadline - time.time()))
        if message and message.get("type") == "danmaku":
            return message
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=29204)
    args = parser.parse_args()

    clients = [connect(args.port, 1) for _ in range(3)]
    names = ["A", "B", "C"]
    for name, client in zip(names, clients):
        log(f"{name} connected (video_id=1)")
    for name, client in zip(names, clients):
        ok = wait_online(client, 3)
        log(f"{name} online=3 -> {'ok' if ok else 'timeout'}")

    clients[0].send_json({"type": "send", "text": "三连接广播",
                          "time_ms": 9000, "color": 16711680})
    log('A 发送 text="三连接广播"')
    for name, client in zip(names[1:], clients[1:]):
        message = recv_danmaku(client)
        if message is None:
            log(f"{name} 未收到（失败）")
            return 1
        log(f'{name} recv danmaku id={message.get("id")} '
            f'text="{message.get("text")}" color={message.get("color")}')

    for name, client in zip(names, clients):
        client.close()
        log(f"{name} closed")
    log("三连接广播补测完成")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
