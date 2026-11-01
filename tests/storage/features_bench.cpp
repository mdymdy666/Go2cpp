// Copyright 2026 Go2Cpp Authors. All rights reserved.
// 功能层基准：Queue ops/s、Watch 回调开销、MemCache Get/Set ops/s + 实测下限。
//
// 用法：
//   ./go2cpp_storage_features_bench              # 1e7
//   ./go2cpp_storage_features_bench --n=1000000  # 缩小规模（ctest 冒烟）
//   ./go2cpp_storage_features_bench --skip-disk  # 只跑内存侧
//
// 每个场景用独立的 KV 目录，避免前一个场景把 LSM 写满后污染下一个场景。

#include "go2cpp/storage/kv.hpp"
#include "go2cpp/storage/memcache.hpp"
#include "go2cpp/storage/queue.hpp"
#include "go2cpp/cache/lru.hpp"
#include "go2cpp/storage/watch.hpp"
#include "seglog.hpp"  // 内部头：WAL 顺序写下限对照

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using go2cpp::storage::KV;
using go2cpp::storage::MemCache;
using go2cpp::storage::MemCacheOptions;
using go2cpp::storage::Queue;
using go2cpp::storage::QueueMessage;
using go2cpp::storage::QueueOptions;
using go2cpp::storage::SegLog;
using go2cpp::storage::SegLogOptions;
using go2cpp::storage::Watcher;
using go2cpp::storage::WatchOptions;

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

struct BenchResult {
  std::string name;
  std::uint64_t n = 0;
  double ns_per_op = 0;
  double ops_per_sec = 0;
  double mb_per_sec = 0;
};

std::vector<BenchResult> g_results;

void Report(const std::string& name, std::uint64_t n, double elapsed_ns,
            double bytes_per_op = 0) {
  BenchResult r;
  r.name = name;
  r.n = n;
  r.ns_per_op = n == 0 ? 0 : elapsed_ns / static_cast<double>(n);
  r.ops_per_sec = r.ns_per_op == 0 ? 0 : 1e9 / r.ns_per_op;
  r.mb_per_sec = bytes_per_op == 0 ? 0
                                   : r.ops_per_sec * bytes_per_op / (1u << 20);
  g_results.push_back(r);
  std::printf("%-42s %10.1f ns/op %12.0f ops/s %10.1f MB/s\n", name.c_str(),
              r.ns_per_op, r.ops_per_sec, r.mb_per_sec);
  std::fflush(stdout);
}

std::string TempDir(const char* tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/go2cpp_feat_%s_XXXXXX", tag);
  char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

std::string Key16(std::uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key-%016llu",
                static_cast<unsigned long long>(i));
  return std::string(buf);
}

std::string Value16(std::uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "val-%016llu",
                static_cast<unsigned long long>(i));
  return std::string(buf);
}

std::string Message(std::uint64_t i) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "msg-%016llu-payload-abcdefgh",
                static_cast<unsigned long long>(i));
  return std::string(buf);
}

// --------------------------------------------------------------------------
// Queue
// --------------------------------------------------------------------------

void BenchQueueAppend(std::uint64_t n, bool topic_cache) {
  const std::string dir = TempDir("q_append");
  auto kv = KV::Open(dir + "/kv");
  if (kv == nullptr) {
    return;
  }
  QueueOptions opt;
  opt.segment_size = 512u << 20;  // 1e7 × ~40B ≈ 400MB：只滚一次段
  opt.topic_cache = topic_cache;
  auto queue = Queue::Open(kv.get(), dir + "/queue", opt);
  if (queue == nullptr) {
    return;
  }
  const std::string msg = Message(0);
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    queue->Append("bench", msg);
  }
  const double elapsed = NowNs() - t0;
  char aname[96];
  std::snprintf(aname, sizeof(aname), "queue.append(40B, topic_cache=%d)",
                topic_cache ? 1 : 0);
  Report(aname, n, elapsed, static_cast<double>(msg.size()));
  const auto stats = queue->GetStats();
  std::printf("      segments=%llu retained=%llu\n",
              static_cast<unsigned long long>(stats.segments),
              static_cast<unsigned long long>(stats.retained));
  queue.reset();
  kv->Close();
}

