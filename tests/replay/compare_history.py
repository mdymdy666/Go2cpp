#!/usr/bin/env python3
"""compare_history.py —— 对照 `--view history` 与 demo 的事件环序列。

用法：
    compare_history.py <demo.log> <history.json> [limit]

demo.log 里 `recorded[i] ...` 行来自 `EventRing::Snapshot()`（与
`--view history` 的数据源相同）；history.json 是
`go2cpp-cli dump --pid <pid> --view history --resource <id>` 的输出。
两者都按 seq 升序，逐条比较 type/actor/target/result。
"""

import json
import re
import sys

TYPE_MAP = {
    "kMutexLock": "lock",
    "kMutexUnlock": "unlock",
    "res-mutex-lock": "lock",
    "res-mutex-unlock": "unlock",
}


def parse_demo(path, limit):
    pattern = re.compile(
        r"^recorded\[(\d+)\]\s+(lock|unlock)\s+resource=(\d+) "
        r"actor=(\d+) target=(\d+) result=(\w+)"
    )
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = pattern.match(line)
            if not match:
                continue
            rows.append(
                (
                    int(match.group(1)),
                    match.group(2),
                    int(match.group(3)),
                    int(match.group(4)),
                    int(match.group(5)),
                    match.group(6),
                )
            )
            if len(rows) >= limit:
                break
    return rows


def parse_history(path, resource, limit):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        data = json.load(handle)
    history = data.get("history", data)
    events = history.get("events", [])
    rows = []
    for event in events:
        # history 视图的事件字段是 `code`（SchedEventCodeName），不是 `type`。
        raw_type = event.get("code", event.get("type", ""))
        kind = TYPE_MAP.get(raw_type)
        if kind is None:
            continue
        if int(event.get("resource_id", 0)) != resource:
            continue
        rows.append(
            (
                int(event.get("index", len(rows))),
                kind,
                resource,
                int(event.get("actor_gid", 0)),
                int(event.get("target_gid", 0)),
                str(event.get("result_name", event.get("result", ""))),
            )
        )
        if len(rows) >= limit:
            break
    return rows


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: compare_history.py <demo.log> <history.json> [limit]")
        return 2
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    demo_rows = parse_demo(sys.argv[1], limit)
    if not demo_rows:
        print("FAIL: demo.log 中没有 recorded[i] 行")
        return 1
    resource = demo_rows[0][2]
    history_rows = parse_history(sys.argv[2], resource, limit)
    print(f"resource={resource} demo_rows={len(demo_rows)} "
          f"history_rows={len(history_rows)}")
    failures = 0
    for index in range(min(len(demo_rows), len(history_rows))):
        demo = demo_rows[index]
        history = history_rows[index]
        left = demo[1:]
        right = history[1:]
        if left == right:
            print(f"PASS: [{index}] demo={left} history={right}")
        else:
            print(f"FAIL: [{index}] demo={left} history={right}")
            failures += 1
    if len(history_rows) < len(demo_rows):
        print("FAIL: history 事件数少于 demo（history 视图可能未返回资源事件）")
        failures += 1
    if failures:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
