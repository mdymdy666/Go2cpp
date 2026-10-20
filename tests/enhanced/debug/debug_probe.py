#!/usr/bin/env python3
"""GO2CPP-DEBUG/1 协议探针（测试夹具，不修改被测代码）。

子命令：
  ping/status/list/dump-all/events  发一条命令并打印 payload
  subscribe --frames N ...          订阅 events，打印 header 与 N 个 PUSH 帧
  raw --text/--hex ...              发送任意字节（可无换行/半条），打印响应
  concurrent --count N --command C  并发 N 条连接，统计成功/失败/延迟
  slow-hold --seconds S             连接后不发送任何字节（测服务端读超时阻塞）
  hold-subscribe --seconds S        建立订阅后不读取 PUSH（测背压）

退出码：0 成功；2 连接/协议错误；3 超时/截断；4 服务端 ERR；5 并发失败。
"""

import argparse
import socket
import sys
import threading
import time

PROTOCOL = "GO2CPP-DEBUG/1"


class ProtocolError(Exception):
    pass


def connect(path: str, timeout: float = 5.0) -> socket.socket:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    sock.connect(path)
    return sock


def recv_header(sock: socket.socket) -> bytes:
    header = b""
    while not header.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError("connection closed before header")
        header += chunk
        if len(header) > 4096:
            raise ProtocolError("header too long")
    return header


def read_frame(sock: socket.socket) -> tuple[str, bytes]:
    header = recv_header(sock)
    parts = header.decode("utf-8", "replace").strip().split(" ")
    if not parts or parts[0] != PROTOCOL:
        raise ProtocolError(f"bad frame header: {header!r}")
    kind = parts[1]
    if kind == "ERR":
        return kind, " ".join(parts[2:]).encode()
    if len(parts) < 3 or not parts[2].isdigit():
        raise ProtocolError(f"bad frame header: {header!r}")
    size = int(parts[2])
    if size > 64 * 1024 * 1024:
        raise ProtocolError(f"unreasonable payload size {size}")
    payload = b""
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk:
            raise EOFError(f"payload truncated at {len(payload)}/{size}")
        payload += chunk
    return kind, payload


def simple(path: str, command: str, timeout: float) -> int:
    try:
        sock = connect(path, timeout)
        sock.sendall((command + "\n").encode())
        kind, payload = read_frame(sock)
        sock.close()
    except (OSError, EOFError, ProtocolError) as error:
        print(f"probe: {error}", file=sys.stderr)
        return 3 if isinstance(error, EOFError) else 2
    text = payload.decode("utf-8", "replace")
    print(text, end="" if text.endswith("\n") else "\n")
    return 4 if kind == "ERR" else 0


def subscribe(path: str, topic: str, interval: int, max_events: int,
              frames: int, timeout: float) -> int:
    try:
        sock = connect(path, timeout)
        sock.sendall(f"SUBSCRIBE {topic} {interval} {max_events}\n".encode())
        kind, payload = read_frame(sock)
        if kind == "ERR":
            print("subscribe failed: " + payload.decode("utf-8", "replace"),
                  file=sys.stderr)
            return 4
        print("=== subscription header ===")
        print(payload.decode("utf-8", "replace").rstrip())
        print("=== push frames ===")
        for index in range(frames):
            kind, payload = read_frame(sock)
            if kind != "PUSH":
                print(f"unexpected frame: {kind}", file=sys.stderr)
                return 2
            text = payload.decode("utf-8", "replace")
            header = {}
            events = 0
            for line in text.splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    header[key] = value
                elif line and not line.startswith("#"):
                    events += 1
            print(f"[frame {index + 1}] event_count={header.get('event_count')} "
                  f"first_seq={header.get('first_seq')} last_seq={header.get('last_seq')} "
                  f"dropped_total={header.get('dropped_total')} lines={events}")
        sock.close()
        return 0
    except (OSError, EOFError, ProtocolError) as error:
        print(f"probe: {error}", file=sys.stderr)
        return 3


