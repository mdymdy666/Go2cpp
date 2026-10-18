/**
 * @file compress_test_support.hpp
 * @brief `go2cpp_compress_test` 的共享断言、IO 桩、数据生成器与互操作工具。
 *
 * @details 所有测试 TU 共用一个可执行文件（`go2cpp_compress_test`），本头提供：
 *
 *   - `COMPRESS_CHECK` / `COMPRESS_REQUIRE` 断言宏（失败累计，`REQUIRE` 立即中止）；
 *   - `StringSource` / `StringSink`：把 `std::string` 包成 `streams` 契约的
 *     最小 Reader/Writer 桩，可限制单次读写字节数以覆盖部分读写路径；
 *   - 故障注入桩：`DataAndEofSource`（同一次返回数据 + EOF）、`IdleSource`
 *     （永远 0 字节且无错）、`FailingSource` / `FailingSink`（返回自定义错误）；
 *   - 数据生成器：低熵日志文本、高熵随机字节、全零；
 *   - **独立实现的** CRC32 / Adler32（不调用 zlib）：用来验证我们写出的
 *     gzip / zlib 尾部校验和是否正确。若调用 zlib 的 `crc32()` 校验 zlib 的
 *     输出，就是「同一个实现自己证明自己」，没有意义；
 *   - `CompressBytes` / `DecompressBytes`：按格式压缩/解压整段数据；
 *   - `TempDir` + `RunCommand`：系统 `gzip` / `python3` 互操作测试用的临时目录
 *     与子进程执行器（析构时 `remove_all`，不留残留文件）。
 */
#pragma once

#include "go2cpp/compress/compress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp_compress_test {

namespace streams = go2cpp::streams;
using go2cpp::ErrorPtr;
using go2cpp::Is;
using go2cpp::NewError;
using go2cpp::compress::AsBytes;
using go2cpp::compress::ByteSpan;

// ============================================================================
// 断言与计时
// ============================================================================

/// 累计失败数；main 据此返回非零。
inline std::atomic<int> g_failures{0};

/// 累计断言数（打印在结尾，便于判断覆盖面）。
inline std::atomic<int> g_checks{0};

/// 记录一次断言失败，不中断测试。
inline void Check(bool condition, const char* expression, const char* file,
                  int line) {
  g_checks.fetch_add(1, std::memory_order_relaxed);
  if (condition) {
    return;
  }
  g_failures.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr, "%s:%d: compress check failed: %s\n", file, line,
               expression);
  std::fflush(stderr);
}

/// 记录一次致命失败并立即中止（继续跑只会建立在已损坏的状态上）。
[[noreturn]] inline void Fail(const char* expression, const char* file,
                              int line) {
  std::fprintf(stderr, "%s:%d: compress REQUIRE failed: %s\n", file, line,
               expression);
  std::fflush(stderr);
  std::abort();
}

/// 断言「没有错误」；失败时打印错误文本，比只打印表达式可诊断得多。
inline void CheckNoError(const ErrorPtr& error, const char* context,
                         const char* file, int line) {
  g_checks.fetch_add(1, std::memory_order_relaxed);
  if (!error) {
    return;
  }
  g_failures.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr, "%s:%d: %s: unexpected error: %s\n", file, line, context,
               go2cpp::ErrorMessage(error).c_str());
  std::fflush(stderr);
}

/// 断言错误身份（`errors.Is` 语义）；失败时打印实际/期望文本。
inline void CheckIsError(const ErrorPtr& error, const ErrorPtr& expected,
                         const char* context, const char* file, int line) {
  g_checks.fetch_add(1, std::memory_order_relaxed);
  if (go2cpp::Is(error, expected)) {
    return;
  }
  g_failures.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr, "%s:%d: %s: error mismatch: got '%s', want '%s'\n", file,
               line, context, go2cpp::ErrorMessage(error).c_str(),
               go2cpp::ErrorMessage(expected).c_str());
  std::fflush(stderr);
}

