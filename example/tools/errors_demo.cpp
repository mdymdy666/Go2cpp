#include "go2cpp/error.hpp"

#include <iostream>
#include <string>
#include <vector>

/**
 * @brief 演示 go2cpp::errors 命名空间这层 Go 风格薄封装。
 * @details 它与 NewError/Wrap/Join/Is/As 语义完全一致，只是提供更贴近
 *          Go 的拼写；错误对象的不可变与共享所有权约束不变。
 * @return 所有场景按预期完成时返回 0。
 */
int main() {
    using namespace go2cpp;

    // 场景一（成功路径）：构造与包装。
    const auto base = errors::New("disk unavailable");
    const auto wrapped = errors::Wrap(base, "read config");
    const bool wrap_ok = base != nullptr && wrapped != nullptr &&
                         ErrorMessage(wrapped) == "read config: disk unavailable" &&
                         errors::Is(wrapped, base);
    std::cout << "[errors][场景一] message=" << ErrorMessage(wrapped) << '\n';

    // 场景二（身份语义）：消息相同但对象不同时 Is 不匹配。
    const bool identity_ok = !errors::Is(wrapped, errors::New("disk unavailable"));
    std::cout << "[errors][场景二] identity_mismatch=" << identity_ok
              << "（预期 true）\n";

    // 场景三（聚合）：Join 的初始化列表与 vector 重载，空输入返回空。
    const auto joined = errors::Join({wrapped, errors::New("retry later")});
    const auto empty = errors::Join(std::vector<ErrorPtr>{});
    const bool join_ok = joined != nullptr && empty == nullptr &&
                         ErrorMessage(joined) ==
                             "read config: disk unavailable\nretry later";
    std::cout << "[errors][场景三] joined=" << ErrorMessage(joined) << '\n';

    // 场景四（类型提取）：As 命中与未命中，未命中返回空而不是抛异常。
    const auto as_string = errors::As<StringError>(errors::New("boom"));
    const bool as_ok = as_string != nullptr && as_string->Message() == "boom" &&
                       errors::As<WrappedError>(base) == nullptr;
    std::cout << "[errors][场景四] as_string_error=" << (as_string != nullptr)
              << " as_wrapped_on_plain=null -> "
              << (errors::As<WrappedError>(base) == nullptr) << '\n';

    // 场景五（错误/边界路径）：Unwrap 到底返回空，不会死循环。
    const auto deepest = errors::Unwrap(errors::Unwrap(wrapped));
    const bool boundary_ok = deepest == nullptr;
    std::cout << "[errors][场景五] unwrap_to_bottom_is_null=" << boundary_ok
              << "（预期 true）\n";

    const bool all = wrap_ok && identity_ok && join_ok && as_ok && boundary_ok;
    std::cout << "[errors][汇总] all=" << std::boolalpha << all << '\n';
    return all ? 0 : 1;
}
