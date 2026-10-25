#!/usr/bin/env python3
"""假的 OpenAI 兼容服务端：第一次请求返回工具调用，第二次返回最终文本。

用于离线验证 agent 循环（不需要真实 API key / 外网）。
启动后先打印监听端口，再阻塞服务。
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length).decode("utf-8"))
        messages = body.get("messages", [])
        has_tool_result = any(item.get("role") == "tool" for item in messages)
        tools = body.get("tools", [])

        if not has_tool_result:
            # 一次返回两个 tool_call，用于验证 agent 的并行执行与按序回填。
            tool_calls = [
                {"id": "call_sqrt", "type": "function",
                 "function": {"name": "sqrt", "arguments": json.dumps({"number": 144})}},
                {"id": "call_add", "type": "function",
                 "function": {"name": "add", "arguments": json.dumps({"a": 1, "b": 2})}},
            ]
            if not tools:
                message = {"role": "assistant", "content": "没有可用工具"}
            else:
                message = {"role": "assistant", "content": None, "tool_calls": tool_calls}
        else:
            tool_msgs = [item for item in messages if item.get("role") == "tool"]
            joined = " | ".join(item.get("content", "") for item in tool_msgs)
            message = {"role": "assistant", "content": "工具返回: " + joined}

        payload = {
            "id": "chatcmpl-fake",
            "object": "chat.completion",
            "model": body.get("model", "fake"),
            "choices": [{"index": 0, "message": message, "finish_reason": "stop"}],
        }
        data = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):  # 静默
        pass


def main() -> int:
    server = HTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_port, flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
