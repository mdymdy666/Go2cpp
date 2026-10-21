/**
 * @file test_channel_thread_stress.cpp
 * @brief 线程版 Channel 高频混合压测 + 挂起现场采集。
 *
 * 复现目标：普通 OS 线程（std::thread，不启动 Scheduler）使用 Channel 的
 * 高频混合负载偶发挂起。场景：
 *   mpmc64 / mpmc1 / mpmc0 : 4 生产者 + 4 消费者，cap=64/1/0
 *   rtt0                   : 两个无缓冲通道双向 ping-pong
 *   mixed                  : 2 worker 每轮 sync::Mutex -> Send -> PC notify，
 *                            另有 consumer 与 helper（线程版）
 *
 * 挂起判定：所有 worker 的进度计数连续 --hang-sec 秒（默认 30s）无变化且
 * 仍有活跃 worker。现场采集：
 *   1. 每个线程的 role / iter / stage；
 *   2. 所有已注册 Channel 的 DebugSnapshot（cap/len/send_waiters/...），
 *      单快照最多等 1s，超时即说明 Channel::m_mutex 被长期持有；
 *   3. 写 /tmp/channel_hang.marker；
 *   4. gdb -p <pid> -batch -ex 'thread apply all bt' -> /tmp/channel_hang_bt.txt，
 *      随后 _exit(3)。
 */
#include "go2cpp/channel.hpp"
#include "go2cpp/core/parking_condition.hpp"
#include "go2cpp/sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

constexpr std::size_t kMaxSlots = 96;

struct alignas(64) SlotState {
  std::atomic<std::uint64_t> iter{0};
  std::atomic<std::uint32_t> stage{0};
  std::atomic<const char*> role{nullptr};
};

SlotState g_slots[kMaxSlots];
std::atomic<int> g_slot_count{0};

std::atomic<std::uint64_t> g_round{0};
std::atomic<std::uint64_t> g_rounds{0};
std::atomic<const char*> g_scenario{"init"};
std::atomic<std::uint64_t> g_errors{0};
std::atomic<bool> g_done{false};
std::atomic<bool> g_hang_reported{false};
std::atomic<int> g_hang_seconds{30};
std::atomic<bool> g_gdb_enabled{true};
std::atomic<std::uint64_t> g_active{0};

std::mutex g_reg_mutex;

struct ChannelEntry {
  std::string name;
  std::function<go2cpp::ChannelDebugSnapshot()> snapshot;
};

std::vector<ChannelEntry> g_channels;

int AllocSlot(const char* role) {
  const int id = g_slot_count.fetch_add(1, std::memory_order_relaxed);
  if (id < 0 || id >= static_cast<int>(kMaxSlots)) {
    std::fprintf(stderr, "[ERROR] worker slot overflow\n");
    std::abort();
  }
  g_slots[id].role.store(role, std::memory_order_relaxed);
  return id;
}

inline void Touch(int slot) {
  g_slots[slot].iter.fetch_add(1, std::memory_order_relaxed);
}

inline void SetStage(int slot, std::uint32_t stage) {
  g_slots[slot].stage.store(stage, std::memory_order_relaxed);
}

void ResetSlots() {
  g_slot_count.store(0, std::memory_order_relaxed);
  for (auto& slot : g_slots) {
    slot.iter.store(0, std::memory_order_relaxed);
    slot.stage.store(0, std::memory_order_relaxed);
    slot.role.store(nullptr, std::memory_order_relaxed);
  }
}

void Fail(const char* what) {
  const auto index = g_errors.fetch_add(1, std::memory_order_relaxed);
  if (index < 20) {
    std::fprintf(stderr, "[ERROR] %s\n", what);
    std::fflush(stderr);
  }
}

void RegisterChannelFn(const std::string& name,
                       std::function<go2cpp::ChannelDebugSnapshot()> snapshot) {
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  g_channels.push_back(ChannelEntry{name, std::move(snapshot)});
}

