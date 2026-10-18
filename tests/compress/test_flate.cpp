/**
 * @file test_flate.cpp
 * @brief `compress::Flate`（raw deflate）单元测试：级别、边界、块式 IO、错误语义。
 *
 * @details 覆盖点（对照 PROJECT_RULES 第 3 节）：
 *   - 四个级别的往返一致性 + `NoCompression` 的「不压缩」性质；
 *   - 空输入、单字节、1 MiB 全零、1 MiB 随机（边界与极端熵）；
 *   - 1 字节分块写、1 字节分块读、7 字节目标缓冲（部分读写路径）；
 *   - `Flush()` 之后流仍可继续写（流式语义）；
 *   - `Close()` 的幂等性与 `ErrClosed`；
 *   - 非法级别 → `ErrInvalidLevel`（不崩溃、不静默降级）；
 *   - 截断 → `ErrUnexpectedEof`；垃圾字节 → `ErrCorrupt`；
 *   - 忘记 `Close()` → 解压方拿到 `ErrUnexpectedEof`（半截流的真实后果）；
 *   - 特殊 Reader：数据 + EOF 同一次返回、永远空读、立刻报错。
 *
 * 压测（吞吐/压缩率）在 `test_stress.cpp`，系统互操作在 `test_interop.cpp`。
 */
#include "compress_test_support.hpp"

namespace {

using namespace go2cpp_compress_test;
namespace compress = go2cpp::compress;
using compress::ByteSpan;
using compress::Flate;
using streams::ReadResult;
using streams::WriteResult;

void TestLevels() {
  Announce("flate: level round-trips");
  const std::string input = MakeLogText(200U * 1024U, 11);
  std::vector<std::size_t> sizes;
  const int levels[] = {compress::NoCompression, compress::BestSpeed,
                        compress::DefaultCompression,
                        compress::BestCompression};
  for (const int level : levels) {
    ErrorPtr error;
    const std::string compressed =
        CompressBytes(Format::kFlate, input, level, &error);
    COMPRESS_CHECK_NO_ERROR(error, "flate compress");
    COMPRESS_CHECK(!compressed.empty());
    ErrorPtr decompress_error;
    const std::string output =
        DecompressBytes(Format::kFlate, compressed, &decompress_error);
    COMPRESS_CHECK_NO_ERROR(decompress_error, "flate decompress");
    COMPRESS_CHECK(output == input);
    sizes.push_back(compressed.size());
  }
  // 级别 0 = 只存储：体积必须 ≥ 原始数据（stored 块有 5 字节/64KiB 开销）。
  COMPRESS_CHECK(sizes[0] >= input.size());
  COMPRESS_CHECK(sizes[0] <= input.size() + input.size() / 1024 + 64);
  // 级别 9 的压缩率必须明显优于级别 1（日志文本）。
  COMPRESS_CHECK(sizes[3] < sizes[1]);
  std::printf("[compress] flate sizes: raw=%zu, nocomp=%zu, speed=%zu, "
              "default=%zu, best=%zu\n",
              input.size(), sizes[0], sizes[1], sizes[2], sizes[3]);
}

void TestBoundaries() {
  Announce("flate: boundaries (empty / single byte / zeros / random)");
  const std::vector<std::pair<const char*, std::string>> cases = {
      {"empty", std::string()},
      {"single", std::string(1, 'A')},
      {"nul-byte", std::string(1, '\0')},
      {"zeros-1m", MakeZeros(1024U * 1024U)},
      {"random-1m", MakeRandomBytes(1024U * 1024U, 7)},
  };
  for (const auto& entry : cases) {
    ErrorPtr error;
    const std::string compressed =
        CompressBytes(Format::kFlate, entry.second, compress::DefaultCompression,
                      &error);
    COMPRESS_CHECK_NO_ERROR(error, entry.first);
    ErrorPtr decompress_error;
    const std::string output =
        DecompressBytes(Format::kFlate, compressed, &decompress_error);
    COMPRESS_CHECK_NO_ERROR(decompress_error, entry.first);
    COMPRESS_CHECK(output == entry.second);
  }
  // 空输入的 raw deflate 流本身非空（至少有一个结束块）。
  ErrorPtr error;
  const std::string empty_compressed =
      CompressBytes(Format::kFlate, std::string(), compress::BestCompression,
                    &error);
  COMPRESS_CHECK_NO_ERROR(error, "empty compress");
  COMPRESS_CHECK(!empty_compressed.empty());
}

void TestChunkedIo() {
  Announce("flate: 1-byte write chunks, 1-byte source chunks, 7-byte reads");
  const std::string input = MakeRandomText(10U * 1024U, 21);
  StringSink sink;
  {
    Flate::Writer<StringSink> writer(sink, compress::DefaultCompression);
    std::size_t accepted = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
      accepted += writer.Write(
          ByteSpan(reinterpret_cast<std::uint8_t*>(
                       const_cast<char*>(input.data() + index)),
                   1));
    }
    COMPRESS_CHECK(accepted == input.size());
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "chunked close");
  }
  const std::string compressed = sink.data();
  ErrorPtr error;
  const std::string output =
      DecompressBytes(Format::kFlate, compressed, &error, 1, 7);
  COMPRESS_CHECK_NO_ERROR(error, "chunked decompress");
  COMPRESS_CHECK(output == input);
}

