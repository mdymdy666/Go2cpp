#!/usr/bin/env python3
"""go2cpp-cli 随机参数 fuzz（1000 组）。

约束：
  - 任何退出码都必须在 CLI 稳定契约 {0,1,2,3,4,5} 内；
  - 不允许超时（124）、信号（<0 或 >=128）；
  - 不执行任意系统可执行文件：replay record 的 <exe> 只会是临时目录里的
    fake 脚本或不存在路径。
"""

import argparse
import os
import random
import subprocess
import sys
import time

ALLOWED = {0, 1, 2, 3, 4, 5}
SUBCOMMANDS = ["dump", "status", "debug", "replay", "migrate",
               "help", "--help", "-h", "--version", ""]
SECTION_VALUES = ["all", "scheduler", "locks", "goroutines", "events", "fiber",
                  "bogus", "scheduler,events", ""]
FORMATS = ["json", "text", "xml", ""]
PIDS = ["0", "1", "-1", "abc", "999999999", "2147483648", ""]
FILES = ["/tmp/nonexistent.g2cr", "/tmp/nonexistent.mig", "",
         "/tmp", "/dev/null"]


def random_argv(rng: random.Random) -> list[str]:
    command = rng.choice(SUBCOMMANDS)
    argv = [] if not command else [command]
    if command == "replay":
        argv.append(rng.choice(["play", "record", "info", "bogus"]))
    if command == "migrate":
        argv.append(rng.choice(["dump", "restore", "bogus"]))
    for _ in range(rng.randint(0, 5)):
        kind = rng.randrange(10)
        if kind == 0:
            argv += ["--pid", rng.choice(PIDS)]
        elif kind == 1:
            argv += ["--gid", rng.choice(PIDS)]
        elif kind == 2:
            argv += ["--out", rng.choice(FILES)]
        elif kind == 3:
            argv += ["--format", rng.choice(FORMATS)]
        elif kind == 4:
            argv += ["--sections", rng.choice(SECTION_VALUES)]
        elif kind == 5:
            argv += ["--json"]
        elif kind == 6:
            argv += ["--timeout-ms", rng.choice(["0", "1", "abc", "600001", "5000"])]
        elif kind == 7:
            argv += ["--socket", rng.choice(["/tmp/nonexistent.sock", "/tmp"])]
        elif kind == 8:
            argv += ["--concurrency", rng.choice(["0", "1", "256", "257", "abc"])]
        else:
            argv.append(rng.choice(["--bogus", "-x", "extra", "--", "-", ""]))
    return argv


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20261020)
    parser.add_argument("--fake-socket", default="")
    parser.add_argument("--fixtures", default="")
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    base_env = dict(os.environ)
    if args.fake_socket:
        base_env["GO2CPP_DEBUG_SOCKET"] = args.fake_socket
    if args.fixtures:
        base_env["CLI_FIXTURES"] = args.fixtures
    counts = {}
    failures = []
    started = time.monotonic()
    for index in range(args.count):
        env = dict(base_env)
        if rng.random() < 0.3:
            env["GO2CPP_DEBUG_SOCKET"] = "/tmp/nonexistent-" + str(index) + ".sock"
        if rng.random() < 0.5:
            env["GO2CPP_REPLAY_TOOL"] = "/bin/true"
            env["GO2CPP_MIGRATE_TOOL"] = "/bin/true"
        argv = random_argv(rng)
        try:
            result = subprocess.run(
                [args.bin] + argv, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=args.timeout, env=env)
        except subprocess.TimeoutExpired:
            failures.append((index, argv, "timeout"))
            counts["timeout"] = counts.get("timeout", 0) + 1
            continue
        code = result.returncode
        counts[str(code)] = counts.get(str(code), 0) + 1
        if code not in ALLOWED:
            failures.append((index, argv, code))
    elapsed_ms = (time.monotonic() - started) * 1000.0
    print(f"RESULT count={args.count} elapsed_ms={elapsed_ms:.1f} "
          f"exit_distribution={counts} failures={len(failures)}")
    for index, argv, code in failures[:8]:
        print(f"FAILURE #{index} code={code} argv={argv}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
