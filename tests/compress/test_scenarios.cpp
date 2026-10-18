/**
 * @file test_scenarios.cpp
 * @brief 8 个真实场景模拟（覆盖 flate / zlib / gzip 的全部功能点）。
 *
 * @details 场景清单与验证目标：
 *
 *   1. **文本压缩（1 MiB 日志）**：gzip 级别 9 的压缩率与耗时；往返一致。
 *   2. **日志轮转（压缩旧日志）**：文件 → `.gz` 文件 → 删除原文件 → 回读校验；
 *      验证 gzip 头尾真的落盘（不只看内存里的字符串）。
 *   3. **读取系统 gzip 生成的文件**：`gzip -c` 产物经 `FileSource` 流式解压。
 *   4. **多成员 gzip**：两个成员拼到一个文件，默认解出完整内容；
 *      `Multistream(false)` 只解第一个成员。
 *   5. **损坏数据检测**：篡改 crc32 → `ErrChecksum`；篡改 magic → `ErrHeader`；
 *      截断 → `ErrUnexpectedEof`；zlib adler32 篡改 → `ErrChecksum`。
 *   6. **HTTP 响应体解压**：解析 `Content-Encoding: gzip` + `Content-Length`，
 *      用 3 字节分块的「socket」读 body 并解压，验证不越读。
 *   7. **流式写 + Flush 后读前缀**：模拟正在写入的 `.gz`（`tail -f` 场景），
 *      Flush 之后的部分可被独立解出，继续写并 Close 后得到完整数据。
 *   8. **zlib 缓存条目**：200 条缓存条目压缩/读取往返，单条损坏可被检测。
 */
#include "compress_test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace {

using namespace go2cpp_compress_test;
namespace compress = go2cpp::compress;
namespace fs = std::filesystem;

/// 逐块读文件的 Reader（模拟真实文件 IO，不一次读进内存）。
class FileSource {
 public:
  explicit FileSource(const std::string& path, std::size_t chunk = 4096)
      : m_stream(path, std::ios::binary), m_chunk(chunk) {}

  streams::ReadResult Read(char* buffer, std::size_t size) {
    if (!m_stream) {
      return streams::ReadResult{0, streams::IoEof()};
    }
    const std::size_t take = std::min(size, m_chunk);
    m_stream.read(buffer, static_cast<std::streamsize>(take));
    const std::streamsize got = m_stream.gcount();
    if (got <= 0) {
      return streams::ReadResult{0, streams::IoEof()};
    }
    return streams::ReadResult{static_cast<std::size_t>(got), {}};
  }

  bool ok() const { return m_stream.is_open(); }

 private:
  std::ifstream m_stream;
  std::size_t m_chunk;
};

/// 逐块写文件的 Writer。
class FileSink {
 public:
  explicit FileSink(const std::string& path)
      : m_stream(path, std::ios::binary | std::ios::trunc) {}

  streams::WriteResult Write(const char* data, std::size_t size) {
    m_stream.write(data, static_cast<std::streamsize>(size));
    if (!m_stream) {
      return streams::WriteResult{0, NewError("scenario: file write failed")};
    }
    return streams::WriteResult{size, {}};
  }

  bool ok() const { return m_stream.is_open(); }

  void Close() {
    m_stream.flush();
    m_stream.close();
  }

 private:
  std::ofstream m_stream;
};

/// 把 `std::string` 按固定块大小、并限制总字节数的 Reader（模拟 socket + Content-Length）。
class LimitedChunkSource {
 public:
  LimitedChunkSource(std::string data, std::size_t limit, std::size_t chunk)
      : m_data(std::move(data)),
        m_limit(std::min(limit, m_data.size())),
        m_chunk(chunk) {}

  streams::ReadResult Read(char* buffer, std::size_t size) {
    if (m_offset >= m_limit) {
      return streams::ReadResult{0, streams::IoEof()};
    }
    std::size_t take = std::min({size, m_chunk, m_limit - m_offset});
    std::memcpy(buffer, m_data.data() + m_offset, take);
    m_offset += take;
    return streams::ReadResult{take, {}};
  }

