/**
 * @file detail/context_backend.hpp
 * @brief 栈切换汇编原语的 C++ 声明。
 *
 * ## 模块定位
 *
 * 这是 Fiber 最底层的部分：**换栈**。上层的 Fiber 只关心"保存/恢复上下文"，
 * 具体怎么保存寄存器、怎么切 `rsp`，全部由这里的声明背后的汇编实现
 * （`context_x64.S`）负责。
 *
 * 把声明单独放一个 detail 头，是为了让汇编与 C++ 的边界清晰：本文件不含任何
 * 逻辑，只有类型与两个函数签名。
 *
 * ## 两个原语
 *
 * - `MakeContext`：在一段栈上搭出"第一次被 jump 进去就调用 entry"的初始上下文；
 * - `JumpContext`：保存当前上下文并切到目标，返回对方交给我们的数据传输。
 *
 * 非 Linux/x86-64 平台上这两个函数返回空值，让 Fiber 创建**安全失败**而不是
 * 在运行时踩到未实现的入口。
 */
#pragma once

#include <cstddef>

namespace go2cpp::detail {

// 轻量上下文后端的 ABI。Linux x86_64 实现借鉴 coost 使用的
// callee-saved 寄存器布局，但接口名称和数据类型属于 Go2Cpp，避免把
// 第三方符号暴露到公共 ABI。Context 指向执行栈上的保存区；Jump 返回
// 被保存的调用点和调用者传入的数据。栈的 guard page、缓存和生命周期
// 由 FiberStack 管理，不能绕过 Fiber API 直接调用这些函数。
/** @brief 底层 Fiber 上下文句柄，由平台实现持有。 */
using Context = void*;

/**
 * @brief 一次上下文切换携带的输入和返回句柄。
 * @details data 指向调用方拥有的只读数据，切换实现不得延长其生命周期。
 */
struct Transfer {
    Context context{nullptr};
    const void* data{nullptr};
};

/** @brief 新上下文首次恢复时调用的入口函数类型。 */
using Entry = void (*)(Transfer) noexcept;

/**
 * @brief 在用户栈上创建底层上下文。
 * @param stack_bottom 栈内存起始地址。
 * @param stack_size 可用栈空间大小（字节）。
 * @param entry 上下文首次恢复时执行的入口。
 * @return 新上下文句柄；创建失败返回空句柄。
 */
Context MakeContext(void* stack_bottom, std::size_t stack_size,
                    Entry entry) noexcept;
/**
 * @brief 切换到目标上下文。
 * @param target 目标上下文句柄。
 * @param data 传递给目标入口或恢复点的只读数据。
 * @return 从目标上下文返回的上下文和数据。
 */
Transfer JumpContext(Context target, const void* data) noexcept;

}  // namespace go2cpp::detail
