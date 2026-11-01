/**
 * @file tests/config/test_config_watch.cpp
 * @brief 配置热更新（`go2cpp/config/watch.hpp`）的场景/压测/随机测试。
 *
 * ## 覆盖
 *
 * 1. **单次修改即生效**：写文件后版本 +1、快照字段完整；
 * 2. **1e4 次连续变更**：每次写入后等待版本前进，验证 10000 次全部生效、
 *    `failed_reloads() == 0`，并统计总耗时与平均延迟；
 * 3. **原子替换**：写 `.tmp` 再 `rename` 覆盖，验证 `IN_MOVED_TO` 路径生效；
 * 4. **非法配置**：loader 拒绝时旧配置保留、失败计数 +1；恢复合法内容后
 *    继续热更新；validator 拒绝路径同样覆盖；
 * 5. **删除 / 重建**：文件删除后旧配置继续服务，重建后自动恢复更新；
 * 6. **并发无锁读**：4 个读者线程持续 `Load()`，写者发布 2000 次配置，
 *    校验 `a + b == sum` 不变量，统计读次数与撕裂次数（必须为 0）；
 * 7. **轮询回退**：`prefer_inotify=false` 时走轮询分支，50 次变更全部生效；
 * 8. **读侧基准**：单线程 1e6 次 `Load()` 与 4 读 1 写并发下的读延迟；
 * 9. **Stop 语义**：停止后文件变化不再触发重载，析构不悬挂。
 *
 * ## 运行
 * ```sh
 * g++ -std=c++17 -O2 -Iinclude tests/config/test_config_watch.cpp -pthread \
 *     -o /tmp/test_config_watch
 * /tmp/test_config_watch
 * ```
 * 本测试只依赖头文件与标准库，不需要额外链接库。
 */
#include "go2cpp/config/watch.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::config::ConfigStore;
using go2cpp::config::ConfigWatcher;
using go2cpp::config::FileWatchOptions;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "[config-watch] FAILED %s:%d: %s\n", file, line,
                 expression);
}

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

class TempDir final {
public:
    explicit TempDir(const char* tag) {
        std::string pattern = std::string("/tmp/go2cpp-config-watch-") + tag +
                              "-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        if (created != nullptr) {
            m_path = created;
        }
    }

    ~TempDir() {
        if (!m_path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const {
        return m_path + "/" + name;
    }

private:
    std::string m_path;
};

/// 测试配置：`a + b == sum` 是跨字段一致性不变量。
struct Settings {
    std::uint64_t generation{0};
    std::string name;
    std::int64_t a{0};
    std::int64_t b{0};
    std::int64_t sum{0};
};

/// 解析 INI 风格的简单配置；非法内容返回 nullptr。
std::shared_ptr<const Settings> LoadSettings(const std::string& path,
                                             std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "无法打开配置文件: " + path;
        }
        return nullptr;
    }
    Settings settings;
    std::string line;
    try {
        while (std::getline(input, line)) {
            if (line.rfind("generation=", 0) == 0) {
                settings.generation = std::stoull(line.substr(11));
            } else if (line.rfind("name=", 0) == 0) {
                settings.name = line.substr(5);
            } else if (line.rfind("a=", 0) == 0) {
                settings.a = std::stoll(line.substr(2));
            } else if (line.rfind("b=", 0) == 0) {
                settings.b = std::stoll(line.substr(2));
            } else if (line.rfind("sum=", 0) == 0) {
                settings.sum = std::stoll(line.substr(4));
            }
        }
    } catch (const std::exception&) {
        if (error != nullptr) {
            *error = "配置字段不是合法整数";
        }
        return nullptr;
    }
    if (settings.generation == 0) {
        if (error != nullptr) {
            *error = "缺少合法 generation";
        }
        return nullptr;
    }
    return std::make_shared<Settings>(settings);
}

