// Copyright 2026 Go2Cpp Authors. All rights reserved.
// Watch 正确性测试：1e5 写触发回调、顺序/无丢失、异步、Replay、游标持久化。

#include "go2cpp/storage/kv.hpp"
#include "go2cpp/storage/watch.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using go2cpp::storage::KV;
using go2cpp::storage::WatchEvent;
using go2cpp::storage::Watcher;
using go2cpp::storage::WatchOptions;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

#define CHECK_MSG(cond, ...)                                              \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      std::fprintf(stderr, __VA_ARGS__);                                  \
      std::fprintf(stderr, "\n");                                         \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

std::string TempDir(const char* tag) {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/go2cpp_watch_%s_XXXXXX", tag);
  char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "evt/%08d", i);
  return std::string(buf);
}

void TestBasicMatch() {
  const std::string dir = TempDir("basic");
  auto kv = KV::Open(dir);
  CHECK(kv != nullptr);
  auto watcher = Watcher::Open(kv.get(), dir + "/watch");
  CHECK(watcher != nullptr);
  if (watcher == nullptr) {
    return;
  }
  std::vector<WatchEvent> events;
  const std::uint64_t id = watcher->Watch(
      "user/", [&events](const WatchEvent& e) { events.push_back(e); });
  CHECK(id != 0);
  CHECK(watcher->SubscriptionCount() == 1);

  CHECK(watcher->Put("user/1", "alice"));
  CHECK(events.size() == 1);
  CHECK(events[0].key == "user/1");
  CHECK(events[0].value == "alice");
  CHECK(!events[0].deleted);
  CHECK(events[0].seq == 0);

  // 不匹配的前缀：不触发
  CHECK(watcher->Put("order/1", "o-1"));
  CHECK(events.size() == 1);
  // 写已经落到 LSM：回调返回后立刻能读到（同步模式的核心保证）
  std::string value;
  CHECK(kv->Get("order/1", &value));
  CHECK(value == "o-1");

  CHECK(watcher->Delete("user/1"));
  CHECK(events.size() == 2);
  CHECK(events[1].deleted);
  CHECK(events[1].key == "user/1");
  std::string gone;
  CHECK(!kv->Get("user/1", &gone));

  CHECK(watcher->Unwatch(id));
  CHECK(watcher->SubscriptionCount() == 0);
  CHECK(watcher->Put("user/2", "bob"));
  CHECK(events.size() == 2);
}

void TestMultiSubscriber() {
  const std::string dir = TempDir("multi_sub");
  auto kv = KV::Open(dir);
  auto watcher = Watcher::Open(kv.get(), dir + "/watch");
  CHECK(watcher != nullptr);
  if (watcher == nullptr) {
    return;
  }
  std::atomic<int> a{0};
  std::atomic<int> b{0};
  const auto id1 = watcher->Watch("user/", [&a](const WatchEvent&) { ++a; });
  const auto id2 = watcher->Watch("user/1", [&b](const WatchEvent&) { ++b; });
  CHECK(id1 != id2);

  CHECK(watcher->Put("user/1", "x"));
  CHECK(watcher->Put("user/2", "y"));
  CHECK(watcher->Put("admin/1", "z"));
  CHECK(a.load() == 2);
  CHECK(b.load() == 1);
  CHECK(watcher->GetStats().delivered == 3);
}

// 1e5 次写：验证回调次数、seq 连续、key 顺序、无丢失。
void TestManyWrites(int n) {
  const std::string dir = TempDir("many");
  auto kv = KV::Open(dir);
  auto watcher = Watcher::Open(kv.get(), dir + "/watch");
  CHECK(watcher != nullptr);
  if (watcher == nullptr) {
    return;
  }
  std::uint64_t count = 0;
  std::uint64_t bad_seq = 0;
  std::uint64_t bad_key = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const std::uint64_t id = watcher->Watch(
      "evt/", [&count, &bad_seq, &bad_key](const WatchEvent& e) {
        if (e.seq != count) {
          ++bad_seq;
        }
        if (e.key != Key(static_cast<int>(count))) {
          ++bad_key;
        }
        ++count;
      });
  CHECK(id != 0);
  for (int i = 0; i < n; ++i) {
    CHECK(watcher->Put(Key(i), "v"));
  }
  const double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
  CHECK_MSG(count == static_cast<std::uint64_t>(n), "count=%llu n=%d",
            static_cast<unsigned long long>(count), n);
  CHECK_MSG(bad_seq == 0, "bad_seq=%llu", static_cast<unsigned long long>(bad_seq));
  CHECK_MSG(bad_key == 0, "bad_key=%llu", static_cast<unsigned long long>(bad_key));
  CHECK(watcher->last_seq() == static_cast<std::uint64_t>(n) - 1);
  const auto stats = watcher->GetStats();
  CHECK(stats.events == static_cast<std::uint64_t>(n));
  std::printf("  [1e5] writes=%d elapsed=%.2fs callback=%.0f/s avg_cb=%.0fns\n",
              n, elapsed, n / elapsed,
              n == 0 ? 0.0 : static_cast<double>(stats.callback_ns) / n);
}

void TestBatchWrite() {
  const std::string dir = TempDir("batch");
  auto kv = KV::Open(dir);
  auto watcher = Watcher::Open(kv.get(), dir + "/watch");
  CHECK(watcher != nullptr);
  if (watcher == nullptr) {
    return;
  }
  std::atomic<int> hits{0};
  CHECK(watcher->Watch("b/", [&hits](const WatchEvent&) { ++hits; }) != 0);
  go2cpp::storage::WriteBatch batch;
  batch.Put("b/1", "1");
  batch.Put("b/2", "2");
  batch.Put("other/3", "3");
  batch.Delete("b/4");
  CHECK(watcher->Write(batch));
  CHECK(hits.load() == 3);
  std::string value;
  CHECK(kv->Get("b/1", &value));
  CHECK(value == "1");
}