void BenchQueueConsume(std::uint64_t n) {
  const std::string dir = TempDir("q_consume");
  auto kv = KV::Open(dir + "/kv");
  if (kv == nullptr) {
    return;
  }
  auto queue = Queue::Open(kv.get(), dir + "/queue");
  if (queue == nullptr) {
    return;
  }
  const std::string msg = Message(0);
  for (std::uint64_t i = 0; i < n; ++i) {
    queue->Append("bench", msg);
  }
  QueueMessage out;
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    if (!queue->Consume("bench", "c1", &out)) {
      break;
    }
  }
  Report("queue.consume(40B)", n, NowNs() - t0,
         static_cast<double>(msg.size()));
  queue->Seek("bench", "c1", 0);
  const double t1 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    if (!queue->Consume("bench", "c1", &out)) {
      break;
    }
    queue->Ack("bench", "c1", out.offset);
  }
  Report("queue.consume+ack(40B)", n, NowNs() - t1,
         static_cast<double>(msg.size()));
  queue.reset();
  kv->Close();
}

void BenchQueueColdReplay(std::uint64_t n) {
  const std::string dir = TempDir("q_cold");
  auto kv = KV::Open(dir + "/kv");
  if (kv == nullptr) {
    return;
  }
  auto queue = Queue::Open(kv.get(), dir + "/queue");
  if (queue == nullptr) {
    return;
  }
  const std::string msg = Message(0);
  for (std::uint64_t i = 0; i < n; ++i) {
    queue->Append("cold", msg);
  }
  queue->Sync();
  QueueMessage out;
  for (std::uint64_t i = 0; i < n; ++i) {
    queue->Consume("cold", "drain", &out);
    queue->Ack("cold", "drain", out.offset);
  }
  // 冷读：消费者从 0 重新开始，内存队列已被回收
  queue->Seek("cold", "replay", 0);
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    if (!queue->Consume("cold", "replay", &out)) {
      break;
    }
    queue->Ack("cold", "replay", out.offset);
  }
  const double elapsed = NowNs() - t0;
  Report("queue.cold_replay(40B)", n, elapsed,
         static_cast<double>(msg.size()));
  const auto stats = queue->GetStats();
  std::printf("      cold_reads=%llu cold_scanned=%llu\n",
              static_cast<unsigned long long>(stats.cold_reads),
              static_cast<unsigned long long>(stats.cold_scanned));
  queue.reset();
  kv->Close();
}

// WAL 顺序写下限：直接往 SegLog 追加同样大小的记录（不含 Queue 的索引/队列）。
void BenchSegLogAppend(std::uint64_t n) {
  const std::string dir = TempDir("seglog");
  SegLog log;
  SegLogOptions opt;
  opt.segment_size = 512u << 20;
  if (!log.Open(dir, opt)) {
    std::printf("seglog open failed\n");
    return;
  }
  const std::string msg = Message(0);
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    log.Append(msg);
  }
  const double elapsed = NowNs() - t0;
  Report("lower.seglog_append(40B)", n, elapsed,
         static_cast<double>(msg.size()));
  std::printf("      produced=%.1f MB segments=%zu\n",
              static_cast<double>(log.bytes()) / (1u << 20),
              log.segment_count());
  log.Close();

  // 4KB 记录：WAL 顺序写的带宽上限（同样走 CRC + 缓冲 + 分块）
  const std::string dir2 = TempDir("seglog_big");
  SegLog big;
  SegLogOptions big_opt;
  big_opt.segment_size = 512u << 20;
  if (!big.Open(dir2, big_opt)) {
    return;
  }
  const std::string rec4k(4096, 'x');
  const std::uint64_t rounds = std::max<std::uint64_t>(n / 256, 20000);
  const double b0 = NowNs();
  for (std::uint64_t i = 0; i < rounds; ++i) {
    big.Append(rec4k);
  }
  const double b1 = NowNs() - b0;
  Report("lower.seglog_append(4KB)", rounds, b1,
         static_cast<double>(rec4k.size()));
  std::printf("      %.2f GB/s WAL 顺序写（4KB 记录）\n",
              static_cast<double>(rounds) * rec4k.size() / b1);
  big.Close();
}

// --------------------------------------------------------------------------
// Watch
// --------------------------------------------------------------------------

