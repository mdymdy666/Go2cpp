/**
 * @file test_error_enhanced.cpp
 * @brief go2cpp::error 验收测试：自研错误码、类型链、错误图、注册表、互操作、并发。
 *
 * @details 覆盖范围（22 个用例，含随机/压测/6 个真实场景）：
 *  1. 23 个内置码的数值、名字、分类与谓词；
 *  2. 旧 csvp/gRPC 16 码 ↔ 新码映射（含无损判定与越界值）；
 *  3. 内置类型链元数据（TypeName/ParentTypeName/InheritanceDepth）；
 *  4. Is / IsA 的类型链语义（含反向不成立）；
 *  5. UnwrapAll(1) 与 UnwrapAll(0) 的层级语义；
 *  6. Wrap/Join 文本展平与空指针处理；
 *  7. As<T> / Has<T> / HasErrorCode 图查询；
 *  8. WalkErrors 前序、kStop/kSkipChildren/异常路径、自指环；
 *  9. ErrorTreeString 树形文本；
 * 10. Registry 注册校验、幂等、错误诊断；
 * 11. Registry 按名字创建、工厂异常、未注册路径；
 * 12. Registry 父链查询（Ancestors/IsA/Depth）；
 * 13. 动态错误码注册、按码查类型、绑定冲突；
 * 14. 并发注册表访问（8 线程 × 500 次创建/查询）；
 * 15. 并发共享错误图的只读查询；
 * 16. 旧体系 ToLegacy / FromLegacy 往返；
 * 17. Bento 桥双向转换与新映射表；
 * 18. 十万层深链的构造/消息/析构（不爆栈）；
 * 19. 随机错误树 + 注册表压测；
 * 20. 类型错误 / 超时专项用例；
 * 21. 每个内置码至少一个类型或场景；
 * 22. 6 个真实场景。
 *
 * 运行：见 tests/error/CMakeLists.txt 或 `tests/error/run.sh`。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/bento/error/bridge.hpp"
#include "go2cpp/bento/error/error.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/error/legacy_code.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// 测试诊断用：两套错误码都允许直接流入 CHECK_EQ 的失败信息。
std::ostream& operator<<(std::ostream& out, go2cpp::error::ErrorCode code) {
    return out << static_cast<std::int32_t>(code);
}

std::ostream& operator<<(std::ostream& out, go2cpp::error::ErrorCategory category) {
    return out << static_cast<int>(category);
}

std::ostream& operator<<(std::ostream& out, go2cpp::error::LegacyErrorCode code) {
    return out << static_cast<std::int32_t>(code);
}

std::ostream& operator<<(std::ostream& out, go2cpp::error::ErrorVisitResult result) {
    return out << static_cast<int>(result);
}

std::ostream& operator<<(std::ostream& out, go2cpp::bento::error::ErrorCode code) {
    return out << static_cast<std::int32_t>(code);
}

namespace {

std::atomic<int> g_failures{0};
std::atomic<int> g_checks{0};
std::mutex g_output_mutex;

void ReportFailure(const char* expression, const char* file, int line, const std::string& detail) {
    g_failures.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cerr << file << ':' << line << ": CHECK failed: " << expression;
    if (!detail.empty()) {
        std::cerr << "  [" << detail << ']';
    }
    std::cerr << '\n';
}

} // namespace

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        g_checks.fetch_add(1);                                                                     \
        if (!(condition)) {                                                                        \
            ReportFailure(#condition, __FILE__, __LINE__, std::string());                          \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                                                 \
    do {                                                                                           \
        g_checks.fetch_add(1);                                                                     \
        const auto check_actual = (actual);                                                        \
        const auto check_expected = (expected);                                                    \
        if (!(check_actual == check_expected)) {                                                   \
            std::ostringstream check_detail;                                                       \
            check_detail << "actual=" << check_actual << " expected=" << check_expected;           \
            ReportFailure(#actual " == " #expected, __FILE__, __LINE__, check_detail.str());       \
        }                                                                                          \
    } while (false)

namespace {

using namespace go2cpp::error; // NOLINT(google-build-using-namespace)：测试文件集中使用

/// @brief 测试用自定义错误：深度 1，统一走宏生成类型链。
class DemoError : public Error {
 public:
    GO2CPP_ERROR_TYPE("demo.DemoError", Error)

    DemoError() = default;
    explicit DemoError(std::string message) : m_message(std::move(message)) {}

    std::string Message() const override { return m_message; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }

 private:
    std::string m_message;
};

/// @brief 测试用二级派生错误：验证自定义类型的多级祖先链。
class DemoChildError final : public DemoError {
 public:
    GO2CPP_ERROR_TYPE("demo.DemoChildError", DemoError)

    DemoChildError() = default;
    explicit DemoChildError(std::string message) : DemoError(std::move(message)) {}
};

/// @brief 自指错误：`UnwrapAll()` 返回自身，用于验证环安全。
class LoopError final : public Error {
 public:
    GO2CPP_ERROR_TYPE("demo.LoopError", Error)

    LoopError() = default;

    std::string Message() const override { return "loop"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
    bool IsWrapping() const noexcept override { return true; }

    void SetSelf(ErrorPtr self) { m_self = std::move(self); }

    std::vector<ErrorPtr> UnwrapAll(std::size_t /*level*/) const override {
        if (!m_self) {
            return {};
        }
        return {m_self};
    }

 private:
    ErrorPtr m_self;
};

/// @brief 遍历抛异常的错误：验证查询接口的 noexcept 降级。
class ThrowingError final : public Error {
 public:
    GO2CPP_ERROR_TYPE("demo.ThrowingError", Error)

    ThrowingError() = default;

    std::string Message() const override { throw std::runtime_error("boom"); }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
    bool IsWrapping() const noexcept override { return true; }
    std::vector<ErrorPtr> UnwrapAll(std::size_t /*level*/) const override {
        throw std::runtime_error("children boom");
    }
};

/// @brief 运行一个具名用例并打印耗时。
void RunTest(const char* name, void (*function)()) {
    const auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_output_mutex);
        std::cout << "[ RUN      ] " << name << '\n';
    }
    function();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    {
        std::lock_guard<std::mutex> lock(g_output_mutex);
        std::cout << "[       OK ] " << name << " (" << elapsed << " ms)\n";
    }
}

// ---------------------------------------------------------------------------
// 1. 错误码词表
// ---------------------------------------------------------------------------
void TestErrorCodes() {
    CHECK_EQ(static_cast<int>(ErrorCode::kOk), 0);
    CHECK_EQ(static_cast<int>(ErrorCode::kUnknown), 1);
    CHECK_EQ(static_cast<int>(ErrorCode::kTypeMismatch), 2);
    CHECK_EQ(static_cast<int>(ErrorCode::kInvalidArgument), 3);
    CHECK_EQ(static_cast<int>(ErrorCode::kOutOfRange), 4);
    CHECK_EQ(static_cast<int>(ErrorCode::kFormatError), 5);
    CHECK_EQ(static_cast<int>(ErrorCode::kNotFound), 6);
    CHECK_EQ(static_cast<int>(ErrorCode::kAlreadyExists), 7);
    CHECK_EQ(static_cast<int>(ErrorCode::kPermissionDenied), 8);
    CHECK_EQ(static_cast<int>(ErrorCode::kUnauthenticated), 9);
    CHECK_EQ(static_cast<int>(ErrorCode::kTimeout), 10);
    CHECK_EQ(static_cast<int>(ErrorCode::kCancelled), 11);
    CHECK_EQ(static_cast<int>(ErrorCode::kResourceExhausted), 12);
    CHECK_EQ(static_cast<int>(ErrorCode::kUnavailable), 13);
    CHECK_EQ(static_cast<int>(ErrorCode::kFailedPrecondition), 14);
    CHECK_EQ(static_cast<int>(ErrorCode::kAborted), 15);
    CHECK_EQ(static_cast<int>(ErrorCode::kIOError), 16);
    CHECK_EQ(static_cast<int>(ErrorCode::kCorruption), 17);
    CHECK_EQ(static_cast<int>(ErrorCode::kInternal), 18);
    CHECK_EQ(static_cast<int>(ErrorCode::kUnimplemented), 19);
    CHECK_EQ(static_cast<int>(ErrorCode::kWouldBlock), 20);
    CHECK_EQ(static_cast<int>(ErrorCode::kDeadlock), 21);
    CHECK_EQ(static_cast<int>(ErrorCode::kWrappedError), 22);
    CHECK_EQ(kLastBuiltinCode, 22);
    CHECK_EQ(kFirstCustomCode, 1000);

    CHECK_EQ(ErrorCodeName(ErrorCode::kOk), std::string_view("OK"));
    CHECK_EQ(ErrorCodeName(ErrorCode::kTypeMismatch), std::string_view("TYPE_MISMATCH"));
    CHECK_EQ(ErrorCodeName(ErrorCode::kTimeout), std::string_view("TIMEOUT"));
    CHECK_EQ(ErrorCodeName(ErrorCode::kIOError), std::string_view("IO_ERROR"));
    CHECK_EQ(ErrorCodeName(ErrorCode::kWrappedError), std::string_view("WRAPPED"));
    CHECK_EQ(ErrorCodeName(static_cast<ErrorCode>(23)), std::string_view("UNKNOWN"));
    CHECK_EQ(ErrorCodeName(static_cast<ErrorCode>(1000)), std::string_view("UNKNOWN"));

    // 23 个名字两两不同，避免日志/监控标签撞名。
    std::set<std::string_view> names;
    for (int value = 0; value <= kLastBuiltinCode; ++value) {
        const std::string_view name = ErrorCodeName(static_cast<ErrorCode>(value));
        CHECK(!name.empty());
        names.insert(name);
    }
    CHECK_EQ(names.size(), std::size_t{23});

    // 分类：每类至少一个码，且只有 kUnknown 落进 kUnknown。
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kOk), ErrorCategory::kOk);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kUnknown), ErrorCategory::kUnknown);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kTypeMismatch), ErrorCategory::kUsage);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kFormatError), ErrorCategory::kUsage);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kNotFound), ErrorCategory::kLookup);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kUnauthenticated), ErrorCategory::kAccess);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kTimeout), ErrorCategory::kTransient);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kCancelled), ErrorCategory::kTransient);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kResourceExhausted), ErrorCategory::kTransient);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kWouldBlock), ErrorCategory::kTransient);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kAborted), ErrorCategory::kState);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kDeadlock), ErrorCategory::kState);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kIOError), ErrorCategory::kData);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kCorruption), ErrorCategory::kData);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kUnimplemented), ErrorCategory::kInternal);
    CHECK_EQ(ErrorCategoryOf(ErrorCode::kWrappedError), ErrorCategory::kWrapped);
    CHECK_EQ(ErrorCategoryOf(static_cast<ErrorCode>(1000)), ErrorCategory::kUnknown);

    // 分类名稳定且非空。
    std::set<std::string_view> category_names;
    for (int value = 0; value <= 9; ++value) {
        const std::string_view name = ErrorCategoryName(static_cast<ErrorCategory>(value));
        CHECK(!name.empty());
        category_names.insert(name);
    }
    CHECK_EQ(category_names.size(), std::size_t{10});

    CHECK(IsOk(ErrorCode::kOk));
    CHECK(!IsOk(ErrorCode::kUnknown));
    CHECK(IsWrappedCode(ErrorCode::kWrappedError));
    CHECK(IsBuiltinCode(ErrorCode::kInternal));
    CHECK(IsBuiltinCode(ErrorCode::kWrappedError));
    CHECK(!IsBuiltinCode(static_cast<ErrorCode>(23)));
    CHECK(!IsBuiltinCode(static_cast<ErrorCode>(1000)));
    CHECK(IsCustomCode(static_cast<ErrorCode>(1000)));
    CHECK(!IsCustomCode(ErrorCode::kInternal));
}

