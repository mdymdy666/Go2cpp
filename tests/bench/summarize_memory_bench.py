#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 run_memory_bench.sh 产出的原始 TSV 汇总成 JSON / 派生 TSV / Markdown 表。

原始 TSV 每个配置一份，列固定（见 fiber_vs_thread_memory.cpp 的 kHeader）：
    mode stack_kb phase n n_ok stack_effective_kb touched_kb rss_kb vsize_kb
    vdata_kb pss_kb anon_kb maxrss_kb threads note

派生口径（为什么这样分）：
    record_delta = created - base   —— 对象/记录/预留的固定开销（栈尚未触页）；
    stack_delta  = touched - created —— 触页带来的真实增长（每协程栈成本）；
    total_delta  = touched - base   —— 一个进程里 N 个单位的全部成本。
    per_unit_*   = total_delta / n_ok，便于与「每线程/每协程」直觉对照。

单位统一为 KB；n_ok 为 0 或字段不可用（-1）时输出 null 而不是 0，避免把
「没测到」伪装成「零成本」。
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import platform
import re
import resource
import sys
from pathlib import Path
from typing import Any

HEADER_FIELDS = [
    "mode", "stack_kb", "phase", "n", "n_ok", "stack_effective_kb", "touched_kb",
    "rss_kb", "vsize_kb", "vdata_kb", "pss_kb", "anon_kb", "maxrss_kb", "threads",
    "note",
]

NUMERIC_FIELDS = [
    "stack_kb", "n", "n_ok", "stack_effective_kb", "touched_kb", "rss_kb",
    "vsize_kb", "vdata_kb", "pss_kb", "anon_kb", "maxrss_kb", "threads",
]


def parse_tsv(path: Path) -> dict[str, dict[str, Any]]:
    """读取单个配置的 TSV，返回 {phase: row}。"""
    phases: dict[str, dict[str, Any]] = {}
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return phases
    for line in text.splitlines():
        if not line or line.startswith("mode\t"):
            continue
        parts = line.split("\t")
        if len(parts) != len(HEADER_FIELDS):
            continue
        row: dict[str, Any] = {}
        for key, raw in zip(HEADER_FIELDS, parts):
            if key in NUMERIC_FIELDS:
                try:
                    row[key] = int(raw)
                except ValueError:
                    row[key] = -1
            else:
                row[key] = raw
        phases[str(row["phase"])] = row
    return phases


def number(value: Any) -> float | None:
    """-1 表示不可用；统一转成 None。"""
    if value is None:
        return None
    if isinstance(value, (int, float)) and value >= 0:
        return float(value)
    return None


def delta(a: dict[str, Any], b: dict[str, Any], key: str) -> float | None:
    av, bv = number(a.get(key)), number(b.get(key))
    if av is None or bv is None:
        return None
    return bv - av


def per_unit(value: float | None, n_ok: int) -> float | None:
    if value is None or n_ok <= 0:
        return None
    return value / n_ok


def read_status(path: Path) -> dict[tuple[str, int, int], dict[str, Any]]:
    out: dict[tuple[str, int, int], dict[str, Any]] = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        mode, stack, n, rc, ms = parts[0], parts[1], parts[2], parts[3], parts[4]
        try:
            key = (mode, int(stack), int(n))
            out[key] = {"exit_code": int(rc), "elapsed_ms": int(ms)}
        except ValueError:
            continue
    return out


