#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Go2Cpp `path` 模块的 1 万条路径对拍脚本。

做三件事：
1. 用固定种子生成 10000 条随机/边角路径（含 `..`、`.`、重复分隔符、空段、
   超长段、非 ASCII、反斜杠）；
2. 分别取 **Go `path.Clean`**（权威 oracle）、**本模块 `Clean`**、
   **Python `posixpath.normpath`**（第二 oracle）的结果；
3. 逐条比较，并把 Go 结果写成 `tests/path/testdata/go_path_clean_10000.txt`
   供 C++ 测试 `go2cpp_path_test` 离线复验（ctest 不依赖 Go）。

已知差异（**不是缺陷**）：POSIX 允许「恰好两个前导斜杠」有实现定义含义，
Python `posixpath.normpath("//a") == "//a"`，而 Go `path.Clean("//a") == "/a"`。
本模块对齐 Go，因此脚本把这类差异单独计数，其余差异必须为 0。

用法：
    python3 tests/path/scripts/cross_check.py
    python3 tests/path/scripts/cross_check.py --binary build-quality/go2cpp_path_test
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import posixpath
import random
import subprocess
import sys
import tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_BINARY = PROJECT_ROOT / "build-quality" / "go2cpp_path_test"
TESTDATA = PROJECT_ROOT / "tests" / "path" / "testdata" / "go_path_clean_10000.txt"
ORACLE = PROJECT_ROOT / "tests" / "path" / "oracle" / "clean_oracle.go"
COUNT = 10000
SEED = 20261018

# 与 Go path_test.go / 任务书一致的固定边角用例，先放进去保证覆盖。
EDGE_CASES = [
    "",
    ".",
    "..",
    "/",
    "//",
    "///",
    "////",
    "/.",
    "/..",
    "/../..",
    "a",
    "a/",
    "a//",
    "a//b",
    "a/./b",
    "a/../b",
    "../a",
    "../../a",
    "abc/def/ghi/../jkl",
    "abc/def/../../..",
    "/abc/def/../../..",
    "abc/def/../../../ghi/jkl/../../../mno",
    "abc/../../././../def",
    "//abc",
    "///abc",
    "//abc//",
    "a/..",
    "a/../..",
    "./",
    "../",
    "../../",
    "...",
    ".../...",
    "..a/../b",
    "a/..b",
    "a\\b/../c",
    "a\\b//c",
    "目录/./文件.txt",
    "目录//../文件.txt",
    "日本語/../../naïve",
    "αβγ/../emoji😀",
    "with space/./x",
]

SEGMENTS = [
    "",
    ".",
    "..",
    "a",
    "b",
    "conf",
    "app.ini",
    "x-y_z",
    "123",
    "file.cpp",
    ".hidden",
    "...",
    "..a",
    "a..",
    "with space",
    "back\\slash",
    "naïve",
    "目录",
    "文件.txt",
    "日本語",
    "αβγ",
    "emoji😀",
]


def random_path(rng: random.Random) -> str:
    """生成一条随机路径（可能不合法、可能为空、可能超长）。"""
    parts: list[str] = []
    for _ in range(rng.randint(0, 8)):
        roll = rng.random()
        if roll < 0.10:
            part = ""
        elif roll < 0.16:
            part = "x" * rng.randint(120, 260)  # 超长段
        else:
            part = rng.choice(SEGMENTS)
        parts.append(part)

    out = ""
    if rng.random() < 0.30:
        out += "/" * rng.randint(1, 4)
    for index, part in enumerate(parts):
        if index > 0:
            out += rng.choice(["/", "/", "/", "//", "///"])
        out += part
    if rng.random() < 0.20:
        out += "/" * rng.randint(1, 3)
    return out


def build_corpus() -> list[str]:
    rng = random.Random(SEED)
    cases = list(EDGE_CASES)
    seen = set(cases)
    while len(cases) < COUNT:
        candidate = random_path(rng)
        # 允许重复（随机测试本就该允许），但优先补足唯一路径以扩大覆盖。
        if candidate in seen and rng.random() < 0.8:
            continue
        seen.add(candidate)
        cases.append(candidate)
    return cases[:COUNT]


