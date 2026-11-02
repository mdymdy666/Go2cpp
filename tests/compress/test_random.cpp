/**
 * @file test_random.cpp
 * @brief 随机测试：随机长度 / 随机内容 / 随机分块 / 随机格式与级别。
 *
 * @details 目标不是「跑通一次」，而是用固定种子覆盖组合空间：
 *
 *   - 长度：0、1~100、1~64 KiB、1~512 KiB、1~2 MiB 五档，按权重抽取；
 *   - 内容：日志文本（低熵）、随机字节（高熵）、全零、可打印随机文本；
 *   - 压缩参数：3 种格式 × 4 个级别随机组合；
 *   - IO 形态：源单次给 0/1~8 KiB 字节、目标缓冲 1~8 KiB（覆盖部分读写）。
 *
 *   种子固定（`20261018`），失败可复现；每轮打印失败上下文（轮次、格式、
 *   级别、长度、分块大小）。另有**显式边界**（空、1 字节、1 MiB 全零、
 *   1 MiB / 2 MiB 随机）对三种格式各跑一遍，避免随机抽样恰好漏掉极端值。
 */
#include "compress_test_support.hpp"
#include "go2cpp/hash/crc32.hpp"

#include <cstdio>

namespace {

using namespace go2cpp_compress_test;
namespace compress = go2cpp::compress;

/// 往返一遍并断言逐字节一致；失败时打印完整上下文。
void CheckRoundTrip(Format format, const std::string& input, int level,
                    std::size_t source_chunk, std::size_t dest_chunk,
                    const char* label) {
  ErrorPtr error;
  const std::string compressed = CompressBytes(format, input, level, &error);
  if (error) {
    std::fprintf(stderr, "[compress] %s: compress failed: %s\n", label,
                 go2cpp::ErrorMessage(error).c_str());
    COMPRESS_CHECK(false);
    return;
  }
  ErrorPtr decompress_error;
  const std::string output = DecompressBytes(format, compressed, &decompress_error,
                                             source_chunk, dest_chunk);
  if (decompress_error) {
    std::fprintf(stderr, "[compress] %s: decompress failed: %s\n", label,
                 go2cpp::ErrorMessage(decompress_error).c_str());
    COMPRESS_CHECK(false);
    return;
  }
  if (output != input) {
    std::fprintf(stderr,
                 "[compress] %s: mismatch (input=%zu, output=%zu, "
                 "compressed=%zu)\n",
                 label, input.size(), output.size(), compressed.size());
  }
  COMPRESS_CHECK(output == input);
}

std::string MakeContent(std::mt19937_64& rng, std::size_t length) {
  switch (rng() % 4) {
    case 0:
      return MakeLogText(length, rng());
    case 1:
      return MakeRandomBytes(length, rng());
    case 2:
      return MakeZeros(length);
    default:
      return MakeRandomText(length, rng());
  }
}

void TestRandomizedRoundTrips() {
  Announce("random: 90 randomized round-trips (fixed seed 20261018)");
  std::mt19937_64 rng(20261018);
  const Format formats[] = {Format::kFlate, Format::kZlib, Format::kGzip};
  const int levels[] = {compress::NoCompression, compress::BestSpeed,
                        compress::DefaultCompression,
                        compress::BestCompression};
  for (int iteration = 0; iteration < 90; ++iteration) {
    const Format format = formats[rng() % 3];
    const int level = levels[rng() % 4];
    const std::uint64_t roll = rng() % 100;
    std::size_t length = 0;
    if (roll < 10) {
      length = 0;
    } else if (roll < 20) {
      length = 1 + rng() % 100;
    } else if (roll < 60) {
      length = 1 + rng() % (64U * 1024U);
    } else if (roll < 90) {
      length = 1 + rng() % (512U * 1024U);
    } else {
      length = 1 + rng() % (2U * 1024U * 1024U);
    }
    const std::string input = MakeContent(rng, length);
    const std::size_t source_chunk =
        (rng() % 3 == 0) ? 0 : (1 + rng() % 8192);
    const std::size_t dest_chunk = 1 + rng() % 8192;
    char label[192];
    std::snprintf(label, sizeof(label),
                  "random #%d format=%s level=%d len=%zu src_chunk=%zu "
                  "dst_chunk=%zu",
                  iteration, FormatName(format), level, length, source_chunk,
                  dest_chunk);
    CheckRoundTrip(format, input, level, source_chunk, dest_chunk, label);
  }
}

void TestExplicitBoundaries() {
  Announce("random: explicit boundary sizes across all formats");
  const struct {
    const char* name;
    std::string data;
  } cases[] = {
      {"empty", std::string()},
      {"one-byte", std::string(1, 'x')},
      {"one-kib", MakeLogText(1024, 301)},
      {"zeros-1m", MakeZeros(1024U * 1024U)},
      {"random-1m", MakeRandomBytes(1024U * 1024U, 302)},
      {"random-2m", MakeRandomBytes(2U * 1024U * 1024U, 303)},
  };
  const Format formats[] = {Format::kFlate, Format::kZlib, Format::kGzip};
  for (const Format format : formats) {
    for (const auto& entry : cases) {
      char label[128];
      std::snprintf(label, sizeof(label), "boundary %s format=%s", entry.name,
                    FormatName(format));
      CheckRoundTrip(format, entry.data, compress::BestCompression, 0, 4096,
                     label);
    }
  }
}

void TestRandomFlushPatterns() {
  Announce("random: interleaved Write/Flush patterns");
  std::mt19937_64 rng(424242);
  const Format formats[] = {Format::kFlate, Format::kZlib, Format::kGzip};
  const int levels[] = {compress::BestSpeed, compress::DefaultCompression,
                        compress::BestCompression};
  for (int iteration = 0; iteration < 12; ++iteration) {
    const Format format = formats[iteration % 3];
    const int level = levels[iteration % 3];
    const std::string input = MakeContent(rng, 200U * 1024U);
    StringSink sink;
    bool ok = true;
    auto drive = [&](auto& writer) {
      std::size_t offset = 0;
      while (offset < input.size() && ok) {
        const std::size_t chunk =
            1 + rng() % (16U * 1024U);  // 随机写块
        const std::size_t take =
            std::min<std::size_t>(chunk, input.size() - offset);
        const std::size_t accepted = writer.Write(ByteSpan(
            reinterpret_cast<std::uint8_t*>(
                const_cast<char*>(input.data() + offset)),
            take));
        if (accepted != take || writer.Err()) {
          ok = false;
          break;
        }
        offset += accepted;
        if (rng() % 3 == 0) {
          if (writer.Flush()) {
            ok = false;
            break;
          }
        }
      }
      if (ok && writer.Close()) {
        ok = false;
      }
    };
    switch (format) {
      case Format::kFlate: {
        compress::Flate::Writer<StringSink> writer(sink, level);
        drive(writer);
        break;
      }
      case Format::kZlib: {
        compress::Zlib::Writer<StringSink> writer(sink, level);
        drive(writer);
        break;
      }
      case Format::kGzip: {
        compress::Gzip::Writer<StringSink> writer(sink, level);
        drive(writer);
        break;
      }
    }
    COMPRESS_CHECK(ok);
    ErrorPtr error;
    const std::string output =
        DecompressBytes(format, sink.data(), &error, 0, 4096);
    COMPRESS_CHECK_NO_ERROR(error, "random flush roundtrip");
    COMPRESS_CHECK(output == input);
  }
}

/**
 * @brief gzip 解压侧 CRC-32 与独立逐位参考实现对拍。
 *
 * @details 解压器的 CRC 现在批量走 `hash::Crc32`（x86-64 为 PCLMULQDQ 折叠），
 *   性能提升的前提是「折叠路径与逐位定义逐字节一致」。本测试用测试支持里
 *   **独立实现的逐位 CRC**（不调用 hash 模块，也不调用 zlib）做 oracle：
 *
 *   - 1e6 随机数据的整体 CRC；
 *   - 长度 0..300 + 1023/1024/4095/4096/65535/65536/999999/1000000：
 *     覆盖折叠门槛 64B 两侧、16 字节整块尾部、Byte 收尾与 1 MiB 大块；
 *   - 随机分段 update：把 1e6 数据切成 1..4096 字节的随机段，累积结果必须
 *     等于整段 CRC（gzip 解压正是按 32 KiB~1 MiB 分段提交的）。
 */
void TestCrc32AgainstReference() {
  Announce("random: crc32 vs independent bitwise reference (1e6 random + length sweep)");
  const std::string pool = MakeRandomBytes(1000000, 505);
  const auto* const bytes = reinterpret_cast<const std::uint8_t*>(pool.data());
  std::vector<std::size_t> lengths;
  lengths.reserve(320);
  for (std::size_t n = 0; n <= 300; ++n) {
    lengths.push_back(n);
  }
  for (const std::size_t n : {1023U, 1024U, 4095U, 4096U, 65535U, 65536U,
                              999999U, 1000000U}) {
    lengths.push_back(n);
  }
  for (const std::size_t n : lengths) {
    const std::uint32_t want = Crc32(std::string_view(pool.data(), n));
    const std::uint32_t got = go2cpp::hash::Crc32::ChecksumIEEE(
        go2cpp::hash::AsBytes(bytes, n));
    if (want != got) {
      std::fprintf(stderr,
                   "[compress] crc32 length %zu mismatch: %08x vs %08x\n", n,
                   want, got);
    }
    COMPRESS_CHECK(want == got);
  }

  const std::uint32_t whole =
      Crc32(std::string_view(pool.data(), pool.size()));
  std::mt19937_64 rng(506);
  std::uint32_t streamed = 0;
  std::size_t offset = 0;
  while (offset < pool.size()) {
    const std::size_t take = 1 + static_cast<std::size_t>(rng() % 4096);
    const std::size_t n = std::min(take, pool.size() - offset);
    streamed = go2cpp::hash::Crc32::Update(
        streamed, go2cpp::hash::IEEETable,
        go2cpp::hash::AsBytes(bytes + offset, n));
    offset += n;
  }
  COMPRESS_CHECK(streamed == whole);
  std::printf(
      "[compress] random: crc32 1e6 random + %zu lengths + segmented update ok\n",
      lengths.size());
}

}  // namespace

/// 随机测试入口。
void RunRandomTests() {
  Announce("=== randomized tests ===");
  TestRandomizedRoundTrips();
  TestExplicitBoundaries();
  TestRandomFlushPatterns();
  TestCrc32AgainstReference();
}