// ---------------------------------------------------------------------------
// 2. 旧码映射
// ---------------------------------------------------------------------------
void TestLegacyCodeMapping() {
    CHECK_EQ(kLegacyCodeCount, 16);
    for (int value = 0; value < kLegacyCodeCount; ++value) {
        const auto legacy = static_cast<LegacyErrorCode>(value);
        const ErrorCode current = FromLegacyCode(legacy);
        CHECK_EQ(FromLegacyCode(value), current);
        CHECK_EQ(ToLegacyCode(current), legacy);
        CHECK(IsLegacyLossless(current));
        CHECK(!LegacyErrorCodeName(legacy).empty());
        CHECK(!ErrorCodeName(current).empty());
        CHECK_EQ(kLegacyCodeMap[static_cast<std::size_t>(value)].legacy, legacy);
    }

    // 改名项：DATA_LOSS → CORRUPTION、DEADLINE_EXCEEDED → TIMEOUT。
    CHECK_EQ(FromLegacyCode(LegacyErrorCode::kDataLoss), ErrorCode::kCorruption);
    CHECK_EQ(FromLegacyCode(LegacyErrorCode::kDeadlineExceeded), ErrorCode::kTimeout);

    // 新词表独有码：反向有损。
    CHECK_EQ(ToLegacyCode(ErrorCode::kTypeMismatch), LegacyErrorCode::kInvalidArgument);
    CHECK_EQ(ToLegacyCode(ErrorCode::kFormatError), LegacyErrorCode::kInvalidArgument);
    CHECK_EQ(ToLegacyCode(ErrorCode::kUnauthenticated), LegacyErrorCode::kPermissionDenied);
    CHECK_EQ(ToLegacyCode(ErrorCode::kIOError), LegacyErrorCode::kUnavailable);
    CHECK_EQ(ToLegacyCode(ErrorCode::kAborted), LegacyErrorCode::kFailedPrecondition);
    CHECK_EQ(ToLegacyCode(ErrorCode::kWouldBlock), LegacyErrorCode::kUnavailable);
    CHECK_EQ(ToLegacyCode(ErrorCode::kDeadlock), LegacyErrorCode::kFailedPrecondition);
    CHECK(!IsLegacyLossless(ErrorCode::kTypeMismatch));
    CHECK(!IsLegacyLossless(ErrorCode::kFormatError));
    CHECK(!IsLegacyLossless(ErrorCode::kUnauthenticated));
    CHECK(!IsLegacyLossless(ErrorCode::kAborted));
    CHECK(!IsLegacyLossless(ErrorCode::kIOError));
    CHECK(!IsLegacyLossless(ErrorCode::kWouldBlock));
    CHECK(!IsLegacyLossless(ErrorCode::kDeadlock));

    // 越界/非法旧码统一退化为 kUnknown，不抛异常。
    CHECK_EQ(FromLegacyCode(16), ErrorCode::kUnknown);
    CHECK_EQ(FromLegacyCode(-1), ErrorCode::kUnknown);
    CHECK_EQ(FromLegacyCode(9999), ErrorCode::kUnknown);
    CHECK_EQ(LegacyErrorCodeName(static_cast<LegacyErrorCode>(99)), std::string_view("UNKNOWN"));
}

// ---------------------------------------------------------------------------
// 3. 内置类型链元数据
// ---------------------------------------------------------------------------
void TestTypeChain() {
    const auto string_error = MakeError<StringError>("s");
    CHECK_EQ(string_error->TypeName(), std::string_view("go2cpp.StringError"));
    CHECK_EQ(string_error->ParentTypeName(), std::string_view("go2cpp.Error"));
    CHECK_EQ(string_error->InheritanceDepth(), std::uint8_t{1});
    CHECK_EQ(string_error->Code(), ErrorCode::kUnknown);

    const auto argument_error = MakeError<ArgumentError>("bad", "count");
    CHECK_EQ(argument_error->InheritanceDepth(), std::uint8_t{1});
    CHECK_EQ(argument_error->Code(), ErrorCode::kInvalidArgument);

    const auto type_error = MakeError<TypeMismatchError>("bad type", "int", "string");
    CHECK_EQ(type_error->Code(), ErrorCode::kTypeMismatch);
    CHECK_EQ(type_error->Expected(), std::string("int"));
    CHECK_EQ(type_error->Actual(), std::string("string"));
    CHECK_EQ(type_error->Message(), std::string("bad type [expected=int] [actual=string]"));

    const auto format_error = MakeError<FormatError>("bad json", "json");
    CHECK_EQ(format_error->Code(), ErrorCode::kFormatError);
    CHECK_EQ(format_error->Format(), std::string("json"));

    const auto range_error = MakeError<OutOfRangeError>("index", 5, 3);
    CHECK_EQ(range_error->Code(), ErrorCode::kOutOfRange);
    CHECK_EQ(range_error->Index(), std::int64_t{5});
    CHECK_EQ(range_error->Limit(), std::int64_t{3});

    const auto io_error = MakeError<IOError>("io");
    CHECK_EQ(io_error->TypeName(), std::string_view("go2cpp.IOError"));
    CHECK_EQ(io_error->InheritanceDepth(), std::uint8_t{1});
    CHECK_EQ(io_error->Code(), ErrorCode::kIOError);

    const auto timeout = MakeError<TimeoutError>("t", "p", 3000);
    CHECK_EQ(timeout->TypeName(), std::string_view("go2cpp.TimeoutError"));
    CHECK_EQ(timeout->ParentTypeName(), std::string_view("go2cpp.IOError"));
    CHECK_EQ(timeout->InheritanceDepth(), std::uint8_t{2});
    CHECK_EQ(timeout->Code(), ErrorCode::kTimeout);
    CHECK_EQ(timeout->TimeoutMs(), std::uint64_t{3000});
    CHECK_EQ(timeout->Path(), std::string("p"));

    const auto not_found = MakeError<NotFoundError>("nf", "k");
    CHECK_EQ(not_found->InheritanceDepth(), std::uint8_t{2});
    CHECK_EQ(not_found->Code(), ErrorCode::kNotFound);

    const auto file_not_found = MakeError<FileNotFoundError>("missing", "/tmp/x");
    CHECK_EQ(file_not_found->InheritanceDepth(), std::uint8_t{3});
    CHECK_EQ(file_not_found->Code(), ErrorCode::kNotFound);

    const auto permission = MakeError<PermissionError>("denied", "/root");
    CHECK_EQ(permission->InheritanceDepth(), std::uint8_t{2});
    CHECK_EQ(permission->Code(), ErrorCode::kPermissionDenied);

    const auto unauthenticated = MakeError<UnauthenticatedError>("no token", "alice");
    CHECK_EQ(unauthenticated->Code(), ErrorCode::kUnauthenticated);
    CHECK_EQ(unauthenticated->Principal(), std::string("alice"));

    const auto cancelled = MakeError<CancelledError>("stop", "ctx");
    CHECK_EQ(cancelled->Code(), ErrorCode::kCancelled);
    CHECK_EQ(cancelled->Reason(), std::string("ctx"));

    const auto exhausted = MakeError<ResourceExhaustedError>("too many", "fd");
    CHECK_EQ(exhausted->Code(), ErrorCode::kResourceExhausted);
    CHECK_EQ(exhausted->Resource(), std::string("fd"));

    const auto corrupted = MakeError<CorruptionError>("crc", "frame");
    CHECK_EQ(corrupted->Code(), ErrorCode::kCorruption);
    CHECK_EQ(corrupted->Source(), std::string("frame"));

    const auto wrapped = MakeError<WrappedError>("ctx", MakeError<StringError>("cause"));
    CHECK_EQ(wrapped->TypeName(), std::string_view("go2cpp.WrappedError"));
    CHECK_EQ(wrapped->Code(), ErrorCode::kWrappedError);
    CHECK(wrapped->IsWrapping());

    const auto joined = MakeError<JoinError>(std::vector<ErrorPtr>{MakeError<StringError>("a")});
    CHECK_EQ(joined->TypeName(), std::string_view("go2cpp.JoinError"));
    CHECK_EQ(joined->Code(), ErrorCode::kWrappedError);
    CHECK(joined->IsWrapping());

    for (const auto& error : {ErrorPtr(string_error), ErrorPtr(argument_error),
                              ErrorPtr(type_error), ErrorPtr(io_error), ErrorPtr(timeout),
                              ErrorPtr(file_not_found), ErrorPtr(permission),
                              ErrorPtr(unauthenticated), ErrorPtr(cancelled),
                              ErrorPtr(exhausted), ErrorPtr(corrupted), ErrorPtr(wrapped),
                              ErrorPtr(joined)}) {
        CHECK(error->IsA(kErrorRootTypeName));
        CHECK(error->IsA(error->TypeName()));
        CHECK(error->InheritanceDepth() <= kMaxErrorDepth);
    }

    const auto demo_child = MakeError<DemoChildError>("d");
    CHECK_EQ(demo_child->TypeName(), std::string_view("demo.DemoChildError"));
    CHECK_EQ(demo_child->ParentTypeName(), std::string_view("demo.DemoError"));
    CHECK_EQ(demo_child->InheritanceDepth(), std::uint8_t{2});
    CHECK(demo_child->IsA("demo.DemoError"));
    CHECK(demo_child->IsA("go2cpp.Error"));
}

