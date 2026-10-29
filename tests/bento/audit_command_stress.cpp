/**
 * @file audit_command_stress.cpp
 * @brief MOD-26 command 模块审查用压测/随机/场景/并发/泄漏程序（不注册 CTest）。
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 *
 * @details 与 `tests/bento/test_command.cpp` 的分工：
 *          - `test_command.cpp` 是注册进 CTest 的确定性回归测试；
 *          - 本程序是**审查压测工具**，覆盖：
 *            1. 20 万次请求/结果帧编解码 + 1MiB 大帧吞吐；
 *            2. 1000 次顺序执行 `/bin/true` 的延迟/fd/RSS/僵尸；
 *            3. 1024 个短命令**同时**执行的峰值 fd/耗时/成功率；
 *            4. 3000 组随机 argv/超时/cwd/策略与独立参考模型对拍；
 *            5. 帧随机变异/截断 fuzz（不崩、拒绝或保持规范编码）；
 *            6. 6 个真实场景：白名单拒绝、超时杀进程组、双流截断、
 *               cwd 沙箱（含符号链接逃逸）、命令注入、fd/僵尸长稳。
 *
 * 用法：
 * @code
 *   audit_command_stress 1.0                     # 全量（约 3-8s）
 *   audit_command_stress 1.0 --mode=bench        # 只跑吞吐/延迟
 *   audit_command_stress 1.0 --mode=random       # 随机对拍/fuzz/注入
 *   audit_command_stress 1.0 --mode=scenarios    # 真实场景 + 1024 并发
 *   audit_command_stress 0.02 --mode=all         # ASan/UBSan 小规模
 *   audit_command_stress 0.005 --mode=leaks      # valgrind 小规模
 * @endcode
 *
 * 退出码：0 = 全部断言通过；1 = 有失败（失败点打印 CHECK 位置）。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/bento/net/command.hpp"

namespace {

using go2cpp::bento::Command;
using go2cpp::bento::CommandCallbacks;
using go2cpp::bento::CommandExecutor;
using go2cpp::bento::CommandPolicy;
using go2cpp::bento::CommandResult;
using go2cpp::bento::CommandStatus;
using go2cpp::bento::DecodeCommand;
using go2cpp::bento::DecodeCommandResult;
using go2cpp::bento::EncodeCommand;
using go2cpp::bento::EncodeCommandResult;
using go2cpp::bento::ValidateCommand;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(condition)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

/// 高精度单调时钟（纳秒）。
std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 纳秒转秒。
double Seconds(std::uint64_t ns) {
    return static_cast<double>(ns) / 1e9;
}

/// 从 /proc/self/status 读某字段（KB）。
std::uint64_t ReadSelfStatusKb(const char* key) {
    std::ifstream in("/proc/self/status");
    std::string line;
    const std::size_t key_len = std::strlen(key);
    while (std::getline(in, line)) {
        if (line.compare(0, key_len, key) == 0) {
            const char* cursor = line.c_str() + key_len;
            while (*cursor != '\0' && (*cursor == ':' || *cursor == ' ' || *cursor == '\t')) {
                ++cursor;
            }
            return std::strtoull(cursor, nullptr, 10);
        }
    }
    return 0;
}

/// 当前进程打开的 fd 数（含遍历目录自身的 fd，多次采样口径一致）。
std::size_t CountFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    while (::readdir(dir) != nullptr) {
        ++count;
    }
    ::closedir(dir);
    return count;
}

/// 统计本进程的僵尸子进程数（扫描 /proc/<pid>/stat 的 state + ppid）。
int CountZombieChildren() {
    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) {
        return 0;
    }
    int zombies = 0;
    const pid_t self = ::getpid();
    while (dirent* entry = ::readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] < '0' || name[0] > '9') {
            continue;
        }
        const std::string stat_path = std::string("/proc/") + name + "/stat";
        std::ifstream in(stat_path);
        std::string line;
        if (!std::getline(in, line)) {
            continue;
        }
        const std::size_t close_paren = line.rfind(')');
        if (close_paren == std::string::npos || close_paren + 2 >= line.size()) {
            continue;
        }
        char state = '\0';
        int ppid = 0;
        if (std::sscanf(line.c_str() + close_paren + 1, " %c %d", &state, &ppid) == 2 &&
            state == 'Z' && ppid == static_cast<int>(self)) {
            ++zombies;
        }
    }
    ::closedir(dir);
    return zombies;
}

/// 在 timeout_ms 内等待 /proc/<pid> 消失。
bool WaitProcessGone(pid_t pid, int timeout_ms) {
    const std::string proc = "/proc/" + std::to_string(static_cast<long>(pid));
    const std::uint64_t deadline = NowNs() + static_cast<std::uint64_t>(timeout_ms) * 1000000ULL;
    while (NowNs() < deadline) {
        if (::access(proc.c_str(), F_OK) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ::access(proc.c_str(), F_OK) != 0;
}

/// 唯一临时目录。
std::filesystem::path MakeTempDir(const char* tag) {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int index = 0; index < 100; ++index) {
        const std::filesystem::path dir =
            base / (std::string("bento_audit_cmd_") + tag + "_" +
                    std::to_string(static_cast<long>(::getpid())) + "_" +
                    std::to_string(index));
        std::error_code ec;
        if (std::filesystem::create_directories(dir, ec) && !ec) {
            return dir;
        }
    }
    return {};
}

void RemoveTree(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

/// 去掉尾部换行。
std::string TrimNewline(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

/// 百分位；输入会被排序。
std::uint64_t Percentile(std::vector<std::uint64_t> samples, double ratio) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const double index = ratio * static_cast<double>(samples.size() - 1);
    const std::size_t offset = static_cast<std::size_t>(index + 0.5);
    return samples[std::min(offset, samples.size() - 1)];
}

void PrintLatency(const char* name, const std::vector<std::uint64_t>& samples) {
    if (samples.empty()) {
        return;
    }
    std::uint64_t total = 0;
    for (const std::uint64_t value : samples) {
        total += value;
    }
    std::printf(
        "  %-26s n=%-7zu avg=%7.1f us  P50=%7.1f  P95=%7.1f  P99=%7.1f\n", name,
        samples.size(), static_cast<double>(total) / samples.size() / 1000.0,
        static_cast<double>(Percentile(samples, 0.50)) / 1000.0,
        static_cast<double>(Percentile(samples, 0.95)) / 1000.0,
        static_cast<double>(Percentile(samples, 0.99)) / 1000.0);
}

// ---------------------------------------------------------------------------
// 1. 帧编解码吞吐
// ---------------------------------------------------------------------------

void BenchCodec(double scale) {
    std::printf("[bench] 帧编解码\n");
    const int request_n = std::max(1000, static_cast<int>(200000.0 * scale));
    const int result_n = std::max(1000, static_cast<int>(100000.0 * scale));

    Command request;
    request.id = "bench-request";
    request.argv = {"/bin/echo", "hello", "世界", "--flag=value"};
    request.timeout_ms = 5000;
    request.cwd = "/tmp";
    const std::vector<std::uint8_t> request_frame = EncodeCommand(request);
    CHECK(!request_frame.empty());
    CHECK(request_frame.size() < 256);
    std::printf("  request frame = %zu B, result header = %zu B\n", request_frame.size(),
                go2cpp::bento::kCommandResultFrameHeaderBytes);

    // 预热。
    Command decoded_request;
    for (int i = 0; i < 1000; ++i) {
        (void)DecodeCommand(request_frame.data(), request_frame.size(), &decoded_request);
    }

    std::uint64_t sink = 0;
    int errors = 0;
    std::vector<std::uint64_t> lat;
    lat.reserve(static_cast<std::size_t>(request_n));
    const std::uint64_t encode_start = NowNs();
    for (int i = 0; i < request_n; ++i) {
        const std::uint64_t begin = NowNs();
        const std::vector<std::uint8_t> frame = EncodeCommand(request);
        const std::uint64_t end = NowNs();
        sink += frame.size();
        lat.push_back(end - begin);
        if (frame.empty()) {
            ++errors;
        }
    }
    const std::uint64_t encode_ns = NowNs() - encode_start;
    CHECK(errors == 0);

    int decode_errors = 0;
    lat.clear();
    const std::uint64_t decode_start = NowNs();
    for (int i = 0; i < request_n; ++i) {
        const std::uint64_t begin = NowNs();
        Command out;
        const bool ok = DecodeCommand(request_frame.data(), request_frame.size(), &out);
        const std::uint64_t end = NowNs();
        if (!ok) {
            ++decode_errors;
        }
        sink += out.argv.size();
        lat.push_back(end - begin);
    }
    const std::uint64_t decode_ns = NowNs() - decode_start;
    CHECK(decode_errors == 0);

    std::printf("  请求帧 encode: %.0f ns/op  %.2f Mops/s\n",
                static_cast<double>(encode_ns) / request_n,
                static_cast<double>(request_n) / Seconds(encode_ns) / 1e6);
    std::printf("  请求帧 decode: %.0f ns/op  %.2f Mops/s\n",
                static_cast<double>(decode_ns) / request_n,
                static_cast<double>(request_n) / Seconds(decode_ns) / 1e6);

    CommandResult result;
    result.id = "bench-result";
    result.status = CommandStatus::kOk;
    result.exit_code = 0;
    result.duration_ms = 12;
    result.standard_output = std::string(512, 'o');
    result.standard_error = std::string(128, 'e');
    result.message = "ok";
    const std::vector<std::uint8_t> result_frame = EncodeCommandResult(result);
    CHECK(!result_frame.empty());
    CommandResult decoded_result;
    CHECK(DecodeCommandResult(result_frame.data(), result_frame.size(), &decoded_result));
    CHECK(decoded_result.standard_output.size() == 512);

    std::vector<std::uint64_t> result_lat;
    result_lat.reserve(static_cast<std::size_t>(result_n));
    int result_errors = 0;
    const std::uint64_t result_start = NowNs();
    for (int i = 0; i < result_n; ++i) {
        const std::uint64_t begin = NowNs();
        const std::vector<std::uint8_t> frame = EncodeCommandResult(result);
        const std::uint64_t end = NowNs();
        sink += frame.size();
        result_lat.push_back(end - begin);
        if (frame.empty()) {
            ++result_errors;
        }
    }
    const std::uint64_t result_ns = NowNs() - result_start;
    CHECK(result_errors == 0);
    std::printf("  结果帧(640B) encode: %.0f ns/op  %.2f Mops/s\n",
                static_cast<double>(result_ns) / result_n,
                static_cast<double>(result_n) / Seconds(result_ns) / 1e6);

    // 1MiB 大帧：只测 200 次，避免测试自身占用过多时间。
    result.standard_output = std::string(1u << 20, 'x');
    const std::vector<std::uint8_t> big_frame = EncodeCommandResult(result);
    CHECK(!big_frame.empty());
    const int big_n = std::max(20, static_cast<int>(200.0 * scale));
    const std::uint64_t big_start = NowNs();
    std::uint64_t big_bytes = 0;
    int big_failures = 0;
    for (int i = 0; i < big_n; ++i) {
        const std::vector<std::uint8_t> frame = EncodeCommandResult(result);
        big_bytes += frame.size();
        CommandResult out;
        if (!DecodeCommandResult(frame.data(), frame.size(), &out)) {
            ++big_failures;
        }
    }
    const std::uint64_t big_ns = NowNs() - big_start;
    CHECK(big_failures == 0);
    std::printf("  1MiB 结果帧 encode+decode: %.1f MiB/s (%d 次)\n",
                static_cast<double>(big_bytes) / Seconds(big_ns) / (1024.0 * 1024.0), big_n);
    (void)sink;
}

// ---------------------------------------------------------------------------
// 2. 顺序执行吞吐与资源
// ---------------------------------------------------------------------------

void BenchExecute(double scale) {
    std::printf("[bench] 顺序执行 /bin/true\n");
    CommandPolicy policy;
    policy.allowed_commands = {"/bin/true"};
    const CommandExecutor executor(policy);
    Command command;
    command.id = "bench-true";
    command.argv = {"/bin/true"};
    command.timeout_ms = 5000;

    for (int i = 0; i < 20; ++i) {
        CHECK(executor.Execute(command).status == CommandStatus::kOk);
    }

    const std::size_t fds_before = CountFds();
    const std::uint64_t rss_before = ReadSelfStatusKb("VmRSS");
    const int zombies_before = CountZombieChildren();

    const int n = std::max(50, static_cast<int>(1000.0 * scale));
    std::vector<std::uint64_t> lat;
    lat.reserve(static_cast<std::size_t>(n));
    int failures = 0;
    const std::uint64_t start = NowNs();
    for (int i = 0; i < n; ++i) {
        const std::uint64_t begin = NowNs();
        const CommandResult result = executor.Execute(command);
        const std::uint64_t end = NowNs();
        lat.push_back(end - begin);
        if (result.status != CommandStatus::kOk || result.exit_code != 0) {
            ++failures;
        }
    }
    const std::uint64_t elapsed = NowNs() - start;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const std::size_t fds_after = CountFds();
    const std::uint64_t rss_after = ReadSelfStatusKb("VmRSS");
    const int zombies_after = CountZombieChildren();

    CHECK(failures == 0);
    CHECK(fds_after <= fds_before + 2);
    CHECK(zombies_after == zombies_before);
    PrintLatency("/bin/true", lat);
    std::printf("  总耗时 %.3f s，吞吐 %.1f 次/s；fd %zu→%zu；RSS %llu→%llu KiB；僵尸 %d→%d\n",
                Seconds(elapsed), static_cast<double>(n) / Seconds(elapsed), fds_before,
                fds_after, static_cast<unsigned long long>(rss_before),
                static_cast<unsigned long long>(rss_after), zombies_before, zombies_after);

    Command echo;
    echo.id = "bench-echo";
    echo.argv = {"/bin/echo", "hello"};
    echo.timeout_ms = 5000;
    CommandPolicy echo_policy;
    echo_policy.allowed_commands = {"/bin/echo"};
    const CommandExecutor echo_executor(echo_policy);
    std::vector<std::uint64_t> echo_lat;
    const int echo_n = std::max(20, static_cast<int>(200.0 * scale));
    echo_lat.reserve(static_cast<std::size_t>(echo_n));
    int echo_failures = 0;
    for (int i = 0; i < echo_n; ++i) {
        const std::uint64_t begin = NowNs();
        const CommandResult result = echo_executor.Execute(echo);
        const std::uint64_t end = NowNs();
        echo_lat.push_back(end - begin);
        if (result.status != CommandStatus::kOk || result.standard_output != "hello\n") {
            ++echo_failures;
        }
    }
    CHECK(echo_failures == 0);
    PrintLatency("/bin/echo hello", echo_lat);
}

// ---------------------------------------------------------------------------
// 3. 并发执行
// ---------------------------------------------------------------------------

struct ConcurrentReport {
    std::size_t peak_fds{0};
    int failures{0};
    double wall_s{0.0};
    std::size_t fds_before{0};
    std::uint64_t rss_before{0};
    std::uint64_t rss_after{0};
};

/// 用 n 个线程同时执行 n 条短命令，返回峰值 fd 与失败数。
ConcurrentReport RunConcurrent(int n, const Command& command, const CommandExecutor& executor) {
    ConcurrentReport report;
    report.fds_before = CountFds();
    report.rss_before = ReadSelfStatusKb("VmRSS");

    std::vector<int> statuses(static_cast<std::size_t>(n), -1);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop_monitor{false};
    std::atomic<std::size_t> peak_fds{0};

    std::thread monitor([&] {
        while (!stop_monitor.load(std::memory_order_relaxed)) {
            const std::size_t current = CountFds();
            std::size_t peak = peak_fds.load(std::memory_order_relaxed);
            while (current > peak &&
                   !peak_fds.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const CommandResult result = executor.Execute(command);
            statuses[static_cast<std::size_t>(i)] = static_cast<int>(result.status);
        });
    }
    while (ready.load(std::memory_order_acquire) < n) {
        std::this_thread::yield();
    }
    const std::uint64_t start = NowNs();
    go.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::uint64_t elapsed = NowNs() - start;
    stop_monitor.store(true, std::memory_order_relaxed);
    monitor.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    report.wall_s = Seconds(elapsed);
    report.peak_fds = peak_fds.load();
    report.rss_after = ReadSelfStatusKb("VmRSS");
    for (const int status : statuses) {
        if (status != static_cast<int>(CommandStatus::kOk)) {
            ++report.failures;
        }
    }
    return report;
}

/// 固定 worker 数的对照：1024 条命令排队执行，衡量稳态吞吐（排除线程风暴）。
void BenchConcurrentPool(double scale) {
    const int total = std::max(64, static_cast<int>(1024.0 * scale));
    const int workers = std::min(64, total);
    CommandPolicy policy;
    policy.allowed_commands = {"/bin/true"};
    const CommandExecutor executor(policy);
    Command command;
    command.argv = {"/bin/true"};
    command.timeout_ms = 5000;

    std::atomic<int> next{0};
    std::atomic<int> ok{0};
    const std::size_t fds_before = CountFds();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    const std::uint64_t start = NowNs();
    for (int worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&] {
            for (;;) {
                const int index = next.fetch_add(1);
                if (index >= total) {
                    return;
                }
                if (executor.Execute(command).status == CommandStatus::kOk) {
                    ok.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::uint64_t elapsed = NowNs() - start;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ok.load() == total);
    CHECK(CountFds() <= fds_before + 2);
    CHECK(CountZombieChildren() == 0);
    std::printf("  [对照] %d worker 执行 %d 条：%.3f s，%.0f 次/s，fd %zu→%zu\n", workers,
                total, Seconds(elapsed), static_cast<double>(total) / Seconds(elapsed),
                fds_before, CountFds());
}

void BenchConcurrent(double scale) {
    const int n = std::min(1500, std::max(64, static_cast<int>(1024.0 * scale)));
    std::printf("[scenario] %d 个短命令同时执行\n", n);    CommandPolicy policy;
    policy.allowed_commands = {"/bin/true"};
    const CommandExecutor executor(policy);
    Command command;
    command.id = "concurrent-true";
    command.argv = {"/bin/true"};
    command.timeout_ms = 10000;

    const int zombies_before = CountZombieChildren();
    const ConcurrentReport report = RunConcurrent(n, command, executor);
    const int zombies_after = CountZombieChildren();
    CHECK(report.failures == 0);
    CHECK(report.peak_fds <= report.fds_before + static_cast<std::size_t>(n) * 7 + 64);
    CHECK(zombies_after == zombies_before);
    std::printf("  全部 kOk=%s，墙钟 %.3f s，吞吐 %.0f 次/s，峰值 fd %zu（基线 %zu）\n",
                report.failures == 0 ? "是" : "否", report.wall_s,
                static_cast<double>(n) / report.wall_s, report.peak_fds, report.fds_before);
    std::printf("  RSS %llu→%llu KiB，僵尸 %d→%d\n",
                static_cast<unsigned long long>(report.rss_before),
                static_cast<unsigned long long>(report.rss_after), zombies_before, zombies_after);

    // 混合并发：成功/超时/拒绝/启动失败在 128 个线程上交错。
    std::printf("[scenario] 混合状态并发（128 线程 × 4 类）\n");
    CommandPolicy mixed_policy;
    mixed_policy.allowed_commands = {"/bin/true", "/bin/sleep", "bento-no-such-xyz"};
    mixed_policy.kill_grace_ms = 30;
    const CommandExecutor mixed_executor(mixed_policy);
    Command ok_cmd;
    ok_cmd.argv = {"/bin/true"};
    ok_cmd.timeout_ms = 5000;
    Command timeout_cmd;
    timeout_cmd.argv = {"/bin/sleep", "5"};
    timeout_cmd.timeout_ms = 40;
    Command denied_cmd;
    denied_cmd.argv = {"/bin/rm"};
    Command spawn_cmd;
    spawn_cmd.argv = {"bento-no-such-xyz"};
    spawn_cmd.timeout_ms = 5000;

    std::atomic<int> mixed_ok{0};
    std::atomic<int> mixed_timeout{0};
    std::atomic<int> mixed_denied{0};
    std::atomic<int> mixed_spawn{0};
    std::vector<std::thread> mixed_threads;
    std::atomic<bool> mixed_go{false};
    std::atomic<int> mixed_ready{0};
    for (int i = 0; i < 128; ++i) {
        mixed_threads.emplace_back([&, i] {
            mixed_ready.fetch_add(1);
            while (!mixed_go.load()) {
                std::this_thread::yield();
            }
            switch (i % 4) {
                case 0:
                    if (mixed_executor.Execute(ok_cmd).status == CommandStatus::kOk) {
                        mixed_ok.fetch_add(1);
                    }
                    break;
                case 1:
                    if (mixed_executor.Execute(timeout_cmd).status == CommandStatus::kTimeout) {
                        mixed_timeout.fetch_add(1);
                    }
                    break;
                case 2:
                    if (mixed_executor.Execute(denied_cmd).status == CommandStatus::kDenied) {
                        mixed_denied.fetch_add(1);
                    }
                    break;
                default:
                    if (mixed_executor.Execute(spawn_cmd).status ==
                        CommandStatus::kSpawnFailed) {
                        mixed_spawn.fetch_add(1);
                    }
                    break;
            }
        });
    }
    while (mixed_ready.load() < 128) {
        std::this_thread::yield();
    }
    mixed_go.store(true);
    for (std::thread& thread : mixed_threads) {
        thread.join();
    }
    CHECK(mixed_ok.load() == 32);
    CHECK(mixed_timeout.load() == 32);
    CHECK(mixed_denied.load() == 32);
    CHECK(mixed_spawn.load() == 32);
    CHECK(CountZombieChildren() == 0);
    std::printf("  成功 %d / 超时 %d / 拒绝 %d / 启动失败 %d（各应为 32）\n",
                mixed_ok.load(), mixed_timeout.load(), mixed_denied.load(), mixed_spawn.load());
    BenchConcurrentPool(scale);
}

// ---------------------------------------------------------------------------
// 4. 随机对拍
// ---------------------------------------------------------------------------

/// 取 basename（只按 '/'）。
std::string_view RefBaseName(std::string_view path) {
    const std::size_t pos = path.find_last_of('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

std::string RefLower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return out;
}

bool RefMatches(std::string_view argv0, const std::vector<std::string>& list) {
    const std::string_view base = RefBaseName(argv0);
    for (const std::string& entry : list) {
        if (entry.empty()) {
            continue;
        }
        if (entry == argv0 || entry == base) {
            return true;
        }
    }
    return false;
}

/// 独立参考实现：按头文件文档的规则重写一遍（不复用被测函数）。
CommandStatus ReferenceValidate(const Command& command, const CommandPolicy& policy) {
    if (policy.max_output_bytes == 0 ||
        policy.max_output_bytes > go2cpp::bento::kCommandMaxOutputCeiling) {
        return CommandStatus::kInvalidArgument;
    }
    if (policy.max_timeout_ms > go2cpp::bento::kCommandMaxTimeoutMs) {
        return CommandStatus::kInvalidArgument;
    }
    if (command.id.size() > go2cpp::bento::kCommandMaxIdBytes ||
        command.id.find('\0') != std::string::npos) {
        return CommandStatus::kInvalidArgument;
    }
    if (command.argv.empty() || command.argv.size() > go2cpp::bento::kCommandMaxArgv) {
        return CommandStatus::kInvalidArgument;
    }
    std::size_t frame_size = go2cpp::bento::kCommandFrameHeaderBytes +
                             4 * command.argv.size() + command.id.size() + command.cwd.size();
    for (const std::string& arg : command.argv) {
        if (arg.empty() || arg.size() > go2cpp::bento::kCommandMaxArgBytes ||
            arg.find('\0') != std::string::npos) {
            return CommandStatus::kInvalidArgument;
        }
        frame_size += arg.size();
    }
    if (command.timeout_ms > policy.max_timeout_ms) {
        return CommandStatus::kInvalidArgument;
    }
    if (command.timeout_ms == 0 && !policy.allow_unlimited_timeout) {
        return CommandStatus::kInvalidArgument;
    }
    if (frame_size > go2cpp::bento::kCommandMaxFrameBytes) {
        return CommandStatus::kInvalidArgument;
    }
    if (command.cwd.size() > go2cpp::bento::kCommandMaxCwdBytes ||
        command.cwd.find('\0') != std::string::npos) {
        return CommandStatus::kInvalidArgument;
    }
    if (!command.cwd.empty() && command.cwd.front() != '/') {
        return CommandStatus::kInvalidArgument;
    }
    if (!policy.allowed_cwd_roots.empty()) {
        if (command.cwd.empty()) {
            return CommandStatus::kDenied;
        }
        const std::filesystem::path cwd =
            std::filesystem::path(command.cwd).lexically_normal();
        bool inside = false;
        for (const std::string& root : policy.allowed_cwd_roots) {
            if (root.empty() || root.front() != '/') {
                continue;
            }
            const std::filesystem::path root_path =
                std::filesystem::path(root).lexically_normal();
            const std::filesystem::path relative = cwd.lexically_relative(root_path);
            if (!relative.empty() && relative.begin() != relative.end() &&
                relative.begin()->string() != "..") {
                inside = true;
                break;
            }
        }
        if (!inside) {
            return CommandStatus::kDenied;
        }
    }
    const std::string& executable = command.argv[0];
    if (RefMatches(executable, policy.denied_commands)) {
        return CommandStatus::kDenied;
    }
    if (policy.use_builtin_deny && !policy.allow_dangerous) {
        const std::string base = RefLower(RefBaseName(executable));
        const std::vector<std::string>& dangerous =
            go2cpp::bento::BuiltinDangerousCommands();
        if (std::find(dangerous.begin(), dangerous.end(), base) != dangerous.end()) {
            return CommandStatus::kDenied;
        }
    }
    if (!policy.allow_all) {
        if (policy.allowed_commands.empty()) {
            return CommandStatus::kDenied;
        }
        if (!RefMatches(executable, policy.allowed_commands)) {
            return CommandStatus::kDenied;
        }
    }
    return CommandStatus::kOk;
}

std::string RandomText(std::mt19937_64* rng, std::size_t length, bool allow_nul,
                       bool allow_slash) {
    static const char kAlphabet[] =
        "abcXYZ019 _-.;|$`'\"\\/(){}[]<>&*?!@#%^+=:,~\t\n世界";
    std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(kAlphabet) - 2));
    std::string out;
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        if (allow_nul && ((*rng)() % 97) == 0) {
            out.push_back('\0');
            continue;
        }
        char ch = kAlphabet[pick(*rng)];
        if (!allow_slash && ch == '/') {
            ch = 'x';
        }
        out.push_back(ch);
    }
    return out;
}

std::string RandomCommandName(std::mt19937_64* rng) {
    static const char* kNames[] = {"/bin/echo", "echo",   "cat",       "/bin/cat",
                                   "rm",        "sh",     "/tmp/evil/echo",
                                   "pwd",       "custom", "bento-no-such-xyz"};
    return kNames[(*rng)() % (sizeof(kNames) / sizeof(kNames[0]))];
}

void RandomMode(double scale, const std::filesystem::path& sandbox_root) {
    std::printf("[random] 3000 组随机 argv/超时/cwd/策略对拍 + 帧变异 + 注入\n");
    std::mt19937_64 rng(20261029ULL);
    const int iterations = std::max(200, static_cast<int>(3000.0 * scale));
    int mismatches = 0;
    int roundtrip_errors = 0;
    int err_request_decode = 0;
    int err_request_fields = 0;
    int err_request_validate = 0;
    int err_mutation_reencode = 0;
    int err_truncation = 0;
    int err_empty_frame = 0;
    int err_result_decode = 0;
    int err_result_fields = 0;
    int err_result_truncation = 0;
    int err_result_flags = 0;
    int err_result_status = 0;
    int err_result_reserved = 0;
    int mutations = 0;
    int mutation_decode_ok = 0;
    int fuzz_buffers = 0;

    const std::vector<std::string> name_pool = {"/bin/echo", "echo", "cat", "/bin/cat",
                                                "rm",        "sh",   "/tmp/evil/echo",
                                                "pwd",       "custom"};
    for (int iter = 0; iter < iterations; ++iter) {
        CommandPolicy policy;
        policy.allow_all = (rng() % 3) == 0;
        policy.allow_dangerous = (rng() % 5) == 0;
        policy.use_builtin_deny = (rng() % 6) != 0;
        const int allowed_count = static_cast<int>(rng() % 4);
        for (int i = 0; i < allowed_count; ++i) {
            policy.allowed_commands.push_back(name_pool[rng() % name_pool.size()]);
        }
        if ((rng() % 7) == 0) {
            policy.denied_commands.push_back(name_pool[rng() % name_pool.size()]);
        }
        const std::uint64_t output_pick = rng() % 6;
        policy.max_output_bytes = output_pick == 0 ? 0
                                   : output_pick == 1 ? 64
                                   : output_pick == 2 ? (1u << 20)
                                   : output_pick == 3 ? (5u << 20)
                                                      : 4096;
        policy.max_timeout_ms = (rng() % 4) == 0 ? (rng() % 2 == 0 ? 0 : 600001) : 600000;
        if ((rng() % 4) == 0) {
            policy.allowed_cwd_roots.push_back((rng() % 2) == 0 ? "/tmp"
                                                                : sandbox_root.string());
        }
        policy.use_path_search = (rng() % 2) == 0;

        Command command;
        switch (rng() % 5) {
            case 0:
                command.id = "";
                break;
            case 1:
                command.id = "id-" + std::to_string(iter);
                break;
            case 2:
                command.id = std::string(64, 'i');
                break;
            case 3:
                command.id = std::string(65, 'i');
                break;
            default:
                command.id = RandomText(&rng, 12, true, true);
                break;
        }
        const int argc = static_cast<int>(rng() % 6) == 0 ? static_cast<int>(rng() % 70)
                                                          : static_cast<int>(rng() % 6);
        for (int i = 0; i < argc; ++i) {
            if (i == 0) {
                command.argv.push_back(RandomCommandName(&rng));
            } else {
                const std::size_t length = (rng() % 8) == 0
                                               ? static_cast<std::size_t>(rng() % 4200)
                                               : static_cast<std::size_t>(rng() % 16);
                command.argv.push_back(RandomText(&rng, length, (rng() % 11) == 0, true));
            }
        }
        const std::uint32_t timeouts[] = {1, 200, 5000, 600000, 600001};
        command.timeout_ms = timeouts[rng() % 5];
        switch (rng() % 8) {
            case 0:
                command.cwd = "";
                break;
            case 1:
                command.cwd = "/tmp";
                break;
            case 2:
                command.cwd = "/tmp/../etc";
                break;
            case 3:
                command.cwd = "/etc";
                break;
            case 4:
                command.cwd = sandbox_root.string();
                break;
            case 5:
                command.cwd = (sandbox_root / "sub").string();
                break;
            case 6:
                command.cwd = "relative/path";
                break;
            default:
                command.cwd = RandomText(&rng, 10, (rng() % 13) == 0, true);
                break;
        }

        const CommandStatus expected = ReferenceValidate(command, policy);
        std::string reason;
        const CommandStatus actual = ValidateCommand(command, policy, &reason);
        if (actual != expected) {
            ++mismatches;
            if (mismatches <= 5) {
                std::fprintf(stderr,
                             "[MISMATCH] iter=%d expected=%s actual=%s argc=%zu timeout=%u "
                             "cwd='%s' allow_all=%d allow_danger=%d builtin=%d roots=%zu\n",
                             iter, go2cpp::bento::CommandStatusToString(expected),
                             go2cpp::bento::CommandStatusToString(actual), command.argv.size(),
                             command.timeout_ms, command.cwd.c_str(), policy.allow_all ? 1 : 0,
                             policy.allow_dangerous ? 1 : 0, policy.use_builtin_deny ? 1 : 0,
                             policy.allowed_cwd_roots.size());
            }
        }

        const std::vector<std::uint8_t> frame = EncodeCommand(command);
        if (!frame.empty()) {
            Command decoded;
            std::string error;
            if (!DecodeCommand(frame.data(), frame.size(), &decoded, &error)) {
                ++roundtrip_errors;
                ++err_request_decode;
            } else {
                if (decoded.id != command.id || decoded.argv != command.argv ||
                    decoded.timeout_ms != command.timeout_ms || decoded.cwd != command.cwd) {
                    ++roundtrip_errors;
                    ++err_request_fields;
                }
                if (ValidateCommand(decoded, policy, nullptr) != actual) {
                    ++roundtrip_errors;
                    ++err_request_validate;
                }
                // 随机变异：成功解码的变异帧必须保持规范编码（重新编码逐字节相同）。
                for (int m = 0; m < 3; ++m) {
                    std::vector<std::uint8_t> mutated = frame;
                    const int flips = 1 + static_cast<int>(rng() % 3);
                    for (int f = 0; f < flips; ++f) {
                        mutated[rng() % mutated.size()] ^=
                            static_cast<std::uint8_t>(1u << (rng() % 8));
                    }
                    ++mutations;
                    Command mutated_out;
                    if (DecodeCommand(mutated.data(), mutated.size(), &mutated_out, nullptr)) {
                        ++mutation_decode_ok;
                        const std::vector<std::uint8_t> reencoded = EncodeCommand(mutated_out);
                        if (reencoded != mutated) {
                            ++roundtrip_errors;
                            ++err_mutation_reencode;
                            if (err_mutation_reencode <= 3) {
                                std::fprintf(stderr,
                                             "[MUT] iter=%d size=%zu decoded_argv=%zu "
                                             "reencoded=%zu\n",
                                             iter, mutated.size(), mutated_out.argv.size(),
                                             reencoded.size());
                            }
                        }
                    }
                }
                // 截断必须拒绝。
                Command truncated_out;
                if (DecodeCommand(frame.data(), frame.size() - 1, &truncated_out, nullptr) ||
                    DecodeCommand(frame.data(), frame.size() / 2, &truncated_out, nullptr) ||
                    DecodeCommand(frame.data(), 0, &truncated_out, nullptr)) {
                    ++roundtrip_errors;
                    ++err_truncation;
                }
            }
        } else if (actual != CommandStatus::kInvalidArgument) {
            ++roundtrip_errors;
            ++err_empty_frame;
        }
    }

    // 随机结果帧往返 + 定向破坏。
    for (int iter = 0; iter < iterations; ++iter) {
        CommandResult result;
        result.id = RandomText(&rng, static_cast<std::size_t>(rng() % 70), false, true);
        result.status = static_cast<CommandStatus>(rng() % 8);
        result.exit_code = static_cast<int>(rng() % 300) - 100;
        result.timed_out = (rng() % 2) == 0;
        result.output_truncated = (rng() % 2) == 0;
        result.duration_ms = rng() % 100000;
        result.standard_output =
            RandomText(&rng, static_cast<std::size_t>(rng() % 5000), false, true);
        result.standard_error =
            RandomText(&rng, static_cast<std::size_t>(rng() % 500), false, true);
        result.message = RandomText(&rng, static_cast<std::size_t>(rng() % 1200), false, true);
        const std::vector<std::uint8_t> frame = EncodeCommandResult(result);
        if (!frame.empty()) {
            CommandResult decoded;
            if (!DecodeCommandResult(frame.data(), frame.size(), &decoded, nullptr)) {
                ++roundtrip_errors;
                ++err_result_decode;
            } else if (decoded.id != result.id || decoded.status != result.status ||
                       decoded.exit_code != result.exit_code ||
                       decoded.timed_out != result.timed_out ||
                       decoded.output_truncated != result.output_truncated ||
                       decoded.duration_ms != result.duration_ms ||
                       decoded.standard_output != result.standard_output ||
                       decoded.standard_error != result.standard_error ||
                       decoded.message != result.message) {
                ++roundtrip_errors;
                ++err_result_fields;
            }
            if (DecodeCommandResult(frame.data(), frame.size() - 1, &decoded, nullptr)) {
                ++roundtrip_errors;
                ++err_result_truncation;  // 截断帧必须被拒绝
            }
            std::vector<std::uint8_t> broken = frame;
            broken[5] = static_cast<std::uint8_t>(broken[5] | 0x80);
            if (DecodeCommandResult(broken.data(), broken.size(), &decoded, nullptr)) {
                ++roundtrip_errors;
                ++err_result_flags;
            }
            broken = frame;
            broken[4] = 99;
            if (DecodeCommandResult(broken.data(), broken.size(), &decoded, nullptr)) {
                ++roundtrip_errors;
                ++err_result_status;
            }
            broken = frame;
            broken[22] = 1;
            if (DecodeCommandResult(broken.data(), broken.size(), &decoded, nullptr)) {
                ++roundtrip_errors;
                ++err_result_reserved;
            }
        } else if (result.id.size() <= go2cpp::bento::kCommandMaxIdBytes &&
                   result.message.size() <= go2cpp::bento::kCommandMaxMessageBytes &&
                   result.standard_output.size() + result.standard_error.size() <=
                       go2cpp::bento::kCommandResultMaxFrameBytes) {
            ++roundtrip_errors;
        }
    }

    // 纯随机字节 fuzz：解码器只允许返回 false，绝不能越界/崩溃。
    const int fuzz_n = std::max(100, static_cast<int>(5000.0 * scale));
    for (int iter = 0; iter < fuzz_n; ++iter) {
        const std::size_t length = static_cast<std::size_t>(rng() % 512);
        std::vector<std::uint8_t> buffer(length, 0);
        for (std::size_t i = 0; i < length; ++i) {
            buffer[i] = static_cast<std::uint8_t>(rng() & 0xFF);
        }
        Command request_out;
        CommandResult result_out;
        (void)DecodeCommand(buffer.data(), buffer.size(), &request_out, nullptr);
        (void)DecodeCommandResult(buffer.data(), buffer.size(), &result_out, nullptr);
        ++fuzz_buffers;
    }

    CHECK(mismatches == 0);
    CHECK(roundtrip_errors == 0);
    CHECK(fuzz_buffers > 0);
    std::printf("  参考模型对拍 %d 轮：不一致 %d；帧往返/变异错误 %d；变异帧成功解码 %d/%d\n",
                iterations, mismatches, roundtrip_errors, mutation_decode_ok, mutations);
    std::printf("  错误分类: req_decode=%d req_fields=%d req_validate=%d mut_reencode=%d "
                "truncation=%d empty=%d | result_decode=%d result_fields=%d "
                "result_trunc=%d flags=%d status=%d reserved=%d\n",
                err_request_decode, err_request_fields, err_request_validate,
                err_mutation_reencode, err_truncation, err_empty_frame, err_result_decode,
                err_result_fields, err_result_truncation, err_result_flags, err_result_status,
                err_result_reserved);
    std::printf("  随机字节 fuzz %d 个缓冲区：无崩溃/无越界（ASan 复跑）\n", fuzz_buffers);

    // 命令注入：随机 shell 元字符必须原样作为 argv 传给 /bin/printf（用 %s
    // 而不是 echo，避免 payload 以 `-n`/`-e` 开头被当成选项）。
    CommandPolicy echo_policy;
    echo_policy.allowed_commands = {"/bin/printf"};
    const CommandExecutor echo_executor(echo_policy);
    const std::filesystem::path sentinel =
        std::filesystem::temp_directory_path() /
        ("bento_cmd_injection_sentinel_" + std::to_string(static_cast<long>(::getpid())));
    const std::string shell_chars = "$();|&`'\"\\ \n\t*?[]{}<>!#~";
    int injection_failures = 0;
    const int injection_n = std::max(20, static_cast<int>(200.0 * scale));
    for (int i = 0; i < injection_n; ++i) {
        std::string payload;
        const std::size_t length = static_cast<std::size_t>(rng() % 40);
        for (std::size_t k = 0; k < length; ++k) {
            payload.push_back(shell_chars[rng() % shell_chars.size()]);
        }
        const std::string full = payload + "; touch " + sentinel.string();
        Command command;
        command.id = "inject";
        command.argv = {"/bin/printf", "%s\n", full};
        command.timeout_ms = 5000;
        const CommandResult result = echo_executor.Execute(command);
        if (result.status != CommandStatus::kOk || result.standard_output != full + "\n" ||
            std::filesystem::exists(sentinel)) {
            ++injection_failures;
        }
    }
    std::error_code ec;
    std::filesystem::remove(sentinel, ec);
    CHECK(!std::filesystem::exists(sentinel));
    CHECK(injection_failures == 0);
    std::printf("  注入用例 %d 次：参数原样传递、哨兵文件未创建（失败 %d）\n", injection_n,
                injection_failures);
}

// ---------------------------------------------------------------------------
// 5. 真实场景
// ---------------------------------------------------------------------------

void ScenarioWhitelist() {
    std::printf("[scenario] 白名单/危险命令/拒绝表\n");
    int spawned = 0;
    CommandCallbacks callbacks;
    callbacks.on_spawn = [&spawned](pid_t) { ++spawned; };

    CommandPolicy deny_all;
    const CommandExecutor executor(deny_all);
    Command echo;
    echo.argv = {"/bin/echo", "x"};
    echo.timeout_ms = 5000;
    CHECK(executor.Execute(echo, callbacks).status == CommandStatus::kDenied);
    CHECK(spawned == 0);

    CommandPolicy allow_all;
    allow_all.allow_all = true;
    const CommandExecutor allow_executor(allow_all);
    for (const char* name : {"rm", "curl", "sh", "dd", "sudo", "kill", "nc"}) {
        Command dangerous;
        dangerous.argv = {name, "--help"};
        dangerous.timeout_ms = 5000;
        CHECK(allow_executor.Execute(dangerous, callbacks).status == CommandStatus::kDenied);
    }
    CHECK(spawned == 0);

    CommandPolicy allow_all_dangerous = allow_all;
    allow_all_dangerous.allow_dangerous = true;
    allow_all_dangerous.denied_commands = {"rm"};
    const CommandExecutor dangerous_executor(allow_all_dangerous);
    Command rm;
    rm.argv = {"/bin/rm", "-rf", "/tmp/never"};
    CHECK(dangerous_executor.Execute(rm).status == CommandStatus::kDenied);

    CommandPolicy only_echo;
    only_echo.allowed_commands = {"echo"};
    const CommandExecutor echo_executor(only_echo);
    Command basename_echo;
    basename_echo.argv = {"/bin/echo", "basename"};
    basename_echo.timeout_ms = 5000;
    CHECK(echo_executor.Execute(basename_echo).status == CommandStatus::kOk);
    Command not_allowed;
    not_allowed.argv = {"/bin/cat", "/etc/hostname"};
    CHECK(echo_executor.Execute(not_allowed).status == CommandStatus::kDenied);

    CommandPolicy builtin_off;
    builtin_off.use_builtin_deny = false;
    builtin_off.allowed_commands = {"sh"};
    const CommandExecutor builtin_off_executor(builtin_off);
    Command sh;
    sh.argv = {"sh", "-c", "true"};
    sh.timeout_ms = 5000;
    CHECK(builtin_off_executor.Execute(sh).status == CommandStatus::kOk);
    std::printf("  默认拒绝/内置表/拒绝表/白名单 basename 均符合预期（未启动任何被拒进程）\n");
}

void ScenarioTimeoutTree() {
    std::printf("[scenario] 超时 SIGTERM→SIGKILL 杀整个进程组\n");
    CommandPolicy policy;
    policy.allow_all = true;
    policy.allow_dangerous = true;
    policy.kill_grace_ms = 150;
    const CommandExecutor executor(policy);

    // (a) TERM 被忽略：必须升级到 SIGKILL 才能收尾。
    pid_t pid = -1;
    CommandCallbacks callbacks;
    callbacks.on_spawn = [&pid](pid_t value) { pid = value; };
    Command stubborn;
    stubborn.id = "stubborn";
    stubborn.argv = {"/bin/sh", "-c", "trap '' TERM; while :; do sleep 1; done"};
    stubborn.timeout_ms = 300;
    const std::uint64_t begin = NowNs();
    const CommandResult killed = executor.Execute(stubborn, callbacks);
    const std::uint64_t elapsed_ms = (NowNs() - begin) / 1000000ULL;
    CHECK(killed.status == CommandStatus::kTimeout);
    CHECK(killed.timed_out);
    CHECK(killed.message.find("SIGKILL") != std::string::npos);
    CHECK(elapsed_ms >= 300);
    CHECK(elapsed_ms < 3000);
    CHECK(pid > 0);
    if (pid > 0) {
        CHECK(WaitProcessGone(pid, 2000));
        errno = 0;
        const bool group_gone = (::kill(-pid, 0) != 0 && errno == ESRCH);
        CHECK(group_gone);
    }
    CHECK(CountZombieChildren() == 0);

    // (b) 正常响应 TERM：应只发送 SIGTERM。
    pid = -1;
    Command sleepy;
    sleepy.id = "sleepy";
    sleepy.argv = {"/bin/sleep", "30"};
    sleepy.timeout_ms = 200;
    const CommandResult termed = executor.Execute(sleepy, callbacks);
    CHECK(termed.status == CommandStatus::kTimeout);
    CHECK(termed.message.find("SIGTERM") != std::string::npos);
    CHECK(WaitProcessGone(pid, 2000));
    CHECK(CountZombieChildren() == 0);
    std::printf("  顽固进程 %.0f ms 后升级 SIGKILL；普通 sleep 走 SIGTERM；均无僵尸\n",
                static_cast<double>(elapsed_ms));
}

void ScenarioTruncation() {
    std::printf("[scenario] 输出截断（stdout/stderr 双流 + 不阻塞）\n");
    const std::filesystem::path dir = MakeTempDir("truncate");
    CHECK(!dir.empty());
    if (dir.empty()) {
        return;
    }
    const std::filesystem::path file = dir / "big.bin";
    const std::size_t file_size = 6u << 20;
    {
        std::ofstream out(file, std::ios::binary);
        const std::vector<char> block(256 * 1024, 'A');
        for (std::size_t written = 0; written < file_size; written += block.size()) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }

    CommandPolicy policy;
    policy.allowed_commands = {"/bin/cat"};
    policy.max_output_bytes = 64 * 1024;
    const CommandExecutor executor(policy);
    Command command;
    command.id = "truncate";
    command.argv = {"/bin/cat", file.string()};
    command.timeout_ms = 5000;
    const std::uint64_t begin = NowNs();
    const CommandResult result = executor.Execute(command);
    const std::uint64_t elapsed_ms = (NowNs() - begin) / 1000000ULL;
    CHECK(result.status == CommandStatus::kOk);
    CHECK(result.standard_output.size() == 64 * 1024);
    CHECK(result.output_truncated);
    CHECK(result.standard_error.empty());
    CHECK(elapsed_ms < 5000);

    // 两路同时大输出：必须都截断且不互相饿死（双管道泵）。
    CommandPolicy both_policy;
    both_policy.allow_all = true;
    both_policy.allow_dangerous = true;
    both_policy.max_output_bytes = 32 * 1024;
    const CommandExecutor both_executor(both_policy);
    std::atomic<std::uint64_t> stdout_seen{0};
    std::atomic<std::uint64_t> stderr_seen{0};
    CommandCallbacks callbacks;
    callbacks.on_stdout = [&stdout_seen](std::string_view text) {
        stdout_seen.fetch_add(text.size());
    };
    callbacks.on_stderr = [&stderr_seen](std::string_view text) {
        stderr_seen.fetch_add(text.size());
    };
    Command both;
    both.id = "both";
    both.argv = {"/bin/sh", "-c",
                 "/bin/cat '" + file.string() + "'; /bin/cat '" + file.string() + "' >&2"};
    both.timeout_ms = 10000;
    const std::uint64_t both_begin = NowNs();
    const CommandResult both_result = both_executor.Execute(both, callbacks);
    const std::uint64_t both_ms = (NowNs() - both_begin) / 1000000ULL;
    CHECK(both_result.status == CommandStatus::kOk);
    CHECK(both_result.standard_output.size() == 32 * 1024);
    CHECK(both_result.standard_error.size() == 32 * 1024);
    CHECK(both_result.output_truncated);
    CHECK(stdout_seen.load() == file_size);
    CHECK(stderr_seen.load() == file_size);
    CHECK(both_ms < 5000);
    std::printf("  stdout 截断 64KiB/6MiB 用时 %.0f ms；双流各 32KiB，回调收到各 %zu B\n",
                static_cast<double>(elapsed_ms), static_cast<std::size_t>(stdout_seen.load()));
    RemoveTree(dir);
}

void ScenarioCwdSandbox() {
    std::printf("[scenario] cwd 沙箱（含符号链接逃逸）\n");
    const std::filesystem::path root = MakeTempDir("cwd");
    CHECK(!root.empty());
    if (root.empty()) {
        return;
    }
    const std::filesystem::path sub = root / "sub";
    std::error_code ec;
    std::filesystem::create_directories(sub, ec);
    const std::filesystem::path escape = root / "escape";
    std::filesystem::create_symlink("/etc", escape, ec);

    CommandPolicy policy;
    policy.allowed_commands = {"/bin/pwd"};
    policy.allowed_cwd_roots = {root.string()};
    const CommandExecutor executor(policy);
    Command command;
    command.id = "cwd";
    command.argv = {"/bin/pwd"};
    command.timeout_ms = 5000;

    command.cwd = root.string();
    CommandResult result = executor.Execute(command);
    CHECK(result.status == CommandStatus::kOk);
    CHECK(TrimNewline(result.standard_output) ==
          std::filesystem::weakly_canonical(root).string());

    command.cwd = sub.string();
    result = executor.Execute(command);
    CHECK(result.status == CommandStatus::kOk);
    CHECK(TrimNewline(result.standard_output) ==
          std::filesystem::weakly_canonical(sub).string());

    command.cwd = (root / "..").string();
    CHECK(executor.Execute(command).status == CommandStatus::kDenied);
    command.cwd = "/etc";
    CHECK(executor.Execute(command).status == CommandStatus::kDenied);
    command.cwd = (root / ".." / "etc").string();
    CHECK(executor.Execute(command).status == CommandStatus::kDenied);
    command.cwd = escape.string();
    CHECK(executor.Execute(command).status == CommandStatus::kDenied);
    command.cwd = "";
    CHECK(executor.Execute(command).status == CommandStatus::kDenied);
    command.cwd = (root / "does-not-exist").string();
    CHECK(executor.Execute(command).status == CommandStatus::kSpawnFailed);
    std::printf("  根目录/子目录放行；.. 穿越/裸 /etc/符号链接逃逸/空 cwd 拒绝；"
                "不存在目录 spawn-failed\n");
    RemoveTree(root);
}

void LeakSoak(double scale) {
    std::printf("[leaks] fd/僵尸/RSS 长稳（混合状态）\n");
    CommandPolicy policy;
    policy.allowed_commands = {"/bin/echo", "/bin/sleep", "bento-no-such-xyz"};
    policy.kill_grace_ms = 20;
    const CommandExecutor executor(policy);
    Command echo;
    echo.argv = {"/bin/echo", "x"};
    echo.timeout_ms = 5000;
    Command sleepy;
    sleepy.argv = {"/bin/sleep", "30"};
    sleepy.timeout_ms = 30;
    Command missing;
    missing.argv = {"bento-no-such-xyz"};
    missing.timeout_ms = 5000;
    Command denied;
    denied.argv = {"/bin/rm", "-rf", "/tmp/never"};

    const std::size_t fds_before = CountFds();
    const std::uint64_t rss_before = ReadSelfStatusKb("VmRSS");
    const int iterations = std::max(40, static_cast<int>(300.0 * scale));
    int failures = 0;
    for (int i = 0; i < iterations; ++i) {
        switch (i % 4) {
            case 0:
                failures += executor.Execute(echo).status != CommandStatus::kOk;
                break;
            case 1:
                failures += executor.Execute(sleepy).status != CommandStatus::kTimeout;
                break;
            case 2:
                failures += executor.Execute(missing).status != CommandStatus::kSpawnFailed;
                break;
            default:
                failures += executor.Execute(denied).status != CommandStatus::kDenied;
                break;
        }
        if ((i % 25) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const std::size_t now = CountFds();
            CHECK(now <= fds_before + 2);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::size_t fds_after = CountFds();
    const std::uint64_t rss_after = ReadSelfStatusKb("VmRSS");
    CHECK(failures == 0);
    CHECK(fds_after <= fds_before + 2);
    CHECK(CountZombieChildren() == 0);
    CHECK(rss_after < rss_before + 32 * 1024);
    std::printf("  %d 轮：fd %zu→%zu，RSS %llu→%llu KiB，僵尸 0，失败 %d\n", iterations,
                fds_before, fds_after, static_cast<unsigned long long>(rss_before),
                static_cast<unsigned long long>(rss_after), failures);
}

}  // namespace

int main(int argc, char** argv) {
    double scale = 1.0;
    std::string mode = "all";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.rfind("--mode=", 0) == 0) {
            mode = std::string(arg.substr(7));
        } else {
            scale = std::strtod(argv[i], nullptr);
        }
    }
    if (scale <= 0.0) {
        scale = 1.0;
    }
    std::printf("[env] nproc=%ld scale=%.3f mode=%s\n", ::sysconf(_SC_NPROCESSORS_ONLN), scale,
                mode.c_str());

    const std::filesystem::path sandbox_root = MakeTempDir("random_root");
    {
        std::error_code ec;
        std::filesystem::create_directories(sandbox_root / "sub", ec);
    }

    if (mode == "all" || mode == "bench") {
        BenchCodec(scale);
        BenchExecute(scale);
    }
    if (mode == "all" || mode == "scenarios") {
        ScenarioWhitelist();
        ScenarioTimeoutTree();
        ScenarioTruncation();
        ScenarioCwdSandbox();
        BenchConcurrent(scale);
    }
    if (mode == "pool") {
        BenchConcurrentPool(scale);
    }
    if (mode == "all" || mode == "random") {
        RandomMode(scale, sandbox_root);
    }
    if (mode == "all" || mode == "leaks") {
        LeakSoak(scale);
    }
    RemoveTree(sandbox_root);

    std::printf("[result] mode=%s checks=%d failures=%d\n", mode.c_str(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