void TestFlush() {
  Announce("flate: Flush keeps the stream open");
  const std::string first = MakeLogText(64U * 1024U, 31);
  const std::string second = MakeLogText(64U * 1024U, 32);
  StringSink sink;
  {
    Flate::Writer<StringSink> writer(sink, compress::BestSpeed);
    COMPRESS_CHECK(WriteAll(writer, first));
    COMPRESS_CHECK_NO_ERROR(writer.Flush(), "flush #1");
    const std::size_t after_flush = sink.data().size();
    COMPRESS_CHECK(after_flush > 0);
    COMPRESS_CHECK(WriteAll(writer, second));
    COMPRESS_CHECK_NO_ERROR(writer.Flush(), "flush #2");
    COMPRESS_CHECK(sink.data().size() > after_flush);
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "close after flush");
  }
  ErrorPtr error;
  const std::string output =
      DecompressBytes(Format::kFlate, sink.data(), &error);
  COMPRESS_CHECK_NO_ERROR(error, "decompress after flush");
  COMPRESS_CHECK(output == first + second);
}

void TestCloseSemantics() {
  Announce("flate: Close idempotency and ErrClosed");
  StringSink sink;
  Flate::Writer<StringSink> writer(sink);
  COMPRESS_CHECK(WriteAll(writer, "hello flate"));
  COMPRESS_CHECK_NO_ERROR(writer.Close(), "first close");
  COMPRESS_CHECK(writer.Closed());
  COMPRESS_CHECK_ERROR(writer.Close(), compress::ErrClosed(), "second close");
  COMPRESS_CHECK(writer.Write(ConstBytes("more")) == 0);
  COMPRESS_CHECK_ERROR(writer.Err(), compress::ErrClosed(), "write after close");
  COMPRESS_CHECK_ERROR(writer.Flush(), compress::ErrClosed(), "flush after close");
}

void TestInvalidLevel() {
  Announce("flate: invalid level is sticky, not silently downgraded");
  StringSink sink;
  Flate::Writer<StringSink> writer(sink, 10);
  COMPRESS_CHECK_ERROR(writer.Err(), compress::ErrInvalidLevel(), "ctor level");
  COMPRESS_CHECK(writer.Write(ConstBytes("abc")) == 0);
  COMPRESS_CHECK_ERROR(writer.Err(), compress::ErrInvalidLevel(), "write level");
  COMPRESS_CHECK_ERROR(writer.Close(), compress::ErrInvalidLevel(), "close level");
  COMPRESS_CHECK(sink.data().empty());

  Flate::Writer<StringSink> low(sink, -2);  // Go 的 HuffmanOnly，本模块不支持
  COMPRESS_CHECK_ERROR(low.Err(), compress::ErrInvalidLevel(), "negative level");
}