#define COMPRESS_CHECK(expression) \
  ::go2cpp_compress_test::Check((expression), #expression, __FILE__, __LINE__)

#define COMPRESS_CHECK_NO_ERROR(error, context) \
  ::go2cpp_compress_test::CheckNoError((error), (context), __FILE__, __LINE__)

#define COMPRESS_CHECK_ERROR(error, expected, context)                    \
  ::go2cpp_compress_test::CheckIsError((error), (expected), (context),    \
                                       __FILE__, __LINE__)

#define COMPRESS_REQUIRE(expression)                       \
  do {                                                     \
    if (!(expression)) {                                   \
      ::go2cpp_compress_test::Fail(#expression, __FILE__, __LINE__); \
    }                                                      \
  } while (false)

/// 打印当前测试段名，便于 ctest 输出定位。
inline void Announce(const char* name) {
  std::printf("[compress] %s\n", name);
  std::fflush(stdout);
}

using Clock = std::chrono::steady_clock;

/// 从 `start` 到现在的毫秒数。
inline double MsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

/// 中位数（输入会被排序复制，不改原序列）。
inline double MedianMs(std::vector<double> samples) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t middle = samples.size() / 2;
  if (samples.size() % 2 == 1) {
    return samples[middle];
  }
  return (samples[middle - 1] + samples[middle]) / 2.0;
}

// ============================================================================
// 字节视图辅助
// ============================================================================

/// 把只读字符串包成 `ByteSpan`（只读语义，但 `ByteSpan` 本质是可写的）。
inline ByteSpan ConstBytes(const std::string& text) {
  return ByteSpan(reinterpret_cast<std::uint8_t*>(
                      const_cast<char*>(text.data())),
                  text.size());
}

// ============================================================================
// IO 桩
// ============================================================================

/**
 * @brief 从 `std::string` 按块交数据的 Reader 桩。
 * @details `max_chunk == 0` 表示一次给完；否则每次最多给 `max_chunk` 字节，
 *   用来覆盖「底层只给一点点」的路径（与 `bufio` 测试里的分块读同思路）。
 */
class StringSource {
 public:
  explicit StringSource(std::string data, std::size_t max_chunk = 0)
      : m_data(std::move(data)), m_max_chunk(max_chunk) {}

  streams::ReadResult Read(char* buffer, std::size_t size) {
    ++m_calls;
    if (m_offset >= m_data.size()) {
      return streams::ReadResult{0, streams::IoEof()};
    }
    std::size_t take = m_data.size() - m_offset;
    take = take < size ? take : size;
    if (m_max_chunk != 0) {
      take = take < m_max_chunk ? take : m_max_chunk;
    }
    std::memcpy(buffer, m_data.data() + m_offset, take);
    m_offset += take;
    return streams::ReadResult{take, {}};
  }

  /// 底层 `Read` 被调用的次数。
  std::size_t calls() const noexcept { return m_calls; }

 private:
  std::string m_data;
  std::size_t m_offset{0};
  std::size_t m_max_chunk{0};
  std::size_t m_calls{0};
};

/// 把写入的字节追加到内部的 Writer 桩；可限制单次写入字节数模拟短写。
class StringSink {
 public:
  explicit StringSink(std::size_t max_chunk = 0) : m_max_chunk(max_chunk) {}

  streams::WriteResult Write(const char* data, std::size_t size) {
    ++m_calls;
    std::size_t take = size;
    if (m_max_chunk != 0) {
      take = take < m_max_chunk ? take : m_max_chunk;
    }
    m_data.append(data, take);
    return streams::WriteResult{take, {}};
  }

  const std::string& data() const noexcept { return m_data; }
  std::string& data() noexcept { return m_data; }
  std::size_t calls() const noexcept { return m_calls; }
  void clear() {
    m_data.clear();
    m_calls = 0;
  }

 private:
  std::string m_data;
  std::size_t m_max_chunk{0};
  std::size_t m_calls{0};
};

/// 「数据 + EOF 同一次返回」的 Reader 桩（Go `io.Reader` 允许的边界情况）。
class DataAndEofSource {
 public:
  explicit DataAndEofSource(std::string data) : m_data(std::move(data)) {}

  streams::ReadResult Read(char* buffer, std::size_t size) {
    if (m_delivered) {
      return streams::ReadResult{0, streams::IoEof()};
    }
    m_delivered = true;
    std::size_t take = m_data.size() < size ? m_data.size() : size;
    std::memcpy(buffer, m_data.data(), take);
    return streams::ReadResult{take, streams::IoEof()};
  }

 private:
  std::string m_data;
  bool m_delivered{false};
};

/// 永远返回 `(0, nil)` 的 Reader 桩：引擎必须在 100 次后报 `IoErrNoProgress`。
class IdleSource {
 public:
  streams::ReadResult Read(char*, std::size_t) {
    ++m_calls;
    return streams::ReadResult{0, {}};
  }
  std::size_t calls() const noexcept { return m_calls; }

 private:
  std::size_t m_calls{0};
};

/// 立刻返回指定错误的 Reader 桩。
class FailingSource {
 public:
  explicit FailingSource(ErrorPtr error) : m_error(std::move(error)) {}

  streams::ReadResult Read(char*, std::size_t) {
    return streams::ReadResult{0, m_error};
  }

 private:
  ErrorPtr m_error;
};

/// 立刻返回指定错误的 Writer 桩。
class FailingSink {
 public:
  explicit FailingSink(ErrorPtr error) : m_error(std::move(error)) {}

  streams::WriteResult Write(const char*, std::size_t) {
    return streams::WriteResult{0, m_error};
  }

 private:
  ErrorPtr m_error;
};

// ============================================================================
// 数据生成器
// ============================================================================

/// 低熵日志文本：大量重复的固定短语 + 变化的行号/时间，接近真实日志的压缩特性。
std::string MakeLogText(std::size_t size, std::uint64_t seed);

/// 高熵随机字节（`std::mt19937_64`，不可压缩）。
std::string MakeRandomBytes(std::size_t size, std::uint64_t seed);

/// 全零（最极端的低熵输入）。
std::string MakeZeros(std::size_t size);

/// 可打印 ASCII 随机文本（熵介于日志与随机字节之间）。
std::string MakeRandomText(std::size_t size, std::uint64_t seed);

// ============================================================================
// 独立校验和实现（不依赖 zlib）
// ============================================================================

/// IEEE CRC32（gzip 多边形 0xEDB88320，反射实现）。
std::uint32_t Crc32(std::string_view data);

/// zlib adler32。
std::uint32_t Adler32(std::string_view data);

// ============================================================================
// 按格式压缩 / 解压
// ============================================================================

/// 三种 DEFLATE 封装格式。
enum class Format : std::uint8_t {
  kFlate, ///< raw deflate
  kZlib,  ///< zlib 头尾
  kGzip,  ///< gzip 头尾
};

/// 格式名字（打印用）。
const char* FormatName(Format format);

/// 压缩整段数据；失败时返回已产生的部分并写 `*error`。
std::string CompressBytes(Format format, const std::string& input, int level,
                          ErrorPtr* error);

/**
 * @brief 解压整段数据。
 * @param source_chunk 底层源单次最多给多少字节（0 = 一次给完）。
 * @param dest_chunk 每次 `Read` 的目标缓冲大小。
 * @param error 解压错误；`IoEof` 视为正常结束，写空错误。
 */
std::string DecompressBytes(Format format, const std::string& input,
                            ErrorPtr* error, std::size_t source_chunk = 0,
                            std::size_t dest_chunk = 64U * 1024U);

/**
 * @brief 从任意 Reader 抽干数据直到 EOF。
 * @param reader 需要提供 `Read(ByteSpan) -> size_t` 与 `Err()`。
 * @param destination_chunk 每次读的目标缓冲大小。
 * @param error 非 EOF 错误；正常结束写空。
 */
template <typename ReaderT>
std::string DrainReader(ReaderT& reader, std::size_t destination_chunk,
                        ErrorPtr* error) {
  if (error) {
    *error = {};
  }
  std::string output;
  std::vector<std::uint8_t> buffer(destination_chunk == 0 ? 4096
                                                          : destination_chunk);
  int idle = 0;
  for (;;) {
    const std::size_t count = reader.Read(ByteSpan(buffer.data(), buffer.size()));
    if (count > 0) {
      output.append(reinterpret_cast<const char*>(buffer.data()), count);
      idle = 0;
    }
    const ErrorPtr& current = reader.Err();
    if (current) {
      if (go2cpp::Is(current, streams::IoEof())) {
        if (error) {
          *error = {};
        }
      } else if (error) {
        *error = current;
      }
      return output;
    }
    if (count == 0 && ++idle >= 100) {
      if (error) {
        *error = streams::IoErrNoProgress();
      }
      return output;
    }
  }
}

/**
 * @brief 把整段数据写进 Writer。
 * @return 是否全部被接受且没有错误。
 */
template <typename WriterT>
bool WriteAll(WriterT& writer, const std::string& data) {
  const std::size_t accepted = writer.Write(ConstBytes(data));
  return accepted == data.size() && !writer.Err();
}

// ============================================================================
// 文件 / 子进程工具（互操作与真实场景用）
// ============================================================================

/// 临时目录：构造时 `mkdtemp`，析构时递归删除（测试不留残留）。
class TempDir {
 public:
  TempDir();
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  /// 创建是否成功。
  bool ok() const noexcept { return !m_path.empty(); }

  /// 目录绝对路径（末尾无 `/`）。
  const std::string& path() const noexcept { return m_path; }

  /// 目录内的文件路径。
  std::string File(std::string_view name) const {
    return m_path + "/" + std::string(name);
  }

 private:
  std::string m_path;
};

/// 二进制写文件；成功返回 true。
bool WriteFile(const std::string& path, std::string_view data);

/// 二进制读整个文件；打不开返回空串（用 `error` 区分空文件与失败）。
std::string ReadFile(const std::string& path, bool* ok = nullptr);

/// 执行 shell 命令，返回退出码（无法执行返回 -1）。
int RunCommand(const std::string& command);

/// 命令是否存在（用 `shell -c "command -v"` 语义探测）。
bool CommandExists(const std::string& name);

}  // namespace go2cpp_compress_test
