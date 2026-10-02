#include "go2cpp/error.hpp"
#include "test_support.hpp"

#include <string>
#include <utility>

namespace {

class TaggedError final : public go2cpp::Error {
public:
    explicit TaggedError(int tag) : m_tag(tag) {}
    std::string Message() const override { return "tagged"; }
    int tag() const noexcept { return m_tag; }

private:
    int m_tag;
};

}  // namespace

/**
 * @brief 验证错误对象、包装链、Is/As 判断和并发访问。
 * @return 无；测试失败由统一断言统计。
 */
void run_error_tests() {
    go2cpp_tests::announce("error values and chains");
    using namespace go2cpp;
    const auto base = NewError("disk unavailable");
    const auto wrapped = Wrap(base, "read config");
    GO2CPP_CHECK(base);
    GO2CPP_CHECK(ErrorMessage(wrapped) == "read config: disk unavailable");
    GO2CPP_CHECK(Is(wrapped, base));
    GO2CPP_CHECK(Is(wrapped, wrapped));
    GO2CPP_CHECK(!Is(wrapped, NewError("disk unavailable")));
    GO2CPP_CHECK(!Unwrap({}));

    const auto tagged = std::make_shared<TaggedError>(7);
    const auto joined = Join({wrapped, tagged});
    GO2CPP_CHECK(ErrorMessage(joined) ==
                 "read config: disk unavailable\ntagged");
    GO2CPP_CHECK(Is(joined, tagged));
    const auto found = As<TaggedError>(joined);
    GO2CPP_CHECK(found && found->tag() == 7);
    const auto singleton_join = Join({base});
    GO2CPP_CHECK(singleton_join &&
                 dynamic_cast<const JoinError*>(singleton_join.get()) !=
                     nullptr);
    GO2CPP_CHECK(Is(singleton_join, base));
    GO2CPP_CHECK(!Join({}));

    // A custom cyclic unwrap graph must not recurse forever.
    class CycleError final : public Error {
    public:
        std::string Message() const override { return "cycle"; }
        ErrorPtr Unwrap() const override { return m_next; }
        ErrorPtr m_next;
    };
    auto cycle = std::make_shared<CycleError>();
    cycle->m_next = cycle;
    GO2CPP_CHECK(!Is(cycle, tagged));
    GO2CPP_CHECK(!As<TaggedError>(cycle));
    cycle->m_next.reset();

    // Identity/type traversal is iterative so a deep generated error chain
    // cannot exhaust the native call stack.
    {
        ErrorPtr deep = base;
        for (int i = 0; i < 100000; ++i) {
            deep = Wrap(std::move(deep), "layer");
        }
        GO2CPP_CHECK(Is(deep, base));
        GO2CPP_CHECK(As<StringError>(deep) == base);
        const auto formatted = ErrorMessage(deep);
        GO2CPP_CHECK(formatted.size() ==
                     100000U * std::string("layer: ").size() +
                         std::string("disk unavailable").size());
        GO2CPP_CHECK(formatted.compare(formatted.size() - 16U, 16U,
                                       "disk unavailable") == 0);
    }

    // Nested Join values use the same iterative formatting and release path.
    {
        ErrorPtr deep_join = base;
        constexpr int join_layers = 100000;
        for (int i = 0; i < join_layers; ++i) {
            deep_join = Join({deep_join, base});
        }
        const auto formatted = ErrorMessage(deep_join);
        const auto line = std::string("disk unavailable");
        GO2CPP_CHECK(formatted.size() ==
                     static_cast<std::size_t>(join_layers + 1) * line.size() +
                         static_cast<std::size_t>(join_layers));
        GO2CPP_CHECK(formatted.compare(formatted.size() - line.size(),
                                       line.size(), line) == 0);
    }
}
