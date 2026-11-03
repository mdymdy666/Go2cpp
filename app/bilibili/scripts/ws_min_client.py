#!/usr/bin/env python3
"""最小 WebSocket 客户端（标准库实现），供弹幕 demo / 压测复用。"""

import base64
import json
import os
import socket
import struct
import time


class WsError(RuntimeError):
    pass


class WsClient:
    """RFC 6455 文本帧客户端：握手 + 发送（带掩码）+ 接收（服务端不带掩码）。"""

    def __init__(self, sock, initial=b""):
        self.sock = sock
        self.buffer = bytearray(initial)
        self.closed = False

    def _read_exact(self, size):
        while len(self.buffer) < size:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise WsError("连接被关闭")
            self.buffer += chunk
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def send_json(self, obj):
        payload = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        header = bytearray([0x81])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        header += mask
        self.sock.sendall(bytes(header) + masked)

    def recv_text(self):
        while True:
            first, second = self._read_exact(2)
            opcode = first & 0x0F
            masked = bool(second & 0x80)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read_exact(8))[0]
            mask = self._read_exact(4) if masked else b""
            payload = self._read_exact(length)
            if masked:
                payload = bytes(byte ^ mask[index % 4]
                                for index, byte in enumerate(payload))
            if opcode == 0x8:
                self.closed = True
                raise WsError("收到 Close 帧")
            if opcode in (0x9, 0xA):
                continue
            return payload.decode("utf-8", "replace")

    def recv_json(self, timeout):
        self.sock.settimeout(timeout)
        try:
            return json.loads(self.recv_text())
        except socket.timeout:
            return None

    def close(self):
        try:
            # 发一个带掩码的 Close 帧（opcode 0x8），让服务端走正常关闭流程。
            self.sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
        except OSError:
            pass
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def ws_connect(host, port, path, timeout=5.0):
    """完成 HTTP Upgrade 握手，返回 (socket, 握手后残留字节)。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode("ascii"))
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise WsError("握手响应为空")
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    if not head.startswith(b"HTTP/1.1 101"):
        raise WsError("握手失败: " + head.decode("latin-1", "replace"))
    return sock, rest
