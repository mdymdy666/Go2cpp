#!/usr/bin/env python3
"""分层改动验收测试：检索层 / 校验层 / 配置层 / 文档检索 + 轻量压测。

用法：
    python3 test_layers_e2e.py <mcp_server> [资源目录]

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
        self.proc.stdin.write((json.dumps(message) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        while True:
            response = self._readline(timeout)
            if response.get("id") == self._id:
                return response

    def call(self, name, arguments):
        return self.request("tools/call", {"name": name, "arguments": arguments})

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def run_suite(server, cwd, resource_dir):
    failures = 0
    checks = 0

    def check(condition, label, detail=""):
        nonlocal failures, checks
        checks += 1
        if not condition:
            failures += 1
            print(f"FAIL: {label} {detail}")

    client = StdioClient(server, cwd, env={"MCP_RESOURCE_DIR": resource_dir} if resource_dir else None)
    try:
        # 1. initialize 反映配置默认值。
        init = client.request("initialize", {})["result"]
        check(init["serverInfo"]["name"] == "go2cpp-mcp", "initialize 服务名", init["serverInfo"]["name"])

        # 2. 分层新增的工具都出现在 tools/list。
        names = {item["name"] for item in client.request("tools/list")["result"]["tools"]}
        for expected in ("search_tools", "knowledge_search", "server_stats"):
            check(expected in names, f"tools/list 含 {expected}", str(sorted(names)))

        # 3. 检索层：按语义找工具。
        search = json.loads(client.call("search_tools", {"query": "平方根"})["result"]["content"][0]["text"])
        retrieved = [item["name"] for item in search["tools"]]
        check("sqrt" in retrieved, "search_tools 命中 sqrt", str(retrieved))
        check(search["count"] >= 1, "search_tools count", str(search["count"]))

        # 4. 检索层：文档检索（resources 目录已索引）。
        knowledge = json.loads(
            client.call("knowledge_search", {"query": "阿九 鳞公 灰民", "top_k": 2})["result"]["content"][0]["text"])
        chunks = knowledge["chunks"]
        check(len(chunks) >= 1, "knowledge_search 返回片段", str(knowledge["count"]))
        if chunks:
            check(chunks[0]["uri"] == "document:///life_choices.txt", "片段来源 URI", chunks[0]["uri"])
            check("灰民" in chunks[0]["text"], "片段内容相关")

        # 5. 统计工具。
        stats = json.loads(client.call("server_stats", {})["result"]["content"][0]["text"])
        check(stats["tools"] >= 25, "server_stats tools", str(stats["tools"]))
        check(stats["document_chunks"] > 0, "server_stats 文档块", str(stats["document_chunks"]))
        check(stats["indexed_tools"] >= 25, "server_stats 索引工具", str(stats["indexed_tools"]))

        # 6. 校验层：schema 约束在协议层拒绝（-32602）。
        out_of_range = client.call("factorial", {"n": 21})
        check(out_of_range.get("error", {}).get("code") == -32602, "factorial 越界 -32602")
        missing = client.call("search_tools", {})
        check(missing.get("error", {}).get("code") == -32602, "search_tools 缺 query -32602")
        extra = client.call("search_tools", {"query": "x", "unknown_field": 1})
        check(extra.get("error", {}).get("code") == -32602, "search_tools 多余字段 -32602")

        # 7. 校验层：非法资源 URI。
        bad_uri = client.request("resources/read", {"uri": "not-a-uri"})
        check(bad_uri.get("error", {}).get("code") == -32602, "非法资源 URI -32602")

        # 8. 校验层不影响合法调用。
        add = client.call("add", {"a": 2, "b": 3})["result"]
        check(add["content"][0]["text"] == "5", "合法参数仍通过", add["content"][0]["text"])
    finally:
        client.close()

    # 9. 配置层：显式 --config 覆盖服务名。
    config_path = os.path.join(cwd, ".tmp_config_layer_test.ini")
    with open(config_path, "w", encoding="utf-8") as handle:
        handle.write("[mcp]\nserver.name=config-layer-test\nllm.model=deepseek-chat\n")
    try:
        configured = subprocess.Popen(
            [server, "--config", config_path],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd,
        )
        message = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        stdout, _ = configured.communicate((message + "\n").encode("utf-8"), timeout=30)
        reply = json.loads(stdout.decode("utf-8").splitlines()[0])
        check(reply["result"]["serverInfo"]["name"] == "config-layer-test", "配置层覆盖服务名",
              reply["result"]["serverInfo"]["name"])
    finally:
        try:
            os.remove(config_path)
        except OSError:
            pass

    # 10. 轻量压测：检索请求的吞吐与分位延迟（stdio 串行，单进程）。
    client = StdioClient(server, cwd, env={"MCP_RESOURCE_DIR": resource_dir} if resource_dir else None)
    try:
        client.request("initialize", {})
        latencies = []
        for _ in range(1000):
            started = time.perf_counter()
            client.call("search_tools", {"query": "计算平方根"})
            latencies.append((time.perf_counter() - started) * 1000.0)
        latencies.sort()
        p50 = latencies[len(latencies) // 2]
        p95 = latencies[int(len(latencies) * 0.95)]
        p99 = latencies[int(len(latencies) * 0.99)]
        throughput = 1000.0 / (sum(latencies) / 1000.0)
        print(f"[bench] search_tools 1000 次: 吞吐={throughput:.0f} req/s "
              f"P50={p50:.3f}ms P95={p95:.3f}ms P99={p99:.3f}ms")
        check(throughput > 2000, "检索吞吐 > 2000 req/s", f"{throughput:.0f}")
        check(p99 < 10.0, "P99 < 10ms", f"{p99:.3f}")
    finally:
        client.close()

    return failures, checks


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: test_layers_e2e.py <mcp_server> [资源目录]", file=sys.stderr)
        return 2
    server = os.path.abspath(sys.argv[1])
    resource_dir = sys.argv[2] if len(sys.argv) > 2 else None
    cwd = os.getcwd()

    failures, checks = run_suite(server, cwd, resource_dir)
    print(f"[layers_e2e] {checks} 项断言，失败 {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
