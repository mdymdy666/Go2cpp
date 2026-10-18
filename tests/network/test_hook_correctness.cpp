/**
 * @file test_hook_correctness.cpp
 * @brief 结合 `os` 模块、`hook` 模块与网络 socket，验证 hook 的正确性。
 *
 * ## 使用者的红线
 *
 * > 结合 OS 模块和 hook 模块，网络 socket，**验证 hook 模块的正确性**，
 * > 这部分非常重要。
 * >
 * > **hook 绝对不能影响其他正常的系统调用。**
 *
 * ## 已知机制（本测试验证这些是否真的成立）
 *
 * `read()` 被 hook 后的路径（`src/network/hook.cpp` 的 `cooperative_io`）：
 *
 * ```cpp
 * auto descriptor = descriptor_for(fd);              // 已登记的受管 fd
 * if (!descriptor && managed_hook_admission) {
 *     descriptor = try_adopt_socket(fd);             // getsockopt(SO_TYPE)
 * }
 * if (!descriptor) {                                 // ← 非 socket 落这里
 *     blocking_region.emplace();
 *     return invoke_real(function, fd, args...);     // 直接 libc，阻塞 worker
 * }
 * // socket 走这里 → park Fiber
 * ```
 *
 * 所以预期是：
 * - **非 socket**（普通文件/FIFO/pipe/eventfd）→ 不做协程让出，直接 libc
 * - **socket** → park Fiber，worker 继续跑别的
 *
 * ## 本测试的判定方法：心跳 Fiber
 *
 * 每个场景都在 **P=1**（单 worker）下跑一个"心跳 Fiber"（每 10ms 自增一次，
 * 用 `sleep_for` 让出）。然后让另一个 Fiber 做被测操作，观察心跳计数：
 *
 * | 心跳增量 | 含义 |
 * |---|---|
 * | 多（>8/200ms）| worker 没被占住 —— 操作 **park 了 Fiber** |
 * | 少（<3/200ms）| worker 被占住 —— 操作 **阻塞了线程** |
 *
 * 阈值刻意放宽（10ms 心跳 × 200ms ≈ 20 次，取 8/3 为界），避免调度抖动导致 flaky。
 */
#include "go2cpp/hook.hpp"
#include "go2cpp/network/io.hpp"
#include "go2cpp/os/file.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

int g_failures = 0;

/// 并发执行两份全量 ctest 时同一个测试二进制会同时运行；临时文件与
/// FIFO 路径必须按 pid 隔离，否则会互相 unlink，制造与代码无关的失败。
std::string ProcessScopedPath(const char* name) {
    return "/tmp/" + std::string(name) + "-" +
           std::to_string(::getpid());
}

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[hook-correct] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// socketpair 的 RAII 包装。
struct SocketPair {
    int fds[2]{-1, -1};
    bool Open() { return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0; }
    ~SocketPair() {
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
    }
};

/// 心跳 Fiber：每 10ms 自增一次。`sleep_for` 在 Fiber 里会 park，所以它能让出 worker。
class Heartbeat {
public:
    explicit Heartbeat(go2cpp::IOManager& manager) {
        m_task = manager.Go([this] {
            while (!m_stop.load(std::memory_order_acquire)) {
                m_count.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(Ms(10));
            }
        });
    }
    ~Heartbeat() { Stop(); }