void TestTruncatedAndGarbage() {
  Announce("flate: truncated input and garbage bytes");
  const std::string input = MakeLogText(32U * 1024U, 41);
  ErrorPtr error;
  std::string compressed =
      CompressBytes(Format::kFlate, input, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for truncation");
  compressed.resize(compressed.size() / 2);
  ErrorPtr truncated_error;
  DecompressBytes(Format::kFlate, compressed, &truncated_error);
  COMPRESS_CHECK_ERROR(truncated_error, compress::ErrUnexpectedEof(),
                       "truncated raw deflate");

  // 0xFF 首字节 = BFINAL=1 + BTYPE=11（保留块类型）→ 非法块。
  const std::string garbage("\xFF\xFF\xFF\xFF\xFF", 5);
  ErrorPtr garbage_error;
  DecompressBytes(Format::kFlate, garbage, &garbage_error);
  COMPRESS_CHECK_ERROR(garbage_error, compress::ErrCorrupt(), "garbage bytes");

  // 空输入：流还没开始就 EOF。
  ErrorPtr empty_error;
  DecompressBytes(Format::kFlate, std::string(), &empty_error);
  COMPRESS_CHECK_ERROR(empty_error, compress::ErrUnexpectedEof(),
                       "empty compressed input");
}

void TestShortWrites() {
  Announce("flate: short-write sink (3 bytes per call)");
  StringSink sink(3);
  const std::string input = MakeLogText(50U * 1024U, 51);
  {
    Flate::Writer<StringSink> writer(sink, compress::BestCompression);
    COMPRESS_CHECK(WriteAll(writer, input));
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "short-write close");
  }
  COMPRESS_CHECK(sink.calls() > 10);
  ErrorPtr error;
  const std::string output =
      DecompressBytes(Format::kFlate, sink.data(), &error);
  COMPRESS_CHECK_NO_ERROR(error, "short-write roundtrip");
  COMPRESS_CHECK(output == input);
}

void TestWriterWithoutClose() {
  Announce("flate: forgetting Close produces a truncated stream");
  const std::string input = MakeLogText(16U * 1024U, 61);
  StringSink sink;
  {
    Flate::Writer<StringSink> writer(sink, compress::BestSpeed);
    COMPRESS_CHECK(WriteAll(writer, input));
    // 故意不 Close：析构只释放 zlib 状态，不写结束块。
  }
  ErrorPtr error;
  DecompressBytes(Format::kFlate, sink.data(), &error);
  COMPRESS_CHECK_ERROR(error, compress::ErrUnexpectedEof(), "no close");
}

void TestSpecialSources() {
  Announce("flate: data+EOF source, idle source, failing source");
  const std::string input = MakeRandomText(4096, 71);
  ErrorPtr error;
  const std::string compressed =
      CompressBytes(Format::kFlate, input, compress::BestCompression, &error);
  COMPRESS_CHECK_NO_ERROR(error, "compress for special sources");

  {
    DataAndEofSource source(compressed);
    Flate::Reader<DataAndEofSource> reader(source);
    ErrorPtr read_error;
    const std::string output = DrainReader(reader, 1024, &read_error);
    COMPRESS_CHECK_NO_ERROR(read_error, "data+EOF source");
    COMPRESS_CHECK(output == input);
  }
  {
    IdleSource source;
    Flate::Reader<IdleSource> reader(source);
    ErrorPtr read_error;
    DrainReader(reader, 1024, &read_error);
    COMPRESS_CHECK_ERROR(read_error, streams::IoErrNoProgress(),
                         "idle source");
    COMPRESS_CHECK(source.calls() >= 100);
  }
  {
    const ErrorPtr boom = NewError("boom");
    FailingSource source(boom);
    Flate::Reader<FailingSource> reader(source);
    ErrorPtr read_error;
    DrainReader(reader, 1024, &read_error);
    COMPRESS_CHECK(go2cpp::Is(read_error, boom));
    COMPRESS_CHECK(read_error == boom);
  }
}

void TestStreamsAdapter() {
  Announce("flate: streams::Reader / streams::Writer adapters");
  const std::string input = MakeLogText(8U * 1024U, 81);
  StringSink sink;
  {
    Flate::Writer<StringSink> writer(sink, compress::BestSpeed);
    const WriteResult written = writer.Write(input.data(), input.size());
    COMPRESS_CHECK(written.ok());
    COMPRESS_CHECK(written.count == input.size());
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "streams writer close");
  }
  StringSource source(sink.data(), 13);
  Flate::Reader<StringSource> reader(source);
  std::string output;
  char buffer[128];
  for (;;) {
    const ReadResult result = reader.Read(buffer, sizeof(buffer));
    output.append(buffer, result.count);
    if (result.error) {
      COMPRESS_CHECK(go2cpp::Is(result.error, streams::IoEof()));
      break;
    }
    COMPRESS_REQUIRE(result.count > 0);
  }
  COMPRESS_CHECK(output == input);
}

}  // namespace

/// flate 测试入口。
void RunFlateTests() {
  Announce("=== flate (raw deflate) ===");
  TestLevels();
  TestBoundaries();
  TestChunkedIo();
  TestFlush();
  TestCloseSemantics();
  TestInvalidLevel();
  TestTruncatedAndGarbage();
  TestShortWrites();
  TestWriterWithoutClose();
  TestSpecialSources();
  TestStreamsAdapter();
}
