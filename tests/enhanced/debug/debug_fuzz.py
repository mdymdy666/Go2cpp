#!/usr/bin/env python3
"""GO2CPP-DEBUG/1 随机帧 fuzz：10000 条随机/半合法请求，检查服务端存活。

随机帧来源：
  - 纯随机字节（可含换行、NUL、非 UTF-8）；
  - 合法命令的随机变形（PING/STATUS/LIST/DUMP/JSON/EVENTS/SUBSCRIBE/HELP）；
  - 4096..8192 字节无换行的超长请求；
  - 随机命令名 + 随机参数。
每条连接发送后 shutdown(SHUT_WR)，让服务端立即看到 EOF 并处理已读内容，
避免把 5s 的 SO_RCVTIMEO 等待算进测试时间。
"""

import argparse
import random
import socket
import sys
import time

PROTOCOL = b"GO2CPP-DEBUG/1"
BASE_COMMANDS = [
    "PING", "STATUS", "LIST", "HELP", "EVENTS 10", "DUMP ALL", "JSON ALL",
    "DUMP scheduler", "JSON scheduler", "TEXT ALL", "DUMP nope", "EVENTS abc",
    "SUBSCRIBE", "SUBSCRIBE events", "SUBSCRIBE bogus",
    "SUBSCRIBE events 0", "SUBSCRIBE events 10 0",
    "SUBSCRIBE events 10 32", "SUBSCRIBE events 999999 999999",
]


def random_payload(rng: random.Random) -> bytes:
    choice = rng.random()
    if choice < 0.55:
        length = rng.choice([0, 1, 2, 3, 8, 16, 64, 200, 512, 1024, 4095, 4096, 5000, 8192])
        data = bytes(rng.randrange(256) for _ in range(length))
        if rng.random() < 0.5:
            data += b"\n"
        return data
    if choice < 0.72:
        command = rng.choice(BASE_COMMANDS)
        if rng.random() < 0.15:
            command = command.lower()
        return command.encode() + (b"\n" if rng.random() < 0.85 else b"")
    if choice < 0.86:
        length = rng.randint(4096, 8192)
        return b"A" * length
    name = "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ-_abcxyz0123456789")
                   for _ in range(rng.randint(1, 24)))
    argument = "".join(rng.choice(" a/b:c=0123456789\t")
                       for _ in range(rng.randint(0, 40)))
    data = (name + argument).encode()
    return data + (b"\n" if rng.random() < 0.7 else b"")


def ping(socket_path: str) -> bool:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2.0)
        sock.connect(socket_path)
        sock.sendall(b"PING\n")
        data = b""
        while not data.endswith(b"\n") or b"OK" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
            if len(data) > 4096:
                break
        sock.close()
        return data.startswith(PROTOCOL + b" OK") and b"pong" in data
    except OSError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--count", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=20261020)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--mode", choices=["random", "ping", "status", "oversize"],
                        default="random")
    args = parser.parse_args()
    rng = random.Random(args.seed)
    counts = {"ok": 0, "err": 0, "none": 0, "reset": 0}
    latencies = []
    started = time.monotonic()
    alive = True
    for index in range(args.count):
        if args.mode == "ping":
            payload = b"PING\n"
        elif args.mode == "status":
            payload = b"STATUS\n"
        elif args.mode == "oversize":
            payload = b"A" * 8192
        else:
            payload = random_payload(rng)
        begin = time.monotonic()
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(args.timeout)
            sock.connect(args.socket)
            try:
                sock.sendall(payload)
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            response = b""
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    break
                except OSError:
                    counts["reset"] += 1
                    break
                if not chunk:
                    break
                response += chunk
                # 收到首个响应块即可分类；SUBSCRIBE 会持续推送，不能等 1s 超时。
                break
            sock.close()
            if response.startswith(PROTOCOL + b" OK") or response.startswith(PROTOCOL + b" PUSH"):
                counts["ok"] += 1
            elif response.startswith(PROTOCOL + b" ERR"):
                counts["err"] += 1
            elif response:
                counts["none"] += 1
            else:
                counts["none"] += 1
        except OSError:
            counts["reset"] += 1
        latencies.append((time.monotonic() - begin) * 1000.0)
        if (index + 1) % 1000 == 0:
            if not ping(args.socket):
                alive = False
                print(f"DEAD after {index + 1} frames", file=sys.stderr)
                break
    if alive:
        alive = ping(args.socket)
    elapsed_ms = (time.monotonic() - started) * 1000.0
    ordered = sorted(latencies)
    p95 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))] if ordered else 0.0
    print(
        f"RESULT count={args.count} ok={counts['ok']} err={counts['err']} "
        f"none={counts['none']} reset={counts['reset']} "
        f"server_alive={1 if alive else 0} elapsed_ms={elapsed_ms:.1f} "
        f"p95_ms={p95:.2f} seed={args.seed}"
    )
    return 0 if alive else 1


if __name__ == "__main__":
    sys.exit(main())
