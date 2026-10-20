/**
 * @file deadlock_routes_server.cpp
 * @brief 真实场景：100+ HTTP client 并发访问多资源路由，锁顺序反转死锁，
 *        再通过 DebugServer / Doctor 从外部捕捉、分析与受控解锁。
 *
 * ## 场景结构
 *
 * - 服务端：`http::Server` + `Scheduler`，每个连接一个 Fiber（main 线程
 *   `accept`，`Scheduler::spawn` 处理连接）；
 * - 4 个资源：`res-a` / `res-b` / `res-c` / `res-d`，各由一个 `sync::Mutex`
 *   保护一个共享计数器；
 * - 交叉依赖：`/res-a` 先 A 后 B，`/res-b` 先 B 后 A（锁顺序反转）；
 *   `/res-d` 先 D 后 C；
 * - 100+ client：默认 120 个加载线程 + 2 个死锁触发 client = 122 个并发
 *   client；加载 client 混合 GET/POST、随机短延迟压 5 条单锁路由
 *   （/res-c、/res-d、/health、/res-a-ro、/res-b-ro）；交叉锁路由
 *   /res-a、/res-b 只由 2 个触发 client 并发访问，保证每轮时序一致；
 * - 死锁：加载阶段结束后，两个触发 client 分别请求 `/res-a?hold=1` 与
 *   `/res-b?hold=1`。handler 拿到第一把锁后在 `ParkingCondition` 上会合，
 *   双方都持锁后才去拿第二把 —— 形成 A→B / B→A 的确定性死锁；
 * - 追加卡死：再发 4 条 `/res-a-ro` 与 4 条 `/res-b-ro`（只拿 A 或只拿 B），
 *   它们会排在两把锁的等待队列里，因此「卡住数」= 2 + 8 = 10；
 * - 观测：Doctor 的 `deadlock-report` 判定全局无进展；`dump` 组件
 *   `scheduler` / `fiber` / `mutex` / `events` 给出 gid、状态、owner、waiters。
 *
 * ## 受控强制解锁的协作约定（重要）
 *
 * `mutex-unlock --force` 会直接 `Mutex::Unlock()`，被剥夺所有权的 Fiber 并
 * 不知道这件事。若它稍后仍然 `Unlock()`，轻则抛 `logic_error`，重则把锁从
 * 新持有者手里抢走。因此本场景约定：
 *
 * 1. 每个 handler 在拿到第一把锁时记录 `g_epoch`；
 * 2. 监控线程轮询 Doctor 的 `stats().forced_unlocks`，一旦增加就递增
 *    `g_epoch`；
 * 3. handler 释放锁前比较 epoch：变了就**跳过**释放（记为 stale-skip）。
 *
 * 这样 force 解锁后：被抢锁的 G1 只释放它后来合法拿到的 B，不会再碰 A；
 * 等待队列正常流动，10 个卡住请求全部完成。**这不能推广为通用做法**：
 * 强制解锁本身仍然是破坏互斥语义的 demo 级操作。
 *
 * ## 用法
 *
 * ```sh
 * ./go2cpp_deadlock_routes --port 18080 --clients 120 --load-ms 1200
 * # 或由 tests/scenarios/deadlock_routes/run_deadlock_scenario.sh 驱动
 * ```
 *
 * 进程 stdout 输出机器可读的阶段行（READY / LOAD_DONE / DEADLOCK_READY /
 * QUIESCENT / RECOVERED / FINAL ...），外部脚本据此执行 Doctor 命令。
 */

#include "go2cpp/concurrency/core/parking_condition.hpp"
#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/debug_server.hpp"
#include "go2cpp/debug/diagnostic_agent.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/network/address.hpp"
#include "go2cpp/network/http.hpp"
#include "go2cpp/network/socket.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace go2cpp;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// 资源：4 把锁 + 各自保护的共享计数器（全部由对应锁保护，禁止无锁读写）
// ---------------------------------------------------------------------------

sync::Mutex g_mutex_a;
sync::Mutex g_mutex_b;
sync::Mutex g_mutex_c;
sync::Mutex g_mutex_d;

/// 临界区里被累加的共享计数器；读写在对应 Mutex 内完成。
std::uint64_t g_counter_a = 0;
std::uint64_t g_counter_b = 0;
std::uint64_t g_counter_c = 0;
std::uint64_t g_counter_d = 0;

// ---------------------------------------------------------------------------
// 场景控制状态
// ---------------------------------------------------------------------------

/// force 解锁代际（仅用于日志展示；判断是否仍拥有锁直接读 Doctor 计数）。
/// handler 在拿锁后与释放前各采样一次 `DiagnosticAgentStats::forced_unlocks`：
/// 两次相等才认为本 G 仍合法拥有这把锁。
std::atomic<std::uint64_t> g_epoch{0};
/// 已观察到的 Doctor 强制解锁次数（来自 DiagnosticAgentStats）。
std::atomic<std::uint64_t> g_forced_unlocks_seen{0};
/// 因 epoch 变化而主动跳过释放的锁次数（受控恢复的审计计数）。
std::atomic<std::uint64_t> g_stale_unlock_skips{0};
/// 释放时撞上 `unlock of unlocked mutex` 并捕获的次数（兜底观测）。
std::atomic<std::uint64_t> g_unlock_exceptions{0};

/// 加载 client 是否还允许请求交叉锁路由（/res-a、/res-b）。
/// 本场景**不让加载 client 走交叉锁路由**：加载期只要出现一次 A→B / B→A
/// 的真实交错，就会在计划外提前死锁，让「触发率」变成随机事件。交叉锁路由
/// 由 2 个触发 client 专门并发访问（阶段 3），加载 client 压另外 5 条路由。
/// 这样每一轮的时序一致，触发率可重复测量。
/// 停止所有加载 client。
std::atomic<bool> g_stop_load{false};
/// 停止监控线程。
std::atomic<bool> g_stop_monitor{false};
/// 正在执行任意请求的加载 client 数；用于静默判定。
std::atomic<int> g_active_load{0};