void TestAsync() {
  const std::string dir = TempDir("async");
  auto kv = KV::Open(dir);
  WatchOptions opt;
  opt.sync_callbacks = false;
  opt.persist_events = false;  // 异步 + 纯内存通知
  auto watcher = Watcher::Open(kv.get(), dir + "/watch", opt);
  CHECK(watcher != nullptr);
  if (watcher == nullptr) {
    return;
  }
  std::atomic<int> delivered{0};
  std::atomic<bool> order_ok{true};
  std::atomic<std::uint64_t> expect{0};
  CHECK(watcher->Watch("a/", [&delivered, &order_ok, &expect](
                                   const WatchEvent& e) {
          if (e.seq < expect.load()) {
            order_ok.store(false);
          }
          expect.store(e.seq + 1);
          ++delivered;
        }) != 0);
  const int n = 10000;
  for (int i = 0; i < n; ++i) {
    CHECK(watcher->Put("a/" + std::to_string(i), "v"));
  }
  // 等后台线程排空（最多 5s）
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (delivered.load() < n && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK_MSG(delivered.load() == n, "delivered=%d n=%d", delivered.load(), n);
  CHECK(order_ok.load());
}

void TestReplay() {
  const std::string dir = TempDir("replay");
  auto kv = KV::Open(dir);
  auto watcher = Watcher::Open(kv.get(), dir + "/watch");
  CHECK(watcher != nullptr);
  if (watcher == nullptr) {
    return;
  }
  std::vector<std::string> seen;
  const std::uint64_t id = watcher->Watch(
      "r/", [&seen](const WatchEvent& e) { seen.push_back(e.key); });
  for (int i = 0; i < 5; ++i) {
    CHECK(watcher->Put("r/" + std::to_string(i), "v"));
  }
  CHECK(seen.size() == 5);
  seen.clear();
  // 从头重放（模拟订阅者重启补课）
  CHECK(watcher->Replay(id, 0));
  CHECK(seen.size() == 5);
  CHECK(seen[0] == "r/0");
  CHECK(seen[4] == "r/4");
  // 从中间重放
  seen.clear();
  CHECK(watcher->Replay(id, 3));
  CHECK(seen.size() == 2);
  CHECK(seen[0] == "r/3");
  // 游标是最后派发的 seq
  CHECK(watcher->Offset(id) == 4);
}

void TestCursorPersistence() {
  const std::string dir = TempDir("cursor");
  const std::uint64_t stable = 4242;
  {
    auto kv = KV::Open(dir);
    auto watcher = Watcher::Open(kv.get(), dir + "/watch");
    CHECK(watcher != nullptr);
    if (watcher == nullptr) {
      return;
    }
    std::atomic<int> hits{0};
    CHECK(watcher->Watch("c/", [&hits](const WatchEvent&) { ++hits; },
                         stable) == stable);
    for (int i = 0; i < 100; ++i) {
      CHECK(watcher->Put("c/" + std::to_string(i), "v"));
    }
    CHECK(watcher->FlushOffsets());
    std::string stored;
    CHECK(kv->Get("watch/4242/offset", &stored));
    CHECK(stored.size() == 8);
    // 游标是 KV 里的普通记录：共用同一套 LSM
    CHECK(watcher->Offset(stable) == 99);
    watcher.reset();
    kv->Close();
  }
  {
    auto kv = KV::Open(dir);
    auto watcher = Watcher::Open(kv.get(), dir + "/watch");
    CHECK(watcher != nullptr);
    if (watcher == nullptr) {
      return;
    }
    std::atomic<int> hits{0};
    CHECK(watcher->Watch("c/", [&hits](const WatchEvent&) { ++hits; }, stable) ==
          stable);
    CHECK(watcher->Offset(stable) == 99);
    // 续上：只重放新事件
    CHECK(watcher->Replay(stable, watcher->Offset(stable) + 1));
    CHECK(hits.load() == 0);
    CHECK(watcher->Put("c/next", "v"));
    CHECK(hits.load() == 1);
    watcher.reset();
    kv->Close();
  }
}

// 没有订阅者时，写路径不应产生事件、不应写变更日志。
void TestNoSubscriberFastPath() {
  const std::string dir = TempDir("nosub");
  auto kv = KV::Open(dir);
  auto watcher = Watcher::Open(kv.get(), dir + "/watch");
  CHECK(watcher != nullptr);
  if (watcher == nullptr) {
    return;
  }
  for (int i = 0; i < 1000; ++i) {
    CHECK(watcher->Put("n/" + std::to_string(i), "v"));
  }
  const auto stats = watcher->GetStats();
  CHECK(stats.events == 0);
  CHECK(stats.log_bytes == 0);
  CHECK(stats.writes == 1000);
}

}  // namespace

int main(int argc, char** argv) {
  int n = 100000;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--n=", 4) == 0) {
      n = std::atoi(argv[i] + 4);
    }
  }
  std::printf("go2cpp_watch_test (n=%d)\n", n);
  TestBasicMatch();
  TestMultiSubscriber();
  TestBatchWrite();
  TestNoSubscriberFastPath();
  TestReplay();
  TestCursorPersistence();
  TestAsync();
  TestManyWrites(n);
  std::printf("%s: %d checks, %d failures\n",
              g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
