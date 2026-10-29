/**
 * @file audit_error_reflect.cpp
 * @brief bento::error / bento::reflect 深度审计：压测、随机错误图、真实场景。
 *
 * @details 本文件是独立审计程序（不注册进 ctest，避免把压测拖进单元测试）：
 * - 压测：Wrap/Join/As/Has/WalkErrors/Registry/Caster/Bind，每项 >= 100 万次；
 * - 随机：1000+ 随机错误图（深包裹/大 Join/共享子图/自环/双节点环）；
 * - 场景：包裹链 + 映射、visitor 收集器、注册表并发幂等、caster 正反例、
 *   bind 配置绑定、JSON 环/转义/截断（覆盖全部功能点）。
 *
 * 构建（Release 等价优化）：
 *   g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
 *       -Iinclude -Itests/bento tests/bento/audit_error_reflect.cpp \
 *       src/bento/error/{error,registry,mapping}.cpp -pthread -o /tmp/audit_error_reflect
 * 运行：/tmp/audit_error_reflect
 *
 * ASAN 构建时定义 AUDIT_NO_NEW_OVERRIDE，避免与 sanitizer 的分配器冲突。
 *
 * @author go2cpp contributors
 * @date 2026-10-29
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "go2cpp/bento/error/error.hpp"
#include "go2cpp/bento/error/mapping.hpp"
#include "go2cpp/bento/error/registry.hpp"
#include "go2cpp/bento/reflect/bind.hpp"
#include "go2cpp/bento/reflect/caster.hpp"
#include "go2cpp/bento/reflect/reflect.hpp"
#include "test_error_support.hpp"

// GCC 13 对“自定义全局 operator new/delete + 内联分配器”存在
// -Wmismatched-new-delete 误报（分配/释放都由本文件的 malloc/free 完成）。
// 该抑制只作用于本审计工具的分配计数重载，不涉及被测库代码。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {

using namespace go2cpp::bento::error;

// ===========================================================================
// 分配计数：仅在显式开启的单线程段计数，压测段零额外开销。
// ===========================================================================

std::atomic<std::uint64_t> g_new_calls{0};
std::atomic<std::uint64_t> g_new_bytes{0};
bool g_count_allocations = false;

/// @brief 记录一次分配；仅在显式开启计数的单线程段生效。
[[maybe_unused]] void NoteAllocation(std::size_t size) {
    if (g_count_allocations) {
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
        g_new_bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

/// @brief 通过函数指针释放，避免 GCC 的自定义分配器配对误报（-Wmismatched-new-delete）。
[[maybe_unused]] void Deallocate(void* ptr) noexcept {
    static void (*const free_fn)(void*) = std::free;
    free_fn(ptr);
}

} // namespace

#ifndef AUDIT_NO_NEW_OVERRIDE
void* operator new(std::size_t size) {
    NoteAllocation(size);
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void operator delete(void* ptr) noexcept {
    Deallocate(ptr);
}

void operator delete(void* ptr, std::size_t /*size*/) noexcept {
    Deallocate(ptr);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    NoteAllocation(size);
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = (size + align - 1U) / align * align;
    void* ptr = nullptr;
    if (::posix_memalign(&ptr, align, rounded == 0 ? align : rounded) != 0) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void operator delete(void* ptr, std::align_val_t /*alignment*/) noexcept {
    Deallocate(ptr);
}
#endif

namespace {

// ===========================================================================
// 随机源与计时
// ===========================================================================

class Rng {
 public:
    explicit Rng(std::uint64_t seed) : m_state(seed) {}

    std::uint64_t Next() noexcept {
        m_state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = m_state;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    std::uint64_t Below(std::uint64_t bound) noexcept { return bound == 0 ? 0 : Next() % bound; }

 private:
    std::uint64_t m_state;
};

struct BenchResult {
    const char* op = "";
    std::uint64_t ops = 0;
    double total_ms = 0.0;
    double ns_per_op = 0.0;
    double p50_ns = 0.0;
    double p95_ns = 0.0;
    double p99_ns = 0.0;
};

double Percentile(std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t index =
        std::min(sorted.size() - 1,
                 static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())));
    return sorted[index];
}

template <typename Fn>
BenchResult Measure(const char* op, std::uint64_t ops, std::size_t batch, Fn fn) {
    const std::uint64_t warmup = std::min<std::uint64_t>(ops, 20000);
    for (std::uint64_t index = 0; index < warmup; ++index) {
        fn();
    }
    if (batch == 0) {
        batch = 1;
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(ops / batch) + 2);
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t done = 0;
    while (done < ops) {
        const std::uint64_t count = std::min<std::uint64_t>(batch, ops - done);
        const auto batch_start = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < count; ++index) {
            fn();
        }
        const auto batch_end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(batch_end - batch_start).count() /
                          static_cast<double>(count));
        done += count;
    }
    const auto end = std::chrono::steady_clock::now();

    BenchResult result;
    result.op = op;
    result.ops = ops;
    result.total_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.ns_per_op = result.total_ms * 1e6 / static_cast<double>(ops);
    std::sort(samples.begin(), samples.end());
    result.p50_ns = Percentile(samples, 0.50);
    result.p95_ns = Percentile(samples, 0.95);
    result.p99_ns = Percentile(samples, 0.99);
    return result;
}

void PrintBench(const BenchResult& result) {
    std::printf("[bench] %-30s ops=%9llu  %8.2f ns/op  %9.0f ops/s  p50=%7.2f p95=%7.2f p99=%7.2f\n",
                result.op, static_cast<unsigned long long>(result.ops), result.ns_per_op,
                result.ns_per_op > 0.0 ? 1e9 / result.ns_per_op : 0.0, result.p50_ns,
                result.p95_ns, result.p99_ns);
}

template <typename Fn>
std::uint64_t CountAllocations(std::uint64_t ops, Fn fn) {
    const std::uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_allocations = true;
    for (std::uint64_t index = 0; index < ops; ++index) {
        fn();
    }
    g_count_allocations = false;
    return g_new_calls.load(std::memory_order_relaxed) - before;
}

std::uint64_t ReadRssKb() {
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return 0;
    }
    char line[256];
    std::uint64_t rss = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            rss = std::strtoull(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(file);
    return rss;
}

// ===========================================================================
// 审计用错误类型
// ===========================================================================

class TaggedError final : public Error {
 public:
    explicit TaggedError(int tag) : m_tag(tag) {}

    BENTO_ERROR_CLASS("audit.TaggedError", "bento.Error", 1)

    std::string Message() const override { return "tagged-error"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
    int Tag() const noexcept { return m_tag; }

 private:
    int m_tag;
};

/// @brief 自环/互环节点：UnwrapAll 返回 weak 指向的节点，用于验证环安全。
class CyclicError final : public Error {
 public:
    BENTO_ERROR_CLASS("audit.CyclicError", "bento.Error", 1)

    std::string Message() const override { return "cyclic"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
    bool IsWrapping() const noexcept override { return true; }

    void SetTarget(const ErrorPtr& target) { m_target = target; }

    std::vector<ErrorPtr> UnwrapAll(std::size_t /*level*/) const override {
        std::vector<ErrorPtr> result;
        if (const ErrorPtr target = m_target.lock()) {
            result.push_back(target);
        }
        return result;
    }

 private:
    std::weak_ptr<const Error> m_target;
};

class AppSqlError final : public Error {
 public:
    explicit AppSqlError(std::string state) : m_state(std::move(state)) {}

    BENTO_ERROR_CLASS("app.SqlError", "bento.Error", 1)

    std::string Message() const override { return "sql failed: " + m_state; }
    ErrorCode Code() const noexcept override { return ErrorCode::kUnavailable; }
    const std::string& State() const noexcept { return m_state; }

