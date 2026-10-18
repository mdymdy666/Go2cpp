/**
 * @file test_interop.cpp
 * @brief 与系统实现的双向互操作验证（**证明格式正确的关键测试**）。
 *
 * @details 自己压、自己解只能证明「内部一致」，两边同时错（例如 gzip 尾部
 *   字节序写反）仍然会通过。本文件用**独立实现**做 oracle：
 *
 *   1. **系统 `gzip`（GNU gzip 1.12）**
 *      - 本模块压缩 → `gzip -t` 校验完整性 → `gzip -d` 解压 → 内容一致；
 *      - `gzip -c` 压缩 → 本模块解压 → 内容一致；
 *      - 多成员：本模块两个成员拼接 → `gzip -dc` 解出完整内容；
 *        `gzip -c A; gzip -c B; cat` 拼接 → 本模块解出完整内容。
 *   2. **Python 3 `zlib` / `gzip` 模块**（另一套完全独立的实现）
 *      - raw deflate、zlib 格式各做双向：本模块压 → Python 解；
 *        Python 压 → 本模块解。
 *      这两条是 gzip CLI 覆盖不到的：`gzip` 命令只会说 gzip 格式，
 *      而 raw deflate / zlib 格式的错误（windowBits 用错、adler32 字节序错）
 *      CLI 发现不了。
 *
 *   python3 或 gzip 不存在时**打印明确的 skip 原因**并不计失败（不能因为
 *   机器缺工具就假装验证过）。本机（Ubuntu 24.04）两者都可用，实际都会跑。
 */
#include "compress_test_support.hpp"

#include <cstdio>

