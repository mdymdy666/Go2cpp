/**
 * @file limits.hpp
 * @brief `bufio` 模块的缓冲区尺寸常量。
 *
 * @details 数值直接对齐 Go 标准库（Go 1.23 `src/bufio/bufio.go`）：
 *   - `defaultBufSize = 4096`
 *   - `minReadBufferSize = 16`
 *   - `maxConsecutiveEmptyReads = 100`（与项目 `streams` 模块一致）
 *   - `MaxScanTokenSize = 64 * 1024`
 *
 *   **为什么保留 4096 而不是更大**：Go 选 4096 是在「一次系统调用的开销」与
 *   「每个 Reader 常驻内存」之间折中；`bufio` 的收益来自**减少调用次数**，
 *   而不是把缓冲做大。测试里对 512 / 4096 / 65536 三档做了对比（见压测输出）。
 */
#pragma once

#include <cstddef>

namespace go2cpp::bufio {

/// 默认缓冲区大小（字节），对齐 Go 的 `defaultBufSize`。
inline constexpr std::size_t kDefaultBufferSize = 4096;

/// 缓冲区允许的最小尺寸；`NewReader/NewWriter(size)` 小于此值时提升到 16（Go 同）。
inline constexpr std::size_t kMinBufferSize = 16;

/// 允许的「连续空读」次数上限；超过即判定底层 Reader 违约（Go/本项目 streams 同）。
inline constexpr std::size_t kMaxConsecutiveEmptyReads = 100;

/// `Scanner` 单个 token 的默认上限，对齐 Go 的 `MaxScanTokenSize`。
inline constexpr std::size_t kMaxScanTokenSize = 64U * 1024U;

namespace detail {

/// 把调用方给的尺寸钳制到合法区间；`0` 不是合法缓冲区。
inline constexpr std::size_t ClampBufferSize(std::size_t size) noexcept {
    return size < kMinBufferSize ? kMinBufferSize : size;
}

}  // namespace detail

}  // namespace go2cpp::bufio
