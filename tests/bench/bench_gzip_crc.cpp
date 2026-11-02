// gzip 解压吞吐基准（1 MiB L6，对照组：系统 zlib）+ CRC-32 正确性/吞吐对拍。
//
// 只做「解压」：压缩产物在计时外生成，解压循环写进预分配缓冲，调用方侧
// 没有 append/分配开销。两种输出块大小都测：
//   - chunk=64 KiB：与既有 `DecompressBytes` 默认值一致（历史数字可比）；
//   - chunk=1 MiB ：整块输出，衡量批量 CRC / 拷贝的收益上限。
//
// 机器上可能有其它负载，因此 go2cpp / zlib 采用**同轮交替**测量（偶数轮
// 先 go 后 zlib、奇数轮反过来），再取逐轮比值的中位数，把频率漂移抵消掉。
//
// CRC 对拍直接调 `go2cpp::hash::Crc32`（compress 的解压路径用的就是它），
// 与 zlib 的 crc32() 比较，避免「自己证明自己」。
//
// 手工编译（不进 CMake，避免改构建清单）：
//   g++ -O3 -DNDEBUG -std=c++17 -Iinclude tests/bench/bench_gzip_crc.cpp
//       -L/tmp/build-gzip -lgo2cpp_compress -lgo2cpp_hash -lgo2cpp_error -lz
//       -Wl,-rpath,/tmp/build-gzip -o /tmp/bench_gzip_crc
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "go2cpp/compress/compress.hpp"
#include "go2cpp/hash/crc32.hpp"

namespace {

using Clock = std::chrono::steady_clock;
namespace compress = go2cpp::compress;
namespace streams = go2cpp::streams;
namespace hash = go2cpp::hash;

constexpr std::size_t kInputBytes = 1024U * 1024U;
constexpr std::size_t kChunk = 64U * 1024U;

/// 固定语料：低熵日志行（与 bench_tools_industry 同一份，数字可比）。
std::string MakeLogText(std::size_t bytes) {
  static const char* kLines[] = {
      "2026-11-02T09:12:33.120Z INFO  scheduler fiber=1024 runq=3 latency_us=42 event=timer\n",
      "2026-11-02T09:12:33.121Z WARN  network peer=10.2.3.4:8080 retry=2 reason=timeout\n",
      "2026-11-02T09:12:33.122Z DEBUG reflect type=PlayerState bytes=61 fields=6 codec=binary\n",
      "2026-11-02T09:12:33.123Z ERROR storage leveldb status=ok compact_ms=17 files=8\n",
  };
  std::string out;
  out.reserve(bytes + 256);
  std::uint32_t state = 12345;
  while (out.size() < bytes) {
    state = state * 1664525u + 1013904223u;
    out.append(kLines[(state >> 16) % 4]);
  }
  out.resize(bytes);
  return out;
}

std::string MakeRandom(std::size_t bytes, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::string out(bytes, '\0');
  std::size_t offset = 0;
  while (offset < bytes) {
    const std::uint64_t value = rng();
    const std::size_t take = std::min<std::size_t>(bytes - offset, sizeof(value));
    std::memcpy(out.data() + offset, &value, take);
    offset += take;
  }
  return out;
}

class StringSource {
 public:
  explicit StringSource(const std::string& data) : m_data(data) {}
  streams::ReadResult Read(char* buffer, std::size_t size) {
    const std::size_t remaining = m_data.size() - m_offset;
    const std::size_t take = std::min(size, remaining);
    std::memcpy(buffer, m_data.data() + m_offset, take);
    m_offset += take;
    return streams::ReadResult{take, {}};
  }

 private:
  const std::string& m_data;
  std::size_t m_offset{0};
};

class StringSink {
 public:
  streams::WriteResult Write(const char* data, std::size_t size) {
    m_data.append(data, size);
    return streams::WriteResult{size, {}};
  }
  std::string& data() { return m_data; }

