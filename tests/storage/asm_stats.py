#!/usr/bin/env python3
"""按函数统计 objdump 指令数 / call 数 / 栈帧 / canary / mutex / 堆分配。

汇编级取证用（见 /tmp/sqlite_asm.md）。

用法:
    python3 /tmp/asm_stats.py <binary> [func-substring ...]

不给 func-substring 时输出全部 `go2cpp::storage` 与 anonymous namespace
函数——后者正好包含 bench_db.cpp 里的原生 sqlite3 对照函数
（`InsertWithNative` 等），因此可以同二进制、同优化等级比较封装与原生。
"""
import re
import subprocess
import sys

FUNC_RE = re.compile(r"^([0-9a-f]+) <(.+)>:$")
INS_RE = re.compile(r"^\s+[0-9a-f]+:\t")


def collect(path):
    out = subprocess.run(
        ["objdump", "-d", "-C", "--no-show-raw-insn", path],
        capture_output=True, text=True, errors="replace").stdout
    funcs = {}
    cur = None
    for line in out.splitlines():
        match = FUNC_RE.match(line)
        if match:
            cur = match.group(2)
            funcs[cur] = []
            continue
        if cur is None:
            continue
        if INS_RE.match(line):
            funcs[cur].append(line.split("\t", 1)[1].strip())
    return funcs


def stats(funcs, wanted):
    result = []
    for name, ins in funcs.items():
        if wanted and not any(w in name for w in wanted):
            continue
        if not wanted and "go2cpp::storage" not in name and \
                "anonymous namespace" not in name:
            continue
        calls = [i for i in ins if i.startswith("call")]
        frame = 0
        for i in ins:
            matched = re.match(r"sub\s+\$0x([0-9a-f]+),%rsp", i)
            if matched:
                frame = int(matched.group(1), 16)
                break
        result.append({
            "name": name,
            "ins": len(ins),
            "calls": len(calls),
            "frame": frame,
            "canary": any("fs:0x28" in i for i in ins),
            "mutex": sum(1 for i in ins if "pthread_mutex" in i),
            "alloc": sum(1 for i in ins
                         if "malloc" in i or "operator new" in i),
        })
    return result


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    funcs = collect(sys.argv[1])
    rows = stats(funcs, sys.argv[2:])
    rows.sort(key=lambda row: row["name"])
    print(f"# binary: {sys.argv[1]}")
    print(f"{'function':64s} {'ins':>6s} {'call':>5s} {'frame':>7s} "
          f"{'canary':>6s} {'mutex':>5s} {'alloc':>5s}")
    for row in rows:
        print(f"{row['name'][:64]:64s} {row['ins']:6d} {row['calls']:5d} "
              f"{row['frame']:7d} {str(row['canary']):>6s} {row['mutex']:5d} "
              f"{row['alloc']:5d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
