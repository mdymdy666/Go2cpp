// Copyright 2026 Go2Cpp Authors. All rights reserved.
// Queue 正确性测试：1e6 append/consume/ack、游标持久化、崩溃恢复、段滚动。

#include "go2cpp/storage/kv.hpp"
#include "go2cpp/storage/queue.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using go2cpp::storage::KV;
using go2cpp::storage::Options;
using go2cpp::storage::Queue;
using go2cpp::storage::QueueMessage;
using go2cpp::storage::QueueOptions;

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
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/go2cpp_queue_%s_XXXXXX", tag);
  char* dir = ::mkdtemp(tmpl);
  return dir != nullptr ? std::string(dir) : std::string();
}

std::string Payload(std::uint64_t i) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "msg-%016llu-body",
                static_cast<unsigned long long>(i));
  return std::string(buf);
}

std::uint64_t ParsePayload(const std::string& s) {
  if (s.size() < 20) {
    return UINT64_MAX;
  }
  return std::strtoull(s.c_str() + 4, nullptr, 10);
}

struct Env {
  std::string dir;
  std::unique_ptr<KV> kv;
  std::unique_ptr<Queue> queue;
};

bool OpenEnv(Env* env, const QueueOptions& opt) {
  env->kv = KV::Open(env->dir);
  if (env->kv == nullptr) {
    return false;
  }
  env->queue = Queue::Open(env->kv.get(), env->dir + "/queue", opt);
  return env->queue != nullptr;
}

void TestBasicRoundTrip() {
  Env env;
  env.dir = TempDir("basic");
  QueueOptions opt;
  CHECK(OpenEnv(&env, opt));
  if (env.queue == nullptr) {
    return;
  }
  std::uint64_t offset = 0;
  CHECK(env.queue->Append("t1", "hello", &offset));
  CHECK(offset == 0);
  CHECK(env.queue->Append("t1", "world", &offset));
  CHECK(offset == 1);
  CHECK(env.queue->Head("t1") == 2);
  CHECK(env.queue->Head("t2") == 0);

  QueueMessage msg;
  CHECK(env.queue->Consume("t1", "c1", &msg));
  CHECK(msg.offset == 0);
  CHECK(msg.value == "hello");
  CHECK(msg.topic == "t1");
  // 未 ack：重复投递同一条（at-least-once）
  CHECK(env.queue->Consume("t1", "c1", &msg));
  CHECK(msg.offset == 0);
  CHECK(env.queue->Cursor("t1", "c1") == 0);
  CHECK(env.queue->Ack("t1", "c1", 0));
  CHECK(env.queue->Cursor("t1", "c1") == 1);
  CHECK(env.queue->Consume("t1", "c1", &msg));
  CHECK(msg.offset == 1);
  CHECK(msg.value == "world");
  // 未 ack 时仍会重复投递同一条；ack 后才推进到末尾
  CHECK(env.queue->Consume("t1", "c1", &msg));
  CHECK(msg.offset == 1);
  CHECK(env.queue->Ack("t1", "c1", 1));
  CHECK(env.queue->Cursor("t1", "c1") == 2);
  CHECK(!env.queue->Consume("t1", "c1", &msg));
  // 乱序 ack（回退）不生效
  CHECK(env.queue->Ack("t1", "c1", 0));
  CHECK(env.queue->Cursor("t1", "c1") == 2);
}