 private:
  std::string m_data;
};

/// go2cpp gzip 压缩。
std::string GzipCompress(const std::string& input, int level) {
  StringSink sink;
  compress::Gzip::Writer<StringSink> writer(sink, level);
  const std::size_t accepted = writer.Write(compress::ByteSpan(
      reinterpret_cast<std::uint8_t*>(const_cast<char*>(input.data())),
      input.size()));
  if (accepted != input.size() || writer.Close()) {
    std::printf("go2cpp gzip compress failed\n");
    std::abort();
  }
  return sink.data();
}

/// go2cpp gzip 解压，输出按 `chunk` 分块直接写进预分配缓冲。
std::size_t GzipDecompress(const std::string& input, std::string* output,
                           std::size_t chunk) {
  StringSource source(input);
  compress::Gzip::Reader<StringSource> reader(source);
  std::size_t total = 0;
  for (;;) {
    const std::size_t take = std::min(chunk, output->size() - total);
    if (take == 0) {
      break;
    }
    const std::size_t count = reader.Read(compress::ByteSpan(
        reinterpret_cast<std::uint8_t*>(output->data()) + total, take));
    total += count;
    if (reader.Err()) {
      break;
    }
    if (count == 0) {
      break;
    }
  }
  return total;
}

std::string ZlibCompress(const std::string& input, int level) {
  z_stream stream{};
  if (deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    std::abort();
  }
  std::string out;
  out.resize(deflateBound(&stream, static_cast<uLong>(input.size())) + 64);
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  const int rc = deflate(&stream, Z_FINISH);
  const std::size_t produced = out.size() - stream.avail_out;
  deflateEnd(&stream);
  if (rc != Z_STREAM_END) {
    std::abort();
  }
  out.resize(produced);
  return out;
}

std::size_t ZlibDecompress(const std::string& input, std::string* output,
                           std::size_t chunk) {
  z_stream stream{};
  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    std::abort();
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  std::size_t total = 0;
  for (;;) {
    const std::size_t take = std::min(chunk, output->size() - total);
    if (take == 0) {
      break;
    }
    stream.next_out = reinterpret_cast<Bytef*>(output->data() + total);
    stream.avail_out = static_cast<uInt>(take);
    const int rc = inflate(&stream, Z_NO_FLUSH);
    total += take - stream.avail_out;
    if (rc == Z_STREAM_END) {
      break;
    }
    if (rc != Z_OK && rc != Z_BUF_ERROR) {
      std::abort();
    }
    if (stream.avail_out == take) {
      break;
    }
  }
  inflateEnd(&stream);
  return total;
}

double Median(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

/// 同轮交替测量：返回 go2cpp / zlib 的 MiB/s 中位数与逐轮加速比中位数。
struct PairStats {
  double go_median{0.0};
  double zlib_median{0.0};
  double speedup_median{0.0};  ///< go2cpp 相对 zlib 的逐轮加速比（>1 表示更快）
};

template <typename GoFn, typename ZlibFn>
PairStats MeasurePair(GoFn&& go, ZlibFn&& zlib, std::size_t decoded_bytes,
                      int iterations) {
  const double mib = static_cast<double>(decoded_bytes) / (1024.0 * 1024.0);
  for (int warmup = 0; warmup < std::max(1, iterations / 5); ++warmup) {
    go();
    zlib();
  }
  std::vector<double> go_samples;
  std::vector<double> zlib_samples;
  std::vector<double> speedups;
  go_samples.reserve(static_cast<std::size_t>(iterations));
  zlib_samples.reserve(static_cast<std::size_t>(iterations));
  speedups.reserve(static_cast<std::size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    double go_seconds = 0.0;
    double zlib_seconds = 0.0;
    const bool go_first = (i % 2) == 0;
    if (go_first) {
      Clock::time_point start = Clock::now();
      go();
      go_seconds = std::chrono::duration<double>(Clock::now() - start).count();
      start = Clock::now();
      zlib();
      zlib_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    } else {
      Clock::time_point start = Clock::now();
      zlib();
      zlib_seconds = std::chrono::duration<double>(Clock::now() - start).count();
      start = Clock::now();
      go();
      go_seconds = std::chrono::duration<double>(Clock::now() - start).count();
    }
    go_samples.push_back(mib / go_seconds);
    zlib_samples.push_back(mib / zlib_seconds);
    speedups.push_back(zlib_seconds / go_seconds);
  }
  PairStats stats;
  stats.go_median = Median(go_samples);
  stats.zlib_median = Median(zlib_samples);
  stats.speedup_median = Median(speedups);
  return stats;
}

void ReportPair(const char* name, const PairStats& stats) {
  std::printf(
      "%-44s go2cpp %8.1f MiB/s   zlib %8.1f MiB/s   "
      "speedup %.3fx (time %.3fx)\n",
      name, stats.go_median, stats.zlib_median, stats.speedup_median,
      1.0 / stats.speedup_median);
  std::fflush(stdout);
}

std::uint32_t Crc32Span(std::uint32_t seed, const std::string& data,
                        std::size_t size) {
  return hash::Crc32::Update(
      seed, hash::IEEETable,
      hash::AsBytes(reinterpret_cast<const std::uint8_t*>(data.data()), size));
}

/// CRC-32（IEEE）正确性：与 zlib 的 crc32() 全量对拍。
bool VerifyCrc32() {
  const std::string data = MakeRandom(1000000, 9001);
  const std::uint32_t expected =
      static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(data.data()),
                                       static_cast<uInt>(data.size())));
  const std::uint32_t actual = Crc32Span(0, data, data.size());
  std::printf("crc32 1e6 random: expected=%08x actual=%08x %s\n", expected,
              actual, expected == actual ? "OK" : "MISMATCH");
  if (expected != actual) {
    return false;
  }
  // 任意分段串接：随机切段逐段累积，必须等于整段。
  std::uint32_t streamed = 0;
  std::mt19937_64 rng(77);
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t take = 1 + static_cast<std::size_t>(rng() % 4096);
    const std::size_t n = std::min(take, data.size() - offset);
    streamed = hash::Crc32::Update(
        streamed, hash::IEEETable,
        hash::AsBytes(reinterpret_cast<const std::uint8_t*>(data.data()) + offset,
                      n));
    offset += n;
  }
  if (streamed != expected) {
    std::printf("crc32 segmented MISMATCH: %08x vs %08x\n", streamed, expected);
    return false;
  }
  // 长度边界：0..300 覆盖折叠门槛 64B 与 16B 尾部，外加 4 KiB/64 KiB/1 MiB。
  std::vector<std::size_t> lengths;
  for (std::size_t n = 0; n <= 300; ++n) {
    lengths.push_back(n);
  }
  for (const std::size_t n : {4095U, 4096U, 4097U, 65535U, 65536U, 65537U,
                              1024U * 1024U}) {
    lengths.push_back(n);
  }
  const std::string pool = MakeRandom(1024U * 1024U, 9002);
  for (const std::size_t n : lengths) {
    const std::uint32_t want = static_cast<std::uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(pool.data()),
              static_cast<uInt>(n)));
    const std::uint32_t got = Crc32Span(0, pool, n);
    if (want != got) {
      std::printf("crc32 length %zu MISMATCH: %08x vs %08x\n", n, want, got);
      return false;
    }
    // 非零初值同样要对：初值 crc 表示「前面已有一段」。
    const std::uint32_t want2 = static_cast<std::uint32_t>(
        crc32(0x12345678U, reinterpret_cast<const Bytef*>(pool.data()),
              static_cast<uInt>(n)));
    const std::uint32_t got2 = Crc32Span(0x12345678U, pool, n);
    if (want2 != got2) {
      std::printf("crc32 seeded length %zu MISMATCH: %08x vs %08x\n", n, want2,
                  got2);
      return false;
    }
  }
  std::printf("crc32 lengths 0..300 + 4095..1MiB (seed 0 / 0x12345678): OK\n");
  return true;
}

