/**
 * @file test_session_table.cpp
 * @brief 会话表测试：1:N 增删查、上限拒绝、90s 租约过期、重复 key 与
 *        16 线程并发注册/查询/清理。
 * @details 覆盖 MOD-14 / FR-06 / RISK-032 / AX-009：
 *          - 场景 1：一个 server + 多个 client 的 1:N 映射与 List 顺序。
 *          - 场景 2：每 key 4 个 client 上限，第 5 个返回 kFull。
 *          - 场景 3：重复 key/client、Remove/RemoveClient/RemoveServer 分支。
 *          - 场景 4：租约边界（90999/91000）、Touch/AddClient 续租、过期重注册。
 *          - 场景 5：16 线程并发 AddServer 同一 key，只有 1 个成功。
 *          - 场景 6：16 线程并发 AddClient 同一 key，恰好 4 个成功。
 *          - 场景 7：16 线程混合读写 2000 次/线程，无死锁/崩溃。
 *          - 场景 8：空 key/id=0、max_keys 上限、配置钳制。
 *          - 场景 9：统计计数。
 *          - 场景 10-18（2026-10-29 审查补充）：状态名全枚举、租约加法饱和、
 *            反复续租后过期索引不残留、List 过滤过期但 size 保留、
 *            RemoveServer 释放名额、Find 返回副本、4 KiB 长 key、默认时钟、
 *            并发写后计数器精确对齐。
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "go2cpp/bento/net/session_table.hpp"
#include "test_util.hpp"

namespace {

using go2cpp::bento::SessionEntry;
using go2cpp::bento::SessionStatus;
using go2cpp::bento::SessionTable;
using go2cpp::bento::SessionTableConfig;

SessionTableConfig Config(std::size_t max_clients = 4, std::uint64_t lease_ms = 90000,
                          std::size_t max_keys = 16) {
    SessionTableConfig config;
    config.max_clients_per_key = max_clients;
    config.lease_ms = lease_ms;
    config.max_keys = max_keys;
    return config;
}

/// 场景 1：1:N 映射、Find/List/Contains 与字段。
void TestAddFind() {
    SessionTable table(Config());
    BENTO_CHECK_EQ(table.AddServer("alpha", 100, 1000), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("beta", 200, 1000), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("alpha", 1, 1000), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("alpha", 2, 1000), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("beta", 3, 1000), SessionStatus::kOk);

    auto entry = table.Find("alpha", 1000);
    BENTO_CHECK(entry.has_value());
    BENTO_CHECK_EQ(entry->key, std::string("alpha"));
    BENTO_CHECK_EQ(entry->server_id, 100U);
    BENTO_CHECK_EQ(entry->ClientCount(), 2U);
    BENTO_CHECK(entry->HasClient(1));
    BENTO_CHECK(entry->HasClient(2));
    BENTO_CHECK(!entry->HasClient(3));
    BENTO_CHECK_EQ(entry->created_ms, 1000U);
    BENTO_CHECK_EQ(entry->lease_deadline_ms, 91000U);
    BENTO_CHECK(!entry->IsExpired(90999));
    BENTO_CHECK(entry->IsExpired(91000));

    auto list = table.List(1000);
    BENTO_CHECK_EQ(list.size(), 2U);
    BENTO_CHECK_EQ(list[0].key, std::string("alpha"));  // std::map 字典序
    BENTO_CHECK_EQ(list[1].key, std::string("beta"));
    BENTO_CHECK(table.Contains("alpha", 1000));
    BENTO_CHECK(!table.Contains("gamma", 1000));
    BENTO_CHECK_EQ(table.size(), 2U);
    BENTO_CHECK(!table.empty());
    BENTO_CHECK(!table.Find("gamma", 1000).has_value());
    BENTO_CHECK(!table.Find("", 1000).has_value());
}

/// 场景 2：client 上限与 key 之间互不影响。
void TestClientLimit() {
    SessionTable table(Config(4));
    BENTO_CHECK_EQ(table.AddServer("k1", 10, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("k2", 20, 0), SessionStatus::kOk);
    for (std::uint64_t client = 1; client <= 4; ++client) {
        BENTO_CHECK_EQ(table.AddClient("k1", client, 0), SessionStatus::kOk);
    }
    BENTO_CHECK_EQ(table.AddClient("k1", 5, 0), SessionStatus::kFull);
    BENTO_CHECK_EQ(table.AddClient("k1", 6, 0), SessionStatus::kFull);
    BENTO_CHECK_EQ(table.stats().full_rejects, 2U);

    const auto entry = table.Find("k1", 0);
    BENTO_CHECK(entry.has_value());
    BENTO_CHECK_EQ(entry->ClientCount(), 4U);
    // 另一个 key 仍有独立名额。
    BENTO_CHECK_EQ(table.AddClient("k2", 1, 0), SessionStatus::kOk);
}

/// 场景 3：重复 key/client 与删除分支。
void TestDuplicateAndRemove() {
    SessionTable table(Config());
    BENTO_CHECK_EQ(table.AddServer("k", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("k", 2, 0), SessionStatus::kAlreadyExists);
    BENTO_CHECK_EQ(table.AddClient("k", 10, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("k", 10, 0), SessionStatus::kAlreadyExists);
    BENTO_CHECK_EQ(table.stats().duplicate_rejects, 2U);
    BENTO_CHECK_EQ(table.AddClient("missing", 11, 0), SessionStatus::kNotFound);

    BENTO_CHECK_EQ(table.RemoveClient("k", 99, 0), SessionStatus::kNotFound);
    BENTO_CHECK_EQ(table.RemoveClient("missing", 10, 0), SessionStatus::kNotFound);
    BENTO_CHECK_EQ(table.RemoveClient("k", 10, 0), SessionStatus::kOk);
    auto entry = table.Find("k", 0);
    BENTO_CHECK(entry.has_value());
    BENTO_CHECK_EQ(entry->ClientCount(), 0U);

    BENTO_CHECK_EQ(table.RemoveServer("k", 2, 0), SessionStatus::kNotFound);
    BENTO_CHECK(table.Contains("k", 0));
    BENTO_CHECK_EQ(table.RemoveServer("missing", 1, 0), SessionStatus::kNotFound);
    BENTO_CHECK(!table.Remove("missing", 0));
    BENTO_CHECK(table.Remove("k", 0));
    BENTO_CHECK(!table.Contains("k", 0));
    BENTO_CHECK(table.empty());
    BENTO_CHECK_EQ(table.stats().removals, 2U);  // RemoveClient + Remove
}

/// 场景 4：租约边界、续租、过期重注册与 lease_ms == 0。
void TestLeaseExpiry() {
    SessionTable table(Config(4, 90000));
    BENTO_CHECK_EQ(table.AddServer("k", 1, 1000), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("k", 2, 1000), SessionStatus::kOk);
    BENTO_CHECK(table.Find("k", 90999).has_value());
    BENTO_CHECK(!table.Find("k", 91000).has_value());
    BENTO_CHECK_EQ(table.size(), 1U);  // 读路径不删除

    BENTO_CHECK_EQ(table.Tick(91000), 1U);
    BENTO_CHECK_EQ(table.size(), 0U);
    BENTO_CHECK_EQ(table.stats().expirations, 1U);
    BENTO_CHECK_EQ(table.Tick(91000), 0U);

    // 过期后同一 key 可以重新注册。
    BENTO_CHECK_EQ(table.AddServer("k", 3, 91000), SessionStatus::kOk);

    // Touch 续租：100000 + 90000 = 190000。
    BENTO_CHECK(table.Touch("k", 100000));
    BENTO_CHECK_EQ(table.stats().touches, 1U);
    BENTO_CHECK(table.Find("k", 189999).has_value());
    BENTO_CHECK(!table.Find("k", 190000).has_value());

    // AddClient 也续租。
    SessionTable client_renew(Config(4, 90000));
    BENTO_CHECK_EQ(client_renew.AddServer("c", 1, 1000), SessionStatus::kOk);
    BENTO_CHECK_EQ(client_renew.AddClient("c", 2, 80000), SessionStatus::kOk);
    BENTO_CHECK(client_renew.Find("c", 160000).has_value());
    BENTO_CHECK(!client_renew.Find("c", 170001).has_value());

    // lease_ms == 0 表示永不过期。
    SessionTable permanent(Config(4, 0));
    BENTO_CHECK_EQ(permanent.AddServer("p", 1, 0), SessionStatus::kOk);
    BENTO_CHECK(permanent.Find("p", 1000000000ULL).has_value());
    BENTO_CHECK_EQ(permanent.Tick(1000000000ULL), 0U);
    BENTO_CHECK(permanent.Touch("nope", 0) == false);
}

/// 场景 5：16 线程并发 AddServer 同一 key，只有 1 个成功。
void TestConcurrentAddServer() {
    constexpr int kThreads = 16;
    SessionTable table(Config());
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> ok{0};
    std::atomic<int> duplicate{0};
    std::atomic<int> other{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index]() {
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const SessionStatus status =
                table.AddServer("same-key", static_cast<std::uint64_t>(index + 1), 0);
            if (status == SessionStatus::kOk) {
                ok.fetch_add(1);
            } else if (status == SessionStatus::kAlreadyExists) {
                duplicate.fetch_add(1);
            } else {
                other.fetch_add(1);
            }
        });
    }
    while (ready.load() < kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(ok.load(), 1);
    BENTO_CHECK_EQ(duplicate.load(), kThreads - 1);
    BENTO_CHECK_EQ(other.load(), 0);
    const auto entry = table.Find("same-key", 0);
    BENTO_CHECK(entry.has_value());
    BENTO_CHECK(entry->server_id != 0);
    BENTO_CHECK_EQ(table.size(), 1U);
    BENTO_CHECK_EQ(table.stats().duplicate_rejects, static_cast<std::uint64_t>(kThreads - 1));
    std::printf("[INFO] concurrent AddServer: ok=%d duplicate=%d\n", ok.load(),
                duplicate.load());
}

/// 场景 6：16 线程并发 AddClient 同一 key，恰好 4 个成功。
void TestConcurrentAddClient() {
    constexpr int kThreads = 16;
    SessionTable table(Config(4));
    BENTO_CHECK_EQ(table.AddServer("k", 1, 0), SessionStatus::kOk);
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> ok{0};
    std::atomic<int> full{0};
    std::atomic<int> duplicate{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index]() {
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const SessionStatus status =
                table.AddClient("k", static_cast<std::uint64_t>(index + 1), 0);
            if (status == SessionStatus::kOk) {
                ok.fetch_add(1);
            } else if (status == SessionStatus::kFull) {
                full.fetch_add(1);
            } else if (status == SessionStatus::kAlreadyExists) {
                duplicate.fetch_add(1);
            }
        });
    }
    while (ready.load() < kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK_EQ(ok.load(), 4);
    BENTO_CHECK_EQ(full.load(), kThreads - 4);
    BENTO_CHECK_EQ(duplicate.load(), 0);
    const auto entry = table.Find("k", 0);
    BENTO_CHECK(entry.has_value());
    BENTO_CHECK_EQ(entry->ClientCount(), 4U);
    std::printf("[INFO] concurrent AddClient: ok=%d full=%d\n", ok.load(), full.load());
}

/// 场景 7：16 线程混合读写，验证无死锁、无数据竞争崩溃。
void TestConcurrentMixed() {
    constexpr int kThreads = 16;
    constexpr int kOps = 2000;
    SessionTable table(Config(4, 90000, 8));
    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::atomic<std::uint64_t> queries{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index]() {
            std::mt19937 rng(static_cast<unsigned int>(index * 7919 + 17));
            std::uint64_t now_ms = 1000;
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int step = 0; step < kOps; ++step) {
                const std::string key = "key" + std::to_string(rng() % 8U);
                const std::uint64_t client_id = (rng() % 32U) + 1;
                const std::uint64_t server_id = (rng() % 8U) + 1;
                switch (rng() % 7U) {
                    case 0:
                        (void)table.AddServer(key, server_id, now_ms);
                        break;
                    case 1:
                        (void)table.AddClient(key, client_id, now_ms);
                        break;
                    case 2:
                        (void)table.RemoveClient(key, client_id, now_ms);
                        break;
                    case 3:
                        (void)table.Touch(key, now_ms);
                        break;
                    case 4:
                        (void)table.Find(key, now_ms);
                        queries.fetch_add(1);
                        break;
                    case 5:
                        (void)table.List(now_ms);
                        queries.fetch_add(1);
                        break;
                    default:
                        (void)table.Tick(now_ms);
                        ++now_ms;
                        break;
                }
                now_ms += 37;
            }
        });
    }
    while (ready.load() < kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_CHECK(queries.load() > 0);
    // 强断言：并发结束后每个 key 的 client 仍不超过上限，且没有条目被写坏。
    // 时间只推进到 75000（< 90000 租约），所以此刻不存在过期条目，
    // List 必须返回与 size() 相同数量的条目。
    const auto snapshot = table.List(75000);
    BENTO_CHECK_EQ(snapshot.size(), table.size());
    BENTO_CHECK(table.size() <= 8U);
    for (const auto& entry : snapshot) {
        BENTO_CHECK(entry.ClientCount() <= 4U);
        BENTO_CHECK(entry.server_id != 0);
        for (const auto& client : entry.clients) {
            BENTO_CHECK(client.client_id >= 1 && client.client_id <= 32);
        }
    }
    const std::size_t remaining = table.size();
    BENTO_CHECK_EQ(table.Tick(1000000000ULL), remaining);
    BENTO_CHECK(table.empty());
    std::printf("[INFO] concurrent mixed: entries cleaned, queries=%llu\n",
                static_cast<unsigned long long>(queries.load()));
}

/// 场景 8：参数校验、max_keys 上限与配置钳制。
void TestValidationAndLimits() {
    SessionTable table(Config(4, 90000, 2));
    BENTO_CHECK_EQ(table.AddServer("", 1, 0), SessionStatus::kInvalidArgument);
    BENTO_CHECK_EQ(table.AddServer("k", 0, 0), SessionStatus::kInvalidArgument);
    BENTO_CHECK_EQ(table.AddClient("", 1, 0), SessionStatus::kInvalidArgument);
    BENTO_CHECK_EQ(table.AddClient("k", 0, 0), SessionStatus::kInvalidArgument);
    BENTO_CHECK_EQ(table.RemoveClient("", 1, 0), SessionStatus::kInvalidArgument);
    BENTO_CHECK_EQ(table.RemoveServer("k", 0, 0), SessionStatus::kInvalidArgument);
    BENTO_CHECK(!table.Remove("", 0));
    BENTO_CHECK(!table.Touch("", 0));

    BENTO_CHECK_EQ(table.AddServer("a", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("b", 2, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("c", 3, 0), SessionStatus::kKeyLimit);
    BENTO_CHECK_EQ(table.stats().key_limit_rejects, 1U);

    // 配置钳制：0 个 client / 0 个 key 都不允许，钳到 1。
    SessionTable clamped(Config(0, 90000, 0));
    BENTO_CHECK_EQ(clamped.config().max_clients_per_key, 1U);
    BENTO_CHECK_EQ(clamped.config().max_keys, 1U);
    BENTO_CHECK_EQ(clamped.AddServer("x", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(clamped.AddClient("x", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(clamped.AddClient("x", 2, 0), SessionStatus::kFull);

    // 过期条目释放 max_keys 名额。
    SessionTable tight(Config(4, 1000, 1));
    BENTO_CHECK_EQ(tight.AddServer("a", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(tight.AddServer("b", 2, 0), SessionStatus::kKeyLimit);
    BENTO_CHECK_EQ(tight.AddServer("b", 2, 2000), SessionStatus::kOk);
    BENTO_CHECK_EQ(tight.size(), 1U);
}

/// 场景 9：统计计数。
void TestStats() {
    SessionTable table(Config(2, 90000, 4));
    BENTO_CHECK_EQ(table.stats().servers_added, 0U);
    BENTO_CHECK_EQ(table.AddServer("k", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("k", 2, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("k", 3, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("k", 4, 0), SessionStatus::kFull);
    BENTO_CHECK_EQ(table.AddServer("k", 9, 0), SessionStatus::kAlreadyExists);
    BENTO_CHECK_EQ(table.AddClient("k", 2, 0), SessionStatus::kAlreadyExists);
    BENTO_CHECK_EQ(table.AddClient("missing", 5, 0), SessionStatus::kNotFound);
    BENTO_CHECK(table.Touch("k", 10));
    BENTO_CHECK_EQ(table.RemoveClient("k", 2, 20), SessionStatus::kOk);
    BENTO_CHECK(table.Remove("k", 30));

    const auto stats = table.stats();
    BENTO_CHECK_EQ(stats.servers_added, 1U);
    BENTO_CHECK_EQ(stats.clients_added, 2U);
    BENTO_CHECK_EQ(stats.full_rejects, 1U);
    BENTO_CHECK_EQ(stats.duplicate_rejects, 2U);
    BENTO_CHECK_EQ(stats.not_found, 1U);
    BENTO_CHECK_EQ(stats.touches, 1U);
    BENTO_CHECK_EQ(stats.removals, 2U);
    BENTO_CHECK_EQ(stats.expirations, 0U);
}

/// 场景 10：SessionStatusName 覆盖全部枚举值与未知值（日志/指标标签稳定性）。
void TestStatusNames() {
    BENTO_CHECK_EQ(std::string(SessionStatusName(SessionStatus::kOk)), std::string("ok"));
    BENTO_CHECK_EQ(std::string(SessionStatusName(SessionStatus::kNotFound)),
                   std::string("not_found"));
    BENTO_CHECK_EQ(std::string(SessionStatusName(SessionStatus::kAlreadyExists)),
                   std::string("already_exists"));
    BENTO_CHECK_EQ(std::string(SessionStatusName(SessionStatus::kFull)), std::string("full"));
    BENTO_CHECK_EQ(std::string(SessionStatusName(SessionStatus::kExpired)),
                   std::string("expired"));
    BENTO_CHECK_EQ(std::string(SessionStatusName(SessionStatus::kInvalidArgument)),
                   std::string("invalid_argument"));
    BENTO_CHECK_EQ(std::string(SessionStatusName(SessionStatus::kKeyLimit)),
                   std::string("key_limit"));
    BENTO_CHECK_EQ(std::string(SessionStatusName(static_cast<SessionStatus>(200))),
                   std::string("unknown"));
}

/// 场景 11：now 接近 UINT64_MAX 时租约加法必须饱和，不能回绕成立即过期。
void TestLeaseOverflow() {
    SessionTable table(Config(4, 90000));
    const std::uint64_t near_max = std::numeric_limits<std::uint64_t>::max() - 10;
    BENTO_CHECK_EQ(table.AddServer("edge", 1, near_max), SessionStatus::kOk);
    const auto entry = table.Find("edge", near_max);
    BENTO_CHECK(entry.has_value());
    // 回绕会得到很小的 deadline，使 now 立刻“过期”。
    BENTO_CHECK(entry->lease_deadline_ms >= near_max);
    BENTO_CHECK_EQ(table.Tick(near_max), 0U);
    BENTO_CHECK(table.Contains("edge", near_max));
    BENTO_CHECK_EQ(table.Remove("edge", near_max), true);
}

/// 场景 12：同一 key 反复续租后，过期索引只保留最新 deadline（优化回归）。
void TestLeaseIndexNoStale() {
    SessionTable table(Config(4, 90000, 8));
    BENTO_CHECK_EQ(table.AddServer("k", 1, 0), SessionStatus::kOk);
    for (int step = 1; step <= 100; ++step) {
        BENTO_CHECK(table.Touch("k", static_cast<std::uint64_t>(step) * 1000));
    }
    // 最新 deadline = 100000 + 90000 = 190000；旧索引项若残留，
    // 会在 Tick(100000) 时把条目误删。
    BENTO_CHECK_EQ(table.Tick(100000), 0U);
    BENTO_CHECK(table.Contains("k", 100000));
    BENTO_CHECK(table.Find("k", 189999).has_value());
    BENTO_CHECK(!table.Find("k", 190000).has_value());
    BENTO_CHECK_EQ(table.Tick(190000), 1U);
    BENTO_CHECK_EQ(table.Tick(190000), 0U);
    BENTO_CHECK_EQ(table.stats().expirations, 1U);
    BENTO_CHECK(table.empty());
}

/// 场景 13：List 过滤过期项但 size() 保留（读路径不删除、不续租）。
void TestListFiltersExpired() {
    SessionTable table(Config(4, 1000, 8));
    // old 的 deadline=1000；fresh 在 500ms 插入（写操作只会清理 <500 的过期项），
    // 所以到了 now=1000 时 old 已过期、fresh（deadline=1500）仍有效。
    BENTO_CHECK_EQ(table.AddServer("old", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("fresh", 2, 500), SessionStatus::kOk);
    const auto list = table.List(1000);
    BENTO_CHECK_EQ(list.size(), 1U);
    BENTO_CHECK_EQ(list[0].key, std::string("fresh"));
    BENTO_CHECK_EQ(table.size(), 2U);
    BENTO_CHECK(!table.Contains("old", 1000));
    BENTO_CHECK(table.Contains("fresh", 1000));
    BENTO_CHECK_EQ(table.Tick(1000), 1U);
}

/// 场景 14：RemoveServer 成功后释放 max_keys 名额。
void TestRemoveServerReleasesSlot() {
    SessionTable table(Config(4, 90000, 1));
    BENTO_CHECK_EQ(table.AddServer("a", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("b", 2, 0), SessionStatus::kKeyLimit);
    BENTO_CHECK_EQ(table.RemoveServer("a", 9, 0), SessionStatus::kNotFound);
    BENTO_CHECK_EQ(table.size(), 1U);
    BENTO_CHECK_EQ(table.RemoveServer("a", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddServer("b", 2, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.Tick(1000000), 1U);
}

/// 场景 15：Find/List 返回副本，改动返回值不能影响表内状态。
void TestFindReturnsCopy() {
    SessionTable table(Config(4, 90000));
    BENTO_CHECK_EQ(table.AddServer("k", 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("k", 7, 0), SessionStatus::kOk);
    auto entry = table.Find("k", 0);
    BENTO_CHECK(entry.has_value());
    entry->clients.clear();
    entry->server_id = 999;
    const auto again = table.Find("k", 0);
    BENTO_CHECK(again.has_value());
    BENTO_CHECK_EQ(again->server_id, 1U);
    BENTO_CHECK_EQ(again->ClientCount(), 1U);
}

/// 场景 16：超长 key（4 KiB）可注册/查找/删除（哈希与拷贝路径）。
void TestLongKey() {
    SessionTable table(Config(4, 90000));
    const std::string long_key(4096, 'x');
    BENTO_CHECK_EQ(table.AddServer(long_key, 1, 0), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient(long_key, 2, 0), SessionStatus::kOk);
    BENTO_CHECK(table.Contains(long_key, 0));
    const auto entry = table.Find(long_key, 0);
    BENTO_CHECK(entry.has_value());
    BENTO_CHECK_EQ(entry->key.size(), 4096U);
    BENTO_CHECK_EQ(table.Remove(long_key, 0), true);
    BENTO_CHECK(table.empty());
}

/// 场景 17：默认时钟（NowMs）路径可用，真实时钟下注册/续租/删除闭环。
void TestDefaultClock() {
    SessionTable table;
    BENTO_CHECK_EQ(table.AddServer("now", 1), SessionStatus::kOk);
    BENTO_CHECK_EQ(table.AddClient("now", 2), SessionStatus::kOk);
    BENTO_CHECK(table.Contains("now"));
    BENTO_CHECK(table.Touch("now"));
    BENTO_CHECK_EQ(table.RemoveServer("now", 1), SessionStatus::kOk);
    BENTO_CHECK(table.empty());
}

/// 场景 18：并发写后计数器精确对齐（8 线程 × 200 次尝试）。
void TestConcurrentStatsConsistency() {
    constexpr int kThreads = 8;
    constexpr int kAttempts = 200;
    SessionTable table(Config(4, 90000, 64));
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index]() {
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int attempt = 0; attempt < kAttempts; ++attempt) {
                const std::string key = "s" + std::to_string(index);
                (void)table.AddServer(key, static_cast<std::uint64_t>(index + 1), 1000);
                (void)table.AddClient(key, static_cast<std::uint64_t>(attempt % 4) + 1, 1000);
            }
        });
    }
    while (ready.load() < kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    const auto stats = table.stats();
    // 每线程 1 个 key：首个 AddServer 成功，其余 199 次重复。
    BENTO_CHECK_EQ(stats.servers_added, static_cast<std::uint64_t>(kThreads));
    BENTO_CHECK_EQ(stats.clients_added, static_cast<std::uint64_t>(kThreads) * 4U);
    // 重复 = AddServer 199×8 + AddClient 196×8。
    BENTO_CHECK_EQ(stats.duplicate_rejects,
                   static_cast<std::uint64_t>(kThreads) * (kAttempts - 1) +
                       static_cast<std::uint64_t>(kThreads) * (kAttempts - 4));
    BENTO_CHECK_EQ(stats.full_rejects, 0U);
    BENTO_CHECK_EQ(stats.not_found, 0U);
    BENTO_CHECK_EQ(table.size(), static_cast<std::size_t>(kThreads));
    for (const auto& entry : table.List(1000)) {
        BENTO_CHECK_EQ(entry.ClientCount(), 4U);
    }
    BENTO_CHECK_EQ(table.Tick(1000000), static_cast<std::size_t>(kThreads));
}

}  // namespace

int main() {
    TestAddFind();
    TestClientLimit();
    TestDuplicateAndRemove();
    TestLeaseExpiry();
    TestConcurrentAddServer();
    TestConcurrentAddClient();
    TestConcurrentMixed();
    TestValidationAndLimits();
    TestStats();
    TestStatusNames();
    TestLeaseOverflow();
    TestLeaseIndexNoStale();
    TestListFiltersExpired();
    TestRemoveServerReleasesSlot();
    TestFindReturnsCopy();
    TestLongKey();
    TestDefaultClock();
    TestConcurrentStatsConsistency();
    return bento_test::Finish("test_session_table");
}