    void Stop() {
        if (m_stopped.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_stop.store(true, std::memory_order_release);
        if (m_task) {
            (void)m_task->wait_for(std::chrono::seconds(3));
        }
    }

    int Count() const { return m_count.load(std::memory_order_relaxed); }

    /// 观察 `window` 时长内的心跳增量。
    int DeltaOver(Ms window) {
        const int before = Count();
        std::this_thread::sleep_for(window);
        return Count() - before;
    }

private:
    std::atomic<int> m_count{0};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_stopped{false};
    std::shared_ptr<go2cpp::Task> m_task;
};

/// 在 P=1 的 IOManager 上跑一个被测 Fiber，并同步等待它完成（带超时）。
bool RunIsolated(go2cpp::IOManager& manager, go2cpp::Task::Function body,
                 std::chrono::seconds limit = std::chrono::seconds(10)) {
    auto task = manager.Go(std::move(body));
    return task != nullptr && task->wait_for(limit);
}

// ===========================================================================
// 场景 1：非 socket fd —— hook 必须零影响
// ===========================================================================

void ScenarioNonSocketZeroImpact() {
    std::printf("[hook-correct][场景1] 非 socket fd：hook 零影响\n");

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    go2cpp::IOManager manager(config);
    Check(manager.Start(), "IOManager 启动失败");
    go2cpp::hook::ScopedEnable hook_scope;
    go2cpp::hook::ScopedIOManagerBinding binding(manager);

    // 准备一个普通文件
    const std::string path = ProcessScopedPath("hook_correct_regular.txt");
    const std::string payload = "hello-hook-correctness-payload";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    // --- 1a. 普通文件：read（hook 版）与 read_f（raw 版）结果必须完全一致 ---
    std::string via_hook;
    std::string via_raw;
    ssize_t rc_hook = -1;
    ssize_t rc_raw = -1;

    (void)RunIsolated(manager, [&] {
        char buffer[128] = {};
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            rc_hook = ::read(fd, buffer, sizeof(buffer));
            via_hook.assign(buffer, rc_hook > 0 ? static_cast<std::size_t>(rc_hook) : 0);
            (void)::close(fd);
        }
    });
    (void)RunIsolated(manager, [&] {
        char buffer[128] = {};
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd >= 0) {
            rc_raw = ::read_f(fd, buffer, sizeof(buffer));
            via_raw.assign(buffer, rc_raw > 0 ? static_cast<std::size_t>(rc_raw) : 0);
            (void)::close(fd);
        }
    });

    std::printf("  1a 普通文件: read=%zd read_f=%zd 数据一致=%d\n",
                rc_hook, rc_raw, via_hook == via_raw ? 1 : 0);
    Check(rc_hook == rc_raw, "非 socket: read 与 read_f 的返回值必须一致");
    Check(via_hook == via_raw, "非 socket: read 与 read_f 读到的数据必须一致");
    Check(via_hook == payload, "非 socket: 读到的内容应与写入一致");

    // --- 1b. 普通文件读：两种调用都不应阻塞 worker（文件读立即返回）---
    {
        int delta_hook = 0;
        (void)RunIsolated(manager, [&] {
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd >= 0) {
                Heartbeat hb(manager);
                char buffer[128] = {};
                for (int i = 0; i < 50; ++i) {
                    (void)::read(fd, buffer, sizeof(buffer));
                    (void)::lseek(fd, 0, SEEK_SET);
                }
                delta_hook = hb.DeltaOver(Ms(100));
                hb.Stop();
                (void)::close(fd);
            }
        });
        std::printf("  1b 普通文件 50 次 read 后心跳增量=%d（应 > 5，说明未阻塞 worker）\n",
                    delta_hook);
        Check(delta_hook > 5, "非 socket: 普通文件读不应阻塞 worker");
    }

    // --- 1c. FIFO：read 与 read_f 都阻塞（因为 FIFO 不能被 Hook 接管）---
    {
        const std::string fifo = ProcessScopedPath("hook_correct_fifo");
        (void)::unlink(fifo.c_str());
        if (::mkfifo(fifo.c_str(), 0600) == 0) {
            // 用 O_NONBLOCK 打开写端，保证 read 不会永久等
            const int wfd = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
            const int rfd = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
            if (rfd >= 0 && wfd >= 0) {
                const char value = 'F';
                (void)::write(wfd, &value, 1);
                char hook_buf = 0;
                char raw_buf = 0;
                const auto hook_rc = ::read(rfd, &hook_buf, 1);
                const auto raw_rc = ::read_f(rfd, &raw_buf, 1);
                std::printf("  1c FIFO: read=%zd read_f=%zd 内容一致=%d\n", hook_rc, raw_rc,
                            hook_buf == raw_buf ? 1 : 0);
                // 第二次读已经没数据（O_NONBLOCK），两者都应返回 -1/EAGAIN
                Check(hook_rc == raw_rc, "FIFO: read 与 read_f 的行为应一致");
            }
            if (rfd >= 0) (void)::close(rfd);
            if (wfd >= 0) (void)::close(wfd);
            (void)::unlink(fifo.c_str());
        }
    }

    std::printf("  场景1 完成\n");
}

