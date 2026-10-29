/**
 * @file test_error.cpp
 * @brief bento::error 验收测试：错误图、遍历/查询、注册表、映射与深链。
 *
 * @details 覆盖四类场景：
 * 1. 功能语义：字符串错、包装、聚合、展平、层级展开；
 * 2. 边界/异常：空指针、环、抛异常的 UnwrapAll、深链（10 万层）；
 * 3. 注册表：内置码、幂等、名字视图稳定性、并发注册、类型工厂；
 * 4. 映射：HTTP/日志/JSON、策略注入、JSON 转义与深度截断。
 *
 * 运行方式：构建后执行 `./tests/bento/bento_test_error`（或 ctest -R bento_error）。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "go2cpp/bento/error/error.hpp"
#include "go2cpp/bento/error/mapping.hpp"
#include "go2cpp/bento/error/registry.hpp"
#include "test_error_support.hpp"

namespace {

using namespace go2cpp::bento::error;

// ---------------------------------------------------------------------------
// 测试用错误类型
// ---------------------------------------------------------------------------

/// @brief 带整型标记的叶子错误，用于验证 As<T>/Has<T>。
class TaggedError final : public Error {
 public:
    explicit TaggedError(int tag) : m_tag(tag) {}

    BENTO_ERROR_CLASS("test.TaggedError", "bento.Error", 1)

    std::string Message() const override { return "tagged"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }

    int Tag() const noexcept { return m_tag; }

 private:
    int m_tag;
};

/// @brief 反射层级测试：基类（深度 1）。
class BaseCustomError : public Error {
 public:
    BENTO_ERROR_CLASS("test.BaseCustomError", "bento.Error", 1)

    std::string Message() const override { return "base"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
};

/// @brief 反射层级测试：派生类（深度 2）。
class LeafCustomError final : public BaseCustomError {
 public:
    BENTO_ERROR_CLASS("test.LeafCustomError", "test.BaseCustomError", 2)

    std::string Message() const override { return "leaf"; }
};

/// @brief 深度 4，注册必须被拒绝。
class DeepCustomError final : public Error {
 public:
    BENTO_ERROR_CLASS("test.DeepCustomError", "bento.Error", 4)

    std::string Message() const override { return "deep"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
};

/// @brief 自指错误：UnwrapAll 返回自己，验证环安全。
class CycleError final : public Error {
 public:
    BENTO_ERROR_CLASS("test.CycleError", "bento.Error", 1)

    std::string Message() const override { return "cycle"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
    bool IsWrapping() const noexcept override { return true; }
    std::vector<ErrorPtr> UnwrapAll(std::size_t /*level*/) const override {
        return {m_self.lock()};
    }

    void SetSelf(const ErrorPtr& self) { m_self = self; }

 private:
    std::weak_ptr<const Error> m_self;
};

/// @brief UnwrapAll 抛异常，验证 noexcept 查询接口的兜底。
class ThrowingUnwrapError final : public Error {
 public:
    BENTO_ERROR_CLASS("test.ThrowingUnwrapError", "bento.Error", 1)

    std::string Message() const override { return "thrower"; }
    ErrorCode Code() const noexcept override { return ErrorCode::kInternal; }
    bool IsWrapping() const noexcept override { return true; }
    std::vector<ErrorPtr> UnwrapAll(std::size_t /*level*/) const override {
        throw std::runtime_error("boom");
    }
};

// ---------------------------------------------------------------------------
// 基础语义
// ---------------------------------------------------------------------------

void TestStringErrorBasics() {
    const ErrorPtr leaf = NewError("leaf", ErrorCode::kDataLoss);
    BENTO_ERROR_CHECK(leaf != nullptr);
    BENTO_ERROR_CHECK_EQ(leaf->Message(), std::string("leaf"));
    BENTO_ERROR_CHECK_EQ(leaf->Code(), ErrorCode::kDataLoss);
    BENTO_ERROR_CHECK_EQ(leaf->TypeName(), std::string_view("bento.StringError"));
    BENTO_ERROR_CHECK_EQ(leaf->ParentTypeName(), std::string_view("bento.Error"));
    BENTO_ERROR_CHECK_EQ(leaf->InheritanceDepth(), std::uint8_t{1});
    BENTO_ERROR_CHECK(!leaf->IsWrapping());
    BENTO_ERROR_CHECK(leaf->UnwrapAll().empty());
    BENTO_ERROR_CHECK(leaf->UnwrapAll(0).empty());
    BENTO_ERROR_CHECK(leaf->Is(*leaf));
}

void TestErrorCodes() {
    BENTO_ERROR_CHECK_EQ(static_cast<int>(ErrorCode::kOk), 0);
    BENTO_ERROR_CHECK_EQ(static_cast<int>(ErrorCode::kDeadlineExceeded), 14);
    BENTO_ERROR_CHECK_EQ(static_cast<int>(ErrorCode::kWrappedError), 15);
    BENTO_ERROR_CHECK_EQ(ErrorCodeName(ErrorCode::kDeadlineExceeded),
                         std::string_view("DEADLINE_EXCEEDED"));
    BENTO_ERROR_CHECK_EQ(ErrorCodeName(static_cast<ErrorCode>(999)),
                         std::string_view("UNKNOWN"));

    const ErrorCode codes[] = {
        ErrorCode::kOk,          ErrorCode::kUnknown,        ErrorCode::kInvalidArgument,
        ErrorCode::kNotFound,    ErrorCode::kAlreadyExists,  ErrorCode::kPermissionDenied,
        ErrorCode::kResourceExhausted, ErrorCode::kFailedPrecondition, ErrorCode::kOutOfRange,
        ErrorCode::kUnimplemented, ErrorCode::kInternal,     ErrorCode::kUnavailable,
        ErrorCode::kDataLoss,    ErrorCode::kCancelled,      ErrorCode::kDeadlineExceeded,
        ErrorCode::kWrappedError,
    };
    std::unordered_set<int> unique;
    for (const ErrorCode code : codes) {
        unique.insert(static_cast<int>(code));
    }
    BENTO_ERROR_CHECK_EQ(unique.size(), sizeof(codes) / sizeof(codes[0]));
}

