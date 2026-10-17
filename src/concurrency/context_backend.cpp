/**
 * @file context_backend.cpp
 * @brief 栈切换汇编原语的 C++ 桥接层。
 *
 * ## 模块定位
 *
 * Fiber 的挂起/恢复本质上是**换栈**：保存当前寄存器与栈指针，加载目标的
 * 寄存器与栈指针。这一步必须用汇编写（`context_x64.S`），本文件只做
 * C++ 侧的转发与平台判断。
 *
 * ## 两个原语
 *
 * - `go2cpp_context_make(stack_bottom, stack_size, entry)`：在一段栈上
 *   搭出"第一次被 jump 进去就会调用 entry"的初始上下文（按 x86-64 SysV
 *   调用约定布置好返回地址与参数寄存器）；
 * - `go2cpp_context_jump(target, data)`：保存当前上下文并切到 `target`，
 *   把 `data` 作为 `entry` 的参数传过去；被切回时返回一个
 *   `Transfer`（携带对方交给我们的数据）。
 *
 * ## 平台策略
 *
 * 只在 Linux + x86-64 上启用汇编实现；其它平台返回空上下文/空传输，
 * 让上层的 Fiber 创建安全失败，而不是在运行时踩到未实现的入口。
 * 这样移植时只需要补一份汇编，不用改 Fiber 逻辑。
 */
#include "go2cpp/detail/context_backend.hpp"

#if defined(__linux__) && defined(__x86_64__)
extern "C" {
void* go2cpp_context_make(void* stack_bottom, std::size_t stack_size,
                          go2cpp::detail::Entry entry) noexcept;
go2cpp::detail::Transfer go2cpp_context_jump(
    void* context, const void* data) noexcept;
}
#endif

namespace go2cpp::detail {

Context MakeContext(void* stack_bottom, std::size_t stack_size,
                    Entry entry) noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return go2cpp_context_make(stack_bottom, stack_size, entry);
#else
    (void)stack_bottom;
    (void)stack_size;
    (void)entry;
    return nullptr;
#endif
}

Transfer JumpContext(Context target, const void* data) noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return go2cpp_context_jump(target, data);
#else
    (void)target;
    (void)data;
    return {};
#endif
}

}  // namespace go2cpp::detail