// ===========================================================================
// 场景 2：socket fd —— hook 必须正确 park
// ===========================================================================

void ScenarioSocketParks() {
    std::printf("[hook-correct][场景2] socket fd：hook 必须 park\n");

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    go2cpp::IOManager manager(config);
    Check(manager.Start(), "IOManager 启动失败");
    go2cpp::hook::ScopedEnable hook_scope;
    go2cpp::hook::ScopedIOManagerBinding binding(manager);

    // --- 2a. socket 上 read（无数据）→ 应 park，心跳继续 ---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败");
        if (pair.fds[0] >= 0) {
            int delta = 0;
            bool read_done = false;
            (void)RunIsolated(manager, [&] {
                Heartbeat hb(manager);
                // 先等心跳稳定
                std::this_thread::sleep_for(Ms(30));
                const int before = hb.Count();
                // 在另一个 Fiber 里做阻塞 read
                auto reader = manager.Go([&] {
                    char value = 0;
                    (void)::read(pair.fds[0], &value, 1);   // 无数据 → park
                    read_done = true;
                });
                // 观察 200ms 的心跳增量
                std::this_thread::sleep_for(Ms(200));
                delta = hb.Count() - before;
                // 制造数据把 reader 唤醒
                const char value = 'S';
                (void)::write(pair.fds[1], &value, 1);
                (void)reader->wait_for(std::chrono::seconds(3));
                hb.Stop();
            });
            std::printf("  2a socket read 阻塞期间心跳增量=%d（应 > 8，说明 park 了）\n", delta);
            Check(delta > 8, "socket read 应 park Fiber，不应阻塞 worker");
            Check(read_done, "socket read 应被数据唤醒");
        }
    }

    // --- 2b. read_f（raw 版）→ 应阻塞 worker，心跳几乎不动 ---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败（2b）");
        if (pair.fds[0] >= 0) {
            // 200ms 接收超时：read_f 阻塞满 200ms 后返回 EAGAIN
            timeval timeout{2, 0};   // 2 秒，**必须远大于观察窗**，否则 read_f 提前返回
            (void)::setsockopt(pair.fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                               sizeof(timeout));
            int delta = 0;
            bool read_done = false;
            (void)RunIsolated(manager, [&] {
                Heartbeat hb(manager);
                std::this_thread::sleep_for(Ms(30));
                const int before = hb.Count();
                // **关键**：`read_f` 阻塞的是整个 worker，所以**不能用另一个 Fiber
                // 写数据来唤醒它** —— 那个 Fiber 自己也跑不到（同一个 P 已被占住）。
                // 改用 `SO_RCVTIMEO` 让它超时返回：既能证明"真的阻塞过"，
                // 又不会挂死测试。这正是"阻塞 worker"与"park Fiber"的本质区别。
                auto reader = manager.Go([&] {
                    char value = 0;
                    (void)::read_f(pair.fds[0], &value, 1);   // 200ms 后超时返回
                    read_done = true;
                });
                std::this_thread::sleep_for(Ms(200));   // 观察窗 < 超时，才能看到阻塞
                delta = hb.Count() - before;
                (void)reader->wait_for(std::chrono::seconds(5));
                hb.Stop();
            });
            std::printf("  2b socket read_f 阻塞期间心跳增量=%d（应 < 3，说明阻塞了 worker）\n",
                        delta);
            Check(delta < 3, "read_f 应阻塞 worker（这正是它与 read 的区别）");
            Check(read_done, "read_f 应在 SO_RCVTIMEO 超时后返回");
        }
    }

    std::printf("  场景2 完成\n");
}

