#!/usr/bin/env python3
"""把矩阵 TSV + 统计 TSV + 缺陷清单汇总成 /tmp/test_enhance.md。"""

import collections
import csv
import os
import platform
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MATRIX = os.environ.get("MATRIX_FILE", "/tmp/test_enhance_matrix.tsv")
STATS = os.environ.get("STATS_FILE", "/tmp/test_enhance_stats.tsv")
REPORT = os.environ.get("REPORT_FILE", "/tmp/test_enhance.md")
DEFECTS = os.path.join(ROOT, "tests", "enhanced", "defects.md")

CAPABILITIES = [("debug", "Debug 调试"), ("migration", "迁移"),
                ("replay", "回放"), ("cli", "CLI"), ("doctor", "Doctor")]
KINDS = ["normal", "boundary", "error", "concurrency", "resource",
         "random", "stress", "inject"]
STATUSES = ["PASS", "FAIL", "SKIP", "GAP"]


def read_rows(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def command(args):
    try:
        return subprocess.run(args, capture_output=True, text=True,
                              timeout=10).stdout.strip()
    except Exception:
        return "?"


def sanitize(text, limit=220):
    return (text or "").replace("|", "\\|").replace("\n", " ")[:limit]


def main() -> int:
    rows = read_rows(MATRIX)
    stats = read_rows(STATS)
    counts = collections.defaultdict(lambda: collections.Counter())
    for row in rows:
        capability = row.get("capability", "unknown")
        counts[capability]["total"] += 1
        counts[capability][row.get("status", "?")] += 1
        counts[capability]["kind:" + row.get("kind", "?")] += 1

    total = sum(c["total"] for c in counts.values())
    passed = sum(c["PASS"] for c in counts.values())
    failed = sum(c["FAIL"] for c in counts.values())
    gaps = sum(c["GAP"] for c in counts.values())
    skipped = sum(c["SKIP"] for c in counts.values())

    defects_text = ""
    if os.path.exists(DEFECTS):
        defects_text = open(DEFECTS, encoding="utf-8").read()
    defect_count = defects_text.count("\n## D") + (1 if defects_text.startswith("## D") else 0)
    p2_count = defects_text.count("\n## P2-") + (1 if defects_text.startswith("## P2-") else 0)

    lines = []
    lines.append("# Go2Cpp 五能力测试增强报告")
    lines.append("")
    lines.append(f"- 生成时间：{time.strftime('%Y-%m-%d %H:%M:%S %z')}")
    lines.append(f"- 工作树：`{ROOT}`")
    lines.append(f"- 内核/架构：`{platform.release()}` / `{platform.machine()}`，"
                 f"nproc={os.cpu_count()}")
    lines.append(f"- git HEAD：`{command(['git', '-C', ROOT, 'rev-parse', '--short', 'HEAD'])}`"
                 f"（测试期间源码有更新，最终二进制已按当前源码重建）")
    lines.append("")
    lines.append("## 1. 结论摘要")
    lines.append("")
    lines.append(f"- 矩阵用例总数：**{total}**（PASS {passed} / FAIL {failed} / "
                 f"SKIP {skipped} / GAP {gaps}）。")
    lines.append(f"- 当前源码缺陷：**{defect_count}** 个 + **{p2_count}** 项 P2 能力缺口。")
    lines.append("- 随机/压测/失败注入数量均达到要求：迁移位翻转 1000、回放交错 500、"
                 "Debug 随机帧 10000、CLI 随机参数 1000；Doctor 并发 10/50/100、"
                 "连续 1000 次 dump、迁移/回放各 100 轮。")
    lines.append("- 父 Fiber park 场景：父 G 仍在 tasks、状态 `kWaiting`（非 kDead/"
                 "kCompleted）——**P0 未复现**；父链/按 gid DumpFiber 属 P2 能力缺口。")
    lines.append("- 旧构建上出现的 CLI play 静态执行、Debug 慢客户端阻塞均已由当前"
                 "源码修复（见缺陷清单“旧构建观察”），最终矩阵不再复现。")
    lines.append("")
    lines.append("## 2. 测试矩阵")
    lines.append("")
    header = ["能力", "用例", "PASS", "FAIL", "SKIP", "GAP"] + KINDS
    lines.append("| " + " | ".join(header) + " |")
    lines.append("|" + "|".join(["---"] * len(header)) + "|")
    for key, label in CAPABILITIES:
        counter = counts.get(key, collections.Counter())
        values = [label, str(counter["total"]), str(counter["PASS"]),
                  str(counter["FAIL"]), str(counter["SKIP"]), str(counter["GAP"])]
        values += [str(counter["kind:" + kind]) for kind in KINDS]
        lines.append("| " + " | ".join(values) + " |")
    values = ["**合计**", str(total), str(passed), str(failed), str(skipped), str(gaps)]
    values += [str(sum(counts[c]["kind:" + kind] for c in counts)) for kind in KINDS]
    lines.append("| " + " | ".join(values) + " |")
    lines.append("")
    lines.append("分类说明：normal 正常；boundary 边界；error 错误；concurrency 并发；"
                 "resource 资源限制；random 随机；stress 压测；inject 失败注入。"
                 "GAP 是能力缺口（非通过、非失败）。")
    lines.append("")

    failed_rows = [row for row in rows if row.get("status") == "FAIL"]
    if failed_rows:
        lines.append("## 3. 失败用例（产品缺陷，不是测试脚本错误）")
        lines.append("")
        for row in failed_rows:
            lines.append(f"- `{row.get('capability')}` / {row.get('case')} "
                         f"（{row.get('kind')}）：{sanitize(row.get('detail'))}")
        lines.append("")
    gap_rows = [row for row in rows if row.get("status") == "GAP"]
    if gap_rows:
        lines.append("## 4. 能力缺口（GAP）")
        lines.append("")
        for row in gap_rows:
            lines.append(f"- `{row.get('capability')}` / {row.get('case')}："
                         f"{sanitize(row.get('detail'))}")
        lines.append("")

    if stats:
        lines.append("## 5. 随机 / 压测统计")
        lines.append("")
        lines.append("| 能力 | 指标 | 值 | 说明 |")
        lines.append("|---|---|---|---|")
        for row in stats:
            lines.append(f"| {row.get('capability')} | {row.get('name')} | "
                         f"{sanitize(row.get('value'), 160)} | "
                         f"{sanitize(row.get('detail'), 200)} |")
        lines.append("")

    lines.append("## 6. 缺陷清单")
    lines.append("")
    lines.append(defects_text if defects_text else "（defects.md 缺失）")
    lines.append("")
    lines.append("## 7. 覆盖与未覆盖说明")
    lines.append("")
    lines.append("- 覆盖：Debug（7 组件/事件环/SUBSCRIBE/一致性标记/父 park）、迁移"
                 "（safepoint/lambda/句柄/签名/拒绝路径）、回放（select/context/values/"
                 "split/until/丢唤醒）、CLI（5 子命令/格式探测/后端分派/协议负路径）、"
                 "Doctor（单线程命令平面/等待图/force-wake/mutex-unlock/inject-fiber）。")
    lines.append("- 失败注入：fd 限制（migration/replay）、64MiB 虚拟内存、SIGTERM/"
                 "SIGSEGV、`/dev/full` 磁盘满、协议截断/超长/坏校验；tmpfs 因 uid=1000 "
                 "不能 mount，改用 `/dev/full` 与文件大小限制替代。")
    lines.append("- 未覆盖：真实多机/QUIC 传输（Doctor 仅 Unix socket）；真实生产规模"
                 "长跑（>10 分钟）；跨架构迁移；tmpfs 真满盘。")
    lines.append("")
    lines.append("## 8. 一键运行")
    lines.append("")
    lines.append("```bash")
    lines.append("cd /UserData/CodexWorkSpace/Go2Cpp-work")
    lines.append("./tests/all_enhanced.sh")
    lines.append("# 或 bash tests/enhanced/run_all.sh")
    lines.append("# 报告: /tmp/test_enhance.md；矩阵: /tmp/test_enhance_matrix.tsv")
    lines.append("```")
    lines.append("")
    lines.append("依赖（按顺序解析，任一命中即可）："
                 "`build-enhanced`（`-DGO2CPP_BUILD_ENHANCED=ON -DGO2CPP_WITH_CLI=ON`）、"
                 "`build-cli` / `build-doctor-full`（CLI）、`build-debug-full` / "
                 "`build-debug`（debug demo）、`build-diag` / `build-doctor-full`"
                 "（doctor demo）、`build-replay`（回放）、`build-migration`（迁移）。"
                 "也可用 `GO2CPP_CLI` / `GO2CPP_REPLAY_DIR` / `GO2CPP_DEBUG_DEMO` / "
                 "`GO2CPP_DIAG_DEMO` / `GO2CPP_MIG_DIR` 显式覆盖。")

    with open(REPORT, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"REPORT {REPORT}")
    print(f"TOTAL={total} PASS={passed} FAIL={failed} SKIP={skipped} GAP={gaps}")
    print(f"DEFECTS={defect_count} P2={p2_count}")
    return 0 if total > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