  std::size_t consumed() const { return m_offset; }

 private:
  std::string m_data;
  std::size_t m_limit{0};
  std::size_t m_offset{0};
  std::size_t m_chunk{0};
};

void ScenarioTextLogCompression() {
  Announce("scenario 1: compress 1 MiB application log (gzip level 9)");
  const std::string log = MakeLogText(1024U * 1024U, 501);
  const auto start = Clock::now();
  ErrorPtr error;
  const std::string compressed =
      CompressBytes(Format::kGzip, log, compress::BestCompression, &error);
  const double compress_ms = MsSince(start);
  COMPRESS_CHECK_NO_ERROR(error, "scenario 1 compress");
  const auto decompress_start = Clock::now();
  ErrorPtr decompress_error;
  const std::string output =
      DecompressBytes(Format::kGzip, compressed, &decompress_error);
  const double decompress_ms = MsSince(decompress_start);
  COMPRESS_CHECK_NO_ERROR(decompress_error, "scenario 1 decompress");
  COMPRESS_CHECK(output == log);
  COMPRESS_CHECK(compressed.size() < log.size() / 4);  // 日志压缩率应远好于 4:1
  std::printf(
      "[compress] scenario 1: %zu -> %zu bytes (%.2fx, %.1f%% saved), "
      "compress=%.1fms, decompress=%.1fms\n",
      log.size(), compressed.size(),
      static_cast<double>(log.size()) / static_cast<double>(compressed.size()),
      100.0 * (1.0 - static_cast<double>(compressed.size()) /
                         static_cast<double>(log.size())),
      compress_ms, decompress_ms);
}

void ScenarioLogRotation() {
  Announce("scenario 2: log rotation (compress app.log -> app.log.gz, delete raw)");
  TempDir dir;
  COMPRESS_REQUIRE(dir.ok());
  const std::string raw_path = dir.File("app.log");
  const std::string gz_path = dir.File("app.log.gz");
  const std::string log = MakeLogText(512U * 1024U, 502);
  COMPRESS_REQUIRE(WriteFile(raw_path, log));
  {
    FileSink sink(gz_path);
    COMPRESS_REQUIRE(sink.ok());
    compress::Gzip::Writer<FileSink> writer(sink, compress::BestSpeed);
    COMPRESS_CHECK(WriteAll(writer, log));
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "scenario 2 close");
    sink.Close();
  }
  COMPRESS_REQUIRE(fs::remove(raw_path));
  COMPRESS_CHECK(!fs::exists(raw_path));
  COMPRESS_REQUIRE(fs::exists(gz_path));
  COMPRESS_CHECK(fs::file_size(gz_path) < log.size());

  bool read_ok = false;
  const std::string on_disk = ReadFile(gz_path, &read_ok);
  COMPRESS_REQUIRE(read_ok);
  COMPRESS_CHECK(static_cast<unsigned char>(on_disk[0]) == 0x1F);
  COMPRESS_CHECK(static_cast<unsigned char>(on_disk[1]) == 0x8B);

  FileSource source(gz_path, 1024);
  COMPRESS_REQUIRE(source.ok());
  compress::Gzip::Reader<FileSource> reader(source);
  ErrorPtr error;
  const std::string restored = DrainReader(reader, 4096, &error);
  COMPRESS_CHECK_NO_ERROR(error, "scenario 2 read back");
  COMPRESS_CHECK(restored == log);
  std::printf("[compress] scenario 2: %zu -> %zu bytes, round-trip ok\n",
              log.size(), fs::file_size(gz_path));
}