// ---------------------------------------------------------------------------
// 4. Is / IsA 语义
// ---------------------------------------------------------------------------
void TestIsSemantics() {
    const auto timeout = MakeError<TimeoutError>("t");
    const auto io_error = MakeError<IOError>("io");
    const auto file_not_found = MakeError<FileNotFoundError>("f");

    CHECK(timeout->IsA("go2cpp.IOError"));
    CHECK(timeout->IsA("go2cpp.TimeoutError"));
    CHECK(timeout->IsA("go2cpp.Error"));
    CHECK(!io_error->IsA("go2cpp.TimeoutError"));
    CHECK(!io_error->IsA("go2cpp.NotFoundError"));
    CHECK(file_not_found->IsA("go2cpp.NotFoundError"));
    CHECK(file_not_found->IsA("go2cpp.IOError"));
    CHECK(!file_not_found->IsA("go2cpp.TimeoutError"));
    CHECK(!timeout->IsA("demo.DemoError"));

    CHECK(timeout->Is(*io_error));
    CHECK(!io_error->Is(*timeout));
    CHECK(file_not_found->Is(*MakeError<NotFoundError>()));
    CHECK(!MakeError<StringError>()->Is(*io_error));

    CHECK(IsA(timeout, "go2cpp.IOError"));
    CHECK(!IsA(ErrorPtr(), "go2cpp.IOError"));
    CHECK(IsErrorRoot(timeout));
    CHECK(!IsErrorRoot(ErrorPtr()));
    CHECK(!Is(ErrorPtr(), *io_error));

    CHECK_EQ(ErrorMessage(timeout), std::string("t"));
    CHECK_EQ(ErrorMessage(ErrorPtr()), std::string());
    CHECK_EQ(ErrorCodeOf(ErrorPtr()), ErrorCode::kOk);
    CHECK_EQ(ErrorCodeOf(io_error), ErrorCode::kIOError);
}