/// 触发 client 是否放行。
std::atomic<bool> g_trigger_go{false};
/// 已发出请求的触发 client 数。
std::atomic<int> g_trigger_started{0};
/// 已完成的触发 client 数。
std::atomic<int> g_trigger_done{0};
/// 已完成的追加卡死 client 数。
std::atomic<int> g_extra_done{0};
/// 两个触发 handler 各自的 gid（拿到第一把锁后写入）。
std::atomic<std::uint64_t> g_trigger_gid_a{0};
std::atomic<std::uint64_t> g_trigger_gid_b{0};

// ---------------------------------------------------------------------------
// 会合点：两个触发 handler 拿到第一把锁后在此同步，确保交错确定发生
// ---------------------------------------------------------------------------

std::mutex g_rendezvous_mutex;
core::ParkingCondition g_rendezvous;
std::atomic<int> g_first_locked{0};

// ---------------------------------------------------------------------------
// 每个 handler Fiber 的观测注解：由 handler 自己写入，供 dump 组件 "fiber" 读取。
// key = gid；handler 退出时删除，dump 只读快照。
// ---------------------------------------------------------------------------

struct FiberMeta {
    std::string role;        ///< 路由角色：res-a / res-b / res-a-ro / ...
    std::string waiting_on;  ///< 正在等待的 Mutex 名；空表示不在锁等待
    std::string held;        ///< 当前持有的 Mutex 名（逗号分隔）
    bool trigger{false};     ///< 是否是死锁触发请求
};

std::mutex g_meta_mutex;
std::map<std::uint64_t, FiberMeta> g_meta;

/// 服务端调度器指针；dump provider 只读使用（生命周期同进程）。
Scheduler* g_scheduler = nullptr;

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------

/// 逐路由计数（仅加载 client；触发/追加 client 单独统计）。
struct RouteCounter {
    std::atomic<std::uint64_t> ok{0};
    std::atomic<std::uint64_t> failed{0};
};
RouteCounter g_route_ok[7];
RouteCounter g_route_failed[7];
const char* const kRouteNames[7] = {"/res-a", "/res-b", "/res-c",
                                    "/res-d", "/res-a-ro", "/res-b-ro",
                                    "/health"};

std::atomic<std::uint64_t> g_load_total{0};
std::atomic<std::uint64_t> g_load_ok{0};
std::atomic<std::uint64_t> g_load_failed{0};

/// 客户端错误分类计数（`http::Result::Error`，0..9）；用于如实报告失败原因。
constexpr int kErrorKinds = 10;
const char* const kErrorNames[kErrorKinds] = {
    "none", "invalid_url", "resolve", "connect", "timeout",
    "send", "receive",     "parse",   "closed",  "cancelled"};
std::atomic<std::uint64_t> g_error_counts[kErrorKinds];
std::mutex g_error_mutex;
std::string g_error_samples[kErrorKinds];

void RecordClientError(int code, const std::string& message) {
    if (code <= 0 || code >= kErrorKinds) {
        return;
    }
    g_error_counts[code].fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_error_mutex);
    if (g_error_samples[code].empty()) {
        g_error_samples[code] = message;
    }
}

std::mutex g_latency_mutex;
std::vector<double> g_latency_ms;

/// 进程退出前的收尾：先移除 socket 文件，再 `_Exit`，避免析构顺序问题。
void ExitNow(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

std::uint64_t CurrentGid() {
    Task* task = Scheduler::current_task_ptr();
    return task == nullptr ? 0U : static_cast<std::uint64_t>(task->id());
}

/// 直接读取 Doctor 的「已执行强制解锁次数」。
/// 用同步计数而不是轮询派生的 epoch：Doctor 在 `Unlock()` **之前**自增，
/// 被唤醒的等待者可能在几微秒内就跑完退出，任何轮询线程都来不及观察到。
std::uint64_t ForcedUnlockCount() {
    return debug::DiagnosticAgent::Instance().stats().forced_unlocks;
}

/**
 * @brief 判断 handler 是否仍合法拥有 `mutex`，可以安全 `Unlock()`。
 *
 * 只看全局强制解锁计数是不够的：一次 force 会让**所有**在此之后释放的
 * handler 都跳过释放，包括从未被夺走的那把锁（例如 B→A 请求里的 B），
 * 结果是把锁永久泄漏。因此：
 *
 * - 计数没变：正常释放；
 * - 计数变了：只有「锁仍 held 且 owner 就是自己」才继续释放；owner 变成
 *   别人或锁已 free，说明本 G 持有的那把锁被 Doctor 夺走了，跳过。
 *
 * 该函数只在计数变化后调用一次快照，不在热路径上。
 */
bool StillOwnsLock(sync::Mutex& mutex, std::uint64_t gid,
                   std::uint64_t forced_at_acquire) {
    if (ForcedUnlockCount() == forced_at_acquire) {
        return true;
    }
    const auto snapshot = mutex.DebugSnapshotInternal();
    if (!snapshot.held) {
        return false;
    }
    if (!snapshot.owner_known) {
        // owner 不可知时保守处理：按仍持有对待（本场景已开启 owner 记录，
        // 正常不会走到这里）。
        return true;
    }
    return snapshot.owner_gid == gid;
}

// ---------------------------------------------------------------------------
// FiberMeta 读写
// ---------------------------------------------------------------------------

void MetaInsert(std::uint64_t gid, FiberMeta meta) {
    if (gid == 0U) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    g_meta[gid] = std::move(meta);
}

void MetaSetWaiting(std::uint64_t gid, std::string waiting_on,
                    std::string held) {
    if (gid == 0U) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    const auto found = g_meta.find(gid);
    if (found == g_meta.end()) {
        return;
    }
    found->second.waiting_on = std::move(waiting_on);
    found->second.held = std::move(held);
}

void MetaErase(std::uint64_t gid) {
    if (gid == 0U) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    g_meta.erase(gid);
}

/// 以 `ParkingCondition` 做 Fiber 感知的睡眠：park 当前 G，不占住 worker。
void FiberSleepMs(int milliseconds) {
    std::mutex mutex;
    core::ParkingCondition condition;
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, std::chrono::milliseconds(milliseconds),
                       [] { return false; });
}

/**
 * @brief 两个触发 handler 的会合：等到两把「第一锁」都被持有。
 *
 * 用 `ParkingCondition` 而不是自旋/线程 sleep：即使两个 Fiber 落在同一个
 * P 上，先到者也会 park，让出 worker 给后者，最终双方都能到齐。
 *
 * @return 双方都到达返回 true；超时返回 false（触发率观测的一部分）。
 */