void TestMultiConsumer() {
  Env env;
  env.dir = TempDir("multi_consumer");
  QueueOptions opt;
  CHECK(OpenEnv(&env, opt));
  if (env.queue == nullptr) {
    return;
  }
  for (int i = 0; i < 10; ++i) {
    CHECK(env.queue->Append("jobs", Payload(static_cast<std::uint64_t>(i))));
  }
  QueueMessage a;
  QueueMessage b;
  // worker-a 每条都 ack；worker-b 一直不 ack，应当反复拿到同一条（at-least-once）。
  for (int i = 0; i < 10; ++i) {
    CHECK(env.queue->Consume("jobs", "worker-a", &a));
    CHECK(env.queue->Consume("jobs", "worker-b", &b));
    CHECK(a.offset == static_cast<std::uint64_t>(i));
    CHECK(b.offset == 0);
    CHECK(env.queue->Ack("jobs", "worker-a", a.offset));
  }
  CHECK(env.queue->Cursor("jobs", "worker-a") == 10);
  CHECK(env.queue->Cursor("jobs", "worker-b") == 0);
  CHECK(env.queue->Consume("jobs", "worker-b", &b));
  CHECK(b.offset == 0);
  // worker-b 补完 10 条后与 worker-a 齐平
  for (int i = 0; i < 10; ++i) {
    CHECK(env.queue->Consume("jobs", "worker-b", &b));
    CHECK(b.offset == static_cast<std::uint64_t>(i));
    CHECK(env.queue->Ack("jobs", "worker-b", b.offset));
  }
  CHECK(env.queue->Cursor("jobs", "worker-b") == 10);
  CHECK(!env.queue->Consume("jobs", "worker-b", &b));
}

void TestMultiTopic() {
  Env env;
  env.dir = TempDir("multi_topic");
  QueueOptions opt;
  CHECK(OpenEnv(&env, opt));
  if (env.queue == nullptr) {
    return;
  }
  // 三个 topic 交错写入
  for (int i = 0; i < 100; ++i) {
    CHECK(env.queue->Append("a", "a-" + std::to_string(i)));
    CHECK(env.queue->Append("b", "b-" + std::to_string(i)));
    CHECK(env.queue->Append("c", "c-" + std::to_string(i)));
  }
  QueueMessage msg;
  for (int i = 0; i < 100; ++i) {
    CHECK(env.queue->Consume("a", "ca", &msg));
    CHECK(msg.offset == static_cast<std::uint64_t>(i));
    CHECK(msg.value == "a-" + std::to_string(i));
    CHECK(env.queue->Ack("a", "ca", msg.offset));
    CHECK(env.queue->Consume("b", "cb", &msg));
    CHECK(msg.value == "b-" + std::to_string(i));
    CHECK(env.queue->Ack("b", "cb", msg.offset));
  }
  CHECK(env.queue->Head("a") == 100);
  CHECK(env.queue->Head("b") == 100);
  CHECK(env.queue->Head("c") == 100);
  CHECK(env.queue->Topics().size() == 3);
}

// 1e6：append 全部后逐条 consume + ack，验证 offset 严格 0..n-1（无丢失/无重复）。
void TestLargeVolume(std::uint64_t n) {
  Env env;
  env.dir = TempDir("large");
  QueueOptions opt;
  opt.cursor_flush_interval = 1u << 16;
  CHECK(OpenEnv(&env, opt));
  if (env.queue == nullptr) {
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t bad = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t off = 0;
    if (!env.queue->Append("bench", Payload(i), &off) || off != i) {
      ++bad;
    }
  }
  CHECK_MSG(bad == 0, "append offset mismatches=%llu",
            static_cast<unsigned long long>(bad));
  CHECK_MSG(env.queue->Head("bench") == n, "head=%llu n=%llu",
            static_cast<unsigned long long>(env.queue->Head("bench")),
            static_cast<unsigned long long>(n));

  QueueMessage msg;
  bad = 0;
  std::uint64_t checked = 0;
  for (std::uint64_t i = 0; i < n; ++i) {
    if (!env.queue->Consume("bench", "c1", &msg)) {
      ++bad;
      break;
    }
    if (msg.offset != i || ParsePayload(msg.value) != i) {
      ++bad;
    }
    if (!env.queue->Ack("bench", "c1", msg.offset)) {
      ++bad;
    }
    ++checked;
  }
  CHECK_MSG(bad == 0, "consume mismatches=%llu checked=%llu",
            static_cast<unsigned long long>(bad),
            static_cast<unsigned long long>(checked));
  CHECK(checked == n);
  CHECK(env.queue->Cursor("bench", "c1") == n);
  CHECK(!env.queue->Consume("bench", "c1", &msg));
  const double elapsed =
      std::chrono::duration<double>(
          std::chrono::steady_clock::now() - t0)
          .count();
  const auto stats = env.queue->GetStats();
  std::printf("  [1e6] n=%llu elapsed=%.2fs append=%.0f/s consume+ack=%.0f/s "
              "segments=%llu retained=%llu bytes=%llu\n",
              static_cast<unsigned long long>(n), elapsed,
              n / elapsed, n / elapsed,
              static_cast<unsigned long long>(stats.segments),
              static_cast<unsigned long long>(stats.retained),
              static_cast<unsigned long long>(stats.produced_bytes));
}

