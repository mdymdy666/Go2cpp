#include "go2cpp/runtime.hpp"

#include <chrono>
#include <iostream>
#include <string>

/**
 * @brief Context 简单版：取消、超时、携带值三件最常用的事。
 * @details ① WithCancel —— 取消父级会级联到子级；
 *          ② WithTimeout —— 到点自动取消，Err() 是 DeadlineExceededError()；
 *          ③ WithValue —— 值沿父链查找，子级可以覆盖父级。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;
    bool ok = true;

    // ① 取消级联：取消父级，子级随之完成
    std::clog << "[context][场景1] WithCancel：父取消级联到子\n";
    const auto root = Background();
    auto parent_pair = WithCancel(root);
    const auto child_pair = WithCancel(parent_pair.first);

    std::cout << "  取消前 child->IsDone()=" << child_pair.first->IsDone() << "（预期 0）\n";
    parent_pair.second();                     // 取消父级
    ok = ok && child_pair.first->IsDone();
    ok = ok && Is(child_pair.first->Err(), CanceledError());
    std::cout << "  取消后 child->IsDone()=" << child_pair.first->IsDone()
              << " Err 是 CanceledError=" << Is(child_pair.first->Err(), CanceledError())
              << "（预期 1 1）\n";

    // ② 超时：不需要任何人调用 cancel，到点自动完成
    std::clog << "[context][场景2] WithTimeout：到点自动取消\n";
    auto timeout_pair = WithTimeout(root, std::chrono::milliseconds(20));
    const bool done = timeout_pair.first->Done()
                          .WaitFor(std::chrono::milliseconds(500));
    ok = ok && done;
    ok = ok && Is(timeout_pair.first->Err(), DeadlineExceededError());
    std::cout << "  到期=" << done
              << " Err 是 DeadlineExceededError="
              << Is(timeout_pair.first->Err(), DeadlineExceededError())
              << "（预期 1 1）\n";

    // ③ 携带值：类型化键沿父链查找，子级可覆盖
    std::clog << "[context][场景3] WithValue：沿父链查找\n";
    const ContextKey<std::string> kUser("user");
    const auto with_user = WithValue(root, kUser, std::string("alice"));
    const auto overridden = WithValue(with_user, kUser, std::string("bob"));

    const auto from_parent = with_user->Value(kUser);
    const auto from_child = overridden->Value(kUser);
    const auto missing = overridden->Value(ContextKey<int>("absent"));
    ok = ok && from_parent.has_value() && *from_parent == "alice";
    ok = ok && from_child.has_value() && *from_child == "bob";
    ok = ok && !missing.has_value();
    std::cout << "  父级读到 " << (from_parent ? *from_parent : "<无>")
              << "，子级读到 " << (from_child ? *from_child : "<无>")
              << "，未设置的键为空=" << !missing.has_value() << '\n';

    std::clog << (ok ? "[context] 全部通过\n" : "[context] 有失败\n");
    return ok ? 0 : 1;
}