def run_go_oracle(corpus: str, env: dict[str, str]) -> list[str]:
    proc = subprocess.run(
        ["go", "run", str(ORACLE)],
        cwd=str(PROJECT_ROOT),
        input=corpus.encode("utf-8"),
        capture_output=True,
        env=env,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "Go oracle 运行失败:\n"
            + proc.stderr.decode("utf-8", errors="replace")
        )
    text = proc.stdout.decode("utf-8", errors="replace")
    results = text.split("\n")
    if results and results[-1] == "":
        results.pop()
    return results


def run_ours(binary: Path, corpus_file: Path) -> list[str]:
    proc = subprocess.run(
        [str(binary), "--clean-file", str(corpus_file)],
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"C++ Clean 运行失败 (exit={proc.returncode}):\n"
            + proc.stderr.decode("utf-8", errors="replace")
        )
    text = proc.stdout.decode("utf-8", errors="replace")
    results = text.split("\n")
    if results and results[-1] == "":
        results.pop()
    return results


def is_posix_double_slash(path: str) -> bool:
    return path.startswith("//") and not path.startswith("///")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY,
                        help="go2cpp_path_test 可执行文件路径")
    parser.add_argument("--write-testdata", action="store_true", default=True,
                        help="把 Go 结果写入 testdata（默认开启）")
    parser.add_argument("--no-write-testdata", dest="write_testdata",
                        action="store_false")
    args = parser.parse_args()

    if not args.binary.is_file():
        print(f"找不到测试可执行文件: {args.binary}", file=sys.stderr)
        print("先构建: cmake --build build-quality --target go2cpp_path_test",
              file=sys.stderr)
        return 2

    env = os.environ.copy()
    env.setdefault("GOCACHE", "/tmp/go2cpp_path_gocache")
    go_version = subprocess.run(
        ["go", "version"], capture_output=True, check=False, env=env
    )
    print("[cross-check] " + go_version.stdout.decode("utf-8", errors="replace").strip())

    cases = build_corpus()
    corpus = "\n".join(cases) + "\n"
    assert len(cases) == COUNT

    with tempfile.TemporaryDirectory(prefix="go2cpp_path_") as tmp:
        corpus_file = Path(tmp) / "corpus.txt"
        corpus_file.write_text(corpus, encoding="utf-8")

        go_results = run_go_oracle(corpus, env)
        ours = run_ours(args.binary, corpus_file)

    if len(go_results) != COUNT or len(ours) != COUNT:
        print(
            f"结果条数不符: corpus={COUNT} go={len(go_results)} "
            f"ours={len(ours)}",
            file=sys.stderr,
        )
        return 1

    ours_mismatch = 0
    py_mismatch = 0
    py_expected = 0
    first_ours: list[tuple[str, str, str]] = []
    first_py: list[tuple[str, str, str]] = []
    for path, go_clean, our_clean in zip(cases, go_results, ours):
        if our_clean != go_clean:
            ours_mismatch += 1
            if len(first_ours) < 20:
                first_ours.append((path, our_clean, go_clean))
        py_clean = posixpath.normpath(path)
        if py_clean != go_clean:
            if is_posix_double_slash(path):
                py_expected += 1
            else:
                py_mismatch += 1
                if len(first_py) < 20:
                    first_py.append((path, py_clean, go_clean))

    print(f"[cross-check] 语料: {COUNT} 条（固定种子 {SEED}）")
    print(f"[cross-check] 本模块 vs Go path.Clean: 不一致 {ours_mismatch} 条")
    print(f"[cross-check] Python posixpath.normpath vs Go: "
          f"已知 `//` 差异 {py_expected} 条，其它不一致 {py_mismatch} 条")

    for path, ours_value, go_value in first_ours:
        print(f"  [ours!=go] {path!r}: ours={ours_value!r} go={go_value!r}")
    for path, py_value, go_value in first_py:
        print(f"  [py!=go]   {path!r}: py={py_value!r} go={go_value!r}")

    if args.write_testdata and ours_mismatch == 0:
        target = TESTDATA
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp_target = target.with_suffix(target.suffix + ".tmp")
        with tmp_target.open("w", encoding="utf-8", newline="\n") as handle:
            for path, go_clean in zip(cases, go_results):
                handle.write(path + "\t" + go_clean + "\n")
        os.replace(tmp_target, target)
        print(f"[cross-check] 已写入 {target}")

    if ours_mismatch != 0 or py_mismatch != 0:
        print("[cross-check] FAILED", file=sys.stderr)
        return 1
    print("[cross-check] OK: 本模块与 Go 完全一致，Python 仅存在已知 `//` 差异")
    return 0


if __name__ == "__main__":
    sys.exit(main())