/// 覆盖写配置；返回是否全部写入成功。
bool WriteSettings(const std::string& path, std::uint64_t generation,
                   const std::string& name = "cfg") {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return false;
    }
    output << "generation=" << generation << "\n"
           << "name=" << name << "-" << generation << "\n"
           << "a=" << generation << "\n"
           << "b=" << generation * 2 << "\n"
           << "sum=" << generation * 3 << "\n";
    output.flush();
    return output.good();
}

/// 原子替换：先写 `.tmp` 再 rename，覆盖编辑器/Vim 的常见保存方式。
bool WriteSettingsAtomic(const std::string& path, std::uint64_t generation) {
    const std::string temporary = path + ".tmp";
    if (!WriteSettings(temporary, generation)) {
        return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

bool WaitForVersion(const ConfigWatcher<Settings>& watcher,
                    std::uint64_t target,
                    std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (watcher.version() >= target) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool WaitForFailedReloads(const ConfigWatcher<Settings>& watcher,
                          std::uint64_t target,
                          std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (watcher.failed_reloads() >= target) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool WaitForGeneration(const ConfigWatcher<Settings>& watcher,
                       std::uint64_t generation,
                       std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        const auto snapshot = watcher.Load();
        if (snapshot && snapshot->generation == generation) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void Announce(const char* name) {
    std::cout << "[test] " << name << std::endl;
}

// ---------------------------------------------------------------------------
// 场景 1：单次修改即生效 + 快照语义
// ---------------------------------------------------------------------------
void ScenarioBasicReload() {
    Announce("single change reloads atomically");
    TempDir temp("basic");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettings(path, 1));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    std::string error;
    CHECK(watcher.Start(&error));
    if (!watcher.running()) {
        std::fprintf(stderr, "[config-watch] Start 失败: %s\n", error.c_str());
    }
    CHECK(watcher.version() == 1);

    {
        auto snapshot = watcher.Load();
        CHECK(static_cast<bool>(snapshot));
        CHECK(snapshot->generation == 1);
        CHECK(snapshot->name == "cfg-1");
        CHECK(snapshot->a + snapshot->b == snapshot->sum);
        auto shared = watcher.LoadShared();
        CHECK(static_cast<bool>(shared));
    }
    CHECK(watcher.reloads() == 1);
    CHECK(watcher.failed_reloads() == 0);
    CHECK(watcher.last_error().empty());

    CHECK(WriteSettings(path, 2));
    CHECK(WaitForVersion(watcher, 2, std::chrono::seconds(5)));
    CHECK(watcher.version() == 2);
    auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot->generation == 2);
    CHECK(snapshot->name == "cfg-2");
    CHECK(watcher.reloads() == 2);
    CHECK(watcher.failed_reloads() == 0);

    // 拷贝/移动语义：同一槽位增加引用，写者不会覆盖正在读的数据。
    auto copy = snapshot;
    auto moved = std::move(copy);
    CHECK(static_cast<bool>(moved));
    CHECK(moved->generation == 2);
    CHECK(snapshot->generation == 2);

    watcher.Stop();
    CHECK(!watcher.running());
}

// ---------------------------------------------------------------------------
// 场景 2：1e4 次连续变更
// ---------------------------------------------------------------------------
void ScenarioTenThousandChanges() {
    Announce("10000 sequential changes");
    TempDir temp("tenk");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettings(path, 1));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    CHECK(watcher.Start());

    constexpr std::uint64_t kChanges = 10000;
    const auto start = Clock::now();
    std::uint64_t completed = 0;
    for (std::uint64_t i = 1; i <= kChanges; ++i) {
        if (!WriteSettingsAtomic(path, i)) {
            CHECK(false);
            break;
        }
        // 初始版本是 1；第 i 次变更生效后版本应为 i + 1。
        if (!WaitForVersion(watcher, i + 1, std::chrono::seconds(5))) {
            std::fprintf(stderr,
                         "[config-watch] 第 %llu 次变更超时（version=%llu）\n",
                         static_cast<unsigned long long>(i),
                         static_cast<unsigned long long>(watcher.version()));
            CHECK(false);
            break;
        }
        completed = i;
    }
    const auto end = Clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();

    CHECK(completed == kChanges);
    CHECK(watcher.version() == kChanges + 1);
    CHECK(watcher.reloads() == kChanges + 1);
    CHECK(watcher.failed_reloads() == 0);
    CHECK(watcher.last_error().empty());
    const auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot && snapshot->generation == kChanges);
    std::printf(
        "[bench] changes=%llu total=%.3fs avg=%.1fus inotify=%d events=%llu\n",
        static_cast<unsigned long long>(kChanges), seconds,
        seconds * 1e6 / static_cast<double>(kChanges),
        watcher.uses_inotify() ? 1 : 0,
        static_cast<unsigned long long>(watcher.change_events()));
    watcher.Stop();
}

// ---------------------------------------------------------------------------
// 场景 3：原子替换（tmp + rename）
// ---------------------------------------------------------------------------
void ScenarioAtomicReplace() {
    Announce("atomic replace via rename");
    TempDir temp("rename");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettingsAtomic(path, 1));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    CHECK(watcher.Start());
    CHECK(watcher.version() == 1);

    for (std::uint64_t i = 2; i <= 20; ++i) {
        CHECK(WriteSettingsAtomic(path, i));
        CHECK(WaitForVersion(watcher, i, std::chrono::seconds(5)));
    }
    CHECK(watcher.failed_reloads() == 0);
    const auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot && snapshot->generation == 20);
    watcher.Stop();
}