void BenchWatch(std::uint64_t n) {
  const std::uint64_t kspace = 1u << 16;
  std::vector<std::string> keys(kspace);
  std::vector<std::string> vals(kspace);
  for (std::uint64_t i = 0; i < kspace; ++i) {
    keys[i] = Key16(i);
    vals[i] = Value16(i);
  }
  // 场景 1：1 个订阅者（前缀 key-），同步回调
  {
    const std::string dir = TempDir("watch_sync");
    auto kv = KV::Open(dir + "/kv");
    if (kv == nullptr) {
      return;
    }
    auto watcher = Watcher::Open(kv.get(), dir + "/watch");
    if (watcher == nullptr) {
      return;
    }
    std::uint64_t sink = 0;
    watcher->Watch("key-", [&sink](const go2cpp::storage::WatchEvent& e) {
      sink += e.key.size();
    });
    const double t0 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
      watcher->Put(keys[i & (kspace - 1)], vals[i & (kspace - 1)]);
    }
    const double elapsed = NowNs() - t0;
    Report("watch.put(sync 1 sub)", n, elapsed, 32.0);
    const auto stats = watcher->GetStats();
    std::printf("      events=%llu delivered=%llu callback=%.1fns\n",
                static_cast<unsigned long long>(stats.events),
                static_cast<unsigned long long>(stats.delivered),
                static_cast<double>(stats.callback_ns) /
                    std::max<std::uint64_t>(stats.delivered, 1));
    (void)sink;
    watcher.reset();
    kv->Close();
  }
  // 场景 2：无订阅者（同一批写，独立 KV）
  {
    const std::string dir = TempDir("watch_nosub");
    auto kv = KV::Open(dir + "/kv");
    if (kv == nullptr) {
      return;
    }
    auto watcher = Watcher::Open(kv.get(), dir + "/watch");
    if (watcher == nullptr) {
      return;
    }
    const double t0 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
      watcher->Put(keys[i & (kspace - 1)], vals[i & (kspace - 1)]);
    }
    const double elapsed = NowNs() - t0;
    Report("watch.put(0 sub, no event)", n, elapsed, 32.0);
    watcher.reset();
    kv->Close();
  }
  // 场景 3：纯 KV::Put 基线（独立 KV）
  {
    const std::string dir = TempDir("kvput");
    auto kv = KV::Open(dir + "/kv");
    if (kv == nullptr) {
      return;
    }
    const double t0 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
      kv->Put(keys[i & (kspace - 1)], vals[i & (kspace - 1)]);
    }
    Report("kv.put(baseline)", n, NowNs() - t0, 32.0);
    kv->Close();
  }
}

void BenchWatchAsync(std::uint64_t n) {
  const std::uint64_t kspace = 1u << 16;
  std::vector<std::string> keys(kspace);
  std::vector<std::string> vals(kspace);
  for (std::uint64_t i = 0; i < kspace; ++i) {
    keys[i] = Key16(i);
    vals[i] = Value16(i);
  }
  const std::string dir = TempDir("watch_async");
  auto kv = KV::Open(dir + "/kv");
  if (kv == nullptr) {
    return;
  }
  WatchOptions opt;
  opt.sync_callbacks = false;
  opt.persist_events = false;
  auto watcher = Watcher::Open(kv.get(), dir + "/watch", opt);
  if (watcher == nullptr) {
    return;
  }
  std::atomic<std::uint64_t> sink{0};
  watcher->Watch("key-", [&sink](const go2cpp::storage::WatchEvent& e) {
    sink.fetch_add(e.key.size(), std::memory_order_relaxed);
  });
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    watcher->Put(keys[i & (kspace - 1)], vals[i & (kspace - 1)]);
  }
  const double enqueue_ns = NowNs() - t0;
  Report("watch.put(async enqueue)", n, enqueue_ns, 32.0);
  const double deadline = NowNs() + 20e9;
  while (watcher->GetStats().delivered < n && NowNs() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::printf("      delivered=%llu/%llu backpressure=%llu\n",
              static_cast<unsigned long long>(watcher->GetStats().delivered),
              static_cast<unsigned long long>(n),
              static_cast<unsigned long long>(
                  watcher->GetStats().async_backpressure));
  watcher.reset();
  kv->Close();
}

// --------------------------------------------------------------------------
// MemCache
// --------------------------------------------------------------------------