// ---------------------------------------------------------------------------
// 5. UnwrapAll 层级语义
// ---------------------------------------------------------------------------
void TestUnwrapLevels() {
    const auto leaf = MakeError<StringError>("leaf");
    CHECK(leaf->UnwrapAll().empty());
    CHECK(leaf->UnwrapAll(0).empty());

    const auto middle = Wrap(leaf, "middle");
    const auto outer = Wrap(middle, "outer");
    CHECK_EQ(outer->UnwrapAll().size(), std::size_t{1});
    CHECK(outer->UnwrapAll().front() == middle);
    CHECK_EQ(outer->UnwrapAll(0).size(), std::size_t{2});
    CHECK(outer->UnwrapAll(0)[0] == middle);
    CHECK(outer->UnwrapAll(0)[1] == leaf);

    const auto joined = Join({outer, MakeError<ArgumentError>("bad")});
    CHECK_EQ(joined->UnwrapAll().size(), std::size_t{2});
    CHECK_EQ(joined->UnwrapAll(0).size(), std::size_t{4});
    CHECK(joined->UnwrapAll(0)[0] == outer);
    CHECK(joined->UnwrapAll(0)[1] == middle);
    CHECK(joined->UnwrapAll(0)[2] == leaf);

    CHECK_EQ(UnwrapAll(ErrorPtr()).size(), std::size_t{0});
    CHECK_EQ(UnwrapAll(joined, 0).size(), std::size_t{4});
    CHECK_EQ(UnwrapAll(joined, 1).size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// 6. Wrap / Join 文本
// ---------------------------------------------------------------------------
void TestWrapMessages() {
    const auto leaf = MakeError<StringError>("leaf");
    CHECK_EQ(ErrorMessage(Wrap(leaf, "ctx")), std::string("ctx: leaf"));
    CHECK_EQ(ErrorMessage(Wrap(Wrap(leaf, "inner"), "outer")), std::string("outer: inner: leaf"));
    CHECK_EQ(ErrorMessage(Wrap(leaf, "")), std::string("leaf"));
    CHECK(Wrap(ErrorPtr(), "ctx") == ErrorPtr());
    CHECK(!IsWrappedError(leaf));
    CHECK(IsWrappedError(Wrap(leaf, "ctx")));

    const auto wrapped = Wrap(leaf, "ctx");
    CHECK(wrapped->Unwrap() == leaf);

    const auto joined = Join({MakeError<StringError>("a"), MakeError<StringError>("b")});
    CHECK_EQ(ErrorMessage(joined), std::string("a\nb"));
    CHECK(Join(std::vector<ErrorPtr>{}) == ErrorPtr());
    CHECK(Join({ErrorPtr(), ErrorPtr()}) == ErrorPtr());
    CHECK_EQ(Join({ErrorPtr(), MakeError<StringError>("x")})->UnwrapAll().size(), std::size_t{1});

    const auto mixed = Join({MakeError<StringError>(""), MakeError<StringError>("x")});
    CHECK_EQ(ErrorMessage(mixed), std::string("x"));

    CHECK_EQ(ErrorMessage(MakeError<FileNotFoundError>("missing", "/tmp/a")),
             std::string("missing [path=/tmp/a]"));
    CHECK_EQ(ErrorMessage(MakeError<ArgumentError>("bad", "count")),
             std::string("bad [argument=count]"));
    CHECK_EQ(ErrorMessage(MakeError<ArgumentError>("bad")), std::string("bad"));
    CHECK_EQ(ErrorMessage(MakeError<FormatError>("bad json")), std::string("bad json"));
}

// ---------------------------------------------------------------------------
// 7. As / Has / HasErrorCode
// ---------------------------------------------------------------------------
void TestQueryTypes() {
    const auto file_not_found = MakeError<FileNotFoundError>("f", "/tmp");
    const auto wrapped = Wrap(file_not_found, "load");

    CHECK(As<FileNotFoundError>(wrapped) == file_not_found);
    CHECK(As<NotFoundError>(wrapped) != nullptr);
    CHECK(As<IOError>(wrapped) != nullptr);
    CHECK(As<Error>(wrapped) != nullptr);
    CHECK(As<ArgumentError>(wrapped) == nullptr);
    CHECK(Has<IOError>(wrapped));
    CHECK(!Has<ArgumentError>(wrapped));
    CHECK(As<IOError>(ErrorPtr()) == nullptr);
    CHECK(!Has<IOError>(ErrorPtr()));

    const auto timeout = MakeError<TimeoutError>("t");
    CHECK(As<IOError>(timeout) != nullptr);
    CHECK(As<TimeoutError>(timeout) != nullptr);

    const auto joined = Join({MakeError<StringError>("a"), wrapped});
    CHECK(Has<FileNotFoundError>(joined));
    CHECK(As<FileNotFoundError>(joined) == file_not_found);

    CHECK(HasErrorCode(wrapped, ErrorCode::kWrappedError));
    CHECK(HasErrorCode(wrapped, ErrorCode::kNotFound));
    CHECK(!HasErrorCode(wrapped, ErrorCode::kCancelled));
    CHECK(!HasErrorCode(ErrorPtr(), ErrorCode::kNotFound));
}

// ---------------------------------------------------------------------------
// 8. WalkErrors
// ---------------------------------------------------------------------------
void TestWalk() {
    const auto leaf = MakeError<StringError>("leaf");
    const auto wrapped = Wrap(leaf, "ctx");
    const auto joined = Join({wrapped, MakeError<ArgumentError>("bad")});

    const WalkResult all = WalkErrors(joined, [](const ErrorPtr&) {
        return ErrorVisitResult::kContinue;
    });
    CHECK(all.ok);
    CHECK_EQ(all.status, ErrorVisitResult::kContinue);
    CHECK_EQ(all.errors.size(), std::size_t{4});
    CHECK(all.errors[0] == joined);

    int visited = 0;
    const WalkResult stopped = WalkErrors(joined, [&visited](const ErrorPtr&) {
        ++visited;
        return ErrorVisitResult::kStop;
    });
    CHECK(stopped.ok);
    CHECK_EQ(stopped.status, ErrorVisitResult::kStop);
    CHECK_EQ(visited, 1);
    CHECK_EQ(stopped.errors.size(), std::size_t{1});

    int skipped_visits = 0;
    const WalkResult skipped = WalkErrors(joined, [&skipped_visits](const ErrorPtr&) {
        ++skipped_visits;
        return ErrorVisitResult::kSkipChildren;
    });
    CHECK(skipped.ok);
    CHECK_EQ(skipped_visits, 1);

    const WalkResult no_visitor = WalkErrors(joined, ErrorVisitor());
    CHECK(!no_visitor.ok);
    const WalkResult null_result = WalkErrors(ErrorPtr(), [](const ErrorPtr&) {
        return ErrorVisitResult::kContinue;
    });
    CHECK(null_result.ok);
    CHECK(null_result.errors.empty());

    const WalkResult thrown = WalkErrors(joined, [](const ErrorPtr&) -> ErrorVisitResult {
        throw std::runtime_error("visitor boom");
    });
    CHECK(!thrown.ok);

    auto loop = std::make_shared<LoopError>();
    loop->SetSelf(loop);
    const WalkResult loop_result = WalkErrors(loop, [](const ErrorPtr&) {
        return ErrorVisitResult::kContinue;
    });
    CHECK(loop_result.ok);
    CHECK_EQ(loop_result.errors.size(), std::size_t{1});
    CHECK_EQ(loop->UnwrapAll(0).size(), std::size_t{1});
    loop->SetSelf(ErrorPtr());
}

// ---------------------------------------------------------------------------
// 9. 树形文本
// ---------------------------------------------------------------------------
void TestTreeString() {
    const auto tree = Join({Wrap(MakeError<FileNotFoundError>("missing", "/tmp/a"), "load config"),
                            MakeError<ArgumentError>("bad", "count")});
    const std::string text = ErrorTreeString(tree);
    CHECK(text.find("go2cpp.JoinError") != std::string::npos);
    CHECK(text.find("go2cpp.WrappedError") != std::string::npos);
    CHECK(text.find("go2cpp.FileNotFoundError") != std::string::npos);
    CHECK(text.find("go2cpp.ArgumentError") != std::string::npos);
    CHECK(text.find("\n  - ") != std::string::npos);
    CHECK_EQ(ErrorTreeString(ErrorPtr()), std::string("<null>"));
}

// ---------------------------------------------------------------------------
// 10. Registry 注册校验
// ---------------------------------------------------------------------------
void TestRegistryRegistration() {
    Registry& registry = Registry::Instance();
    CHECK(&Registry::Instance() == &registry);
    registry.ClearLastError();

    CHECK(registry.Has("go2cpp.Error"));
    CHECK(registry.Has("go2cpp.StringError"));
    CHECK(registry.Has("go2cpp.TypeMismatchError"));
    CHECK(registry.Has("go2cpp.FormatError"));
    CHECK(registry.Has("go2cpp.OutOfRangeError"));
    CHECK(registry.Has("go2cpp.CancelledError"));
    CHECK(registry.Has("go2cpp.ResourceExhaustedError"));
    CHECK(registry.Has("go2cpp.CorruptionError"));
    CHECK(registry.Has("go2cpp.UnauthenticatedError"));
    CHECK(registry.Has("go2cpp.JoinError"));
    CHECK(registry.Size() >= std::size_t{16});
    CHECK_EQ(registry.Depth("go2cpp.Error"), std::uint8_t{0});
    CHECK_EQ(registry.Depth("go2cpp.IOError"), std::uint8_t{1});
    CHECK_EQ(registry.Depth("go2cpp.TimeoutError"), std::uint8_t{2});
    CHECK_EQ(registry.Depth("go2cpp.FileNotFoundError"), std::uint8_t{3});
    CHECK_EQ(registry.Depth("demo.Unknown"), std::uint8_t{0});
    CHECK_EQ(registry.ParentName("go2cpp.TimeoutError"), std::string_view("go2cpp.IOError"));
    CHECK_EQ(registry.ParentName("go2cpp.Error"), std::string_view());
    CHECK_EQ(registry.ParentName("demo.Unknown"), std::string_view());

    const std::size_t before = registry.Size();
    CHECK(registry.Register<StringError>());
    CHECK_EQ(registry.Size(), before);
    CHECK(registry.Register("go2cpp.IOError", "go2cpp.Error", 1,
                            []() -> ErrorPtr { return MakeError<IOError>(); }));

    registry.ClearLastError();
    CHECK(!registry.Register("go2cpp.IOError", "go2cpp.Error", 2,
                             []() -> ErrorPtr { return MakeError<IOError>(); }));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kAlreadyExists));
    CHECK(!registry.LastErrorDetail().empty());

    registry.ClearLastError();
    CHECK(!registry.Register("", "go2cpp.Error", 1, []() -> ErrorPtr { return {}; }));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kInvalidArgument));

    registry.ClearLastError();
    CHECK(!registry.Register("demo.TooDeep", "go2cpp.Error", 100, []() -> ErrorPtr { return {}; }));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kOutOfRange));

    registry.ClearLastError();
    CHECK(!registry.Register("demo.BadRoot", "go2cpp.Error", 0, []() -> ErrorPtr { return {}; }));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kInvalidArgument));

    registry.ClearLastError();
    CHECK(!registry.Register("demo.NoParent", "", 1, []() -> ErrorPtr { return {}; }));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kInvalidArgument));

    registry.ClearLastError();
    CHECK(!registry.Register("demo.NoFactory", "go2cpp.Error", 1, ErrorFactory()));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kInvalidArgument));

    registry.ClearLastError();
    CHECK(!registry.Register("demo.OrphanParent", "demo.NotRegistered", 1,
                             []() -> ErrorPtr { return MakeError<DemoError>(); }));
    CHECK_EQ(registry.LastErrorCode(),
             static_cast<std::uint32_t>(ErrorCode::kFailedPrecondition));

    registry.ClearLastError();
    CHECK(!registry.Register("demo.DepthJump", "go2cpp.Error", 2,
                             []() -> ErrorPtr { return MakeError<DemoError>(); }));
    CHECK_EQ(registry.LastErrorCode(),
             static_cast<std::uint32_t>(ErrorCode::kFailedPrecondition));

    registry.ClearLastError();
    CHECK(registry.Register<DemoError>());
    CHECK(registry.Register<DemoChildError>());
    CHECK(registry.Has("demo.DemoError"));
    CHECK(registry.Has("demo.DemoChildError"));
    CHECK_EQ(registry.Depth("demo.DemoChildError"), std::uint8_t{2});

    CHECK(registry.Register("demo.ThrowingError", "go2cpp.Error", 1, []() -> ErrorPtr {
        return MakeError<ThrowingError>();
    }));

    const std::vector<TypeInfo> types = registry.Types();
    CHECK_EQ(types.size(), registry.Size());
    CHECK(!types.empty());
}

// ---------------------------------------------------------------------------
// 11. Registry 创建
// ---------------------------------------------------------------------------
void TestRegistryCreate() {
    Registry& registry = Registry::Instance();
    registry.ClearLastError();

    const ErrorPtr not_found = registry.Create("go2cpp.NotFoundError");
    CHECK(not_found != nullptr);
    CHECK_EQ(not_found->Code(), ErrorCode::kNotFound);
    CHECK(not_found->IsA("go2cpp.IOError"));

    const ErrorPtr timeout = registry.Create("go2cpp.TimeoutError");
    CHECK(timeout != nullptr);
    CHECK_EQ(timeout->Code(), ErrorCode::kTimeout);

    const ErrorPtr type_mismatch = registry.Create("go2cpp.TypeMismatchError");
    CHECK(type_mismatch != nullptr);
    CHECK_EQ(type_mismatch->Code(), ErrorCode::kTypeMismatch);

    const ErrorPtr joined = registry.Create("go2cpp.JoinError");
    CHECK(joined != nullptr);
    CHECK(joined->IsWrapping());

    const ErrorPtr demo = registry.Create("demo.DemoChildError");
    CHECK(demo != nullptr);
    CHECK_EQ(demo->TypeName(), std::string_view("demo.DemoChildError"));
    CHECK(demo->IsA("demo.DemoError"));

    registry.ClearLastError();
    CHECK(registry.Create("go2cpp.Error") == nullptr);
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kFailedPrecondition));

    registry.ClearLastError();
    CHECK(registry.Create("demo.NotRegistered") == nullptr);
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kNotFound));

    registry.ClearLastError();
    CHECK(registry.Register("demo.Boom", "go2cpp.Error", 1, []() -> ErrorPtr {
        throw std::runtime_error("factory boom");
    }));
    CHECK(registry.Create("demo.Boom") == nullptr);
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kInternal));
    CHECK(registry.Has("go2cpp.StringError"));

    registry.ClearLastError();
    CHECK(registry.Register("demo.EmptyFactory", "go2cpp.Error", 1, []() -> ErrorPtr {
        return ErrorPtr();
    }));
    CHECK(registry.Create("demo.EmptyFactory") == nullptr);
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kInternal));

    registry.ClearLastError();
    CHECK(registry.Register("demo.Reentrant", "go2cpp.Error", 1, []() -> ErrorPtr {
        Registry::Instance().Has("go2cpp.Error");
        return MakeError<DemoError>("reentrant");
    }));
    const ErrorPtr reentrant = registry.Create("demo.Reentrant");
    CHECK(reentrant != nullptr);
    CHECK_EQ(reentrant->Message(), std::string("reentrant"));
}