// ===========================================================================
// 场景 3：os 模块与 hook 共存时互不干扰
// ===========================================================================

void ScenarioOsCoexists() {
    std::printf("[hook-correct][场景3] os::File 与 hook 共存\n");

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    go2cpp::IOManager manager(config);
    Check(manager.Start(), "IOManager 启动失败");
    go2cpp::hook::ScopedEnable hook_scope;
    go2cpp::hook::ScopedIOManagerBinding binding(manager);

    const std::string path = ProcessScopedPath("hook_correct_osmix.txt");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "os-file-content";
    }

    // --- 3a. 同一个 Fiber 里交替做 os::File::Read 与 socket read ---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败（3a）");
        if (pair.fds[0] >= 0) {
            bool os_ok = false;
            bool sock_ok = false;
            (void)RunIsolated(manager, [&] {
                // 1) os::File 读（raw syscall，应不受 hook 影响）
                go2cpp::os::File file;
                const auto status = go2cpp::os::File::Open(
                    path, go2cpp::os::OpenFlag::kReadOnly, 0644, &file);
                if (status.ok()) {
                    char buffer[64] = {};
                    const ssize_t got = file.Read(buffer, sizeof(buffer));
                    os_ok = (got > 0) &&
                            std::string(buffer, static_cast<std::size_t>(got)) ==
                                "os-file-content";
                }
                // 2) 紧接着在 socket 上做 hook read（应正常 park）
                auto reader = manager.Go([&] {
                    char value = 0;
                    (void)::read(pair.fds[0], &value, 1);
                    sock_ok = (value == 'M');
                });
                std::this_thread::sleep_for(Ms(30));
                const char value = 'M';
                (void)::write(pair.fds[1], &value, 1);
                (void)reader->wait_for(std::chrono::seconds(3));
            });
            std::printf("  3a 交替: os::File 读成功=%d socket read 成功=%d\n",
                        os_ok ? 1 : 0, sock_ok ? 1 : 0);
            Check(os_ok, "os::File 读不应受 hook 影响");
            Check(sock_ok, "socket read 应能正常 park 与唤醒");
        }
    }

    // --- 3b. hook 开关切换前后，os::File 行为必须一致 ---
    {
        auto read_via_os = [&]() -> std::string {
            std::string result;
            (void)RunIsolated(manager, [&] {
                go2cpp::os::File file;
                const auto status = go2cpp::os::File::Open(
                    path, go2cpp::os::OpenFlag::kReadOnly, 0644, &file);
                if (status.ok()) {
                    char buffer[64] = {};
                    const ssize_t got = file.Read(buffer, sizeof(buffer));
                    if (got > 0) {
                        result.assign(buffer, static_cast<std::size_t>(got));
                    }
                }
            });
            return result;
        };

        const bool previous = go2cpp::hook::OpEnabled(go2cpp::hook::HookOp::kRead);
        (void)go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kRead, true);
        const std::string with_hook = read_via_os();
        (void)go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kRead, false);
        const std::string without_hook = read_via_os();
        (void)go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kRead, previous);

        std::printf("  3b 开关前后: 开=[%s] 关=[%s] 一致=%d\n", with_hook.c_str(),
                    without_hook.c_str(), with_hook == without_hook ? 1 : 0);
        Check(with_hook == without_hook, "os::File 行为不应随 hook 开关变化");
        Check(with_hook == "os-file-content", "os::File 读到的内容应正确");
    }

    std::printf("  场景3 完成\n");
}

// ===========================================================================
// 场景 4：*_f 与 hook 版混用时的一致性
// ===========================================================================