// ---------------------------------------------------------------------------
// 场景 4：非法配置保留旧值 + 恢复
// ---------------------------------------------------------------------------
void ScenarioInvalidConfigKeepsOldValue() {
    Announce("invalid config keeps previous snapshot");
    TempDir temp("invalid");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettings(path, 7));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    // validator：拒绝 name 以 "bad" 开头的新配置，覆盖校验失败分支。
    watcher.SetValidator([](const Settings& settings, std::string* error) {
        if (settings.name.rfind("bad", 0) == 0) {
            if (error != nullptr) {
                *error = "name 不允许以 bad 开头";
            }
            return false;
        }
        return true;
    });
    CHECK(watcher.Start());
    CHECK(watcher.version() == 1);

    // loader 失败：文件内容缺 generation。
    {
        std::ofstream output(path, std::ios::trunc);
        output << "not-a-config\n";
    }
    CHECK(WaitForFailedReloads(watcher, 1, std::chrono::seconds(5)));
    CHECK(watcher.version() == 1);
    CHECK(!watcher.last_error().empty());
    {
        auto snapshot = watcher.Load();
        CHECK(static_cast<bool>(snapshot));
        CHECK(snapshot && snapshot->generation == 7);
    }

    // validator 失败：loader 成功但内容被拒绝。
    CHECK(WriteSettings(path, 8, "bad-name"));
    CHECK(WaitForFailedReloads(watcher, 2, std::chrono::seconds(5)));
    CHECK(watcher.version() == 1);
    CHECK(watcher.last_error().find("bad") != std::string::npos);

    // 恢复合法内容后继续热更新。
    CHECK(WriteSettings(path, 9, "good-name"));
    CHECK(WaitForVersion(watcher, 2, std::chrono::seconds(5)));
    CHECK(watcher.version() == 2);
    CHECK(watcher.failed_reloads() == 2);
    const auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot && snapshot->generation == 9);
    CHECK(snapshot && snapshot->name == "good-name-9");
    watcher.Stop();
}