bool RendezvousFirstLocks() {
    std::unique_lock<std::mutex> lock(g_rendezvous_mutex);
    const int arrived = g_first_locked.fetch_add(1) + 1;
    g_rendezvous.notify_all();
    if (arrived >= 2) {
        return true;
    }
    return g_rendezvous.wait_for(lock, 400ms, [] {
        return g_first_locked.load(std::memory_order_acquire) >= 2;
    });
}

// ---------------------------------------------------------------------------
// HTTP 帮助函数
// ---------------------------------------------------------------------------

http::Response TextResponse(http::Status status, std::string body) {
    http::Response response;
    response.status = status;
    response.header.set("Content-Type", "text/plain");
    response.body = std::move(body);
    response.close = true;  // 每个连接只服务一个请求，简化 Fiber 生命周期
    return response;
}

http::Response OkResponse(const std::string& route, std::uint64_t gid) {
    return TextResponse(http::Status::OK,
                        "route=" + route + " gid=" + std::to_string(gid) +
                            " ok=1\n");
}

/// 模拟临界区工作：在持锁状态下累加共享计数器。
void BurnInsideLock(std::uint64_t* counter, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        *counter += 1;
    }
}

/**
 * @brief 交叉锁 handler：`/res-a`（A→B）与 `/res-b`（B→A）。
 *
 * 步骤：拿第一把锁 →（触发请求会合 + 30ms 窗口）→ 拿第二把锁 → 工作 →
 * 释放第二把 → 按 epoch 规则释放第一把。
 */
http::Response HandleCrossLock(const http::Request& request, bool a_first) {
    const std::uint64_t gid = CurrentGid();
    const bool trigger = request.query().find("hold=1") != std::string::npos;
    const char* first_name = a_first ? "res-a" : "res-b";
    const char* second_name = a_first ? "res-b" : "res-a";
    const std::string route = first_name;

    // 只有触发请求维护观测注解：普通加载请求在两个 Lock 之间不能碰任何
    // 全局互斥量（g_meta_mutex），否则压测期会被这把全局锁人为拉宽锁顺序
    // 窗口，制造「计划外」的自然死锁。
    if (trigger) {
        FiberMeta meta;
        meta.role = route;
        meta.trigger = true;
        MetaInsert(gid, meta);
    }
    const auto note_wait = [&](const char* waiting, const char* held) {
        if (trigger) {
            MetaSetWaiting(gid, waiting, held);
        }
    };

    sync::Mutex& first = a_first ? g_mutex_a : g_mutex_b;
    sync::Mutex& second = a_first ? g_mutex_b : g_mutex_a;
    std::uint64_t* first_counter = a_first ? &g_counter_a : &g_counter_b;
    std::uint64_t* second_counter = a_first ? &g_counter_b : &g_counter_a;

    note_wait(first_name, "");
    if (!first.Lock()) {
        if (trigger) {
            MetaErase(gid);
        }
        return TextResponse(http::Status::ServiceUnavailable, "lock-failed\n");
    }
    if (trigger) {
        if (a_first) {
            g_trigger_gid_a.store(gid, std::memory_order_release);
        } else {
            g_trigger_gid_b.store(gid, std::memory_order_release);
        }
    }
    // 关键：拿锁之后再采样强制解锁计数。此后若计数变化，说明本锁可能已被
    // Doctor 强制夺走，释放时必须跳过。**只对触发请求采样**：普通加载请求
    // 在这个窗口里不能做任何额外工作（连一次 stats() 都不行），否则会人为
    // 拉宽锁顺序反转窗口，在压测阶段制造计划外的自然死锁。
    std::uint64_t forced_at_first = 0;
    if (trigger) {
        forced_at_first = ForcedUnlockCount();
    }
    note_wait("", first_name);

    if (trigger) {
        // 两个 client 的请求在服务端会合：都持第一把锁后，才一起拿第二把。
        const bool rend = RendezvousFirstLocks();
        std::fprintf(stderr,
                     "[scenario] trigger gid=%llu first=%s rendezvous=%d "
                     "-> second lock %s\n",
                     static_cast<unsigned long long>(gid), first_name,
                     rend ? 1 : 0, second_name);
        // 把交错窗口再拉宽一点，确保双方都进入第二次 Lock 的等待队列。
        FiberSleepMs(30);
    }

    note_wait(second_name, first_name);
    if (!second.Lock()) {
        note_wait("", first_name);
        if (!trigger || StillOwnsLock(first, gid, forced_at_first)) {
            first.Unlock();
        }
        if (trigger) {
            MetaErase(gid);
        }
        return TextResponse(http::Status::ServiceUnavailable, "lock-failed\n");
    }
    if (trigger) {
        note_wait("", (std::string(first_name) + "," + second_name).c_str());
    }

    // 临界区：两把锁都持有，累加两个共享计数器。
    BurnInsideLock(first_counter, 64);
    BurnInsideLock(second_counter, 64);

    second.Unlock();
    const bool still_ours = !trigger || StillOwnsLock(first, gid, forced_at_first);
    if (still_ours) {
        try {
            first.Unlock();
        } catch (const std::exception& exception) {
            // 兜底：极窄的竞态窗口内（force 计数已可见、锁已被等者释放）会
            // 走到这里。锁本身已经是释放状态，跳过即可，不能把 500 抛给 client。
            g_unlock_exceptions.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[scenario] unlock-exception gid=%llu mutex=%s "
                         "what=%s\n",
                         static_cast<unsigned long long>(gid), first_name,
                         exception.what());
        }
    } else {
        // 受控恢复：本 G 的第一把锁已被 force-unlock 夺走，不能再释放，
        // 否则会把锁从新的合法持有者手里抢走。
        g_stale_unlock_skips.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[scenario] stale-unlock-skip gid=%llu mutex=%s "
                     "forced_at_lock=%llu forced_now=%llu\n",                     static_cast<unsigned long long>(gid), first_name,
                     static_cast<unsigned long long>(forced_at_first),
                     static_cast<unsigned long long>(ForcedUnlockCount()));
    }
    if (trigger) {
        MetaErase(gid);
    }
    return OkResponse(route, gid);
}

