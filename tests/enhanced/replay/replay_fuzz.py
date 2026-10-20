#!/usr/bin/env python3
"""G2CRPLY 事件流随机交错 fuzz（结构合法：重排整条记录并重算 CRC）。

每轮随机选择：
  - shuffle：完全打乱非 kEnd 事件顺序；
  - swap：随机交换 1..8 对事件；
然后重算整文件 CRC 与 kEnd 记录 CRC，使文件仍能通过格式校验。
对 `replay play` 观察：ok / inconsistent(stuck) / format-error / 崩溃 / 超时。

判定：崩溃或超时 = 缺陷；ok/stuck/format 都是可接受的「有界响应」。
"""

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile
import time
import zlib

MAGIC = b"G2CRPLY\x00"
RECORD_HEADER = 40
KEND = 99


def parse(data: bytes):
    if len(data) < 32 or data[:8] != MAGIC:
        raise ValueError("bad magic/header")
    header_size = struct.unpack_from("<H", data, 10)[0]
    if header_size < 32 or header_size > len(data):
        raise ValueError("bad header_size")
    header = data[:header_size]
    records = []
    end_record = None
    offset = header_size
    while offset + RECORD_HEADER <= len(data):
        size = struct.unpack_from("<I", data, offset)[0]
        kind = struct.unpack_from("<H", data, offset + 4)[0]
        if size < RECORD_HEADER or offset + size > len(data):
            raise ValueError(f"bad record at {offset}")
        record = data[offset:offset + size]
        if kind == KEND:
            end_record = record
            offset += size
            break
        records.append(record)
        offset += size
    if end_record is None:
        raise ValueError("no kEnd")
    if offset != len(data):
        raise ValueError("trailing bytes after kEnd")
    return header, records, end_record


def rebuild(header: bytes, records, end_record: bytes, rng: random.Random,
            mode: str) -> bytes:
    order = list(records)
    actual = mode
    if mode == "mixed":
        actual = "shuffle" if rng.random() < 0.5 else "swap"
    if actual == "shuffle":
        rng.shuffle(order)
    else:
        pairs = rng.randint(1, 8)
        for _ in range(pairs):
            left = rng.randrange(len(order))
            right = rng.randrange(len(order))
            order[left], order[right] = order[right], order[left]
    body = b"".join(order)
    end = bytearray(end_record)
    # kEnd payload: records u64, dropped u64, file_crc u32, reserved u32
    struct.pack_into("<I", end, RECORD_HEADER + 16, zlib.crc32(body) & 0xFFFFFFFF)
    crc = zlib.crc32(bytes(end[:RECORD_HEADER - 4]) + bytes(end[RECORD_HEADER:]))
    struct.pack_into("<I", end, RECORD_HEADER - 4, crc & 0xFFFFFFFF)
    return header + body + bytes(end)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--replay", required=True)
    parser.add_argument("--count", type=int, default=500)
    parser.add_argument("--seed", type=int, default=20261020)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--mode", choices=["shuffle", "swap", "mixed"],
                        default="mixed")
    parser.add_argument("--workdir", default="")
    args = parser.parse_args()

    data = open(args.input, "rb").read()
    try:
        header, records, end_record = parse(data)
    except ValueError as error:
        print(f"RESULT error={error}")
        return 2
    rng = random.Random(args.seed)
    workdir = args.workdir or tempfile.mkdtemp(prefix="replay-fuzz-")
    os.makedirs(workdir, exist_ok=True)
    target = os.path.join(workdir, "interleaved.g2cr")
    counts = {"ok": 0, "stuck": 0, "format": 0, "other": 0,
              "crash": 0, "timeout": 0}
    samples = []
    started = time.monotonic()
    for index in range(args.count):
        mutated = rebuild(header, records, end_record, rng, args.mode)
        with open(target, "wb") as handle:
            handle.write(mutated)
        try:
            result = subprocess.run(
                [args.replay, "play", target],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=args.timeout,
            )
        except subprocess.TimeoutExpired:
            counts["timeout"] += 1
            samples.append(f"#{index} timeout")
            continue
        code = result.returncode
        if code == 0:
            counts["ok"] += 1
        elif code == 1:
            counts["stuck"] += 1
        elif code == 2:
            counts["format"] += 1
        elif code < 0 or code >= 128:
            counts["crash"] += 1
            samples.append(f"#{index} rc={code}")
        else:
            counts["other"] += 1
            samples.append(f"#{index} rc={code} {result.stderr[-120:]!r}")
    elapsed_ms = (time.monotonic() - started) * 1000.0
    print(
        f"RESULT count={args.count} ok={counts['ok']} stuck={counts['stuck']} "
        f"format={counts['format']} other={counts['other']} "
        f"crash={counts['crash']} timeout={counts['timeout']} "
        f"elapsed_ms={elapsed_ms:.1f} seed={args.seed} mode={args.mode}"
    )
    for sample in samples[:10]:
        print(f"SAMPLE {sample}")
    return 0 if counts["crash"] == 0 and counts["timeout"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
