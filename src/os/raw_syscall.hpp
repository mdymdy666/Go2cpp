/**
 * @file raw_syscall.hpp
 * @brief `os` 模块内部使用的**内联汇编 syscall**（不经 libc 的 `syscall()`）。
 *
 * ## 为什么不用 `::syscall(SYS_*)`
 *
 * `::syscall` 是 libc 的**通用变参入口**：它要把 6 个参数从可变参数寄存器
 * 搬运到 syscall 约定的位置、保存/恢复被调用者保存寄存器、再经 PLT 跳到
 * `__libc_syscall`。在微基准（本机 Ryzen 9 8945HX / WSL2）上，这段包装
 * 每次约 **45 ns**：
 *
 * | 调用方式 | `getpid` x2e6 | 8B `read` x5e6 |
 * | --- | ---: | ---: |
 * | `::syscall(SYS_*)` | 151.1 ns/op | 319.2 ns/op |
 * | 内联 `syscall` 指令 | 106.5 ns/op | 211.4 ns/op |
 *
 * 对一个 8 字节 `read`（内核侧约 200~450 ns）来说，45 ns 是 10~20% 的固定税。
 * 本模块的读写热路径全部走这里，把这笔税省掉。
 *
 * ## 与本文件配套的 errno 约定
 *
 * 这里的函数**不写 `errno`**（写 errno 要经 TLS 调 `__errno_location`，
 * 又是一次间接调用）。返回值直接采用内核约定：
 *
 * - `>= 0`：成功值；
 * - `[-4095, -1]`：失败，值为 `-errno`。
 *
 * 调用点自己决定是映射成 `last_error()` 还是补写 `errno`（只有需要把错误
 * 透出给 libc API 的少数位置才补）。
 *
 * ## 平台
 *
 * 只有 x86_64 提供内联版本；其它架构（aarch64/riscv/…）回退到
 * `::syscall`，语义一致（返回 `-errno`，不是 `-1`+errno）。
 */
#pragma once

#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>

namespace go2cpp::os::detail {

#if defined(__x86_64__)

/// 0 参数 syscall；返回 `>=0` 成功，`[-4095,-1]` 为 `-errno`。
inline long Syscall0(long nr) noexcept {
    long ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(nr) : "rcx", "r11", "memory");
    return ret;
}

inline long Syscall1(long nr, long a1) noexcept {
    long ret;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}

inline long Syscall2(long nr, long a1, long a2) noexcept {
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2)
                         : "rcx", "r11", "memory");
    return ret;
}

inline long Syscall3(long nr, long a1, long a2, long a3) noexcept {
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

inline long Syscall4(long nr, long a1, long a2, long a3, long a4) noexcept {
    register long r10 __asm__("r10") = a4;
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                         : "rcx", "r11", "memory");
    return ret;
}

inline long Syscall5(long nr, long a1, long a2, long a3, long a4, long a5) noexcept {
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                         : "rcx", "r11", "memory");
    return ret;
}

inline long Syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) noexcept {
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
    return ret;
}

#else  // 非 x86_64：回退到 libc syscall，但把 -1/errno 归一成 -errno

namespace fallback {
inline long Normalize(long ret) noexcept { return ret < 0 ? -static_cast<long>(errno) : ret; }
}  // namespace fallback

inline long Syscall0(long nr) noexcept { return fallback::Normalize(::syscall(nr)); }
inline long Syscall1(long nr, long a1) noexcept { return fallback::Normalize(::syscall(nr, a1)); }
inline long Syscall2(long nr, long a1, long a2) noexcept {
    return fallback::Normalize(::syscall(nr, a1, a2));
}
inline long Syscall3(long nr, long a1, long a2, long a3) noexcept {
    return fallback::Normalize(::syscall(nr, a1, a2, a3));
}
inline long Syscall4(long nr, long a1, long a2, long a3, long a4) noexcept {
    return fallback::Normalize(::syscall(nr, a1, a2, a3, a4));
}
inline long Syscall5(long nr, long a1, long a2, long a3, long a4, long a5) noexcept {
    return fallback::Normalize(::syscall(nr, a1, a2, a3, a4, a5));
}
inline long Syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) noexcept {
    return fallback::Normalize(::syscall(nr, a1, a2, a3, a4, a5, a6));
}

#endif

/// 内核错误码判定：返回值落在 `[-4095, -1]` 表示失败。
inline bool SyscallFailed(long result) noexcept { return result < 0 && result >= -4095; }

/// 把失败返回值转成 errno（成功值不检查，调用方先判 `SyscallFailed`）。
inline int SyscallErrno(long result) noexcept { return result < 0 ? static_cast<int>(-result) : 0; }

}  // namespace go2cpp::os::detail
