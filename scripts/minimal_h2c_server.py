#!/usr/bin/env python3
"""
一个最小的 h2c（明文 HTTP/2）服务端，用于反向验证 Go2Cpp 的 HTTP/2 客户端。

## 为什么手写而不是用库

环境里没有 nghttpd（需要 C++23 <print> 与 urlparse submodule），也没有
Node.js 的 http2 模块，也没有 Python 的 h2 包。为了得到一个**独立实现**来
验证"我的客户端能不能正确解析别人发的帧"，这里直接用 socket + struct
手工拼出合法的 HTTP/2 帧。

它只做三件事，但每一件都是协议必需的：
1. 读客户端前言 `PRI * HTTP/2.0\\r\\n\\r\\nSM\\r\\n\\r\\n`；
2. SETTINGS 交换（收对端的 SETTINGS，回自己的 SETTINGS 和对端的 ACK）；
3. 收到 HEADERS 后回响应：`:status: 200` 用 HPACK 静态表索引 8（单字节 0x88），
   再发一个 DATA 帧带 body 并置 END_STREAM。

## 用法

    python3 minimal_h2c_server.py [port] [--seconds N]

它会在 stdout 打印收到的请求头（已用极简 HPACK 解码，只认静态索引），
方便与客户端输出对照。
"""
import socket
import struct
import sys
import time

PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

# HPACK 静态表里我们可能收到的少量条目（索引 -> (name, value)）
# 只列客户端请求最常用的几个，够打印日志用。
STATIC = {
    1: (b":authority", b""),
    2: (b":method", b"GET"),
    3: (b":method", b"POST"),
    4: (b":path", b"/"),
    5: (b":path", b"/index.html"),
    6: (b":scheme", b"http"),
    7: (b":scheme", b"https"),
    8: (b":status", b"200"),
}


def make_frame(ftype: int, flags: int, stream_id: int, payload: bytes = b"") -> bytes:
    """组装一个 HTTP/2 帧：3 字节长度 + 1 字节类型 + 1 字节标志 + 4 字节流 id。"""
    return (
        struct.pack(">I", len(payload))[1:]
        + bytes([ftype, flags])
        + struct.pack(">I", stream_id & 0x7FFFFFFF)
        + payload
    )