template <typename T>
void RegisterChannel(const std::string& name,
                     const std::shared_ptr<go2cpp::Channel<T>>& channel) {
  RegisterChannelFn(name, [channel] { return channel->DebugSnapshot(); });
}

void ClearChannels() {
  std::lock_guard<std::mutex> lock(g_reg_mutex);
  g_channels.clear();
}

const char* StageName(const char* role, std::uint32_t stage) {
  const std::string_view r = role != nullptr ? role : "";
  if (r == "producer") {
    switch (stage) {
      case 0: return "idle";
      case 1: return "Send(enter)";
      case 2: return "Send(done)";
      case 9: return "finished";
      default: return "?";
    }
  }
  if (r == "consumer") {
    switch (stage) {
      case 0: return "idle";
      case 3: return "Recv(enter)";
      case 4: return "Recv(done)";
      case 9: return "finished";
      default: return "?";
    }
  }
  if (r == "mixed_worker") {
    switch (stage) {
      case 0: return "idle";
      case 1: return "Mutex::Lock";
      case 2: return "Mutex::Unlock";
      case 3: return "Channel::Send(enter)";
      case 4: return "Channel::Send(done)";
      case 5: return "pc notify";
      case 9: return "finished";
      default: return "?";
    }
  }
  if (r == "mixed_consumer") {
    switch (stage) {
      case 0: return "idle";
      case 6: return "Channel::Recv(enter)";
      case 7: return "Channel::Recv(done)";
      case 9: return "finished";
      default: return "?";
    }
  }
  if (r == "mixed_helper") {
    switch (stage) {
      case 0: return "idle";
      case 8: return "pc.wait";
      case 9: return "finished";
      default: return "?";
    }
  }
  if (r == "rtt_a" || r == "rtt_b") {
    switch (stage) {
      case 0: return "idle";
      case 1: return "Send(enter)";
      case 2: return "Recv(enter)";
      case 9: return "finished";
      default: return "?";
    }
  }
  return "?";
}

void DumpChannels() {
  std::vector<ChannelEntry> entries;
  {
    std::lock_guard<std::mutex> lock(g_reg_mutex);
    entries = g_channels;
  }
  if (entries.empty()) {
    std::fprintf(stderr, "  (no registered channels)\n");
    return;
  }
  for (auto& entry : entries) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto snapshot = std::make_shared<go2cpp::ChannelDebugSnapshot>();
    auto fn = entry.snapshot;
    std::thread([fn, done, snapshot] {
      *snapshot = fn();
      done->store(true, std::memory_order_release);
    }).detach();
    bool finished = false;
    for (int i = 0; i < 100; ++i) {
      if (done->load(std::memory_order_acquire)) {
        finished = true;
        break;
      }
      std::this_thread::sleep_for(Ms(10));
    }
    if (!finished) {
      std::fprintf(stderr,
                   "  channel %-12s : SNAPSHOT BLOCKED (Channel::m_mutex held?)\n",
                   entry.name.c_str());
      continue;
    }
    std::fprintf(stderr,
                 "  channel %-12s : cap=%zu len=%zu send_waiters=%zu "
                 "recv_waiters=%zu closed=%d gen=%llu\n",
                 entry.name.c_str(), snapshot->capacity, snapshot->length,
                 snapshot->send_waiters, snapshot->recv_waiters,
                 snapshot->closed ? 1 : 0,
                 static_cast<unsigned long long>(snapshot->generation));
  }
}