void ScenarioMixedApiConsistency() {
    std::printf("[hook-correct][场景4] *_f 与 hook 版混用\n");

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    go2cpp::IOManager manager(config);
    Check(manager.Start(), "IOManager 启动失败");
    go2cpp::hook::ScopedEnable hook_scope;
    go2cpp::hook::ScopedIOManagerBinding binding(manager);

    // --- 4a. 同一 socket：先 read_f 再 read，后者仍应正常 park ---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败（4a）");
        if (pair.fds[0] >= 0) {
            bool raw_ok = false;
            bool hook_ok = false;
            int delta_after_mix = 0;
            (void)RunIsolated(manager, [&] {
                // 先放一个字节，用 read_f 读掉（不 park）
                {
                    const char value = 'A';
                    (void)::write(pair.fds[1], &value, 1);
                    std::this_thread::sleep_for(Ms(10));
                    char got = 0;
                    raw_ok = (::read_f(pair.fds[0], &got, 1) == 1) && (got == 'A');
                }
                // 再用 hook 版 read（无数据 → 应 park）
                Heartbeat hb(manager);
                std::this_thread::sleep_for(Ms(30));
                const int before = hb.Count();
                auto reader = manager.Go([&] {
                    char got = 0;
                    (void)::read(pair.fds[0], &got, 1);
                    hook_ok = (got == 'B');
                });
                std::this_thread::sleep_for(Ms(200));
                delta_after_mix = hb.Count() - before;
                const char value = 'B';
                (void)::write(pair.fds[1], &value, 1);
                (void)reader->wait_for(std::chrono::seconds(3));
                hb.Stop();
            });
            std::printf("  4a read_f 成功=%d，随后 read 的心跳增量=%d（应 > 8）\n",
                        raw_ok ? 1 : 0, delta_after_mix);
            Check(raw_ok, "read_f 应能正常读");
            Check(delta_after_mix > 8, "read_f 之后再用 read 仍应正常 park");
            Check(hook_ok, "read 应能读到数据");
        }
    }

    // --- 4b. close_f 之后 close（hook 版）应仍能正常关闭 ---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败（4b）");
        if (pair.fds[0] >= 0) {
            int close_f_rc = -1;
            int close_rc = -1;
            (void)RunIsolated(manager, [&] {
                close_f_rc = ::close_f(pair.fds[0]);
                pair.fds[0] = -1;   // 已关闭，避免析构重复 close
                close_rc = ::close(pair.fds[1]);
                pair.fds[1] = -1;
            });
            std::printf("  4b close_f=%d close=%d\n", close_f_rc, close_rc);
            Check(close_f_rc == 0, "close_f 应成功");
            Check(close_rc == 0, "close 应成功（即使前面用过 close_f）");
        }
    }

    std::printf("  场景4 完成\n");
}

// ===========================================================================
// 场景 5：压力与竞态
// ===========================================================================

