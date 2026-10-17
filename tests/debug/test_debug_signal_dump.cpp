/**
 * @file test_debug_signal_dump.cpp
 * @brief 信号驱动跨进程 dump 的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 未安装时 Pending() 为假、PollOnce 返回 0 并给出错误
 *   场景 2 安装后 raise(SIGUSR1)：Pending 为真，PollOnce 产生文件且内容非空
 *   场景 3 连续两次信号 -> 两次 dump 两个文件；PollOnce 消费请求（第二次调用返回 0）
 *   场景 4 watcher 线程模式：信号到来后由后台线程完成 dump
 *   场景 5 Uninstall 后信号不再被接管（Pending 清零、PollOnce 报错）
 *   压测   1000 次 PollOnce（无请求时）应快速返回
 *   随机   随机信号次数，校验文件数与 dump 计数一致
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#endif

namespace {

using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::debug::DebugEventCollector;
using go2cpp::debug::DebugSignalDumper;

bool FileExists(const std::string& path) {
    std::ifstream in(path);
    return in.good();
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Fixture {
    explicit Fixture(std::size_t workers) {
        SchedulerConfig config;
        config.processor_count = workers;
        config.max_workers = workers;
        config.min_workers = workers;
        scheduler = std::make_unique<Scheduler>(config);
        scheduler->start();
        auto warm = scheduler->spawn([] {});
        warm->wait();
    }
    ~Fixture() {
        if (scheduler) {
            scheduler->shutdown();
        }
    }
    std::unique_ptr<Scheduler> scheduler;
};

}  // namespace

void run_debug_signal_dump_tests() {
    go2cpp_tests::announce("signal-driven cross-process dump");
    using namespace go2cpp::debug;

#if !defined(__unix__) && !defined(__APPLE__)
    std::clog << "  [跳过] 非 POSIX 平台不支持信号驱动 dump\n";
    return;
#else
    const int sig = DebugSignalDumper::SignalNumber();
    GO2CPP_CHECK(sig != 0);

    // 场景 1：未安装
    {
        DebugSignalDumper::Uninstall();
        GO2CPP_CHECK(!DebugSignalDumper::Pending());
        std::string error;
        GO2CPP_CHECK(DebugSignalDumper::PollOnce(nullptr, &error) == 0);
        GO2CPP_CHECK(!error.empty());
    }

    // 场景 2：安装 + raise -> 产生文件
    {
        Fixture fixture(1);
        const std::string dir = "/tmp";
        std::string error;
        GO2CPP_CHECK(DebugSignalDumper::Install(*fixture.scheduler, nullptr, dir, &error));
        GO2CPP_CHECK(error.empty());
        GO2CPP_CHECK(!DebugSignalDumper::Pending());

        GO2CPP_CHECK(std::raise(sig) == 0);
        GO2CPP_CHECK(DebugSignalDumper::Pending());

        std::string path;
        GO2CPP_CHECK(DebugSignalDumper::PollOnce(&path, &error) == 1);
        GO2CPP_CHECK(!path.empty());
        GO2CPP_CHECK(FileExists(path));
        const std::string content = ReadFile(path);
        GO2CPP_CHECK(content.find("[scheduler]") != std::string::npos);
        GO2CPP_CHECK(content.find("fibers (all non-terminated G in registries)") != std::string::npos);
        std::remove(path.c_str());

        // 请求已被消费
        GO2CPP_CHECK(!DebugSignalDumper::Pending());
        GO2CPP_CHECK(DebugSignalDumper::PollOnce(nullptr, &error) == 0);
        DebugSignalDumper::Uninstall();
    }

    // 场景 3：连续两次信号 -> 两个文件
    {
        Fixture fixture(1);
        std::string error;
        GO2CPP_CHECK(DebugSignalDumper::Install(*fixture.scheduler, nullptr, "/tmp", &error));
        const std::size_t before = DebugSignalDumper::DumpCount();

        std::vector<std::string> files;
        for (int i = 0; i < 2; ++i) {
            GO2CPP_CHECK(std::raise(sig) == 0);
            std::string path;
            GO2CPP_CHECK(DebugSignalDumper::PollOnce(&path, &error) == 1);
            files.push_back(path);
            // 保证文件名里的时间戳不同
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        GO2CPP_CHECK(files.size() == 2);
        GO2CPP_CHECK(DebugSignalDumper::DumpCount() == before + 2);
        for (const auto& path : files) {
            if (!path.empty() && FileExists(path)) {
                std::remove(path.c_str());
            }
        }
        DebugSignalDumper::Uninstall();
    }

    // 场景 4：watcher 线程
    {
        Fixture fixture(1);
        std::string error;
        GO2CPP_CHECK(DebugSignalDumper::Install(*fixture.scheduler, nullptr, "/tmp", &error));
        GO2CPP_CHECK(DebugSignalDumper::StartWatcher(std::chrono::milliseconds(5), &error));
        const std::size_t before = DebugSignalDumper::DumpCount();
        GO2CPP_CHECK(std::raise(sig) == 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (DebugSignalDumper::DumpCount() > before) {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        GO2CPP_CHECK(done);
        DebugSignalDumper::StopWatcher();
        DebugSignalDumper::Uninstall();
    }

    // 场景 5：Uninstall 后不再接管
    {
        Fixture fixture(1);
        std::string error;
        GO2CPP_CHECK(DebugSignalDumper::Install(*fixture.scheduler, nullptr, "/tmp", &error));
        DebugSignalDumper::Uninstall();
        GO2CPP_CHECK(!DebugSignalDumper::Pending());
        GO2CPP_CHECK(DebugSignalDumper::PollOnce(nullptr, &error) == 0);
        GO2CPP_CHECK(!error.empty());
    }

    // 压测：无请求时 1000 次 PollOnce 应迅速返回
    {
        Fixture fixture(1);
        std::string error;
        GO2CPP_CHECK(DebugSignalDumper::Install(*fixture.scheduler, nullptr, "/tmp", &error));
        constexpr int kPolls = 1000;
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t dumps = 0;
        for (int i = 0; i < kPolls; ++i) {
            dumps += DebugSignalDumper::PollOnce();
        }
        const auto t1 = std::chrono::steady_clock::now();
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::clog << "  [压测] PollOnce（无请求）x" << kPolls << ": " << micros
                  << " us  => " << (micros * 1000.0 / kPolls) << " ns/次" << std::endl;
        GO2CPP_CHECK(dumps == 0);
        GO2CPP_CHECK(micros < 1000000);   // 1000 次应在 1 秒内
        DebugSignalDumper::Uninstall();
    }

    // 随机：随机信号次数，校验 dump 计数一致
    {
        Fixture fixture(1);
        std::string error;
        GO2CPP_CHECK(DebugSignalDumper::Install(*fixture.scheduler, nullptr, "/tmp", &error));
        std::mt19937 rng(20261017);
        std::uniform_int_distribution<int> count_dist(1, 5);
        bool ok = true;
        for (int round = 0; round < 8; ++round) {
            const int times = count_dist(rng);
            const std::size_t before = DebugSignalDumper::DumpCount();
            std::size_t produced = 0;
            for (int i = 0; i < times; ++i) {
                if (std::raise(sig) != 0) {
                    continue;
                }
                std::string path;
                if (DebugSignalDumper::PollOnce(&path, &error) == 1) {
                    ++produced;
                    if (!path.empty() && FileExists(path)) {
                        std::remove(path.c_str());
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ok = ok && (DebugSignalDumper::DumpCount() == before + produced);
        }
        GO2CPP_CHECK(ok);
        DebugSignalDumper::Uninstall();
    }
#endif
}