// ---------------------------------------------------------------------------
// 12. Registry 父链查询
// ---------------------------------------------------------------------------
void TestRegistryAncestors() {
    Registry& registry = Registry::Instance();

    const std::vector<std::string_view> chain = registry.Ancestors("go2cpp.FileNotFoundError");
    CHECK_EQ(chain.size(), std::size_t{4});
    CHECK_EQ(chain[0], std::string_view("go2cpp.FileNotFoundError"));
    CHECK_EQ(chain[1], std::string_view("go2cpp.NotFoundError"));
    CHECK_EQ(chain[2], std::string_view("go2cpp.IOError"));
    CHECK_EQ(chain[3], std::string_view("go2cpp.Error"));

    CHECK(registry.Ancestors("demo.Unknown").empty());
    CHECK(registry.Ancestors("go2cpp.Error").size() == std::size_t{1});

    CHECK(registry.IsA("go2cpp.FileNotFoundError", "go2cpp.IOError"));
    CHECK(registry.IsA("go2cpp.FileNotFoundError", "go2cpp.Error"));
    CHECK(!registry.IsA("go2cpp.IOError", "go2cpp.FileNotFoundError"));
    CHECK(!registry.IsA("demo.Unknown", "go2cpp.Error"));

    const std::vector<std::string_view> type_names = {
        "go2cpp.StringError",           "go2cpp.ArgumentError",
        "go2cpp.TypeMismatchError",     "go2cpp.FormatError",
        "go2cpp.OutOfRangeError",       "go2cpp.IOError",
        "go2cpp.TimeoutError",          "go2cpp.NotFoundError",
        "go2cpp.FileNotFoundError",     "go2cpp.PermissionError",
        "go2cpp.UnauthenticatedError",  "go2cpp.CancelledError",
        "go2cpp.ResourceExhaustedError","go2cpp.CorruptionError",
        "go2cpp.WrappedError",          "go2cpp.JoinError",
        "demo.DemoError",               "demo.DemoChildError"};
    for (const std::string_view type_name : type_names) {
        const ErrorPtr instance = registry.Create(type_name);
        CHECK(instance != nullptr);
        if (!instance) {
            continue;
        }
        CHECK_EQ(instance->TypeName(), type_name);
        CHECK_EQ(std::string_view(instance->ParentTypeName()), registry.ParentName(type_name));
        CHECK_EQ(instance->InheritanceDepth(), registry.Depth(type_name));
        CHECK_EQ(registry.Ancestors(type_name).size(),
                 static_cast<std::size_t>(instance->InheritanceDepth()) + 1U);
        for (const std::string_view ancestor : registry.Ancestors(type_name)) {
            CHECK(instance->IsA(ancestor));
        }
    }
}

// ---------------------------------------------------------------------------
// 13. 动态错误码与绑定
// ---------------------------------------------------------------------------
void TestRegistryCodes() {
    Registry& registry = Registry::Instance();

    const std::uint32_t code_a = registry.RegisterCode("demo.code.a");
    const std::uint32_t code_b = registry.RegisterCode("demo.code.b");
    CHECK(code_a >= static_cast<std::uint32_t>(kFirstCustomCode));
    CHECK(code_b > code_a);
    CHECK_EQ(registry.RegisterCode("demo.code.a"), code_a);
    CHECK_EQ(registry.LookupCode("demo.code.a"), code_a);
    CHECK_EQ(registry.LookupCode("demo.code.not_registered"), std::uint32_t{0});
    CHECK_EQ(registry.CodeName(code_a), std::string_view("demo.code.a"));
    CHECK_EQ(registry.CodeName(0), std::string_view("OK"));
    CHECK_EQ(registry.CodeName(3), std::string_view("INVALID_ARGUMENT"));
    CHECK_EQ(registry.CodeName(6), std::string_view("NOT_FOUND"));
    CHECK_EQ(registry.CodeName(10), std::string_view("TIMEOUT"));
    CHECK_EQ(registry.CodeName(22), std::string_view("WRAPPED"));
    CHECK_EQ(registry.CodeName(23), std::string_view("UNKNOWN"));
    CHECK_EQ(registry.CodeName(999), std::string_view("UNKNOWN"));
    CHECK_EQ(registry.CodeName(999999), std::string_view("<unknown>"));
    CHECK(registry.CodeCount() >= std::size_t{2});

    const std::uint32_t count_before = static_cast<std::uint32_t>(registry.CodeCount());
    registry.ClearLastError();
    CHECK_EQ(registry.RegisterCode(""), std::uint32_t{0});
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kInvalidArgument));
    CHECK_EQ(static_cast<std::uint32_t>(registry.CodeCount()), count_before);

    CHECK_EQ(registry.TypeNameOf(static_cast<std::int32_t>(ErrorCode::kNotFound)),
             std::string_view("go2cpp.NotFoundError"));
    CHECK_EQ(registry.TypeNameOf(static_cast<std::int32_t>(ErrorCode::kTimeout)),
             std::string_view("go2cpp.TimeoutError"));
    CHECK_EQ(registry.TypeNameOf(static_cast<std::int32_t>(ErrorCode::kCancelled)),
             std::string_view("go2cpp.CancelledError"));
    CHECK_EQ(registry.TypeNameOf(static_cast<std::int32_t>(ErrorCode::kIOError)),
             std::string_view("go2cpp.IOError"));
    CHECK_EQ(registry.TypeNameOf(static_cast<std::int32_t>(ErrorCode::kUnavailable)),
             std::string_view());
    CHECK_EQ(registry.CodeOf("go2cpp.NotFoundError"),
             static_cast<std::int32_t>(ErrorCode::kNotFound));
    CHECK_EQ(registry.CodeOf("go2cpp.FileNotFoundError"), kNoCode);
    CHECK_EQ(registry.CodeOf("demo.Unknown"), kNoCode);

    CHECK(registry.BindCode("go2cpp.NotFoundError",
                            static_cast<std::int32_t>(ErrorCode::kNotFound)));
    registry.ClearLastError();
    CHECK(!registry.BindCode("go2cpp.FileNotFoundError",
                             static_cast<std::int32_t>(ErrorCode::kNotFound)));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kAlreadyExists));
    registry.ClearLastError();
    CHECK(!registry.BindCode("demo.NotRegistered", static_cast<std::int32_t>(ErrorCode::kInternal)));
    CHECK_EQ(registry.LastErrorCode(), static_cast<std::uint32_t>(ErrorCode::kNotFound));

    CHECK(registry.BindCode("demo.DemoError", static_cast<std::int32_t>(code_a)));
    CHECK_EQ(registry.TypeNameOf(static_cast<std::int32_t>(code_a)),
             std::string_view("demo.DemoError"));

    registry.ClearLastError();
    CHECK_EQ(registry.LastErrorCode(), std::uint32_t{0});
    CHECK(registry.LastErrorDetail().empty());
}

