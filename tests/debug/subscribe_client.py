#!/usr/bin/env python3
"""SUBSCRIBE 实时推送 demo 的客户端。

协议（见 include/go2cpp/debug/debug_server.hpp）：

    C: SUBSCRIBE events 20 128\\n
    S: GO2CPP-DEBUG/1 OK <n>\\n<subscription header>
    S: GO2CPP-DEBUG/1 PUSH <n>\\n<push payload>   (反复)
"""

import argparse
import socket
import sys


def recv_frame(sock):
    """读一个完整的 GO2CPP-DEBUG/1 帧，返回 (kind, payload) 或 None。"""
    header = b""
    while not header.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            return None
        header += chunk
        if len(header) > 512:
            raise RuntimeError("frame header too long")
    parts = header.decode("utf-8", errors="replace").strip().split(" ")
    if len(parts) < 2 or parts[0] != "GO2CPP-DEBUG/1":
        raise RuntimeError(f"bad frame header: {header!r}")
    kind = parts[1]
    if kind == "ERR":
        return kind, " ".join(parts[2:])
    size = int(parts[2])
    payload = b""
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            return None
        payload += chunk
    return kind, payload.decode("utf-8", errors="replace")


def print_event_lines(payload, limit=8):
    """只打印事件行（避免头部字段刷屏），并给出总条数。"""
    lines = [line for line in payload.splitlines() if line and not line.startswith("#")
             and "=" not in line]
    for line in lines[:limit]:
        print(f"    {line}")
    if len(lines) > limit:
        print(f"    ... ({len(lines) - limit} more event lines)")
    return len(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("socket_path")
    parser.add_argument("--frames", type=int, default=3, help="打印多少个 PUSH 帧后退出")
    parser.add_argument("--interval", type=int, default=20, help="轮询间隔 ms")
    parser.add_argument("--max-events", type=int, default=128, help="单帧事件上限")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect(args.socket_path)
    command = f"SUBSCRIBE events {args.interval} {args.max_events}\n"
    sock.sendall(command.encode())

    kind, payload = recv_frame(sock)
    if kind != "OK":
        print(f"subscribe failed: {kind} {payload}", file=sys.stderr)
        return 1
    print("=== subscription header ===")
    print(payload.rstrip())
    print("=== push frames ===")

    frames = 0
    total_events = 0
    while frames < args.frames:
        try:
            frame = recv_frame(sock)
        except socket.timeout:
            print("timeout waiting for push frame", file=sys.stderr)
            return 2
        if frame is None:
            print("connection closed", file=sys.stderr)
            return 2
        kind, payload = frame
        if kind != "PUSH":
            print(f"unexpected frame: {kind} {payload}", file=sys.stderr)
            return 2
        frames += 1
        header = {}
        for line in payload.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                header[key] = value
        count = print_event_lines(payload)
        total_events += count
        print(f"  [frame {frames}] event_count={header.get('event_count')} "
              f"first_seq={header.get('first_seq')} last_seq={header.get('last_seq')} "
              f"dropped_total={header.get('dropped_total')}")
    sock.close()
    print(f"=== done: {frames} frames / {total_events} events ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