/// CRC 单函数吞吐（1 MiB 随机，走 hash 模块的 PCLMULQDQ 路径）。
void BenchmarkCrc(const std::string& data) {
  const int iterations = 400;
  std::vector<double> samples;
  samples.reserve(iterations);
  for (int i = 0; i < 20; ++i) {
    (void)Crc32Span(0, data, data.size());
  }
  for (int i = 0; i < iterations; ++i) {
    Clock::time_point start = Clock::now();
    volatile std::uint32_t sink = Crc32Span(0, data, data.size());
    (void)sink;
    const double seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    samples.push_back(static_cast<double>(data.size()) / seconds / 1e9);
  }
  std::printf("crc32 1MiB (hash::Crc32/PCLMUL): %6.2f GB/s\n", Median(samples));
}

}  // namespace

int main() {
  if (!VerifyCrc32()) {
    return 1;
  }
  const std::string text = MakeLogText(kInputBytes);
  const std::string packed = GzipCompress(text, 6);
  const std::string zpacked = ZlibCompress(text, 6);
  std::string output(kInputBytes + kChunk, '\0');

  std::printf("input=%zu B  go2cpp_ratio=%.4f  zlib_ratio=%.4f\n", text.size(),
              static_cast<double>(packed.size()) / static_cast<double>(text.size()),
              static_cast<double>(zpacked.size()) / static_cast<double>(text.size()));

  const int iterations = 200;
  for (const std::string* stream : {&packed, &zpacked}) {
    const std::size_t got = GzipDecompress(*stream, &output, kChunk);
    if (got != text.size() ||
        std::memcmp(output.data(), text.data(), text.size()) != 0) {
      std::printf("go2cpp decompress mismatch\n");
      return 1;
    }
  }
  {
    const std::size_t got = ZlibDecompress(zpacked, &output, kChunk);
    if (got != text.size() ||
        std::memcmp(output.data(), text.data(), text.size()) != 0) {
      std::printf("zlib decompress mismatch\n");
      return 1;
    }
  }

  const PairStats own64 = MeasurePair(
      [&] { GzipDecompress(packed, &output, kChunk); },
      [&] { ZlibDecompress(zpacked, &output, kChunk); }, text.size(),
      iterations);
  const PairStats same64 = MeasurePair(
      [&] { GzipDecompress(zpacked, &output, kChunk); },
      [&] { ZlibDecompress(zpacked, &output, kChunk); }, text.size(),
      iterations);
  const PairStats same1m = MeasurePair(
      [&] { GzipDecompress(zpacked, &output, kInputBytes); },
      [&] { ZlibDecompress(zpacked, &output, kInputBytes); }, text.size(),
      iterations);

  ReportPair("gzip_decompress 64K (go2cpp stream)", own64);
  ReportPair("gzip_decompress 64K (same zlib stream)", same64);
  ReportPair("gzip_decompress 1M  (same zlib stream)", same1m);

  // 压缩侧回归检查（FindMatch 的越界读修复不应影响吞吐）。
  {
    const int compress_iterations = 100;
    for (int i = 0; i < 10; ++i) {
      (void)GzipCompress(text, 6);
    }
    std::vector<double> samples;
    samples.reserve(compress_iterations);
    for (int i = 0; i < compress_iterations; ++i) {
      const Clock::time_point start = Clock::now();
      const std::string compressed = GzipCompress(text, 6);
      const double seconds =
          std::chrono::duration<double>(Clock::now() - start).count();
      samples.push_back(1.0 / seconds);
      if (compressed.empty()) {
        std::abort();
      }
    }
    std::printf("gzip_compress L6 1MiB: %.1f MiB/s\n", Median(samples));
  }

  BenchmarkCrc(MakeRandom(kInputBytes, 9003));
  return 0;
}