// ---------------------------------------------------------------------------
// 场景 5：删除 / 重建
// ---------------------------------------------------------------------------
void ScenarioDeleteAndRecreate() {
    Announce("delete keeps old value, recreate reloads");
    TempDir temp("delete");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettings(path, 3));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    CHECK(watcher.Start());
    CHECK(watcher.version() == 1);

    std::error_code error;
    CHECK(std::filesystem::remove(path, error));
    CHECK(WaitForFailedReloads(watcher, 1, std::chrono::seconds(5)));
    {
        auto snapshot = watcher.Load();
        CHECK(static_cast<bool>(snapshot));
        CHECK(snapshot && snapshot->generation == 3);
    }

    CHECK(WriteSettings(path, 4));
    CHECK(WaitForVersion(watcher, 2, std::chrono::seconds(5)));
    const auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot && snapshot->generation == 4);
    watcher.Stop();
}

// ---------------------------------------------------------------------------
// 场景 6：并发无锁读（跨字段一致性）
// ---------------------------------------------------------------------------
void ScenarioConcurrentReaders() {
    Announce("4 readers x 2000 published configs, no torn reads");
    TempDir temp("concurrent");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettings(path, 1));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    CHECK(watcher.Start());

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> torn{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&watcher, &stop, &reads, &torn]() {
            std::uint64_t local_reads = 0;
            std::uint64_t local_torn = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto snapshot = watcher.Load();
                if (!snapshot) {
                    continue;
                }
                if (snapshot->a + snapshot->b != snapshot->sum) {
                    ++local_torn;
                }
                ++local_reads;
            }
            reads.fetch_add(local_reads, std::memory_order_relaxed);
            torn.fetch_add(local_torn, std::memory_order_relaxed);
        });
    }

    constexpr std::uint64_t kChanges = 2000;
    const auto start = Clock::now();
    for (std::uint64_t i = 2; i <= kChanges; ++i) {
        // 原子替换：读者永远看到完整文件，不会因半截内容产生失败重载。
        CHECK(WriteSettingsAtomic(path, i));
    }
    CHECK(WaitForGeneration(watcher, kChanges, std::chrono::seconds(20)));
    const auto end = Clock::now();
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) {
        reader.join();
    }
    const double seconds = std::chrono::duration<double>(end - start).count();
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
    CHECK(watcher.failed_reloads() == 0);
    const auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot && snapshot->generation == kChanges);
    std::printf(
        "[bench] concurrent reads=%llu torn=%llu publish=%.3fs "
        "reads_per_publish=%.1f\n",
        static_cast<unsigned long long>(reads.load()),
        static_cast<unsigned long long>(torn.load()), seconds,
        static_cast<double>(reads.load()) / static_cast<double>(kChanges));
    watcher.Stop();
}

// ---------------------------------------------------------------------------
// 场景 7：轮询回退
// ---------------------------------------------------------------------------
void ScenarioPollingFallback() {
    Announce("polling fallback (prefer_inotify=false)");
    TempDir temp("polling");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettings(path, 1));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    options.watch.prefer_inotify = false;
    options.watch.poll_interval = std::chrono::milliseconds(1);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    CHECK(watcher.Start());
    CHECK(!watcher.uses_inotify());

    for (std::uint64_t i = 2; i <= 51; ++i) {
        // 轮询模式用原子替换：inode 变化可以被可靠检测（同尺寸原地覆盖
        // 在文件系统时间戳粒度内可能漏检，见头文件边界说明）。
        CHECK(WriteSettingsAtomic(path, i));
        CHECK(WaitForVersion(watcher, i, std::chrono::seconds(5)));
    }
    CHECK(watcher.failed_reloads() == 0);
    const auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot && snapshot->generation == 51);
    watcher.Stop();
}