namespace {

using namespace go2cpp_compress_test;

/// 本模块 gzip 输出交给系统 gzip：`-t` 校验 + `-d` 解压，内容必须一致。
bool SystemGzipAccepts(const TempDir& dir, const std::string& payload, int level,
                       const char* label) {
  ErrorPtr error;
  const std::string compressed =
      CompressBytes(Format::kGzip, payload, level, &error);
  if (error) {
    std::fprintf(stderr, "[compress] interop %s: compress failed: %s\n", label,
                 go2cpp::ErrorMessage(error).c_str());
    return false;
  }
  const std::string gz_path = dir.File("ours.gz");
  const std::string out_path = dir.File("ours.out");
  if (!WriteFile(gz_path, compressed)) {
    std::fprintf(stderr, "[compress] interop %s: cannot write %s\n", label,
                 gz_path.c_str());
    return false;
  }
  const int check = RunCommand("gzip -t '" + gz_path + "' 2>/dev/null");
  if (check != 0) {
    std::fprintf(stderr, "[compress] interop %s: gzip -t exited %d\n", label,
                 check);
    return false;
  }
  const int decompress =
      RunCommand("gzip -dc '" + gz_path + "' > '" + out_path + "' 2>/dev/null");
  if (decompress != 0) {
    std::fprintf(stderr, "[compress] interop %s: gzip -d exited %d\n", label,
                 decompress);
    return false;
  }
  bool read_ok = false;
  const std::string output = ReadFile(out_path, &read_ok);
  if (!read_ok || output != payload) {
    std::fprintf(stderr,
                 "[compress] interop %s: content mismatch (payload=%zu, "
                 "gzip-out=%zu)\n",
                 label, payload.size(), output.size());
    return false;
  }
  return true;
}

/// 系统 gzip 压缩的输出交给本模块解压，内容必须一致。
bool OurReaderAcceptsSystemGzip(const TempDir& dir, const std::string& payload,
                               const char* label) {
  const std::string in_path = dir.File("system.in");
  const std::string gz_path = dir.File("system.gz");
  if (!WriteFile(in_path, payload)) {
    return false;
  }
  const int compress =
      RunCommand("gzip -c '" + in_path + "' > '" + gz_path + "' 2>/dev/null");
  if (compress != 0) {
    std::fprintf(stderr, "[compress] interop %s: gzip -c exited %d\n", label,
                 compress);
    return false;
  }
  bool read_ok = false;
  const std::string compressed = ReadFile(gz_path, &read_ok);
  if (!read_ok) {
    return false;
  }
  ErrorPtr error;
  const std::string output =
      DecompressBytes(Format::kGzip, compressed, &error);
  if (error) {
    std::fprintf(stderr, "[compress] interop %s: our reader failed: %s\n",
                 label, go2cpp::ErrorMessage(error).c_str());
    return false;
  }
  if (output != payload) {
    std::fprintf(stderr, "[compress] interop %s: content mismatch\n", label);
    return false;
  }
  return true;
}

/// 本模块把两个成员拼成一个文件，系统 gzip 必须解出两段内容的拼接。
bool SystemGzipAcceptsMultistream(const TempDir& dir, const std::string& first,
                                 const std::string& second) {
  ErrorPtr error;
  const std::string first_member =
      CompressBytes(Format::kGzip, first, go2cpp::compress::BestSpeed, &error);
  const std::string second_member =
      CompressBytes(Format::kGzip, second, go2cpp::compress::BestCompression,
                    &error);
  if (error) {
    return false;
  }
  const std::string gz_path = dir.File("multi-ours.gz");
  const std::string out_path = dir.File("multi-ours.out");
  if (!WriteFile(gz_path, first_member + second_member)) {
    return false;
  }
  const int rc =
      RunCommand("gzip -dc '" + gz_path + "' > '" + out_path + "' 2>/dev/null");
  if (rc != 0) {
    std::fprintf(stderr, "[compress] interop multistream: gzip -d exited %d\n",
                 rc);
    return false;
  }
  bool read_ok = false;
  const std::string output = ReadFile(out_path, &read_ok);
  return read_ok && output == first + second;
}

/// 系统 gzip 生成的两个成员拼接后，本模块必须解出完整内容。
bool OurReaderAcceptsSystemMultistream(const TempDir& dir,
                                      const std::string& first,
                                      const std::string& second) {
  const std::string first_in = dir.File("member1.in");
  const std::string second_in = dir.File("member2.in");
  const std::string first_gz = dir.File("member1.gz");
  const std::string second_gz = dir.File("member2.gz");
  const std::string joined = dir.File("members.gz");
  if (!WriteFile(first_in, first) || !WriteFile(second_in, second)) {
    return false;
  }
  if (RunCommand("gzip -c '" + first_in + "' > '" + first_gz +
                 "' 2>/dev/null") != 0 ||
      RunCommand("gzip -c '" + second_in + "' > '" + second_gz +
                 "' 2>/dev/null") != 0) {
    return false;
  }
  if (RunCommand("cat '" + first_gz + "' '" + second_gz + "' > '" + joined +
                 "'") != 0) {
    return false;
  }
  bool read_ok = false;
  const std::string compressed = ReadFile(joined, &read_ok);
  if (!read_ok) {
    return false;
  }
  ErrorPtr error;
  const std::string output =
      DecompressBytes(Format::kGzip, compressed, &error);
  if (error) {
    std::fprintf(stderr,
                 "[compress] interop multistream: our reader failed: %s\n",
                 go2cpp::ErrorMessage(error).c_str());
    return false;
  }
  return output == first + second;
}

/// 写入 Python oracle 脚本；返回脚本路径（失败返回空）。
std::string WritePythonOracle(const TempDir& dir) {
  const std::string path = dir.File("oracle.py");
  const std::string script =
      "import sys, zlib\n"
      "\n"
      "def main():\n"
      "    mode, src, dst = sys.argv[1], sys.argv[2], sys.argv[3]\n"
      "    with open(src, 'rb') as handle:\n"
      "        data = handle.read()\n"
      "    if mode == 'd-raw':\n"
      "        out = zlib.decompress(data, -15)\n"
      "    elif mode == 'd-zlib':\n"
      "        out = zlib.decompress(data)\n"
      "    elif mode == 'c-raw':\n"
      "        engine = zlib.compressobj(9, zlib.DEFLATED, -15)\n"
      "        out = engine.compress(data) + engine.flush()\n"
      "    elif mode == 'c-zlib':\n"
      "        out = zlib.compress(data, 9)\n"
      "    else:\n"
      "        sys.stderr.write('unknown mode: %s\\n' % mode)\n"
      "        return 2\n"
      "    with open(dst, 'wb') as handle:\n"
      "        handle.write(out)\n"
      "    return 0\n"
      "\n"
      "sys.exit(main())\n";
  if (!WriteFile(path, script)) {
    return {};
  }
  return path;
}

/// 用 Python oracle 检查 raw deflate / zlib 的双向互操作。
bool PythonRoundTrip(const TempDir& dir, const std::string& oracle,
                     const std::string& payload, Format format,
                     const char* label) {
  const std::string suffix = format == Format::kFlate ? "raw" : "zlib";
  const std::string in_path = dir.File("py.in");
  const std::string compressed_path = dir.File("py.ours.bin");
  const std::string decoded_path = dir.File("py.decoded");
  const std::string python_compressed_path = dir.File("py.python.bin");
  if (!WriteFile(in_path, payload)) {
    return false;
  }
  ErrorPtr error;
  const std::string compressed =
      CompressBytes(format, payload, go2cpp::compress::BestCompression, &error);
  if (error) {
    std::fprintf(stderr, "[compress] interop %s: compress failed\n", label);
    return false;
  }
  if (!WriteFile(compressed_path, compressed)) {
    return false;
  }
  // 正向：本模块压 → Python 解（mode = d-raw / d-zlib）。
  if (RunCommand("python3 '" + oracle + "' d-" + suffix + " '" +
                 compressed_path + "' '" + decoded_path +
                 "' 2>/dev/null") != 0) {
    std::fprintf(stderr,
                 "[compress] interop %s: python rejected our stream\n", label);
    return false;
  }
  bool read_ok = false;
  const std::string decoded = ReadFile(decoded_path, &read_ok);
  if (!read_ok || decoded != payload) {
    std::fprintf(stderr,
                 "[compress] interop %s: python output mismatch (ours=%zu, "
                 "python=%zu)\n",
                 label, payload.size(), decoded.size());
    return false;
  }
  // 反向：Python 压 → 本模块解（mode = c-raw / c-zlib）。
  if (RunCommand("python3 '" + oracle + "' c-" + suffix + " '" + in_path +
                 "' '" + python_compressed_path + "' 2>/dev/null") != 0) {
    return false;
  }
  const std::string python_compressed =
      ReadFile(python_compressed_path, &read_ok);
  if (!read_ok) {
    return false;
  }
  ErrorPtr decompress_error;
  const std::string ours =
      DecompressBytes(format, python_compressed, &decompress_error);
  if (decompress_error) {
    std::fprintf(stderr,
                 "[compress] interop %s: our reader rejects python stream: %s\n",
                 label, go2cpp::ErrorMessage(decompress_error).c_str());
    return false;
  }
  if (ours != payload) {
    std::fprintf(stderr, "[compress] interop %s: content mismatch\n", label);
    return false;
  }
  return true;
}

}  // namespace

