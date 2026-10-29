/**
 * @file audit_session_table_stress.cpp
 * @brief session_table 模块审查用压测/随机/场景程序（不注册进 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_session_table.cpp` 的分工：
 *          - `test_session_table.cpp` 是注册进 CTest 的回归测试，只放确定性、
 *            毫秒级用例；
 *          - 本程序是**审查压测工具**，覆盖百万级 AddServer/AddClient/Find/
 *            List/Tick、影子模型随机测试、5 个真实场景（1:N 上限、90s 租约
 *            边界、16 线程并发、重复 key 语义、长稳）与泄漏模式。
 *
 *          刻意**不** include `test_util.hpp`：该头文件会拖入 `link.hpp`，
 *          使本程序无法证明 session_table 只依赖标准库。这里只用
 *          `session_table.hpp` + 标准库，链接 `session_table.cpp` 与
 *          `protocol.cpp` 即可构建。
 *
 * 用法：
 * @code
 *   audit_session_table_stress --mode=all --scale=1.0          # 全量（约 90s）
 *   audit_session_table_stress --mode=bench --scale=1.0        # 只跑吞吐
 *   audit_session_table_stress --mode=random --scale=1.0       # 影子模型随机
 *   audit_session_table_stress --mode=scenarios                # 5 个真实场景
 *   audit_session_table_stress --mode=soak --seconds=60        # 60s 长稳
 *   audit_session_table_stress --mode=all --scale=0.02         # ASan/UBSan
 *   audit_session_table_stress --mode=leaks --scale=0.01       # valgrind
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/session_table.hpp"

namespace {

using go2cpp::bento::SessionEntry;
using go2cpp::bento::SessionStatus;
using go2cpp::bento::SessionTable;
using go2cpp::bento::SessionTableConfig;

int g_failures = 0;
long g_checks = 0;

/// ASan 的隔离区会保留已释放内存，进程 RSS 会远高于真实占用；
/// 内存类阈值只在普通构建生效，sanitizer 构建只打印数值不做断言。
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitizerBuild = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kSanitizerBuild = true;
#else
constexpr bool kSanitizerBuild = false;
#endif
#else
constexpr bool kSanitizerBuild = false;
#endif

/// 断言：失败时记录位置与数值，但不中断，便于一次看到全部问题。
#define AUDIT_CHECK(condition)                                                          \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                               \
        }                                                                               \
    } while (0)

#define AUDIT_CHECK_EQ(actual, expected)                                                          \
    do {                                                                                          \
        ++g_checks;                                                                               \
        if (!((actual) == (expected))) {                                                          \
            std::fprintf(stderr, "[FAIL] %s:%d: %s == %s\n", __FILE__, __LINE__, #actual,         \
                         #expected);                                                            \
            ++g_failures;                                                                         \
        }                                                                                         \
    } while (0)

using Clock = std::chrono::steady_clock;

/// 进程当前 RSS（KiB）；Linux 专用，读 /proc/self/status。
std::size_t CurrentRssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::size_t kb = 0;
            if (std::sscanf(line.c_str(), "VmRSS: %zu kB", &kb) == 1) {
                return kb;
            }
        }
    }
    return 0;
}

/// 进程峰值 RSS（KiB）。
std::size_t PeakRssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            std::size_t kb = 0;
            if (std::sscanf(line.c_str(), "VmHWM: %zu kB", &kb) == 1) {
                return kb;
            }
        }
    }
    return 0;
}

struct BenchRow {
    std::string name;
    std::uint64_t ops = 0;
    double ns_per_op = 0.0;
    double ops_per_sec = 0.0;
    std::size_t rss_kb = 0;
};

std::vector<BenchRow> g_bench_rows;

/// 执行一段被测代码并记录吞吐。ops 为逻辑操作数。
template <typename Function>
void Bench(const char* name, std::uint64_t ops, Function&& function) {
    const auto begin = Clock::now();
    function();
    const auto end = Clock::now();
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    BenchRow row;
    row.name = name;
    row.ops = ops;
    row.ns_per_op = seconds * 1e9 / static_cast<double>(ops);
    row.ops_per_sec = static_cast<double>(ops) / seconds;
    row.rss_kb = CurrentRssKb();
    g_bench_rows.push_back(row);
    std::printf("[BENCH] %-22s ops=%-9llu %8.1f ns/op  %10.2f Mops/s  rss=%zuKiB\n", name,
                static_cast<unsigned long long>(ops), row.ns_per_op, row.ops_per_sec / 1e6,
                row.rss_kb);
}

SessionTableConfig MakeConfig(std::size_t max_clients, std::uint64_t lease_ms,
                              std::size_t max_keys) {
    SessionTableConfig config;
    config.max_clients_per_key = max_clients;
    config.lease_ms = lease_ms;
    config.max_keys = max_keys;
    return config;
}

// ---------------------------------------------------------------------------
// 压测
// ---------------------------------------------------------------------------

/// AddServer 吞吐：每轮 1024 key 的完整建表 + 析构，共 1e6 次注册。
void BenchAddServer(std::uint64_t total_ops) {
    constexpr std::size_t kKeysPerTable = 1024;
    const std::uint64_t tables = (total_ops + kKeysPerTable - 1) / kKeysPerTable;
    Bench("add_server", tables * kKeysPerTable, [&]() {
        for (std::uint64_t table_index = 0; table_index < tables; ++table_index) {
            SessionTable table(MakeConfig(4, 90000, kKeysPerTable));
            for (std::size_t key_index = 0; key_index < kKeysPerTable; ++key_index) {
                const std::string key = "k" + std::to_string(key_index);
                (void)table.AddServer(key, key_index + 1, 1000);
            }
        }
    });
}

/// AddClient 吞吐：每轮 1024 key × 4 client = 4096 次加入。
void BenchAddClient(std::uint64_t total_ops) {
    constexpr std::size_t kKeysPerTable = 1024;
    const std::uint64_t tables = (total_ops + kKeysPerTable * 4 - 1) / (kKeysPerTable * 4);
    Bench("add_client", tables * kKeysPerTable * 4, [&]() {
        for (std::uint64_t table_index = 0; table_index < tables; ++table_index) {
            SessionTable table(MakeConfig(4, 90000, kKeysPerTable));
            for (std::size_t key_index = 0; key_index < kKeysPerTable; ++key_index) {
                (void)table.AddServer("k" + std::to_string(key_index), key_index + 1, 1000);
            }
            for (std::uint64_t client = 1; client <= 4; ++client) {
                for (std::size_t key_index = 0; key_index < kKeysPerTable; ++key_index) {
                    (void)table.AddClient("k" + std::to_string(key_index), client, 1000);
                }
            }
        }
    });
}

/// Find 命中/未命中吞吐：1024 key 稳态表。
void BenchFind(std::uint64_t total_ops, bool hit) {
    SessionTable table(MakeConfig(4, 90000, 1024));
    for (std::size_t key_index = 0; key_index < 1024; ++key_index) {
        (void)table.AddServer("k" + std::to_string(key_index), key_index + 1, 1000);
    }
    std::mt19937 rng(42);
    std::vector<std::string> hit_keys;
    hit_keys.reserve(1024);
    for (std::size_t key_index = 0; key_index < 1024; ++key_index) {
        hit_keys.push_back("k" + std::to_string(key_index));
    }
    const std::string miss_key = "zzz-missing";
    std::uint64_t sink = 0;
    Bench(hit ? "find_hit" : "find_miss", total_ops, [&]() {
        for (std::uint64_t index = 0; index < total_ops; ++index) {
            const std::string& key = hit ? hit_keys[rng() % 1024U] : miss_key;
            const auto entry = table.Find(key, 1000);
            sink += entry.has_value() ? 1U : 0U;
        }
    });
    (void)sink;
}

/// List 吞吐：小表（8 key）与满表（1024 key）各一档。
void BenchList(std::uint64_t total_ops, std::size_t keys) {
    SessionTable table(MakeConfig(4, 90000, 1024));
    for (std::size_t key_index = 0; key_index < keys; ++key_index) {
        (void)table.AddServer("k" + std::to_string(key_index), key_index + 1, 1000);
        (void)table.AddClient("k" + std::to_string(key_index), 1, 1000);
    }
    std::uint64_t sink = 0;
    const std::uint64_t ops =
        keys >= 1024 ? std::max<std::uint64_t>(total_ops / 100, 1000) : total_ops;
    Bench(keys >= 1024 ? "list_1024" : "list_small", ops, [&]() {
        for (std::uint64_t index = 0; index < ops; ++index) {
            sink += table.List(1000).size();
        }
    });
    (void)sink;
}

/// Touch 稳态吞吐：1024 key，now 缓慢前进但永不越过租约（写路径仍会扫描）。
void BenchTouch(std::uint64_t total_ops) {
    SessionTable table(MakeConfig(4, 90000, 1024));
    for (std::size_t key_index = 0; key_index < 1024; ++key_index) {
        (void)table.AddServer("k" + std::to_string(key_index), key_index + 1, 0);
    }
    std::mt19937 rng(7);
    std::vector<std::string> keys;
    keys.reserve(1024);
    for (std::size_t key_index = 0; key_index < 1024; ++key_index) {
        keys.push_back("k" + std::to_string(key_index));
    }
    std::uint64_t now_ms = 1000;
    bool all_ok = true;
    Bench("touch_1024", total_ops, [&]() {
        for (std::uint64_t index = 0; index < total_ops; ++index) {
            now_ms += 1;
            all_ok = table.Touch(keys[rng() % 1024U], now_ms) && all_ok;
        }
    });
    AUDIT_CHECK(all_ok);
    AUDIT_CHECK_EQ(table.size(), 1024U);
}

/// Tick 空转（无过期）与全过期各一档。
void BenchTick(std::uint64_t total_ops, bool expire) {
    if (!expire) {
        SessionTable table(MakeConfig(4, 90000, 8));
        for (std::size_t key_index = 0; key_index < 8; ++key_index) {
            (void)table.AddServer("k" + std::to_string(key_index), key_index + 1, 0);
        }
        std::uint64_t removed = 0;
        Bench("tick_noop", total_ops, [&]() {
            for (std::uint64_t index = 0; index < total_ops; ++index) {
                removed += table.Tick(1000);
            }
        });
        AUDIT_CHECK_EQ(removed, 0U);
        return;
    }
    // 每轮重建 1024 条已过期条目，再一次性 Tick 清空。
    constexpr std::size_t kKeys = 1024;
    const std::uint64_t rounds = std::max<std::uint64_t>(total_ops / kKeys, 1);
    std::uint64_t removed = 0;
    Bench("tick_expire", rounds * kKeys, [&]() {
        for (std::uint64_t round = 0; round < rounds; ++round) {
            SessionTable table(MakeConfig(4, 1000, kKeys));
            for (std::size_t key_index = 0; key_index < kKeys; ++key_index) {
                (void)table.AddServer("k" + std::to_string(key_index), key_index + 1, 0);
            }
            removed += table.Tick(5000);
        }
    });
    AUDIT_CHECK_EQ(removed, rounds * kKeys);
}

/// 混合读写：1e6 次，模拟 16 线程之前先做单线程基线。
void BenchMixed(std::uint64_t total_ops) {
    SessionTable table(MakeConfig(4, 90000, 1024));
    std::mt19937 rng(99);
    std::uint64_t now_ms = 1000;
    std::uint64_t sink = 0;
    Bench("mixed_1t", total_ops, [&]() {
        for (std::uint64_t index = 0; index < total_ops; ++index) {
            const std::string key = "k" + std::to_string(rng() % 1024U);
            switch (rng() % 6U) {
                case 0:
                    sink += static_cast<std::uint64_t>(table.AddServer(key, 1, now_ms));
                    break;
                case 1:
                    sink +=
                        static_cast<std::uint64_t>(table.AddClient(key, rng() % 4U + 1, now_ms));
                    break;
                case 2:
                    sink += table.Touch(key, now_ms) ? 1U : 0U;
                    break;
                case 3:
                    sink += table.Find(key, now_ms).has_value() ? 1U : 0U;
                    break;
                case 4:
                    sink += table.List(now_ms).size();
                    break;
                default:
                    sink += table.Tick(now_ms);
                    break;
            }
            now_ms += 3;
        }
    });
    (void)sink;
    AUDIT_CHECK(table.size() <= 1024U);
}

void RunBench(double scale) {
    const auto scaled = [scale](std::uint64_t base) {
        return static_cast<std::uint64_t>(static_cast<double>(base) * scale);
    };
    const std::size_t rss_start = CurrentRssKb();
    BenchAddServer(scaled(1000000));
    BenchAddClient(scaled(1000000));
    BenchFind(scaled(1000000), true);
    BenchFind(scaled(1000000), false);
    BenchList(scaled(1000000), 8);
    BenchList(scaled(1000000), 1024);
    BenchTouch(scaled(1000000));
    BenchTick(scaled(1000000), false);
    BenchTick(scaled(1000000), true);
    BenchMixed(scaled(1000000));
    const std::size_t rss_end = CurrentRssKb();
    std::printf("[BENCH] rss_start=%zuKiB rss_end=%zuKiB peak=%zuKiB delta=%zuKiB\n", rss_start,
                rss_end, PeakRssKb(), rss_end > rss_start ? rss_end - rss_start : 0);
    if (!kSanitizerBuild && rss_end > rss_start + 65536) {
        std::fprintf(stderr, "[FAIL] RSS 增长超过 64MiB：%zuKiB -> %zuKiB\n", rss_start, rss_end);
        ++g_failures;
    }
    ++g_checks;
}

// ---------------------------------------------------------------------------
// 影子模型随机测试
// ---------------------------------------------------------------------------

/// 影子模型：与实现同语义的地图，用来逐操作比对。
struct ShadowEntry {
    std::uint64_t server_id = 0;
    std::vector<std::uint64_t> clients;
    std::uint64_t lease_deadline_ms = 0;
};

struct ShadowStats {
    std::uint64_t servers_added = 0;
    std::uint64_t clients_added = 0;
    std::uint64_t removals = 0;
    std::uint64_t expirations = 0;
    std::uint64_t duplicate_rejects = 0;
    std::uint64_t full_rejects = 0;
    std::uint64_t key_limit_rejects = 0;
    std::uint64_t not_found = 0;
    std::uint64_t touches = 0;
};

class ShadowModel {
   public:
    explicit ShadowModel(SessionTableConfig config) : m_config(config) {
        if (m_config.max_clients_per_key == 0) {
            m_config.max_clients_per_key = 1;
        }
        if (m_config.max_keys == 0) {
            m_config.max_keys = 1;
        }
    }

    bool IsExpired(const ShadowEntry& entry, std::uint64_t now_ms) const {
        return entry.lease_deadline_ms != 0 && now_ms >= entry.lease_deadline_ms;
    }

    void RemoveExpired(std::uint64_t now_ms) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (IsExpired(it->second, now_ms)) {
                it = m_entries.erase(it);
                ++m_stats.expirations;
            } else {
                ++it;
            }
        }
    }

    void Refresh(ShadowEntry& entry, std::uint64_t now_ms) const {
        entry.lease_deadline_ms = m_config.lease_ms == 0 ? 0 : now_ms + m_config.lease_ms;
    }

    SessionStatus AddServer(const std::string& key, std::uint64_t server_id,
                            std::uint64_t now_ms) {
        if (key.empty() || server_id == 0) {
            return SessionStatus::kInvalidArgument;
        }
        RemoveExpired(now_ms);
        if (m_entries.count(key) != 0) {
            ++m_stats.duplicate_rejects;
            return SessionStatus::kAlreadyExists;
        }
        if (m_entries.size() >= m_config.max_keys) {
            ++m_stats.key_limit_rejects;
            return SessionStatus::kKeyLimit;
        }
        ShadowEntry entry;
        entry.server_id = server_id;
        Refresh(entry, now_ms);
        m_entries.emplace(key, std::move(entry));
        ++m_stats.servers_added;
        return SessionStatus::kOk;
    }

    SessionStatus AddClient(const std::string& key, std::uint64_t client_id,
                            std::uint64_t now_ms) {
        if (key.empty() || client_id == 0) {
            return SessionStatus::kInvalidArgument;
        }
        RemoveExpired(now_ms);
        auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            ++m_stats.not_found;
            return SessionStatus::kNotFound;
        }
        if (std::find(it->second.clients.begin(), it->second.clients.end(), client_id) !=
            it->second.clients.end()) {
            ++m_stats.duplicate_rejects;
            return SessionStatus::kAlreadyExists;
        }
        if (it->second.clients.size() >= m_config.max_clients_per_key) {
            ++m_stats.full_rejects;
            return SessionStatus::kFull;
        }
        it->second.clients.push_back(client_id);
        Refresh(it->second, now_ms);
        ++m_stats.clients_added;
        return SessionStatus::kOk;
    }

    bool Remove(const std::string& key, std::uint64_t now_ms) {
        if (key.empty()) {
            return false;
        }
        RemoveExpired(now_ms);
        const auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            ++m_stats.not_found;
            return false;
        }
        m_entries.erase(it);
        ++m_stats.removals;
        return true;
    }

    SessionStatus RemoveClient(const std::string& key, std::uint64_t client_id,
                               std::uint64_t now_ms) {
        if (key.empty() || client_id == 0) {
            return SessionStatus::kInvalidArgument;
        }
        RemoveExpired(now_ms);
        const auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            ++m_stats.not_found;
            return SessionStatus::kNotFound;
        }
        auto& clients = it->second.clients;
        const auto client_it = std::find(clients.begin(), clients.end(), client_id);
        if (client_it == clients.end()) {
            ++m_stats.not_found;
            return SessionStatus::kNotFound;
        }
        clients.erase(client_it);
        Refresh(it->second, now_ms);
        ++m_stats.removals;
        return SessionStatus::kOk;
    }

    SessionStatus RemoveServer(const std::string& key, std::uint64_t server_id,
                               std::uint64_t now_ms) {
        if (key.empty() || server_id == 0) {
            return SessionStatus::kInvalidArgument;
        }
        RemoveExpired(now_ms);
        const auto it = m_entries.find(key);
        if (it == m_entries.end() || it->second.server_id != server_id) {
            ++m_stats.not_found;
            return SessionStatus::kNotFound;
        }
        m_entries.erase(it);
        ++m_stats.removals;
        return SessionStatus::kOk;
    }

    bool Touch(const std::string& key, std::uint64_t now_ms) {
        if (key.empty()) {
            return false;
        }
        RemoveExpired(now_ms);
        const auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            ++m_stats.not_found;
            return false;
        }
        Refresh(it->second, now_ms);
        ++m_stats.touches;
        return true;
    }

    std::optional<ShadowEntry> Find(const std::string& key, std::uint64_t now_ms) const {
        if (key.empty()) {
            return std::nullopt;
        }
        const auto it = m_entries.find(key);
        if (it == m_entries.end() || IsExpired(it->second, now_ms)) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<std::pair<std::string, ShadowEntry>> List(std::uint64_t now_ms) const {
        std::vector<std::pair<std::string, ShadowEntry>> out;
        for (const auto& pair : m_entries) {
            if (!IsExpired(pair.second, now_ms)) {
                out.emplace_back(pair.first, pair.second);
            }
        }
        return out;
    }

    bool Contains(const std::string& key, std::uint64_t now_ms) const {
        if (key.empty()) {
            return false;
        }
        const auto it = m_entries.find(key);
        return it != m_entries.end() && !IsExpired(it->second, now_ms);
    }

    std::size_t size() const { return m_entries.size(); }

    std::size_t Tick(std::uint64_t now_ms) {
        std::size_t removed = 0;
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (IsExpired(it->second, now_ms)) {
                it = m_entries.erase(it);
                ++removed;
                ++m_stats.expirations;
            } else {
                ++it;
            }
        }
        return removed;
    }

    const ShadowStats& stats() const { return m_stats; }

   private:
    SessionTableConfig m_config;
    std::map<std::string, ShadowEntry> m_entries;
    ShadowStats m_stats;
};

/// 比对表与影子模型的整表状态（List 顺序、entry 字段、计数）。
void CompareFullState(const SessionTable& table, const ShadowModel& shadow,
                      std::uint64_t now_ms) {
    const auto list = table.List(now_ms);
    const auto expected = shadow.List(now_ms);
    AUDIT_CHECK_EQ(list.size(), expected.size());
    AUDIT_CHECK_EQ(table.size(), shadow.size());
    const std::size_t common = std::min(list.size(), expected.size());
    for (std::size_t index = 0; index < common; ++index) {
        AUDIT_CHECK_EQ(list[index].key, expected[index].first);
        AUDIT_CHECK_EQ(list[index].server_id, expected[index].second.server_id);
        AUDIT_CHECK_EQ(list[index].ClientCount(), expected[index].second.clients.size());
        AUDIT_CHECK_EQ(list[index].lease_deadline_ms, expected[index].second.lease_deadline_ms);
        for (std::size_t client_index = 0; client_index < list[index].ClientCount();
             ++client_index) {
            AUDIT_CHECK_EQ(list[index].clients[client_index].client_id,
                           expected[index].second.clients[client_index]);
        }
    }
}

void CompareStats(const SessionTable& table, const ShadowModel& shadow) {
    const auto actual = table.stats();
    const auto& expected = shadow.stats();
    AUDIT_CHECK_EQ(actual.servers_added, expected.servers_added);
    AUDIT_CHECK_EQ(actual.clients_added, expected.clients_added);
    AUDIT_CHECK_EQ(actual.removals, expected.removals);
    AUDIT_CHECK_EQ(actual.expirations, expected.expirations);
    AUDIT_CHECK_EQ(actual.duplicate_rejects, expected.duplicate_rejects);
    AUDIT_CHECK_EQ(actual.full_rejects, expected.full_rejects);
    AUDIT_CHECK_EQ(actual.key_limit_rejects, expected.key_limit_rejects);
    AUDIT_CHECK_EQ(actual.not_found, expected.not_found);
    AUDIT_CHECK_EQ(actual.touches, expected.touches);
}

/// 单个种子的随机操作序列；每个操作后对比影子模型。
void RunRandomSeed(std::uint32_t seed, int operations) {
    std::mt19937 rng(seed);
    SessionTableConfig config = MakeConfig(4, 90000, 64);
    SessionTable table(config);
    ShadowModel shadow(config);
    std::uint64_t now_ms = 1000;

    for (int step = 0; step < operations; ++step) {
        // 时间推进：多数是小步，偶尔跨过 90s 租约，制造过期。
        now_ms += (rng() % 10U == 0) ? (rng() % 120000U) : (rng() % 2000U);
        const std::string key = "k" + std::to_string(rng() % 80U);
        const std::uint64_t client_id = (rng() % 8U) + 1;
        const std::uint64_t server_id = (rng() % 8U) + 1;
        SessionStatus actual_status = SessionStatus::kOk;

        switch (rng() % 12U) {
            case 0:
            case 1:
                actual_status = table.AddServer(key, server_id, now_ms);
                AUDIT_CHECK_EQ(actual_status, shadow.AddServer(key, server_id, now_ms));
                break;
            case 2:
            case 3:
            case 4:
                actual_status = table.AddClient(key, client_id, now_ms);
                AUDIT_CHECK_EQ(actual_status, shadow.AddClient(key, client_id, now_ms));
                break;
            case 5:
            case 6:
                actual_status = table.RemoveClient(key, client_id, now_ms);
                AUDIT_CHECK_EQ(actual_status, shadow.RemoveClient(key, client_id, now_ms));
                break;
            case 7:
                AUDIT_CHECK_EQ(table.Remove(key, now_ms), shadow.Remove(key, now_ms));
                break;
            case 8:
                AUDIT_CHECK_EQ(table.RemoveServer(key, server_id, now_ms),
                               shadow.RemoveServer(key, server_id, now_ms));
                break;
            case 9:
                AUDIT_CHECK_EQ(table.Touch(key, now_ms), shadow.Touch(key, now_ms));
                break;
            case 10: {
                const auto actual = table.Find(key, now_ms);
                const auto expected = shadow.Find(key, now_ms);
                AUDIT_CHECK_EQ(actual.has_value(), expected.has_value());
                if (actual.has_value() && expected.has_value()) {
                    AUDIT_CHECK_EQ(actual->server_id, expected->server_id);
                    AUDIT_CHECK_EQ(actual->ClientCount(), expected->clients.size());
                    AUDIT_CHECK_EQ(actual->lease_deadline_ms, expected->lease_deadline_ms);
                }
                AUDIT_CHECK_EQ(table.Contains(key, now_ms), shadow.Contains(key, now_ms));
                break;
            }
            default:
                AUDIT_CHECK_EQ(table.Tick(now_ms), shadow.Tick(now_ms));
                break;
        }

        // 高概率做轻量一致性检查，低概率做整表比对。
        AUDIT_CHECK_EQ(table.size(), shadow.size());
        if (step % 97 == 0) {
            CompareFullState(table, shadow, now_ms);
        }
        if (step % 499 == 0) {
            CompareStats(table, shadow);
        }
        if (step % 13 == 0) {
            const std::string probe = "k" + std::to_string(rng() % 80U);
            AUDIT_CHECK_EQ(table.Contains(probe, now_ms), shadow.Contains(probe, now_ms));
        }
    }
    CompareFullState(table, shadow, now_ms);
    CompareStats(table, shadow);
}

void RunRandom(double scale, std::uint32_t base_seed) {
    const int seeds = std::max(1, static_cast<int>(20 * scale));
    const int operations = std::max(200, static_cast<int>(5000 * scale));
    for (int index = 0; index < seeds; ++index) {
        RunRandomSeed(base_seed + static_cast<std::uint32_t>(index), operations);
    }
    std::printf("[RANDOM] seeds=%d ops/seed=%d total=%lld checks=%ld failures=%d\n", seeds,
                operations, static_cast<long long>(seeds) * operations, g_checks, g_failures);
}

// ---------------------------------------------------------------------------
// 真实场景
// ---------------------------------------------------------------------------

/// 场景 1：1 server + 4 client，第 5 个拒绝；移除一个后名额可复用。
void ScenarioClientLimit() {
    SessionTable table(MakeConfig(4, 90000, 16));
    AUDIT_CHECK_EQ(table.AddServer("invite-A", 7001, 1000), SessionStatus::kOk);
    for (std::uint64_t viewer = 101; viewer <= 104; ++viewer) {
        AUDIT_CHECK_EQ(table.AddClient("invite-A", viewer, 1000), SessionStatus::kOk);
    }
    AUDIT_CHECK_EQ(table.AddClient("invite-A", 105, 1000), SessionStatus::kFull);
    AUDIT_CHECK_EQ(table.AddClient("invite-A", 106, 1000), SessionStatus::kFull);
    AUDIT_CHECK_EQ(table.stats().full_rejects, 2U);
    const auto entry = table.Find("invite-A", 1000);
    AUDIT_CHECK(entry.has_value());
    AUDIT_CHECK_EQ(entry->ClientCount(), 4U);
    AUDIT_CHECK(entry->HasClient(101) && entry->HasClient(102) && entry->HasClient(103) &&
                entry->HasClient(104));
    AUDIT_CHECK(!entry->HasClient(105));

    // Viewer 离开后名额回收，第 5 个真正的 Viewer 可以进来。
    AUDIT_CHECK_EQ(table.RemoveClient("invite-A", 102, 2000), SessionStatus::kOk);
    AUDIT_CHECK_EQ(table.AddClient("invite-A", 105, 2000), SessionStatus::kOk);
    const auto after = table.Find("invite-A", 2000);
    AUDIT_CHECK(after.has_value());
    AUDIT_CHECK_EQ(after->ClientCount(), 4U);
    AUDIT_CHECK(after->HasClient(105));
    AUDIT_CHECK(!after->HasClient(102));
    std::printf("[SCENARIO 1] 1:N 上限：4 成功 / 第 5 拒绝 / 移除后补位成功\n");
}

/// 场景 2：90s 租约边界的假时钟精测。
void ScenarioLeaseBoundary() {
    constexpr std::uint64_t kLease = 90000;
    SessionTable table(MakeConfig(4, kLease, 16));
    AUDIT_CHECK_EQ(table.AddServer("host", 1, 1000), SessionStatus::kOk);
    // 边界：deadline 前 1ms 有效，deadline 当刻即过期。
    AUDIT_CHECK(table.Find("host", 1000 + kLease - 1).has_value());
    AUDIT_CHECK(!table.Find("host", 1000 + kLease).has_value());
    AUDIT_CHECK(!table.Contains("host", 1000 + kLease));
    // 读路径不删除、不续租。
    AUDIT_CHECK_EQ(table.size(), 1U);
    AUDIT_CHECK(table.Find("host", 0).has_value());  // 回拨时间也不改写 deadline
    AUDIT_CHECK_EQ(table.Tick(1000 + kLease), 1U);
    AUDIT_CHECK_EQ(table.size(), 0U);

    // Touch 在临过期前续租：94999 + 90000 = 184999。
    AUDIT_CHECK_EQ(table.AddServer("host", 2, 5000), SessionStatus::kOk);
    AUDIT_CHECK(table.Touch("host", 94999));
    AUDIT_CHECK(table.Find("host", 94999 + kLease - 1).has_value());
    AUDIT_CHECK(!table.Find("host", 94999 + kLease).has_value());

    // AddClient 续租。
    SessionTable add_client(MakeConfig(4, kLease, 16));
    AUDIT_CHECK_EQ(add_client.AddServer("h", 1, 0), SessionStatus::kOk);
    AUDIT_CHECK_EQ(add_client.AddClient("h", 9, 80000), SessionStatus::kOk);
    AUDIT_CHECK(add_client.Find("h", 80000 + kLease - 1).has_value());
    AUDIT_CHECK(!add_client.Find("h", 80000 + kLease).has_value());

    // RemoveClient 也续租（写路径活跃），验证文档声明。
    SessionTable remove_client(MakeConfig(4, kLease, 16));
    AUDIT_CHECK_EQ(remove_client.AddServer("h", 1, 0), SessionStatus::kOk);
    AUDIT_CHECK_EQ(remove_client.AddClient("h", 9, 0), SessionStatus::kOk);
    AUDIT_CHECK_EQ(remove_client.RemoveClient("h", 9, 70000), SessionStatus::kOk);
    AUDIT_CHECK(remove_client.Find("h", 70000 + kLease - 1).has_value());
    AUDIT_CHECK(!remove_client.Find("h", 70000 + kLease).has_value());

    // 过期后重注册，拿到干净条目。
    AUDIT_CHECK_EQ(table.AddServer("host", 3, 500000), SessionStatus::kOk);
    const auto fresh = table.Find("host", 500000);
    AUDIT_CHECK(fresh.has_value());
    AUDIT_CHECK_EQ(fresh->server_id, 3U);
    AUDIT_CHECK_EQ(fresh->ClientCount(), 0U);
    std::printf(
        "[SCENARIO 2] 90s 租约：89999 有效 / 90000 过期 / Touch+AddClient+RemoveClient "
        "续租 / 过期重注册\n");
}

/// 场景 3：16 线程并发注册 + 查询 + 清理，验证最终一致性。
void ScenarioConcurrent() {
    constexpr int kThreads = 16;
    constexpr std::size_t kKeys = 8;
    SessionTable table(MakeConfig(4, 90000, kKeys));
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> register_ok{0};
    std::atomic<int> register_duplicate{0};
    std::atomic<int> query_reads{0};
    std::atomic<int> unexpected{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index]() {
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::mt19937 rng(static_cast<std::uint32_t>(index * 131 + 7));
            if (index < 8) {
                // 8 个写线程：各自负责一个 key 的 server 注册，随后加 client。
                const std::string key = "k" + std::to_string(index);
                for (int attempt = 0; attempt < 50; ++attempt) {
                    const auto status =
                        table.AddServer(key, static_cast<std::uint64_t>(index + 1), 1000);
                    if (status == SessionStatus::kOk) {
                        register_ok.fetch_add(1);
                    } else if (status == SessionStatus::kAlreadyExists) {
                        register_duplicate.fetch_add(1);
                    } else {
                        unexpected.fetch_add(1);
                    }
                }
                for (std::uint64_t client = 1; client <= 4; ++client) {
                    (void)table.AddClient(key, client, 1000);
                }
            } else {
                // 8 个读线程：查/列/续租/清理混合。
                for (int step = 0; step < 5000; ++step) {
                    const std::string key = "k" + std::to_string(rng() % kKeys);
                    switch (rng() % 5U) {
                        case 0:
                            (void)table.Find(key, 1000);
                            break;
                        case 1:
                            (void)table.List(1000);
                            break;
                        case 2:
                            (void)table.Touch(key, 1000);
                            break;
                        case 3:
                            (void)table.Contains(key, 1000);
                            break;
                        default:
                            (void)table.Tick(1000);
                            break;
                    }
                    query_reads.fetch_add(1);
                }
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
    // 8 个写线程 × 50 次尝试，每个 key 恰好 1 次成功、其余 49 次重复拒绝。
    AUDIT_CHECK_EQ(register_ok.load(), 8);
    AUDIT_CHECK_EQ(register_duplicate.load(), 8 * 49);
    AUDIT_CHECK_EQ(unexpected.load(), 0);
    AUDIT_CHECK_EQ(table.size(), kKeys);
    const auto list = table.List(1000);
    AUDIT_CHECK_EQ(list.size(), kKeys);
    for (const auto& entry : list) {
        AUDIT_CHECK_EQ(entry.ClientCount(), 4U);
        AUDIT_CHECK_EQ(entry.server_id != 0, true);
    }
    AUDIT_CHECK_EQ(table.stats().servers_added, 8U);
    AUDIT_CHECK(query_reads.load() > 0);
    AUDIT_CHECK_EQ(table.Tick(1000000), kKeys);  // 全部过期后清空
    AUDIT_CHECK(table.empty());
    std::printf(
        "[SCENARIO 3] 16 线程并发：register_ok=8 dup=%d reads=%d 最终 8 key × 4 client\n",
        register_duplicate.load(), query_reads.load());
}

/// 场景 4：重复 key 注册的拒绝语义（串行 + 并发同 key）。
void ScenarioDuplicateKey() {
    SessionTable table(MakeConfig(4, 90000, 4));
    AUDIT_CHECK_EQ(table.AddServer("dup", 1, 1000), SessionStatus::kOk);
    // 相同 server_id 重放：仍是 kAlreadyExists（非幂等 kOk），文档口径一致。
    AUDIT_CHECK_EQ(table.AddServer("dup", 1, 1000), SessionStatus::kAlreadyExists);
    AUDIT_CHECK_EQ(table.AddServer("dup", 2, 1000), SessionStatus::kAlreadyExists);
    const auto entry = table.Find("dup", 1000);
    AUDIT_CHECK(entry.has_value());
    AUDIT_CHECK_EQ(entry->server_id, 1U);  // 首个注册者保留，未被后者抢占
    AUDIT_CHECK_EQ(table.stats().duplicate_rejects, 2U);

    // 16 线程并发抢同一 key：恰好 1 个成功。
    constexpr int kThreads = 16;
    SessionTable race(MakeConfig(4, 90000, 4));
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> ok{0};
    std::atomic<int> dup{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&, index]() {
            ready.fetch_add(1);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto status =
                race.AddServer("race", static_cast<std::uint64_t>(index + 100), 1000);
            if (status == SessionStatus::kOk) {
                ++ok;
            } else if (status == SessionStatus::kAlreadyExists) {
                ++dup;
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
    AUDIT_CHECK_EQ(ok.load(), 1);
    AUDIT_CHECK_EQ(dup.load(), kThreads - 1);
    const auto winner = race.Find("race", 1000);
    AUDIT_CHECK(winner.has_value());
    AUDIT_CHECK(winner->server_id >= 100 && winner->server_id < 100 + kThreads);
    std::printf(
        "[SCENARIO 4] 重复 key：串行拒绝且首个 server 保留；16 线程抢注 1 成功 15 拒绝\n");
}

/// 场景 5：60s 长稳（真实时钟），随机增删 + 周期 Tick，RSS 不漂移。
void ScenarioSoak(int seconds) {
    SessionTable table(MakeConfig(4, 90000, 256));
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ops{0};
    std::atomic<std::uint64_t> rss_peak{0};
    std::vector<std::thread> workers;
    const int kWorkers = 4;
    workers.reserve(static_cast<std::size_t>(kWorkers));
    for (int index = 0; index < kWorkers; ++index) {
        workers.emplace_back([&, index]() {
            std::mt19937 rng(static_cast<std::uint32_t>(index * 977 + 3));
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint64_t now_ms = go2cpp::bento::NowMs();
                const std::string key = "soak" + std::to_string(rng() % 256U);
                switch (rng() % 8U) {
                    case 0:
                        (void)table.AddServer(key, (rng() % 1000U) + 1, now_ms);
                        break;
                    case 1:
                    case 2:
                        (void)table.AddClient(key, (rng() % 8U) + 1, now_ms);
                        break;
                    case 3:
                        (void)table.RemoveClient(key, (rng() % 8U) + 1, now_ms);
                        break;
                    case 4:
                        (void)table.Touch(key, now_ms);
                        break;
                    case 5:
                        (void)table.Find(key, now_ms);
                        break;
                    case 6:
                        (void)table.List(now_ms);
                        break;
                    default:
                        (void)table.Tick(now_ms);
                        break;
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    const auto begin = Clock::now();
    std::uint64_t last_ops = 0;
    std::size_t rss_start = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const std::uint64_t now_ops = ops.load();
        const std::size_t rss = CurrentRssKb();
        if (rss_start == 0) {
            rss_start = rss;
        }
        std::printf("[SOAK] t=%3llus ops=%llu (+%llu) rss=%zuKiB\n",
                    static_cast<unsigned long long>(
                        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - begin)
                            .count()),
                    static_cast<unsigned long long>(now_ops),
                    static_cast<unsigned long long>(now_ops - last_ops), rss);
        last_ops = now_ops;
        std::uint64_t peak = rss_peak.load();
        while (rss > peak && !rss_peak.compare_exchange_weak(peak, rss)) {
        }
        if (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - begin).count() >=
            seconds) {
            break;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& thread : workers) {
        thread.join();
    }
    const std::size_t rss_end = CurrentRssKb();
    // 收尾：全部过期后必须清空，验证长稳期间没有“删不掉”的条目。
    const std::size_t remaining = table.size();
    AUDIT_CHECK_EQ(table.Tick(UINT64_MAX), remaining);
    AUDIT_CHECK(table.empty());
    AUDIT_CHECK(ops.load() > 100000);
    std::printf("[SCENARIO 5] 60s 长稳：ops=%llu rss_start=%zuKiB rss_peak=%lluKiB rss_end=%zuKiB\n",
                static_cast<unsigned long long>(ops.load()), rss_start,
                static_cast<unsigned long long>(rss_peak.load()), rss_end);
    if (!kSanitizerBuild && rss_peak.load() > 65536) {
        std::fprintf(stderr, "[FAIL] 长稳 RSS 峰值超过 64MiB：%lluKiB\n",
                     static_cast<unsigned long long>(rss_peak.load()));
        ++g_failures;
    }
    ++g_checks;
}

// ---------------------------------------------------------------------------
// 泄漏模式
// ---------------------------------------------------------------------------

/// 大量构造/析构 + 增删，配合 ASan/valgrind 检查泄漏。
void RunLeaks(double scale) {
    const int rounds = std::max(10, static_cast<int>(2000 * scale));
    for (int round = 0; round < rounds; ++round) {
        SessionTable table(MakeConfig(4, 1000, 64));
        for (std::uint64_t key = 0; key < 64; ++key) {
            (void)table.AddServer("k" + std::to_string(key), key + 1, 0);
            for (std::uint64_t client = 1; client <= 4; ++client) {
                (void)table.AddClient("k" + std::to_string(key), client, 0);
            }
        }
        (void)table.List(5000);  // 全部过期
        (void)table.Tick(5000);
        for (std::uint64_t key = 0; key < 32; ++key) {
            (void)table.Remove("k" + std::to_string(key), 5000);
        }
    }
    // 长驻表：反复扩容/删除同一批 key，验证 vector/map 节点不泄漏。
    SessionTable table(MakeConfig(4, 0, 128));
    for (int round = 0; round < rounds; ++round) {
        for (std::uint64_t key = 0; key < 128; ++key) {
            const std::string name = "long" + std::to_string(key);
            (void)table.AddServer(name, key + 1, 0);
            for (std::uint64_t client = 1; client <= 4; ++client) {
                (void)table.AddClient(name, client, 0);
            }
        }
        for (std::uint64_t key = 0; key < 128; ++key) {
            (void)table.Remove("long" + std::to_string(key), 0);
        }
    }
    AUDIT_CHECK(table.empty());
    std::printf("[LEAKS] rounds=%d finished rss=%zuKiB peak=%zuKiB\n", rounds, CurrentRssKb(),
                PeakRssKb());
}

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------

std::string ArgValue(int argc, char** argv, const char* flag, const char* fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], flag) == 0) {
            return argv[index + 1];
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = ArgValue(argc, argv, "--mode", "all");
    const double scale =
        std::max(0.001, std::atof(ArgValue(argc, argv, "--scale", "1.0").c_str()));
    const std::uint32_t seed = static_cast<std::uint32_t>(std::strtoul(
        ArgValue(argc, argv, "--seed", "20261029").c_str(), nullptr, 10));
    const int seconds = std::max(1, std::atoi(ArgValue(argc, argv, "--seconds", "60").c_str()));

    std::printf("[INFO] mode=%s scale=%.3f seed=%u\n", mode.c_str(), scale, seed);
    const auto begin = Clock::now();

    if (mode == "bench" || mode == "all") {
        RunBench(scale);
    }
    if (mode == "random" || mode == "all") {
        RunRandom(scale, seed);
    }
    if (mode == "scenarios" || mode == "all") {
        ScenarioClientLimit();
        ScenarioLeaseBoundary();
        ScenarioConcurrent();
        ScenarioDuplicateKey();
        if (mode == "all" && scale >= 1.0) {
            ScenarioSoak(seconds);
        }
    }
    if (mode == "soak") {
        ScenarioSoak(seconds);
    }
    if (mode == "leaks" || mode == "all") {
        RunLeaks(scale);
    }

    const double elapsed =
        std::chrono::duration_cast<std::chrono::duration<double>>(Clock::now() - begin).count();
    std::printf("[SUMMARY] checks=%ld failures=%d elapsed=%.2fs peak_rss=%zuKiB\n", g_checks,
                g_failures, elapsed, PeakRssKb());
    if (g_failures != 0) {
        std::fprintf(stderr, "[FAIL] %d 处断言失败\n", g_failures);
        return 1;
    }
    std::printf("[PASS] audit_session_table_stress\n");
    return 0;
}
