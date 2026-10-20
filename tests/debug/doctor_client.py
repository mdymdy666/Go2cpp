#!/usr/bin/env python3
"""Doctor（G2CD/1）命令行客户端 + 并发验收脚本。

用法：
  python3 doctor_client.py <socket> status
  python3 doctor_client.py <socket> "dump sections=scheduler" --json
  python3 doctor_client.py <socket> --concurrency 10 --command status

帧格式与 include/go2cpp/debug/diagnostic_protocol.hpp 完全一致：
  magic(4)=G2CD version(1) kind(1) flags(2) stream_id(4) seq(4)
  payload_len(4) checksum(4)=FNV-1a(header with checksum=0 + payload) payload

退出码：
  0  成功（响应 kind=response）
  2  参数/连接错误
  3  服务端返回 error 帧
  4  force-wake/mutex-unlock 明确 refused
  5  超时/帧错误
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
import time

MAGIC = 0x44433247
VERSION = 1
HEADER = struct.Struct("<IBBHIIII")
HEADER_SIZE = HEADER.size  # 24
KIND_REQUEST = 1
KIND_RESPONSE = 2
KIND_ERROR = 3
FLAG_CHUNKED = 1
FLAG_LAST = 2
FLAG_JSON = 4
MAX_PAYLOAD = 16 * 1024 * 1024


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def encode_frame(kind: int, stream_id: int, payload: bytes, flags: int = 0,
                 seq: int = 0) -> bytes:
    header = HEADER.pack(MAGIC, VERSION, kind, flags, stream_id, seq,
                         len(payload), 0)
    checksum = fnv1a(header + payload)
    header = HEADER.pack(MAGIC, VERSION, kind, flags, stream_id, seq,
                         len(payload), checksum)
    return header + payload


class FrameReader:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buffer = b""

    def read_frame(self, timeout: float) -> tuple[int, int, bytes, int, int]:
        self.sock.settimeout(timeout)
        while True:
            if len(self.buffer) >= HEADER_SIZE:
                (magic, version, kind, flags, stream_id, seq, length,
                 checksum) = HEADER.unpack_from(self.buffer, 0)
                if magic != MAGIC:
                    raise ValueError("bad-magic")
                if version != VERSION:
                    raise ValueError("unsupported-version")
                if length > MAX_PAYLOAD:
                    raise ValueError("payload-too-large")
                total = HEADER_SIZE + length
                if len(self.buffer) >= total:
                    payload = self.buffer[HEADER_SIZE:total]
                    header_zero = HEADER.pack(MAGIC, VERSION, kind, flags,
                                              stream_id, seq, length, 0)
                    if fnv1a(header_zero + payload) != checksum:
                        raise ValueError("bad-checksum")
                    self.buffer = self.buffer[total:]
                    return kind, flags, payload, stream_id, seq
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            self.buffer += chunk


def call(sock: socket.socket, command: str, stream_id: int = 1,
         timeout: float = 10.0) -> tuple[int, bytes]:
    sock.sendall(encode_frame(KIND_REQUEST, stream_id,
                              command.encode("utf-8")))
    reader = FrameReader(sock)
    assembled = b""
    deadline = time.monotonic() + timeout
    while True:
        remaining = max(0.05, deadline - time.monotonic())
        kind, flags, payload, response_stream, _seq = reader.read_frame(remaining)
        if response_stream != stream_id:
            continue
        assembled += payload
        if not (flags & FLAG_CHUNKED) or (flags & FLAG_LAST):
            return kind, assembled


def parse_kv(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line or line.startswith("["):
            continue
        key, value = line.split("=", 1)
        values[key] = value
    return values


def one_shot(path: str, command: str, timeout: float) -> int:
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(path)
        kind, payload = call(sock, command, timeout=timeout)
    except (OSError, ValueError, ConnectionError) as error:
        print(f"doctor: {error}", file=sys.stderr)
        return 2
    finally:
        sock.close()
    text = payload.decode("utf-8", "replace")
    print(text, end="" if text.endswith("\n") else "\n")
    if kind == KIND_ERROR:
        return 3
    if "verdict=refused" in text:
        return 4
    return 0


def concurrency_probe(path: str, count: int, command: str,
                      timeout: float) -> int:
    results: dict[int, tuple[int, bytes]] = {}
    failures: list[str] = []
    lock = threading.Lock()

    def worker(index: int) -> None:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            stream_id = 1000 + index
            kind, payload = call(sock, command, stream_id=stream_id,
                                 timeout=timeout)
            with lock:
                results[stream_id] = (kind, payload)
        except (OSError, ValueError, ConnectionError) as error:
            with lock:
                failures.append(f"stream={1000 + index}: {error}")
        finally:
            sock.close()

    threads = [threading.Thread(target=worker, args=(index,))
               for index in range(count)]
    started = time.monotonic()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.monotonic() - started

    if failures:
        print("FAIL concurrency: " + "; ".join(failures), file=sys.stderr)
        return 5
    if len(results) != count:
        print(f"FAIL concurrency: got {len(results)}/{count}", file=sys.stderr)
        return 5
    for stream_id, (kind, payload) in sorted(results.items()):
        if kind != KIND_RESPONSE:
            print(f"FAIL stream {stream_id}: kind={kind}", file=sys.stderr)
            return 5
        if not payload:
            print(f"FAIL stream {stream_id}: empty payload", file=sys.stderr)
            return 5
    print(f"PASS concurrency={count} elapsed_ms={elapsed * 1000:.1f} "
          f"streams={sorted(results)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Doctor G2CD/1 client (Unix socket)")
    parser.add_argument("socket", help="Unix socket path")
    parser.add_argument("command", nargs="?", default=None,
                        help="diagnostic command (default: status)")
    parser.add_argument("--command", dest="command_option", default=None,
                        help="diagnostic command (same as positional)")
    parser.add_argument("--json", action="store_true",
                        help="append `json=1` to the command")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--concurrency", type=int, default=0,
                        help="probe N concurrent requests; 0 = single request")
    args = parser.parse_args()

    command = args.command_option or args.command or "status"
    if args.json and "json=1" not in command:
        command += " json=1"

    if args.concurrency > 0:
        return concurrency_probe(args.socket, args.concurrency, command,
                                 args.timeout)
    return one_shot(args.socket, command, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
