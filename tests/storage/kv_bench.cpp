// Copyright 2026 Go2Cpp Authors. All rights reserved.
// KV 基准：1e7 压测 + 对照（unordered_map / MemTable）+ 实测下限
// （随机访存延迟 / fdatasync）+ 多线程与 group commit。
//
// 用法：
//   ./go2cpp_kv_bench                 # 1e7
//   ./go2cpp_kv_bench --n=1000000     # 缩小规模（CI 冒烟）
//   ./go2cpp_kv_bench --skip-disk     # 只跑内存侧

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "go2cpp/storage/kv.hpp"
#include "memtable.hpp"

using go2cpp::storage::KV;
using go2cpp::storage::MemTable;
using go2cpp::storage::Options;

namespace {

using Clock = std::chrono::steady_clock;

double NowNs() {
  return std::chrono::duration<double, std::nano>(
             Clock::now().time_since_epoch())
      .count();
}

struct XorShift {
  std::uint64_t s;
  explicit XorShift(std::uint64_t seed) : s(seed ? seed : 88172645463325252ull) {}
  std::uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

int MakeKey(char* buf, std::uint64_t i) {
  return std::snprintf(buf, 32, "key-%016llu",
                       static_cast<unsigned long long>(i));
}

int MakeValue(char* buf, std::uint64_t i) {
  return std::snprintf(buf, 32, "value-%016llu",
                       static_cast<unsigned long long>(i));
}

struct BenchResult {
  std::string name;
  double ns_per_op = 0;
  double ops_per_sec = 0;
  std::uint64_t n = 0;
};

std::vector<BenchResult> g_results;

void Report(const std::string& name, std::uint64_t n, double elapsed_ns) {
  BenchResult r;
  r.name = name;
  r.n = n;
  r.ns_per_op = elapsed_ns / static_cast<double>(n);
  r.ops_per_sec = 1e9 / r.ns_per_op;
  g_results.push_back(r);
  std::printf("BENCH %-42s n=%-10llu %10.2f ns/op  %12.0f ops/s\n",
              name.c_str(), static_cast<unsigned long long>(n),
              r.ns_per_op, r.ops_per_sec);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 实测下限
// ---------------------------------------------------------------------------

void BenchRandomAccess(std::uint64_t n) {
  const std::size_t kBytes = 64ull << 20;  // 64MiB > L3(32MiB)
  const std::size_t kSlots = kBytes / sizeof(std::uint32_t);
  std::vector<std::uint32_t> perm(kSlots);
  for (std::size_t i = 0; i < kSlots; ++i) {
    perm[i] = static_cast<std::uint32_t>(i);
  }
  XorShift rng(12345);
  for (std::size_t i = kSlots - 1; i > 0; --i) {
    const std::size_t j = rng.Next() % (i + 1);
    std::swap(perm[i], perm[j]);
  }
  std::vector<std::uint32_t> ring(kSlots);
  for (std::size_t i = 0; i < kSlots; ++i) {
    ring[perm[i]] = perm[(i + 1) % kSlots];
  }
  std::uint32_t idx = 0;
  std::uint64_t sink = 0;
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    idx = ring[idx];
    sink += idx;
  }
  const double t1 = NowNs();
  if (sink == 0xffffffffu) {
    std::printf(" ");
  }
  Report("lower-bound: 64MiB 随机访存(依赖链)", n, t1 - t0);
}

void BenchFsync(std::uint64_t n) {
  const char* path = "/tmp/go2cpp_kv_fsync.tmp";
  const int fd = ::open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (fd < 0) {
    std::printf("BENCH fsync: open failed\n");
    return;
  }
  char buf[4096];
  std::memset(buf, 0xab, sizeof(buf));
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(n));
  double total = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    if (::pwrite(fd, buf, sizeof(buf), 0) != static_cast<ssize_t>(sizeof(buf))) {
      break;
    }
    const double t0 = NowNs();
    if (::fdatasync(fd) != 0) {
      break;
    }
    const double dt = NowNs() - t0;
    samples.push_back(dt);
    total += dt;
  }
  ::close(fd);
  ::unlink(path);
  if (samples.empty()) {
    return;
  }
  std::sort(samples.begin(), samples.end());
  const double median = samples[samples.size() / 2];
  std::printf(
      "BENCH lower-bound: fdatasync(4KiB)            n=%-10zu median=%.1f us  "
      "mean=%.1f us  min=%.1f us\n",
      samples.size(), median / 1000.0,
      total / static_cast<double>(samples.size()) / 1000.0,
      samples.front() / 1000.0);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 对照实现
// ---------------------------------------------------------------------------

void BenchUnorderedMap(std::uint64_t n) {
  std::unordered_map<std::string, std::string> map;
  map.reserve(static_cast<std::size_t>(n) * 2);
  char kbuf[32];
  char vbuf[32];
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int kl = MakeKey(kbuf, i);
    const int vl = MakeValue(vbuf, i);
    map.emplace(std::string(kbuf, kl), std::string(vbuf, vl));
  }
  const double t1 = NowNs();
  Report("baseline: unordered_map insert", n, t1 - t0);