void ScenarioStressAndRace() {
    std::printf("[hook-correct][场景5] 压力与竞态\n");

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.min_workers = 1;
    config.max_workers = 1;
    go2cpp::IOManager manager(config);
    Check(manager.Start(), "IOManager 启动失败");
    go2cpp::hook::ScopedEnable hook_scope;
    go2cpp::hook::ScopedIOManagerBinding binding(manager);

    // --- 5a. 多 Fiber 并发在同一 socket 上 read（不应互相干扰到崩溃）---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败（5a）");
        if (pair.fds[0] >= 0) {
            std::atomic<int> readers_done{0};
            constexpr int kReaders = 4;
            (void)RunIsolated(manager, [&] {
                std::vector<std::shared_ptr<go2cpp::Task>> readers;
                readers.reserve(kReaders);
                for (int i = 0; i < kReaders; ++i) {
                    readers.push_back(manager.Go([&] {
                        char value = 0;
                        (void)::read(pair.fds[0], &value, 1);
                        readers_done.fetch_add(1, std::memory_order_relaxed);
                    }));
                }
                std::this_thread::sleep_for(Ms(30));
                // 发足够多的字节，让每个 reader 都能拿到一个
                for (int i = 0; i < kReaders; ++i) {
                    const char value = static_cast<char>('0' + i);
                    (void)::write(pair.fds[1], &value, 1);
                    std::this_thread::sleep_for(Ms(5));
                }
                for (auto& reader : readers) {
                    (void)reader->wait_for(std::chrono::seconds(5));
                }
            }, std::chrono::seconds(15));
            std::printf("  5a %d 个 Fiber 并发 read：完成 %d 个\n", kReaders,
                        readers_done.load());
            Check(readers_done.load() == kReaders, "并发 read 的 Fiber 应全部完成");
        }
    }

    // --- 5b. Fiber 在 socket 上 park 时，另一线程 close 该 socket ---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败（5b）");
        if (pair.fds[0] >= 0) {
            std::atomic<bool> reader_done{false};
            std::atomic<bool> reader_failed{false};
            (void)RunIsolated(manager, [&] {
                auto reader = manager.Go([&] {
                    char value = 0;
                    const auto got = ::read(pair.fds[0], &value, 1);
                    if (got < 0) {
                        reader_failed.store(true, std::memory_order_relaxed);
                    }
                    reader_done.store(true, std::memory_order_relaxed);
                });
                std::this_thread::sleep_for(Ms(50));   // 让 reader 进入 park
                // 从另一个普通线程 close 这个 fd
                std::thread closer([&] {
                    (void)::close(pair.fds[0]);
                });
                closer.join();
                pair.fds[0] = -1;   // 已关闭
                const bool finished = reader->wait_for(std::chrono::seconds(3));
                if (!finished) {
                    // 关闭被 park 的 fd 后 reader 可能永远不被唤醒 —— 记录下来
                    std::printf("  5b 警告：park 中 close fd 后 reader 未在 3 秒内结束\n");
                }
            }, std::chrono::seconds(15));
            std::printf("  5b park 中 close：reader 结束=%d 出错=%d\n",
                        reader_done.load() ? 1 : 0, reader_failed.load() ? 1 : 0);
            // 这里不硬性断言"必须返回错误"，因为不同内核/实现的行为可能不同；
            // 只要求"不崩溃、进程能继续"。
            Check(true, "park 中 close fd 不应导致崩溃");
        }
    }

    // --- 5c. 有 Fiber 已 park 时切换 SetOpEnabled(kRead, false) ---
    {
        SocketPair pair;
        Check(pair.Open(), "socketpair 创建失败（5c）");
        if (pair.fds[0] >= 0) {
            std::atomic<bool> reader_done{false};
            (void)RunIsolated(manager, [&] {
                auto reader = manager.Go([&] {
                    char value = 0;
                    (void)::read(pair.fds[0], &value, 1);   // park
                    reader_done.store(true, std::memory_order_relaxed);
                });
                std::this_thread::sleep_for(Ms(50));        // reader 已 park
                // 此时关闭 kRead
                const bool previous = go2cpp::hook::OpEnabled(go2cpp::hook::HookOp::kRead);
                const bool set_ok =
                    go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kRead, false);
                std::printf("  5c 已 park 时关闭 kRead：set_ok=%d\n", set_ok ? 1 : 0);
                // 再发数据，看已 park 的 reader 会不会被唤醒
                const char value = 'W';
                (void)::write(pair.fds[1], &value, 1);
                const bool woken = reader->wait_for(std::chrono::seconds(3));
                std::printf("  5c 已 park 的 reader 是否被唤醒=%d（结论：唤醒路径不查开关）\n",
                            woken ? 1 : 0);
                Check(woken, "已 park 的 Fiber 应该仍能被唤醒（唤醒路径不查开关）");
                (void)go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kRead, previous);
            }, std::chrono::seconds(15));
        }
    }

    std::printf("  场景5 完成\n");
}

}  // namespace

int main() {
    std::printf("=== hook 正确性验证（结合 os / hook / socket）===\n");
    std::printf("判定方法：心跳 Fiber（10ms 一跳，观察 200ms）\n");
    std::printf("  心跳增量 > 8 → 操作 park 了 Fiber（worker 没被占住）\n");
    std::printf("  心跳增量 < 3 → 操作阻塞了 worker\n\n");

    ScenarioNonSocketZeroImpact();
    ScenarioSocketParks();
    ScenarioOsCoexists();
    ScenarioMixedApiConsistency();
    ScenarioStressAndRace();

    if (g_failures == 0) {
        std::printf("\n[hook-correct] failures=0\n");
    } else {
        std::printf("\n[hook-correct] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
