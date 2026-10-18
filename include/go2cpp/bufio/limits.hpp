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
 *
 * ## 模块定位
 *
 * `bufio` 的调参面：`Reader`/`Writer`/`Scanner` 的构造默认值与扩容上限都取自
 * 这里，`detail::ClampBufferSize` 是它们共用的入口校验。改动任何一个常量都会
 * 同时改变三个类的默认行为，因此不要按单个调用方的需要来调。
 *
 * ## 本文件定义什么
 *
 * 四个 `inline constexpr` 常量（默认缓冲、最小缓冲、连续空读上限、单 token 上限）
 * 与一个尺寸钳制函数，全部 header-only、无链接依赖。
 *
 * ## 阅读建议
 *
 * 先看 `kDefaultBufferSize` 为何是 4096，再看 `ClampBufferSize`——它解释了
 * 为什么传入 0 不会得到 0 字节缓冲，而是被提升到 16。
 *
 * ## 边界与陷阱
 *
 * `kMaxScanTokenSize` 只约束 `Scanner`，`Reader::ReadString`/`ReadLine` 不受它
 * 限制（`ReadLine` 的上限是缓冲容量）；`kMaxConsecutiveEmptyReads` 同时被
 * `Reader::fill`、`Scanner::ReadMore`、`Writer::Flush`/`ReadFrom` 使用，含义是
 * 「连续无进展」而非「读失败」。
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
