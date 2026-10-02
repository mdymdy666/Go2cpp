#include "go2cpp/config.hpp"
#include "go2cpp/log.hpp"
#include "test_support.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

/**
 * @brief 验证日志格式化、并发写入和动态配置热加载。
 * @return 无；测试失败由统一断言统计。
 */
void run_log_config_tests() {
    go2cpp_tests::announce("日志与配置工程化接口");
    using namespace go2cpp;
    using namespace go2cpp::config;
    using namespace std::chrono_literals;
    IniFile ini;
    std::string error;
    GO2CPP_REQUIRE(ini.Parse("# 注释\n[scheduler]\nmin_workers=1\nmax_workers=4\ncollect_metrics=false\n[log]\nlevel=info\nstdout=false\n", &error));
    RuntimeConfig config;
    GO2CPP_REQUIRE(RuntimeConfig::FromIni(ini, &config, &error));
    GO2CPP_CHECK(config.log_level == log::Level::Info);
    GO2CPP_CHECK(config.scheduler.max_workers == 4);
    GO2CPP_CHECK(!config.scheduler.collect_metrics);
    config.scheduler.max_workers = 33;
    GO2CPP_CHECK(!config.Validate(&error));
    config.scheduler.max_workers = 4;

    auto logger = std::make_shared<Logger>("test");
    std::atomic<int> callback_count{0};
    logger->SetLevel(log::Level::Info);
    logger->AddWorker(std::make_shared<log::LogItemWorker>(
        std::make_shared<log::MinimumLevelFilter>(log::Level::Info),
        std::make_shared<log::PatternFormatter>("{level}:{message}\n"),
        std::make_shared<log::CallbackSink>([&](const log::LogRecord&, std::string_view text) {
            if (text.find("并发") != std::string_view::npos) ++callback_count;
        })));
    constexpr int kThreads = 8;
    constexpr int kMessages = 250;
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([logger] {
            for (int j = 0; j < kMessages; ++j) GO2CPP_LOG_INFO(logger) << "并发";
        });
    }
    for (auto& worker : workers) worker.join();
    GO2CPP_CHECK(callback_count.load() == kThreads * kMessages);
    const auto directory = std::filesystem::temp_directory_path() / "go2cpp-log-test";
    std::filesystem::remove_all(directory);
    config.log_directory = directory.string(); config.log_file = "runtime.log"; config.log_level = log::Level::Warn;
    GO2CPP_REQUIRE(config.ApplyLogging(&error));
    auto configured = GO2CPP_LOG_NAME("configured");
    GO2CPP_LOG_INFO(configured) << "不会写入默认发布日志";
    GO2CPP_LOG_WARN(configured) << "警告写入文件";
    configured.reset();
    std::ifstream file(directory / "runtime.log"); std::stringstream contents; contents << file.rdbuf();
    GO2CPP_CHECK(contents.str().find("警告写入文件") != std::string::npos);
    GO2CPP_CHECK(contents.str().find("不会写入") == std::string::npos);
    std::filesystem::remove_all(directory);

    // Sylar 风格的 %d/%T/%N/%p/%m 格式和多输出地。
    auto memory = std::make_shared<log::MemorySink>();
    auto root = std::make_shared<Logger>("root-test");
    root->SetLevel(log::Level::Info);
    root->AddWorker(std::make_shared<log::LogItemWorker>(
        std::make_shared<log::MinimumLevelFilter>(log::Level::Info),
        std::make_shared<log::PatternFormatter>("%p%T%N%T%m%n"), memory));
    auto child = std::make_shared<Logger>("child-test");
    child->SetLevel(log::Level::Info);
    child->SetParent(root);
    child->SetPropagate(true);
    GO2CPP_LOG_INFO(child) << "层级传播";
    const auto lines = memory->Snapshot();
    GO2CPP_REQUIRE(lines.size() == 1);
    GO2CPP_CHECK(lines.front().find("INFO\tchild-test\t层级传播") != std::string::npos);

    const auto rotating_path = std::filesystem::temp_directory_path() / "go2cpp-rotate.log";
    std::filesystem::remove(rotating_path);
    auto rotating = std::make_shared<log::RotatingFileSink>(
        rotating_path.string(), 32, 2);
    log::LogRecord record;
    for (int i = 0; i < 8; ++i) rotating->Write(record, "012345678901234567890123456789\n");
    GO2CPP_CHECK(std::filesystem::exists(rotating_path.string() + ".1"));
    std::filesystem::remove(rotating_path);
    std::filesystem::remove(rotating_path.string() + ".1");
    std::filesystem::remove(rotating_path.string() + ".2");

    // ConfigVar 监听器和文件热加载：修改 ini 后已创建 Logger 立即切换级别。
    auto& registry = config::Config::Instance();
    GO2CPP_REQUIRE(config::BindLoggingConfig(registry, &error));
    const auto watch_path = std::filesystem::temp_directory_path() / "go2cpp-dynamic.ini";
    {
        std::ofstream watch_file(watch_path);
        watch_file << "[log]\nlevel=error\nstdout=false\nformat={message}\\n\n";
    }
    const auto edit_lock_path = watch_path.string() + ".lock";
    {
        std::ofstream edit_lock(edit_lock_path);
        edit_lock << "editing";
    }
    std::string watcher_error;
    GO2CPP_CHECK(!registry.StartWatcher(watch_path.string(),
                                        std::chrono::milliseconds(20),
                                        &watcher_error));
    GO2CPP_CHECK(watcher_error.find("编辑锁") != std::string::npos);
    std::filesystem::remove(edit_lock_path);
    GO2CPP_REQUIRE(registry.StartWatcher(watch_path.string(), std::chrono::milliseconds(20)));
    auto dynamic_logger = GO2CPP_LOG_NAME("dynamic");
    GO2CPP_CHECK(dynamic_logger->level() == log::Level::Error);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    {
        std::ofstream watch_file(watch_path, std::ios::trunc);
        watch_file << "[log]\nlevel=info\nstdout=false\nformat=%p:%m%n\n";
    }
    auto watcher_timeout = 1s;
    if (RUNNING_ON_VALGRIND) {
        // 文件监视线程在 Memcheck 下会被显著放慢；放大 watchdog，
        // 但不改变配置热加载的轮询间隔和状态断言。
        watcher_timeout *= 120;
    }
    GO2CPP_REQUIRE_EVENTUALLY(dynamic_logger->level() == log::Level::Info,
                              watcher_timeout);
    // 模拟编辑器的截断写入：半个配置不能覆盖旧快照，写完整后仍应继续生效。
    {
        std::ofstream watch_file(watch_path, std::ios::trunc);
        watch_file << "[log]\nlevel=";
        watch_file.flush();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    GO2CPP_CHECK(dynamic_logger->level() == log::Level::Info);
    {
        std::ofstream watch_file(watch_path, std::ios::trunc);
        watch_file << "[log]\nlevel=warn\nstdout=false\nformat=%p:%m%n\n";
    }
    GO2CPP_REQUIRE_EVENTUALLY(dynamic_logger->level() == log::Level::Warn,
                              watcher_timeout);
    // 编辑锁非空期间不读取配置；清空锁文件是写入事务的提交点。
    {
        std::ofstream edit_lock(edit_lock_path, std::ios::trunc);
        edit_lock << "editing";
    }
    {
        std::ofstream watch_file(watch_path, std::ios::trunc);
        watch_file << "[log]\nlevel=debug\nstdout=false\nformat=%p:%m%n\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    GO2CPP_CHECK(dynamic_logger->level() == log::Level::Warn);
    {
        std::ofstream edit_lock(edit_lock_path, std::ios::trunc);
    }
    GO2CPP_REQUIRE_EVENTUALLY(dynamic_logger->level() == log::Level::Debug,
                              watcher_timeout);
    registry.StopWatcher();
    std::filesystem::remove(watch_path);
    std::filesystem::remove(edit_lock_path);

    RuntimeConfig dynamic_runtime;
    GO2CPP_REQUIRE(config::BindRuntimeConfig(registry, &dynamic_runtime, &error));
    auto max_workers = registry.Lookup<std::size_t>("scheduler.max_workers", 0, "最大 M");
    max_workers->SetValue(8);
    GO2CPP_CHECK(dynamic_runtime.scheduler.max_workers == 8);
    // 运行时配置变量也执行同一套范围校验，非法值不会覆盖旧值。
    std::string validation_error;
    GO2CPP_CHECK(!max_workers->SetValue(33, &validation_error));
    GO2CPP_CHECK(dynamic_runtime.scheduler.max_workers == 8);
    GO2CPP_CHECK(!validation_error.empty());
    auto min_workers = registry.Lookup<std::size_t>("scheduler.min_workers", 0, "最小 M");
    GO2CPP_REQUIRE(min_workers != nullptr);
    GO2CPP_CHECK(!min_workers->SetValue(9, &validation_error));
    GO2CPP_CHECK(dynamic_runtime.scheduler.min_workers == 0);
    IniFile invalid_runtime;
    GO2CPP_REQUIRE(invalid_runtime.Parse("[scheduler]\nmax_workers=33\n", &validation_error));
    GO2CPP_CHECK(!registry.LoadFromIni(invalid_runtime, &validation_error));
    GO2CPP_CHECK(max_workers->GetValue() == 8);
    auto metrics = registry.Lookup<bool>("scheduler.collect_metrics", true, "是否采集调度指标");
    GO2CPP_REQUIRE(metrics != nullptr);
    GO2CPP_CHECK(metrics->SetValue(false));
    GO2CPP_CHECK(!dynamic_runtime.scheduler.collect_metrics);
}
