/**
 * @file test_zlib.cpp
 * @brief `compress::Zlib`（RFC 1950）测试：头/尾格式、校验和检测、边界与分块 IO。
 *
 * @details 与 flate 测试的区别在于**格式本身**也要验证：
 *
 *   - 2 字节头：CM=8、CINFO≤7、`(CMF<<8|FLG) % 31 == 0`、FDICT=0；
 *   - 4 字节尾：adler32，**大端序**，用测试里独立实现的 `Adler32` 对拍；
 *   - 篡改尾部 → `ErrChecksum`；篡改头 → `ErrHeader`；
 *   - 构造 `FDICT=1` 的头 → `ErrDictionary`（本模块不支持预置字典）。
 *
 *   这些断言不依赖「自己压、自己解」——它们直接对字节布局做检查，再用
 *   独立实现的 adler32 验证尾部数值。
 */
#include "compress_test_support.hpp"

namespace {

using namespace go2cpp_compress_test;
namespace compress = go2cpp::compress;
using compress::Zlib;

std::uint32_t ReadBigEndian32(const std::string& data, std::size_t offset) {
  COMPRESS_REQUIRE(offset + 4 <= data.size());
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

void TestHeaderAndTrailer() {
  Announce("zlib: RFC 1950 header and adler32 trailer");
  const std::vector<std::pair<const char*, std::string>> cases = {
      {"text", MakeLogText(100U * 1024U, 101)},
      {"empty", std::string()},
      {"single", std::string(1, '\0')},
      {"zeros", MakeZeros(300U * 1024U)},
      {"random", MakeRandomBytes(100U * 1024U, 102)},
  };
  for (const auto& entry : cases) {
    ErrorPtr error;
    const std::string compressed =
        CompressBytes(Format::kZlib, entry.second, compress::BestCompression,
                      &error);
    COMPRESS_CHECK_NO_ERROR(error, entry.first);
    COMPRESS_REQUIRE(compressed.size() >= 6);
    const unsigned int cmf = static_cast<unsigned char>(compressed[0]);
    const unsigned int flg = static_cast<unsigned char>(compressed[1]);
    // CM = 8（deflate）；CINFO = 窗口大小指数，15 位窗口 → 7。
    COMPRESS_CHECK((cmf & 0x0F) == 8);
    COMPRESS_CHECK((cmf >> 4) <= 7);
    // FCHECK：整个 2 字节头必须是 31 的倍数。
    COMPRESS_CHECK(((cmf << 8) | flg) % 31 == 0);
    // FDICT 必须为 0（本模块不写字典）。
    COMPRESS_CHECK((flg & 0x20) == 0);
    // 尾部 adler32（大端）必须等于独立计算值。
    COMPRESS_CHECK(ReadBigEndian32(compressed, compressed.size() - 4) ==
                   Adler32(entry.second));
    ErrorPtr decompress_error;
    const std::string output =
        DecompressBytes(Format::kZlib, compressed, &decompress_error);
    COMPRESS_CHECK_NO_ERROR(decompress_error, entry.first);
    COMPRESS_CHECK(output == entry.second);
  }
}

void TestLevels() {
  Announce("zlib: level round-trips");
  const std::string input = MakeLogText(150U * 1024U, 111);
  std::size_t no_compression = 0;
  std::size_t best_speed = 0;
  std::size_t best_compression = 0;
  const int levels[] = {compress::NoCompression, compress::BestSpeed,
                        compress::DefaultCompression,
                        compress::BestCompression};
  std::size_t index = 0;
  for (const int level : levels) {
    ErrorPtr error;
    const std::string compressed =
        CompressBytes(Format::kZlib, input, level, &error);
    COMPRESS_CHECK_NO_ERROR(error, "zlib compress");
    ErrorPtr decompress_error;
    COMPRESS_CHECK(DecompressBytes(Format::kZlib, compressed, &decompress_error) ==
                   input);
    COMPRESS_CHECK_NO_ERROR(decompress_error, "zlib decompress");
    switch (index) {
      case 0:
        no_compression = compressed.size();
        break;
      case 1:
        best_speed = compressed.size();
        break;
      case 3:
        best_compression = compressed.size();
        break;
      default:
        break;
    }
    ++index;
  }
  COMPRESS_CHECK(no_compression >= input.size());
  COMPRESS_CHECK(best_compression < best_speed);
}

void TestChecksumTampering() {
  Announce("zlib: tampered adler32 is reported as ErrChecksum");
  const std::string input = MakeLogText(20U * 1024U, 121);
  ErrorPtr error;
  std::string compressed =
      CompressBytes(Format::kZlib, input, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for tamper");
  compressed[compressed.size() - 1] =
      static_cast<char>(static_cast<unsigned char>(compressed.back()) ^ 0xFFU);
  ErrorPtr tamper_error;
  DecompressBytes(Format::kZlib, compressed, &tamper_error);
  COMPRESS_CHECK_ERROR(tamper_error, compress::ErrChecksum(),
                       "zlib adler32 mismatch");

  // 篡改头部的 FLG：FCHECK 不再成立 → ErrHeader。
  std::string bad_header =
      CompressBytes(Format::kZlib, input, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for header tamper");
  bad_header[1] = static_cast<char>(static_cast<unsigned char>(bad_header[1]) ^ 0x01U);
  ErrorPtr header_error;
  DecompressBytes(Format::kZlib, bad_header, &header_error);
  COMPRESS_CHECK_ERROR(header_error, compress::ErrHeader(), "zlib bad header");

  // 篡改 CM（压缩方法）：0x78 → 0x79。
  std::string bad_cm =
      CompressBytes(Format::kZlib, input, compress::BestSpeed, &error);
  bad_cm[0] = static_cast<char>(0x79);
  ErrorPtr cm_error;
  DecompressBytes(Format::kZlib, bad_cm, &cm_error);
  COMPRESS_CHECK_ERROR(cm_error, compress::ErrHeader(), "zlib bad CM");
}

void TestDictionary() {
  Announce("zlib: FDICT streams are rejected with ErrDictionary");
  // CMF=0x78, FLG=0x20：FDICT=1 且 (0x7820 % 31 == 0)，随后 4 字节 DICTID。
  const std::string stream("\x78\x20\x00\x00\x00\x00", 6);
  ErrorPtr error;
  DecompressBytes(Format::kZlib, stream, &error);
  COMPRESS_CHECK_ERROR(error, compress::ErrDictionary(), "zlib FDICT");
}

void TestTruncated() {
  Announce("zlib: truncated stream and empty input");
  const std::string input = MakeLogText(16U * 1024U, 131);
  ErrorPtr error;
  std::string compressed =
      CompressBytes(Format::kZlib, input, compress::DefaultCompression, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for truncation");
  compressed.resize(compressed.size() / 2);
  ErrorPtr truncated_error;
  DecompressBytes(Format::kZlib, compressed, &truncated_error);
  COMPRESS_CHECK_ERROR(truncated_error, compress::ErrUnexpectedEof(),
                       "truncated zlib");

  ErrorPtr empty_error;
  DecompressBytes(Format::kZlib, std::string(), &empty_error);
  COMPRESS_CHECK_ERROR(empty_error, compress::ErrUnexpectedEof(),
                       "empty zlib input");
}

void TestChunkedIo() {
  Announce("zlib: chunked source/destination round-trip");
  const std::string input = MakeRandomText(40U * 1024U, 141);
  StringSink sink;
  {
    Zlib::Writer<StringSink> writer(sink, compress::BestCompression);
    COMPRESS_CHECK(WriteAll(writer, input));
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "zlib chunked close");
  }
  ErrorPtr error;
  const std::string output =
      DecompressBytes(Format::kZlib, sink.data(), &error, 5, 64);
  COMPRESS_CHECK_NO_ERROR(error, "zlib chunked decompress");
  COMPRESS_CHECK(output == input);
}

void TestInvalidLevelAndClosed() {
  Announce("zlib: invalid level / closed writer");
  StringSink sink;
  Zlib::Writer<StringSink> bad(sink, 42);
  COMPRESS_CHECK_ERROR(bad.Err(), compress::ErrInvalidLevel(), "zlib level");

  Zlib::Writer<StringSink> writer(sink, compress::BestSpeed);
  COMPRESS_CHECK(WriteAll(writer, "payload"));
  COMPRESS_CHECK_NO_ERROR(writer.Close(), "zlib close");
  COMPRESS_CHECK_ERROR(writer.Close(), compress::ErrClosed(), "zlib reclose");
  COMPRESS_CHECK_ERROR(writer.Flush(), compress::ErrClosed(), "zlib flush closed");
  COMPRESS_CHECK(writer.Write(ConstBytes("x")) == 0);
  COMPRESS_CHECK_ERROR(writer.Err(), compress::ErrClosed(), "zlib write closed");
}

void TestTrailingBytesIgnored() {
  Announce("zlib: trailing bytes after stream end are ignored");
  const std::string input = MakeLogText(8U * 1024U, 151);
  ErrorPtr error;
  const std::string compressed =
      CompressBytes(Format::kZlib, input, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for trailing bytes");
  ErrorPtr trailing_error;
  const std::string output =
      DecompressBytes(Format::kZlib, compressed + "trailing-garbage",
                      &trailing_error);
  COMPRESS_CHECK_NO_ERROR(trailing_error, "zlib trailing");
  COMPRESS_CHECK(output == input);
}

}  // namespace

/// zlib 测试入口。
void RunZlibTests() {
  Announce("=== zlib (RFC 1950) ===");
  TestHeaderAndTrailer();
  TestLevels();
  TestChecksumTampering();
  TestDictionary();
  TestTruncated();
  TestChunkedIo();
  TestInvalidLevelAndClosed();
  TestTrailingBytesIgnored();
}
