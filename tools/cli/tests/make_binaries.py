#!/usr/bin/env python3
"""生成测试用的二进制记录/快照文件（按 tools/replay 与 tools/migration 的格式头）。"""

import struct
import sys
from pathlib import Path


def make_replay(path: str) -> None:
    # FileHeader: magic[8] + version u16 + header_size u16 + flags u32 +
    #             created_unix_ns u64 + producer_id u64 = 32 bytes
    header = b"G2CRPLY\0" + struct.pack("<HHIQQ", 1, 32, 1, 0, 7)
    assert len(header) == 32, len(header)
    Path(path).write_bytes(header + b"\x00" * 32)


def make_migration_v2(path: str) -> None:
    # v2 Header 固定 192 字节；CLI 仍兼容读（tools/migration/include/migration/format.hpp）
    header = bytearray(192)
    header[0:8] = b"G2CMIG02"
    struct.pack_into("<I", header, 8, 2)      # version
    struct.pack_into("<I", header, 12, 192)   # header_size
    struct.pack_into("<I", header, 20, 2)     # section_count
    struct.pack_into("<Q", header, 44, 42)    # fiber_id
    struct.pack_into("<Q", header, 60, 65536)  # stack_size
    Path(path).write_bytes(bytes(header))


def make_migration_v3(path: str) -> None:
    # v3 Header 固定 224 字节，version=3（当前写版本）。
    header = bytearray(224)
    header[0:8] = b"G2CMIG03"
    struct.pack_into("<I", header, 8, 3)      # version
    struct.pack_into("<I", header, 12, 224)   # header_size
    struct.pack_into("<I", header, 20, 2)     # section_count
    struct.pack_into("<Q", header, 44, 42)    # fiber_id
    struct.pack_into("<Q", header, 60, 65536)  # stack_size
    Path(path).write_bytes(bytes(header))


def make_migration_unsupported(path: str) -> None:
    # 合法 v3 魔数但版本号不受支持：CLI 必须明确拒绝，而不是交给后端。
    header = bytearray(224)
    header[0:8] = b"G2CMIG03"
    struct.pack_into("<I", header, 8, 9)      # version=9
    struct.pack_into("<I", header, 12, 224)
    Path(path).write_bytes(bytes(header))


def make_migration_v1(path: str) -> None:
    # 旧版 v1 魔数；CLI 必须明确拒绝（tools/migration v2 已不读 v1）。
    header = bytearray(128)
    header[0:8] = b"G2CMIG01"
    struct.pack_into("<I", header, 8, 1)
    struct.pack_into("<I", header, 12, 128)
    Path(path).write_bytes(bytes(header))


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: make_binaries.py <work-dir>", file=sys.stderr)
        return 2
    work = sys.argv[1]
    make_replay(f"{work}/good.g2cr")
    make_migration_v2(f"{work}/good.mig")
    make_migration_v3(f"{work}/good-v3.mig")
    make_migration_unsupported(f"{work}/unsupported.mig")
    make_migration_v1(f"{work}/legacy_v1.mig")
    return 0


if __name__ == "__main__":
    sys.exit(main())
