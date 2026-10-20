/**
 * @file adopt_demo.cpp
 * @brief 真实 `go2cpp::Fiber` / `Channel<T>` 的跨进程迁移 demo。
 *
 * ## 为什么单独一个程序
 *
 * 迁移必须是**两个进程**：进程 A 在 Fiber 等待 channel 时导出镜像，
 * 进程 B 按镜像重建栈与等待状态，再投递消息、恢复执行。用同一个可执行
 * 文件的不同子命令启动两个进程，可以保证 build-id、代码布局与模块路径
 * 完全一致（这是恢复的前置条件，也是快照校验的一部分）。
 *
 * ## 运行
 *
 * ```bash
 * export MIGRATION_SIGNING_KEY="$(head -c 32 /dev/urandom | xxd -p -c 64)"
 * ./go2cpp_fiber_adopt_demo dump    /tmp/fiber-adopt.mig
 * ./go2cpp_fiber_adopt_demo restore /tmp/fiber-adopt.mig
 * ./go2cpp_fiber_adopt_demo selftest /tmp/fiber-adopt-selftest
 * ```
 *
 * ## 子集限制
 *
 * 只支持无缓冲 channel、单生产者单消费者、无 select、无 deadline/context
 * 的接收等待。复杂场景由 `Channel<T>::ExportWaitState` 明确拒绝。
 */
#if !defined(GO2CPP_MIGRATION_API) || !GO2CPP_MIGRATION_API
#error "adopt_demo 需要 -DGO2CPP_MIGRATION_API=1（cmake -DGO2CPP_MIGRATION_API=ON）"
#endif

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/fiber.hpp"
#include "go2cpp/tools/go.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

