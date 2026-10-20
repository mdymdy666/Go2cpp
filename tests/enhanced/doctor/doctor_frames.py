#!/usr/bin/env python3
"""Doctor G2CD/1 帧探针：坏校验/坏 magic/截断/超长命令。

帧格式（include/go2cpp/debug/diagnostic_protocol.hpp）：
  magic(4)=G2CD version(1) kind(1) flags(2) stream_id(4) seq(4)
  payload_len(4) checksum(4)=FNV-1a(header with checksum=0 + payload) payload
"""

import argparse
import socket
import struct
import sys

MAGIC = 0x44433247
VERSION = 1
HEADER = struct.Struct("<IBBHIIII")
KIND_REQUEST = 1
KIND_RESPONSE = 2
KIND_ERROR = 3


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def encode(payload: bytes, *, checksum_delta: int = 0,
           magic: int = MAGIC) -> bytes:
    header = HEADER.pack(magic, VERSION, KIND_REQUEST, 0, 7, 0,
                         len(payload), 0)
    checksum = (fnv1a(header + payload) + checksum_delta) & 0xFFFFFFFF
    header = HEADER.pack(magic, VERSION, KIND_REQUEST, 0, 7, 0,
                         len(payload), checksum)
    return header + payload


def read_response(sock: socket.socket) -> tuple[int, bytes]:
    header = b""
    while len(header) < HEADER.size:
        chunk = sock.recv(HEADER.size - len(header))
        if not chunk:
            raise ConnectionError("closed before header")
        header += chunk
    (magic, version, kind, flags, stream, seq, length,
     checksum) = HEADER.unpack(header)
    if magic != MAGIC or version != VERSION:
        raise ValueError(f"bad response header magic={magic:#x}")
    if length > 16 * 1024 * 1024:
        raise ValueError("response too large")
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise ConnectionError("closed mid payload")
        payload += chunk
    return kind, payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("socket")
    parser.add_argument("mode", choices=["bad-checksum", "bad-magic", "truncated",
                                        "oversized", "unknown", "status"])
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()

    if args.mode == "bad-checksum":
        data = encode(b"status", checksum_delta=1)
    elif args.mode == "bad-magic":
        data = encode(b"status", magic=0xDEADBEEF)
    elif args.mode == "truncated":
        data = HEADER.pack(MAGIC, VERSION, KIND_REQUEST, 0, 9, 0, 100, 0) + b"abc"
    elif args.mode == "oversized":
        data = encode(b"x" * 5000)
    elif args.mode == "unknown":
        data = encode(b"bogus-command")
    else:
        data = encode(b"status")

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(args.timeout)
    try:
        sock.connect(args.socket)
        sock.sendall(data)
        if args.mode == "truncated":
            sock.shutdown(socket.SHUT_WR)
        kind, payload = read_response(sock)
        print(f"kind={kind} bytes={len(payload)}")
        print(payload.decode("utf-8", "replace")[:300])
        return 0 if kind in (KIND_RESPONSE, KIND_ERROR) else 2
    except (OSError, ValueError, ConnectionError) as error:
        print(f"closed/error: {error}")
        return 3
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
