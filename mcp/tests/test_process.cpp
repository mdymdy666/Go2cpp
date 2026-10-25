/**
 * @file test_process.cpp
 * @brief `go2cpp::mcp::util::RunProcess`（`go2cpp::os::Run` 适配层）的功能、边界与压力测试。
 *
 * 覆盖：启动并读取 stdout/stderr、标准输入、非零退出码、被信号终止、
 * 超时终止与部分输出保留、命令不存在、环境变量继承、大输出不会因管道写满
 * 而卡死、并发调用、随机二进制 stdin、提前退出时的 SIGPIPE、反复启动的 fd 泄漏。
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_util.hpp"
#include "util/process.hpp"

#include "go2cpp/os/proc.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

go2cpp::mcp::util::ProcessResult Run(std::vector<std::string> argv, int timeout_ms = 3000,
                             const std::string& input = {}) {
    return go2cpp::mcp::util::RunProcess(argv, timeout_ms, input);
}

long long ElapsedMs(SteadyClock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - start)
        .count();
}

}  // namespace

int main() {
    // 1. 空 argv：不启动任何进程，返回无效结果。
    {
        const auto result = go2cpp::mcp::util::RunProcess({}, 1000);
        CHECK(result.exit_code == -1);
        CHECK(!result.timed_out);
        CHECK(result.output.empty());
    }

    // 2. 启动并读取合并输出 + 非零退出码。
    {
        const auto result =
            Run({"/bin/sh", "-c", "printf 'stdout-marker'; printf 'stderr-marker' >&2; exit 3"});
        CHECK(result.exit_code == 3);
        CHECK(!result.timed_out);
        CHECK(result.output.find("stdout-marker") != std::string::npos);
        CHECK(result.output.find("stderr-marker") != std::string::npos);
    }

    // 3. 标准输入写入与 EOF（cat 原样回显）。
    {
        const std::string input = "line-1\nline-2\n";
        const auto result = Run({"/bin/cat"}, 3000, input);
        CHECK(result.exit_code == 0);
        CHECK(!result.timed_out);
        CHECK(result.output == input);
    }

    // 4. 超时终止：timed_out 为真、退出码 -1，且总耗时远小于子进程寿命。
    {
        const auto begin = SteadyClock::now();
        const auto result = Run({"/bin/sleep", "30"}, 200);
        const long long elapsed = ElapsedMs(begin);
        CHECK(result.timed_out);
        CHECK(result.exit_code == -1);
        CHECK(elapsed >= 150);
        CHECK(elapsed < 3000);
    }

    // 5. 超时前已产生的输出不丢。
    {
        const auto result =
            Run({"/bin/sh", "-c", "printf 'partial-before-timeout'; exec /bin/sleep 30"}, 250);
        CHECK(result.timed_out);
        CHECK(result.output.find("partial-before-timeout") != std::string::npos);
    }

    // 6. 子进程被信号终止：不是超时，退出码映射为 -1。
    {
        const auto result = Run({"/bin/sh", "-c", "kill -TERM $$"});
        CHECK(!result.timed_out);
        CHECK(result.exit_code == -1);
    }

    // 7. 命令不存在：保持旧语义（退出码 127 + 诊断文本）。
    {
        const auto result = Run({"mcp-definitely-missing-command-20261024"}, 1000);
        CHECK(result.exit_code == 127);
        CHECK(!result.timed_out);
        CHECK(result.output.find("exec failed") != std::string::npos);
        CHECK(result.output.find("mcp-definitely-missing-command-20261024") !=
              std::string::npos);
    }

    // 8. PATH 查找（裸程序名）与显式退出码。
    {
        const auto result = Run({"sh", "-c", "printf 'path-ok'; exit 42"});
        CHECK(result.exit_code == 42);
        CHECK(result.output == "path-ok");
    }

    // 9. 环境变量继承（不经过 env_clear）。
    {
        (void)::setenv("MCP_OS_SWITCH_ENV", "inherited-value", 1);
        const auto result =
            Run({"/bin/sh", "-c", "printf '%s' \"$MCP_OS_SWITCH_ENV\""});
        (void)::unsetenv("MCP_OS_SWITCH_ENV");
        CHECK(result.exit_code == 0);
        CHECK(result.output == "inherited-value");
    }

    // 10. 只有 stderr 的输出也进入合并管道。
    {
        const auto result = Run({"/bin/sh", "-c", "printf 'only-stderr' >&2"});
        CHECK(result.exit_code == 0);
        CHECK(result.output == "only-stderr");
    }

    // 11. 大输出（1 MiB）：父进程持续排空，不会因管道写满而卡死。
    {
        const auto result =
            Run({"/bin/sh", "-c", "head -c 1048576 /dev/zero | tr '\\0' 'x'"}, 10000);
        CHECK(result.exit_code == 0);
        CHECK(!result.timed_out);
        CHECK(result.output.size() == 1048576);
        CHECK(result.output == std::string(1048576, 'x'));
    }

    // 12. 默认超时分支（timeout <= 0 仍能正常完成）。
    {
        const auto result = Run({"/bin/true"}, 0);
        CHECK(result.exit_code == 0);
        CHECK(!result.timed_out);
    }

    // 13. 并发调用：4 线程 × 5 次，输出互不串扰。
    {
        std::atomic<int> failures{0};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([worker, &failures] {
                for (int index = 0; index < 5; ++index) {
                    const std::string expected =
                        "thread-" + std::to_string(worker) + "-" + std::to_string(index);
                    const auto result = go2cpp::mcp::util::RunProcess(
                        {"/bin/sh", "-c", "printf 'thread-%s-%s' \"$1\" \"$2\"", "sh",
                         std::to_string(worker), std::to_string(index)},
                        5000);
                    if (result.exit_code != 0 || result.timed_out || result.output != expected) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        CHECK(failures.load(std::memory_order_relaxed) == 0);
    }

    // 14. 随机二进制 stdin（含 NUL 与 0xFF）原样穿过管道。
    {
        std::mt19937 generator(20261024u);
        for (int iteration = 0; iteration < 8; ++iteration) {
            const std::size_t length = static_cast<std::size_t>(generator() % 4096u);
            std::string data(length, '\0');
            for (char& byte : data) {
                byte = static_cast<char>(generator() % 256u);
            }
            const auto result = Run({"/bin/cat"}, 5000, data);
            CHECK(result.exit_code == 0);
            CHECK(!result.timed_out);
            CHECK(result.output == data);
        }
    }

    // 15. 子进程提前退出时写 stdin 会得到 EPIPE：SIGPIPE 已忽略，不能杀测试进程。
    {
        const auto result = Run({"/bin/true"}, 3000, std::string(1 << 20, 'x'));
        CHECK(result.exit_code == 0);
        CHECK(!result.timed_out);
    }

    // 16. 忽略 SIGTERM 的子进程：宽限期后升级 SIGKILL，仍能按时返回。
    {
        const auto begin = SteadyClock::now();
        const auto result =
            Run({"/bin/sh", "-c", "trap '' TERM; while :; do sleep 0.1; done"}, 200);
        const long long elapsed = ElapsedMs(begin);
        CHECK(result.timed_out);
        CHECK(result.exit_code == -1);
        CHECK(elapsed >= 150);
        CHECK(elapsed < 3000);
    }

    // 17. 反复启动后的 fd 泄漏检查（os 侧提供 /proc/self/fd 计数）。
    {
        for (int warmup = 0; warmup < 3; ++warmup) {
            (void)Run({"/bin/true"}, 2000);
        }
        const std::size_t before = go2cpp::os::CountOpenFds();
        for (int index = 0; index < 30; ++index) {
            const auto result = Run({"/bin/true"}, 2000);
            CHECK(result.exit_code == 0);
            CHECK(!result.timed_out);
        }
        const std::size_t after = go2cpp::os::CountOpenFds();
        CHECK(after <= before + 2);
    }

    return go2cpp::mcp::test::Summary("process");
}