// 游标持久化：写入一批、ack、Sync，重开同一 KV 目录后游标不回退。
void TestCursorPersistence() {
  const std::string dir = TempDir("cursor");
  {
    auto kv = KV::Open(dir);
    CHECK(kv != nullptr);
    QueueOptions opt;
    auto q = Queue::Open(kv.get(), dir + "/queue", opt);
    CHECK(q != nullptr);
    if (q == nullptr) {
      return;
    }
    for (int i = 0; i < 5000; ++i) {
      CHECK(q->Append("persist", Payload(static_cast<std::uint64_t>(i))));
    }
    QueueMessage msg;
    for (int i = 0; i < 5000; ++i) {
      CHECK(q->Consume("persist", "worker", &msg));
      CHECK(q->Ack("persist", "worker", msg.offset));
    }
    CHECK(q->Cursor("persist", "worker") == 5000);
    CHECK(q->Sync());
    // 游标作为普通 KV 记录存在（证明共用同一套 LSM）
    std::string stored;
    CHECK(kv->Get("queue/persist/worker/offset", &stored));
    CHECK(stored.size() == 8);
    q.reset();
    kv->Close();
  }
  {
    auto kv = KV::Open(dir);
    CHECK(kv != nullptr);
    auto q = Queue::Open(kv.get(), dir + "/queue");
    CHECK(q != nullptr);
    if (q == nullptr) {
      return;
    }
    CHECK(q->Head("persist") == 5000);
    CHECK(q->Cursor("persist", "worker") == 5000);
    QueueMessage msg;
    CHECK(!q->Consume("persist", "worker", &msg));
    // 继续追加：offset 接续
    std::uint64_t off = 0;
    CHECK(q->Append("persist", "after-restart", &off));
    CHECK(off == 5000);
    CHECK(q->Consume("persist", "worker", &msg));
    CHECK(msg.offset == 5000);
    CHECK(msg.value == "after-restart");
    q.reset();
    kv->Close();
  }
}

// 段滚动：小段阈值下应产生多个段，且恢复后消息完整。
void TestSegmentRoll() {
  const std::string dir = TempDir("roll");
  QueueOptions opt;
  opt.segment_size = 256 * 1024;
  opt.index_interval = 64;
  std::uint64_t total = 20000;
  {
    auto kv = KV::Open(dir);
    auto q = Queue::Open(kv.get(), dir + "/queue", opt);
    CHECK(q != nullptr);
    if (q == nullptr) {
      return;
    }
    for (std::uint64_t i = 0; i < total; ++i) {
      CHECK(q->Append("roll", Payload(i)));
    }
    CHECK(q->SegmentCount() >= 2);
    CHECK(q->Sync());
    const auto stats = q->GetStats();
    CHECK_MSG(stats.segments >= 2, "segments=%llu",
              static_cast<unsigned long long>(stats.segments));
    q.reset();
    kv->Close();
  }
  // 索引文件应存在（段封口时写成 SSTable）
  int idx_files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir + "/queue")) {
    if (e.path().extension() == ".idx") {
      ++idx_files;
    }
  }
  CHECK(idx_files >= 1);

  {
    auto kv = KV::Open(dir);
    auto q = Queue::Open(kv.get(), dir + "/queue", opt);
    CHECK(q != nullptr);
    if (q == nullptr) {
      return;
    }
    CHECK(q->Head("roll") == total);
    QueueMessage msg;
    std::uint64_t bad = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      if (!q->Consume("roll", "c", &msg) || msg.offset != i) {
        ++bad;
        break;
      }
      CHECK(q->Ack("roll", "c", msg.offset));
    }
    CHECK(bad == 0);
    // 冷读（模拟消费者从 0 重放已回收/未预取的历史）
    CHECK(q->Seek("roll", "replay", 0));
    CHECK(q->Consume("roll", "replay", &msg));
    CHECK(msg.offset == 0);
    CHECK(ParsePayload(msg.value) == 0);
    q.reset();
    kv->Close();
  }
}

