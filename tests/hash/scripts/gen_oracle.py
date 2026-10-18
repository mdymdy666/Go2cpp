#!/usr/bin/env python3
"""生成 `tests/hash/testdata/oracle.tsv`：用 Python 标准库给 C++ 实现当 oracle。

为什么要有这个脚本
------------------
`hash` 模块的五种摘要都要求「与权威实现逐位一致」。C++ 测试不能把期望值写死
几百条，也无法在 ctest 里依赖本机 Python。这里的做法与项目 `path` 模块一致：
**一次性预生成对拍数据**，ctest 只读 TSV，不依赖 Python。

数据生成器（C++ 侧 `tests/hash/hash_test_support.hpp` 必须逐字节一致）
--------------------------------------------------------------------
    state = seed
    state = (state * 6364136223846793005 + 1442695040888963407) mod 2^64
    byte  = state >> 56

这个 LCG（Knuth MMIX）只用 64 位整数运算，Python 与 C++ 都能精确复现，
避免「随机数据其实两边不一样」的假对拍。

oracle 来源
-----------
- CRC-32（IEEE）: `zlib.crc32`（zlib 的查表实现，与 Go `crc32.ChecksumIEEE` 同定义）
- Adler-32:      `zlib.adler32`
- MD5 / SHA-1 / SHA-256: `hashlib`
- CRC-32C / CRC-32K: 本脚本内的**逐位参考实现**（zlib 不提供这两个多项式）。
  逐位算法与 C++ 的查表/Slicing-by-8 是完全不同的实现路径，正好互相印证。

用法
----
    python3 tests/hash/scripts/gen_oracle.py            # 重写两个 oracle 文件
    python3 tests/hash/scripts/gen_oracle.py --check    # 打印标准检查值

默认生成：
    testdata/oracle.tsv         45 条边界/大文件用例（0 B ~ 10 MiB）
    testdata/oracle_random.tsv  10,000 条随机长度（0~1024 B）/随机内容用例
"""

from __future__ import annotations

import argparse
import hashlib
import random
import sys
import zlib
from pathlib import Path

MASK64 = (1 << 64) - 1
MULTIPLIER = 6364136223846793005
INCREMENT = 1442695040888963407

# 与 C++ 头文件中的常量一致（均为反射形式）。
IEEE = 0xEDB88320
CASTAGNOLI = 0x82F63B78
KOOPMAN = 0xEB31D82E


def generate(length: int, seed: int) -> bytes:
    """按文档中的 LCG 生成 `length` 字节。"""
    data = bytearray(length)
    state = seed & MASK64
    for index in range(length):
        state = (state * MULTIPLIER + INCREMENT) & MASK64
        data[index] = (state >> 56) & 0xFF
    return bytes(data)


def crc32_bitwise(data: bytes, poly: int) -> int:
    """逐位 CRC-32 参考实现（反射多项式，初值/终值取反）。"""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ poly
            else:
                crc >>= 1
    return crc ^ 0xFFFFFFFF


def case_lengths() -> list[int]:
    """覆盖空串、单字节、块边界（64/128）及其前后、1 MiB 与 10 MiB。"""
    return [
        0, 1, 2, 3, 7, 8, 15, 16, 31, 32,
        55, 56, 57, 63, 64, 65,
        119, 120, 127, 128, 129,
        191, 192, 255, 256, 257,
        511, 512, 513, 1000, 1023, 1024, 1025,
        4095, 4096, 4097,
        65535, 65536, 65537,
        100000, 262144,
        1048576, 1048577,
        4194304, 10485760,
    ]


def case_seed(index: int) -> int:
    """固定公式生成种子；同一 index 永远得到同一数据。"""
    return (0x9E3779B97F4A7C15 * (index + 1)) & MASK64


