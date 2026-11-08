/**
 * @file test_error_reflect_slim.cpp
 * @brief 精简版 `go2cpp/error.hpp` 与 `go2cpp/reflect.hpp` 的回归测试。
 *
 * 覆盖：错误工厂/包装/聚合、`Is`、`WalkErrors` 环安全与异常边界、300 层与
 *       20 万层 `Wrap` 链析构、嵌套释放、`Message` 展平格式、`TypeTraits`、
 *       `TypeRegistry` 注册查询（含缓存轮转/名字边界/视图稳定性）、字段表遍历
 *       与同长度字段名过滤。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "go2cpp/tools/error.hpp"
#include "go2cpp/reflect/type.hpp"

namespace {

int g_failures = 0;

/// @brief 断言；失败时打印位置并累计失败数，不中断后续用例。
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, \
                         __LINE__, #cond);                                \
            ++g_failures;                                                 \
        }                                                                 \
    } while (false)

}  // namespace

/// @brief 字段反射用的示例结构体。
struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::string label;
};

template <>
struct go2cpp::reflect::FieldList<Point> {
    static constexpr go2cpp::reflect::TypeId kId = 100;
    static constexpr const char* kName = "Point";
    static const go2cpp::reflect::FieldDescriptor* Fields(std::size_t& count) {
        static const go2cpp::reflect::FieldDescriptor fields[] = {
            EXTENED_REFLECT_FIELD(Point, x),
            EXTENED_REFLECT_FIELD(Point, y),
            EXTENED_REFLECT_FIELD(Point, label),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

/// @brief 两个字段名长度相同，用于验证 `name_size` 过滤不会误判。
struct TwoFields {
    std::int32_t ab = 0;
    std::int32_t ac = 0;
};

template <>
struct go2cpp::reflect::FieldList<TwoFields> {
    static const go2cpp::reflect::FieldDescriptor* Fields(std::size_t& count) {
        static const go2cpp::reflect::FieldDescriptor fields[] = {
            EXTENED_REFLECT_FIELD(TwoFields, ab),
            EXTENED_REFLECT_FIELD(TwoFields, ac),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

/// @brief 覆写 `Is` 的自定义错误：按类型匹配而不是按地址。
class TaggedError final : public go2cpp::Error {
   public:
    std::string Message() const override { return "tagged"; }
    bool Is(const go2cpp::Error& target) const noexcept override {
        return dynamic_cast<const TaggedError*>(&target) != nullptr;
    }
};

/// @brief 非拥有删除器：用于在测试里构造 shared_ptr 环而不泄漏。
struct NoopDeleter {
    void operator()(const go2cpp::Error*) const noexcept {}
};

/// @brief 可被外部把原因指向自己的自定义错误，用来制造环。
class RingError final : public go2cpp::Error {
   public:
    explicit RingError(std::string name) : m_name(std::move(name)) {}
    std::string Message() const override { return m_name; }
    std::vector<go2cpp::ErrorPtr> UnwrapAll() const override { return causes; }

    std::vector<go2cpp::ErrorPtr> causes;

   private:
    std::string m_name;
};

/// @brief `UnwrapAll` 抛异常的自定义错误，用来验证遍历边界不向调用方传播异常。
class ThrowingUnwrapError final : public go2cpp::Error {
   public:
    std::string Message() const override { return "throwing"; }
    std::vector<go2cpp::ErrorPtr> UnwrapAll() const override {
        throw std::runtime_error("unwrap boom");
    }
};

namespace {

int g_counted_destroyed = 0;

/// @brief 析构计数叶子，用于验证嵌套释放确实把整张图释放干净。
class CountedError final : public go2cpp::Error {
   public:
    ~CountedError() override { ++g_counted_destroyed; }
    std::string Message() const override { return "counted"; }
};

/// @brief 析构时再释放另一条链：触发迭代工作表的嵌套路径。
class NestedReleaseError final : public go2cpp::Error {
   public:
    explicit NestedReleaseError(go2cpp::ErrorPtr nested) : m_nested(std::move(nested)) {}
    ~NestedReleaseError() override { m_nested.reset(); }
    std::string Message() const override { return "nested"; }

   private:
    go2cpp::ErrorPtr m_nested;
};

}  // namespace

namespace {

void TestErrorBasics() {
    const go2cpp::ErrorPtr leaf = extened::NewError("boom");
    CHECK(go2cpp::ErrorMessage(leaf) == "boom");
    CHECK(leaf->ErrorString() == "boom");
    CHECK(extened::Unwrap(leaf) == nullptr);
    CHECK(extened::UnwrapAll(leaf).empty());

    const go2cpp::ErrorPtr inner = extened::Wrap(leaf, "read config");
    CHECK(go2cpp::ErrorMessage(inner) == "read config: boom");
    CHECK(extened::Unwrap(inner) == leaf);
    CHECK(extened::UnwrapAll(inner).size() == 1);

    const go2cpp::ErrorPtr outer = extened::Wrap(inner, "load");
    CHECK(go2cpp::ErrorMessage(outer) == "load: read config: boom");
    CHECK(extened::Wrap(go2cpp::ErrorPtr{}, "ignored") == nullptr);

    const go2cpp::ErrorPtr joined = extened::Join({extened::NewError("a"), extened::NewError("b")});
    CHECK(go2cpp::ErrorMessage(joined) == "a\nb");
    CHECK(extened::UnwrapAll(joined).size() == 2);
    CHECK(extened::Join(std::vector<go2cpp::ErrorPtr>{}) == nullptr);
    CHECK(extened::Join({go2cpp::ErrorPtr{}, go2cpp::ErrorPtr{}}) == nullptr);

    const go2cpp::ErrorPtr nested = extened::Join({joined, extened::NewError("c")});
    CHECK(go2cpp::ErrorMessage(nested) == "a\nb\nc");
}

void TestMessageFormatting() {
    // 空描述层被忽略，不产生多余的 ": "。
    const go2cpp::ErrorPtr blank = extened::Wrap(extened::Wrap(extened::NewError("leaf"), ""), "outer");
    CHECK(go2cpp::ErrorMessage(blank) == "outer: leaf");

    // 包装 Join：Join 的换行文本作为叶子整体追加。
    const go2cpp::ErrorPtr joined = extened::Join({extened::NewError("x"), extened::NewError("y")});
    CHECK(go2cpp::ErrorMessage(extened::Wrap(joined, "wrap")) == "wrap: x\ny");

    // 自定义错误作为叶子，走虚接口。
    CHECK(go2cpp::ErrorMessage(extened::Wrap(std::make_shared<TaggedError>(), "w")) == "w: tagged");

    // Join 内嵌 Join 只展开子原因，不输出额外分隔。
    const go2cpp::ErrorPtr nested =
        extened::Join({extened::Join({extened::NewError("a")}), extened::NewError("b")});
    CHECK(go2cpp::ErrorMessage(nested) == "a\nb");
}

void TestIs() {
    const go2cpp::ErrorPtr cause = extened::NewError("cause");
    const go2cpp::ErrorPtr wrapped = extened::Wrap(cause, "outer");
    CHECK(extened::Is(wrapped, cause));   // 包装后仍能匹配到原因
    CHECK(extened::Is(cause, cause));     // 叶子匹配自身
    CHECK(!extened::Is(cause, extened::NewError("other")));
    CHECK(extened::Is(go2cpp::ErrorPtr{}, go2cpp::ErrorPtr{}));
    CHECK(!extened::Is(cause, go2cpp::ErrorPtr{}));
    CHECK(!extened::Is(go2cpp::ErrorPtr{}, cause));
    CHECK(extened::Is(wrapped, wrapped));  // 根节点自身

    const go2cpp::ErrorPtr tagged = std::make_shared<TaggedError>();
    CHECK(extened::Is(tagged, std::make_shared<TaggedError>()));
    CHECK(!extened::Is(tagged, cause));

    // Join 图里的深层原因也能匹配。
    const go2cpp::ErrorPtr graph =
        extened::Join({extened::Wrap(cause, "a"), extened::NewError("b")});
    CHECK(extened::Is(graph, cause));
}

void TestWalkErrors() {
    const go2cpp::ErrorPtr wrapped = extened::Wrap(extened::NewError("leaf"), "wrap");
    std::size_t visited = 0;
    CHECK(extened::WalkErrors(wrapped, [&visited](const go2cpp::ErrorPtr&) {
        ++visited;
        return go2cpp::ErrorVisitResult::kContinue;
    }));
    CHECK(visited == 2);

    visited = 0;
    CHECK(!extened::WalkErrors(wrapped, [&visited](const go2cpp::ErrorPtr&) {
        ++visited;
        return go2cpp::ErrorVisitResult::kStop;
    }));
    CHECK(visited == 1);

    CHECK(!extened::WalkErrors(wrapped, go2cpp::ErrorVisitor{}));
    CHECK(extened::WalkErrors(go2cpp::ErrorPtr{}, [](const go2cpp::ErrorPtr&) {
        return go2cpp::ErrorVisitResult::kContinue;
    }));
}

void TestWalkErrorsMixedGraph() {
    // 菱形：两条 WrappedError 共享同一个 cause，cause 只能访问一次。
    const go2cpp::ErrorPtr shared = extened::NewError("shared");
    const go2cpp::ErrorPtr left = extened::Wrap(shared, "a");
    const go2cpp::ErrorPtr right = extened::Wrap(shared, "b");
    const go2cpp::ErrorPtr diamond = extened::Join({left, right});

    std::size_t visited = 0;
    CHECK(extened::WalkErrors(diamond, [&visited](const go2cpp::ErrorPtr&) {
        ++visited;
        return go2cpp::ErrorVisitResult::kContinue;
    }));
    CHECK(visited == 4);  // diamond + left + right + shared
    CHECK(go2cpp::ErrorMessage(diamond) == "a: shared\nb: shared");

    // 嵌套 Join：内层 Join 自身也是节点。
    const go2cpp::ErrorPtr nested = extened::Join(
        {extened::Join({extened::NewError("x"), extened::NewError("y")}), extened::NewError("z")});
    visited = 0;
    CHECK(extened::WalkErrors(nested, [&visited](const go2cpp::ErrorPtr&) {
        ++visited;
        return go2cpp::ErrorVisitResult::kContinue;
    }));
    CHECK(visited == 5);  // nested + 内层 Join + x + y + z

    // 自定义错误（只覆写 UnwrapAll）走通用路径。
    RingError lone("lone");
    visited = 0;
    CHECK(extened::WalkErrors(go2cpp::ErrorPtr(&lone, NoopDeleter{}),
                             [&visited](const go2cpp::ErrorPtr&) {
                                 ++visited;
                                 return go2cpp::ErrorVisitResult::kContinue;
                             }));
    CHECK(visited == 1);
}

void TestWalkErrorsExceptionBoundary() {
    // 访问器抛异常：边界按失败返回，不向调用方传播。
    const go2cpp::ErrorPtr leaf = extened::NewError("leaf");
    std::size_t visited = 0;
    const bool ok = extened::WalkErrors(leaf, [&visited](const go2cpp::ErrorPtr&)
                                           -> go2cpp::ErrorVisitResult {
        ++visited;
        throw std::runtime_error("visitor boom");
    });
    CHECK(!ok);
    CHECK(visited == 1);

    // 自定义 UnwrapAll 抛异常：同样按失败返回。
    const go2cpp::ErrorPtr throwing = std::make_shared<ThrowingUnwrapError>();
    CHECK(!extened::WalkErrors(throwing, [](const go2cpp::ErrorPtr&) {
        return go2cpp::ErrorVisitResult::kContinue;
    }));

    // 包装 + 抛异常：先访问包装层，再在取原因时失败。
    visited = 0;
    const go2cpp::ErrorPtr wrapped_throwing = extened::Wrap(throwing, "outer");
    CHECK(!extened::WalkErrors(wrapped_throwing, [&visited](const go2cpp::ErrorPtr&) {
        ++visited;
        return go2cpp::ErrorVisitResult::kContinue;
    }));
    CHECK(visited == 2);
}

void TestWalkErrorsRing() {
    // 环：first.causes -> second，second.causes -> first；两个指针都不拥有对象。
    RingError first("first");
    RingError second("second");
    first.causes.push_back(go2cpp::ErrorPtr(&second, NoopDeleter{}));
    second.causes.push_back(go2cpp::ErrorPtr(&first, NoopDeleter{}));
    const go2cpp::ErrorPtr ring(&first, NoopDeleter{});

    std::size_t visited = 0;
    CHECK(extened::WalkErrors(ring, [&visited](const go2cpp::ErrorPtr&) {
        ++visited;
        return go2cpp::ErrorVisitResult::kContinue;
    }));
    CHECK(visited == 2);  // 每个节点只访问一次，不死循环
    CHECK(extened::Is(ring, go2cpp::ErrorPtr(&second, NoopDeleter{})));
}

void TestDeepWrapDestruction() {
    // 规格要求：300 层 Wrap 链析构不爆栈。
    go2cpp::ErrorPtr chain = extened::NewError("leaf");
    for (int i = 0; i < 300; ++i) {
        chain = extened::Wrap(std::move(chain), "layer");
    }
    const std::string message = go2cpp::ErrorMessage(chain);
    CHECK(message.size() == 300u * 7u + 4u);
    CHECK(message.compare(0, 7, "layer: ") == 0);
    CHECK(message.compare(message.size() - 4, 4, "leaf") == 0);
    chain.reset();

    // 加强项：20 万层递归析构必然爆栈，只有迭代工作表能通过。
    go2cpp::ErrorPtr deep = extened::NewError("deep-leaf");
    for (int i = 0; i < 200000; ++i) {
        deep = extened::Wrap(std::move(deep), "d");
    }
    CHECK(go2cpp::ErrorMessage(deep).size() == 200000u * 3u + 9u);
    deep.reset();
}

void TestNestedRelease() {
    // 外层链释放到一半时，某个节点的析构再释放一条内层链：
    // 内层的 cause 必须回推到外层工作表，而不是就地递归。
    g_counted_destroyed = 0;
    go2cpp::ErrorPtr inner = std::make_shared<CountedError>();
    for (int i = 0; i < 3; ++i) {
        inner = extened::Wrap(std::move(inner), "inner");
    }
    go2cpp::ErrorPtr outer = std::make_shared<NestedReleaseError>(std::move(inner));
    for (int i = 0; i < 5; ++i) {
        outer = extened::Wrap(std::move(outer), "outer");
    }
    outer.reset();
    CHECK(g_counted_destroyed == 1);
}

void TestTypeTraits() {
    using go2cpp::reflect::TypeIdOf;
    CHECK(go2cpp::reflect::TypeTraits<bool>::kId == go2cpp::reflect::kBoolType);
    CHECK(go2cpp::reflect::TypeTraits<int>::kId == go2cpp::reflect::kInt32Type);
    CHECK(go2cpp::reflect::TypeTraits<std::int64_t>::kId == go2cpp::reflect::kInt64Type);
    CHECK(go2cpp::reflect::TypeTraits<unsigned int>::kId == go2cpp::reflect::kUint32Type);
    CHECK(go2cpp::reflect::TypeTraits<std::uint64_t>::kId == go2cpp::reflect::kUint64Type);
    CHECK(go2cpp::reflect::TypeTraits<double>::kId == go2cpp::reflect::kDoubleType);
    CHECK(go2cpp::reflect::TypeTraits<std::string>::kId == go2cpp::reflect::kStringType);
    CHECK(go2cpp::reflect::TypeTraits<std::vector<std::uint8_t>>::kId ==
          go2cpp::reflect::kBytesType);

    CHECK(TypeIdOf<Point>() == 100);
    CHECK(TypeIdOf<int>() == go2cpp::reflect::kInt32Type);
    CHECK(std::string_view(go2cpp::reflect::TypeNameOf<Point>()) == "Point");
    CHECK(std::string_view(go2cpp::reflect::TypeName<Point>()) == "Point");
    CHECK(std::string_view(go2cpp::reflect::TypeNameOf<long double>()) == "unknown");
    CHECK(go2cpp::reflect::IsBuiltinType<int>());
    CHECK(!go2cpp::reflect::IsBuiltinType<Point>());
}

void TestTypeRegistry() {
    auto& registry = go2cpp::reflect::TypeRegistry::Instance();
    const go2cpp::reflect::TypeId id = registry.Register("Point", sizeof(Point));
    CHECK(id >= go2cpp::reflect::kFirstDynamicType);
    CHECK(registry.Register("Point", sizeof(Point)) == id);  // 同名幂等
    CHECK(registry.Contains(id));
    CHECK(registry.Contains(go2cpp::reflect::kStringType));
    CHECK(!registry.Contains(go2cpp::reflect::kUnknownType));

    go2cpp::reflect::TypeId found = go2cpp::reflect::kUnknownType;
    CHECK(registry.Find("Point", found));
    CHECK(found == id);
    CHECK(!registry.Find("Missing", found));

    std::string_view name;
    CHECK(registry.Name(id, name));
    CHECK(name == "Point");
    CHECK(!registry.Name(go2cpp::reflect::kUnknownType, name));

    CHECK(registry.Register("", 8) == go2cpp::reflect::kUnknownType);
    CHECK(registry.Register("bad", 0) == go2cpp::reflect::kUnknownType);
    CHECK(registry.Size() >= 9);  // 8 个内置 + Point
}

void TestRegistryCacheEdges() {
    auto& registry = go2cpp::reflect::TypeRegistry::Instance();
    // 超过缓存槽数的名字集合：轮转替换后逐个查询仍必须正确。
    constexpr int kNames = 40;
    for (int index = 0; index < kNames; ++index) {
        const std::string name = "cache.edge." + std::to_string(index);
        CHECK(registry.Register(name, 8) >= go2cpp::reflect::kFirstDynamicType);
    }
    for (int round = 0; round < 2; ++round) {
        for (int index = 0; index < kNames; ++index) {
            const std::string name = "cache.edge." + std::to_string(index);
            go2cpp::reflect::TypeId found = go2cpp::reflect::kUnknownType;
            CHECK(registry.Find(name, found));
            CHECK(registry.Contains(found));
            std::string_view stored;
            CHECK(registry.Name(found, stored));
            CHECK(stored == name);
        }
    }

    // 同名幂等：重复注册不覆盖 size、不分配新 id。
    const std::string repeated = "cache.edge.3";
    const go2cpp::reflect::TypeId id = registry.Register(repeated, 8);
    CHECK(registry.Register(repeated, 999) == id);
    go2cpp::reflect::TypeId repeated_id = go2cpp::reflect::kUnknownType;
    CHECK(registry.Find(repeated, repeated_id));
    CHECK(repeated_id == id);
}

void TestRegisterNameBounds() {
    auto& registry = go2cpp::reflect::TypeRegistry::Instance();
    const std::string max_name(go2cpp::reflect::kMaxTypeNameBytes, 'n');
    const go2cpp::reflect::TypeId id = registry.Register(max_name, 8);
    CHECK(id != go2cpp::reflect::kUnknownType);
    go2cpp::reflect::TypeId found = go2cpp::reflect::kUnknownType;
    CHECK(registry.Find(max_name, found));
    CHECK(found == id);

    const std::string too_long(go2cpp::reflect::kMaxTypeNameBytes + 1, 'n');
    CHECK(registry.Register(too_long, 8) == go2cpp::reflect::kUnknownType);
    CHECK(!registry.Find(too_long, found));
    CHECK(!registry.Find("", found));
}

void TestNameViewStability() {
    auto& registry = go2cpp::reflect::TypeRegistry::Instance();
    const go2cpp::reflect::TypeId id = registry.Register("stable.name", sizeof(int));
    std::string_view view;
    CHECK(registry.Name(id, view));
    const std::string expected(view);
    // 注册足够多的名字触发 rehash：key 节点的引用/指针不应失效。
    for (int index = 0; index < 200; ++index) {
        (void)registry.Register("rehash." + std::to_string(index), 8);
    }
    CHECK(view == expected);
    CHECK(view == "stable.name");
}

void TestFieldReflection() {
    std::size_t count = 0;
    const go2cpp::reflect::FieldDescriptor* fields = go2cpp::reflect::FieldList<Point>::Fields(count);
    CHECK(count == 3);
    CHECK(std::string_view(fields[0].name, fields[0].name_size) == "x");
    CHECK(std::string_view(fields[1].name, fields[1].name_size) == "y");
    CHECK(std::string_view(fields[2].name, fields[2].name_size) == "label");
    CHECK(fields[0].offset == offsetof(Point, x));
    CHECK(fields[1].offset == offsetof(Point, y));
    CHECK(fields[2].offset == offsetof(Point, label));
    CHECK(fields[0].type_id == go2cpp::reflect::kInt32Type);
    CHECK(fields[2].type_id == go2cpp::reflect::kStringType);
    CHECK(fields[2].size == sizeof(std::string));

    CHECK(go2cpp::reflect::FieldCount<Point>() == 3);
    CHECK(go2cpp::reflect::FieldCount<int>() == 0);
    CHECK(go2cpp::reflect::HasFields<Point>());
    CHECK(!go2cpp::reflect::HasFields<int>());

    std::size_t index = 0;
    const go2cpp::reflect::FieldDescriptor* label =
        go2cpp::reflect::FindField<Point>("label", &index);
    CHECK(label != nullptr);
    CHECK(index == 2);
    CHECK(label->offset == offsetof(Point, label));
    CHECK(go2cpp::reflect::FindField<Point>("missing") == nullptr);

    // 字段表偏移与真实对象布局一致。
    Point point{};
    const auto real_offset = static_cast<std::size_t>(
        reinterpret_cast<const char*>(&point.label) - reinterpret_cast<const char*>(&point));
    CHECK(real_offset == go2cpp::reflect::FieldOffset(&Point::label));
}

void TestFieldNameLengthFilter() {
    // 长度相同、内容不同：不能因 name_size 相同就误判命中。
    std::size_t index = 99;
    const go2cpp::reflect::FieldDescriptor* ac =
        go2cpp::reflect::FindField<TwoFields>("ac", &index);
    CHECK(ac != nullptr);
    CHECK(index == 1);
    CHECK(ac->offset == offsetof(TwoFields, ac));

    index = 99;
    const go2cpp::reflect::FieldDescriptor* ab =
        go2cpp::reflect::FindField<TwoFields>("ab", &index);
    CHECK(ab != nullptr);
    CHECK(index == 0);

    // 前缀相同但长度不同：必须走长度过滤后判为不命中。
    CHECK(go2cpp::reflect::FindField<TwoFields>("a") == nullptr);
    CHECK(go2cpp::reflect::FindField<TwoFields>("abc") == nullptr);
    CHECK(go2cpp::reflect::FindField<TwoFields>("") == nullptr);
}

}  // namespace

int main() {
    TestErrorBasics();
    TestMessageFormatting();
    TestIs();
    TestWalkErrors();
    TestWalkErrorsMixedGraph();
    TestWalkErrorsExceptionBoundary();
    TestWalkErrorsRing();
    TestDeepWrapDestruction();
    TestNestedRelease();
    TestTypeTraits();
    TestTypeRegistry();
    TestRegistryCacheEdges();
    TestRegisterNameBounds();
    TestNameViewStability();
    TestFieldReflection();
    TestFieldNameLengthFilter();

    if (g_failures != 0) {
        std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::puts("ALL TESTS PASSED");
    return 0;
}
