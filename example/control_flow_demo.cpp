#include "go2cpp/control_flow.hpp"

#include <iostream>

int main() {
    go2cpp::panic failure;
    go2cpp::recover recovery(failure);
    {
        go2cpp::defer on_scope_exit([&] {
            const auto result = recovery.take();
            if (result.has_value()) {
                std::cout << "recovered: " << result->message
                          << " code=" << result->code << '\n';
            }
        });
        failure.call("demo failure", 7);
        // panic 是显式状态发布；C++ 不会自动跳转，业务代码应在这里
        // return、分支或把状态交给上层。
    }
    std::cout << "active=" << std::boolalpha << failure.active() << '\n';
    return failure.recovered() ? 0 : 1;
}
