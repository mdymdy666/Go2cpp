/**
 * @file context_backend_win32.cpp
 * @brief Windows 的栈切换后端——用 **原生 Fiber API**（`CreateFiber`/`SwitchToFiber`）。
 *
 * ## 为什么用 Fiber API 而不是移植汇编
 * Linux 的 `context_x64.S` 手工保存 callee-saved 寄存器 + 切 `rsp`；
 * Windows x64 的调用约定、TEB、SEH 链都与 SysV 不同，**手写汇编风险高**。
 * `CreateFiber`/`SwitchToFiber` 是 Windows 官方的用户态协程实现，
 * **语义完全对应**「保存/恢复执行上下文」，由内核/运行时保证正确性。
 *
 * ## 与 Linux 实现的契约差异（**重要**）
 * | 项 | Linux（context_x64.S） | Windows（Fiber API） |
 * |---|---|---|
 * | 栈 | **调用方提供**（`stack_bottom`/`stack_size`，FiberStack 管） | **`CreateFiber` 自己分配**（`stack_bottom` 参数被忽略） |
 * | 栈大小 | 调用方精确控制 | `CreateFiber` 按 `dwStackSize` 向上取整（默认保留页 + 提交页） |
 * | 栈 guard | FiberStack 管 | 系统管（`PAGE_GUARD`） |
 * | 上下文句柄 | 栈上保存区指针 | **Fiber 数据块指针**（`LPVOID`） |
 * | 首次入口 | 汇编布置（返回地址 + 参数） | `CreateFiber` 的 `lpStartAddress`（`void(*)(void*)`） |
 * | 传 data | `JumpContext(target, data)` 直接传 | **TLS 中转**（`SwitchToFiber` 无参数） |
 *
 * ## 关键实现细节
 * 1. **当前线程必须先 `ConvertThreadToFiber`**——否则 `SwitchToFiber` 失败；
 *    本文件在首次 `JumpContext` 时懒初始化（`thread_local` 标记）。
 * 2. `Transfer.context` 用 `GetCurrentFiber()`（与 `CreateFiber` 返回的句柄同类型）。
 * 3. TLS 中转 `g_payload`：`JumpContext` 先写「我要给对方的」→ `SwitchToFiber`
 *    → 被切回后读「对方给我的」（对方已覆盖）。
 * 4. **fiber 入口返回后不能直接 `return`**——必须 `SwitchToFiber` 回主 fiber，
 *    否则线程直接退出。本实现让 `entry` 返回后切回「调度 fiber」（由上层保证
 *    `entry` 永不返回，这里只是防御）。
 *
 * ## 待验证
 * 本文件**无法在 Linux 上编译验证**（`windows.h`）——需 Windows 实机跑
 * `tests/concurrency/test_fiber.cpp` 确认。
 */
#if defined(_WIN32)

#include "go2cpp/concurrency/detail/context_backend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace go2cpp::detail {
namespace {

/// @brief 每个线程的「当前切换载荷」：JumpContext 前写「给对方」，切回后读「对方给的」。
thread_local Transfer g_payload{nullptr, nullptr};

/// @brief 当前线程是否已 ConvertThreadToFiber（每线程只能转一次）。
thread_local bool g_thread_is_fiber = false;

/// @brief 创建上下文时打包给 trampoline 的参数。
struct EntryContext {
  Entry entry;
  Transfer initial;
};

/// @brief `CreateFiber` 的入口 trampoline：转成 `Entry(Transfer)`。
void CALLBACK FiberTrampoline(void* param) {
  auto* ec = static_cast<EntryContext*>(param);
  if (ec != nullptr && ec->entry != nullptr) {
    ec->entry(ec->initial);
  }
  // 正常情况 entry 永不返回（上层用 SwitchToFiber 切走）；若返回了，
  // 说明上层逻辑异常——切回 g_payload.context（上一次的调用者）。
  if (g_payload.context != nullptr) {
    ::SwitchToFiber(g_payload.context);
  }
  // 无人可切：让线程结束（避免 UB）
  ::ExitThread(0);
}

/// @brief 懒初始化：把当前线程转成 fiber。
bool EnsureThreadIsFiber() noexcept {
  if (g_thread_is_fiber) {
    return true;
  }
  if (::ConvertThreadToFiber(nullptr) == nullptr) {
    // 已转换过（ERROR_ALREADY_FIBER）也算成功
    if (::GetLastError() == ERROR_ALREADY_FIBER) {
      g_thread_is_fiber = true;
      return true;
    }
    return false;
  }
  g_thread_is_fiber = true;
  return true;
}

}  // namespace

Context MakeContext(void* /*stack_bottom*/, std::size_t stack_size,
                    Entry entry) noexcept {
  if (entry == nullptr) {
    return nullptr;
  }
  auto* ec = new (std::nothrow) EntryContext{entry, {nullptr, nullptr}};
  if (ec == nullptr) {
    return nullptr;
  }
  // 注意：CreateFiber 自己分配栈；stack_bottom/stack_size 仅作提示。
  const SIZE_T size = stack_size == 0 ? 0 : static_cast<SIZE_T>(stack_size);
  void* fiber = ::CreateFiber(size, &FiberTrampoline, ec);
  if (fiber == nullptr) {
    delete ec;
    return nullptr;
  }
  return fiber;
}

Transfer JumpContext(Context target, const void* data) noexcept {
  if (target == nullptr || !EnsureThreadIsFiber()) {
    return {nullptr, nullptr};
  }
  // 把「我要给对方的」放进 TLS：对方被切回来时会读到（并覆盖）。
  g_payload.context = ::GetCurrentFiber();
  g_payload.data = data;
  ::SwitchToFiber(target);
  // 被切回：g_payload 已被对方覆盖为「对方给我的」
  return g_payload;
}

}  // namespace go2cpp::detail

#endif  // defined(_WIN32)