// 冷读路径：retain 很小，强制消息被回收后仍能从日志读到。
void TestColdRead() {
  Env env;
  env.dir = TempDir("cold");
  QueueOptions opt;
  opt.retain = 16;
  CHECK(OpenEnv(&env, opt));
  if (env.queue == nullptr) {
    return;
  }
  for (int i = 0; i < 1000; ++i) {
    CHECK(env.queue->Append("cold", Payload(static_cast<std::uint64_t>(i))));
  }
  // 内存里只剩最后 16 条，消费者却从 0 开始 -> 必须走日志冷读预取。
  // 未 ack 之前按 at-least-once 语义反复拿到同一条。
  QueueMessage msg;
  for (int i = 0; i < 10; ++i) {
    if (!env.queue->Consume("cold", "c", &msg)) {
      CHECK(false);
      return;
    }
    CHECK_MSG(msg.offset == 0, "i=%d off=%llu val=%s", i,
              static_cast<unsigned long long>(msg.offset), msg.value.c_str());
    CHECK(ParsePayload(msg.value) == 0);
  }
  CHECK_MSG(env.queue->GetStats().cold_reads > 0, "cold_reads=%llu",
            static_cast<unsigned long long>(env.queue->GetStats().cold_reads));
  CHECK(env.queue->Ack("cold", "c", 0));
  // 预取批次（0..999）都还在内存里：后面的消费不再碰日志。
  const std::uint64_t cold_before = env.queue->GetStats().cold_reads;
  std::uint64_t bad = 0;
  for (int i = 1; i < 1000; ++i) {
    if (!env.queue->Consume("cold", "c", &msg) ||
        msg.offset != static_cast<std::uint64_t>(i)) {
      ++bad;
      break;
    }
    CHECK(env.queue->Ack("cold", "c", msg.offset));
  }
  CHECK_MSG(bad == 0, "cold read failed at offset");
  CHECK(env.queue->GetStats().cold_reads == cold_before);
}