// ---------------------------------------------------------------------------
// 场景 8：读侧基准（无锁 Load）
// ---------------------------------------------------------------------------
void ScenarioReadBenchmark() {
    Announce("benchmark: 1e6 lock-free snapshots");
    auto initial = std::make_shared<Settings>();
    initial->generation = 1;
    initial->a = 1;
    initial->b = 2;
    initial->sum = 3;
    ConfigStore<Settings> store(std::move(initial));

    constexpr std::uint64_t kReads = 1000000;
    std::uint64_t checksum = 0;
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < kReads; ++i) {
        const auto snapshot = store.Load();
        if (snapshot) {
            checksum += snapshot->generation;
        }
    }
    const auto end = Clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    CHECK(checksum == kReads);
    std::printf("[bench] store.load reads=%llu total=%.3fs ns_per_read=%.2f\n",
                static_cast<unsigned long long>(kReads), seconds,
                seconds * 1e9 / static_cast<double>(kReads));

    // 并发：4 读者持续 Load，写者发布 1e4 个新版本。
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> torn{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&store, &stop, &reads, &torn]() {
            std::uint64_t local_reads = 0;
            std::uint64_t local_torn = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto snapshot = store.Load();
                if (!snapshot) {
                    continue;
                }
                if (snapshot->a + snapshot->b != snapshot->sum) {
                    ++local_torn;
                }
                ++local_reads;
            }
            reads.fetch_add(local_reads, std::memory_order_relaxed);
            torn.fetch_add(local_torn, std::memory_order_relaxed);
        });
    }
    constexpr std::uint64_t kPublishes = 10000;
    const auto publish_start = Clock::now();
    for (std::uint64_t i = 2; i <= kPublishes; ++i) {
        auto value = std::make_shared<Settings>();
        value->generation = i;
        value->a = static_cast<std::int64_t>(i);
        value->b = static_cast<std::int64_t>(i * 2);
        value->sum = static_cast<std::int64_t>(i * 3);
        store.Store(std::move(value));
    }
    const auto publish_end = Clock::now();
    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) {
        reader.join();
    }
    const double publish_seconds =
        std::chrono::duration<double>(publish_end - publish_start).count();
    CHECK(torn.load() == 0);
    CHECK(store.version() == kPublishes);
    std::printf(
        "[bench] store.store publishes=%llu total=%.3fs avg=%.1fus "
        "reads=%llu torn=%llu\n",
        static_cast<unsigned long long>(kPublishes), publish_seconds,
        publish_seconds * 1e6 / static_cast<double>(kPublishes),
        static_cast<unsigned long long>(reads.load()),
        static_cast<unsigned long long>(torn.load()));
}

// ---------------------------------------------------------------------------
// 场景 9：Stop 语义
// ---------------------------------------------------------------------------
void ScenarioStop() {
    Announce("stop prevents further reloads");
    TempDir temp("stop");
    const std::string path = temp.file("app.ini");
    CHECK(WriteSettings(path, 1));

    ConfigWatcher<Settings>::Options options;
    options.debounce = std::chrono::milliseconds(0);
    ConfigWatcher<Settings> watcher(path, &LoadSettings, options);
    CHECK(watcher.Start());
    CHECK(watcher.version() == 1);
    watcher.Stop();
    CHECK(!watcher.running());

    CHECK(WriteSettings(path, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(watcher.version() == 1);
    // 停止后仍可手动重载（用于运维触发的「立即重新读取」）。
    CHECK(watcher.ReloadNow());
    CHECK(watcher.version() == 2);
    const auto snapshot = watcher.Load();
    CHECK(static_cast<bool>(snapshot));
    CHECK(snapshot && snapshot->generation == 2);
}

}  // namespace

int main() {
    const auto start = Clock::now();
    ScenarioBasicReload();
    ScenarioTenThousandChanges();
    ScenarioAtomicReplace();
    ScenarioInvalidConfigKeepsOldValue();
    ScenarioDeleteAndRecreate();
    ScenarioConcurrentReaders();
    ScenarioPollingFallback();
    ScenarioReadBenchmark();
    ScenarioStop();
    const double seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    if (g_failures == 0) {
        std::printf("[config-watch] PASS (%.2fs)\n", seconds);
        return 0;
    }
    std::printf("[config-watch] FAIL (%d checks, %.2fs)\n", g_failures, seconds);
    return 1;
}
