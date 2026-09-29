#include "go2cpp/config.hpp"
#include "go2cpp/log.hpp"
#include "test_support.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

void run_log_config_tests() {
    go2cpp_tests::announce("日志与配置工程化接口");
    using namespace go2cpp;
    using namespace go2cpp::config;
    IniFile ini;
    std::string error;
    GO2CPP_REQUIRE(ini.Parse("# 注释\n[scheduler]\nmin_workers=1\nmax_workers=4\n[log]\nlevel=info\nstdout=false\n", &error));
    RuntimeConfig config;
    GO2CPP_REQUIRE(RuntimeConfig::FromIni(ini, &config, &error));
    GO2CPP_CHECK(config.log_level == log::Level::Info);
    GO2CPP_CHECK(config.scheduler.max_workers == 4);
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
}