void BenchMemCache(std::uint64_t n, std::size_t shards, const char* label,
                   bool purge_on_get) {
  MemCacheOptions opt;
  opt.capacity = 1u << 20;
  opt.shards = shards;
  opt.purge_on_get = purge_on_get;
  auto cache_p = MemCache::Open(nullptr, opt);
  if (cache_p == nullptr) {
    return;
  }
  MemCache& cache = *cache_p;
  const std::uint64_t space = 1u << 20;
  std::vector<std::string> keys(space);
  std::vector<std::string> vals(space);
  for (std::uint64_t i = 0; i < space; ++i) {
    keys[i] = Key16(i);
    vals[i] = Value16(i);
  }
  XorShift rng(0xdeadbeef);
  char name[96];
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::uint64_t k = rng.Next() % space;
    cache.Set(keys[k], vals[k]);
  }
  const double set_ns = NowNs() - t0;
  std::snprintf(name, sizeof(name), "memcache.set(%s)", label);
  Report(name, n, set_ns, 32.0);

  // 3 轮取最小：WSL 上单次测量有 ±25% 噪声，取最稳定的一轮。
  double best_get = 1e300;
  double best_nocopy = 1e300;
  std::uint64_t first_hits = 0;
  for (int round = 0; round < 3; ++round) {
    const double t1 = NowNs();
    std::uint64_t hits = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::uint64_t k = rng.Next() % space;
      std::string v;
      if (cache.Get(keys[k], &v)) {
        ++hits;
      }
    }
    best_get = std::min(best_get, NowNs() - t1);
    if (round == 0) {
      first_hits = hits;
    }
    const double t2 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::uint64_t k = rng.Next() % space;
      if (cache.Get(keys[k], nullptr)) {
        ++hits;
      }
    }
    best_nocopy = std::min(best_nocopy, NowNs() - t2);
  }
  std::snprintf(name, sizeof(name), "memcache.get(%s,purge=%d)", label,
                purge_on_get ? 1 : 0);
  Report(name, n, best_get, 32.0);
  const double hitrate = 100.0 * static_cast<double>(first_hits) /
                         static_cast<double>(std::max<std::uint64_t>(n, 1));
  std::snprintf(name, sizeof(name), "memcache.get_nocopy(%s,purge=%d)", label,
                purge_on_get ? 1 : 0);
  Report(name, n, best_nocopy, 32.0);

  // Peek：只查表，不提升 LRU、不拷贝 value —— 纯查找下限
  double best_peek = 1e300;
  for (int round = 0; round < 3; ++round) {
    const double t3 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::uint64_t k = rng.Next() % space;
      cache.Peek(keys[k], nullptr);
    }
    best_peek = std::min(best_peek, NowNs() - t3);
  }
  std::snprintf(name, sizeof(name), "memcache.peek(%s)", label);
  Report(name, n, best_peek, 32.0);
  std::printf("      %s hitrate=%.1f%%\n", cache.StatsString().c_str(),
              hitrate);
}

// 对照：项目现有的 LruCache（同样是「哈希 + LRU + value 拷贝」，但没有
// 分片锁、没有 TTL 时间轮）。用它作为「带淘汰策略的缓存」的同类下限。
void BenchProjectLru(std::uint64_t n) {
  const std::uint64_t space = 1u << 20;
  std::vector<std::string> keys(space);
  std::vector<std::string> vals(space);
  for (std::uint64_t i = 0; i < space; ++i) {
    keys[i] = Key16(i);
    vals[i] = Value16(i);
  }
  go2cpp::cache::CacheOptions opt;
  opt.capacity = space;
  go2cpp::cache::LruCache<std::string, std::string> lru(opt);
  for (std::uint64_t i = 0; i < space; ++i) {
    lru.Put(keys[i], vals[i]);
  }
  XorShift rng(0xdeadbeef);
  double best = 1e300;
  for (int round = 0; round < 3; ++round) {
    const double t0 = NowNs();
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::uint64_t k = rng.Next() % space;
      std::string v;
      lru.Get(keys[k], &v);
    }
    best = std::min(best, NowNs() - t0);
  }
  Report("lrucache.get(project LRU,1e6)", n, best, 32.0);
}

// 对照：std::unordered_map（同 key 形状、同容量）
void BenchUnorderedMap(std::uint64_t n) {
  const std::uint64_t space = 1u << 20;
  std::unordered_map<std::string, std::string> map;
  map.reserve(space);
  std::vector<std::string> keys(space);
  for (std::uint64_t i = 0; i < space; ++i) {
    keys[i] = Key16(i);
    map.emplace(keys[i], Value16(i));
  }
  XorShift rng(0xdeadbeef);
  std::uint64_t hits = 0;
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::uint64_t k = rng.Next() % space;
    const auto it = map.find(keys[k]);
    if (it != map.end()) {
      ++hits;
    }
  }
  Report("unordered_map.find(1e6)", n, NowNs() - t0, 32.0);
  (void)hits;
}

// --------------------------------------------------------------------------
// 实测下限
// --------------------------------------------------------------------------

