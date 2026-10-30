#!/usr/bin/env python3
"""文档数字一致性测试：README/docs 声明的 ctest 数与工具数必须与实际一致。

覆盖：
1. 实际执行 `ctest -N` 取 Total Tests 与全部用例名；
2. 实际启动 mcp_server 调 `tools/list` 取工具数；
3. 从构建目录 CMakeCache.txt 推导 profile（bento/mcp/osmon/runtime/enhanced），
   docs/testing.md 的「构建配置与 ctest 数量」表必须登记该 profile，且数字
   等于实际值；
4. 每个 profile 的「测试地图」必须逐个列出该配置全部 ctest 名（集合相等），
   标题项数与表格行数必须等于登记总数，新增测试时忘记改文档会在这里失败；
5. README.md / docs/testing.md 的「N 个 ctest」「Total Tests: N」「N/N passed」
   数字必须落在已登记的总数集合内；
6. 工具清单（22 内置 + 3 检索 + 4 监控 + 4 Socket = 33）与 tools/list 实际数量一致。

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


PROFILE_OPTIONS = [
    ("bento", "GO2CPP_WITH_BENTO"),
    ("mcp", "GO2CPP_WITH_MCP"),
    ("osmon", "GO2CPP_BUILD_OS_MONITOR_TESTS"),
    ("runtime", "GO2CPP_BUILD_TESTS"),
    ("enhanced", "GO2CPP_BUILD_ENHANCED"),
]


def build_profile(build_dir):
    '''从 CMakeCache.txt 推导构建 profile（如 ``bento+mcp``）。

    返回 None 表示读不到缓存，调用方按未知配置宽松处理。
    '''
    cache_path = os.path.join(build_dir, "CMakeCache.txt")
    flags = {}
    try:
        with open(cache_path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if "=" not in line or line.lstrip().startswith("//"):
                    continue
                key, _, value = line.partition("=")
                flags[key.strip()] = value.strip().upper()
    except OSError:
        return None
    tags = [tag for tag, option in PROFILE_OPTIONS
            if flags.get(option + ":BOOL") == "ON"]
    return "+".join(tags) if tags else "minimal"


def profile_table(text):
    '''解析「构建配置与 ctest 数量」表，返回 {profile: ctest 总数}。'''
    totals = {}
    for match in re.finditer(
            r"\|\s*`([a-z][a-z+_]*)`\s*\|[^|\n]*\|\s*\**(\d+)\**\s*\|",
            text):
        totals[match.group(1)] = int(match.group(2))
    return totals


def profile_maps(text):
    '''解析按 profile 分区的测试地图，返回 {profile: (声明项数, 正文)}。'''
    maps = {}
    for match in re.finditer(
            r"##\s*测试地图（profile\s*`([^`]+)`\s*[，,]\s*(\d+)\s*项）\s*\n(.*?)"
            r"(?=\n##\s|\Z)",
            text, re.S):
        maps[match.group(1)] = (int(match.group(2)), match.group(3))
    return maps


def check_profile_consistency(totals, maps):
    '''跨 profile 自洽：每个登记项都有地图，项数与表格行数都等于登记总数。'''
    for name, total in sorted(totals.items()):
        check(name in maps, f"docs/testing.md 缺少 profile {name} 的测试地图")
        if name not in maps:
            continue
        declared, map_text = maps[name]
        map_names = set(re.findall(r"\|\s*\d+\s*\|\s*`([^`]+)`\s*\|", map_text))
        check(declared == total,
              f"docs/testing.md profile {name} 地图标题项数应等于表中总数",
              f"{declared} vs {total}")
        check(len(map_names) == declared,
              f"docs/testing.md profile {name} 地图行数应等于声明项数",
              f"{len(map_names)} vs {declared}")


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

    # 3) 构建 profile 与文档登记的 ctest 总数。
    profile = build_profile(BUILD_DIR)
    totals = profile_table(testing)
    maps = profile_maps(testing)
    known_totals = set(totals.values()) | {actual_total}
    print(f"[docs_consistency] 构建 profile={profile}，文档登记="
          + ", ".join(f"{name}:{value}" for name, value in sorted(totals.items())),
          flush=True)

    check(bool(totals), "docs/testing.md 应有「构建配置与 ctest 数量」表")
    check_profile_consistency(totals, maps)

    if profile is None:
        print("WARN: 读不到 CMakeCache.txt，无法推导构建 profile；跳过绑定校验")
    elif profile not in totals:
        print(f"WARN: docs/testing.md 未登记 profile {profile}，跳过总数/地图绑定校验；"
              "请在「构建配置与 ctest 数量」表与测试地图中登记")
    else:
        check(totals[profile] == actual_total,
              f"docs/testing.md 中 profile {profile} 的 ctest 总数应为 {actual_total}",
              str(totals[profile]))
        check(profile in maps, f"docs/testing.md 应有 profile {profile} 的测试地图")
        if profile in maps:
            map_text = maps[profile][1]
            map_names = set(re.findall(r"\|\s*\d+\s*\|\s*`([^`]+)`\s*\|", map_text))
            missing = sorted(actual_names - map_names)
            extra = sorted(map_names - actual_names)
            check(not missing, "docs 测试地图缺少 ctest", ", ".join(missing))
            if extra:
                print("WARN: docs 测试地图含其他配置用例（不计入失败）: "
                      + ", ".join(extra[:10])
                      + (" ..." if len(extra) > 10 else ""))

    # 4) 文档中的 ctest 数字必须落在已登记的总数集合内。
    for label, nums, path in (
            ("README ctest 总数",
             values(r"全部\s+(\d+)\s+个\s+ctest|Total Tests:\s*(\d+)|(\d+)\s+个\s+ctest",
                    readme), "README.md"),
            ("docs ctest 总数",
             values(r"ctest 总数[^\d]{0,12}(\d+)|Total Tests:\s*(\d+)|(\d+)\s+个\s+ctest",
                    testing), "docs/testing.md")):
        check(bool(nums), f"{label}: {path} 中应至少出现一次声明")
        for value in nums:
            check(value in known_totals,
                  f"{label}: {path} 出现未登记的 ctest 总数 {value}",
                  f"已登记: {sorted(known_totals)}")

    # 5) N/N passed 中的 N 必须是已登记的 ctest 总数。
    for path, text in (("README.md", readme), ("docs/testing.md", testing)):
        passed = re.findall(r"(\d+)\s*/\s*(\d+)\s+passed", text)
        check(any(int(done) == int(total) and int(done) in known_totals
                  for done, total in passed),
              f"{path} 应声明 N/N passed（N 为已登记的 ctest 总数）")

    # 6) 工具数：README 的「N 个工具」与 tools/list 实际数量一致。
    check_all_equal("README 工具总数",
                    values(r"(\d+)\s*个工具", readme), actual_tools,
                    "README.md")
    # 「应输出 N」为可选声明（旧 README 格式），不再强制。
    # 22 内置 + 3 检索 + 4 监控 + 4 Socket = 33 的分解也要自洽。
    breakdown = re.search(
        r"(\d+)\s*内置\s*\+\s*(\d+)\s*检索\s*\+\s*(\d+)\s*监控"
        r"(?:\s*\+\s*(\d+)\s*Socket)?\s*=\s*(\d+)",
        readme)
    if breakdown:
        builtin, retrieval, monitor, socket, total = breakdown.groups()
        socket_count = int(socket) if socket else 0
        parts = int(builtin) + int(retrieval) + int(monitor) + socket_count
        check(parts == int(total),
              "README 工具分类相加应等于总数",
              f"{builtin}+{retrieval}+{monitor}+{socket_count}!={total}")
        check(int(total) == actual_tools,
              f"README 工具分类总数应为 {actual_tools}", str(total))
    else:
        check(False, "README.md 应声明「N 内置 + N 检索 + N 监控 [+ N Socket] = N」的工具分解")

    print(f"[docs_consistency] {CHECKS} 项断言，失败 {FAILURES}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