def raw(path: str, data: bytes, timeout: float) -> int:
    try:
        sock = connect(path, timeout)
        try:
            sock.sendall(data)
            sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        chunks = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                break
            except OSError:
                break
            if not chunk:
                break
            chunks.append(chunk)
        sock.close()
    except (OSError, ProtocolError) as error:
        print(f"probe: {error}", file=sys.stderr)
        return 2
    response = b"".join(chunks)
    print(f"bytes={len(response)}")
    print(response.decode("utf-8", "replace"))
    if response.startswith(PROTOCOL.encode() + b" ERR"):
        return 4
    return 0 if response.startswith(PROTOCOL.encode()) else 3


def concurrent(path: str, count: int, command: str, timeout: float) -> int:
    ok = 0
    failures = []
    latencies = []
    lock = threading.Lock()

    def worker(index: int) -> None:
        nonlocal ok
        started = time.monotonic()
        try:
            sock = connect(path, timeout)
            sock.sendall((command + "\n").encode())
            kind, _payload = read_frame(sock)
            sock.close()
            elapsed = (time.monotonic() - started) * 1000.0
            with lock:
                if kind == "OK":
                    ok += 1
                    latencies.append(elapsed)
                else:
                    failures.append((index, kind))
        except (OSError, EOFError, ProtocolError) as error:
            with lock:
                failures.append((index, str(error)))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    latencies.sort()
    p95 = latencies[int(len(latencies) * 0.95)] if latencies else 0.0
    print(f"ok={ok} failures={len(failures)} p95_ms={p95:.1f} "
          f"max_ms={(max(latencies) if latencies else 0):.1f} sample={failures[:3]}")
    return 0 if ok == count else 5


def slow_hold(path: str, seconds: float) -> int:
    try:
        sock = connect(path, 2.0)
    except OSError as error:
        print(f"probe: {error}", file=sys.stderr)
        return 2
    time.sleep(seconds)
    sock.close()
    return 0


def hold_subscribe(path: str, seconds: float) -> int:
    try:
        sock = connect(path, 2.0)
        sock.sendall(b"SUBSCRIBE events 5 64\n")
        # 只读 header，随后故意不读 PUSH，让服务端发送缓冲填满。
        kind, payload = read_frame(sock)
        if kind != "OK":
            print(f"probe: subscribe failed {kind}", file=sys.stderr)
            return 4
        time.sleep(seconds)
        sock.close()
        return 0
    except (OSError, EOFError, ProtocolError) as error:
        print(f"probe: {error}", file=sys.stderr)
        return 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("socket")
    parser.add_argument("command")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--topic", default="events")
    parser.add_argument("--interval", type=int, default=10)
    parser.add_argument("--max-events", type=int, default=32)
    parser.add_argument("--frames", type=int, default=2)
    parser.add_argument("--text", default="")
    parser.add_argument("--hex", default="")
    parser.add_argument("--no-newline", action="store_true")
    parser.add_argument("--count", type=int, default=10)
    parser.add_argument("--command-text", default="STATUS")
    parser.add_argument("--seconds", type=float, default=3.0)
    args = parser.parse_args()

    command = args.command
    if command == "subscribe":
        return subscribe(args.socket, args.topic, args.interval,
                         args.max_events, args.frames, args.timeout)
    if command == "raw":
        if args.hex:
            data = bytes.fromhex(args.hex)
        else:
            data = args.text.encode()
            if not args.no_newline:
                data += b"\n"
        return raw(args.socket, data, args.timeout)
    if command == "concurrent":
        return concurrent(args.socket, args.count, args.command_text,
                          args.timeout)
    if command == "slow-hold":
        return slow_hold(args.socket, args.seconds)
    if command == "hold-subscribe":
        return hold_subscribe(args.socket, args.seconds)
    if command == "dump-all":
        return simple(args.socket, "JSON ALL", args.timeout)
    if command == "list":
        return simple(args.socket, "LIST", args.timeout)
    if command == "events":
        return simple(args.socket, "EVENTS 20", args.timeout)
    return simple(args.socket, command.upper(), args.timeout)


if __name__ == "__main__":
    sys.exit(main())