def recv_exact(conn: socket.socket, need: int) -> bytes:
    """阻塞读到恰好 need 字节；对端关闭时抛 EOFError。"""
    chunks = []
    got = 0
    while got < need:
        chunk = conn.recv(need - got)
        if not chunk:
            raise EOFError("peer closed")
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def decode_integer(data: bytes, prefix_bits: int):
    """HPACK 前缀整数解码（RFC 7541 §5.1）。返回 (值, 消耗字节数)。"""
    mask = (1 << prefix_bits) - 1
    value = data[0] & mask
    if value < mask:
        return value, 1
    shift = 0
    index = 1
    while True:
        byte = data[index]
        index += 1
        value += (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            return value, index


def dump_headers(block: bytes) -> list:
    """
    极简 HPACK 解码：只处理索引字段（1xxxxxxx）与名字索引 + 原样值的字面量
    （01xxxxxx / 0000xxxx）。足够打印客户端请求头；遇到 Huffman 或动态表就跳过。
    """
    out = []
    offset = 0
    while offset < len(block):
        byte = block[offset]
        if byte & 0x80:  # 索引字段
            index, used = decode_integer(block[offset:], 7)
            offset += used
            if index in STATIC:
                name, value = STATIC[index]
                out.append((name.decode(errors="replace"),
                            value.decode(errors="replace")))
            else:
                out.append(("<index %d>" % index, ""))
        elif (byte & 0xC0) == 0x40 or (byte & 0xF0) == 0x00 or (byte & 0xF0) == 0x10:
            prefix = 6 if (byte & 0xC0) == 0x40 else 4
            name_index, used = decode_integer(block[offset:], prefix)
            offset += used
            if name_index in STATIC:
                name = STATIC[name_index][0]
            else:
                name = b"<name %d>" % name_index
            # 值：Huffman 位 + 7 位长度
            if offset >= len(block):
                break
            huffman = bool(block[offset] & 0x80)
            value_len, used = decode_integer(block[offset:], 7)
            offset += used
            raw = block[offset:offset + value_len]
            offset += value_len
            text = "<huffman %d bytes>" % len(raw) if huffman else raw.decode(errors="replace")
            out.append((name.decode(errors="replace") if isinstance(name, bytes) else name,
                        text))
        else:
            break
    return out


def serve(port: int, seconds: float) -> int:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(8)
    listener.settimeout(seconds)
    print("[python-h2c] 监听 127.0.0.1:%d" % port, flush=True)

    try:
        conn, _ = listener.accept()
    except socket.timeout:
        print("[python-h2c] 等待连接超时", flush=True)
        return 1

    conn.settimeout(seconds)
    try:
        # 1) 客户端前言
        preface = recv_exact(conn, len(PREFACE))
        if preface != PREFACE:
            print("[python-h2c] 前言不匹配: %r" % preface[:24], flush=True)
            return 2
        print("[python-h2c] 收到客户端前言", flush=True)

        # 2) 自己的 SETTINGS（空，表示全部用默认值）
        conn.sendall(make_frame(0x4, 0, 0, b""))
        print("[python-h2c] 已发送 SETTINGS", flush=True)

        # 3) 帧循环
        requests = 0
        deadline = time.time() + seconds
        while time.time() < deadline:
            header = recv_exact(conn, 9)
            length = int.from_bytes(header[0:3], "big")
            ftype = header[3]
            flags = header[4]
            stream_id = int.from_bytes(header[5:9], "big") & 0x7FFFFFFF
            payload = recv_exact(conn, length) if length else b""

            if ftype == 0x4:  # SETTINGS
                if not (flags & 0x1):
                    # 收到对端 SETTINGS，回 ACK
                    conn.sendall(make_frame(0x4, 0x1, 0, b""))
                    print("[python-h2c] 收到 SETTINGS 并回 ACK", flush=True)
            elif ftype == 0x6:  # PING
                if not (flags & 0x1):
                    conn.sendall(make_frame(0x6, 0x1, 0, payload))
                    print("[python-h2c] 收到 PING 并回 ACK", flush=True)
            elif ftype == 0x1:  # HEADERS
                requests += 1
                print("[python-h2c] 收到 HEADERS: stream=%d flags=0x%x len=%d"
                      % (stream_id, flags, length), flush=True)
                for name, value in dump_headers(payload):
                    print("[python-h2c]   %s: %s" % (name, value), flush=True)

                # 回响应：:status: 200 = 静态表索引 8 -> 0x88，END_HEADERS
                body = b"hello from python h2c server\n"
                conn.sendall(make_frame(0x1, 0x4, stream_id, b"\x88"))
                # 再发 DATA，END_STREAM
                conn.sendall(make_frame(0x0, 0x1, stream_id, body))
                print("[python-h2c] 已响应 stream=%d: :status 200 + %d 字节 body"
                      % (stream_id, len(body)), flush=True)
            elif ftype == 0x7:  # GOAWAY
                print("[python-h2c] 收到 GOAWAY", flush=True)
                break

        print("[python-h2c] 共处理 %d 个请求" % requests, flush=True)
        return 0 if requests > 0 else 3
    except (EOFError, socket.timeout) as exc:
        print("[python-h2c] 连接结束: %s" % exc, flush=True)
        return 0
    finally:
        conn.close()
        listener.close()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18081
    seconds = 20.0
    if "--seconds" in sys.argv:
        seconds = float(sys.argv[sys.argv.index("--seconds") + 1])
    sys.exit(serve(port, seconds))
