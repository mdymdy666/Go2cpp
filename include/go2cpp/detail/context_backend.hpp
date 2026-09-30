#pragma once

#include <cstddef>

namespace go2cpp::detail {

// 轻量上下文后端的 ABI。Linux x86_64 实现借鉴 coost 使用的
// callee-saved 寄存器布局，但接口名称和数据类型属于 Go2Cpp，避免把
// 第三方符号暴露到公共 ABI。Context 指向执行栈上的保存区；Jump 返回
// 被保存的调用点和调用者传入的数据。栈的 guard page、缓存和生命周期
// 由 FiberStack 管理，不能绕过 Fiber API 直接调用这些函数。
using Context = void*;

struct Transfer {
    Context context{nullptr};
    const void* data{nullptr};
};

using Entry = void (*)(Transfer) noexcept;

Context MakeContext(void* stack_bottom, std::size_t stack_size,
                    Entry entry) noexcept;
Transfer JumpContext(Context target, const void* data) noexcept;

}  // namespace go2cpp::detail