void BenchLowerBounds(std::uint64_t n, bool skip_disk) {
  volatile std::uint64_t sink = 0;
  const double t0 = NowNs();
  for (std::uint64_t i = 0; i < n; ++i) {
    sink += i;
  }
  Report("baseline.empty_loop", n, NowNs() - t0);

  BenchProjectLru(std::min<std::uint64_t>(n, 2000000));
  BenchUnorderedMap(std::min<std::uint64_t>(n, 2000000));

  {
    const std::size_t kSize = 1u << 20;
    std::vector<char> src(kSize, 'a');
    std::vector<char> dst(kSize, 'b');
    const std::uint64_t rounds = 256;  // 256MB
    const double m0 = NowNs();
    for (std::uint64_t i = 0; i < rounds; ++i) {
      std::memcpy(dst.data(), src.data(), kSize);
      sink += static_cast<std::uint64_t>(dst[i & 1023]);
    }
    const double elapsed = NowNs() - m0;
    const double bytes = static_cast<double>(rounds) * kSize;
    Report("lower.memcpy(1MB x256)", rounds, elapsed, kSize);
    std::printf("      %.1f GB/s 单线程 memcpy（内存带宽下限）\n",
                bytes / elapsed);
  }
  (void)sink;

  if (skip_disk) {
    return;
  }
  // 顺序写下限：/tmp（overlay/磁盘）与 /dev/shm（tmpfs）各测一次
  for (const char* root : {"/tmp", "/dev/shm"}) {
    const std::string path = std::string(root) + "/go2cpp_seqwrite.bin";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0644);
    if (fd < 0) {
      continue;
    }
    std::vector<char> buf(1u << 20, 'x');
    const int rounds = 1024;
    const double m0 = NowNs();
    for (int i = 0; i < rounds; ++i) {
      const ssize_t w = ::write(fd, buf.data(), buf.size());
      if (w != static_cast<ssize_t>(buf.size())) {
        break;
      }
    }
    ::fdatasync(fd);
    const double elapsed = NowNs() - m0;
    ::close(fd);
    ::unlink(path.c_str());
    const double bytes = static_cast<double>(rounds) * buf.size();
    char name[96];
    std::snprintf(name, sizeof(name), "lower.write(1MB x1024 @%s)", root);
    Report(name, rounds, elapsed, buf.size());
    std::printf("      %.1f GB/s 顺序写（含一次 fdatasync）\n", bytes / elapsed);
  }
  // fdatasync 单次延迟
  {
    const std::string path = TempDir("fsync") + "/data.bin";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          0644);
    if (fd >= 0) {
      std::vector<char> buf(4096, 'y');
      const int rounds = 200;
      const double m0 = NowNs();
      for (int i = 0; i < rounds; ++i) {
        const ssize_t w = ::write(fd, buf.data(), buf.size());
        if (w != static_cast<ssize_t>(buf.size())) {
          break;
        }
        ::fdatasync(fd);
      }
      const double elapsed = NowNs() - m0;
      ::close(fd);
      ::unlink(path.c_str());
      Report("lower.write+fdatasync(4KB)", rounds, elapsed, 4096);
      std::printf("      %.1f us/次 fdatasync\n", elapsed / rounds / 1000.0);
    }
  }
}

void PrintSummary() {
  std::printf("\n==== 汇总（ns/op 越小越好）====\n");
  std::printf("%-42s %12s %14s\n", "name", "ns/op", "ops/s");
  for (const auto& r : g_results) {
    std::printf("%-42s %12.1f %14.0f\n", r.name.c_str(), r.ns_per_op,
                r.ops_per_sec);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t n = 10000000;
  bool skip_disk = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--n=", 4) == 0) {
      n = std::strtoull(argv[i] + 4, nullptr, 10);
    } else if (std::strcmp(argv[i], "--skip-disk") == 0) {
      skip_disk = true;
    }
  }
  std::printf("go2cpp_storage_features_bench (n=%llu, skip_disk=%d)\n",
              static_cast<unsigned long long>(n), skip_disk ? 1 : 0);
  const std::uint64_t mid = std::max<std::uint64_t>(n / 4, 200000);
  BenchQueueAppend(n, false);  // 优化前：每次查找都走 map
  BenchQueueAppend(n, true);   // 优化后：thread_local topic 缓存
  BenchQueueConsume(mid);
  BenchQueueColdReplay(std::min<std::uint64_t>(mid, 1000000));
  BenchSegLogAppend(n);
  BenchWatch(mid);
  BenchWatchAsync(mid);
  BenchMemCache(n, 1, "1 shard", true);   // 优化前：每次 Get 扫 TTL 轮
  BenchMemCache(n, 1, "1 shard", false);  // 优化后：节流
  BenchMemCache(n, 8, "8 shards", false);
  BenchLowerBounds(n, skip_disk);
  PrintSummary();
  return 0;
}