void TestNewErrorDefaultCode() {
    const ErrorPtr error = NewError("x");
    BENTO_ERROR_CHECK_EQ(error->Code(), ErrorCode::kUnknown);
    BENTO_ERROR_CHECK_EQ(ErrorMessage(nullptr), std::string(""));
    BENTO_ERROR_CHECK(!IsWrappedError(nullptr));
}

void TestWrapChainMessage() {
    const ErrorPtr leaf = NewError("leaf");
    const ErrorPtr db = Wrap(leaf, "db");
    const ErrorPtr api = Wrap(db, "api");
    BENTO_ERROR_CHECK_EQ(ErrorMessage(api), std::string("api: db: leaf"));
    BENTO_ERROR_CHECK_EQ(api->Code(), ErrorCode::kWrappedError);
    BENTO_ERROR_CHECK(api->IsWrapping());
    BENTO_ERROR_CHECK_EQ(api->TypeName(), std::string_view("bento.WrappedError"));
    BENTO_ERROR_CHECK_EQ(api->InheritanceDepth(), std::uint8_t{1});
    BENTO_ERROR_CHECK_EQ(api->UnwrapAll(1).size(), std::size_t{1});
    BENTO_ERROR_CHECK_EQ(api->UnwrapAll(0).size(), std::size_t{2});
    BENTO_ERROR_CHECK_EQ(api->UnwrapAll(0)[0], db);
    BENTO_ERROR_CHECK_EQ(api->UnwrapAll(0)[1], leaf);
}

void TestWrapNullAndEmptyMessage() {
    BENTO_ERROR_CHECK(Wrap(nullptr, "ctx") == nullptr);
    BENTO_ERROR_CHECK_EQ(ErrorMessage(Wrap(NewError("leaf"), "")), std::string("leaf"));
    BENTO_ERROR_CHECK_EQ(ErrorMessage(Wrap(NewError(""), "ctx")), std::string("ctx"));
    BENTO_ERROR_CHECK_EQ(ErrorMessage(Wrap(NewError(""), "")), std::string(""));
}

void TestJoinBasics() {
    const ErrorPtr first = NewError("a");
    const ErrorPtr second = NewError("b");
    const ErrorPtr joined = Join({first, second});
    BENTO_ERROR_CHECK(joined != nullptr);
    BENTO_ERROR_CHECK_EQ(joined->Message(), std::string("a\nb"));
    BENTO_ERROR_CHECK_EQ(joined->Code(), ErrorCode::kWrappedError);
    BENTO_ERROR_CHECK(joined->IsWrapping());
    BENTO_ERROR_CHECK_EQ(joined->TypeName(), std::string_view("bento.JoinError"));
    BENTO_ERROR_CHECK_EQ(joined->UnwrapAll(1).size(), std::size_t{2});
    BENTO_ERROR_CHECK_EQ(joined->UnwrapAll(0).size(), std::size_t{2});

    BENTO_ERROR_CHECK(Join(std::vector<ErrorPtr>{}) == nullptr);
    BENTO_ERROR_CHECK(Join({nullptr, nullptr}) == nullptr);
    BENTO_ERROR_CHECK_EQ(Join({first, nullptr})->UnwrapAll(1).size(), std::size_t{1});
}

void TestJoinNestedMessage() {
    const ErrorPtr inner = Join({NewError("b"), NewError("c")});
    const ErrorPtr outer = Join({NewError("a"), inner});
    BENTO_ERROR_CHECK_EQ(ErrorMessage(outer), std::string("a\nb\nc"));
    // 后代包含嵌套的 Join 节点本身：inner、b、c、a -> 4。
    BENTO_ERROR_CHECK_EQ(outer->UnwrapAll(0).size(), std::size_t{4});
}

void TestUnwrapLevelsAndFreeFunction() {
    const ErrorPtr first = NewError("a");
    const ErrorPtr second = NewError("b");
    const ErrorPtr inner = Join({first, second});
    const ErrorPtr wrapped = Wrap(inner, "ctx");

    BENTO_ERROR_CHECK_EQ(wrapped->UnwrapAll(1).size(), std::size_t{1});
    BENTO_ERROR_CHECK_EQ(wrapped->UnwrapAll(0).size(), std::size_t{3});
    BENTO_ERROR_CHECK_EQ(UnwrapAll(wrapped, 1).size(), std::size_t{1});
    BENTO_ERROR_CHECK_EQ(UnwrapAll(wrapped, 0).size(), std::size_t{3});
    BENTO_ERROR_CHECK(UnwrapAll(nullptr).empty());
    BENTO_ERROR_CHECK(UnwrapAll(first, 1).empty());
}

// ---------------------------------------------------------------------------
// 查询与遍历
// ---------------------------------------------------------------------------

void TestIsIdentityAndNull() {
    const ErrorPtr first = NewError("same");
    const ErrorPtr second = NewError("same");
    BENTO_ERROR_CHECK(Is(first, first));
    BENTO_ERROR_CHECK(!Is(first, second));
    BENTO_ERROR_CHECK(Is(ErrorPtr{}, ErrorPtr{}));
    BENTO_ERROR_CHECK(!Is(first, ErrorPtr{}));
    BENTO_ERROR_CHECK(!Is(ErrorPtr{}, first));
}