// ---------------------------------------------------------------------------
// 14. 并发注册表
// ---------------------------------------------------------------------------
void TestConcurrentRegistry() {
    Registry& registry = Registry::Instance();
    constexpr int kThreads = 8;
    constexpr int kIterations = 500;

    std::atomic<int> create_failures{0};
    std::atomic<int> type_failures{0};
    std::atomic<int> code_failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([thread_index, &registry, &create_failures, &type_failures,
                              &code_failures]() {
            const std::string type_name = "demo.concurrent." + std::to_string(thread_index);
            if (!registry.Register(type_name, "go2cpp.Error", 1, [type_name]() -> ErrorPtr {
                    return NewError(type_name);
                })) {
                type_failures.fetch_add(1);
            }
            const std::string code_name = "demo.concurrent.code." + std::to_string(thread_index);
            const std::uint32_t code = registry.RegisterCode(code_name);
            if (code < static_cast<std::uint32_t>(kFirstCustomCode)) {
                code_failures.fetch_add(1);
            }
            if (registry.LookupCode(code_name) != code) {
                code_failures.fetch_add(1);
            }
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const ErrorPtr created = registry.Create("go2cpp.TimeoutError");
                if (!created || created->Code() != ErrorCode::kTimeout) {
                    create_failures.fetch_add(1);
                }
                if (registry.Ancestors("go2cpp.FileNotFoundError").size() != 4) {
                    create_failures.fetch_add(1);
                }
                if (!registry.IsA("go2cpp.FileNotFoundError", "go2cpp.IOError")) {
                    create_failures.fetch_add(1);
                }
                if (!registry.Has(type_name)) {
                    create_failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK_EQ(type_failures.load(), 0);
    CHECK_EQ(code_failures.load(), 0);
    CHECK_EQ(create_failures.load(), 0);
    CHECK(registry.Has("demo.concurrent.0"));
    CHECK(registry.Has("demo.concurrent.7"));
}

// ---------------------------------------------------------------------------
// 15. 并发只读错误图
// ---------------------------------------------------------------------------
void TestConcurrentErrorGraph() {
    const auto tree = Join({Wrap(MakeError<FileNotFoundError>("missing", "/tmp/a"), "load"),
                            Wrap(MakeError<TimeoutError>("timeout", "peer", 1000), "connect"),
                            MakeError<TypeMismatchError>("bad", "int", "string")});
    const std::string expected_message = tree->Message();
    const std::size_t expected_descendants = tree->UnwrapAll(0).size();
    CHECK_EQ(expected_descendants, std::size_t{5});

    constexpr int kThreads = 8;
    constexpr int kIterations = 300;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&tree, &expected_message, expected_descendants, &mismatches]() {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                if (tree->Message() != expected_message) {
                    mismatches.fetch_add(1);
                }
                if (tree->UnwrapAll(0).size() != expected_descendants) {
                    mismatches.fetch_add(1);
                }
                if (As<IOError>(tree) == nullptr) {
                    mismatches.fetch_add(1);
                }
                if (!Has<FileNotFoundError>(tree) || !Has<TypeMismatchError>(tree)) {
                    mismatches.fetch_add(1);
                }
                const WalkResult walked = WalkErrors(tree, [](const ErrorPtr&) {
                    return ErrorVisitResult::kContinue;
                });
                if (!walked.ok || walked.errors.size() != expected_descendants + 1) {
                    mismatches.fetch_add(1);
                }
                if (!HasErrorCode(tree, ErrorCode::kTimeout)) {
                    mismatches.fetch_add(1);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK_EQ(mismatches.load(), 0);
}

// ---------------------------------------------------------------------------
// 16. 旧体系互操作
// ---------------------------------------------------------------------------
void TestLegacyCompat() {
    const auto error = Wrap(MakeError<FileNotFoundError>("missing", "/tmp/a"), "load config");
    const ::go2cpp::ErrorPtr legacy = ToLegacy(error);
    CHECK(legacy != nullptr);
    CHECK_EQ(legacy->Message(), error->Message());

    const auto adapter = ::go2cpp::As<LegacyAdapter>(legacy);
    CHECK(adapter != nullptr);
    CHECK(adapter->Inner() == error);
    CHECK_EQ(legacy->UnwrapAll().size(), std::size_t{1});
    CHECK(legacy->Unwrap() != nullptr);
    CHECK_EQ(legacy->Unwrap()->Message(), std::string("missing [path=/tmp/a]"));

    const ::go2cpp::ErrorPtr outer = ::go2cpp::Wrap(legacy, "outer");
    CHECK_EQ(::go2cpp::ErrorMessage(outer),
             std::string("outer: load config: missing [path=/tmp/a]"));
    CHECK(::go2cpp::Is(outer, legacy));

    const ::go2cpp::ErrorPtr legacy_join =
        ::go2cpp::Join({::go2cpp::NewError("first"), ::go2cpp::NewError("second")});
    const ErrorPtr converted = FromLegacy(legacy_join);
    CHECK(converted != nullptr);
    CHECK_EQ(converted->UnwrapAll().size(), std::size_t{2});
    CHECK_EQ(converted->Message(), std::string("first\nsecond"));

    const ::go2cpp::ErrorPtr legacy_wrapped =
        ::go2cpp::Wrap(::go2cpp::NewError("cause"), "context");
    const ErrorPtr converted_wrapped = FromLegacy(legacy_wrapped);
    CHECK(converted_wrapped != nullptr);
    CHECK_EQ(converted_wrapped->Message(), std::string("context: cause"));
    CHECK_EQ(converted_wrapped->UnwrapAll().size(), std::size_t{0});

    CHECK(ToLegacy(ErrorPtr()) == ::go2cpp::ErrorPtr());
    CHECK(FromLegacy(::go2cpp::ErrorPtr()) == ErrorPtr());

    ErrorPtr deep = NewError("leaf");
    for (int index = 0; index < 1000; ++index) {
        deep = Wrap(deep, "layer");
    }
    const ::go2cpp::ErrorPtr deep_legacy = ToLegacy(deep);
    CHECK(deep_legacy != nullptr);
    CHECK(!deep_legacy->Message().empty());
}

// ---------------------------------------------------------------------------
// 17. Bento 桥
// ---------------------------------------------------------------------------
void TestBentoBridge() {
    const auto error = Wrap(MakeError<TimeoutError>("timeout", "peer", 2500), "connect");

    const ::go2cpp::bento::error::ErrorPtr bento_error =
        ::go2cpp::bento::error::FromGo2Cpp(error);
    CHECK(bento_error != nullptr);
    CHECK_EQ(bento_error->Message(), std::string("connect: timeout [path=peer]"));
    CHECK_EQ(bento_error->Code(), ::go2cpp::bento::error::ErrorCode::kWrappedError);
    CHECK_EQ(bento_error->TypeName(), std::string_view("go2cpp.WrappedError"));
    CHECK_EQ(bento_error->InheritanceDepth(), std::uint8_t{1});

    const auto adapter =
        ::go2cpp::bento::error::As<::go2cpp::bento::error::BentoAdapter>(bento_error);
    CHECK(adapter != nullptr);
    CHECK(adapter->Inner() == error);

    // 映射表自洽：16 个旧码都能查到新码，再反向回到原值。
    for (const auto& entry : ::go2cpp::bento::error::kLegacyCodeBridge) {
        CHECK_EQ(::go2cpp::bento::error::ToGo2CppCode(entry.legacy), entry.current);
        CHECK_EQ(::go2cpp::bento::error::ToBentoCode(entry.current), entry.legacy);
        CHECK(::go2cpp::bento::error::IsBentoCodeLossless(entry.current));
    }
    CHECK_EQ(::go2cpp::bento::error::kLegacyCodeBridge.size(), std::size_t{16});

    // 有损退化：类型错误 → INVALID_ARGUMENT；IO 错误 → UNAVAILABLE。
    CHECK_EQ(::go2cpp::bento::error::ToBentoCode(ErrorCode::kTypeMismatch),
             ::go2cpp::bento::error::ErrorCode::kInvalidArgument);
    CHECK_EQ(::go2cpp::bento::error::ToBentoCode(ErrorCode::kFormatError),
             ::go2cpp::bento::error::ErrorCode::kInvalidArgument);
    CHECK_EQ(::go2cpp::bento::error::ToBentoCode(ErrorCode::kTimeout),
             ::go2cpp::bento::error::ErrorCode::kDeadlineExceeded);
    CHECK_EQ(::go2cpp::bento::error::ToBentoCode(ErrorCode::kIOError),
             ::go2cpp::bento::error::ErrorCode::kUnavailable);
    CHECK(!::go2cpp::bento::error::IsBentoCodeLossless(ErrorCode::kTypeMismatch));

    const ::go2cpp::bento::error::ErrorPtr bento_join = ::go2cpp::bento::error::Join(
        {::go2cpp::bento::error::NewError("a", ::go2cpp::bento::error::ErrorCode::kNotFound),
         ::go2cpp::bento::error::NewError("b", ::go2cpp::bento::error::ErrorCode::kInternal)});
    const ErrorPtr converted = ::go2cpp::bento::error::ToGo2Cpp(bento_join);
    CHECK(converted != nullptr);
    CHECK_EQ(converted->UnwrapAll().size(), std::size_t{2});
    CHECK_EQ(converted->Message(), std::string("a\nb"));

    const ::go2cpp::bento::error::ErrorPtr bento_wrapped = ::go2cpp::bento::error::Wrap(
        ::go2cpp::bento::error::NewError("cause",
                                         ::go2cpp::bento::error::ErrorCode::kUnavailable),
        "ctx");
    const ErrorPtr converted_wrapped = ::go2cpp::bento::error::ToGo2Cpp(bento_wrapped);
    CHECK(converted_wrapped != nullptr);
    CHECK_EQ(converted_wrapped->Message(), std::string("ctx: cause"));
    CHECK_EQ(converted_wrapped->Code(), ErrorCode::kUnavailable);

    CHECK(::go2cpp::bento::error::FromGo2Cpp(ErrorPtr()) ==
          ::go2cpp::bento::error::ErrorPtr());
    CHECK(::go2cpp::bento::error::ToGo2Cpp(::go2cpp::bento::error::ErrorPtr()) == ErrorPtr());
}

// ---------------------------------------------------------------------------
// 18. 十万层深链
// ---------------------------------------------------------------------------
void TestDeepChain() {
    constexpr int kDepth = 100000;
    const auto start = std::chrono::steady_clock::now();
    {
        ErrorPtr chain = NewError("leaf");
        for (int index = 0; index < kDepth; ++index) {
            chain = Wrap(chain, "");
        }
        CHECK(chain != nullptr);
        CHECK_EQ(chain->UnwrapAll(1).size(), std::size_t{1});
        const std::string message = chain->Message();
        CHECK(!message.empty());
        CHECK(message.size() < 4096);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cout << "  deep chain: " << kDepth << " layers in " << elapsed << " ms\n";
    CHECK(elapsed < 60000);
}

// ---------------------------------------------------------------------------
// 19. 随机错误树
// ---------------------------------------------------------------------------
ErrorPtr BuildRandomTree(std::mt19937& rng, int depth, int& node_count) {
    std::uniform_int_distribution<int> kind_distribution(0, 3);
    std::uniform_int_distribution<int> type_distribution(0, 4);
    if (depth <= 0) {
        kind_distribution = std::uniform_int_distribution<int>(0, 0);
    }
    const int kind = kind_distribution(rng);
    const int type = type_distribution(rng);
    if (kind == 3) {
        const ErrorPtr child = BuildRandomTree(rng, depth - 1, node_count);
        ++node_count;
        return Wrap(child, "wrap");
    }
    ErrorPtr leaf;
    switch (type) {
        case 0:
            leaf = NewError("e", ErrorCode::kInternal);
            break;
        case 1:
            leaf = MakeError<ArgumentError>("a", "arg");
            break;
        case 2:
            leaf = MakeError<FileNotFoundError>("nf", "/tmp");
            break;
        case 3:
            leaf = MakeError<TypeMismatchError>("tm", "int", "string");
            break;
        default:
            leaf = MakeError<TimeoutError>("to", "peer", 10);
            break;
    }
    ++node_count;
    if (kind == 0) {
        return leaf;
    }
    if (kind == 1) {
        ++node_count;
        return Wrap(leaf, "ctx");
    }
    const ErrorPtr other = BuildRandomTree(rng, depth - 1, node_count);
    ++node_count;
    return Join({leaf, other});
}

std::string Fingerprint(const ErrorPtr& error) {
    if (!error) {
        return "<null>";
    }
    std::string result(error->TypeName());
    result += '|';
    if (error->IsWrapping()) {
        for (const auto& child : error->UnwrapAll()) {
            result += Fingerprint(child);
            result += ';';
        }
    }
    return result;
}

void TestRandomTrees() {
    std::mt19937 rng(20261030U);
    int total_nodes = 0;
    for (int round = 0; round < 300; ++round) {
        int node_count = 0;
        const ErrorPtr tree = BuildRandomTree(rng, 6, node_count);
        total_nodes += node_count;
        CHECK(tree != nullptr);
        const std::vector<ErrorPtr> direct = tree->UnwrapAll(1);
        const std::vector<ErrorPtr> all = tree->UnwrapAll(0);
        CHECK(all.size() >= direct.size());
        CHECK_EQ(all.size(), static_cast<std::size_t>(node_count) - 1U);

        const WalkResult walked = WalkErrors(tree, [](const ErrorPtr&) {
            return ErrorVisitResult::kContinue;
        });
        CHECK(walked.ok);
        CHECK_EQ(walked.errors.size(), all.size() + 1U);
        CHECK(!tree->Message().empty() || all.empty());

        bool has_type_mismatch = dynamic_cast<const TypeMismatchError*>(tree.get()) != nullptr;
        for (const auto& node : all) {
            if (dynamic_cast<const TypeMismatchError*>(node.get()) != nullptr) {
                has_type_mismatch = true;
            }
        }
        CHECK_EQ(Has<TypeMismatchError>(tree), has_type_mismatch);
        CHECK_EQ(Fingerprint(tree), Fingerprint(tree));
    }

    const auto replay = []() {
        std::mt19937 replay_rng(20261030U);
        std::string sequence;
        for (int round = 0; round < 20; ++round) {
            int node_count = 0;
            sequence += Fingerprint(BuildRandomTree(replay_rng, 6, node_count));
        }
        return sequence;
    };
    CHECK_EQ(replay(), replay());
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::cout << "  random trees: 300 rounds, " << total_nodes << " nodes\n";
}

// ---------------------------------------------------------------------------
// 20. 类型错误 / 超时专项
// ---------------------------------------------------------------------------
void TestTypeMismatchAndTimeout() {
    // 场景：反射/配置解码时期望 int，实际拿到 string。
    const ErrorPtr error = Wrap(MakeError<TypeMismatchError>(
                                    "field type mismatch", "int", "string"),
                                "decode config");
    const auto mismatch = As<TypeMismatchError>(error);
    CHECK(mismatch != nullptr);
    CHECK_EQ(mismatch->Expected(), std::string("int"));
    CHECK_EQ(mismatch->Actual(), std::string("string"));
    CHECK_EQ(error->Code(), ErrorCode::kWrappedError);
    CHECK(HasErrorCode(error, ErrorCode::kTypeMismatch));
    CHECK_EQ(ErrorCategoryOf(mismatch->Code()), ErrorCategory::kUsage);
    CHECK_EQ(ErrorMessage(error), std::string("decode config: field type mismatch "
                                               "[expected=int] [actual=string]"));

    // 场景：等待远端响应超时，必须能从超时里取回阈值并归类为可重试。
    const auto timeout = MakeError<TimeoutError>("read timeout", "peer:443", 1500);
    CHECK_EQ(timeout->Code(), ErrorCode::kTimeout);
    CHECK_EQ(timeout->TimeoutMs(), std::uint64_t{1500});
    CHECK_EQ(ErrorCategoryOf(timeout->Code()), ErrorCategory::kTransient);
    CHECK(timeout->IsA("go2cpp.IOError"));
    CHECK_EQ(As<TimeoutError>(Wrap(timeout, "connect"))->TimeoutMs(), std::uint64_t{1500});

    // 类型错误与超时的码不同、分类不同，不会在统计里混为一谈。
    CHECK(mismatch->Code() != timeout->Code());
    CHECK(ErrorCategoryOf(mismatch->Code()) != ErrorCategoryOf(timeout->Code()));
}

// ---------------------------------------------------------------------------
// 21. 每个内置码至少一个类型/场景
// ---------------------------------------------------------------------------
struct CodeScenario {
    ErrorCode code;
    const char* scenario;
    ErrorPtr (*factory)();
};

void TestEveryCodeHasScenario() {
    const std::array<CodeScenario, 23> scenarios = {{
        {ErrorCode::kOk, "无错误：空 ErrorPtr", []() -> ErrorPtr { return nullptr; }},
        {ErrorCode::kUnknown, "未知：默认 StringError",
         []() -> ErrorPtr { return NewError("unknown"); }},
        {ErrorCode::kTypeMismatch, "类型错误：期望 int 得到 string",
         []() -> ErrorPtr { return MakeError<TypeMismatchError>("tm", "int", "string"); }},
        {ErrorCode::kInvalidArgument, "参数非法：缺少必填参数",
         []() -> ErrorPtr { return MakeError<ArgumentError>("required", "name"); }},
        {ErrorCode::kOutOfRange, "越界：下标超过上限",
         []() -> ErrorPtr { return MakeError<OutOfRangeError>("index", 9, 3); }},
        {ErrorCode::kFormatError, "格式错误：JSON 解析失败",
         []() -> ErrorPtr { return MakeError<FormatError>("bad token", "json"); }},
        {ErrorCode::kNotFound, "不存在：配置键缺失",
         []() -> ErrorPtr { return MakeError<NotFoundError>("missing", "app.port"); }},
        {ErrorCode::kAlreadyExists, "已存在：重复注册类型",
         []() -> ErrorPtr { return NewError("dup", ErrorCode::kAlreadyExists); }},
        {ErrorCode::kPermissionDenied, "权限不足：无读权限",
         []() -> ErrorPtr { return MakeError<PermissionError>("denied", "/root/x"); }},
        {ErrorCode::kUnauthenticated, "未认证：token 过期",
         []() -> ErrorPtr { return MakeError<UnauthenticatedError>("expired", "alice"); }},
        {ErrorCode::kTimeout, "超时：等待响应超过阈值",
         []() -> ErrorPtr { return MakeError<TimeoutError>("slow", "peer", 100); }},
        {ErrorCode::kCancelled, "取消：context 被取消",
         []() -> ErrorPtr { return MakeError<CancelledError>("cancelled", "ctx.done"); }},
        {ErrorCode::kResourceExhausted, "资源耗尽：fd 用尽",
         []() -> ErrorPtr { return MakeError<ResourceExhaustedError>("full", "fd"); }},
        {ErrorCode::kUnavailable, "不可用：依赖服务未启动",
         []() -> ErrorPtr { return NewError("down", ErrorCode::kUnavailable); }},
        {ErrorCode::kFailedPrecondition, "前置条件：状态机顺序错误",
         []() -> ErrorPtr { return NewError("not ready", ErrorCode::kFailedPrecondition); }},
        {ErrorCode::kAborted, "中止：并发冲突导致放弃",
         []() -> ErrorPtr { return NewError("conflict", ErrorCode::kAborted); }},
        {ErrorCode::kIOError, "IO 错误：写入失败",
         []() -> ErrorPtr { return MakeError<IOError>("write failed", "/tmp/x"); }},
        {ErrorCode::kCorruption, "数据损坏：校验和不符",
         []() -> ErrorPtr { return MakeError<CorruptionError>("crc", "frame"); }},
        {ErrorCode::kInternal, "内部错误：不变量被破坏",
         []() -> ErrorPtr { return NewError("bug", ErrorCode::kInternal); }},
        {ErrorCode::kUnimplemented, "未实现：平台不支持",
         []() -> ErrorPtr { return NewError("n/a", ErrorCode::kUnimplemented); }},
        {ErrorCode::kWouldBlock, "未就绪：非阻塞读写", []() -> ErrorPtr {
             return NewError("again", ErrorCode::kWouldBlock);
         }},
        {ErrorCode::kDeadlock, "死锁：检测到循环等待",
         []() -> ErrorPtr { return NewError("cycle", ErrorCode::kDeadlock); }},
        {ErrorCode::kWrappedError, "包装节点：Wrap 上下文",
         []() -> ErrorPtr { return Wrap(NewError("cause"), "ctx"); }},
    }};

    CHECK_EQ(scenarios.size(), std::size_t{23});
    std::set<int> covered;
    for (const auto& entry : scenarios) {
        covered.insert(static_cast<int>(entry.code));
        const ErrorPtr error = entry.factory();
        CHECK_EQ(ErrorCodeOf(error), entry.code);
        if (error) {
            CHECK(!error->Message().empty());
            CHECK(!ErrorCodeName(error->Code()).empty());
        }
        CHECK(!std::string_view(entry.scenario).empty());
    }
    CHECK_EQ(covered.size(), std::size_t{23});
}

// ---------------------------------------------------------------------------
// 22. 真实场景（6 个）
// ---------------------------------------------------------------------------
void TestRealScenarios() {
    Registry& registry = Registry::Instance();

    // 场景 1：配置文件读取失败（FileNotFoundError + 包装上下文 + 按码查询）。
    {
        const ErrorPtr cause = MakeError<FileNotFoundError>("open failed", "/etc/app.conf");
        const ErrorPtr error = Wrap(cause, "load config");
        CHECK_EQ(error->Code(), ErrorCode::kWrappedError);
        CHECK(!error->IsA("go2cpp.IOError"));
        CHECK(Has<IOError>(error));
        CHECK(HasErrorCode(error, ErrorCode::kNotFound));
        CHECK(As<FileNotFoundError>(error) == cause);
        CHECK_EQ(ErrorMessage(error), std::string("load config: open failed [path=/etc/app.conf]"));
    }

    // 场景 2：类型错误（解码配置时字段类型不符）+ 分类统计。
    {
        const ErrorPtr error = Wrap(MakeError<TypeMismatchError>(
                                        "field type mismatch", "int", "string"),
                                    "decode config");
        const auto mismatch = As<TypeMismatchError>(error);
        CHECK(mismatch != nullptr);
        CHECK_EQ(mismatch->Expected(), std::string("int"));
        CHECK_EQ(ErrorCategoryOf(mismatch->Code()), ErrorCategory::kUsage);
        CHECK_EQ(std::string(ErrorCodeName(mismatch->Code())), std::string("TYPE_MISMATCH"));
    }

    // 场景 3：网络连接超时（TimeoutError + 阈值 + 类型检索 + 树形日志）。
    {
        const ErrorPtr error = Wrap(MakeError<TimeoutError>("connect timeout", "10.0.0.1:80", 3000),
                                    "dial peer");
        const auto timeout = As<TimeoutError>(error);
        CHECK(timeout != nullptr);
        CHECK_EQ(timeout->TimeoutMs(), std::uint64_t{3000});
        CHECK_EQ(timeout->Code(), ErrorCode::kTimeout);
        CHECK_EQ(ErrorCategoryOf(timeout->Code()), ErrorCategory::kTransient);
        CHECK(ErrorTreeString(error).find("go2cpp.TimeoutError") != std::string::npos);
    }

    // 场景 4：取消传播（CancelledError 与 Timeout 区分开，不误判成超时）。
    {
        const ErrorPtr error = MakeError<CancelledError>("request cancelled", "context");
        CHECK_EQ(error->Code(), ErrorCode::kCancelled);
        CHECK(!HasErrorCode(error, ErrorCode::kTimeout));
        CHECK_EQ(ErrorCategoryOf(error->Code()), ErrorCategory::kTransient);
        CHECK_EQ(ErrorMessage(error), std::string("request cancelled [reason=context]"));
    }

    // 场景 5：资源耗尽 + 批量任务部分失败（按错误码分组统计）。
    {
        std::vector<ErrorPtr> failures;
        std::unordered_map<int, int> expected_counts;
        for (int index = 0; index < 120; ++index) {
            switch (index % 4) {
                case 0:
                    failures.push_back(MakeError<NotFoundError>("nf", "k" + std::to_string(index)));
                    ++expected_counts[static_cast<int>(ErrorCode::kNotFound)];
                    break;
                case 1:
                    failures.push_back(
                        MakeError<TimeoutError>("to", "p" + std::to_string(index), 100));
                    ++expected_counts[static_cast<int>(ErrorCode::kTimeout)];
                    break;
                case 2:
                    failures.push_back(
                        MakeError<ResourceExhaustedError>("full", "fd"));
                    ++expected_counts[static_cast<int>(ErrorCode::kResourceExhausted)];
                    break;
                default:
                    failures.push_back(
                        MakeError<PermissionError>("denied", "/r" + std::to_string(index)));
                    ++expected_counts[static_cast<int>(ErrorCode::kPermissionDenied)];
                    break;
            }
        }
        const ErrorPtr error = Join(std::move(failures));
        CHECK(error != nullptr);
        CHECK_EQ(error->UnwrapAll().size(), std::size_t{120});
        std::unordered_map<int, int> actual_counts;
        const WalkResult walked = WalkErrors(error, [&actual_counts](const ErrorPtr& node) {
            ++actual_counts[static_cast<int>(node->Code())];
            return ErrorVisitResult::kContinue;
        });
        CHECK(walked.ok);
        CHECK_EQ(actual_counts[static_cast<int>(ErrorCode::kWrappedError)], 1);
        for (const auto& entry : expected_counts) {
            CHECK_EQ(actual_counts[entry.first], entry.second);
        }
    }

    // 场景 6：跨体系（旧模块 + Bento 服务端）统一处理同一错误。
    {
        const ErrorPtr error = Wrap(MakeError<PermissionError>("denied", "/root/secret"), "open");
        const ::go2cpp::ErrorPtr legacy = ToLegacy(error);
        CHECK_EQ(::go2cpp::ErrorMessage(legacy), error->Message());
        CHECK(::go2cpp::As<LegacyAdapter>(legacy) != nullptr);

        const ::go2cpp::bento::error::ErrorPtr bento_error =
            ::go2cpp::bento::error::FromGo2Cpp(error);
        CHECK_EQ(bento_error->Message(), error->Message());
        CHECK_EQ(bento_error->Code(), ::go2cpp::bento::error::ErrorCode::kWrappedError);
        const ErrorPtr round_trip = ::go2cpp::bento::error::ToGo2Cpp(bento_error);
        CHECK_EQ(round_trip->Message(), error->Message());
        CHECK_EQ(round_trip->Code(), ErrorCode::kPermissionDenied);
    }

    // 注册表仍可用：错误码名字映射覆盖新码。
    CHECK_EQ(registry.CodeName(static_cast<std::uint32_t>(ErrorCode::kTypeMismatch)),
             std::string_view("TYPE_MISMATCH"));
}

// ---------------------------------------------------------------------------
// 注册表压测（量化指标）
// ---------------------------------------------------------------------------
void TestRegistryThroughput() {
    Registry& registry = Registry::Instance();
    constexpr int kIterations = 200000;
    const auto start = std::chrono::steady_clock::now();
    std::size_t created = 0;
    for (int index = 0; index < kIterations; ++index) {
        const ErrorPtr error = registry.Create("go2cpp.StringError");
        if (error != nullptr) {
            ++created;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start);
    const double per_operation_ns =
        static_cast<double>(elapsed.count()) * 1000000.0 / static_cast<double>(kIterations);
    CHECK_EQ(created, static_cast<std::size_t>(kIterations));
    {
        std::lock_guard<std::mutex> lock(g_output_mutex);
        std::cout << "  registry.Create: " << kIterations << " ops in " << elapsed.count()
                  << " ms (" << per_operation_ns << " ns/op)\n";
    }
    CHECK(elapsed.count() < 60000);
}

} // namespace

int main() {
    std::cout << "=== go2cpp::error enhanced tests ===\n";
    RunTest("error_codes", TestErrorCodes);
    RunTest("legacy_code_mapping", TestLegacyCodeMapping);
    RunTest("type_chain", TestTypeChain);
    RunTest("is_semantics", TestIsSemantics);
    RunTest("unwrap_levels", TestUnwrapLevels);
    RunTest("wrap_messages", TestWrapMessages);
    RunTest("query_types", TestQueryTypes);
    RunTest("walk", TestWalk);
    RunTest("tree_string", TestTreeString);
    RunTest("registry_registration", TestRegistryRegistration);
    RunTest("registry_create", TestRegistryCreate);
    RunTest("registry_ancestors", TestRegistryAncestors);
    RunTest("registry_codes", TestRegistryCodes);
    RunTest("concurrent_registry", TestConcurrentRegistry);
    RunTest("concurrent_error_graph", TestConcurrentErrorGraph);
    RunTest("legacy_compat", TestLegacyCompat);
    RunTest("bento_bridge", TestBentoBridge);
    RunTest("deep_chain", TestDeepChain);
    RunTest("random_trees", TestRandomTrees);
    RunTest("type_mismatch_and_timeout", TestTypeMismatchAndTimeout);
    RunTest("every_code_has_scenario", TestEveryCodeHasScenario);
    RunTest("real_scenarios", TestRealScenarios);
    RunTest("registry_throughput", TestRegistryThroughput);

    const int failures = g_failures.load();
    std::cout << "=== " << (failures == 0 ? "PASSED" : "FAILED") << ": 23 tests, "
              << g_checks.load() << " checks, " << failures << " failures ===\n";
    return failures == 0 ? 0 : 1;
}
