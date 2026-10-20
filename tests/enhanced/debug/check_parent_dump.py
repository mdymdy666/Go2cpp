#!/usr/bin/env python3
"""检查父 Fiber park 后的 scheduler dump（P0 可见性/状态 + P2 父链缺口）。

输入是 `go2cpp-cli dump --sections scheduler` 的 JSON（aggregate），
输出结构化检查结果，退出码：
  0  父/子都在 tasks 且状态非终态；
  3  父或子缺失，或父状态为终态（P0）；
  4  父链/栈字段缺失（P2，仍返回 0 之外的独立码供测试区分）。
"""

import json
import sys


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: check_parent_dump.py <dump.json> <parent_gid> <child_gid>")
        return 2
    path, parent_gid, child_gid = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    doc = json.load(open(path, encoding="utf-8"))
    sched = doc.get("components", {}).get("scheduler", {})
    tasks = sched.get("tasks", [])
    by_id = {int(task.get("id", -1)): task for task in tasks}
    parent = by_id.get(parent_gid)
    child = by_id.get(child_gid)
    print(f"tasks={len(tasks)} parent_gid={parent_gid} child_gid={child_gid}")
    print(f"parent_present={1 if parent else 0} child_present={1 if child else 0}")
    if parent:
        print(f"parent_state={parent.get('state')} queued={parent.get('queued')} "
              f"started={parent.get('started')}")
    if child:
        print(f"child_state={child.get('state')} queued={child.get('queued')} "
              f"started={child.get('started')}")

    p0 = False
    if parent is None:
        print("P0 parent_missing_from_tasks")
        p0 = True
    elif parent.get("state") in ("kDead", "kCancelled", "kFailed"):
        print(f"P0 parent_terminal_state={parent.get('state')}")
        p0 = True
    elif parent.get("state") != "kWaiting":
        print(f"WARN parent_state={parent.get('state')} (expected kWaiting)")
    if child is None:
        print("P0 child_missing_from_tasks")
        p0 = True

    # P2：tasks schema 是否携带父链/栈。
    task_keys = set()
    for task in tasks:
        task_keys.update(task.keys())
    has_parent = "parent_id" in task_keys or "parent_gid" in task_keys
    has_stack = "stack" in task_keys or "has_stack" in task_keys
    print(f"tasks_have_parent_id={1 if has_parent else 0} "
          f"tasks_have_stack={1 if has_stack else 0}")
    if not has_parent or not has_stack:
        print("P2 scheduler.tasks 缺少 parent_id/stack；DumpFiber 无按 gid 查询入口")
    if p0:
        return 3
    return 4 if (not has_parent or not has_stack) else 0


if __name__ == "__main__":
    sys.exit(main())