void TestIsTraversal() {
    const ErrorPtr leaf = NewError("leaf");
    const ErrorPtr wrapped = Wrap(leaf, "w");
    const ErrorPtr joined = Join({NewError("x"), wrapped});
    BENTO_ERROR_CHECK(Is(joined, leaf));
    BENTO_ERROR_CHECK(Is(joined, wrapped));
    BENTO_ERROR_CHECK(!Is(joined, NewError("leaf")));
}

void TestHasCode() {
    const ErrorPtr joined =
        Join({Wrap(NewError("io", ErrorCode::kUnavailable), "read"), NewError("x")});
    BENTO_ERROR_CHECK(Has(joined, ErrorCode::kUnavailable));
    BENTO_ERROR_CHECK(Has(joined, ErrorCode::kUnknown));
    BENTO_ERROR_CHECK(!Has(joined, ErrorCode::kCancelled));
    BENTO_ERROR_CHECK(!Has(nullptr, ErrorCode::kInternal));
}

void TestWalkPreOrder() {
    const ErrorPtr leaf_a = NewError("a");
    const ErrorPtr wrap_a = Wrap(leaf_a, "x");
    const ErrorPtr leaf_b = NewError("b");
    const ErrorPtr root = Join({wrap_a, leaf_b});

    const WalkResult walked = WalkErrors(
        root, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
    BENTO_ERROR_CHECK(walked.ok);
    BENTO_ERROR_CHECK_EQ(walked.status, ErrorVisitResult::kContinue);
    BENTO_ERROR_CHECK_EQ(walked.errors.size(), std::size_t{4});
    if (walked.errors.size() == 4) {
        BENTO_ERROR_CHECK_EQ(walked.errors[0], root);
        BENTO_ERROR_CHECK_EQ(walked.errors[1], wrap_a);
        BENTO_ERROR_CHECK_EQ(walked.errors[2], leaf_a);
        BENTO_ERROR_CHECK_EQ(walked.errors[3], leaf_b);
    }
}

void TestWalkSkipChildren() {
    const ErrorPtr leaf = NewError("a");
    const ErrorPtr wrap = Wrap(leaf, "x");
    const ErrorPtr root = Join({wrap, NewError("b")});

    const WalkResult walked = WalkErrors(root, [&wrap](const ErrorPtr& current) {
        return current == wrap ? ErrorVisitResult::kSkipChildren
                               : ErrorVisitResult::kContinue;
    });
    BENTO_ERROR_CHECK(walked.ok);
    BENTO_ERROR_CHECK_EQ(walked.errors.size(), std::size_t{3});
    if (walked.errors.size() == 3) {
        BENTO_ERROR_CHECK_EQ(walked.errors[2], root->UnwrapAll(1)[1]);
    }
}

void TestWalkStop() {
    const ErrorPtr root = Join({NewError("a"), Wrap(NewError("b"), "x")});
    int visited = 0;
    const WalkResult walked = WalkErrors(root, [&visited](const ErrorPtr&) {
        ++visited;
        return visited == 2 ? ErrorVisitResult::kStop : ErrorVisitResult::kContinue;
    });
    BENTO_ERROR_CHECK(walked.ok);
    BENTO_ERROR_CHECK_EQ(walked.status, ErrorVisitResult::kStop);
    BENTO_ERROR_CHECK_EQ(walked.errors.size(), std::size_t{2});
}

void TestWalkEmptyAndNullVisitor() {
    const WalkResult empty = WalkErrors(
        nullptr, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
    BENTO_ERROR_CHECK(empty.ok);
    BENTO_ERROR_CHECK(empty.errors.empty());

    const WalkResult invalid = WalkErrors(NewError("x"), ErrorVisitor{});
    BENTO_ERROR_CHECK(!invalid.ok);
    BENTO_ERROR_CHECK(invalid.errors.empty());
}

void TestWalkVisitorThrows() {
    const ErrorPtr root = Join({NewError("a"), NewError("b")});
    const WalkResult walked = WalkErrors(root, [](const ErrorPtr&) -> ErrorVisitResult {
        throw std::runtime_error("visitor failure");
    });
    BENTO_ERROR_CHECK(!walked.ok);
    BENTO_ERROR_CHECK_EQ(walked.errors.size(), std::size_t{1});
}

void TestAsAndHasType() {
    const auto tagged = std::make_shared<TaggedError>(7);
    const ErrorPtr root = Join({NewError("plain"), Wrap(tagged, "ctx")});

    const std::shared_ptr<const TaggedError> found = As<TaggedError>(root);
    BENTO_ERROR_CHECK(found != nullptr);
    if (found) {
        BENTO_ERROR_CHECK_EQ(found->Tag(), 7);
    }
    BENTO_ERROR_CHECK(Has<TaggedError>(root));
    BENTO_ERROR_CHECK(!Has<TaggedError>(NewError("plain")));
    BENTO_ERROR_CHECK(As<TaggedError>(nullptr) == nullptr);
    BENTO_ERROR_CHECK(As<BaseCustomError>(root) == nullptr);
}

void TestAsPreOrder() {
    const ErrorPtr first = Wrap(std::make_shared<TaggedError>(1), "first");
    const ErrorPtr second = Wrap(std::make_shared<TaggedError>(2), "second");
    const ErrorPtr root = Join({first, second});
    const auto found = As<TaggedError>(root);
    BENTO_ERROR_CHECK(found != nullptr);
    if (found) {
        BENTO_ERROR_CHECK_EQ(found->Tag(), 1);
    }
}

void TestCycleSafety() {
    auto cycle = std::make_shared<CycleError>();
    cycle->SetSelf(cycle);
    const ErrorPtr root = cycle;

    BENTO_ERROR_CHECK(Is(root, root));
    BENTO_ERROR_CHECK(!Is(root, NewError("other")));
    BENTO_ERROR_CHECK(!Has<TaggedError>(root));
    BENTO_ERROR_CHECK(Has(root, ErrorCode::kInternal));

    int visits = 0;
    const WalkResult walked = WalkErrors(root, [&visits](const ErrorPtr&) {
        ++visits;
        return ErrorVisitResult::kContinue;
    });
    BENTO_ERROR_CHECK(walked.ok);
    BENTO_ERROR_CHECK_EQ(visits, 1);
    BENTO_ERROR_CHECK_EQ(walked.errors.size(), std::size_t{1});
}

void TestThrowingUnwrapIsContained() {
    const auto thrower = std::make_shared<ThrowingUnwrapError>();
    const ErrorPtr root = Wrap(thrower, "outer");
    BENTO_ERROR_CHECK(Is(root, thrower));
    BENTO_ERROR_CHECK(Has(root, ErrorCode::kInternal));
    BENTO_ERROR_CHECK(!Is(root, NewError("other")));
    BENTO_ERROR_CHECK(As<StringError>(root) == nullptr);
    BENTO_ERROR_CHECK(!Has(root, ErrorCode::kCancelled));
}

// ---------------------------------------------------------------------------
// 深链与深 Join（析构不得递归爆栈）
// ---------------------------------------------------------------------------

void TestDeepChain() {
    const std::size_t depth = 100000;
    const ErrorPtr leaf = NewError("leaf", ErrorCode::kDataLoss);
    ErrorPtr deep = leaf;
    for (std::size_t index = 0; index < depth; ++index) {
        deep = Wrap(deep, "w");
    }

    BENTO_ERROR_CHECK(Is(deep, leaf));
    BENTO_ERROR_CHECK(Has(deep, ErrorCode::kDataLoss));
    BENTO_ERROR_CHECK(As<StringError>(deep) == leaf);
    BENTO_ERROR_CHECK_EQ(deep->UnwrapAll(0).size(), depth);
    const std::string message = ErrorMessage(deep);
    BENTO_ERROR_CHECK_EQ(message.size(), depth * 3U + 4U);
    BENTO_ERROR_CHECK(message.find(": leaf") == message.size() - 6U);

    deep.reset(); // 触发 10 万层迭代析构。
    BENTO_ERROR_CHECK(leaf != nullptr);
}

void TestWalkDeepChain() {
    constexpr std::size_t kDepth = 20000;
    const ErrorPtr leaf = NewError("leaf");
    ErrorPtr root = leaf;
    for (std::size_t index = 0; index < kDepth; ++index) {
        root = Wrap(root, "w");
    }
    const WalkResult walked = WalkErrors(
        root, [](const ErrorPtr&) { return ErrorVisitResult::kContinue; });
    BENTO_ERROR_CHECK(walked.ok);
    BENTO_ERROR_CHECK_EQ(walked.errors.size(), kDepth + 1);
    if (walked.errors.size() == kDepth + 1) {
        BENTO_ERROR_CHECK_EQ(walked.errors.front(), root);
        BENTO_ERROR_CHECK_EQ(walked.errors.back(), leaf);
    }
    root.reset();
    BENTO_ERROR_CHECK(leaf != nullptr);
}

void TestCycleUnwrapAllDedup() {
    auto cycle = std::make_shared<CycleError>();
    cycle->SetSelf(cycle);
    const std::vector<ErrorPtr> all = cycle->UnwrapAll(0);
    BENTO_ERROR_CHECK_EQ(all.size(), std::size_t{1});
    if (!all.empty()) {
        BENTO_ERROR_CHECK_EQ(all.front(), ErrorPtr(cycle));
    }
    BENTO_ERROR_CHECK_EQ(cycle->UnwrapAll(1).size(), std::size_t{1});
}

void TestAsInCycle() {
    auto cycle = std::make_shared<CycleError>();
    cycle->SetSelf(cycle);
    const ErrorPtr root = Join({cycle, Wrap(std::make_shared<TaggedError>(9), "ctx")});
    const auto found = As<TaggedError>(root);
    BENTO_ERROR_CHECK(found != nullptr);
    if (found) {
        BENTO_ERROR_CHECK_EQ(found->Tag(), 9);
    }
    BENTO_ERROR_CHECK(Is(root, cycle));
}

void TestDeepJoinDestruction() {
    const std::size_t depth = 20000;
    const ErrorPtr leaf = NewError("leaf", ErrorCode::kDataLoss);
    ErrorPtr joined = leaf;
    for (std::size_t index = 0; index < depth; ++index) {
        joined = Join({joined, NewError("x")});
    }
    BENTO_ERROR_CHECK(Has(joined, ErrorCode::kDataLoss));
    // 每层新增一个 Join + 一个 "x" 叶子；根自身不计入后代。
    BENTO_ERROR_CHECK_EQ(joined->UnwrapAll(0).size(), depth * 2U);
    joined.reset();
    BENTO_ERROR_CHECK(leaf != nullptr);
}

// ---------------------------------------------------------------------------
// ErrorCodeRegistry
// ---------------------------------------------------------------------------

void TestCodeRegistryBuiltins() {
    ErrorCodeRegistry& registry = ErrorCodeRegistry::Instance();
    BENTO_ERROR_CHECK_EQ(registry.Name(0), std::string_view("kOk"));
    BENTO_ERROR_CHECK(registry.Size() >= 10U);

    const std::uint32_t builtins[] = {codes::kProtocol, codes::kFrame,   codes::kChecksum,
                                      codes::kSession, codes::kPeer,    codes::kTimeout,
                                      codes::kCrypto,  codes::kConfig,  codes::kInternal,
                                      codes::kConstraint};
    const std::string_view expected[] = {"bento.protocol", "bento.frame",  "bento.checksum",
                                         "bento.session",  "bento.peer",   "bento.timeout",
                                         "bento.crypto",   "bento.config", "bento.internal",
                                         "bento.constraint"};
    std::unordered_set<std::uint32_t> unique;
    for (std::size_t index = 0; index < sizeof(builtins) / sizeof(builtins[0]); ++index) {
        BENTO_ERROR_CHECK(builtins[index] != 0U);
        unique.insert(builtins[index]);
        BENTO_ERROR_CHECK_EQ(registry.Name(builtins[index]), expected[index]);
        BENTO_ERROR_CHECK_EQ(registry.Lookup(expected[index]), builtins[index]);
        BENTO_ERROR_CHECK(registry.Contains(expected[index]));
    }
    BENTO_ERROR_CHECK_EQ(unique.size(), sizeof(builtins) / sizeof(builtins[0]));
}

void TestCodeRegistryIdempotentAndInvalid() {
    ErrorCodeRegistry& registry = ErrorCodeRegistry::Instance();
    const std::uint32_t first = registry.Register("test.error.same_name");
    BENTO_ERROR_CHECK(first != 0U);
    const std::size_t size = registry.Size();
    BENTO_ERROR_CHECK_EQ(registry.Register("test.error.same_name"), first);
    BENTO_ERROR_CHECK_EQ(registry.Size(), size);
    BENTO_ERROR_CHECK_EQ(registry.Name(first), std::string_view("test.error.same_name"));

    BENTO_ERROR_CHECK_EQ(registry.Register(""), 0U);
    BENTO_ERROR_CHECK_EQ(registry.Name(0xFFFFFFFFU), std::string_view("<unknown>"));
    BENTO_ERROR_CHECK_EQ(registry.Lookup("test.error.not_registered"), 0U);
    BENTO_ERROR_CHECK(!registry.Contains("test.error.not_registered"));
}

void TestCodeRegistryViewStability() {
    ErrorCodeRegistry& registry = ErrorCodeRegistry::Instance();
    const std::string_view view = registry.Name(codes::kProtocol);
    for (int index = 0; index < 256; ++index) {
        registry.Register("test.error.stability." + std::to_string(index));
    }
    BENTO_ERROR_CHECK_EQ(view, std::string_view("bento.protocol"));
}

void TestCodeRegistryConcurrent() {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 250;
    constexpr std::size_t kTotal = static_cast<std::size_t>(kThreads) *
                                   static_cast<std::size_t>(kPerThread);
    std::vector<std::uint32_t> allocated(kTotal, 0);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([thread_index, &allocated]() {
            for (int index = 0; index < kPerThread; ++index) {
                const std::string name = "test.error.concurrent." +
                                         std::to_string(thread_index) + "." +
                                         std::to_string(index);
                const std::size_t slot =
                    static_cast<std::size_t>(thread_index) * kPerThread +
                    static_cast<std::size_t>(index);
                allocated[slot] = ErrorCodeRegistry::Instance().Register(name);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    const std::unordered_set<std::uint32_t> unique(allocated.begin(), allocated.end());
    BENTO_ERROR_CHECK_EQ(unique.size(), kTotal);
    BENTO_ERROR_CHECK(unique.count(0U) == 0U);
}

// ---------------------------------------------------------------------------
// ErrorTypeRegistry
// ---------------------------------------------------------------------------

void TestCodeRegistryBulkRoundTrip() {
    ErrorCodeRegistry& registry = ErrorCodeRegistry::Instance();
    std::vector<std::pair<std::uint32_t, std::string>> registered;
    registered.reserve(512);
    for (int index = 0; index < 512; ++index) {
        const std::string name = "test.error.bulk." + std::to_string(index);
        registered.emplace_back(registry.Register(name), name);
    }
    for (const auto& entry : registered) {
        BENTO_ERROR_CHECK_EQ(registry.Lookup(entry.second), entry.first);
        BENTO_ERROR_CHECK_EQ(registry.Name(entry.first), std::string_view(entry.second));
    }
}

void TestTypeRegistryBuiltins() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    BENTO_ERROR_CHECK(registry.Has("bento.StringError"));
    BENTO_ERROR_CHECK(registry.Has("bento.WrappedError"));
    BENTO_ERROR_CHECK(registry.Has("bento.JoinError"));
    BENTO_ERROR_CHECK(registry.Size() >= 3U);

    BENTO_ERROR_CHECK_EQ(registry.ParentName("bento.StringError"),
                         std::string_view("bento.Error"));
    BENTO_ERROR_CHECK_EQ(registry.Depth("bento.StringError"), std::uint8_t{1});
    BENTO_ERROR_CHECK_EQ(registry.ParentName("bento.JoinError"), std::string_view("bento.Error"));

    const ErrorPtr string_error = registry.Create("bento.StringError");
    BENTO_ERROR_CHECK(string_error != nullptr);
    if (string_error) {
        BENTO_ERROR_CHECK_EQ(string_error->TypeName(), std::string_view("bento.StringError"));
        BENTO_ERROR_CHECK_EQ(string_error->Code(), ErrorCode::kUnknown);
        BENTO_ERROR_CHECK(!string_error->IsWrapping());
    }
    const ErrorPtr wrapped = registry.Create("bento.WrappedError");
    BENTO_ERROR_CHECK(wrapped != nullptr);
    if (wrapped) {
        BENTO_ERROR_CHECK(wrapped->IsWrapping());
        BENTO_ERROR_CHECK_EQ(wrapped->Code(), ErrorCode::kWrappedError);
    }
    const ErrorPtr joined = registry.Create("bento.JoinError");
    BENTO_ERROR_CHECK(joined != nullptr);
    if (joined) {
        BENTO_ERROR_CHECK(joined->IsWrapping());
    }
}

void TestTypeRegistryRegisterMacroType() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    registry.ClearLastError();
    BENTO_ERROR_CHECK(registry.Register<LeafCustomError>());
    BENTO_ERROR_CHECK(registry.Has("test.LeafCustomError"));
    BENTO_ERROR_CHECK_EQ(registry.ParentName("test.LeafCustomError"),
                         std::string_view("test.BaseCustomError"));
    BENTO_ERROR_CHECK_EQ(registry.Depth("test.LeafCustomError"), std::uint8_t{2});
    BENTO_ERROR_CHECK_EQ(LeafCustomError::BentoErrorTypeName(),
                         std::string_view("test.LeafCustomError"));
    BENTO_ERROR_CHECK_EQ(LeafCustomError::BentoErrorDepth(), std::uint8_t{2});

    const ErrorPtr created = registry.Create("test.LeafCustomError");
    BENTO_ERROR_CHECK(created != nullptr);
    if (created) {
        BENTO_ERROR_CHECK_EQ(created->Message(), std::string("leaf"));
        BENTO_ERROR_CHECK_EQ(created->TypeName(), std::string_view("test.LeafCustomError"));
        BENTO_ERROR_CHECK_EQ(created->ParentTypeName(),
                             std::string_view("test.BaseCustomError"));
        BENTO_ERROR_CHECK_EQ(created->InheritanceDepth(), std::uint8_t{2});
    }
    BENTO_ERROR_CHECK_EQ(registry.LastError(), 0U);
}

void TestTypeRegistryIdempotent() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    BENTO_ERROR_CHECK(registry.Register<LeafCustomError>());
    const std::size_t size = registry.Size();
    registry.ClearLastError();
    BENTO_ERROR_CHECK(registry.Register<LeafCustomError>());
    BENTO_ERROR_CHECK_EQ(registry.Size(), size);
    BENTO_ERROR_CHECK_EQ(registry.LastError(), 0U);

    BENTO_ERROR_CHECK(
        registry.Register("test.LeafCustomError", "test.BaseCustomError", 2,
                          []() -> ErrorPtr { return std::make_shared<LeafCustomError>(); }));
    BENTO_ERROR_CHECK_EQ(registry.Size(), size);
}

void TestTypeRegistryConflictKeepsFirst() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    BENTO_ERROR_CHECK(registry.Register("test.ConflictType", "bento.Error", 1,
                                        []() -> ErrorPtr { return NewError("first"); }));
    // 同名不同元数据：幂等返回 true 但保留首次注册（不覆盖工厂/父类型/深度）。
    BENTO_ERROR_CHECK(registry.Register("test.ConflictType", "other.Parent", 3,
                                        []() -> ErrorPtr { return NewError("second"); }));
    BENTO_ERROR_CHECK_EQ(registry.ParentName("test.ConflictType"),
                         std::string_view("bento.Error"));
    BENTO_ERROR_CHECK_EQ(registry.Depth("test.ConflictType"), std::uint8_t{1});
    const ErrorPtr created = registry.Create("test.ConflictType");
    BENTO_ERROR_CHECK(created != nullptr);
    if (created) {
        BENTO_ERROR_CHECK_EQ(created->Message(), std::string("first"));
    }
}

void TestTypeRegistryDepthBoundary() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    registry.ClearLastError();
    BENTO_ERROR_CHECK(registry.Register("test.Depth3", "test.BaseCustomError", 3,
                                        []() -> ErrorPtr { return NewError("depth3"); }));
    BENTO_ERROR_CHECK_EQ(registry.Depth("test.Depth3"), std::uint8_t{3});
    BENTO_ERROR_CHECK_EQ(registry.Register("test.Depth4", "test.Depth3", 4,
                                           []() -> ErrorPtr { return NewError("depth4"); }),
                         false);
    BENTO_ERROR_CHECK_EQ(registry.LastError(), kDepthLimitErrorCode);
}