  std::vector<std::uint32_t> order(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    order[i] = static_cast<std::uint32_t>(i);
  }
  XorShift rng(777);
  for (std::size_t i = order.size(); i > 1; --i) {
    const std::size_t j = rng.Next() % i;
    std::swap(order[i - 1], order[j]);
  }
  std::string scratch;
  std::uint64_t sink = 0;
  const double t2 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int kl = MakeKey(kbuf, order[i]);
    auto it = map.find(std::string(kbuf, kl));
    if (it != map.end()) {
      sink += it->second.size();
    }
  }
  const double t3 = NowNs();
  if (sink == 0) {
    std::printf(" ");
  }
  Report("baseline: unordered_map find(随机)", n, t3 - t2);
}

void BenchMemTable(std::uint64_t n) {
  Options opt;
  opt.memtable_size = static_cast<std::size_t>(n) * 96 + (64ull << 20);
  MemTable table(opt);
  char kbuf[32];
  char vbuf[32];
  std::uint64_t inserted = 0;
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int kl = MakeKey(kbuf, i);
    const int vl = MakeValue(vbuf, i);
    if (!table.Put(std::string_view(kbuf, kl), std::string_view(vbuf, vl))) {
      break;
    }
    ++inserted;
  }
  const double t1 = NowNs();
  if (inserted > 0) {
    Report("MemTable: Put(顺序)", inserted, t1 - t0);
  }

  std::vector<std::uint32_t> order(static_cast<std::size_t>(inserted));
  for (std::uint64_t i = 0; i < inserted; ++i) {
    order[i] = static_cast<std::uint32_t>(i);
  }
  XorShift rng(4242);
  for (std::size_t i = order.size(); i > 1; --i) {
    const std::size_t j = rng.Next() % i;
    std::swap(order[i - 1], order[j]);
  }
  std::string value;
  value.reserve(64);
  std::uint64_t hits = 0;
  const double t2 = NowNs();
  for (std::uint64_t i = 0; i < inserted; ++i) {
    const int kl = MakeKey(kbuf, order[i]);
    if (table.Get(std::string_view(kbuf, kl), &value)) {
      ++hits;
    }
  }
  const double t3 = NowNs();
  if (hits != inserted) {
    std::printf("  [warn] MemTable hits=%llu/%llu\n",
                static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(inserted));
  }
  Report("MemTable: Get(随机)", inserted, t3 - t2);
}

// ---------------------------------------------------------------------------
// KV 端到端
// ---------------------------------------------------------------------------

void BenchKv(const std::string& dir, const char* label, std::uint64_t n,
             bool disk) {
  std::filesystem::remove_all(dir);
  Options opt;
  opt.memtable_size = 32ull << 20;
  opt.max_sstables = 4;
  opt.sstables_per_compaction = 2;
  opt.sync_writes = false;
  auto db = KV::Open(dir, opt);
  if (!db) {
    std::printf("BENCH %s: open failed\n", label);
    return;
  }
  char kbuf[32];
  char vbuf[32];
  std::uint64_t put_errors = 0;
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int kl = MakeKey(kbuf, i);
    const int vl = MakeValue(vbuf, i);
    if (!db->Put(std::string_view(kbuf, kl), std::string_view(vbuf, vl))) {
      ++put_errors;
    }
  }
  const double t1 = NowNs();
  if (put_errors != 0) {
    std::printf("  [warn] put_errors=%llu last_error=%s\n",
                static_cast<unsigned long long>(put_errors),
                db->LastError().c_str());
  }
  Report(std::string(label) + ": Put(顺序)", n, t1 - t0);

  std::vector<std::uint32_t> order(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    order[i] = static_cast<std::uint32_t>(i);
  }
  XorShift rng(999);
  for (std::size_t i = order.size(); i > 1; --i) {
    const std::size_t j = rng.Next() % i;
    std::swap(order[i - 1], order[j]);
  }
  std::string value;
  value.reserve(64);
  std::uint64_t hits = 0;
  const double t2 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int kl = MakeKey(kbuf, order[i]);
    if (db->Get(std::string_view(kbuf, kl), &value)) {
      ++hits;
    }
  }
  const double t3 = NowNs();
  std::printf("  %s hits=%llu/%llu\n", label,
              static_cast<unsigned long long>(hits),
              static_cast<unsigned long long>(n));
  Report(std::string(label) + ": Get(随机,命中)", n, t3 - t2);
  std::printf("  %s stats: %s\n", label, db->StatsString().c_str());
  if (hits != n) {
    std::printf("  [warn] hits=%llu/%llu\n",
                static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(n));
  }
  const double t4 = NowNs();
  db->Close();
  const double t5 = NowNs();
  Report(std::string(label) + ": Close(含最终刷盘)", n, t5 - t4);
  std::filesystem::remove_all(dir);
  (void)disk;
}