/// 单锁 handler：只拿一把锁（`/res-c`、`/res-a-ro`、`/res-b-ro`）。
/// `track` 为真时才维护 dump 注解（只给少量卡死观测请求用，热路径保持零全局锁）。
http::Response HandleSingleLock(const http::Request& request,
                                const std::string& route, sync::Mutex& mutex,
                                std::uint64_t* counter, bool track) {
    (void)request;
    const std::uint64_t gid = CurrentGid();
    if (track) {
        FiberMeta meta;
        meta.role = route;
        MetaInsert(gid, meta);
        MetaSetWaiting(gid, route, "");
    }
    if (!mutex.Lock()) {
        if (track) {
            MetaErase(gid);
        }
        return TextResponse(http::Status::ServiceUnavailable, "lock-failed\n");
    }
    if (track) {
        MetaSetWaiting(gid, "", route);
    }
    BurnInsideLock(counter, 64);
    mutex.Unlock();
    if (track) {
        MetaErase(gid);
    }
    return OkResponse(route, gid);
}

/// 双锁 handler：先 D 后 C（`/res-d`）。热路径，不维护 dump 注解。
http::Response HandleDLockC(const http::Request& request) {
    (void)request;
    const std::uint64_t gid = CurrentGid();
    if (!g_mutex_d.Lock()) {
        return TextResponse(http::Status::ServiceUnavailable, "lock-failed\n");
    }
    BurnInsideLock(&g_counter_d, 64);
    if (!g_mutex_c.Lock()) {
        g_mutex_d.Unlock();
        return TextResponse(http::Status::ServiceUnavailable, "lock-failed\n");
    }
    BurnInsideLock(&g_counter_c, 64);
    g_mutex_c.Unlock();
    g_mutex_d.Unlock();
    return OkResponse("/res-d", gid);
}

http::Response HandleHealth(const http::Request& request) {
    (void)request;
    return TextResponse(http::Status::OK, "ok\n");
}

http::Response HandleNotFound(const http::Request& request) {
    return TextResponse(http::Status::NotFound, "not-found:" + request.path());
}

// ---------------------------------------------------------------------------
// dump 组件：fiber / mutex
// ---------------------------------------------------------------------------

debug::DumpPayload DumpScenarioFibers() {
    std::map<std::uint64_t, FiberMeta> meta;
    {
        std::lock_guard<std::mutex> lock(g_meta_mutex);
        meta = g_meta;
    }
    std::map<std::uint64_t, std::string> states;
    if (g_scheduler != nullptr) {
        for (const auto& task : g_scheduler->DebugSnapshotTasks()) {
            states[static_cast<std::uint64_t>(task.id)] =
                debug::GStateNameStable(task.state);
        }
    }
    debug::TextDump text;
    text.Head("go2cpp.scenario.fiber/1");
    text.Key("tracked", static_cast<std::uint64_t>(meta.size()));
    std::size_t index = 0;
    for (const auto& entry : meta) {
        const std::string prefix = "fiber." + std::to_string(index++) + ".";
        text.Key(prefix + "gid", static_cast<std::uint64_t>(entry.first));
        text.Key(prefix + "role", entry.second.role);
        text.Key(prefix + "trigger", entry.second.trigger);
        const auto state = states.find(entry.first);
        text.Key(prefix + "state",
                 state == states.end() ? std::string("gone") : state->second);
        text.Key(prefix + "waiting_on", entry.second.waiting_on);
        text.Key(prefix + "held", entry.second.held);
    }
    debug::DumpPayload payload;
    payload.text = text.Take();
    payload.json = "{\"schema\":\"go2cpp.scenario.fiber/1\",\"version\":1,"
                   "\"tracked\":" +
                   std::to_string(meta.size()) + "}";
    return payload;
}

debug::DumpPayload DumpScenarioMutexes() {
    const std::pair<const char*, sync::Mutex*> mutexes[] = {
        {"res-a", &g_mutex_a},
        {"res-b", &g_mutex_b},
        {"res-c", &g_mutex_c},
        {"res-d", &g_mutex_d},
    };
    debug::TextDump text;
    text.Head("go2cpp.scenario.mutex/1");
    text.Key("mutex_count",
             static_cast<std::uint64_t>(sizeof(mutexes) / sizeof(mutexes[0])));
    for (const auto& entry : mutexes) {
        const auto snapshot = entry.second->DebugSnapshotInternal();
        const std::string prefix = std::string("mutex.") + entry.first + ".";
        text.Key(prefix + "held", snapshot.held);
        text.Key(prefix + "waiters",
                 static_cast<std::uint64_t>(snapshot.waiters));
        text.Key(prefix + "fiber_waiters",
                 static_cast<std::uint64_t>(snapshot.fiber_waiters));
        text.Key(prefix + "native_waiters",
                 static_cast<std::uint64_t>(snapshot.native_waiters));
        text.Key(prefix + "starving", snapshot.starving);
        text.Key(prefix + "owner_gid",
                 static_cast<std::uint64_t>(snapshot.owner_gid));
        text.Key(prefix + "owner_known", snapshot.owner_known);
        text.KeyHex(prefix + "state_bits",
                    static_cast<std::uint64_t>(snapshot.state_bits));
    }
    debug::DumpPayload payload;
    payload.text = text.Take();
    payload.json = "{\"schema\":\"go2cpp.scenario.mutex/1\",\"version\":1}";
    return payload;
}

// ---------------------------------------------------------------------------
// 监控线程：把 Doctor 的强制解锁映射为本进程的 epoch
// ---------------------------------------------------------------------------

void MonitorThread() {
    while (!g_stop_monitor.load(std::memory_order_acquire)) {
        const auto stats = debug::DiagnosticAgent::Instance().stats();
        const std::uint64_t forced = stats.forced_unlocks;
        if (forced != g_forced_unlocks_seen.load(std::memory_order_acquire)) {
            g_forced_unlocks_seen.store(forced, std::memory_order_release);
            g_epoch.fetch_add(1, std::memory_order_acq_rel);
            std::fprintf(stderr,
                         "[scenario] force-unlock observed forced_total=%llu "
                         "epoch=%llu\n",
                         static_cast<unsigned long long>(forced),
                         static_cast<unsigned long long>(
                             g_epoch.load(std::memory_order_acquire)));
        }
        std::this_thread::sleep_for(2ms);
    }
}