void TestTypeRegistryUnknownType() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    registry.ClearLastError();
    BENTO_ERROR_CHECK(!registry.Has("test.MissingType"));
    BENTO_ERROR_CHECK(registry.Create("test.MissingType") == nullptr);
    BENTO_ERROR_CHECK(registry.ParentName("test.MissingType").empty());
    BENTO_ERROR_CHECK_EQ(registry.Depth("test.MissingType"), std::uint8_t{0});
    BENTO_ERROR_CHECK(registry.LastError() != 0U);
    BENTO_ERROR_CHECK(!registry.LastErrorDetail().empty());
    registry.ClearLastError();
    BENTO_ERROR_CHECK_EQ(registry.LastError(), 0U);
    BENTO_ERROR_CHECK(registry.LastErrorDetail().empty());
}

void TestTypeRegistryRejectsInvalid() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();

    registry.ClearLastError();
    BENTO_ERROR_CHECK(!registry.Register("", "bento.Error", 1,
                                         []() -> ErrorPtr { return NewError("x"); }));
    BENTO_ERROR_CHECK(registry.LastError() != 0U);

    registry.ClearLastError();
    BENTO_ERROR_CHECK(
        !registry.Register("test.NoFactory", "bento.Error", 1, ErrorFactory{}));
    BENTO_ERROR_CHECK(registry.LastError() != 0U);

    registry.ClearLastError();
    BENTO_ERROR_CHECK(!registry.Register("test.NoParent", "", 1,
                                         []() -> ErrorPtr { return NewError("x"); }));
    BENTO_ERROR_CHECK_EQ(registry.LastError(), codes::kConstraint);

    registry.ClearLastError();
    BENTO_ERROR_CHECK(!registry.Register<DeepCustomError>());
    BENTO_ERROR_CHECK(!registry.Has("test.DeepCustomError"));
    BENTO_ERROR_CHECK_EQ(registry.LastError(), kDepthLimitErrorCode);
    BENTO_ERROR_CHECK(!registry.LastErrorDetail().empty());

    registry.ClearLastError();
    BENTO_ERROR_CHECK(registry.Register("test.RootLike", "", 0,
                                        []() -> ErrorPtr { return NewError("root"); }));
    BENTO_ERROR_CHECK_EQ(registry.Depth("test.RootLike"), std::uint8_t{0});
    BENTO_ERROR_CHECK(registry.ParentName("test.RootLike").empty());
}

