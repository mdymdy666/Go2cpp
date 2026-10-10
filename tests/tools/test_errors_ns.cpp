#include "go2cpp/error.hpp"

#include "test_support.hpp"

#include <vector>

namespace {

/** @brief errors::New 与 errors::Wrap 与 CamelCase 版本语义一致。 */
void check_new_and_wrap() {
    const auto base = go2cpp::errors::New("disk unavailable");
    GO2CPP_REQUIRE(base != nullptr);
    GO2CPP_CHECK(go2cpp::ErrorMessage(base) == "disk unavailable");

    const auto wrapped = go2cpp::errors::Wrap(base, "read config");
    GO2CPP_REQUIRE(wrapped != nullptr);
    GO2CPP_CHECK(go2cpp::ErrorMessage(wrapped) ==
                 "read config: disk unavailable");
    GO2CPP_CHECK(go2cpp::errors::Is(wrapped, base));
    GO2CPP_CHECK(go2cpp::errors::Unwrap(wrapped) == base);
    GO2CPP_CHECK(go2cpp::errors::Unwrap(base) == nullptr);
}

/** @brief Join 的 vector 与 initializer_list 重载行为一致，空输入返回空。 */
void check_join_overloads() {
    const auto first = go2cpp::errors::New("first");
    const auto second = go2cpp::errors::New("second");

    const auto from_vector =
        go2cpp::errors::Join(std::vector<go2cpp::ErrorPtr>{first, second});
    GO2CPP_REQUIRE(from_vector != nullptr);
    GO2CPP_CHECK(go2cpp::ErrorMessage(from_vector) == "first\nsecond");

    const auto from_list = go2cpp::errors::Join({first, second});
    GO2CPP_REQUIRE(from_list != nullptr);
    GO2CPP_CHECK(go2cpp::ErrorMessage(from_list) == "first\nsecond");

    const auto empty = go2cpp::errors::Join(std::vector<go2cpp::ErrorPtr>{});
    GO2CPP_CHECK(empty == nullptr);
}

/** @brief As 按类型提取；类型不匹配时返回空指针而不是抛异常。 */
void check_as() {
    const auto base = go2cpp::errors::New("boom");
    const auto as_string = go2cpp::errors::As<go2cpp::StringError>(base);
    GO2CPP_REQUIRE(as_string != nullptr);
    GO2CPP_CHECK(as_string->Message() == "boom");
    GO2CPP_CHECK(go2cpp::errors::As<go2cpp::WrappedError>(base) == nullptr);
}

/**
 * @brief 错误身份按对象而非消息判定，薄封装不改变该语义。
 * @details 两条消息相同但对象不同的错误必须互不匹配，否则包装链会失去意义。
 */
/**
 * @brief 空 target 的特殊语义：`Is(error, nullptr)` 等价于「error 是否为空」。
 * @details 源码 `src/tools/error.cpp:392-394` 明确 `if (!target) return !error;`。
 *          头文件 `include/go2cpp/tools/error.hpp:136` 只有裸声明、没有任何注释，
 *          调用方无从得知这条与常规「身份匹配」完全不同的语义（台账 D-14）。
 */
void check_null_target_semantics() {
    const go2cpp::ErrorPtr empty;
    const auto base = go2cpp::errors::New("base");
    GO2CPP_CHECK(go2cpp::errors::Is(empty, nullptr));
    GO2CPP_CHECK(!go2cpp::errors::Is(base, nullptr));
    GO2CPP_CHECK(go2cpp::errors::Is(nullptr, nullptr));
}

void check_identity_semantics() {
    const auto camel = go2cpp::Wrap(go2cpp::NewError("base"), "ctx");
    const auto snake = go2cpp::errors::Wrap(go2cpp::errors::New("base"), "ctx");
    GO2CPP_CHECK(go2cpp::ErrorMessage(camel) == go2cpp::ErrorMessage(snake));
    GO2CPP_CHECK(!go2cpp::errors::Is(snake, go2cpp::errors::New("base")));

    const auto same = go2cpp::errors::New("base");
    const auto chained = go2cpp::errors::Wrap(same, "ctx");
    GO2CPP_CHECK(go2cpp::errors::Is(chained, same));
}

}  // namespace

/**
 * @brief 验证 go2cpp::errors 命名空间这层 Go 风格薄封装。
 * @details 它是用户在途改动新增的 API（`include/go2cpp/tools/error.hpp:204-246`），
 *          此前无任何测试与示例引用（台账 T-05）。按台账默认建议保留并补测。
 * @return 无；失败项由测试支持代码累计。
 */
void run_errors_namespace_tests() {
    go2cpp_tests::announce("go-style errors namespace wrappers");
    check_new_and_wrap();
    check_join_overloads();
    check_as();
    check_identity_semantics();
    check_null_target_semantics();
}