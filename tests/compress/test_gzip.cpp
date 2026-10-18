/**
 * @file test_gzip.cpp
 * @brief `compress::Gzip`（RFC 1952）测试：头/尾格式、多成员、校验和与错误语义。
 *
 * @details 重点在 gzip 独有的两件事：
 *
 *   1. **10 字节头 + 8 字节尾的字节布局**：magic `1f 8b`、CM=8、FLG=0、
 *      MTIME=0；尾部 crc32 与 ISIZE 均为**小端序**，用测试里独立实现的
 *      `Crc32` 对拍（不调用 zlib）。
 *   2. **多成员（concatenated gzip）**：`Multistream(true)` 默认开启。
 *      `A.gz + B.gz + empty.gz` 必须解出 `A + B`；`Multistream(false)` 只解
 *      第一个成员；成员之间夹杂非 gzip 数据 → `ErrHeader`。
 */
#include "compress_test_support.hpp"

namespace {

using namespace go2cpp_compress_test;
namespace compress = go2cpp::compress;
using compress::Gzip;

std::uint32_t ReadLittleEndian32(const std::string& data, std::size_t offset) {
  COMPRESS_REQUIRE(offset + 4 <= data.size());
  return static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3])) << 24);
}

void TestHeaderAndTrailer() {
  Announce("gzip: RFC 1952 header and crc32/ISIZE trailer");
  const std::vector<std::pair<const char*, std::string>> cases = {
      {"text", MakeLogText(100U * 1024U, 201)},
      {"empty", std::string()},
      {"single", std::string(1, 'Z')},
      {"zeros", MakeZeros(256U * 1024U)},
      {"random", MakeRandomBytes(100U * 1024U, 202)},
  };
  for (const auto& entry : cases) {
    ErrorPtr error;
    const std::string compressed =
        CompressBytes(Format::kGzip, entry.second, compress::BestCompression,
                      &error);
    COMPRESS_CHECK_NO_ERROR(error, entry.first);
    COMPRESS_REQUIRE(compressed.size() >= 18);
    COMPRESS_CHECK(static_cast<unsigned char>(compressed[0]) == 0x1F);
    COMPRESS_CHECK(static_cast<unsigned char>(compressed[1]) == 0x8B);
    COMPRESS_CHECK(static_cast<unsigned char>(compressed[2]) == 8);  // CM=deflate
    COMPRESS_CHECK(static_cast<unsigned char>(compressed[3]) == 0);  // FLG=0
    // MTIME=0（本模块不写时间戳，输出可复现）。
    for (std::size_t index = 4; index < 8; ++index) {
      COMPRESS_CHECK(compressed[index] == 0);
    }
    const std::size_t trailer = compressed.size() - 8;
    COMPRESS_CHECK(ReadLittleEndian32(compressed, trailer) == Crc32(entry.second));
    COMPRESS_CHECK(ReadLittleEndian32(compressed, trailer + 4) ==
                   static_cast<std::uint32_t>(entry.second.size() & 0xFFFFFFFFU));
    ErrorPtr decompress_error;
    COMPRESS_CHECK(DecompressBytes(Format::kGzip, compressed, &decompress_error) ==
                   entry.second);
    COMPRESS_CHECK_NO_ERROR(decompress_error, entry.first);
  }
}