void TestTypeRegistryFactoryFailures() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    registry.ClearLastError();
    BENTO_ERROR_CHECK(registry.Register("test.NullFactoryResult", "bento.Error", 1,
                                        []() -> ErrorPtr { return {}; }));
    BENTO_ERROR_CHECK(registry.Create("test.NullFactoryResult") == nullptr);
    BENTO_ERROR_CHECK(registry.LastError() != 0U);

    registry.ClearLastError();
    BENTO_ERROR_CHECK(registry.Register("test.ThrowingFactory", "bento.Error", 1,
                                        []() -> ErrorPtr {
                                            throw std::runtime_error("factory boom");
                                        }));
    BENTO_ERROR_CHECK(registry.Create("test.ThrowingFactory") == nullptr);
    BENTO_ERROR_CHECK(registry.LastError() != 0U);

    registry.ClearLastError();
    const std::size_t size = registry.Size();
    registry.RegisterBuiltinErrorTypes();
    BENTO_ERROR_CHECK_EQ(registry.Size(), size);
    BENTO_ERROR_CHECK_EQ(registry.LastError(), 0U);
}

void TestTypeRegistryConcurrentCreate() {
    ErrorTypeRegistry& registry = ErrorTypeRegistry::Instance();
    registry.ClearLastError();
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([thread_index, &registry, &failures]() {
            for (int index = 0; index < kPerThread; ++index) {
                const std::string name = "test.error.type.concurrent." +
                                         std::to_string(thread_index) + "." +
                                         std::to_string(index);
                if (!registry.Register(name, "bento.Error", 1,
                                       []() -> ErrorPtr { return NewError("concurrent"); })) {
                    ++failures;
                }
                if (!registry.Create(name)) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    BENTO_ERROR_CHECK_EQ(failures.load(), 0);
    BENTO_ERROR_CHECK_EQ(registry.LastError(), 0U);
}

// ---------------------------------------------------------------------------
// mapping：HTTP / 日志 / JSON / 策略注入
// ---------------------------------------------------------------------------

std::size_t CountOccurrences(const std::string& text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void TestHttpMapping() {
    struct Expectation {
        ErrorCode code;
        int status;
    };
    const Expectation expectations[] = {
        {ErrorCode::kOk, 200},
        {ErrorCode::kInvalidArgument, 400},
        {ErrorCode::kNotFound, 404},
        {ErrorCode::kAlreadyExists, 409},
        {ErrorCode::kPermissionDenied, 403},
        {ErrorCode::kResourceExhausted, 429},
        {ErrorCode::kFailedPrecondition, 412},
        {ErrorCode::kInternal, 500},
        {ErrorCode::kUnavailable, 503},
        {ErrorCode::kCancelled, 499},
        {ErrorCode::kDeadlineExceeded, 504},
        {ErrorCode::kWrappedError, 500},
    };
    for (const Expectation& expectation : expectations) {
        BENTO_ERROR_CHECK_EQ(MapHttpStatus(expectation.code), expectation.status);
    }
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(ErrorPtr{}), 200);
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(static_cast<ErrorCode>(12345)), 500);
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(NewError("x", ErrorCode::kNotFound)), 404);
}

void TestLogLevelMapping() {
    BENTO_ERROR_CHECK_EQ(DefaultLogLevel(ErrorCode::kOk), LogLevel::kInfo);
    BENTO_ERROR_CHECK_EQ(DefaultLogLevel(ErrorCode::kCancelled), LogLevel::kDebug);
    BENTO_ERROR_CHECK_EQ(DefaultLogLevel(ErrorCode::kDeadlineExceeded), LogLevel::kWarn);
    BENTO_ERROR_CHECK_EQ(DefaultLogLevel(ErrorCode::kInternal), LogLevel::kError);
    BENTO_ERROR_CHECK_EQ(DefaultLogLevel(static_cast<ErrorCode>(12345)), LogLevel::kError);
    BENTO_ERROR_CHECK_EQ(MapLogLevel(ErrorPtr{}), LogLevel::kInfo);
    BENTO_ERROR_CHECK_EQ(MapLogLevel(NewError("x", ErrorCode::kDataLoss)), LogLevel::kError);
    BENTO_ERROR_CHECK_EQ(LogLevelName(LogLevel::kWarn), std::string_view("WARN"));
    BENTO_ERROR_CHECK_EQ(LogLevelName(static_cast<LogLevel>(99)), std::string_view("ERROR"));
}

void TestJsonLeaf() {
    const std::string message = "he said \"hi\"\npath\\x\ttab";
    const ErrorPtr error = NewError(message, ErrorCode::kNotFound);
    const std::string json = MapJson(error);
    BENTO_ERROR_CHECK(json.front() == '{');
    BENTO_ERROR_CHECK(json.back() == '}');
    BENTO_ERROR_CHECK(json.find("\"code\":3") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"code_name\":\"NOT_FOUND\"") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"type\":\"bento.StringError\"") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"parent\":\"bento.Error\"") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"depth\":1") != std::string::npos);
    BENTO_ERROR_CHECK(json.find("\"wrapping\":false") != std::string::npos);
    const std::string escaped = "he said \\\"hi\\\"\\npath\\\\x\\ttab";
    BENTO_ERROR_CHECK(json.find(escaped) != std::string::npos);
    BENTO_ERROR_CHECK(json.find('\n') == std::string::npos);
}