def format_case(length: int, seed: int) -> str:
    """计算一条 oracle 记录（与生成器、oracle 列顺序绑定）。"""
    data = generate(length, seed)
    crc_ieee = zlib.crc32(data) & 0xFFFFFFFF
    adler = zlib.adler32(data) & 0xFFFFFFFF
    crc_castagnoli = crc32_bitwise(data, CASTAGNOLI)
    crc_koopman = crc32_bitwise(data, KOOPMAN)
    md5 = hashlib.md5(data).hexdigest()
    sha1 = hashlib.sha1(data).hexdigest()
    sha256 = hashlib.sha256(data).hexdigest()
    return " ".join(
        [
            str(length),
            str(seed),
            f"{crc_ieee:08x}",
            f"{adler:08x}",
            f"{crc_castagnoli:08x}",
            f"{crc_koopman:08x}",
            md5,
            sha1,
            sha256,
        ]
    )


def build_rows() -> list[tuple[int, int, str]]:
    rows: list[tuple[int, int, str]] = []
    for index, length in enumerate(case_lengths()):
        seed = case_seed(index)
        rows.append((length, seed, format_case(length, seed)))
    return rows


def build_random_rows(count: int) -> list[tuple[int, int, str]]:
    """`count` 条随机长度/随机内容（同一 seed 下结果可复现）。"""
    rng = random.Random(20261018)
    rows: list[tuple[int, int, str]] = []
    for _ in range(count):
        length = rng.randrange(0, 1025)
        seed = rng.getrandbits(64)
        rows.append((length, seed, format_case(length, seed)))
    return rows


HEADER = """\
# go2cpp hash oracle v1
# 生成器: tests/hash/scripts/gen_oracle.py（Python zlib / hashlib + 逐位 CRC 参考实现）
# columns: length seed crc32_ieee adler32 crc32_castagnoli crc32_koopman md5 sha1 sha256
# 所有摘要/校验和均为十六进制；CRC/Adler 为 8 位十六进制数值
# 数据生成器: state = (state * 6364136223846793005 + 1442695040888963407) mod 2^64;
#             byte = state >> 56; state 初值 = seed
"""


def write_rows(path: Path, rows: list[tuple[int, int, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER)
        for _, _, line in rows:
            handle.write(line + "\n")
    total = sum(length for length, _, _ in rows)
    print(f"wrote {path} ({len(rows)} cases, {total} bytes of input)")


def write_oracle(path: Path) -> None:
    write_rows(path, build_rows())


def check_values() -> None:
    data = b"123456789"
    print("standard check values for b\"123456789\":")
    print(f"  crc32(IEEE)      = {zlib.crc32(data) & 0xFFFFFFFF:08x}")
    print(f"  crc32(Castagnoli)= {crc32_bitwise(data, CASTAGNOLI):08x}")
    print(f"  crc32(Koopman)   = {crc32_bitwise(data, KOOPMAN):08x}")
    print(f"  adler32          = {zlib.adler32(data) & 0xFFFFFFFF:08x}")
    print(f"  md5              = {hashlib.md5(data).hexdigest()}")
    print(f"  sha1             = {hashlib.sha1(data).hexdigest()}")
    print(f"  sha256           = {hashlib.sha256(data).hexdigest()}")
    empty = b""
    print("empty input:")
    print(f"  crc32            = {zlib.crc32(empty) & 0xFFFFFFFF:08x}")
    print(f"  adler32          = {zlib.adler32(empty) & 0xFFFFFFFF:08x}")
    print(f"  md5              = {hashlib.md5(empty).hexdigest()}")
    print(f"  sha1             = {hashlib.sha1(empty).hexdigest()}")
    print(f"  sha256           = {hashlib.sha256(empty).hexdigest()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="打印标准检查值后退出")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "testdata" / "oracle.tsv",
        help="边界/大文件用例输出路径（默认 tests/hash/testdata/oracle.tsv）",
    )
    parser.add_argument(
        "--random-count",
        type=int,
        default=10000,
        help="随机用例条数（默认 10000，长度 0~1024 字节）",
    )
    parser.add_argument(
        "--random-output",
        type=Path,
        default=Path(__file__).resolve().parent.parent
        / "testdata"
        / "oracle_random.tsv",
        help="随机用例输出路径（默认 tests/hash/testdata/oracle_random.tsv）",
    )
    args = parser.parse_args()
    if args.check:
        check_values()
        return 0
    write_oracle(args.output)
    write_rows(args.random_output, build_random_rows(args.random_count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
