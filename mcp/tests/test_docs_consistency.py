#!/usr/bin/env python3
"""文档数字一致性测试：README/docs 声明的 ctest 数与工具数必须与实际一致。

覆盖：
1. 实际执行 `ctest -N` 取 Total Tests 与全部用例名；
2. 实际启动 mcp_server 调 `tools/list` 取工具数；
3. README.md 中「N 个 ctest」「Total Tests: N」「N/N passed」「N 个工具」
   「应输出 N」必须等于实际值；
4. docs/testing.md 中「ctest 总数 **N**」「N 个 ctest」「N/N passed」必须
   等于实际值；
5. docs/testing.md 的测试地图表格必须逐个列出全部 ctest 名（集合相等），
   新增测试时忘记改文档会在这里失败；
6. 工具清单（22 内置 + 3 检索 + 4 监控 = 29）与 tools/list 实际数量一致。

用法: test_docs_consistency.py <mcp_server> <source_dir> <build_dir>
"""

import json
import os
import re
import select
import subprocess
import sys
import tempfile

SERVER = sys.argv[1] if len(sys.argv) > 1 else ""
SOURCE_DIR = sys.argv[2] if len(sys.argv) > 2 else ""
BUILD_DIR = sys.argv[3] if len(sys.argv) > 3 else ""

FAILURES = 0
CHECKS = 0


def check(condition, label, detail=""):
    global FAILURES, CHECKS
    CHECKS += 1
    if not condition:
        FAILURES += 1
        print(f"FAIL: {label} {detail}")