void TestJsonNullAndTree() {
    BENTO_ERROR_CHECK_EQ(MapJson(nullptr), std::string("null"));

    const ErrorPtr root = Wrap(Join({NewError("a"), NewError("b")}), "ctx");
    const std::string json = MapJson(root);
    BENTO_ERROR_CHECK_EQ(CountOccurrences(json, "\"code\":"), std::size_t{4});
    BENTO_ERROR_CHECK_EQ(CountOccurrences(json, "\"causes\":"), std::size_t{2});
    BENTO_ERROR_CHECK(json.find("\"message\":\"ctx: a\\nb\"") != std::string::npos);
    BENTO_ERROR_CHECK_EQ(MapJson(root), DefaultJson(root));
}

void TestJsonCycleAndChildrenError() {
    auto cycle = std::make_shared<CycleError>();
    cycle->SetSelf(cycle);
    const std::string cycle_json = MapJson(cycle);
    BENTO_ERROR_CHECK(cycle_json.find("\"causes\":[null]") != std::string::npos);

    const ErrorPtr thrower = Wrap(std::make_shared<ThrowingUnwrapError>(), "outer");
    const std::string thrower_json = MapJson(thrower);
    BENTO_ERROR_CHECK(thrower_json.find("\"children_error\":true") != std::string::npos);
    BENTO_ERROR_CHECK(thrower_json.find("\"message\":\"outer: thrower\"") !=
                      std::string::npos);
}