void ReportHang() {
  const std::uint64_t round = g_round.load(std::memory_order_relaxed);
  const char* scenario = g_scenario.load(std::memory_order_relaxed);
  std::fprintf(stderr,
               "\n================================================================\n");
  std::fprintf(stderr, "[HANG] pid=%d round=%llu/%llu scenario=%s\n",
               static_cast<int>(::getpid()),
               static_cast<unsigned long long>(round),
               static_cast<unsigned long long>(g_rounds.load()),
               scenario != nullptr ? scenario : "?");
  std::fprintf(stderr, "[HANG] worker states:\n");
  const int count = g_slot_count.load(std::memory_order_relaxed);
  for (int i = 0; i < count && i < static_cast<int>(kMaxSlots); ++i) {
    const char* role = g_slots[i].role.load(std::memory_order_relaxed);
    if (role == nullptr) {
      continue;
    }
    const std::uint32_t stage = g_slots[i].stage.load(std::memory_order_relaxed);
    std::fprintf(stderr, "  slot %-2d %-16s iter=%-10llu stage=%u (%s)\n", i,
                 role,
                 static_cast<unsigned long long>(
                     g_slots[i].iter.load(std::memory_order_relaxed)),
                 stage, StageName(role, stage));
  }
  std::fprintf(stderr, "[HANG] channel snapshots:\n");
  DumpChannels();
  std::fprintf(stderr,
               "----------------------------------------------------------------\n");
  std::fflush(stderr);

  char marker[256];
  const int marker_len =
      std::snprintf(marker, sizeof(marker), "%d %llu %s\n",
                    static_cast<int>(::getpid()),
                    static_cast<unsigned long long>(round),
                    scenario != nullptr ? scenario : "?");
  const int fd = ::open("/tmp/channel_hang.marker",
                        O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0) {
    if (::write(fd, marker, static_cast<std::size_t>(marker_len)) < 0) {
      // marker 只是给外部脚本的提示；写失败时仍继续抓栈。
    }
    (void)::close(fd);
  }

  if (g_gdb_enabled.load(std::memory_order_relaxed)) {
    char command[512];
    std::snprintf(command, sizeof(command),
                  "gdb -p %d -batch -ex 'set pagination off' "
                  "-ex 'thread apply all bt' > /tmp/channel_hang_bt.txt 2>&1",
                  static_cast<int>(::getpid()));
    const int rc = std::system(command);
    std::fprintf(stderr, "[HANG] gdb rc=%d, backtrace -> /tmp/channel_hang_bt.txt\n",
                 rc);
    std::fflush(stderr);
  }
  ::sleep(1);
  ::_exit(3);
}

void WatchdogLoop() {
  std::uint64_t last_sum = 0;
  auto last_change = Clock::now();
  std::fprintf(stderr, "[watchdog] armed: threshold=%ds\n",
               g_hang_seconds.load(std::memory_order_relaxed));
  std::fflush(stderr);
  while (!g_done.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(Ms(100));
    if (g_done.load(std::memory_order_acquire)) {
      return;
    }
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < kMaxSlots; ++i) {
      sum += g_slots[i].iter.load(std::memory_order_relaxed);
    }
    const auto now = Clock::now();
    if (sum != last_sum) {
      last_sum = sum;
      last_change = now;
      continue;
    }
    if (g_active.load(std::memory_order_relaxed) == 0) {
      last_change = now;
      continue;
    }
    const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                          now - last_change)
                          .count();
    if (idle >= g_hang_seconds.load(std::memory_order_relaxed) &&
        !g_hang_reported.exchange(true)) {
      ReportHang();
    }
  }
}

// ---------------------------------------------------------------------------
// 场景：线程版 MPMC（cap=64 / 1 / 0）
// ---------------------------------------------------------------------------

