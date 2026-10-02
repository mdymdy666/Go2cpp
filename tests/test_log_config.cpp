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

    // 自定义格式项：用户只需继承 Item 并注册工厂，即可扩展 Sylar 风格占位符。
    // 这里故意覆写小写 format，验证旧式扩展类无需改成大写 Format 接口。
    struct UserItem final : public log::LogFormatter::Item {
        void format(std::ostream& stream, const log::LogRecord& item_record) const override {
            stream << "USER(" << item_record.logger << ")";
        }
    };
    GO2CPP_CHECK(!log::LogFormatter::HasFormat("g"));
    GO2CPP_REQUIRE(log::LogFormatter::AddFormat(
        "g", [](const std::string&) {
            return std::make_shared<UserItem>();
        }));
    GO2CPP_CHECK(log::LogFormatter::HasFormat("g"));
    // 同一 key 不允许静默覆盖，避免不同模块之间的格式协议互相污染。
    GO2CPP_CHECK(!log::LogFormatter::addFormat(
        "g", [](const std::string&) {
            return std::make_shared<UserItem>();
        }));
    log::PatternFormatter user_formatter("%g:%m|{g}");
    GO2CPP_CHECK(!user_formatter.HasError());
    GO2CPP_CHECK(user_formatter.Format(log::LogRecord{
        log::Level::Info, "custom-logger", "扩展格式", {}, {}, 0, 0, 0, 0, {}, 0}) ==
                 "USER(custom-logger):扩展格式|USER(custom-logger)");
    GO2CPP_CHECK(log::LogFormatter::RemoveFormat("g"));
    GO2CPP_CHECK(!log::LogFormatter::HasFormat("g"));

    // 模板注册接口适合新手：只需提供一个带 option 构造函数的 Item 类型。
    struct TagItem final : public log::LogFormatter::Item {
        explicit TagItem(std::string option) : m_option(std::move(option)) {}
        void Format(std::ostream& stream, const log::LogRecord&) const override {
            stream << "TAG(" << m_option << ")";
        }
        std::string m_option;
    };
    GO2CPP_REQUIRE(log::LogFormatter::addFormat<TagItem>("q"));
    log::PatternFormatter tag_formatter("%q{demo}");
    GO2CPP_CHECK(!tag_formatter.HasError());
    GO2CPP_CHECK(tag_formatter.Format(log::LogRecord{}) == "TAG(demo)");
    GO2CPP_CHECK(log::LogFormatter::RemoveFormat("q"));

    // 无选项工厂适合固定输出的 Item；内置 %d 已占用，不能被业务插件覆盖。
    struct FixedItem final : public log::LogFormatter::Item {
        void Format(std::ostream& stream, const log::LogRecord&) const override {
            stream << "FIXED";
        }
    };
    GO2CPP_REQUIRE(log::LogFormatter::addFormat(
        "h", [] { return std::make_shared<FixedItem>(); }));
    log::PatternFormatter fixed_formatter("%h");
    GO2CPP_CHECK(fixed_formatter.Format(log::LogRecord{}) == "FIXED");
    GO2CPP_CHECK(!log::LogFormatter::addFormat(
        "d", [] { return std::make_shared<FixedItem>(); }));
    GO2CPP_CHECK(log::LogFormatter::RemoveFormat("h"));

    log::PatternFormatter function_formatter("{function}");
    GO2CPP_CHECK(function_formatter.Format(log::LogRecord{
        log::Level::Info, {}, {}, {}, "函数名", 0, 0, 0, 0, {}, 0}) == "函数名");
    GO2CPP_CHECK(user_formatter.Format(log::LogRecord{
        log::Level::Info, "custom-logger", "注销后仍可用", {}, {}, 0, 0, 0, 0, {}, 0}) ==
                 "USER(custom-logger):注销后仍可用|USER(custom-logger)");

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

    // 文件配置采用两阶段提交：任何字段校验失败都不能留下半套值；
    // 提交监听器只能看到完整的新快照，适合扩展模块一次性重建资源。
    auto transaction_first = registry.Lookup<int>("test.transaction_first", 1, "事务测试字段一");
    auto transaction_second = registry.Lookup<int>("test.transaction_second", 2, "事务测试字段二");
    GO2CPP_REQUIRE(transaction_first != nullptr && transaction_second != nullptr);
    std::atomic<int> commit_notifications{0};
    std::atomic<bool> saw_complete_snapshot{false};
    const auto commit_id = registry.AddCommitListener(
        [&](const std::vector<ConfigVarBase::ptr>& changed) {
            if (changed.size() == 2 && transaction_first->GetValue() == 10 &&
                transaction_second->GetValue() == 20) {
                saw_complete_snapshot.store(true);
            }
            commit_notifications.fetch_add(1);
        });
    IniFile invalid_transaction;
    GO2CPP_REQUIRE(invalid_transaction.Parse(
        "[test]\ntransaction_first=10\ntransaction_second=bad\n", &validation_error));
    GO2CPP_CHECK(!registry.LoadFromIni(invalid_transaction, &validation_error));
    GO2CPP_CHECK(transaction_first->GetValue() == 1);
    GO2CPP_CHECK(transaction_second->GetValue() == 2);
    IniFile valid_transaction;
    GO2CPP_REQUIRE(valid_transaction.Parse(
        "[test]\ntransaction_first=10\ntransaction_second=20\n", &validation_error));
    GO2CPP_REQUIRE(registry.LoadFromIni(valid_transaction, &validation_error));
    GO2CPP_CHECK(transaction_first->GetValue() == 10);
    GO2CPP_CHECK(transaction_second->GetValue() == 20);
    GO2CPP_CHECK(commit_notifications.load() == 1);
    GO2CPP_CHECK(saw_complete_snapshot.load());
    registry.DelCommitListener(commit_id);
}