/// 互操作测试入口。
void RunInteropTests() {
  Announce("=== interop with GNU gzip and Python zlib ===");
  TempDir dir;
  COMPRESS_REQUIRE(dir.ok());
  const std::string text = MakeLogText(256U * 1024U, 401);
  const std::string random = MakeRandomBytes(256U * 1024U, 402);
  const std::string zeros = MakeZeros(1024U * 1024U);
  const std::string empty;
  const std::string one_byte(1, 'q');

  const bool has_gzip = CommandExists("gzip");
  if (has_gzip) {
    // 双向：本模块 → gzip -d；gzip -c → 本模块。
    const struct {
      const char* label;
      const std::string* data;
      int level;
    } cases[] = {
        {"text-level9", &text, go2cpp::compress::BestCompression},
        {"text-level1", &text, go2cpp::compress::BestSpeed},
        {"text-level0", &text, go2cpp::compress::NoCompression},
        {"random-level9", &random, go2cpp::compress::BestCompression},
        {"zeros-level9", &zeros, go2cpp::compress::BestCompression},
        {"empty-level9", &empty, go2cpp::compress::BestCompression},
        {"one-byte-default", &one_byte,
         go2cpp::compress::DefaultCompression},
    };
    for (const auto& entry : cases) {
      char label[128];
      std::snprintf(label, sizeof(label), "%s->gzip", entry.label);
      COMPRESS_CHECK(SystemGzipAccepts(dir, *entry.data, entry.level, label));
      std::snprintf(label, sizeof(label), "gzip->%s", entry.label);
      COMPRESS_CHECK(OurReaderAcceptsSystemGzip(dir, *entry.data, label));
    }

    // 多成员双向。
    const std::string first = MakeLogText(64U * 1024U, 403);
    const std::string second = MakeRandomText(64U * 1024U, 404);
    COMPRESS_CHECK(SystemGzipAcceptsMultistream(dir, first, second));
    COMPRESS_CHECK(OurReaderAcceptsSystemMultistream(dir, first, second));
    std::printf(
        "[compress] interop: GNU gzip 7 datasets x 2 directions + "
        "multistream x 2 directions all passed\n");
  } else {
    std::printf(
        "[compress] interop: SKIP GNU gzip checks (gzip not found in PATH)\n");
  }

  const bool has_python = CommandExists("python3");
  if (has_python) {
    const std::string oracle = WritePythonOracle(dir);
    COMPRESS_REQUIRE(!oracle.empty());
    for (const Format format :
         {Format::kFlate, Format::kZlib}) {
      char label[128];
      std::snprintf(label, sizeof(label), "python-%s-text",
                    FormatName(format));
      COMPRESS_CHECK(PythonRoundTrip(dir, oracle, text, format, label));
      std::snprintf(label, sizeof(label), "python-%s-random",
                    FormatName(format));
      COMPRESS_CHECK(PythonRoundTrip(dir, oracle, random, format, label));
      std::snprintf(label, sizeof(label), "python-%s-empty",
                    FormatName(format));
      COMPRESS_CHECK(PythonRoundTrip(dir, oracle, empty, format, label));
    }
    std::printf(
        "[compress] interop: Python zlib oracle (raw deflate + zlib) "
        "3 payloads x 2 directions all passed\n");
  } else {
    std::printf(
        "[compress] interop: SKIP Python zlib oracle (python3 not found)\n");
  }
}