bool RunMpmc(std::uint64_t msgs, std::size_t capacity, int producers,
             int consumers) {
  auto channel = go2cpp::MakeChannel<int>(capacity);
  RegisterChannel("ch_" + std::to_string(capacity), channel);

  std::vector<std::atomic<std::uint8_t>> seen(msgs);
  for (auto& slot : seen) {
    slot.store(0, std::memory_order_relaxed);
  }
  std::atomic<std::uint64_t> next{0};

  g_active.fetch_add(static_cast<std::uint64_t>(producers + consumers));

  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(producers + consumers));

  const std::uint64_t per_producer = msgs / static_cast<std::uint64_t>(producers);
  const std::uint64_t extra_producer = msgs % static_cast<std::uint64_t>(producers);
  for (int p = 0; p < producers; ++p) {
    const std::uint64_t count =
        per_producer + (static_cast<std::uint64_t>(p) < extra_producer ? 1 : 0);
    threads.emplace_back([&, count] {
      const int slot = AllocSlot("producer");
      for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t value =
            next.fetch_add(1, std::memory_order_relaxed);
        SetStage(slot, 1);
        const auto result = channel->Send(static_cast<int>(value));
        if (result.status != go2cpp::ChannelStatus::kReady) {
          Fail("mpmc Send returned non-ready");
        }
        SetStage(slot, 2);
        Touch(slot);
      }
      SetStage(slot, 9);
      g_active.fetch_sub(1, std::memory_order_relaxed);
    });
  }

  const std::uint64_t per_consumer = msgs / static_cast<std::uint64_t>(consumers);
  const std::uint64_t extra_consumer = msgs % static_cast<std::uint64_t>(consumers);
  for (int c = 0; c < consumers; ++c) {
    const std::uint64_t count =
        per_consumer + (static_cast<std::uint64_t>(c) < extra_consumer ? 1 : 0);
    threads.emplace_back([&, count] {
      const int slot = AllocSlot("consumer");
      for (std::uint64_t i = 0; i < count; ++i) {
        SetStage(slot, 3);
        auto result = channel->Recv();
        if (!result.Ok()) {
          Fail("mpmc Recv returned non-ok");
          SetStage(slot, 9);
          g_active.fetch_sub(1, std::memory_order_relaxed);
          return;
        }
        const int value = *result.value;
        if (value < 0 || static_cast<std::uint64_t>(value) >= msgs) {
          Fail("mpmc Recv value out of range");
        } else {
          seen[static_cast<std::size_t>(value)].fetch_add(
              1, std::memory_order_relaxed);
        }
        SetStage(slot, 4);
        Touch(slot);
      }
      SetStage(slot, 9);
      g_active.fetch_sub(1, std::memory_order_relaxed);
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  std::uint64_t duplicated = 0;
  std::uint64_t missing = 0;
  for (auto& slot : seen) {
    const std::uint8_t value = slot.load(std::memory_order_relaxed);
    if (value == 0) {
      ++missing;
    } else if (value > 1) {
      ++duplicated;
    }
  }
  ClearChannels();
  if (duplicated != 0 || missing != 0) {
    std::fprintf(stderr,
                 "[ERROR] mpmc(cap=%zu) verify failed: duplicated=%llu "
                 "missing=%llu\n",
                 capacity, static_cast<unsigned long long>(duplicated),
                 static_cast<unsigned long long>(missing));
    std::fflush(stderr);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 场景：无缓冲双向 ping-pong（纯 rendezvous）
// ---------------------------------------------------------------------------

bool RunRtt(std::uint64_t rounds) {
  auto channel_a = go2cpp::MakeChannel<int>(0);
  auto channel_b = go2cpp::MakeChannel<int>(0);
  RegisterChannel("rtt_a", channel_a);
  RegisterChannel("rtt_b", channel_b);
  std::atomic<std::uint64_t> done_a{0};
  std::atomic<std::uint64_t> done_b{0};

  g_active.fetch_add(2);
  std::thread thread_a([&] {
    const int slot = AllocSlot("rtt_a");
    for (std::uint64_t i = 0; i < rounds; ++i) {
      SetStage(slot, 1);
      const auto send = channel_a->Send(static_cast<int>(i));
      if (send.status != go2cpp::ChannelStatus::kReady) {
        Fail("rtt A Send non-ready");
      }
      SetStage(slot, 2);
      const auto recv = channel_b->Recv();
      if (!recv.Ok()) {
        Fail("rtt A Recv non-ok");
        break;
      }
      done_a.fetch_add(1, std::memory_order_relaxed);
      Touch(slot);
    }
    SetStage(slot, 9);
    g_active.fetch_sub(1, std::memory_order_relaxed);
  });
  std::thread thread_b([&] {
    const int slot = AllocSlot("rtt_b");
    for (std::uint64_t i = 0; i < rounds; ++i) {
      SetStage(slot, 1);
      const auto recv = channel_a->Recv();
      if (!recv.Ok()) {
        Fail("rtt B Recv non-ok");
        break;
      }
      SetStage(slot, 2);
      const auto send = channel_b->Send(static_cast<int>(i));
      if (send.status != go2cpp::ChannelStatus::kReady) {
        Fail("rtt B Send non-ready");
      }
      done_b.fetch_add(1, std::memory_order_relaxed);
      Touch(slot);
    }
    SetStage(slot, 9);
    g_active.fetch_sub(1, std::memory_order_relaxed);
  });
  thread_a.join();
  thread_b.join();
  ClearChannels();
  if (done_a.load() != rounds || done_b.load() != rounds) {
    std::fprintf(stderr, "[ERROR] rtt count mismatch: a=%llu b=%llu\n",
                 static_cast<unsigned long long>(done_a.load()),
                 static_cast<unsigned long long>(done_b.load()));
    std::fflush(stderr);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 场景：Mutex + Channel + ParkingCondition 混合（线程版）
// ---------------------------------------------------------------------------

bool RunMixed(std::uint64_t n) {
  constexpr std::uint64_t kWorkers = 2;
  constexpr std::size_t kCapacity = 64;

  auto channel = go2cpp::MakeChannel<int>(kCapacity);
  RegisterChannel("mixed_ch", channel);

  go2cpp::sync::Mutex mutex;
  go2cpp::core::ParkingCondition condition;
  std::mutex pc_mutex;
  std::uint64_t flag = 0;
  std::uint64_t counter = 0;
  bool stop = false;
  std::atomic<std::uint64_t> received{0};

  std::vector<std::thread> threads;
  threads.reserve(2);
  g_active.fetch_add(3);  // workers + consumer；helper 不计入活跃

  const std::uint64_t per_worker = n / kWorkers;
  const std::uint64_t extra_worker = n % kWorkers;
  for (std::uint64_t w = 0; w < kWorkers; ++w) {
    const std::uint64_t count = per_worker + (w < extra_worker ? 1 : 0);
    threads.emplace_back([&, count] {
      const int slot = AllocSlot("mixed_worker");
      for (std::uint64_t i = 0; i < count; ++i) {
        SetStage(slot, 1);
        mutex.Lock();
        ++counter;
        SetStage(slot, 2);
        mutex.Unlock();
        SetStage(slot, 3);
        const auto send = channel->Send(static_cast<int>(i));
        if (send.status != go2cpp::ChannelStatus::kReady) {
          Fail("mixed Send non-ready");
        }
        SetStage(slot, 4);
        {
          std::lock_guard<std::mutex> lock(pc_mutex);
          ++flag;
        }
        SetStage(slot, 5);
        condition.notify_one();
        Touch(slot);
      }
      SetStage(slot, 9);
      g_active.fetch_sub(1, std::memory_order_relaxed);
    });
  }

  std::thread consumer([&] {
    const int slot = AllocSlot("mixed_consumer");
    for (std::uint64_t i = 0; i < n; ++i) {
      SetStage(slot, 6);
      auto result = channel->Recv();
      if (!result.Ok()) {
        Fail("mixed Recv non-ok");
        break;
      }
      received.fetch_add(1, std::memory_order_relaxed);
      SetStage(slot, 7);
      Touch(slot);
    }
    SetStage(slot, 9);
    g_active.fetch_sub(1, std::memory_order_relaxed);
  });

  std::thread helper([&] {
    const int slot = AllocSlot("mixed_helper");
    std::uint64_t seen = 0;
    for (;;) {
      SetStage(slot, 8);
      std::unique_lock<std::mutex> lock(pc_mutex);
      condition.wait(lock, [&] { return flag != seen || stop; });
      seen = flag;
      if (stop) {
        break;
      }
      Touch(slot);
    }
    SetStage(slot, 9);
  });

  for (auto& thread : threads) {
    thread.join();
  }
  consumer.join();
  {
    std::lock_guard<std::mutex> lock(pc_mutex);
    stop = true;
  }
  condition.notify_all();
  helper.join();

  ClearChannels();
  const std::uint64_t final_counter = counter;
  if (received.load() != n || final_counter != n) {
    std::fprintf(stderr,
                 "[ERROR] mixed verify failed: received=%llu counter=%llu "
                 "expected=%llu\n",
                 static_cast<unsigned long long>(received.load()),
                 static_cast<unsigned long long>(final_counter),
                 static_cast<unsigned long long>(n));
    std::fflush(stderr);
    return false;
  }
  return true;
}

struct Options {
  std::uint64_t rounds = 100;
  std::uint64_t msgs = 100000;
  std::string scenario = "all";
  int hang_seconds = 30;
  bool gdb = true;
  bool inject_hang = false;
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    auto value = [&](const char* name) -> const char* {
      const std::size_t length = std::strlen(name);
      if (std::strncmp(arg, name, length) == 0 && arg[length] == '=') {
        return arg + length + 1;
      }
      return nullptr;
    };
    if (const char* v = value("--rounds")) {
      options.rounds = std::strtoull(v, nullptr, 10);
    } else if (const char* v = value("--msgs")) {
      options.msgs = std::strtoull(v, nullptr, 10);
    } else if (const char* v = value("--scenario")) {
      options.scenario = v;
    } else if (const char* v = value("--hang-sec")) {
      options.hang_seconds = std::atoi(v);
    } else if (std::strcmp(arg, "--no-gdb") == 0) {
      options.gdb = false;
    } else if (std::strcmp(arg, "--inject-hang") == 0) {
      options.inject_hang = true;
    } else if (std::strcmp(arg, "--help") == 0) {
      std::printf(
          "usage: %s [--rounds=N] [--msgs=N] "
          "[--scenario=all|mpmc64|mpmc1|mpmc0|rtt0|mixed] "
          "[--hang-sec=N] [--no-gdb]\n",
          argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg);
      std::exit(2);
    }
  }
  if (options.rounds == 0 || options.msgs == 0) {
    std::fprintf(stderr, "rounds/msgs must be positive\n");
    std::exit(2);
  }
  return options;
}

bool ScenarioEnabled(const Options& options, const char* name) {
  return options.scenario == "all" || options.scenario == name;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  // 允许外部 gdb 在挂起时 attach（yama ptrace_scope=1 默认只允许祖先进程）。
  (void)::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

  g_rounds.store(options.rounds);
  g_hang_seconds.store(options.hang_seconds);
  g_gdb_enabled.store(options.gdb);
  std::remove("/tmp/channel_hang.marker");

  std::thread watchdog(WatchdogLoop);
  watchdog.detach();

  if (options.inject_hang) {
    // 自检注入：cap=1 通道上第二个 Send 永久阻塞（无人接收），验证
    // watchdog -> dump -> marker -> gdb 全链路。
    g_scenario.store("inject_hang");
    g_active.store(2);
    auto channel = go2cpp::MakeChannel<int>(1);
    RegisterChannel("inject_ch", channel);
    std::thread([channel] {
      const int slot = AllocSlot("producer");
      SetStage(slot, 1);
      (void)channel->Send(1);
      SetStage(slot, 9);
    }).detach();
    std::thread([channel] {
      const int slot = AllocSlot("producer");
      SetStage(slot, 1);
      (void)channel->Send(2);
      SetStage(slot, 9);
    }).detach();
    std::this_thread::sleep_for(std::chrono::hours(1));
  }

  const auto program_start = Clock::now();
  for (std::uint64_t round = 1; round <= options.rounds; ++round) {
    g_round.store(round);
    ResetSlots();
    ClearChannels();
    g_active.store(0);
    g_hang_reported.store(false);

    const auto round_start = Clock::now();
    bool ok = true;

    if (ScenarioEnabled(options, "mpmc64")) {
      g_scenario.store("mpmc64");
      const auto start = Clock::now();
      ok = RunMpmc(options.msgs, 64, 4, 4) && ok;
      std::printf("[round %llu/%llu] mpmc64 msgs=%llu %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  static_cast<unsigned long long>(options.msgs),
                  std::chrono::duration<double>(Clock::now() - start).count());
      std::fflush(stdout);
    }
    if (ok && ScenarioEnabled(options, "mpmc8_64")) {
      g_scenario.store("mpmc8_64");
      const auto start = Clock::now();
      ok = RunMpmc(options.msgs, 64, 8, 8) && ok;
      std::printf("[round %llu/%llu] mpmc8_64 msgs=%llu %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  static_cast<unsigned long long>(options.msgs),
                  std::chrono::duration<double>(Clock::now() - start).count());
      std::fflush(stdout);
    }
    if (ok && ScenarioEnabled(options, "mpmc8_0")) {
      g_scenario.store("mpmc8_0");
      const auto start = Clock::now();
      ok = RunMpmc(options.msgs, 0, 8, 8) && ok;
      std::printf("[round %llu/%llu] mpmc8_0 msgs=%llu %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  static_cast<unsigned long long>(options.msgs),
                  std::chrono::duration<double>(Clock::now() - start).count());
      std::fflush(stdout);
    }
    if (ok && ScenarioEnabled(options, "mpmc1")) {
      g_scenario.store("mpmc1");
      const auto start = Clock::now();
      ok = RunMpmc(options.msgs, 1, 4, 4) && ok;
      std::printf("[round %llu/%llu] mpmc1 msgs=%llu %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  static_cast<unsigned long long>(options.msgs),
                  std::chrono::duration<double>(Clock::now() - start).count());
      std::fflush(stdout);
    }
    if (ok && ScenarioEnabled(options, "mpmc0")) {
      g_scenario.store("mpmc0");
      const auto start = Clock::now();
      ok = RunMpmc(options.msgs, 0, 4, 4) && ok;
      std::printf("[round %llu/%llu] mpmc0 msgs=%llu %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  static_cast<unsigned long long>(options.msgs),
                  std::chrono::duration<double>(Clock::now() - start).count());
      std::fflush(stdout);
    }
    if (ok && ScenarioEnabled(options, "rtt0")) {
      g_scenario.store("rtt0");
      const auto start = Clock::now();
      ok = RunRtt(options.msgs) && ok;
      std::printf("[round %llu/%llu] rtt0 msgs=%llu %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  static_cast<unsigned long long>(options.msgs),
                  std::chrono::duration<double>(Clock::now() - start).count());
      std::fflush(stdout);
    }
    if (ok && ScenarioEnabled(options, "mixed")) {
      g_scenario.store("mixed");
      const auto start = Clock::now();
      ok = RunMixed(options.msgs) && ok;
      std::printf("[round %llu/%llu] mixed msgs=%llu %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  static_cast<unsigned long long>(options.msgs),
                  std::chrono::duration<double>(Clock::now() - start).count());
      std::fflush(stdout);
    }

    const auto round_seconds =
        std::chrono::duration<double>(Clock::now() - round_start).count();
    if (!ok || g_errors.load(std::memory_order_relaxed) != 0) {
      std::fprintf(stderr, "[FAIL] round=%llu scenario=%s errors=%llu\n",
                   static_cast<unsigned long long>(round),
                   g_scenario.load(),
                   static_cast<unsigned long long>(
                       g_errors.load(std::memory_order_relaxed)));
      std::fflush(stderr);
      g_done.store(true);
      return 1;
    }
    if (round_seconds > 5.0) {
      std::printf("[round %llu/%llu] slow round: %.3fs\n",
                  static_cast<unsigned long long>(round),
                  static_cast<unsigned long long>(options.rounds),
                  round_seconds);
      std::fflush(stdout);
    }
  }

  g_done.store(true);
  const auto total =
      std::chrono::duration<double>(Clock::now() - program_start).count();
  std::printf("[done] rounds=%llu msgs/round/scenario=%llu total=%.1fs\n",
              static_cast<unsigned long long>(options.rounds),
              static_cast<unsigned long long>(options.msgs), total);
  std::fflush(stdout);
  return 0;
}