def build_report(logs: Path, status_path: Path, build_dir: str) -> dict[str, Any]:
    statuses = read_status(status_path)
    configs: list[dict[str, Any]] = []

    # 以 status 为准枚举配置；日志目录里额外的文件忽略（避免把旧日志混进来）。
    for (mode, stack, n), st in sorted(statuses.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        log_path = logs / f"{mode}_{stack}kb_{n}.tsv"
        phases = parse_tsv(log_path)
        base = phases.get("base", {})
        created = phases.get("created", {})
        touched = phases.get("touched", {})
        note = touched.get("note") or created.get("note") or base.get("note") or ""
        n_ok = int(touched.get("n_ok", created.get("n_ok", 0)) or 0)

        if st["exit_code"] == 0 and "touched" in phases:
            status = "OK"
        elif st["exit_code"] == 124:
            status = "TIMEOUT"
        elif st["exit_code"] < 0 or st["exit_code"] >= 128:
            status = "CRASH"
        else:
            status = "FAIL"

        total = {
            "rss_kb": delta(base, touched, "rss_kb"),
            "vsize_kb": delta(base, touched, "vsize_kb"),
            "vdata_kb": delta(base, touched, "vdata_kb"),
            "pss_kb": delta(base, touched, "pss_kb"),
            "anon_kb": delta(base, touched, "anon_kb"),
        }
        record = {
            "rss_kb": delta(base, created, "rss_kb"),
            "vsize_kb": delta(base, created, "vsize_kb"),
            "vdata_kb": delta(base, created, "vdata_kb"),
            "pss_kb": delta(base, created, "pss_kb"),
        }
        stack_delta = {
            "rss_kb": delta(created, touched, "rss_kb"),
            "vsize_kb": delta(created, touched, "vsize_kb"),
            "vdata_kb": delta(created, touched, "vdata_kb"),
            "pss_kb": delta(created, touched, "pss_kb"),
        }
        configs.append({
            "mode": mode,
            "stack_kb": stack,
            "n": n,
            "n_ok": n_ok,
            "status": status,
            "exit_code": st["exit_code"],
            "elapsed_ms": st["elapsed_ms"],
            "stack_effective_kb": touched.get("stack_effective_kb", created.get("stack_effective_kb", -1)),
            "touched_kb": touched.get("touched_kb", created.get("touched_kb", -1)),
            "note": note,
            "phases": {"base": base, "created": created, "touched": touched},
            "total_delta": total,
            "record_delta": record,
            "stack_delta": stack_delta,
            "per_unit": {
                key: per_unit(value, n_ok) for key, value in total.items()
            },
        })

    return {
        "generated_at": _dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "environment": {
            "platform": platform.platform(),
            "kernel": platform.release(),
            "machine": platform.machine(),
            "cpu_count": os.cpu_count(),
            "page_size_bytes": resource.getpagesize(),
            "build_dir": build_dir,
            "python": sys.version.split()[0],
        },
        "units": "KB（n_ok 为 0 或字段不可用的派生值为 null）",
        "configs": configs,
    }


def fmt(value: float | None, digits: int = 1) -> str:
    if value is None:
        return "n/a"
    return f"{value:.{digits}f}"


def markdown_tables(report: dict[str, Any]) -> str:
    configs = report["configs"]
    lines: list[str] = []
    lines.append(f"生成时间：{report['generated_at']}")
    env = report["environment"]
    lines.append(
        f"环境：{env['kernel']} / {env['machine']} / {env['cpu_count']} CPU / "
        f"页 {env['page_size_bytes']} B / 构建目录 {env['build_dir']}"
    )
    lines.append("")

    modes = sorted({c["mode"] for c in configs})
    stacks = sorted({c["stack_kb"] for c in configs})
    ns = sorted({c["n"] for c in configs})

    def matrix(title: str, getter) -> None:
        lines.append(f"### {title}")
        lines.append("")
        lines.append("| mode | stack_kb | " + " | ".join(f"n={n}" for n in ns) + " |")
        lines.append("|---|---:|" + "---:|" * len(ns))
        for mode in modes:
            for stack in stacks:
                cells = []
                for n in ns:
                    hit = next((c for c in configs if c["mode"] == mode
                                and c["stack_kb"] == stack and c["n"] == n), None)
                    cells.append(fmt(getter(hit)) if hit else "n/a")
                lines.append(f"| {mode} | {stack} | " + " | ".join(cells) + " |")
        lines.append("")

    matrix("每单位 VmRSS（KB/单位，touched-base 除以 n_ok）",
           lambda c: c["per_unit"]["rss_kb"])
    matrix("每单位 VmSize（KB/单位，含虚拟预留）",
           lambda c: c["per_unit"]["vsize_kb"])
    matrix("固定记录开销 VmRSS（KB，created-base，栈未触页）",
           lambda c: c["record_delta"]["rss_kb"])
    matrix("固定记录开销 VmSize（KB，created-base）",
           lambda c: c["record_delta"]["vsize_kb"])
    matrix("栈触页增量 VmRSS（KB，touched-created）",
           lambda c: c["stack_delta"]["rss_kb"])
    matrix("总 VmRSS 增量（KB，touched-base）",
           lambda c: c["total_delta"]["rss_kb"])

    bad = [c for c in configs if c["status"] != "OK"]
    lines.append("### 非 OK 配置")
    lines.append("")
    if not bad:
        lines.append("无。")
    else:
        lines.append("| mode | stack_kb | n | status | exit | note |")
        lines.append("|---|---:|---:|---|---:|---|")
        for c in bad:
            lines.append(
                f"| {c['mode']} | {c['stack_kb']} | {c['n']} | {c['status']} | "
                f"{c['exit_code']} | {c['note']} |"
            )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--logs", required=True, type=Path)
    parser.add_argument("--status", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    parser.add_argument("--out-tsv", required=True, type=Path)
    parser.add_argument("--out-md", required=True, type=Path)
    parser.add_argument("--build", default="")
    args = parser.parse_args()

    if not args.logs.is_dir():
        print(f"日志目录不存在：{args.logs}", file=sys.stderr)
        return 2

    report = build_report(args.logs, args.status, args.build)
    if not report["configs"]:
        print("没有可汇总的配置（status 为空）", file=sys.stderr)
        return 2

    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    # 派生 TSV：一行一个配置，便于 Excel/awk 二次处理。
    columns = [
        "mode", "stack_kb", "n", "n_ok", "status", "stack_effective_kb",
        "touched_kb", "record_rss_kb", "stack_rss_kb", "total_rss_kb",
        "per_unit_rss_kb", "record_vsize_kb", "stack_vsize_kb", "total_vsize_kb",
        "per_unit_vsize_kb", "note",
    ]
    rows = []
    for c in report["configs"]:
        rows.append([
            c["mode"], c["stack_kb"], c["n"], c["n_ok"], c["status"],
            c["stack_effective_kb"], c["touched_kb"],
            fmt(c["record_delta"]["rss_kb"]), fmt(c["stack_delta"]["rss_kb"]),
            fmt(c["total_delta"]["rss_kb"]), fmt(c["per_unit"]["rss_kb"]),
            fmt(c["record_delta"]["vsize_kb"]), fmt(c["stack_delta"]["vsize_kb"]),
            fmt(c["total_delta"]["vsize_kb"]), fmt(c["per_unit"]["vsize_kb"]),
            c["note"],
        ])
    lines = ["\t".join(columns)]
    lines.extend("\t".join(str(v) for v in row) for row in rows)
    args.out_tsv.write_text("\n".join(lines) + "\n", encoding="utf-8")

    md = markdown_tables(report)
    args.out_md.write_text(md + "\n", encoding="utf-8")
    print(md)
    print(f"JSON: {args.out_json}")
    print(f"派生 TSV: {args.out_tsv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