void TestLevels() {
  Announce("gzip: level round-trips");
  const std::string input = MakeLogText(150U * 1024U, 211);
  std::size_t best_speed = 0;
  std::size_t best_compression = 0;
  std::size_t no_compression = 0;
  const int levels[] = {compress::NoCompression, compress::BestSpeed,
                        compress::DefaultCompression,
                        compress::BestCompression};
  std::size_t index = 0;
  for (const int level : levels) {
    ErrorPtr error;
    const std::string compressed =
        CompressBytes(Format::kGzip, input, level, &error);
    COMPRESS_CHECK_NO_ERROR(error, "gzip compress");
    ErrorPtr decompress_error;
    COMPRESS_CHECK(DecompressBytes(Format::kGzip, compressed, &decompress_error) ==
                   input);
    COMPRESS_CHECK_NO_ERROR(decompress_error, "gzip decompress");
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
  Announce("gzip: tampered crc32 / ISIZE / magic");
  const std::string input = MakeLogText(20U * 1024U, 221);
  ErrorPtr error;
  const std::string baseline =
      CompressBytes(Format::kGzip, input, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for tamper");

  std::string bad_crc = baseline;
  bad_crc[bad_crc.size() - 8] =
      static_cast<char>(static_cast<unsigned char>(bad_crc[bad_crc.size() - 8]) ^ 0xFFU);
  ErrorPtr crc_error;
  DecompressBytes(Format::kGzip, bad_crc, &crc_error);
  COMPRESS_CHECK_ERROR(crc_error, compress::ErrChecksum(), "gzip crc32");

  std::string bad_size = baseline;
  bad_size[bad_size.size() - 4] =
      static_cast<char>(static_cast<unsigned char>(bad_size[bad_size.size() - 4]) ^ 0x01U);
  ErrorPtr size_error;
  DecompressBytes(Format::kGzip, bad_size, &size_error);
  COMPRESS_CHECK_ERROR(size_error, compress::ErrChecksum(), "gzip ISIZE");

  std::string bad_magic = baseline;
  bad_magic[0] = static_cast<char>(0x1E);
  ErrorPtr magic_error;
  DecompressBytes(Format::kGzip, bad_magic, &magic_error);
  COMPRESS_CHECK_ERROR(magic_error, compress::ErrHeader(), "gzip magic");

  std::string bad_method = baseline;
  bad_method[2] = static_cast<char>(7);  // CM=7 不是 deflate
  ErrorPtr method_error;
  DecompressBytes(Format::kGzip, bad_method, &method_error);
  COMPRESS_CHECK_ERROR(method_error, compress::ErrHeader(), "gzip CM");

  // 篡改 deflate 载荷中段：gzip 的 crc32 必须能发现（raw deflate 发现不了）。
  std::string bad_payload = baseline;
  bad_payload[bad_payload.size() / 2] =
      static_cast<char>(static_cast<unsigned char>(bad_payload[bad_payload.size() / 2]) ^ 0xFFU);
  ErrorPtr payload_error;
  DecompressBytes(Format::kGzip, bad_payload, &payload_error);
  COMPRESS_CHECK(payload_error != nullptr);
  COMPRESS_CHECK(go2cpp::Is(payload_error, compress::ErrChecksum()) ||
                 go2cpp::Is(payload_error, compress::ErrCorrupt()));
}

void TestTruncated() {
  Announce("gzip: truncated stream and empty input");
  const std::string input = MakeLogText(16U * 1024U, 231);
  ErrorPtr error;
  std::string compressed =
      CompressBytes(Format::kGzip, input, compress::DefaultCompression, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for truncation");
  compressed.resize(compressed.size() / 2);
  ErrorPtr truncated_error;
  DecompressBytes(Format::kGzip, compressed, &truncated_error);
  COMPRESS_CHECK_ERROR(truncated_error, compress::ErrUnexpectedEof(),
                       "truncated gzip");

  ErrorPtr empty_error;
  DecompressBytes(Format::kGzip, std::string(), &empty_error);
  COMPRESS_CHECK_ERROR(empty_error, compress::ErrUnexpectedEof(),
                       "empty gzip input");

  // 只缺尾部 8 字节（校验和）：解压能产出全部数据，但结束时报截断。
  std::string no_trailer =
      CompressBytes(Format::kGzip, input, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for missing trailer");
  no_trailer.resize(no_trailer.size() - 8);
  ErrorPtr trailer_error;
  const std::string partial =
      DecompressBytes(Format::kGzip, no_trailer, &trailer_error);
  COMPRESS_CHECK_ERROR(trailer_error, compress::ErrUnexpectedEof(),
                       "missing gzip trailer");
  COMPRESS_CHECK(partial == input);
}

void TestMultistream() {
  Announce("gzip: concatenated members");
  const std::string first = MakeLogText(30U * 1024U, 241);
  const std::string second = MakeRandomText(20U * 1024U, 242);
  ErrorPtr error;
  const std::string first_member =
      CompressBytes(Format::kGzip, first, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "first member");
  const std::string second_member =
      CompressBytes(Format::kGzip, second, compress::BestCompression, &error);
  COMPRESS_CHECK_NO_ERROR(error, "second member");
  const std::string empty_member =
      CompressBytes(Format::kGzip, std::string(), compress::DefaultCompression,
                    &error);
  COMPRESS_CHECK_NO_ERROR(error, "empty member");

  // 3 个成员（中间插一个空成员）→ 解出 first + second。
  const std::string concatenated = first_member + empty_member + second_member;
  ErrorPtr concat_error;
  const std::string output =
      DecompressBytes(Format::kGzip, concatenated, &concat_error);
  COMPRESS_CHECK_NO_ERROR(concat_error, "multistream");
  COMPRESS_CHECK(output == first + second);

  // Multistream(false)：只解第一个成员。
  StringSource source(concatenated, 17);
  Gzip::Reader<StringSource> reader(source);
  COMPRESS_CHECK(reader.Multistream());
  reader.Multistream(false);
  COMPRESS_CHECK(!reader.Multistream());
  ErrorPtr single_error;
  const std::string single = DrainReader(reader, 4096, &single_error);
  COMPRESS_CHECK_NO_ERROR(single_error, "multistream disabled");
  COMPRESS_CHECK(single == first);

  // 空成员单独也能解出空内容。
  ErrorPtr empty_error;
  COMPRESS_CHECK(DecompressBytes(Format::kGzip, empty_member, &empty_error).empty());
  COMPRESS_CHECK_NO_ERROR(empty_error, "empty member alone");
}

void TestTrailingGarbage() {
  Announce("gzip: trailing non-gzip bytes are an error (Go-compatible)");
  const std::string input = MakeLogText(8U * 1024U, 251);
  ErrorPtr error;
  const std::string compressed =
      CompressBytes(Format::kGzip, input, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for trailing");
  ErrorPtr trailing_error;
  DecompressBytes(Format::kGzip, compressed + std::string(16, 'x'),
                  &trailing_error);
  COMPRESS_CHECK_ERROR(trailing_error, compress::ErrHeader(),
                       "gzip trailing garbage");

  // 16 个 0x00：GNU gzip 会忽略并告警，Go 与本模块都报头错误。
  ErrorPtr zero_error;
  DecompressBytes(Format::kGzip, compressed + std::string(16, '\0'),
                  &zero_error);
  COMPRESS_CHECK_ERROR(zero_error, compress::ErrHeader(),
                       "gzip trailing zeros");
}

void TestChunkedIo() {
  Announce("gzip: chunked source/destination round-trip");
  const std::string input = MakeRandomText(50U * 1024U, 261);
  StringSink sink;
  {
    Gzip::Writer<StringSink> writer(sink, compress::BestCompression);
    COMPRESS_CHECK(WriteAll(writer, input));
    COMPRESS_CHECK_NO_ERROR(writer.Flush(), "gzip flush");
    COMPRESS_CHECK(WriteAll(writer, input));
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "gzip chunked close");
  }
  ErrorPtr error;
  const std::string output =
      DecompressBytes(Format::kGzip, sink.data(), &error, 3, 5);
  COMPRESS_CHECK_NO_ERROR(error, "gzip chunked decompress");
  COMPRESS_CHECK(output == input + input);
}

void TestWriterSemantics() {
  Announce("gzip: invalid level, close semantics, short writes");
  {
    StringSink sink;
    Gzip::Writer<StringSink> bad(sink, -3);
    COMPRESS_CHECK_ERROR(bad.Err(), compress::ErrInvalidLevel(), "gzip level");
  }
  {
    StringSink sink(7);  // 每次最多写 7 字节
    Gzip::Writer<StringSink> writer(sink, compress::BestSpeed);
    const std::string input = MakeLogText(40U * 1024U, 271);
    COMPRESS_CHECK(WriteAll(writer, input));
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "gzip short-write close");
    COMPRESS_CHECK_ERROR(writer.Close(), compress::ErrClosed(), "gzip reclose");
    COMPRESS_CHECK(writer.Write(ConstBytes("x")) == 0);
    COMPRESS_CHECK_ERROR(writer.Err(), compress::ErrClosed(), "gzip write closed");
    ErrorPtr error;
    COMPRESS_CHECK(DecompressBytes(Format::kGzip, sink.data(), &error) == input);
    COMPRESS_CHECK_NO_ERROR(error, "gzip short-write roundtrip");
  }
}

}  // namespace

/// gzip 测试入口。
void RunGzipTests() {
  Announce("=== gzip (RFC 1952) ===");
  TestHeaderAndTrailer();
  TestLevels();
  TestChecksumTampering();
  TestTruncated();
  TestMultistream();
  TestTrailingGarbage();
  TestChunkedIo();
  TestWriterSemantics();
}