namespace {

#if defined(GO2CPP_ADOPT_DEMO_ALT)
// 反例二进制：内容与主 demo 不同，因此 build-id 不同；用它导出的快照
// 必须被主 demo 拒绝（代码布局/编译选项可能不同，不能按模块偏移恢复）。
const char* const kAdoptDemoAltMarker =
    "go2cpp-fiber-adopt-demo-alt-build-id-marker-v1";
#endif

std::atomic<go2cpp::Fiber*> g_fiber{nullptr};
go2cpp::Channel<int>* g_channel = nullptr;
std::atomic<bool> g_body_started{false};

const char* StateName(go2cpp::FiberState state) {
    switch (state) {
    case go2cpp::FiberState::Ready:
        return "Ready";
    case go2cpp::FiberState::Running:
        return "Running";
    case go2cpp::FiberState::Suspended:
        return "Suspended";
    case go2cpp::FiberState::Completed:
        return "Completed";
    case go2cpp::FiberState::Failed:
        return "Failed";
    }
    return "Unknown";
}

/// Fiber 主体：开启迁移、登记自己、在无缓冲 channel 上阻塞接收。
void FiberBody() {
    go2cpp::Fiber* const self = go2cpp::Fiber::Current();
    if (self == nullptr) {
        std::printf("[body] 不在 Fiber 中运行\n");
        return;
    }
    self->EnableMigration();
    g_fiber.store(self, std::memory_order_release);
    g_body_started.store(true, std::memory_order_release);
    std::printf("[body] fiber#%llu 启动，准备 <-ch 阻塞接收\n",
                static_cast<unsigned long long>(self->id()));
    std::fflush(stdout);

    auto result = g_channel->Recv();
    if (!result.ok || !result.value.has_value()) {
        std::printf("[body] 接收失败（status=%d）\n",
                    static_cast<int>(result.status));
        return;
    }
    std::printf("[body] fiber 继续执行，收到消息 = %d\n", *result.value);
    std::fflush(stdout);
}

/// 等待 Fiber 进入挂起状态（park 在迁移等待点）。
go2cpp::Fiber* WaitForSuspendedFiber(int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        go2cpp::Fiber* const fiber = g_fiber.load(std::memory_order_acquire);
        if (fiber != nullptr &&
            fiber->state() == go2cpp::FiberState::Suspended) {
            return fiber;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

std::string SelfPath() {
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    return std::string(buffer, static_cast<std::size_t>(length));
}

int RunDump(const std::string& path) {
    go2cpp::Channel<int> channel;
    g_channel = &channel;
    std::printf("[A] 启动 Fiber：go2cpp::go(...) 提交到默认 Scheduler\n");
    const auto task = go2cpp::go(&FiberBody);
    if (!task) {
        std::printf("[A] go() 提交失败\n");
        return 1;
    }
    go2cpp::Fiber* const fiber = WaitForSuspendedFiber(5000);
    if (fiber == nullptr) {
        std::printf("[A] 等待 Fiber 挂起超时（state=%s）\n",
                    g_fiber.load() != nullptr
                        ? StateName(g_fiber.load()->state())
                        : "null");
        return 1;
    }
    std::printf("[A] Fiber 已 park：state=%s，等待通道 %p\n",
                StateName(fiber->state()), static_cast<void*>(&channel));

    std::string error;
    go2cpp::FiberMigrationOptions export_options;
    // 受控 demo：等待路径会留下容器内部的死值（deque block / 临时
    // shared_ptr），它们不可迁移但也不再被使用。显式允许清零，审计计数
    // 仍会写进快照摘要；生产应消除残留指针并保持默认拒绝。
    export_options.allow_dead_heap_slots = true;
    if (!fiber->ExportMigrationImage(path, export_options, &error)) {
        std::printf("[A] 导出失败：%s\n", error.c_str());
        return 1;
    }
    std::printf("[A] 导出成功：%s\n", path.c_str());
    std::printf("[A] 进程 A 退出（Fiber 栈与等待状态已写入快照）\n");
    std::fflush(stdout);
    // Scheduler 的 worker 仍持有挂起的 G；demo 用 _Exit 直接结束进程，
    // 不做 shutdown 排空（真实系统应由迁移协调者决定旧进程的收尾方式）。
    std::_Exit(0);
}

int RunRestore(const std::string& path) {
    go2cpp::Channel<int> channel;
    go2cpp::FiberMigrationOptions options;
    options.channel = &channel;
    std::string error;
    auto fiber = go2cpp::Fiber::ImportMigrationImage(path, options, &error);
    if (!fiber) {
        std::printf("[B] 导入失败：%s\n", error.c_str());
        return 1;
    }
    std::printf("[B] 导入成功：fiber#%llu state=%s\n",
                static_cast<unsigned long long>(fiber->id()),
                StateName(fiber->state()));
    std::printf("[B] 等待者已挂回目标进程的 channel %p\n",
                static_cast<void*>(&channel));

    const auto sent = channel.Send(42);
    if (sent.status != go2cpp::ChannelStatus::kReady) {
        std::printf("[B] 发送失败：status=%d\n", static_cast<int>(sent.status));
        return 1;
    }
    std::printf("[B] 消息 42 已送达等待队列，恢复 Fiber\n");
    std::fflush(stdout);
    if (!fiber->resume()) {
        std::printf("[B] resume 失败（state=%s）\n", StateName(fiber->state()));
        return 1;
    }
    if (fiber->state() != go2cpp::FiberState::Completed) {
        std::printf("[B] Fiber 未完成：state=%s failure=%s\n",
                    StateName(fiber->state()),
                    fiber->failure() ? "yes" : "no");
        return 1;
    }
    std::printf("[B] Fiber 恢复执行并正常结束：state=%s\n",
                StateName(fiber->state()));
    return 0;
}

/// fork + exec 指定可执行文件；返回子进程退出码（-1 表示异常终止）。
int RunChildPath(const std::string& executable,
                 const std::vector<std::string>& arguments) {
    if (executable.empty()) {
        return -1;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        std::vector<std::string> storage = arguments;
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (auto& argument : storage) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        std::_Exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/// fork + exec 当前可执行文件。
int RunChild(const std::vector<std::string>& arguments) {
    return RunChildPath(SelfPath(), arguments);
}

int RunSelfTest(const std::string& directory) {
    const std::string dump_path = directory + "/fiber-adopt.mig";
    int failures = 0;
    const auto check = [&failures](bool condition, const char* name,
                                   const std::string& detail) {
        std::printf("[selftest] %s %s%s%s\n", condition ? "PASS" : "FAIL", name,
                    detail.empty() ? "" : "：", detail.c_str());
        if (!condition) {
            ++failures;
        }
    };

    // 1) 正常跨进程迁移：子进程导出 → 父进程导入 → 发送 → 恢复 → 完成。
    {
        const int child = RunChild({"dump", dump_path});
        check(child == 0, "dump 子进程", "exit=" + std::to_string(child));
        go2cpp::Channel<int> channel;
        go2cpp::FiberMigrationOptions options;
        options.channel = &channel;
        std::string error;
        auto fiber = go2cpp::Fiber::ImportMigrationImage(dump_path, options, &error);
        bool ok = fiber != nullptr;
        if (ok) {
            const auto sent = channel.Send(7);
            ok = sent.status == go2cpp::ChannelStatus::kReady &&
                 fiber->resume() &&
                 fiber->state() == go2cpp::FiberState::Completed;
        }
        check(ok, "正常迁移闭环", error);
    }

    // 2) 同一快照重复导入（随机消息值）：恢复必须可重复。
    {
        std::mt19937 generator(20261020U);
        std::uniform_int_distribution<int> distribution(-100000, 100000);
        bool ok = true;
        std::string detail;
        constexpr int kStressRounds = 100;
        const auto stress_start = std::chrono::steady_clock::now();
        for (int index = 0; index < kStressRounds && ok; ++index) {
            go2cpp::Channel<int> channel;
            go2cpp::FiberMigrationOptions options;
            options.channel = &channel;
            std::string error;
            auto fiber = go2cpp::Fiber::ImportMigrationImage(dump_path, options, &error);
            if (!fiber) {
                ok = false;
                detail = error;
                break;
            }
            const int value = distribution(generator);
            const auto sent = channel.Send(value);
            ok = sent.status == go2cpp::ChannelStatus::kReady &&
                 fiber->resume() &&
                 fiber->state() == go2cpp::FiberState::Completed;
            if (!ok) {
                detail = "第 " + std::to_string(index) + " 轮恢复失败";
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - stress_start);
        const double elapsed_ms = static_cast<double>(elapsed.count()) / 1e6;
        std::printf("[selftest] 恢复压测：%d 次，总耗时 %.3f ms，平均 %.1f us/次\n",
                    kStressRounds, elapsed_ms,
                    elapsed_ms * 1000.0 / static_cast<double>(kStressRounds));
        check(ok, "重复导入 100 次", detail);
    }

    // 3) 篡改快照（翻转一个字节）必须被 CRC/签名拒绝。
    {
        const std::string tampered = directory + "/fiber-adopt-tampered.mig";
        std::string bytes;
        {
            FILE* file = std::fopen(dump_path.c_str(), "rb");
            if (file != nullptr) {
                char buffer[4096];
                std::size_t n = 0;
                while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
                    bytes.append(buffer, n);
                }
                std::fclose(file);
            }
        }
        bool ok = bytes.size() > 512;
        if (ok) {
            bytes[bytes.size() / 2] ^= 0x5a;
            FILE* file = std::fopen(tampered.c_str(), "wb");
            ok = file != nullptr;
            if (ok) {
                std::fwrite(bytes.data(), 1, bytes.size(), file);
                std::fclose(file);
            }
        }
        go2cpp::Channel<int> channel;
        go2cpp::FiberMigrationOptions options;
        options.channel = &channel;
        std::string error;
        auto fiber = go2cpp::Fiber::ImportMigrationImage(tampered, options, &error);
        check(ok && fiber == nullptr, "拒绝篡改快照", error);
        std::remove(tampered.c_str());
    }

    // 4) 未配置签名密钥时导出必须失败（默认拒绝生成未签名快照）。
    {
        const std::string unsigned_path = directory + "/fiber-adopt-unsigned.mig";
        const char* saved = std::getenv("MIGRATION_SIGNING_KEY");
        const std::string saved_value = saved != nullptr ? saved : "";
        const char* saved_file = std::getenv("MIGRATION_SIGNING_KEY_FILE");
        const std::string saved_file_value = saved_file != nullptr ? saved_file : "";
        ::unsetenv("MIGRATION_SIGNING_KEY");
        ::unsetenv("MIGRATION_SIGNING_KEY_FILE");
        go2cpp::Channel<int> channel;
        g_channel = &channel;
        g_fiber.store(nullptr);
        auto task = go2cpp::go(&FiberBody);
        go2cpp::Fiber* fiber = WaitForSuspendedFiber(5000);
        std::string error;
        bool exported = false;
        go2cpp::FiberMigrationOptions export_options;
        export_options.allow_dead_heap_slots = true;
        if (fiber != nullptr) {
            exported =
                fiber->ExportMigrationImage(unsigned_path, export_options, &error);
        }
        if (!saved_value.empty()) {
            ::setenv("MIGRATION_SIGNING_KEY", saved_value.c_str(), 1);
        }
        if (!saved_file_value.empty()) {
            ::setenv("MIGRATION_SIGNING_KEY_FILE", saved_file_value.c_str(), 1);
        }
        check(fiber != nullptr && !exported, "拒绝无密钥导出", error);
        std::remove(unsigned_path.c_str());
    }

    // 5) 复杂等待场景必须被明确拒绝：两个接收等待者。
    {
        go2cpp::Channel<int> channel;
        g_channel = &channel;
        g_fiber.store(nullptr);
        std::atomic<int> parked{0};
        auto first = go2cpp::go([&] {
            go2cpp::Fiber::Current()->EnableMigration();
            parked.fetch_add(1);
            (void)g_channel->Recv();
        });
        auto second = go2cpp::go([&] {
            go2cpp::Fiber::Current()->EnableMigration();
            parked.fetch_add(1);
            (void)g_channel->Recv();
        });
        for (int index = 0; index < 5000 && parked.load() < 2; ++index) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        go2cpp::ChannelWaitState wait;
        std::string error;
        const bool rejected = !channel.ExportWaitState(&wait, &error);
        check(rejected, "拒绝多等待者导出", error);
        // 解开两个 Fiber：各发一条消息后它们会完成。
        (void)channel.Send(1);
        (void)channel.Send(2);
        (void)first;
        (void)second;
    }

    // 6) 栈上未登记堆指针必须被拒绝（std::string 的堆缓冲在栈帧上）。
    {
        go2cpp::Channel<int> channel;
        g_channel = &channel;
        g_fiber.store(nullptr);
        std::atomic<bool> ready{false};
        auto task = go2cpp::go([&] {
            go2cpp::Fiber* const self = go2cpp::Fiber::Current();
            self->EnableMigration();
            g_fiber.store(self, std::memory_order_release);
            std::string heap_payload(256, 'x');
            heap_payload[0] = 'y';
            ready.store(true);
            (void)g_channel->Recv();
            std::printf("[selftest] heap_payload 长度=%zu\n", heap_payload.size());
        });
        go2cpp::Fiber* fiber = nullptr;
        for (int index = 0; index < 5000; ++index) {
            fiber = g_fiber.load(std::memory_order_acquire);
            if (fiber != nullptr && fiber->state() == go2cpp::FiberState::Suspended &&
                ready.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::string error;
        const bool exported =
            fiber != nullptr && fiber->ExportMigrationImage(dump_path + ".heap", {}, &error);
        check(fiber != nullptr && !exported, "拒绝未登记堆指针", error);
        (void)channel.Send(3);
        (void)ready;
        (void)task;
    }

    // 7) 关闭通道的等待者导出必须被拒绝。
    {
        go2cpp::Channel<int> channel;
        g_channel = &channel;
        g_fiber.store(nullptr);
        auto task = go2cpp::go(&FiberBody);
        go2cpp::Fiber* fiber = WaitForSuspendedFiber(5000);
        channel.Close();
        std::string error;
        go2cpp::ChannelWaitState wait;
        const bool rejected = fiber != nullptr && !channel.ExportWaitState(&wait, &error);
        check(rejected, "拒绝已关闭通道导出", error);
        (void)task;
    }

    // 8) build-id 校验：另一个可执行文件生成的快照必须被拒绝（代码布局不同）。
    {
        const std::string self = SelfPath();
        const auto slash = self.find_last_of('/');
        const std::string directory_of_self =
            slash == std::string::npos ? std::string(".") : self.substr(0, slash);
        const std::string alt = directory_of_self + "/go2cpp_fiber_adopt_demo_alt";
        const std::string alt_dump = directory + "/fiber-adopt-alt.mig";
        const int child = RunChildPath(alt, {"dump", alt_dump});
        bool rejected = false;
        std::string detail = "alt 可执行文件不存在或不匹配";
        if (child == 0) {
            go2cpp::Channel<int> channel;
            go2cpp::FiberMigrationOptions options;
            options.channel = &channel;
            std::string error;
            auto fiber = go2cpp::Fiber::ImportMigrationImage(alt_dump, options, &error);
            rejected = fiber == nullptr;
            detail = error;
        }
        check(rejected, "拒绝其他 build-id 的快照", detail);
        std::remove(alt_dump.c_str());
    }

    std::printf("[selftest] 结果：%s（失败 %d 项）\n",
                failures == 0 ? "全部通过" : "存在失败", failures);
    std::fflush(stdout);
    // 场景 4/5/6/7 会留下挂起的 Fiber 与等待节点；demo 不需要 Scheduler 的
    // 优雅收尾，直接结束进程，避免 shutdown 排空卡住。
    std::_Exit(failures == 0 ? 0 : 1);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(GO2CPP_ADOPT_DEMO_ALT)
    // 让反例二进制的 marker 真正参与链接，保证它的 build-id 与主 demo 不同。
    std::printf("[alt] build marker: %s\n", kAdoptDemoAltMarker);
#endif
    if (argc < 2) {
        std::printf(
            "用法：\n"
            "  go2cpp_fiber_adopt_demo dump     <file>\n"
            "  go2cpp_fiber_adopt_demo restore  <file>\n"
            "  go2cpp_fiber_adopt_demo selftest <directory>\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "dump" && argc == 3) {
        return RunDump(argv[2]);
    }
    if (mode == "restore" && argc == 3) {
        return RunRestore(argv[2]);
    }
    if (mode == "selftest" && argc == 3) {
        return RunSelfTest(argv[2]);
    }
    std::printf("未知子命令或参数数量不对：%s\n", mode.c_str());
    return 2;
}