void TestJsonDeepTruncated() {
    ErrorPtr deep = NewError("leaf");
    for (int index = 0; index < 100; ++index) {
        deep = Wrap(deep, "w");
    }
    const std::string json = MapJson(deep);
    BENTO_ERROR_CHECK(json.find("\"truncated\":true") != std::string::npos);
    // 截断在 64 层：每层错误消息仍包含剩余链，但总量必须有界。
    BENTO_ERROR_CHECK(json.size() < 40000U);
}

void TestMappingPolicyInjection() {
    MappingPolicy policy;
    policy.http_status = [](ErrorCode /*code*/) { return 418; };
    policy.log_level = [](ErrorCode /*code*/) { return LogLevel::kFatal; };
    policy.json = [](const ErrorPtr& /*error*/) { return std::string("custom"); };

    BENTO_ERROR_CHECK_EQ(MapHttpStatus(ErrorCode::kInternal, policy), 418);
    BENTO_ERROR_CHECK_EQ(MapLogLevel(ErrorCode::kInternal, policy), LogLevel::kFatal);
    BENTO_ERROR_CHECK_EQ(MapJson(NewError("x"), policy), std::string("custom"));

    MappingPolicy partial;
    partial.http_status = policy.http_status;
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(ErrorCode::kInternal, partial), 418);
    BENTO_ERROR_CHECK_EQ(MapLogLevel(ErrorCode::kInternal, partial), LogLevel::kError);
    BENTO_ERROR_CHECK(MapJson(NewError("x"), partial).find("\"code\":1") !=
                      std::string::npos);

    const MappingPolicy& defaults = DefaultMappingPolicy();
    BENTO_ERROR_CHECK_EQ(MapHttpStatus(ErrorCode::kNotFound, defaults), 404);
}

} // namespace

