#!/usr/bin/env python3
"""stdio 端到端测试：启动真实 mcp_server 子进程，覆盖工具/提示/资源与错误路径。

用法：
    python3 test_stdio_e2e.py /path/to/mcp_server [/path/to/resource_dir]

退出码 0 表示全部通过。
"""

import json
import os
import select
import subprocess
import sys
import time

CASES = []


def case(func):
    CASES.append(func)
    return func


class StdioClient:
    def __init__(self, server_path, cwd, env=None):
        merged_env = dict(os.environ)
        if env:
            merged_env.update(env)
        self.notifications = []
        self._buffer = b""
        self._id = 0
        # 必须用二进制管道 + 自己的行缓冲：text 模式会在 TextIOWrapper 里预读，
        # select() 只看内核缓冲，预读过的数据会让后续读超时。
        self.proc = subprocess.Popen(
            [server_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
            env=merged_env,
            bufsize=0,
        )

    def _readline(self, timeout):
        while b"\n" not in self._buffer:
            ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
            if not ready:
                raise TimeoutError("等待响应超时")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError("server 提前退出")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def request(self, method, params=None, timeout=30.0):
        self._id += 1
        message = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            reply = self._readline(max(0.1, deadline - time.time()))
            if "method" in reply and "id" not in reply:
                self.notifications.append(reply)
                continue
            if reply.get("id") == message["id"]:
                return reply
        raise TimeoutError("没有收到匹配的响应")

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        stderr = self.proc.stderr.read().decode("utf-8", "replace")
        return self.proc.returncode, stderr


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: test_stdio_e2e.py <mcp_server> [资源目录]", file=sys.stderr)
        return 2
    server = sys.argv[1]
    resource_dir = sys.argv[2] if len(sys.argv) > 2 else None
    cwd = os.path.dirname(os.path.dirname(os.path.abspath(server))) if not resource_dir else os.getcwd()

    client = StdioClient(server, cwd, env={"MCP_RESOURCE_DIR": resource_dir} if resource_dir else None)
    failures = 0
    checks = 0

    def check(condition, label, detail=""):
        nonlocal failures, checks
        checks += 1
        if not condition:
            failures += 1
            print(f"FAIL: {label} {detail}")

    try:
        init = client.request("initialize", {})
        check(init["result"]["protocolVersion"] == "2025-06-18", "initialize 协议版本")
        check(init["result"]["capabilities"]["tools"] == {}, "initialize 能力声明")

        tools = client.request("tools/list")["result"]["tools"]
        names = {item["name"] for item in tools}
        expected = {
            "calculator", "add", "subtract", "multiply", "divide", "power", "sqrt", "factorial",
            "sleep", "get_weather", "logging_test", "progress_test",
            "ros2_node_list", "ros2_topic_list", "ros2_topic_info", "ros2_service_list",
            "ros2_service_info", "ros2_action_list", "ros2_param_list", "ros2_param_get",
            "ros2_command", "echo",
        }
        check(expected <= names, "tools/list 覆盖原项目全部工具", str(expected - names))

        prompts = client.request("prompts/list")["result"]["prompts"]
        check([item["name"] for item in prompts] == ["code-review"], "prompts/list")

        resources = client.request("resources/list")["result"]["resources"]
        uris = {item["uri"] for item in resources}
        check(uris == {"bacio:///quote", "document:///life-choices.txt"}, "resources/list", str(uris))

        # 计算类工具
        def call(name, arguments):
            return client.request("tools/call", {"name": name, "arguments": arguments})["result"]

        def text_of(result):
            return result["content"][0]["text"]

        check(text_of(call("calculator", {"expression": "(12+8)*3"})) == "60", "calculator 表达式")
        check(text_of(call("add", {"a": 17, "b": 25})) == "42", "add")
        check(text_of(call("subtract", {"a": 50, "b": 8})) == "42", "subtract")
        check(text_of(call("multiply", {"a": 6, "b": 7})) == "42", "multiply")
        check(text_of(call("divide", {"a": 84, "b": 2})) == "42", "divide")
        check(text_of(call("power", {"base": 2, "exponent": 6})) == "64", "power")
        check(text_of(call("sqrt", {"number": 144})) == "12", "sqrt")
        check(text_of(call("factorial", {"n": 5})) == "120", "factorial")

        # 错误路径
        check(call("divide", {"a": 1, "b": 0})["isError"], "divide 除零报错")
        check(call("sqrt", {"number": -1})["isError"], "sqrt 负数报错")
        # 校验层把「schema 约束不满足」提前到协议层：返回 -32602，而不是进入工具
        # 内部再返回 isError。这是本轮的语义变化，断言随之调整。
        out_of_range = client.request("tools/call", {"name": "factorial", "arguments": {"n": 21}})
        check(out_of_range.get("error", {}).get("code") == -32602, "factorial 越界由校验层拒绝")
        check(call("calculator", {"expression": "1+"})["isError"], "calculator 非法表达式")

        unknown = client.request("tools/call", {"name": "no_such_tool", "arguments": {}})
        check(unknown.get("error", {}).get("code") == -32001, "未知工具 -32001")

        # 通知类工具
        before = len(client.notifications)
        progress = client.request(
            "tools/call",
            {"name": "progress_test", "arguments": {}, "_meta": {"progressToken": "tok-1"}},
        )["result"]
        progress_notes = [n for n in client.notifications[before:] if n["method"] == "notifications/progress"]
        check(len(progress_notes) == 4, "progress_test 推送 4 条进度", str(len(progress_notes)))
        check(progress_notes[0]["params"]["progressToken"] == "tok-1", "progressToken 回填")
        check(not progress["isError"], "progress_test 结果")

        before = len(client.notifications)
        logging = client.request("tools/call", {"name": "logging_test", "arguments": {}})["result"]
        logging_notes = [n for n in client.notifications[before:] if n["method"] == "notifications/message"]
        check(len(logging_notes) == 1, "logging_test 推送 1 条 message")
        check(not logging["isError"], "logging_test 结果")

        no_token = client.request("tools/call", {"name": "progress_test", "arguments": {}})["result"]
        check(no_token["isError"], "progress_test 无 token 报错")

        # 提示
        prompt = client.request("prompts/get", {"name": "code-review", "arguments": {"language": "C++"}})
        text = prompt["result"]["messages"][0]["content"]["text"]
        check("C++" in text, "code-review prompt 内容")
        bad_prompt = client.request("prompts/get", {"name": "code-review", "arguments": {}})
        check(bad_prompt.get("error", {}).get("code") == -32602, "code-review 缺参数 -32602")

        # 资源
        quote = client.request("resources/read", {"uri": "bacio:///quote"})["result"]["contents"][0]
        check(quote["mimeType"] == "text/plain" and len(quote["text"]) > 10, "bacio quote")
        document = client.request("resources/read", {"uri": "document:///life-choices.txt"})["result"]["contents"][0]
        # 资源是一篇约 500 字的短故事：只校验非空且像完整文档，
        # 不用固定长度门槛（否则改一次文案就要改测试）。
        check(len(document["text"]) > 300, "document 资源长度", str(len(document["text"])))
        missing = client.request("resources/read", {"uri": "missing:///x"})
        check(missing.get("error", {}).get("code") == -32000, "未知资源报错")

        # ROS 工具：没有 ros2 时应返回 isError + JSON 载荷，而不是崩溃
        ros = call("ros2_node_list", {"timeout_ms": 3000})
        payload = json.loads(text_of(ros))
        check("exitCode" in payload and "timedOut" in payload, "ros2_node_list 结构")
        blocked = call("ros2_command", {"args": ["run", "demo", "node"]})
        check(blocked["isError"], "ros2_command 未授权变更被拒绝")

        # 未知方法
        unknown_method = client.request("no/such/method")
        check(unknown_method.get("error", {}).get("code") == -32601, "未知方法 -32601")
    finally:
        code, stderr = client.close()
        if stderr.strip():
            print("[server stderr]", stderr.strip()[:500], file=sys.stderr)

    print(f"[stdio_e2e] {checks} 项断言，失败 {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