void ScenarioReadSystemGzipFile() {
  Announce("scenario 3: read a file produced by system gzip");
  TempDir dir;
  COMPRESS_REQUIRE(dir.ok());
  if (!CommandExists("gzip")) {
    std::printf("[compress] scenario 3: SKIP (gzip not found)\n");
    return;
  }
  const std::string raw_path = dir.File("payload.txt");
  const std::string gz_path = dir.File("payload.txt.gz");
  const std::string content = MakeLogText(300U * 1024U, 503);
  COMPRESS_REQUIRE(WriteFile(raw_path, content));
  COMPRESS_REQUIRE(RunCommand("gzip -c '" + raw_path + "' > '" + gz_path +
                              "' 2>/dev/null") == 0);
  COMPRESS_REQUIRE(fs::exists(gz_path));
  FileSource source(gz_path, 777);
  COMPRESS_REQUIRE(source.ok());
  compress::Gzip::Reader<FileSource> reader(source);
  ErrorPtr error;
  const std::string output = DrainReader(reader, 8192, &error);
  COMPRESS_CHECK_NO_ERROR(error, "scenario 3 decompress");
  COMPRESS_CHECK(output == content);
  std::printf("[compress] scenario 3: system gzip file %zu -> %zu bytes read ok\n",
              fs::file_size(gz_path), output.size());
}

void ScenarioMultimember() {
  Announce("scenario 4: concatenated gzip members on disk");
  TempDir dir;
  COMPRESS_REQUIRE(dir.ok());
  const std::string first = MakeLogText(32U * 1024U, 504);
  const std::string second = MakeRandomText(24U * 1024U, 505);
  const std::string first_path = dir.File("part1.gz");
  const std::string second_path = dir.File("part2.gz");
  const std::string joined_path = dir.File("joined.gz");
  for (const auto& entry :
       {std::make_pair(&first_path, &first), std::make_pair(&second_path, &second)}) {
    FileSink sink(*entry.first);
    COMPRESS_REQUIRE(sink.ok());
    compress::Gzip::Writer<FileSink> writer(sink, compress::BestSpeed);
    COMPRESS_CHECK(WriteAll(writer, *entry.second));
    COMPRESS_CHECK_NO_ERROR(writer.Close(), "scenario 4 member close");
    sink.Close();
  }
  bool read_ok = false;
  const std::string first_bytes = ReadFile(first_path, &read_ok);
  COMPRESS_REQUIRE(read_ok);
  const std::string second_bytes = ReadFile(second_path, &read_ok);
  COMPRESS_REQUIRE(read_ok);
  COMPRESS_REQUIRE(WriteFile(joined_path, first_bytes + second_bytes));

  FileSource source(joined_path, 4096);
  COMPRESS_REQUIRE(source.ok());
  compress::Gzip::Reader<FileSource> reader(source);
  ErrorPtr error;
  const std::string joined = DrainReader(reader, 4096, &error);
  COMPRESS_CHECK_NO_ERROR(error, "scenario 4 multistream");
  COMPRESS_CHECK(joined == first + second);

  FileSource single_source(joined_path, 4096);
  compress::Gzip::Reader<FileSource> single_reader(single_source);
  single_reader.Multistream(false);
  ErrorPtr single_error;
  const std::string single = DrainReader(single_reader, 4096, &single_error);
  COMPRESS_CHECK_NO_ERROR(single_error, "scenario 4 first member");
  COMPRESS_CHECK(single == first);

  if (CommandExists("gzip")) {
    const std::string out_path = dir.File("joined.out");
    COMPRESS_REQUIRE(RunCommand("gzip -dc '" + joined_path + "' > '" + out_path +
                                "' 2>/dev/null") == 0);
    const std::string gzip_output = ReadFile(out_path, &read_ok);
    COMPRESS_CHECK(read_ok);
    COMPRESS_CHECK(gzip_output == first + second);
  }
  std::printf("[compress] scenario 4: %zu + %zu bytes across two members ok\n",
              first.size(), second.size());
}