int main() {
    TestStringErrorBasics();
    TestErrorCodes();
    TestNewErrorDefaultCode();
    TestWrapChainMessage();
    TestWrapNullAndEmptyMessage();
    TestJoinBasics();
    TestJoinNestedMessage();
    TestUnwrapLevelsAndFreeFunction();
    TestIsIdentityAndNull();
    TestIsTraversal();
    TestHasCode();
    TestWalkPreOrder();
    TestWalkSkipChildren();
    TestWalkStop();
    TestWalkEmptyAndNullVisitor();
    TestWalkVisitorThrows();
    TestAsAndHasType();
    TestAsPreOrder();
    TestCycleSafety();
    TestThrowingUnwrapIsContained();
    TestDeepChain();
    TestWalkDeepChain();
    TestCycleUnwrapAllDedup();
    TestAsInCycle();
    TestDeepJoinDestruction();
    TestCodeRegistryBuiltins();
    TestCodeRegistryIdempotentAndInvalid();
    TestCodeRegistryViewStability();
    TestCodeRegistryConcurrent();
    TestCodeRegistryBulkRoundTrip();
    TestTypeRegistryBuiltins();
    TestTypeRegistryRegisterMacroType();
    TestTypeRegistryIdempotent();
    TestTypeRegistryConflictKeepsFirst();
    TestTypeRegistryDepthBoundary();
    TestTypeRegistryUnknownType();
    TestTypeRegistryRejectsInvalid();
    TestTypeRegistryFactoryFailures();
    TestTypeRegistryConcurrentCreate();
    TestHttpMapping();
    TestLogLevelMapping();
    TestJsonLeaf();
    TestJsonNullAndTree();
    TestJsonCycleAndChildrenError();
    TestJsonDeepTruncated();
    TestMappingPolicyInjection();
    return bento_error_test::Finish("bento_test_error");
}