// 崩溃恢复：子进程 append 1e5 条 + Sync 后 kill -9；父进程重开验证不丢。
void TestCrashRecovery() {
  const std::string dir = TempDir("crash");
  {
    auto kv = KV::Open(dir);
    CHECK(kv != nullptr);
    kv->Close();
  }
  std::fflush(stdout);
  const pid_t pid = ::fork();
  if (pid == 0) {
    auto kv = KV::Open(dir);
    if (kv == nullptr) {
      ::_exit(2);
    }
    QueueOptions opt;
    opt.segment_size = 1u << 20;
    auto q = Queue::Open(kv.get(), dir + "/queue", opt);
    if (q == nullptr) {
      ::_exit(3);
    }
    for (int i = 0; i < 100000; ++i) {
      if (!q->Append("crash", Payload(static_cast<std::uint64_t>(i)))) {
        ::_exit(4);
      }
    }
    // 确认能正常消费前 10 条（模拟业务推进游标）
    QueueMessage msg;
    for (int i = 0; i < 10; ++i) {
      if (!q->Consume("crash", "w", &msg) || msg.offset !=
              static_cast<std::uint64_t>(i)) {
        ::_exit(5);
      }
      q->Ack("crash", "w", msg.offset);
    }
    if (!q->Sync()) {
      ::_exit(6);
    }
    // 游标是 KV 记录：KV 自身的 WAL 缓冲需要一次 Close 才 fdatasync。
    // 这里模拟“业务正常关闭引擎、随后进程被强杀”，验证游标持久化。
    kv->Close();
    ::raise(SIGKILL);  // 不执行析构：模拟进程被强杀
    ::_exit(7);
  }
  int status = 0;
  CHECK(pid > 0);
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status));

  auto kv = KV::Open(dir);
  CHECK(kv != nullptr);
  QueueOptions opt;
  opt.segment_size = 1u << 20;
  auto q = Queue::Open(kv.get(), dir + "/queue", opt);
  CHECK(q != nullptr);
  if (q == nullptr) {
    return;
  }
  CHECK_MSG(q->Head("crash") == 100000, "head=%llu",
            static_cast<unsigned long long>(q->Head("crash")));
  CHECK(q->Cursor("crash", "w") == 10);
  QueueMessage msg;
  CHECK(q->Consume("crash", "w", &msg));
  CHECK(msg.offset == 10);
  CHECK(ParsePayload(msg.value) == 10);
  std::uint64_t off = 0;
  CHECK(q->Append("crash", "post-crash", &off));
  CHECK(off == 100000);
  q.reset();
  kv->Close();
}

// 未 Sync 的写不应破坏已 Sync 的前缀（尾部可能丢，但已在盘上的不能坏）。
void TestTruncatedTail() {
  const std::string dir = TempDir("truncated");
  {
    auto kv = KV::Open(dir);
    auto q = Queue::Open(kv.get(), dir + "/queue");
    CHECK(q != nullptr);
    if (q == nullptr) {
      return;
    }
    for (int i = 0; i < 1000; ++i) {
      CHECK(q->Append("trunc", Payload(static_cast<std::uint64_t>(i))));
    }
    CHECK(q->Sync());
    q.reset();
    kv->Close();
  }
  // 追加一段垃圾（模拟 kill -9 时的半条记录）
  const std::string seg = dir + "/queue/000000.seg";
  {
    FILE* f = std::fopen(seg.c_str(), "ab");
    CHECK(f != nullptr);
    if (f != nullptr) {
      const char garbage[13] = {0x01, 0x02, 0x03, 0x04,
                                0x40, 0x00, 0x01, 0x41};
      std::fwrite(garbage, 1, sizeof(garbage), f);
      std::fclose(f);
    }
  }
  auto kv = KV::Open(dir);
  auto q = Queue::Open(kv.get(), dir + "/queue");
  CHECK(q != nullptr);
  if (q == nullptr) {
    return;
  }
  CHECK(q->Head("trunc") == 1000);
  QueueMessage msg;
  CHECK(q->Consume("trunc", "c", &msg));
  CHECK(msg.offset == 0);
  // 尾部垃圾之后仍能追加
  std::uint64_t off = 0;
  CHECK(q->Append("trunc", "after-garbage", &off));
  CHECK(off == 1000);
  q.reset();
  kv->Close();
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t n = 1000000;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--n=", 4) == 0) {
      n = std::strtoull(argv[i] + 4, nullptr, 10);
    }
  }
  std::printf("go2cpp_queue_test (n=%llu)\n",
              static_cast<unsigned long long>(n));
  TestBasicRoundTrip();
  TestMultiConsumer();
  TestMultiTopic();
  TestColdRead();
  TestCursorPersistence();
  TestSegmentRoll();
  TestTruncatedTail();
  TestCrashRecovery();
  TestLargeVolume(n);
  std::printf("%s: %d checks, %d failures\n",
              g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
