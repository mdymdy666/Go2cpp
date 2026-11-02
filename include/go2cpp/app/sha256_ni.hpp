#pragma once

/**
 * @file sha256_ni.hpp
 * @brief 资源校验用 SHA-256 的兼容入口：实现已下沉到 `go2cpp::hash::Sha256`。
 *
 * @details 2026-11-02 统一：本文件原先内嵌一份 SHA-NI 压缩函数（约 430 行），
 *          与 `src/hash/sha256.cpp` 的标量实现、`logging/redact` 的内嵌实现
 *          三份并存。现在硬件加速与运行时 CPUID 分派已下沉到 `hash` 模块
 *          （`src/hash/sha256.cpp`），三处共用同一实现、同一输出。
 *
 *          保留本头文件只为不打断 `src/app/resource.cpp` 的调用点：
 *          `Sha256Fast` 是 `go2cpp::hash::Sha256` 的薄包装（接口保持不变，
 *          内部整块批量压缩，1 MiB 校验实测约 2.2 GB/s）；新代码请直接使用
 *          `go2cpp::hash::Sha256`。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>

#include "go2cpp/hash/hash.hpp"
#include "go2cpp/hash/sha256.hpp"

namespace go2cpp::app {

/**
 * @brief SHA-256 增量摘要（硬件优先，软件回退）。
 *
 * @details 接口刻意与 `go2cpp::hash::Sha256` 的用法一致（`Write` / `Sum`），
 *          `Sum` 直接写裸缓冲，避免每次摘要一次 `std::vector` 分配。
 *          `Write` 接受任意长度：内部按 64B 块压缩，尾部留在成员缓冲。
 */
class Sha256Fast {
   public:
    /// @brief 摘要长度。
    static constexpr std::size_t kSize = go2cpp::hash::Sha256::kSize;
    /// @brief 压缩块大小。
    static constexpr std::size_t kBlockSize = go2cpp::hash::Sha256::kBlockSize;

    /// @brief 当前 CPU 是否支持 SHA-NI（进程内只做一次 CPUID）。
    static bool HardwareAvailable() noexcept {
        return go2cpp::hash::Sha256::HardwareAccelerated();
    }

    /// @brief 复位到空输入状态。
    void Reset() noexcept { m_hash.Reset(); }

    /// @brief 追加字节（无需对齐）。
    void Write(const std::uint8_t* data, std::size_t size) noexcept {
        if (size == 0 || data == nullptr) {
            return;
        }
        m_hash.Write(go2cpp::hash::AsBytes(data, size));
    }

    /// @brief 写摘要（32 字节）；不改变自身状态。
    void Sum(std::uint8_t* out) const noexcept {
        if (out == nullptr) {
            return;
        }
        m_hash.Sum(go2cpp::hash::AsBytes(out, kSize));
    }

   private:
    go2cpp::hash::Sha256 m_hash;  ///< 统一实现（内部按 CPUID 选 SHA-NI / 标量）。
};

}  // namespace go2cpp::app
