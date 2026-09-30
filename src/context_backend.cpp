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
