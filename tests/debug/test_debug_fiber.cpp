/**
 * @file test_debug_fiber.cpp
 * @brief DebugFiber 与 DebugRegistry 的测试。
 *
 * 覆盖（按项目规范：压测 + 随机 + >=5 个真实场景）：
 *   场景 1 带描述的 Fiber 构造与查询
 *   场景 2 未注册 Fiber 的查询语义（present=false，与"已注册但字段为空"区分）
 *   场景 3 特殊字符（引号/换行/中文/超长）的元数据承载
 *   场景 4 Fiber::id() 与注册表的关联查询
 *   场景 5 多线程并发注册/查询
 *   压测   1000 次 DebugFiber 创建销毁 + 20000 次注册表读写
 *   随机   随机名字/描述/行号 500 组，逐组校验读回一致
 */

#include "go2cpp/debug.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string RandomText(std::mt19937& rng, std::size_t max_len) {
    static const char kAlphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-. /";
    std::uniform_int_distribution<std::size_t> len_dist(0, max_len);
    std::uniform_int_distribution<std::size_t> ch_dist(0, sizeof(kAlphabet) - 2);
    const std::size_t len = len_dist(rng);
    std::string out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kAlphabet[ch_dist(rng)]);
    }
    return out;
}

}  // namespace

void run_debug_fiber_tests() {
    go2cpp_tests::announce("debug fiber metadata and registry");
    using namespace go2cpp;
    using namespace go2cpp::debug;

    // 场景 1：带描述的 Fiber 构造与查询
    {
        FiberDebugInfo info;
        info.name = "s_main_task";
        info.description = "worker loop for demo";
        info.file = "example/demo.cpp";
        info.line = 128;
        auto fiber = std::make_unique<DebugFiber>([] {}, info);
        FiberDebugInfo expected = info;
        expected.present = true;  // 构造即视为已注册
        GO2CPP_CHECK(fiber->info() == expected);
        GO2CPP_CHECK(fiber->description() == "worker loop for demo");
        GO2CPP_CHECK(fiber->name() == "s_main_task");
        GO2CPP_CHECK(fiber->debug_id() == fiber->id());
        GO2CPP_CHECK(fiber->info().present);
        // 继承来的状态查询仍可用
        GO2CPP_CHECK(fiber->stack_size() > 0);
        fiber->set_description("changed");
        GO2CPP_CHECK(fiber->description() == "changed");
    }

    // 场景 2：未注册查询语义（present=false，不得与"已注册但字段为空"混淆）
    {
        auto& registry = DebugRegistry::Instance();
        registry.Clear();
        GO2CPP_CHECK(!registry.Lookup(123456).has_value());
        GO2CPP_CHECK(registry.Size() == 0);

        FiberDebugInfo empty_info;  // 各字段为空
        registry.Register(7, empty_info);
        const auto found = registry.Lookup(7);
        GO2CPP_CHECK(found.has_value());
        GO2CPP_CHECK(found->present);          // 已注册
        GO2CPP_CHECK(found->name.empty());     // 但没有内容
        GO2CPP_CHECK(!registry.Lookup(8).has_value());
        GO2CPP_CHECK(registry.Unregister(7));
        GO2CPP_CHECK(!registry.Unregister(7));  // 二次注销返回 false
    }

    // 场景 3：特殊字符承载（含引号、换行、中文、超长）
    {
        auto& registry = DebugRegistry::Instance();
        registry.Clear();
        FiberDebugInfo info;
        info.name = "name_with_\"quotes\"";
        info.description = "第一行\n第二行\t制表符 \\ 反斜杠";
        info.file = "路径/含中文/文件.cpp";
        info.line = 999999;
        std::string long_desc(4096, 'x');
        info.description += long_desc;
        registry.Register(42, info);
        const auto found = registry.Lookup(42);
        GO2CPP_CHECK(found.has_value());
        GO2CPP_CHECK(found->name == info.name);
        GO2CPP_CHECK(found->description == info.description);
        GO2CPP_CHECK(found->description.size() == info.description.size());
        GO2CPP_CHECK(found->file == "路径/含中文/文件.cpp");
        GO2CPP_CHECK(found->line == 999999);
    }

    // 场景 4：Fiber::id() 与注册表关联
    {
        auto& registry = DebugRegistry::Instance();
        registry.Clear();
        std::vector<DebugFiber::Ptr> fibers;
        for (int i = 0; i < 8; ++i) {
            FiberDebugInfo info;
            info.name = "task_" + std::to_string(i);
            info.line = static_cast<std::uint32_t>(100 + i);
            auto fiber = std::make_unique<DebugFiber>([] {}, info);
            registry.Register(fiber->debug_id(), fiber->info());
            fibers.push_back(std::move(fiber));
        }
        GO2CPP_CHECK(registry.Size() == 8);
        for (int i = 0; i < 8; ++i) {
            const auto found = registry.Lookup(fibers[static_cast<std::size_t>(i)]->debug_id());
            GO2CPP_CHECK(found.has_value());
            GO2CPP_CHECK(found->name == "task_" + std::to_string(i));
            GO2CPP_CHECK(found->line == static_cast<std::uint32_t>(100 + i));
        }
        const auto snapshot = registry.Snapshot();
        GO2CPP_CHECK(snapshot.size() == 8);
    }

    // 场景 5：多线程并发注册/查询（4 线程 × 500 次）
    {
        auto& registry = DebugRegistry::Instance();
        registry.Clear();
        constexpr int kThreads = 4;
        constexpr int kPerThread = 500;
        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t, &mismatches] {
                auto& local_registry = DebugRegistry::Instance();
                for (int i = 0; i < kPerThread; ++i) {
                    const std::uint64_t id =
                        static_cast<std::uint64_t>(t) * 100000U +
                        static_cast<std::uint64_t>(i) + 1U;
                    FiberDebugInfo info;
                    info.name = "t" + std::to_string(t) + "_" + std::to_string(i);
                    info.line = static_cast<std::uint32_t>(i);
                    local_registry.Register(id, info);
                    const auto found = local_registry.Lookup(id);
                    if (!found.has_value() || found->name != info.name ||
                        found->line != info.line) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        GO2CPP_CHECK(mismatches.load(std::memory_order_relaxed) == 0);
        GO2CPP_CHECK(registry.Size() ==
                     static_cast<std::size_t>(kThreads * kPerThread));
    }

    // 压测：1000 次 DebugFiber 创建销毁 + 20000 次注册表读写
    {
        auto& registry = DebugRegistry::Instance();
        registry.Clear();
        constexpr int kCreateCount = 1000;
        const auto create_started = std::chrono::steady_clock::now();
        for (int i = 0; i < kCreateCount; ++i) {
            FiberDebugInfo info;
            info.name = "bench";
            info.line = static_cast<std::uint32_t>(i);
            auto fiber = std::make_unique<DebugFiber>([] {}, info);
            registry.Register(fiber->debug_id(), fiber->info());
        }
        const auto create_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - create_started);
        GO2CPP_CHECK(registry.Size() == static_cast<std::size_t>(kCreateCount));

        constexpr int kOps = 20000;
        const auto ops_started = std::chrono::steady_clock::now();
        for (int i = 0; i < kOps; ++i) {
            FiberDebugInfo info;
            info.name = "op";
            registry.Register(static_cast<std::uint64_t>(i) + 1000000U, info);
            (void)registry.Lookup(static_cast<std::uint64_t>(i) + 1000000U);
        }
        const auto ops_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - ops_started);
        // 只报告量级，不作性能结论（性能结论必须来自 build-bench）。
        std::clog << "  [压测] " << kCreateCount << " 次创建+注册: "
                  << create_elapsed.count() << " us; " << kOps << " 次注册+查询: "
                  << ops_elapsed.count() << " us" << std::endl;
        GO2CPP_CHECK(create_elapsed.count() > 0);
        GO2CPP_CHECK(ops_elapsed.count() > 0);
        registry.Clear();
    }

    // 随机测试：500 组随机元数据，逐组校验读回一致
    {
        auto& registry = DebugRegistry::Instance();
        registry.Clear();
        std::mt19937 rng(20261016);
        std::uniform_int_distribution<std::uint32_t> line_dist(0, 1000000);
        bool all_ok = true;
        for (int i = 0; i < 500; ++i) {
            FiberDebugInfo info;
            info.name = RandomText(rng, 32);
            info.description = RandomText(rng, 256);
            info.file = RandomText(rng, 64);
            info.line = line_dist(rng);
            const auto id = static_cast<std::uint64_t>(i) + 1U;
            registry.Register(id, info);
            const auto found = registry.Lookup(id);
            all_ok = all_ok && found.has_value();
            if (found.has_value()) {
                all_ok = all_ok && found->name == info.name &&
                         found->description == info.description &&
                         found->file == info.file && found->line == info.line;
            }
        }
        GO2CPP_CHECK(all_ok);
        GO2CPP_CHECK(registry.Size() == 500);
        registry.Clear();
    }
}