// ---------------------------------------------------------------------------
// 加载 client
// ---------------------------------------------------------------------------

struct ClientConfig {
    int clients{120};
    int load_ms{1200};
    int extra_waiters{4};
    int port{18080};
};

struct RequestOutcome {
    bool ok{false};
    long status{0};
    double latency_ms{0.0};
};

RequestOutcome SendRequest(const std::string& base, const std::string& route,
                           bool post, int slot,
                           std::chrono::milliseconds timeout = 60s) {
    const std::string url = base + route;
    const auto start = std::chrono::steady_clock::now();
    http::Result result;
    if (post) {
        result = http::Post(url, "{\"slot\":" + std::to_string(slot) + "}",
                            "application/json", timeout);
    } else {
        result = http::Get(url, timeout);
    }
    const auto end = std::chrono::steady_clock::now();
    RequestOutcome outcome;
    outcome.latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    if (result.ok() && result.response) {
        outcome.status = static_cast<long>(result.response->status);
        outcome.ok = outcome.status == 200;
    } else {
        outcome.status = -static_cast<long>(result.error);
        RecordClientError(static_cast<int>(result.error), result.message);
    }
    return outcome;
}

void RecordLoadOutcome(int route_index, const RequestOutcome& outcome) {
    g_load_total.fetch_add(1, std::memory_order_relaxed);
    if (outcome.ok) {
        g_load_ok.fetch_add(1, std::memory_order_relaxed);
        g_route_ok[route_index].ok.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_load_failed.fetch_add(1, std::memory_order_relaxed);
        g_route_failed[route_index].failed.fetch_add(1,
                                                    std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(g_latency_mutex);
    g_latency_ms.push_back(outcome.latency_ms);
}

/// 选择要压的路由下标。加载期只压 /res-c、/res-d、/health、/res-a-ro、
/// /res-b-ro 五条路由；交叉锁路由 /res-a、/res-b 留给触发 client。
int PickRoute(std::mt19937& random) {
    std::uniform_int_distribution<int> distribution(0, 99);
    const int roll = distribution(random);
    if (roll < 28) {
        return 2;  // /res-c
    }
    if (roll < 56) {
        return 3;  // /res-d
    }
    if (roll < 66) {
        return 6;  // /health
    }
    if (roll < 84) {
        return 4;  // /res-a-ro（只拿 A）
    }
    return 5;  // /res-b-ro（只拿 B）
}

void LoadWorker(int index, std::string base) {
    std::mt19937 random(static_cast<std::uint32_t>(index) * 7919U + 17U);
    std::uniform_int_distribution<int> post_roll(0, 99);
    // 每个 client 两请求之间停顿 20~60ms：维持 100+ 并发 client 的同时把
    // 建连速率压到安全区间。`http::Client` 每个请求都新建连接（Connection:
    // close），连接进入 TIME_WAIT；若不限速，数万连接会在 1s 内耗尽本机
    // 临时端口，成功率会被 connect 失败拉低 —— 那是压测参数问题，不是被测
    // 死锁场景的结论。
    std::uniform_int_distribution<int> pause_us(20000, 60000);
    while (!g_stop_load.load(std::memory_order_acquire)) {
        const int route = PickRoute(random);
        const bool post = post_roll(random) < 35;  // GET/POST 混合
        g_active_load.fetch_add(1, std::memory_order_acq_rel);
        // 与「停止加载」竞态：先入计数再复查停止标志，保证 orchestrator
        // 看到 active_load==0 之后不可能再发出新请求（否则 join 会等一个
        // 已经卡在死锁上的加载线程）。
        if (g_stop_load.load(std::memory_order_acquire)) {
            g_active_load.fetch_sub(1, std::memory_order_acq_rel);
            break;
        }
        const RequestOutcome outcome =
            SendRequest(base, kRouteNames[route], post, index, 1000ms);
        g_active_load.fetch_sub(1, std::memory_order_acq_rel);
        RecordLoadOutcome(route, outcome);
        std::this_thread::sleep_for(
            std::chrono::microseconds(pause_us(random)));
    }
}

/// 死锁触发 client：等放行后请求 `/res-a?hold=1` 或 `/res-b?hold=1`。
/// 建连失败时重试：`http::Client` 在短连接风暴下偶发 connect 失败（见
/// FINAL_ERRORS），触发请求必须到达服务端，否则这一轮就构造不出死锁。
void TriggerWorker(int which, std::string base) {
    while (!g_trigger_go.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
    }
    g_trigger_started.fetch_add(1, std::memory_order_acq_rel);
    const std::string route = which == 0 ? "/res-a?hold=1" : "/res-b?hold=1";
    RequestOutcome outcome;
    for (int attempt = 0; attempt < 8; ++attempt) {
        outcome = SendRequest(base, route, false, 900 + which);
        if (outcome.status > 0) {
            break;  // 拿到 HTTP 状态码即可（死锁请求会一直等到恢复）
        }
        std::this_thread::sleep_for(5ms);
    }
    std::fprintf(stderr,
                 "[scenario] trigger-%d returned ok=%d status=%ld "
                 "latency_ms=%.1f\n",
                 which, outcome.ok ? 1 : 0, outcome.status,
                 outcome.latency_ms);
    g_trigger_done.fetch_add(1, std::memory_order_acq_rel);
}

/// 追加卡死 client：只请求一把锁的只读路由，用来把等待队列填出可观测长度。
void ExtraWaiterWorker(int which, std::string base) {
    const std::string route = which % 2 == 0 ? "/res-a-ro" : "/res-b-ro";
    const RequestOutcome outcome = SendRequest(base, route, false, 2000 + which);
    std::fprintf(stderr,
                 "[scenario] extra-%d route=%s returned ok=%d status=%ld "
                 "latency_ms=%.1f\n",
                 which, route.c_str(), outcome.ok ? 1 : 0, outcome.status,
                 outcome.latency_ms);
    g_extra_done.fetch_add(1, std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// 统计输出
// ---------------------------------------------------------------------------

struct LatencySummary {
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double max{0.0};
    std::size_t count{0};
};

LatencySummary SummarizeLatency() {
    std::vector<double> samples;
    {
        std::lock_guard<std::mutex> lock(g_latency_mutex);
        samples = g_latency_ms;
    }
    LatencySummary summary;
    summary.count = samples.size();
    if (samples.empty()) {
        return summary;
    }
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double ratio) {
        if (samples.empty()) {
            return 0.0;
        }
        std::size_t index = static_cast<std::size_t>(
            ratio * static_cast<double>(samples.size() - 1));
        return samples[index];
    };
    summary.p50 = at(0.50);
    summary.p95 = at(0.95);
    summary.p99 = at(0.99);
    summary.max = samples.back();
    return summary;
}

void PrintLoadSummary(const char* tag, double elapsed_seconds) {
    const auto summary = SummarizeLatency();
    const std::uint64_t total = g_load_total.load(std::memory_order_relaxed);
    const std::uint64_t ok = g_load_ok.load(std::memory_order_relaxed);
    const std::uint64_t failed = g_load_failed.load(std::memory_order_relaxed);
    const double qps = elapsed_seconds > 0.0
                           ? static_cast<double>(total) / elapsed_seconds
                           : 0.0;
    const double success_rate =
        total == 0U ? 0.0
                    : 100.0 * static_cast<double>(ok) /
                          static_cast<double>(total);
    std::printf(
        "%s load_requests=%llu ok=%llu failed=%llu success_rate=%.2f%% "
        "qps=%.1f p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f max_ms=%.2f\n",
        tag, static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(ok),
        static_cast<unsigned long long>(failed), success_rate, qps,
        summary.p50, summary.p95, summary.p99, summary.max);
    std::fflush(stdout);
}

void PrintClientErrors(const char* tag) {
    std::printf("%s", tag);
    for (int i = 1; i < kErrorKinds; ++i) {
        std::printf(" %s=%llu", kErrorNames[i],
                    static_cast<unsigned long long>(
                        g_error_counts[i].load(std::memory_order_relaxed)));
    }
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        for (int i = 1; i < kErrorKinds; ++i) {
            if (!g_error_samples[i].empty()) {
                std::printf(" sample_%s=\"%s\"", kErrorNames[i],
                            g_error_samples[i].c_str());
            }
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

void PrintRouteSummary(const char* tag) {
    std::printf("%s", tag);
    for (int i = 0; i < 7; ++i) {
        std::printf(" %s=%llu/%llu", kRouteNames[i],
                    static_cast<unsigned long long>(
                        g_route_ok[i].ok.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        g_route_ok[i].ok.load(std::memory_order_relaxed) +
                        g_route_failed[i].failed.load(
                            std::memory_order_relaxed)));
    }
    std::printf("\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 编排线程
// ---------------------------------------------------------------------------

int RunOrchestrator(const ClientConfig& config, const std::string& base_url,
                    std::vector<std::thread>* load_threads,
                    std::vector<std::thread>* trigger_threads) {
    const auto started = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(150ms);

    // ---- 阶段 0：串行预热 4 条资源路由 ----
    // 编排线程**顺序**发送 4 条请求：此刻不可能有两个交叉锁请求同时在途，
    // 因此不会提前触发死锁，只验证 /res-a、/res-b 的交叉锁路径可用。
    {
        const RequestOutcome a_get = SendRequest(base_url, "/res-a", false, 0);
        const RequestOutcome a_post = SendRequest(base_url, "/res-a", true, 0);
        const RequestOutcome b_get = SendRequest(base_url, "/res-b", false, 0);
        const RequestOutcome b_post = SendRequest(base_url, "/res-b", true, 0);
        std::printf(
            "WARMUP res-a.get=%ld res-a.post=%ld res-b.get=%ld res-b.post=%ld\n",
            a_get.status, a_post.status, b_get.status, b_post.status);
        std::fflush(stdout);
    }

    // ---- 阶段 1：100+ client 并发压测（交叉锁路由留给触发 client）----
    std::printf("LOAD_START clients=%d load_ms=%d\n", config.clients + 2,
                config.load_ms);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(config.load_ms));
    const double load_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    PrintLoadSummary("LOAD_DONE", load_seconds);
    PrintClientErrors("LOAD_ERRORS");

    // ---- 阶段 2：停掉加载 client 并 join；触发前进程必须静默 ----
    // 必须**先停流量再构造死锁**：若加载请求仍在跑，它们会不断堆到 A/B 的
    // 等待队列里，join 就可能等一个已经卡在死锁上的加载线程（旧版本故障）。
    g_stop_load.store(true, std::memory_order_release);
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline &&
               g_active_load.load(std::memory_order_acquire) != 0) {
            std::this_thread::sleep_for(5ms);
        }
    }
    for (auto& thread : *load_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    std::printf("LOAD_QUIESCENT active_load=%d\n",
                g_active_load.load(std::memory_order_acquire));
    std::fflush(stdout);

    // ---- 阶段 3：触发两个 client 的死锁 ----
    g_trigger_go.store(true, std::memory_order_release);
    bool deadlock = false;
    std::uint64_t gid_a = 0;
    std::uint64_t gid_b = 0;
    std::size_t waiters_a = 0;
    std::size_t waiters_b = 0;
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto a = g_mutex_a.DebugSnapshotInternal();
            const auto b = g_mutex_b.DebugSnapshotInternal();
            gid_a = g_trigger_gid_a.load(std::memory_order_acquire);
            gid_b = g_trigger_gid_b.load(std::memory_order_acquire);
            if (gid_a != 0U && gid_b != 0U && a.held && b.held &&
                a.waiters >= 1U && b.waiters >= 1U &&
                a.owner_gid == gid_a && b.owner_gid == gid_b) {
                deadlock = true;
                waiters_a = a.waiters;
                waiters_b = b.waiters;
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
    }
    if (!deadlock) {
        std::printf("TRIGGER_FAILED gid_a=%llu gid_b=%llu\n",
                    static_cast<unsigned long long>(gid_a),
                    static_cast<unsigned long long>(gid_b));
        std::fflush(stdout);
        return 2;
    }
    std::printf(
        "DEADLOCK_READY gid_a=%llu gid_b=%llu waiters_a=%zu waiters_b=%zu "
        "trigger_started=%d\n",
        static_cast<unsigned long long>(gid_a),
        static_cast<unsigned long long>(gid_b), waiters_a, waiters_b,
        g_trigger_started.load(std::memory_order_acquire));
    std::fflush(stdout);

    // ---- 阶段 4：追加 8 个只拿单把锁的 client，全部排队卡住 ----
    std::vector<std::thread> extra_threads;
    const int extras_started = config.extra_waiters * 2;
    for (int i = 0; i < extras_started; ++i) {
        extra_threads.emplace_back(ExtraWaiterWorker, i, base_url);
    }
    std::this_thread::sleep_for(300ms);
    const int stuck_before =
        (2 - g_trigger_done.load(std::memory_order_acquire)) +
        (extras_started - g_extra_done.load(std::memory_order_acquire));
    std::printf(
        "STUCK_PROBE stuck=%d trigger_done=%d extra_done=%d waiters_a=%zu "
        "waiters_b=%zu\n",
        stuck_before, g_trigger_done.load(std::memory_order_acquire),
        g_extra_done.load(std::memory_order_acquire),
        g_mutex_a.DebugSnapshotInternal().waiters,
        g_mutex_b.DebugSnapshotInternal().waiters);
    std::fflush(stdout);

    // ---- 阶段 5：加载线程已在阶段 2 退出；此刻只剩被卡住的 10 个请求 ----
    std::printf(
        "QUIESCENT stuck=%d active_load=%d waiters_a=%zu waiters_b=%zu\n",
        (2 - g_trigger_done.load(std::memory_order_acquire)) +
            (extras_started - g_extra_done.load(std::memory_order_acquire)),
        g_active_load.load(std::memory_order_acquire),
        g_mutex_a.DebugSnapshotInternal().waiters,
        g_mutex_b.DebugSnapshotInternal().waiters);
    std::fflush(stdout);

    // ---- 阶段 6：等待外部 Doctor 强制解锁，所有卡住请求恢复 ----
    bool recovered = false;
    {
        const auto deadline = std::chrono::steady_clock::now() + 60s;
        while (std::chrono::steady_clock::now() < deadline) {
            const int done = g_trigger_done.load(std::memory_order_acquire) +
                             g_extra_done.load(std::memory_order_acquire);
            if (done >= 2 + extras_started &&
                g_forced_unlocks_seen.load(std::memory_order_acquire) > 0U) {
                recovered = true;
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
    }
    if (!recovered) {
        std::printf(
            "RECOVERY_FAILED trigger_done=%d extra_done=%d forced=%llu "
            "waiters_a=%zu waiters_b=%zu\n",
            g_trigger_done.load(std::memory_order_acquire),
            g_extra_done.load(std::memory_order_acquire),
            static_cast<unsigned long long>(
                g_forced_unlocks_seen.load(std::memory_order_acquire)),
            g_mutex_a.DebugSnapshotInternal().waiters,
            g_mutex_b.DebugSnapshotInternal().waiters);
        std::fflush(stdout);
        return 3;
    }

    for (auto& thread : extra_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    for (auto& thread : *trigger_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    const auto finished = std::chrono::steady_clock::now();
    std::printf(
        "RECOVERED stuck_after=0 trigger_done=%d extra_done=%d "
        "stuck_before=%d forced_unlocks=%llu stale_unlock_skips=%llu "
        "unlock_exceptions=%llu elapsed_ms=%.0f\n",
        g_trigger_done.load(std::memory_order_acquire),
        g_extra_done.load(std::memory_order_acquire), stuck_before,
        static_cast<unsigned long long>(
            g_forced_unlocks_seen.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(
            g_stale_unlock_skips.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(
            g_unlock_exceptions.load(std::memory_order_acquire)),
        std::chrono::duration<double, std::milli>(finished - started).count());
    PrintLoadSummary("FINAL", load_seconds);
    PrintRouteSummary("FINAL_ROUTES ok/total:");
    PrintClientErrors("FINAL_ERRORS");
    std::printf(
        "FINAL_RESOURCES res-a=%llu res-b=%llu res-c=%llu res-d=%llu "
        "gid_a=%llu gid_b=%llu\n",
        static_cast<unsigned long long>(g_counter_a),
        static_cast<unsigned long long>(g_counter_b),
        static_cast<unsigned long long>(g_counter_c),
        static_cast<unsigned long long>(g_counter_d),
        static_cast<unsigned long long>(gid_a),
        static_cast<unsigned long long>(gid_b));
    std::fflush(stdout);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    ClientConfig config;
    std::string debug_socket;
    std::string doctor_socket;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            return i + 1 < argc ? argv[++i] : std::string{};
        };
        if (arg == "--port") {
            config.port = std::atoi(next().c_str());
        } else if (arg == "--clients") {
            config.clients = std::atoi(next().c_str());
        } else if (arg == "--load-ms") {
            config.load_ms = std::atoi(next().c_str());
        } else if (arg == "--extra-waiters") {
            config.extra_waiters = std::atoi(next().c_str());
        } else if (arg == "--debug-socket") {
            debug_socket = next();
        } else if (arg == "--doctor-socket") {
            doctor_socket = next();
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: %s [--port N] [--clients N] [--load-ms N] "
                "[--extra-waiters N] [--debug-socket PATH] "
                "[--doctor-socket PATH]\n",
                argv[0]);
            return 0;
        }
    }
    if (config.clients < 100) {
        std::fprintf(stderr, "[scenario] clients=%d < 100，按 100 处理\n",
                     config.clients);
        config.clients = 100;
    }

    // 记录锁的近似 owner（gid），供 dump / deadlock-report 使用。
    // 这是既有调试开关，默认在 Release 下关闭，场景显式打开。
    sync::SetHeldLockParkDetection(true);

    SchedulerConfig scheduler_config;
    scheduler_config.processor_count = 4;
    scheduler_config.min_workers = 4;
    scheduler_config.max_workers = 8;
    scheduler_config.collect_metrics = true;
    Scheduler scheduler(scheduler_config);
    scheduler.start();
    g_scheduler = &scheduler;

    // 监听端口。用 ListenTCP 而不是手写 CreateTCP/bind/listen：它设置了
    // SO_REUSEADDR，重复运行场景时不会被上一轮的 TIME_WAIT 顶掉。
    auto listener = net::ListenTCP("127.0.0.1",
                                   static_cast<std::uint16_t>(config.port),
                                   4096);
    if (!listener) {
        std::fprintf(stderr, "[scenario] bind/listen 127.0.0.1:%d failed\n",
                     config.port);
        return 1;
    }

    http::Server server(listener);
    server.route(http::Method::Get, "/res-a",
                 [](const http::Request& request) {
                     return HandleCrossLock(request, true);
                 });
    server.route(http::Method::Post, "/res-a",
                 [](const http::Request& request) {
                     return HandleCrossLock(request, true);
                 });
    server.route(http::Method::Get, "/res-b",
                 [](const http::Request& request) {
                     return HandleCrossLock(request, false);
                 });
    server.route(http::Method::Post, "/res-b",
                 [](const http::Request& request) {
                     return HandleCrossLock(request, false);
                 });
    server.route(http::Method::Get, "/res-c",
                 [](const http::Request& request) {
                     return HandleSingleLock(request, "/res-c", g_mutex_c,
                                             &g_counter_c, false);
                 });
    server.route(http::Method::Post, "/res-c",
                 [](const http::Request& request) {
                     return HandleSingleLock(request, "/res-c", g_mutex_c,
                                             &g_counter_c, false);
                 });
    server.route(http::Method::Get, "/res-d",
                 [](const http::Request& request) {
                     return HandleDLockC(request);
                 });
    server.route(http::Method::Post, "/res-d",
                 [](const http::Request& request) {
                     return HandleDLockC(request);
                 });
    server.route(http::Method::Get, "/res-a-ro",
                 [](const http::Request& request) {
                     return HandleSingleLock(request, "/res-a-ro", g_mutex_a,
                                             &g_counter_a, true);
                 });
    server.route(http::Method::Post, "/res-a-ro",
                 [](const http::Request& request) {
                     return HandleSingleLock(request, "/res-a-ro", g_mutex_a,
                                             &g_counter_a, true);
                 });
    server.route(http::Method::Get, "/res-b-ro",
                 [](const http::Request& request) {
                     return HandleSingleLock(request, "/res-b-ro", g_mutex_b,
                                             &g_counter_b, true);
                 });
    server.route(http::Method::Post, "/res-b-ro",
                 [](const http::Request& request) {
                     return HandleSingleLock(request, "/res-b-ro", g_mutex_b,
                                             &g_counter_b, true);
                 });
    server.route(http::Method::Get, "/health", HandleHealth);
    server.route(http::Method::Post, "/health", HandleHealth);
    server.set_fallback(HandleNotFound);

    // DebugServer：dump 的 socket 服务。
    debug::DebugServer& debug_server = debug::DebugServer::Instance();
    if (!debug_server.Start(debug_socket)) {
        std::fprintf(stderr, "[scenario] DebugServer start failed: %s\n",
                     debug_server.last_error().c_str());
        return 1;
    }
    // 自描述组件：scheduler 由 Doctor 注册；fiber/mutex 由场景提供带注解的视图。
    (void)debug::DebugServer::Instance().Register(
        "fiber", "scenario fiber gid/state/waiting_on/held",
        DumpScenarioFibers);
    (void)debug::DebugServer::Instance().Register(
        "mutex", "scenario resource mutexes owner/waiters",
        DumpScenarioMutexes);

    // Doctor：诊断命令平面。
    debug::DiagnosticAgent& doctor = debug::DiagnosticAgent::Instance();
    if (!doctor.RegisterScheduler("scheduler", scheduler)) {
        std::fprintf(stderr, "[scenario] doctor register scheduler failed\n");
        return 1;
    }
    if (!doctor.RegisterMutex("res-a", g_mutex_a) ||
        !doctor.RegisterMutex("res-b", g_mutex_b) ||
        !doctor.RegisterMutex("res-c", g_mutex_c) ||
        !doctor.RegisterMutex("res-d", g_mutex_d)) {
        std::fprintf(stderr, "[scenario] doctor register mutex failed\n");
        return 1;
    }
    debug::DiagnosticAgentConfig doctor_config;
    doctor_config.socket_path = doctor_socket;
    doctor_config.recv_timeout_ms = 20;
    doctor_config.detect_interval = 100ms;
    doctor_config.deadlock_threshold = 600ms;
    doctor_config.auto_force_wake = false;
    if (!doctor.Start(doctor_config)) {
        std::fprintf(stderr, "[scenario] Doctor start failed: %s\n",
                     doctor.last_error().c_str());
        return 1;
    }

    std::printf(
        "READY pid=%d port=%d clients=%d debug_socket=%s doctor_socket=%s\n",
        static_cast<int>(::getpid()), config.port, config.clients + 2,
        debug_server.path().c_str(), doctor.socket_path().c_str());
    std::fflush(stdout);

    const std::string base_url =
        "http://127.0.0.1:" + std::to_string(config.port);

    std::thread monitor(MonitorThread);

    std::vector<std::thread> load_threads;
    load_threads.reserve(static_cast<std::size_t>(config.clients));
    for (int i = 0; i < config.clients; ++i) {
        load_threads.emplace_back(LoadWorker, i, base_url);
    }
    std::vector<std::thread> trigger_threads;
    trigger_threads.emplace_back(TriggerWorker, 0, base_url);
    trigger_threads.emplace_back(TriggerWorker, 1, base_url);

    const auto shutdown = [&](int code) {
        g_stop_load.store(true, std::memory_order_release);
        g_stop_monitor.store(true, std::memory_order_release);
        if (monitor.joinable()) {
            monitor.join();
        }
        doctor.Stop();
        debug_server.Stop();
        ExitNow(code);
    };

    std::thread orchestrator([&] {
        const int result =
            RunOrchestrator(config, base_url, &load_threads, &trigger_threads);
        shutdown(result);
    });

    // 主线程：accept 循环，每条连接一个 Fiber。
    while (true) {
        auto connection = listener->accept();
        if (!connection) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        scheduler.spawn([&server, connection]() mutable {
            server.serve_connection(std::move(connection), 1);
        });
    }
    return 0;
}