 private:
    std::string m_state;
};

// ===========================================================================
// 参考遍历（与实现同语义的前序 DFS + 地址去重）
// ===========================================================================

void RefTraverse(const ErrorPtr& node, std::vector<const Error*>& order,
                 std::unordered_set<const Error*>& visited) {
    if (!node) {
        return;
    }
    if (!visited.insert(node.get()).second) {
        return;
    }
    order.push_back(node.get());
    if (!node->IsWrapping()) {
        return;
    }
    const std::vector<ErrorPtr> children = node->UnwrapAll();
    for (const ErrorPtr& child : children) {
        RefTraverse(child, order, visited);
    }
}

// ===========================================================================
// 压测：error 查询与构造
// ===========================================================================

void PrintAllocationStats() {
    const ErrorPtr leaf = NewError("leaf", ErrorCode::kNotFound);
    const ErrorPtr graph = Join({Wrap(leaf, "a"), NewError("b", ErrorCode::kInternal),
                                 Wrap(NewError("c", ErrorCode::kDataLoss), "d")});

    const std::uint64_t wrap_allocs = CountAllocations(1000, [&leaf]() {
        const ErrorPtr wrapped = Wrap(leaf, "ctx");
        (void)wrapped;
    });
    const std::uint64_t join_allocs = CountAllocations(1000, [&leaf]() {
        const ErrorPtr joined = Join({leaf, leaf});
        (void)joined;
    });
    const std::uint64_t as_allocs = CountAllocations(1000, [&graph]() {
        const auto found = As<StringError>(graph);
        (void)found;
    });
    const std::uint64_t has_allocs = CountAllocations(1000, [&graph]() {
        (void)Has(graph, ErrorCode::kDataLoss);
    });
    const std::uint64_t walk_allocs = CountAllocations(1000, [&graph]() {
        const WalkResult walked = WalkErrors(
            graph, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
        (void)walked;
    });
    std::printf("[alloc] Wrap=%.2f/op Join=%.2f/op As=%.2f/op Has(code)=%.2f/op Walk=%.2f/op\n",
                static_cast<double>(wrap_allocs) / 1000.0,
                static_cast<double>(join_allocs) / 1000.0,
                static_cast<double>(as_allocs) / 1000.0,
                static_cast<double>(has_allocs) / 1000.0,
                static_cast<double>(walk_allocs) / 1000.0);
}

void RunErrorBenchmarks() {
    const ErrorPtr leaf = NewError("leaf", ErrorCode::kNotFound);
    const ErrorPtr leaf_hit = NewError("hit", ErrorCode::kDataLoss);
    const ErrorPtr tagged = std::make_shared<TaggedError>(42);
    // 8 节点图：Join(Wrap(leaf), Join(leaf_hit, Wrap(tagged, "ctx")), leaf)
    const ErrorPtr graph = Join({Wrap(leaf, "ctx"), Join({leaf_hit, Wrap(tagged, "tag")}),
                                 NewError("other", ErrorCode::kInternal)});

    PrintBench(Measure("Wrap(leaf)", 1000000, 4096, [&leaf]() {
        const ErrorPtr wrapped = Wrap(leaf, "ctx");
        (void)wrapped;
    }));
    PrintBench(Measure("Join({leaf,leaf_hit})", 1000000, 4096, [&leaf, &leaf_hit]() {
        const ErrorPtr joined = Join({leaf, leaf_hit});
        (void)joined;
    }));
    PrintBench(Measure("As<StringError> hit", 1000000, 4096, [&graph]() {
        const auto found = As<StringError>(graph);
        (void)found;
    }));
    PrintBench(Measure("As<TaggedError> hit", 1000000, 4096, [&graph]() {
        const auto found = As<TaggedError>(graph);
        (void)found;
    }));
    PrintBench(Measure("As<AppSqlError> miss", 1000000, 4096, [&graph]() {
        const auto found = As<AppSqlError>(graph);
        (void)found;
    }));
    PrintBench(Measure("Has<TaggedError>", 1000000, 4096, [&graph]() {
        (void)Has<TaggedError>(graph);
    }));
    PrintBench(Measure("Has(code) hit", 1000000, 4096, [&graph]() {
        (void)Has(graph, ErrorCode::kDataLoss);
    }));
    PrintBench(Measure("Has(code) miss", 1000000, 4096, [&graph]() {
        (void)Has(graph, static_cast<ErrorCode>(99));
    }));
    PrintBench(Measure("Is(root, leaf)", 1000000, 4096, [&graph, &leaf]() {
        (void)Is(graph, leaf);
    }));
    PrintBench(Measure("WalkErrors(8 nodes)", 1000000, 2048, [&graph]() {
        const WalkResult walked = WalkErrors(
            graph, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
        (void)walked;
    }));
    PrintBench(Measure("UnwrapAll(0) 8 nodes", 200000, 512, [&graph]() {
        const std::vector<ErrorPtr> all = graph->UnwrapAll(0);
        (void)all;
    }));
    PrintBench(Measure("ErrorMessage(8 nodes)", 200000, 512, [&graph]() {
        const std::string message = ErrorMessage(graph);
        (void)message;
    }));
}

void RunRegistryBenchmarks() {
    ErrorCodeRegistry& codes = ErrorCodeRegistry::Instance();
    const std::string_view names[] = {
        "bento.protocol", "bento.frame", "bento.checksum", "bento.session", "bento.peer",
        "bento.timeout",  "bento.crypto", "bento.config",  "bento.internal", "bento.constraint",
    };
    PrintBench(Measure("Registry.Lookup", 1000000, 4096, [&codes, &names]() {
        static std::size_t cursor = 0;
        const std::string_view name = names[cursor++ % 10];
        (void)codes.Lookup(name);
    }));
    PrintBench(Measure("Registry.Name", 1000000, 4096, [&codes]() {
        static std::uint32_t cursor = 1;
        (void)codes.Name(cursor++ % 12U);
    }));

    ErrorTypeRegistry& types = ErrorTypeRegistry::Instance();
    PrintBench(Measure("TypeRegistry.Create", 1000000, 2048, [&types]() {
        const ErrorPtr created = types.Create("bento.StringError");
        (void)created;
    }));
    PrintBench(Measure("TypeRegistry.Has", 1000000, 4096, [&types]() {
        (void)types.Has("bento.WrappedError");
    }));
    PrintBench(Measure("TypeRegistry.Depth", 1000000, 4096, [&types]() {
        (void)types.Depth("bento.JoinError");
    }));
    PrintBench(Measure("TypeRegistry.ParentName", 1000000, 4096, [&types]() {
        (void)types.ParentName("bento.StringError");
    }));
}

void RunReflectBenchmarks() {
    volatile std::size_t sink = 0;
    PrintBench(Measure("Describe<int> (compile-time)", 1000000, 4096, [&sink]() {
        const go2cpp::bento::reflect::TypeDescriptor desc =
            go2cpp::bento::reflect::Describe<int>();
        sink += desc.size + static_cast<std::size_t>(desc.kind);
    }));
    PrintBench(Measure("NameOf<std::string>", 1000000, 4096, [&sink]() {
        sink += std::strlen(go2cpp::bento::reflect::NameOf<std::string>());
    }));
    PrintBench(Measure("SizeOf<double>", 1000000, 4096, [&sink]() {
        sink += go2cpp::bento::reflect::SizeOf<double>();
    }));

    const go2cpp::bento::reflect::caster::StringToInt to_int;
    const go2cpp::bento::reflect::caster::StringToDouble to_double;
    const go2cpp::bento::reflect::caster::StringToBool to_bool;
    int int_value = 0;
    double double_value = 0.0;
    bool bool_value = false;
    // 输入用运行期数组而不是字面量：字面量让编译器把整段解析折叠成常量
    // （实测 1M 次 0.01 ns/op，说明循环被优化掉了）。
    const std::string int_texts[] = {"-123456789", "123456789", "0", "2147483647",
                                     "-2147483648"};
    const std::string double_texts[] = {"-12345.6789e3", "1.5", "-0.25", "1e-3",
                                        "3.141592653589793"};
    const std::string bool_texts[] = {"TRUE", "false", "yes", "0", "NO"};
    std::size_t text_cursor = 0;
    // volatile 累加：否则解析结果未被观察，整个循环会被优化掉。
    volatile std::int64_t caster_sink = 0;
    PrintBench(Measure("caster int", 1000000, 4096,
                       [&to_int, &int_value, &int_texts, &text_cursor, &caster_sink]() {
                           if (to_int(int_texts[text_cursor++ % 5U], int_value)) {
                               caster_sink += int_value;
                           }
                       }));
    PrintBench(Measure("caster double", 1000000, 4096,
                       [&to_double, &double_value, &double_texts, &text_cursor, &caster_sink]() {
                           if (to_double(double_texts[text_cursor++ % 5U], double_value)) {
                               caster_sink += static_cast<std::int64_t>(double_value);
                           }
                       }));
    PrintBench(Measure("caster bool", 1000000, 4096,
                       [&to_bool, &bool_value, &bool_texts, &text_cursor, &caster_sink]() {
                           if (to_bool(bool_texts[text_cursor++ % 5U], bool_value)) {
                               caster_sink += bool_value ? 1 : 0;
                           }
                       }));
    std::printf("[bench] caster_sink=%lld\n", static_cast<long long>(caster_sink));
    std::printf("[bench] sink=%zu (防止编译期折叠)\n", static_cast<std::size_t>(sink));
}

void RunMappingBenchmarks() {
    const ErrorPtr root = Wrap(Join({NewError("a", ErrorCode::kNotFound), NewError("b")}), "ctx");
    PrintBench(Measure("MapHttpStatus(code)", 1000000, 4096, []() {
        (void)MapHttpStatus(ErrorCode::kNotFound);
    }));
    PrintBench(Measure("MapLogLevel(code)", 1000000, 4096, []() {
        (void)MapLogLevel(ErrorCode::kCancelled);
    }));
    PrintBench(Measure("DefaultJson(leaf)", 200000, 512, []() {
        static const ErrorPtr leaf = NewError("leaf", ErrorCode::kNotFound);
        const std::string json = DefaultJson(leaf);
        (void)json;
    }));
    PrintBench(Measure("DefaultJson(3-node graph)", 100000, 256, [&root]() {
        const std::string json = DefaultJson(root);
        (void)json;
    }));
}

// ===========================================================================
// 压测：深链 / 宽 Join / 并发 / 内存
// ===========================================================================

void RunDeepChainBenchmarks() {
    constexpr std::size_t kDepth = 100000;
    const ErrorPtr leaf = NewError("leaf", ErrorCode::kDataLoss);
    ErrorPtr deep = leaf;
    const auto build_start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kDepth; ++index) {
        deep = Wrap(deep, "w");
    }
    const auto build_end = std::chrono::steady_clock::now();
    const double build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();
    std::printf("[deep] build %zu layers: %.2f ms (%.1f ns/layer)\n", kDepth, build_ms,
                build_ms * 1e6 / static_cast<double>(kDepth));

    const auto as_start = std::chrono::steady_clock::now();
    std::size_t found_count = 0;
    for (int index = 0; index < 100; ++index) {
        if (As<StringError>(deep)) {
            ++found_count;
        }
    }
    const auto as_end = std::chrono::steady_clock::now();
    const double as_ms = std::chrono::duration<double, std::milli>(as_end - as_start).count();
    std::printf("[deep] As<StringError> x100: %.2f ms (%.1f ns/query, %.1f ns/node, found=%zu)\n",
                as_ms, as_ms * 1e6 / 100.0, as_ms * 1e6 / (100.0 * static_cast<double>(kDepth)),
                found_count);

    const auto has_start = std::chrono::steady_clock::now();
    for (int index = 0; index < 100; ++index) {
        (void)Has(deep, ErrorCode::kDataLoss);
    }
    const auto has_end = std::chrono::steady_clock::now();
    const double has_ms = std::chrono::duration<double, std::milli>(has_end - has_start).count();
    std::printf("[deep] Has(code) x100: %.2f ms (%.1f ns/node)\n", has_ms,
                has_ms * 1e6 / (100.0 * static_cast<double>(kDepth)));

    const auto walk_start = std::chrono::steady_clock::now();
    std::size_t walked_total = 0;
    for (int index = 0; index < 20; ++index) {
        const WalkResult walked = WalkErrors(
            deep, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
        walked_total += walked.errors.size();
    }
    const auto walk_end = std::chrono::steady_clock::now();
    const double walk_ms = std::chrono::duration<double, std::milli>(walk_end - walk_start).count();
    std::printf("[deep] WalkErrors x20: %.2f ms (%.1f ns/node, nodes=%zu)\n", walk_ms,
                walk_ms * 1e6 / (20.0 * static_cast<double>(kDepth)), walked_total / 20);

    const auto message_start = std::chrono::steady_clock::now();
    std::size_t message_size = 0;
    for (int index = 0; index < 20; ++index) {
        message_size = ErrorMessage(deep).size();
    }
    const auto message_end = std::chrono::steady_clock::now();
    const double message_ms =
        std::chrono::duration<double, std::milli>(message_end - message_start).count();
    std::printf("[deep] Message x20: %.2f ms (%.1f ns/layer, bytes=%zu)\n", message_ms,
                message_ms * 1e6 / (20.0 * static_cast<double>(kDepth)), message_size);

    const std::size_t rss_before = ReadRssKb();
    const auto unwind_start = std::chrono::steady_clock::now();
    for (int index = 0; index < 5; ++index) {
        const std::vector<ErrorPtr> all = deep->UnwrapAll(0);
        (void)all;
    }
    const auto unwind_end = std::chrono::steady_clock::now();
    const double unwind_ms =
        std::chrono::duration<double, std::milli>(unwind_end - unwind_start).count();
    std::printf("[deep] UnwrapAll(0) x5: %.2f ms, RSS %.1f MB\n", unwind_ms,
                static_cast<double>(rss_before) / 1024.0);

    deep.reset();
    std::printf("[deep] destroy 100k chain: RSS %.1f MB, leaf alive=%d\n",
                static_cast<double>(ReadRssKb()) / 1024.0, leaf != nullptr ? 1 : 0);
}

void RunWideJoinBenchmarks() {
    constexpr std::size_t kWidth = 100000;
    std::vector<ErrorPtr> causes;
    causes.reserve(kWidth);
    for (std::size_t index = 0; index < kWidth; ++index) {
        causes.push_back(NewError("e" + std::to_string(index), ErrorCode::kInternal));
    }
    const ErrorPtr wide = Join(std::move(causes));

    const auto unwind_start = std::chrono::steady_clock::now();
    std::size_t seen = 0;
    for (int index = 0; index < 5; ++index) {
        seen = wide->UnwrapAll(0).size();
    }
    const auto unwind_end = std::chrono::steady_clock::now();
    std::printf("[wide] UnwrapAll(0) x5 width=%zu: %.2f ms\n", seen,
                std::chrono::duration<double, std::milli>(unwind_end - unwind_start).count());

    const auto walk_start = std::chrono::steady_clock::now();
    const WalkResult walked = WalkErrors(
        wide, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
    const auto walk_end = std::chrono::steady_clock::now();
    std::printf("[wide] WalkErrors width=%zu: %.2f ms (nodes=%zu)\n", kWidth,
                std::chrono::duration<double, std::milli>(walk_end - walk_start).count(),
                walked.errors.size());

    const auto message_start = std::chrono::steady_clock::now();
    const std::string message = ErrorMessage(wide);
    const auto message_end = std::chrono::steady_clock::now();
    std::printf("[wide] Message width=%zu: %.2f ms (bytes=%zu, lines=%zu)\n", kWidth,
                std::chrono::duration<double, std::milli>(message_end - message_start).count(),
                message.size(),
                static_cast<std::size_t>(std::count(message.begin(), message.end(), '\n')) + 1U);
}

void RunConcurrentBenchmark() {
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 250000;
    const ErrorPtr shared = Join({Wrap(NewError("leaf", ErrorCode::kNotFound), "ctx"),
                                  NewError("other", ErrorCode::kInternal)});
    std::atomic<int> failures{0};
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&shared, &failures]() {
            ErrorPtr local = NewError("local", ErrorCode::kCancelled);
            for (int index = 0; index < kOpsPerThread; ++index) {
                switch (index & 3) {
                    case 0:
                        local = Wrap(local, "w");
                        if (local->Code() != ErrorCode::kWrappedError) {
                            ++failures;
                        }
                        break;
                    case 1:
                        if (!Is(shared, shared)) {
                            ++failures;
                        }
                        break;
                    case 2:
                        if (!Has(shared, ErrorCode::kInternal)) {
                            ++failures;
                        }
                        break;
                    default: {
                        const WalkResult walked = WalkErrors(
                            shared, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
                        if (walked.errors.size() != 4U) {
                            ++failures;
                        }
                        break;
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const auto end = std::chrono::steady_clock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(end - start).count();
    const std::uint64_t total_ops =
        static_cast<std::uint64_t>(kThreads) * static_cast<std::uint64_t>(kOpsPerThread);
    std::printf("[concurrent] %d threads x %d ops = %llu ops in %.1f ms, %.0f ops/s, failures=%d\n",
                kThreads, kOpsPerThread, static_cast<unsigned long long>(total_ops), total_ms,
                static_cast<double>(total_ops) * 1e3 / total_ms, failures.load());
    BENTO_ERROR_CHECK_EQ(failures.load(), 0);
}

void RunMemoryBenchmark() {
    const std::uint64_t rss_start = ReadRssKb();
    std::uint64_t rss_peak = rss_start;

    {
        ErrorPtr deep = NewError("leaf");
        for (int index = 0; index < 300000; ++index) {
            deep = Wrap(deep, "w");
        }
        rss_peak = ReadRssKb();
        std::printf("[memory] 300k wrap chain: RSS %.1f MB (delta %.1f MB, %.1f B/layer)\n",
                    static_cast<double>(rss_peak) / 1024.0,
                    static_cast<double>(rss_peak - rss_start) / 1024.0,
                    static_cast<double>(rss_peak - rss_start) * 1024.0 / 300000.0);
    }
    const std::uint64_t rss_after_chain = ReadRssKb();
    std::printf("[memory] after chain release: RSS %.1f MB (delta %.1f MB)\n",
                static_cast<double>(rss_after_chain) / 1024.0,
                static_cast<double>(rss_after_chain - rss_start) / 1024.0);

    {
        std::vector<ErrorPtr> causes;
        causes.reserve(200000);
        for (int index = 0; index < 200000; ++index) {
            causes.push_back(NewError("e" + std::to_string(index)));
        }
        const ErrorPtr wide = Join(std::move(causes));
        std::printf("[memory] 200k wide join: RSS %.1f MB (delta %.1f MB, %.1f B/cause)\n",
                    static_cast<double>(ReadRssKb()) / 1024.0,
                    static_cast<double>(ReadRssKb() - rss_start) / 1024.0,
                    static_cast<double>(ReadRssKb() - rss_start) * 1024.0 / 200000.0);
        (void)wide;
    }
    std::printf("[memory] final RSS %.1f MB\n", static_cast<double>(ReadRssKb()) / 1024.0);
}

// ===========================================================================
// 随机错误图
// ===========================================================================

struct RandomGraph {
    ErrorPtr root;
    std::vector<ErrorPtr> nodes;
    std::vector<ErrorPtr> leaves;
    std::vector<ErrorPtr> tagged;
    bool cyclic = false;
};

ErrorPtr BuildRandomNode(Rng& rng, int depth, int max_depth, std::uint64_t& sequence,
                         RandomGraph& graph) {
    const std::uint64_t choice = rng.Below(100);
    const bool at_leaf = depth >= max_depth;
    if (at_leaf || choice < 35) {
        if (rng.Below(10) < 3) {
            const ErrorPtr node = std::make_shared<TaggedError>(static_cast<int>(rng.Below(1000)));
            graph.nodes.push_back(node);
            graph.tagged.push_back(node);
            return node;
        }
        const ErrorPtr node =
            NewError("leaf-" + std::to_string(sequence++),
                     static_cast<ErrorCode>(static_cast<std::int32_t>(rng.Below(16))));
        graph.nodes.push_back(node);
        graph.leaves.push_back(node);
        return node;
    }
    if (choice < 65) {
        // 20% 直接复用已构造节点形成共享子图（DAG）；否则新建子树。
        // 注意不能“先建子树再丢弃”，否则 nodes 里会留下不可达节点。
        ErrorPtr cause;
        if (!graph.nodes.empty() && rng.Below(100) < 20) {
            cause = graph.nodes[static_cast<std::size_t>(rng.Below(graph.nodes.size()))];
        } else {
            cause = BuildRandomNode(rng, depth + 1, max_depth, sequence, graph);
        }
        const ErrorPtr node = Wrap(std::move(cause), "ctx-" + std::to_string(sequence++));
        graph.nodes.push_back(node);
        return node;
    }
    if (choice < 95) {
        const std::size_t count = 2U + static_cast<std::size_t>(rng.Below(5));
        std::vector<ErrorPtr> causes;
        causes.reserve(count + 1);
        for (std::size_t index = 0; index < count; ++index) {
            causes.push_back(BuildRandomNode(rng, depth + 1, max_depth, sequence, graph));
        }
        // 25% 重复一个原因，验证共享子图去重。
        if (rng.Below(100) < 25) {
            causes.push_back(causes.front());
        }
        const ErrorPtr node = Join(std::move(causes));
        graph.nodes.push_back(node);
        return node;
    }
    auto cyclic = std::make_shared<CyclicError>();
    cyclic->SetTarget(cyclic);
    graph.nodes.push_back(cyclic);
    graph.cyclic = true;
    return cyclic;
}

void ValidateRandomGraph(const RandomGraph& graph, int tree_index) {
    if (!graph.root) {
        BENTO_ERROR_CHECK(false);
        return;
    }
    std::vector<const Error*> order;
    std::unordered_set<const Error*> visited;
    RefTraverse(graph.root, order, visited);

    const WalkResult walked = WalkErrors(
        graph.root, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
    BENTO_ERROR_CHECK_EQ(walked.ok, true);
    BENTO_ERROR_CHECK_EQ(walked.errors.size(), order.size());
    if (walked.errors.size() == order.size()) {
        for (std::size_t index = 0; index < order.size(); ++index) {
            BENTO_ERROR_CHECK_EQ(walked.errors[index].get(), order[index]);
        }
    }

    // As<T> 必须返回参考遍历中第一个匹配节点。
    const auto tagged = As<TaggedError>(graph.root);
    const TaggedError* expected_tagged = nullptr;
    for (const Error* node : order) {
        if (const auto* candidate = dynamic_cast<const TaggedError*>(node)) {
            expected_tagged = candidate;
            break;
        }
    }
    BENTO_ERROR_CHECK_EQ(tagged.get(), expected_tagged);

    // Has(code) 与参考遍历一致。
    std::unordered_set<int> codes;
    for (const Error* node : order) {
        codes.insert(static_cast<int>(node->Code()));
    }
    for (const int code : codes) {
        bool expected = false;
        for (const Error* node : order) {
            if (static_cast<int>(node->Code()) == code) {
                expected = true;
                break;
            }
        }
        BENTO_ERROR_CHECK_EQ(Has(graph.root, static_cast<ErrorCode>(code)), expected);
    }

    // Is 对每个可达节点成立。
    for (const ErrorPtr& node : graph.nodes) {
        if (node) {
            BENTO_ERROR_CHECK(Is(graph.root, node));
        }
    }

    // UnwrapAll(0) 必须地址去重，且元素全部可达。
    const std::vector<ErrorPtr> descendants = graph.root->UnwrapAll(0);
    std::unordered_set<const Error*> unique;
    for (const ErrorPtr& node : descendants) {
        if (!node) {
            BENTO_ERROR_CHECK(false);
            continue;
        }
        BENTO_ERROR_CHECK(unique.insert(node.get()).second);
        BENTO_ERROR_CHECK_EQ(visited.count(node.get()), std::size_t{1});
    }
    BENTO_ERROR_CHECK(descendants.size() <= order.size());

    // 无环图：Message 必须有界且不抛；有环图只验证遍历类接口。
    if (!graph.cyclic) {
        const std::string message = ErrorMessage(graph.root);
        BENTO_ERROR_CHECK(message.size() <= order.size() * 64U + 64U);
    }
    const std::string json = MapJson(graph.root);
    BENTO_ERROR_CHECK(!json.empty());
    BENTO_ERROR_CHECK(json.size() <= order.size() * 512U + 4096U);
    (void)tree_index;
}

void TestRandomGraphs() {
    Rng rng(0x5eeda11ce5ULL);
    int trees = 0;
    for (int tree_index = 0; tree_index < 1200; ++tree_index) {
        RandomGraph graph;
        std::uint64_t sequence = static_cast<std::uint64_t>(tree_index) * 1000ULL;
        const int max_depth = 1 + static_cast<int>(rng.Below(10));
        graph.root = BuildRandomNode(rng, 0, max_depth, sequence, graph);
        ValidateRandomGraph(graph, tree_index);
        ++trees;
    }
    std::printf("[random] graphs=%d checks=%d failures=%d\n", trees,
                bento_error_test::g_checks, bento_error_test::g_failures);
}

void TestRandomDeepChains() {
    Rng rng(0xdecafbadULL);
    for (int tree_index = 0; tree_index < 12; ++tree_index) {
        const std::size_t depth =
            2000U + static_cast<std::size_t>(rng.Below(18000));
        const ErrorPtr leaf = NewError("leaf", ErrorCode::kDataLoss);
        ErrorPtr root = leaf;
        for (std::size_t index = 0; index < depth; ++index) {
            root = Wrap(root, "w");
        }
        BENTO_ERROR_CHECK(Is(root, leaf));
        BENTO_ERROR_CHECK(Has(root, ErrorCode::kDataLoss));
        BENTO_ERROR_CHECK_EQ(As<StringError>(root).get(), leaf.get());
        BENTO_ERROR_CHECK_EQ(root->UnwrapAll(0).size(), depth);
        const std::string message = ErrorMessage(root);
        BENTO_ERROR_CHECK_EQ(message.size(), depth * 3U + 4U);
        const WalkResult walked = WalkErrors(
            root, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
        BENTO_ERROR_CHECK_EQ(walked.errors.size(), depth + 1);
    }
    std::printf("[random] deep chains=12 depth=2k..20k done\n");
}

void TestRandomWideJoins() {
    Rng rng(0xabcdef01ULL);
    for (int join_index = 0; join_index < 12; ++join_index) {
        const std::size_t width = 1000U + static_cast<std::size_t>(rng.Below(4000));
        std::vector<ErrorPtr> causes;
        causes.reserve(width);
        for (std::size_t index = 0; index < width; ++index) {
            causes.push_back(NewError("e" + std::to_string(index),
                                      static_cast<ErrorCode>(static_cast<std::int32_t>(
                                          rng.Below(16)))));
        }
        const ErrorPtr root = Join(std::move(causes));
        BENTO_ERROR_CHECK_EQ(root->UnwrapAll(0).size(), width);
        BENTO_ERROR_CHECK_EQ(root->UnwrapAll(1).size(), width);
        const std::string message = ErrorMessage(root);
        BENTO_ERROR_CHECK_EQ(static_cast<std::size_t>(
                                 std::count(message.begin(), message.end(), '\n')) + 1U,
                             width);
        const WalkResult walked = WalkErrors(
            root, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
        BENTO_ERROR_CHECK_EQ(walked.errors.size(), width + 1);
    }
    std::printf("[random] wide joins=12 width=1k..5k done\n");
}

void TestRandomCycles() {
    for (int cycle_index = 0; cycle_index < 200; ++cycle_index) {
        auto first = std::make_shared<CyclicError>();
        auto second = std::make_shared<CyclicError>();
        first->SetTarget(second);
        second->SetTarget(first);
        const ErrorPtr root = Join({first, second, NewError("tail", ErrorCode::kDataLoss)});

        BENTO_ERROR_CHECK(Is(root, first));
        BENTO_ERROR_CHECK(Is(root, second));
        BENTO_ERROR_CHECK(Has(root, ErrorCode::kDataLoss));
        BENTO_ERROR_CHECK_EQ(As<CyclicError>(root).get(), first.get());
        const WalkResult walked = WalkErrors(
            root, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
        BENTO_ERROR_CHECK_EQ(walked.errors.size(), std::size_t{4});
        const std::vector<ErrorPtr> descendants = root->UnwrapAll(0);
        std::unordered_set<const Error*> unique;
        for (const ErrorPtr& node : descendants) {
            BENTO_ERROR_CHECK(unique.insert(node.get()).second);
        }
        BENTO_ERROR_CHECK(descendants.size() <= 3U);
        const std::string json = MapJson(root);
        BENTO_ERROR_CHECK(json.size() < 8192U);
    }
    std::printf("[random] cycles=200 (2-node mutual + tail) done\n");
}

// ===========================================================================
// 场景 1：包裹链 + 映射（HTTP/日志/JSON/As 提取）
// ===========================================================================

void ScenarioWrappedChain() {
    // 模拟 HTTP handler -> repository -> SQL driver 的三层包裹。
    const ErrorPtr driver = Wrap(NewError("connection refused", ErrorCode::kUnavailable), "dial");
    const ErrorPtr repository = Wrap(driver, "query user:42");
    const ErrorPtr handler = Wrap(repository, "GET /api/user/42 failed");

    BENTO_ERROR_CHECK_EQ(ErrorMessage(handler),
                         std::string("GET /api/user/42 failed: query user:42: dial: "
                                     "connection refused"));
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(handler), 500);   // 包装节点码 15 -> 500
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(NewError("x", ErrorCode::kUnavailable)), 503);
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(NewError("x", ErrorCode::kNotFound)), 404);
    BENTO_ERROR_CHECK_EQ(MapLogLevel(handler), LogLevel::kError);
    BENTO_ERROR_CHECK_EQ(MapLogLevel(NewError("x", ErrorCode::kNotFound)), LogLevel::kWarn);

    // 业务代码：从包装链中提取底层错误类型并读取结构化字段。
    const ErrorPtr sql = Wrap(std::make_shared<AppSqlError>("42P01"), "query failed");
    const auto sql_error = As<AppSqlError>(sql);
    BENTO_ERROR_CHECK(sql_error != nullptr);
    if (sql_error) {
        BENTO_ERROR_CHECK_EQ(sql_error->State(), std::string("42P01"));
    }
    BENTO_ERROR_CHECK(Has<AppSqlError>(Wrap(sql, "handler")));

    // JSON：结构与转义必须同时成立。
    const std::string json = DefaultJson(handler);
    BENTO_ERROR_CHECK(json.find("\"type\":\"bento.WrappedError\"") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"code_name\":\"WRAPPED_ERROR\"") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"depth\":1") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"wrapping\":true") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"message\":\"GET /api/user/42 failed: query user:42: dial: "
                                "connection refused\"") != std::string::npos);
    BENTO_ERROR_CHECK_EQ(static_cast<std::size_t>(std::count(json.begin(), json.end(), '\n')),
                         std::size_t{0});

    // 无错误的正常路径。
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(ErrorPtr{}), 200);
    BENTO_ERROR_CHECK_EQ(MapLogLevel(ErrorPtr{}), LogLevel::kInfo);
    BENTO_ERROR_CHECK_EQ(DefaultJson(ErrorPtr{}), std::string("null"));
}

// ===========================================================================
// 场景 2：visitor 收集器（跳过/停止/深度）
// ===========================================================================

void ScenarioVisitorCollector() {
    const ErrorPtr root = Join({Wrap(NewError("db down", ErrorCode::kUnavailable), "db"),
                                Join({NewError("cache miss", ErrorCode::kNotFound),
                                      Wrap(NewError("timeout", ErrorCode::kDeadlineExceeded),
                                           "rpc")}),
                                NewError("request failed", ErrorCode::kInternal)});

    // 收集器：跳过 WrappedError 的子节点（只保留包装上下文，不展开原因）。
    const WalkResult skipped = WalkErrors(root, [](const ErrorPtr& node) {
        return node->TypeName() == std::string_view("bento.WrappedError")
                   ? ErrorVisitResult::kSkipChildren
                   : ErrorVisitResult::kContinue;
    });
    BENTO_ERROR_CHECK(skipped.ok);
    BENTO_ERROR_CHECK_EQ(skipped.errors.size(), std::size_t{6});
    bool saw_db_leaf = false;
    bool saw_rpc_leaf = false;
    for (const ErrorPtr& node : skipped.errors) {
        if (node->Message() == "db down") {
            saw_db_leaf = true;
        }
        if (node->Message() == "timeout") {
            saw_rpc_leaf = true;
        }
    }
    BENTO_ERROR_CHECK(!saw_db_leaf);
    BENTO_ERROR_CHECK(!saw_rpc_leaf);

    // 深度聚合：只统计叶子错误码。
    int leaf_count = 0;
    int wrapping_count = 0;
    const WalkResult all = WalkErrors(root, [&](const ErrorPtr& node) {
        if (node->IsWrapping()) {
            ++wrapping_count;
        } else {
            ++leaf_count;
        }
        return ErrorVisitResult::kContinue;
    });
    BENTO_ERROR_CHECK(all.ok);
    BENTO_ERROR_CHECK_EQ(all.errors.size(), std::size_t{8});
    BENTO_ERROR_CHECK_EQ(wrapping_count, 4);
    BENTO_ERROR_CHECK_EQ(leaf_count, 4);

    // 早停：命中第一个 kUnavailable 就停止。
    int stop_visits = 0;
    const WalkResult stopped = WalkErrors(root, [&stop_visits](const ErrorPtr& node) {
        ++stop_visits;
        return node->Code() == ErrorCode::kUnavailable ? ErrorVisitResult::kStop
                                                       : ErrorVisitResult::kContinue;
    });
    BENTO_ERROR_CHECK_EQ(stopped.status, ErrorVisitResult::kStop);
    BENTO_ERROR_CHECK_EQ(stopped.errors.size(), std::size_t{3});
    BENTO_ERROR_CHECK_EQ(stop_visits, 3);

    // visitor 抛异常：保留已访问部分并标记 ok=false。
    const WalkResult thrown = WalkErrors(root, [](const ErrorPtr&) -> ErrorVisitResult {
        throw std::runtime_error("collector failure");
    });
    BENTO_ERROR_CHECK(!thrown.ok);
    BENTO_ERROR_CHECK_EQ(thrown.errors.size(), std::size_t{1});
    BENTO_ERROR_CHECK_EQ(thrown.status, ErrorVisitResult::kContinue);
}

// ===========================================================================
// 场景 3：注册表并发幂等
// ===========================================================================

void ScenarioRegistryIdempotency() {
    ErrorCodeRegistry& codes = ErrorCodeRegistry::Instance();
    ErrorTypeRegistry& types = ErrorTypeRegistry::Instance();

    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::vector<std::uint32_t> allocated(static_cast<std::size_t>(kThreads) * kPerThread, 0);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([thread_index, &allocated, &failures]() {
            for (int index = 0; index < kPerThread; ++index) {
                const std::string name = "scenario.code." + std::to_string(thread_index) + "." +
                                         std::to_string(index);
                const std::size_t slot =
                    static_cast<std::size_t>(thread_index) * kPerThread +
                    static_cast<std::size_t>(index);
                allocated[slot] = ErrorCodeRegistry::Instance().Register(name);
                if (allocated[slot] == 0U) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_ERROR_CHECK_EQ(failures.load(), 0);

    std::unordered_set<std::uint32_t> unique(allocated.begin(), allocated.end());
    BENTO_ERROR_CHECK_EQ(unique.size(), allocated.size());
    BENTO_ERROR_CHECK_EQ(unique.count(0U), std::size_t{0});

    // 幂等：重复注册拿回同码，Size 不变。
    const std::size_t size_before = codes.Size();
    for (const std::string_view name : {"bento.protocol", "bento.frame", "bento.timeout"}) {
        BENTO_ERROR_CHECK_EQ(codes.Register(name), codes.Lookup(name));
    }
    BENTO_ERROR_CHECK_EQ(codes.Size(), size_before);

    // 每个名字都能 Lookup/Name 往返。
    for (std::size_t index = 0; index < allocated.size(); ++index) {
        const std::uint32_t code = allocated[index];
        const std::string name = "scenario.code." + std::to_string(index / kPerThread) + "." +
                                 std::to_string(index % kPerThread);
        BENTO_ERROR_CHECK_EQ(codes.Lookup(name), code);
        BENTO_ERROR_CHECK_EQ(codes.Name(code), std::string_view(name));
    }

    // 类型注册：并发 + 幂等 + Create 往返。
    const std::size_t type_size_before = types.Size();
    for (int index = 0; index < 200; ++index) {
        const std::string name = "scenario.type." + std::to_string(index);
        BENTO_ERROR_CHECK(types.Register(name, "bento.Error", 1,
                                         []() -> ErrorPtr { return NewError("factory"); }));
        BENTO_ERROR_CHECK(types.Register(name, "bento.Error", 1,
                                         []() -> ErrorPtr { return NewError("factory"); }));
        BENTO_ERROR_CHECK_EQ(types.Depth(name), std::uint8_t{1});
        BENTO_ERROR_CHECK_EQ(types.ParentName(name), std::string_view("bento.Error"));
        const ErrorPtr created = types.Create(name);
        BENTO_ERROR_CHECK(created != nullptr);
        if (created) {
            BENTO_ERROR_CHECK_EQ(created->Message(), std::string("factory"));
        }
    }
    BENTO_ERROR_CHECK(types.Size() >= type_size_before + 200U);

    // 同名不同元数据：保留首次注册（幂等不覆盖），文档化行为。
    BENTO_ERROR_CHECK(types.Register("scenario.type.conflict", "bento.Error", 1,
                                     []() -> ErrorPtr { return NewError("first"); }));
    BENTO_ERROR_CHECK(types.Register("scenario.type.conflict", "other.Parent", 3,
                                     []() -> ErrorPtr { return NewError("second"); }));
    BENTO_ERROR_CHECK_EQ(types.ParentName("scenario.type.conflict"),
                         std::string_view("bento.Error"));
    BENTO_ERROR_CHECK_EQ(types.Depth("scenario.type.conflict"), std::uint8_t{1});
    const ErrorPtr conflict = types.Create("scenario.type.conflict");
    BENTO_ERROR_CHECK(conflict != nullptr);
    if (conflict) {
        BENTO_ERROR_CHECK_EQ(conflict->Message(), std::string("first"));
    }
}

// ===========================================================================
// 场景 4：反射 caster 正反例
// ===========================================================================

void ScenarioCasterTable() {
    struct IntCase {
        const char* text;
        bool ok;
        int value;
    };
    const IntCase int_cases[] = {
        {"0", true, 0},          {"-0", true, 0},          {"+7", true, 7},
        {"2147483647", true, 2147483647}, {"-2147483648", true, -2147483648},
        {"2147483648", false, 0}, {"-2147483649", false, 0},
        {"", false, 0},           {"+", false, 0},          {"-", false, 0},
        {" 1", false, 0},         {"1 ", false, 0},         {"1.0", false, 0},
        {"1e3", false, 0},        {"0x10", false, 0},       {"--1", false, 0},
        {"0007", true, 7},
    };
    const go2cpp::bento::reflect::caster::StringToInt to_int;
    for (const IntCase& item : int_cases) {
        int value = 424242;
        const bool ok = to_int(item.text, value);
        BENTO_ERROR_CHECK_EQ(ok, item.ok);
        if (ok) {
            BENTO_ERROR_CHECK_EQ(value, item.value);
        } else {
            BENTO_ERROR_CHECK_EQ(value, 424242);  // 失败不得改写输出参数
        }
    }

    struct DoubleCase {
        const char* text;
        bool ok;
    };
    const DoubleCase double_cases[] = {
        {"0", true},      {"-0.0", true},   {".5", true},      {"+2.", true},
        {"1e3", true},    {"1E-3", true},   {"1e309", false},  {"-1e309", false},
        {"1e-400", false}, {"inf", false},  {"nan", false},    {"0x10", false},
        {"1,5", false},   {"1.2.3", false}, {"", false},       {"1 ", false},
    };
    const go2cpp::bento::reflect::caster::StringToDouble to_double;
    for (const DoubleCase& item : double_cases) {
        double value = 42.0;
        const bool ok = to_double(item.text, value);
        BENTO_ERROR_CHECK_EQ(ok, item.ok);
        if (ok) {
            BENTO_ERROR_CHECK(std::isfinite(value));
        }
    }

    const go2cpp::bento::reflect::caster::StringToBool to_bool;
    for (const char* text : {"true", "TRUE", "yes", "1", "false", "NO", "0"}) {
        bool value = false;
        BENTO_ERROR_CHECK(to_bool(text, value));
    }
    for (const char* text : {"", "2", " true", "y", "0x1", "on", "off"}) {
        bool value = false;
        BENTO_ERROR_CHECK(!to_bool(text, value));
    }

    // 随机往返：合法值解析后再序列化必须一致。
    Rng rng(0x1234abcdULL);
    int round_trip_failures = 0;
    for (int index = 0; index < 20000; ++index) {
        const int expected = static_cast<int>(static_cast<std::int64_t>(rng.Next() % 4000000001ULL) -
                                              2000000000LL);
        const std::string text = std::to_string(expected);
        int parsed = 0;
        if (!to_int(text, parsed) || parsed != expected) {
            ++round_trip_failures;
        }
    }
    BENTO_ERROR_CHECK_EQ(round_trip_failures, 0);

    // 随机垃圾输入：不得崩溃，成功时必须可复现。
    constexpr char kAlphabet[] = "0123456789+-.eE abcXYZ\t\n'\"\\,;:{}[]()_/|*&^%$#@!~`";
    int garbage_ok = 0;
    for (int index = 0; index < 5000; ++index) {
        const std::size_t length = static_cast<std::size_t>(rng.Below(16));
        std::string text;
        text.reserve(length);
        for (std::size_t offset = 0; offset < length; ++offset) {
            text.push_back(kAlphabet[rng.Below(sizeof(kAlphabet) - 1U)]);
        }
        int int_value = 0;
        double double_value = 0.0;
        bool bool_value = false;
        const bool int_ok = to_int(text, int_value);
        const bool double_ok = to_double(text, double_value);
        const bool bool_ok = to_bool(text, bool_value);
        if (int_ok) {
            int again = 0;
            BENTO_ERROR_CHECK_EQ(to_int(text, again), true);
            BENTO_ERROR_CHECK_EQ(again, int_value);
            ++garbage_ok;
        }
        if (double_ok) {
            BENTO_ERROR_CHECK(std::isfinite(double_value));
            ++garbage_ok;
        }
        if (bool_ok) {
            ++garbage_ok;
        }
    }
    std::printf("[scenario] caster: table ok, random round-trip=20000, garbage=5000 (accepted=%d)\n",
                garbage_ok);

    // TypeTraits：未特化类型必须退化为 unknown + void Caster（编译期约束）。
    const go2cpp::bento::reflect::TypeDescriptor unknown =
        go2cpp::bento::reflect::Describe<long double>();
    BENTO_ERROR_CHECK_EQ(std::string_view(unknown.name), std::string_view("unknown"));
    BENTO_ERROR_CHECK_EQ(unknown.is_numeric, false);
    BENTO_ERROR_CHECK_EQ(unknown.kind, go2cpp::bento::reflect::FieldKind::kString);
    BENTO_ERROR_CHECK_EQ(go2cpp::bento::reflect::SizeOf<volatile const int>(),
                         sizeof(int));
}

// ===========================================================================
// 场景 5：bind 配置绑定
// ===========================================================================

struct AppConfig {
    std::string host = "127.0.0.1";
    int port = 8080;
    double ratio = 0.5;
    bool secure = false;
    std::string note = "default";
};

int g_fields_calls = 0;

} // namespace

namespace go2cpp::bento::reflect {

template <>
struct FieldList<AppConfig> {
    static std::vector<FieldBinder<AppConfig>> Fields() {
        ++g_fields_calls;
        return {BENTO_BIND_FIELD(AppConfig, host), BENTO_BIND_FIELD(AppConfig, port),
                BENTO_BIND_FIELD(AppConfig, ratio), BENTO_BIND_FIELD(AppConfig, secure),
                BENTO_BIND_FIELD(AppConfig, note)};
    }
};

} // namespace go2cpp::bento::reflect

namespace {

void ScenarioBindConfig() {
    using go2cpp::bento::reflect::Bind;
    using go2cpp::bento::reflect::BindError;
    using go2cpp::bento::reflect::BindErrorKind;
    using go2cpp::bento::reflect::BindOptions;

    const std::map<std::string, std::string> full{{"host", "example.com"},
                                                   {"port", "9090"},
                                                   {"ratio", "0.25"},
                                                   {"secure", "true"},
                                                   {"note", "hello 世界"}};
    AppConfig config;
    BENTO_ERROR_CHECK(Bind(config, full));
    BENTO_ERROR_CHECK_EQ(config.host, std::string("example.com"));
    BENTO_ERROR_CHECK_EQ(config.port, 9090);
    BENTO_ERROR_CHECK_EQ(config.ratio, 0.25);
    BENTO_ERROR_CHECK(config.secure);
    BENTO_ERROR_CHECK_EQ(config.note, std::string("hello 世界"));

    // 部分配置保留默认值。
    const std::map<std::string, std::string> partial{{"port", "1234"}};
    AppConfig partial_config;
    BENTO_ERROR_CHECK(Bind(partial_config, partial));
    BENTO_ERROR_CHECK_EQ(partial_config.port, 1234);
    BENTO_ERROR_CHECK_EQ(partial_config.host, std::string("127.0.0.1"));
    BENTO_ERROR_CHECK_EQ(partial_config.ratio, 0.5);
    BENTO_ERROR_CHECK(!partial_config.secure);

    // 失败：强保证，对象不被部分修改。
    AppConfig kept;
    kept.host = "original";
    kept.port = 1;
    BindError error;
    const std::map<std::string, std::string> bad{{"host", "changed"}, {"port", "not-a-number"}};
    BENTO_ERROR_CHECK(!Bind(kept, bad, BindOptions{}, &error));
    BENTO_ERROR_CHECK_EQ(error.kind, BindErrorKind::kWrongType);
    BENTO_ERROR_CHECK_EQ(error.field, std::string("port"));
    BENTO_ERROR_CHECK_EQ(kept.host, std::string("original"));
    BENTO_ERROR_CHECK_EQ(kept.port, 1);

    // 未知字段：默认忽略，reject_unknown 失败。
    const std::map<std::string, std::string> unknown{{"mystery", "x"}, {"port", "80"}};
    AppConfig tolerant;
    BENTO_ERROR_CHECK(Bind(tolerant, unknown));
    BENTO_ERROR_CHECK_EQ(tolerant.port, 80);
    BindOptions strict;
    strict.reject_unknown = true;
    BindError strict_error;
    BENTO_ERROR_CHECK(!Bind(tolerant, unknown, strict, &strict_error));
    BENTO_ERROR_CHECK_EQ(strict_error.kind, BindErrorKind::kUnknownField);
    BENTO_ERROR_CHECK_EQ(strict_error.field, std::string("mystery"));
    BENTO_ERROR_CHECK_EQ(tolerant.port, 80);

    // require_all：缺字段失败；齐全成功；reject_unknown 优先于 require_all。
    BindOptions require_all;
    require_all.require_all = true;
    BindError missing_error;
    AppConfig missing_target;
    BENTO_ERROR_CHECK(!Bind(missing_target, partial, require_all, &missing_error));
    BENTO_ERROR_CHECK_EQ(missing_error.kind, BindErrorKind::kMissingField);
    BENTO_ERROR_CHECK(!missing_error.field.empty());
    AppConfig complete;
    BENTO_ERROR_CHECK(Bind(complete, full, require_all));
    BindOptions both = strict;
    both.require_all = true;
    BindError both_error;
    AppConfig both_target;
    BENTO_ERROR_CHECK(!Bind(both_target, unknown, both, &both_error));
    BENTO_ERROR_CHECK_EQ(both_error.kind, BindErrorKind::kUnknownField);

    // optional 形式。
    const std::optional<AppConfig> parsed = Bind<AppConfig>(partial);
    BENTO_ERROR_CHECK(parsed.has_value());
    if (parsed) {
        BENTO_ERROR_CHECK_EQ(parsed->port, 1234);
    }
    BENTO_ERROR_CHECK(!Bind<AppConfig>(bad));
    BENTO_ERROR_CHECK(!Bind<AppConfig>(std::map<std::string, std::string>{{"ratio", "inf"}}));

    // unordered_map 也满足 Map 概念。
    const std::unordered_map<std::string, std::string> unordered{{"host", "u.example"},
                                                                  {"secure", "1"}};
    AppConfig unordered_config;
    BENTO_ERROR_CHECK(Bind(unordered_config, unordered));
    BENTO_ERROR_CHECK_EQ(unordered_config.host, std::string("u.example"));
    BENTO_ERROR_CHECK(unordered_config.secure);

    // 大小写敏感；空 map 成功。
    AppConfig case_config;
    BENTO_ERROR_CHECK(Bind(case_config, std::map<std::string, std::string>{{"Port", "1"}}));
    BENTO_ERROR_CHECK_EQ(case_config.port, 8080);
    BENTO_ERROR_CHECK(Bind(case_config, std::map<std::string, std::string>{}));

    std::printf("[scenario] bind: 重复 Bind 不再重建字段表（Fields 累计调用=%d，含 bench 对照）\n",
                g_fields_calls);
    // 缓存生效后，重复 Bind 不再重建字段表。
    const int fields_before = g_fields_calls;
    for (int index = 0; index < 16; ++index) {
        AppConfig cache_config;
        BENTO_ERROR_CHECK(Bind(cache_config, partial));
    }
    BENTO_ERROR_CHECK_EQ(g_fields_calls, fields_before);
}

void RunBindBenchmark() {
    const std::map<std::string, std::string> values{{"host", "example.com"},
                                                     {"port", "9090"},
                                                     {"ratio", "0.25"},
                                                     {"secure", "true"},
                                                     {"note", "hello"}};
    AppConfig config;
    PrintBench(Measure("Bind(AppConfig 5 fields)", 200000, 512, [&config, &values]() {
        (void)go2cpp::bento::reflect::Bind(config, values);
    }));
    // 对照：未缓存路径每次 Bind 都要重建一次字段表（vector + 5 个 std::function）。
    PrintBench(Measure("FieldList::Fields() rebuild", 200000, 512, []() {
        const auto fields = go2cpp::bento::reflect::FieldList<AppConfig>::Fields();
        (void)fields;
    }));
}

// ===========================================================================
// 场景 6：JSON 环 / 转义 / 截断
// ===========================================================================

void ScenarioJsonEdges() {
    // 转义：引号、反斜杠、控制字符、中文透传。
    const ErrorPtr escaped = NewError("a\"b\\c\nd\te\x01中", ErrorCode::kInvalidArgument);
    const std::string json = DefaultJson(escaped);
    BENTO_ERROR_CHECK(json.find("a\\\"b\\\\c\\nd\\te\\u0001中") != std::string::npos);

    // 环：共享节点只在首次展开，第二次为 null。
    auto cycle = std::make_shared<CyclicError>();
    cycle->SetTarget(cycle);
    const std::string cycle_json = DefaultJson(cycle);
    BENTO_ERROR_CHECK(cycle_json.find("\"causes\":[null]") != std::string::npos);

    // 深度截断：64 层上限。
    ErrorPtr deep = NewError("leaf");
    for (int index = 0; index < 200; ++index) {
        deep = Wrap(deep, "w");
    }
    const std::string deep_json = DefaultJson(deep);
    BENTO_ERROR_CHECK(deep_json.find("\"truncated\":true") != std::string::npos);
    BENTO_ERROR_CHECK(deep_json.size() < 100000U);

    // 工厂返回失败/异常时 JSON 仍可序列化。
    const ErrorPtr factory_null = ErrorTypeRegistry::Instance().Create("scenario.missing.type");
    BENTO_ERROR_CHECK_EQ(factory_null, nullptr);
    BENTO_ERROR_CHECK_EQ(DefaultJson(factory_null), std::string("null"));
}

// ===========================================================================
// 线程局部深链析构（验证释放队列不爆栈）
// ===========================================================================

thread_local ErrorPtr g_thread_local_error;

void TestThreadLocalDeepChain() {
    std::thread worker([]() {
        ErrorPtr deep = NewError("leaf", ErrorCode::kDataLoss);
        for (int index = 0; index < 50000; ++index) {
            deep = Wrap(deep, "w");
        }
        g_thread_local_error = deep;
    });
    worker.join();
    std::printf("[thread] thread_local 50k chain destroyed with constant stack\n");
}

// ===========================================================================
// main
// ===========================================================================

void RunBenchmarkSuite() {
    std::printf("=== benchmarks (Release, 12 cores) ===\n");
    PrintAllocationStats();
    RunErrorBenchmarks();
    RunRegistryBenchmarks();
    RunReflectBenchmarks();
    RunMappingBenchmarks();
    RunDeepChainBenchmarks();
    RunWideJoinBenchmarks();
    RunConcurrentBenchmark();
    RunMemoryBenchmark();
    // Bind bench 放最后：它会产生大量小对象分配/释放，放在并发/内存测量之前
    // 会通过分配器碎片影响后续数据（实测并发吞吐下降约 60%）。
    RunBindBenchmark();
}

void RunRandomSuite() {
    std::printf("=== randomized graphs ===\n");
    TestRandomGraphs();
    TestRandomDeepChains();
    TestRandomWideJoins();
    TestRandomCycles();
}

void RunScenarioSuite() {
    std::printf("=== scenarios ===\n");
    ScenarioWrappedChain();
    ScenarioVisitorCollector();
    ScenarioRegistryIdempotency();
    ScenarioCasterTable();
    ScenarioBindConfig();
    ScenarioJsonEdges();
    TestThreadLocalDeepChain();
}

} // namespace

int main() {
    RunBenchmarkSuite();
    RunRandomSuite();
    RunScenarioSuite();
    std::printf("[checks] total=%d failures=%d\n", bento_error_test::g_checks,
                bento_error_test::g_failures);
    return bento_error_test::Finish("audit_error_reflect");
}
