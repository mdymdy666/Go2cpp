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

}  // namespace

/// 随机测试入口。
void RunRandomTests() {
  Announce("=== randomized tests ===");
  TestRandomizedRoundTrips();
  TestExplicitBoundaries();
  TestRandomFlushPatterns();
}