void ScenarioCorruptionDetection() {
  Announce("scenario 5: corrupted streams are rejected");
  TempDir dir;
  COMPRESS_REQUIRE(dir.ok());
  const std::string payload = MakeLogText(64U * 1024U, 506);
  ErrorPtr error;
  const std::string valid =
      CompressBytes(Format::kGzip, payload, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "scenario 5 compress");

  // (a) 篡改 crc32（尾部 8 字节的第一个字节）。
  std::string bad_crc = valid;
  bad_crc[bad_crc.size() - 8] =
      static_cast<char>(static_cast<unsigned char>(bad_crc[bad_crc.size() - 8]) ^ 0x5AU);
  ErrorPtr crc_error;
  const std::string crc_data =
      DecompressBytes(Format::kGzip, bad_crc, &crc_error);
  COMPRESS_CHECK_ERROR(crc_error, compress::ErrChecksum(), "scenario 5 crc32");
  COMPRESS_CHECK(crc_data == payload);  // 数据本身没错，错在校验和

  // (b) 篡改头 magic。
  std::string bad_magic = valid;
  bad_magic[0] = static_cast<char>(0x00);
  ErrorPtr magic_error;
  DecompressBytes(Format::kGzip, bad_magic, &magic_error);
  COMPRESS_CHECK_ERROR(magic_error, compress::ErrHeader(), "scenario 5 magic");

  // (c) 截断（磁盘上半截文件）。
  const std::string truncated_path = dir.File("truncated.gz");
  COMPRESS_REQUIRE(WriteFile(truncated_path, valid.substr(0, valid.size() * 2 / 3)));
  FileSource source(truncated_path, 512);
  compress::Gzip::Reader<FileSource> reader(source);
  ErrorPtr truncated_error;
  DrainReader(reader, 4096, &truncated_error);
  COMPRESS_CHECK_ERROR(truncated_error, compress::ErrUnexpectedEof(),
                       "scenario 5 truncated");

  // (d) zlib 的 adler32 篡改。
  const std::string zlib_valid =
      CompressBytes(Format::kZlib, payload, compress::BestSpeed, &error);
  COMPRESS_CHECK_NO_ERROR(error, "scenario 5 zlib compress");
  std::string zlib_bad = zlib_valid;
  zlib_bad[zlib_bad.size() - 2] =
      static_cast<char>(static_cast<unsigned char>(zlib_bad[zlib_bad.size() - 2]) ^ 0x01U);
  ErrorPtr zlib_error;
  DecompressBytes(Format::kZlib, zlib_bad, &zlib_error);
  COMPRESS_CHECK_ERROR(zlib_error, compress::ErrChecksum(),
                       "scenario 5 adler32");
  std::printf("[compress] scenario 5: crc32/magic/truncation/adler32 all detected\n");
}

void ScenarioHttpBody() {
  Announce("scenario 6: HTTP response body with Content-Encoding: gzip");
  const std::string body = MakeLogText(128U * 1024U, 507);
  ErrorPtr error;
  const std::string compressed =
      CompressBytes(Format::kGzip, body, compress::BestCompression, &error);
  COMPRESS_CHECK_NO_ERROR(error, "scenario 6 compress");
  const std::string headers =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "Content-Encoding: gzip\r\n"
      "Content-Length: " +
      std::to_string(compressed.size()) +
      "\r\n"
      "Connection: keep-alive\r\n"
      "\r\n";
  // 模拟「先读头，再按 Content-Length 读 body」的客户端。
  const std::size_t separator = headers.find("\r\n\r\n");
  COMPRESS_REQUIRE(separator != std::string::npos);
  COMPRESS_CHECK(headers.find("Content-Encoding: gzip") != std::string::npos);
  const std::string length_header = "Content-Length: ";
  const std::size_t length_pos = headers.find(length_header);
  COMPRESS_REQUIRE(length_pos != std::string::npos);
  const std::size_t length_end = headers.find("\r\n", length_pos);
  COMPRESS_REQUIRE(length_end != std::string::npos);
  const std::size_t declared = static_cast<std::size_t>(std::stoul(
      headers.substr(length_pos + length_header.size(),
                     length_end - length_pos - length_header.size())));
  COMPRESS_CHECK(declared == compressed.size());

  LimitedChunkSource source(compressed, declared, 3);  // 3 字节一块的 socket
  compress::Gzip::Reader<LimitedChunkSource> reader(source);
  ErrorPtr decompress_error;
  const std::string decoded = DrainReader(reader, 7000, &decompress_error);
  COMPRESS_CHECK_NO_ERROR(decompress_error, "scenario 6 decompress");
  COMPRESS_CHECK(decoded == body);
  COMPRESS_CHECK(source.consumed() == declared);  // 没有越读 Content-Length
  std::printf(
      "[compress] scenario 6: body %zu -> %zu bytes, read %zu bytes of body\n",
      body.size(), compressed.size(), source.consumed());
}

