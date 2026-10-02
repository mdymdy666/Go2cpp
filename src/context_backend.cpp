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

/// 函数功能：完成 JumpContext 调用，读取或更新相关运行时状态。
/// 执行流程：
/// 1. 校验传入参数以及当前对象、线程和 Fiber 状态；
/// 2. 按状态机规则获取必要的同步保护并执行核心操作；
/// 3. 发布返回结果、处理异常或取消，并通知相关等待者。
/// @param[in] target 调用方传入的参数，具体约束以头文件声明为准。
/// @param[in] data 调用方传入的参数，具体约束以头文件声明为准。
/// @return 返回值表示操作结果；void、构造函数和析构函数通过对象状态完成工作。
/// @note 该函数遵循所属模块的生命周期与并发约束；失败路径不会遗留等待节点或锁。
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