void BenchSyncWrites(const std::string& dir, std::uint64_t n) {
  std::filesystem::remove_all(dir);
  Options opt;
  opt.sync_writes = true;
  opt.memtable_size = 16ull << 20;
  auto db = KV::Open(dir, opt);
  if (!db) {
    return;
  }
  char kbuf[32];
  char vbuf[32];
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const int kl = MakeKey(kbuf, i);
    const int vl = MakeValue(vbuf, i);
    db->Put(std::string_view(kbuf, kl), std::string_view(vbuf, vl));
  }
  const double t1 = NowNs();
  Report("KV(sync=true): Put 单线程", n, t1 - t0);
  db->Close();
  std::filesystem::remove_all(dir);
}

void BenchSyncWritesThreads(const std::string& dir, int threads,
                            std::uint64_t per_thread) {
  std::filesystem::remove_all(dir);
  Options opt;
  opt.sync_writes = true;
  opt.memtable_size = 16ull << 20;
  auto db = KV::Open(dir, opt);
  if (!db) {
    return;
  }
  std::vector<std::thread> pool;
  const double t0 = NowNs();
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      char kbuf[32];
      char vbuf[32];
      for (std::uint64_t i = 0; i < per_thread; ++i) {
        const std::uint64_t id = static_cast<std::uint64_t>(t) * per_thread + i;
        const int kl = MakeKey(kbuf, id);
        const int vl = MakeValue(vbuf, id);
        db->Put(std::string_view(kbuf, kl), std::string_view(vbuf, vl));
      }
    });
  }
  for (auto& th : pool) {
    th.join();
  }
  const double t1 = NowNs();
  const std::uint64_t total = static_cast<std::uint64_t>(threads) * per_thread;
  Report("KV(sync=true): Put " + std::to_string(threads) + " 线程(group commit)",
         total, t1 - t0);
  db->Close();
  std::filesystem::remove_all(dir);
}

void BenchMultiThreadGet(const std::string& dir, std::uint64_t n, int threads,
                         std::uint64_t per_thread) {
  std::filesystem::remove_all(dir);
  Options opt;
  opt.memtable_size = 64ull << 20;
  opt.sstables_per_compaction = 2;
  opt.max_sstables = 4;
  auto db = KV::Open(dir, opt);
  if (!db) {
    return;
  }
  char kbuf[32];
  char vbuf[32];
  for (std::uint64_t i = 0; i < n; ++i) {
    db->Put(std::string_view(kbuf, MakeKey(kbuf, i)),
            std::string_view(vbuf, MakeValue(vbuf, i)));
  }
  std::atomic<std::uint64_t> hits{0};
  std::atomic<std::uint64_t> errors{0};
  std::vector<std::thread> pool;
  const double t0 = NowNs();
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      XorShift rng(1000 + t);
      std::string value;
      value.reserve(64);
      std::uint64_t local = 0;
      for (std::uint64_t i = 0; i < per_thread; ++i) {
        const std::uint64_t id = rng.Next() % n;
        const int kl = MakeKey(kbuf, id);
        if (db->Get(std::string_view(kbuf, kl), &value)) {
          ++local;
        }
      }
      hits.fetch_add(local);
    });
  }
  for (auto& th : pool) {
    th.join();
  }
  const double t1 = NowNs();
  if (errors.load() != 0) {
    std::printf("  [warn] errors\n");
  }
  Report("KV: Get " + std::to_string(threads) + " 线程(随机)",
         static_cast<std::uint64_t>(threads) * per_thread, t1 - t0);
  db->Close();
  std::filesystem::remove_all(dir);
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t n = 10000000;
  bool skip_disk = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind("--n=", 0) == 0) {
      n = std::strtoull(arg.c_str() + 4, nullptr, 10);
    } else if (arg == "--skip-disk") {
      skip_disk = true;
    }
  }
  std::printf("KV benchmark: n=%llu\n",
              static_cast<unsigned long long>(n));
  std::printf("CPU: ");
  std::fflush(stdout);
  if (std::system("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2") != 0) {
    std::printf("(unknown)\n");
  }

  BenchRandomAccess(n);
  BenchFsync(200);
  BenchUnorderedMap(n < 1000000 ? n : 1000000);
  BenchMemTable(n);
  BenchKv("/dev/shm/go2cpp_kv_bench_mem", "KV(tmpfs)", n, false);
  if (!skip_disk) {
    BenchKv("/tmp/go2cpp_kv_bench_disk", "KV(ext4)", n, true);
  }
  BenchSyncWrites("/tmp/go2cpp_kv_bench_sync", 500);
  BenchSyncWritesThreads("/tmp/go2cpp_kv_bench_sync8", 8, 100);
  BenchMultiThreadGet("/tmp/go2cpp_kv_bench_mt", n < 2000000 ? n : 2000000, 8,
                      250000);

  std::printf("\n===== 汇总 =====\n");
  for (const auto& r : g_results) {
    std::printf("%-44s %10.2f ns/op %12.0f ops/s\n", r.name.c_str(),
                r.ns_per_op, r.ops_per_sec);
  }
  return 0;
}