void ScenarioStreamingTail() {
  Announce("scenario 7: streaming gzip (read the prefix after Flush)");
  const std::string first = MakeLogText(128U * 1024U, 508);
  const std::string second = MakeLogText(128U * 1024U, 509);
  StringSink sink;
  compress::Gzip::Writer<StringSink> writer(sink, compress::BestSpeed);
  COMPRESS_CHECK(WriteAll(writer, first));
  COMPRESS_CHECK_NO_ERROR(writer.Flush(), "scenario 7 flush");

  // 模拟另一个进程 tail 这个正在写的 .gz：只有 Flush 之前的数据可解。
  const std::string snapshot = sink.data();
  ErrorPtr prefix_error;
  const std::string prefix =
      DecompressBytes(Format::kGzip, snapshot, &prefix_error);
  COMPRESS_CHECK(prefix == first);
  COMPRESS_CHECK_ERROR(prefix_error, compress::ErrUnexpectedEof(),
                       "scenario 7 prefix ends mid-stream");

  COMPRESS_CHECK(WriteAll(writer, second));
  COMPRESS_CHECK_NO_ERROR(writer.Close(), "scenario 7 close");
  ErrorPtr full_error;
  const std::string full = DecompressBytes(Format::kGzip, sink.data(), &full_error);
  COMPRESS_CHECK_NO_ERROR(full_error, "scenario 7 full");
  COMPRESS_CHECK(full == first + second);
  std::printf("[compress] scenario 7: prefix=%zu bytes after Flush, full=%zu bytes\n",
              prefix.size(), full.size());
}

void ScenarioZlibCache() {
  Announce("scenario 8: zlib-compressed cache entries (200 entries)");
  std::map<std::string, std::string> cache;
  std::vector<std::string> keys;
  keys.reserve(200);
  std::size_t raw_bytes = 0;
  std::size_t stored_bytes = 0;
  for (int index = 0; index < 200; ++index) {
    const std::string key = "cache-key-" + std::to_string(index);
    const std::string value = MakeLogText(2048, static_cast<std::uint64_t>(600 + index));
    raw_bytes += value.size();
    ErrorPtr error;
    const std::string packed =
        CompressBytes(Format::kZlib, value, compress::DefaultCompression, &error);
    COMPRESS_CHECK_NO_ERROR(error, "scenario 8 compress");
    stored_bytes += packed.size();
    cache.emplace(key, packed);
    keys.push_back(key);
  }
  COMPRESS_CHECK(stored_bytes < raw_bytes / 2);
  for (const std::string& key : keys) {
    const std::string& packed = cache.at(key);
    ErrorPtr error;
    const std::string value = DecompressBytes(Format::kZlib, packed, &error);
    COMPRESS_CHECK_NO_ERROR(error, "scenario 8 decompress");
    COMPRESS_CHECK(value.size() == 2048);
  }
  // 单条缓存损坏必须被检出，而不是返回垃圾数据。
  std::string damaged = cache.at(keys[42]);
  damaged[damaged.size() - 1] =
      static_cast<char>(static_cast<unsigned char>(damaged.back()) ^ 0xFFU);
  ErrorPtr damaged_error;
  DecompressBytes(Format::kZlib, damaged, &damaged_error);
  COMPRESS_CHECK_ERROR(damaged_error, compress::ErrChecksum(),
                       "scenario 8 damaged entry");
  std::printf("[compress] scenario 8: %zu -> %zu bytes across 200 entries\n",
              raw_bytes, stored_bytes);
}

}  // namespace

/// 真实场景测试入口。
void RunScenarioTests() {
  Announce("=== real-world scenarios ===");
  ScenarioTextLogCompression();
  ScenarioLogRotation();
  ScenarioReadSystemGzipFile();
  ScenarioMultimember();
  ScenarioCorruptionDetection();
  ScenarioHttpBody();
  ScenarioStreamingTail();
  ScenarioZlibCache();
}
