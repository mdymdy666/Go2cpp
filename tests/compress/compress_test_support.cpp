/**
 * @file compress_test_support.cpp
 * @brief 测试支撑的实现：数据生成、独立校验和、格式压缩/解压、临时目录与子进程。
 */
#include "compress_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <sys/wait.h>

namespace go2cpp_compress_test {

namespace {

/// 日志里重复出现的固定短语（低熵的主要来源）。
constexpr const char* const kLevels[] = {"INFO", "WARN", "ERROR", "DEBUG"};

constexpr const char* const kMessages[] = {
    "scheduler: fiber parked on channel, workers=8",
    "config: reload completed, 3 vars changed, generation=42",
    "io: epoll_wait returned 128 events in 0.4ms",
    "network: connection established to 10.0.0.7:8080",
    "timer: task 42 fired late by 3ms",
    "os: opened fd 1024 for /var/log/app.log",
    "compress: rotated 512 KiB segment, ratio=0.09",
    "runtime: gc cycle 17 finished, heap=12MiB",
};

}  // namespace

std::string MakeLogText(std::size_t size, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::string output;
  output.reserve(size + 128);
  std::uint64_t line = 0;
  while (output.size() < size) {
    const char* const level = kLevels[rng() % (sizeof(kLevels) / sizeof(kLevels[0]))];
    const char* const message =
        kMessages[rng() % (sizeof(kMessages) / sizeof(kMessages[0]))];
    char buffer[256];
    const int written = std::snprintf(
        buffer, sizeof(buffer),
        "2026-10-18T12:%02llu:%02llu.%03lluZ %-5s worker=%u %s line=%llu\n",
        static_cast<unsigned long long>((line / 60) % 60),
        static_cast<unsigned long long>(line % 60),
        static_cast<unsigned long long>(line % 1000), level,
        static_cast<unsigned>(rng() % 8), message,
        static_cast<unsigned long long>(line));
    if (written <= 0) {
      break;
    }
    output.append(buffer, static_cast<std::size_t>(written));
    ++line;
  }
  output.resize(size);
  return output;
}

std::string MakeRandomBytes(std::size_t size, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::string output;
  output.resize(size);
  std::size_t offset = 0;
  while (offset < size) {
    const std::uint64_t value = rng();
    const std::size_t take = std::min<std::size_t>(size - offset, sizeof(value));
    std::memcpy(output.data() + offset, &value, take);
    offset += take;
  }
  return output;
}

std::string MakeZeros(std::size_t size) { return std::string(size, '\0'); }

std::string MakeRandomText(std::size_t size, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  static constexpr char kAlphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-.:/";
  std::string output;
  output.resize(size);
  for (std::size_t index = 0; index < size; ++index) {
    output[index] = kAlphabet[rng() % (sizeof(kAlphabet) - 1)];
  }
  return output;
}

std::uint32_t Crc32(std::string_view data) {
  static const std::array<std::uint32_t, 256> kTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
      }
      table[index] = value;
    }
    return table;
  }();
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char byte : data) {
    crc = kTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFU;
}

std::uint32_t Adler32(std::string_view data) {
  constexpr std::uint32_t kMod = 65521U;
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const unsigned char byte : data) {
    a = (a + byte) % kMod;
    b = (b + a) % kMod;
  }
  return (b << 16) | a;
}

const char* FormatName(Format format) {
  switch (format) {
    case Format::kFlate:
      return "flate";
    case Format::kZlib:
      return "zlib";
    case Format::kGzip:
      return "gzip";
  }
  return "unknown";
}

namespace {

/// 与格式无关的写循环：写满 → Close。
template <typename WriterT>
void CompressInto(WriterT& writer, const std::string& input, ErrorPtr* error) {
  const std::size_t accepted = writer.Write(ConstBytes(input));
  if (accepted != input.size()) {
    if (error) {
      *error = writer.Err();
    }
    return;
  }
  if (error) {
    *error = writer.Close();
  }
}

}  // namespace

std::string CompressBytes(Format format, const std::string& input, int level,
                          ErrorPtr* error) {
  if (error) {
    *error = {};
  }
  StringSink sink;
  switch (format) {
    case Format::kFlate: {
      go2cpp::compress::Flate::Writer<StringSink> writer(sink, level);
      CompressInto(writer, input, error);
      break;
    }
    case Format::kZlib: {
      go2cpp::compress::Zlib::Writer<StringSink> writer(sink, level);
      CompressInto(writer, input, error);
      break;
    }
    case Format::kGzip: {
      go2cpp::compress::Gzip::Writer<StringSink> writer(sink, level);
      CompressInto(writer, input, error);
      break;
    }
  }
  return sink.data();
}

std::string DecompressBytes(Format format, const std::string& input,
                            ErrorPtr* error, std::size_t source_chunk,
                            std::size_t dest_chunk) {
  StringSource source(input, source_chunk);
  switch (format) {
    case Format::kFlate: {
      go2cpp::compress::Flate::Reader<StringSource> reader(source);
      return DrainReader(reader, dest_chunk, error);
    }
    case Format::kZlib: {
      go2cpp::compress::Zlib::Reader<StringSource> reader(source);
      return DrainReader(reader, dest_chunk, error);
    }
    case Format::kGzip: {
      go2cpp::compress::Gzip::Reader<StringSource> reader(source);
      return DrainReader(reader, dest_chunk, error);
    }
  }
  if (error) {
    *error = NewError("compress test: unknown format");
  }
  return {};
}

TempDir::TempDir() {
  char name_template[] = "/tmp/go2cpp-compress-XXXXXX";
  if (char* const created = ::mkdtemp(name_template)) {
    m_path = created;
  }
}

TempDir::~TempDir() {
  if (m_path.empty()) {
    return;
  }
  std::error_code ignored;
  std::filesystem::remove_all(m_path, ignored);
}

bool WriteFile(const std::string& path, std::string_view data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  return stream.good();
}

std::string ReadFile(const std::string& path, bool* ok) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    if (ok) {
      *ok = false;
    }
    return {};
  }
  std::string data((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  if (ok) {
    *ok = stream.good() || stream.eof();
  }
  return data;
}

int RunCommand(const std::string& command) {
  const int status = std::system(command.c_str());
  if (status == -1) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

bool CommandExists(const std::string& name) {
  return RunCommand("command -v '" + name + "' >/dev/null 2>&1") == 0;
}

}  // namespace go2cpp_compress_test