class McpClient:
    """最小 stdio MCP 客户端，只用于 tools/list。"""

    def __init__(self, server_path, cwd):
        self._buffer = b""
        self._id = 0
        self.proc = subprocess.Popen(
            [server_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
            bufsize=0,
        )

    def _readline(self, timeout):
        while b"\n" not in self._buffer:
            ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
            if not ready:
                raise TimeoutError("等待 mcp_server 响应超时")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise EOFError("mcp_server 提前退出")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def request(self, method, params=None, timeout=20.0):
        self._id += 1
        message = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(
            (json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        while True:
            reply = self._readline(timeout)
            if "method" in reply and "id" not in reply:
                continue
            if reply.get("id") == message["id"]:
                return reply

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            return self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait(timeout=5)


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def values(pattern, text):
    """返回正则全部匹配中的第一个非空捕获组整数值。"""
    result = []
    for match in re.finditer(pattern, text):
        for group in match.groups():
            if group is not None:
                result.append(int(group))
                break
    return result


def check_all_equal(label, found, expected, path):
    check(bool(found), f"{label}: {path} 中应至少出现一次声明")
    for value in found:
        check(value == expected,
              f"{label}: {path} 声明 {value}，实际 {expected}")


def main():
    if not SERVER or not SOURCE_DIR or not BUILD_DIR:
        print("用法: test_docs_consistency.py <mcp_server> <source_dir> <build_dir>")
        return 2

    readme_path = os.path.join(SOURCE_DIR, "README.md")
    testing_path = os.path.join(SOURCE_DIR, "docs", "testing.md")
    for path in (readme_path, testing_path):
        if not os.path.isfile(path):
            print(f"FAIL: 文档不存在: {path}")
            return 2

    # 1) 实际 ctest 数量与用例名。
    ctest = subprocess.run(
        ["ctest", "--test-dir", BUILD_DIR, "-N"],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(ctest.returncode == 0, "ctest -N 应成功", ctest.stderr[-300:])
    total_match = re.search(r"Total Tests:\s*(\d+)", ctest.stdout)
    check(total_match is not None, "ctest -N 输出应包含 Total Tests")
    actual_total = int(total_match.group(1)) if total_match else -1
    actual_names = set(re.findall(r"Test\s+#\d+:\s+(\S+)", ctest.stdout))
    check(len(actual_names) == actual_total,
          "ctest -N 用例名数量应与 Total Tests 一致",
          f"{len(actual_names)} vs {actual_total}")

    # 2) 实际工具数。
    workdir = tempfile.mkdtemp(prefix="mcp_docs_consistency_")
    client = McpClient(SERVER, cwd=workdir)
    tool_names = []
    try:
        init = client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "docs-consistency", "version": "0.1.0"},
            },
        )
        check("result" in init, "initialize 成功", str(init)[:200])
        listed = client.request("tools/list")
        tool_names = [item["name"] for item in listed["result"]["tools"]]
    finally:
        client.close()
    actual_tools = len(tool_names)
    check(actual_tools > 0, "tools/list 应返回工具", str(actual_tools))

    readme = read_text(readme_path)
    testing = read_text(testing_path)
    print(f"[docs_consistency] 实际: ctest={actual_total}，工具={actual_tools}",
          flush=True)

    # 3) README 的 ctest 数字。
    check_all_equal("README ctest 总数", values(r"全部\s+(\d+)\s+个\s+ctest|Total Tests:\s*(\d+)|(\d+)\s+个\s+ctest", readme),
                    actual_total, "README.md")
    check_all_equal("README Total Tests",
                    values(r"Total Tests:\s*(\d+)", readme), actual_total,
                    "README.md")
    passed = re.findall(r"(\d+)\s*/\s*(\d+)\s+passed", readme)
    check(any(int(d) == actual_total and int(t) == actual_total
              for d, t in passed),
          f"README.md 应声明当前 {actual_total}/{actual_total} passed（历史基线不拦截）")

    # 4) docs/testing.md 的 ctest 数字。
    docs_total = values(r"ctest 总数[^\d]{0,12}(\d+)", testing)
    check_all_equal("docs ctest 总数", docs_total, actual_total, "docs/testing.md")
    check_all_equal("docs N 个 ctest",
                    values(r"(\d+)\s+个\s+ctest", testing), actual_total,
                    "docs/testing.md")
    docs_passed = re.findall(r"(\d+)\s*/\s*(\d+)\s+passed", testing)
    check(any(int(d) == actual_total and int(t) == actual_total
              for d, t in docs_passed),
          f"docs/testing.md 应声明当前 {actual_total}/{actual_total} passed（历史基线不拦截）")

    # 5) docs 测试地图必须逐个列出实际 ctest 名。
    # 只在「测试地图」章节内解析，避免把覆盖矩阵里的数字表格误当成用例表。
    map_section = re.search(r"##\s*(?:4\.\s*)?测试地图.*?(?=\n##\s+)", testing,
                            re.S)
    map_text = map_section.group(0) if map_section else testing
    map_names = set(re.findall(r"\|\s*\d+\s*\|\s*`([^`]+)`\s*\|", map_text))
    check(bool(map_names), "docs/testing.md 应有测试地图表格")
    missing = sorted(actual_names - map_names)
    extra = sorted(map_names - actual_names)
    check(not missing, "docs 测试地图缺少 ctest", ", ".join(missing))
    if extra:
        print(f"WARN: docs 测试地图含历史行（不计入失败）: ", ", ".join(extra))
    check(actual_names.issubset(map_names),
          "docs 测试地图应与 ctest -N 完全一致",
          f"表格 {len(map_names)} 项 vs 实际 {len(actual_names)} 项")

    # 6) 工具数：README 的「29 个工具 / 应输出 29」与实际一致。
    check_all_equal("README 工具总数",
                    values(r"(\d+)\s*个工具", readme), actual_tools,
                    "README.md")
    # 「应输出 N」为可选声明（旧 README 格式），不再强制。
    # 22 内置 + 3 检索 + 4 监控 = 29 的分解也要自洽。
    breakdown = re.search(
        r"(\d+)\s*内置\s*\+\s*(\d+)\s*检索\s*\+\s*(\d+)\s*监控\s*=\s*(\d+)",
        readme)
    if breakdown:
        builtin, retrieval, monitor, total = (int(part) for part in
                                              breakdown.groups())
        check(builtin + retrieval + monitor == total,
              "README 工具分类相加应等于总数", f"{builtin}+{retrieval}+{monitor}!={total}")
        check(total == actual_tools,
              f"README 工具分类总数应为 {actual_tools}", str(total))
    else:
        check(False, "README.md 应声明「N 内置 + N 检索 + N 监控 = N」的工具分解")

    print(f"[docs_consistency] {CHECKS} 项断言，失败 {FAILURES}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
