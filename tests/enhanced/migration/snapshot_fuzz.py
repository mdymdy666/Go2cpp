#!/usr/bin/env python3
"""迁移快照随机位翻转压测（只读被测二进制，不修改源码）。

对一份已签名的 G2CMIG03 快照做 N 次随机单比特翻转，逐个调用
`migrate restore`，统计：被拒绝 / 被接受（潜在完整性问题）/ 崩溃 / 超时。
任何一次被接受或崩溃都返回非 0。

用法：
  python3 snapshot_fuzz.py bitflip --input SNAP --migrate BIN --count 1000
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time


def bitflip(args: argparse.Namespace) -> int:
    data = bytearray(open(args.input, "rb").read())
    if not data:
        print("RESULT error=empty-input")
        return 2
    rng = random.Random(args.seed)
    workdir = args.workdir or tempfile.mkdtemp(prefix="migration-bitflip-")
    os.makedirs(workdir, exist_ok=True)
    target = os.path.join(workdir, "flip.mig")
    rejected = 0
    accepted = 0
    crashed = 0
    timed_out = 0
    samples = []
    started = time.monotonic()
    env = dict(os.environ)
    for index in range(args.count):
        mutated = bytearray(data)
        position = rng.randrange(len(mutated))
        bit = rng.randrange(8)
        mutated[position] ^= 1 << bit
        with open(target, "wb") as handle:
            handle.write(mutated)
        try:
            result = subprocess.run(
                [args.migrate, "restore", target],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
                env=env,
            )
        except subprocess.TimeoutExpired:
            timed_out += 1
            samples.append(f"#{index} pos={position} bit={bit} timeout")
            continue
        output = result.stdout.decode("utf-8", "replace")
        if result.returncode == 0:
            accepted += 1
            samples.append(f"#{index} pos={position} bit={bit} ACCEPTED")
        elif result.returncode < 0 or result.returncode >= 128:
            crashed += 1
            samples.append(
                f"#{index} pos={position} bit={bit} signal/rc={result.returncode} "
                f"{output.strip().splitlines()[-1] if output.strip() else ''}"
            )
        else:
            rejected += 1
    elapsed_ms = (time.monotonic() - started) * 1000.0
    print(
        f"RESULT count={args.count} rejected={rejected} accepted={accepted} "
        f"crashed={crashed} timeout={timed_out} elapsed_ms={elapsed_ms:.1f} "
        f"seed={args.seed}"
    )
    for sample in samples[:10]:
        print(f"SAMPLE {sample}")
    if args.keep:
        print(f"WORKDIR {workdir}")
    else:
        shutil.rmtree(workdir, ignore_errors=True)
    return 0 if accepted == 0 and crashed == 0 and timed_out == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    flip = sub.add_parser("bitflip")
    flip.add_argument("--input", required=True)
    flip.add_argument("--migrate", required=True)
    flip.add_argument("--count", type=int, default=1000)
    flip.add_argument("--seed", type=int, default=20261020)
    flip.add_argument("--timeout", type=float, default=10.0)
    flip.add_argument("--workdir", default="")
    flip.add_argument("--keep", action="store_true")
    flip.set_defaults(func=bitflip)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
