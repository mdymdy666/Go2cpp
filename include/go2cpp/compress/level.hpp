/**
 * @file level.hpp
 * @brief 压缩级别常量（对齐 Go `compress/flate` 的 `DefaultCompression` 等）。
 *
 * @details 这些数值**不是自定义的**：它们就是 zlib `deflateInit2` 接受的
 *   `level` 取值，Go 的 `compress/flate`、`compress/zlib`、`compress/gzip`
 *   也定义了同一组常量且数值一致：
 *
 *   | 常量 | 值 | zlib 行为 |
 *   | --- | --- | --- |
 *   | `NoCompression` | 0 | 只做存储块（stored），用于「先占位、后补压」或测速基线 |
 *   | `BestSpeed` | 1 | 最快，压缩率最低 |
 *   | `BestCompression` | 9 | 最慢，压缩率最高 |
 *   | `DefaultCompression` | -1 | 交给 zlib 选（等价于级别 6） |
 *
 *   Go 还有 `HuffmanOnly`（-2）：只做 Huffman 编码、不做 LZ77 匹配。本模块
 *   **不提供**——任务未要求，且它在本项目没有真实使用场景；`IsValidLevel`
 *   因此把它判为非法。如果以后需要，加一个常量并把区间放宽到 -2 即可，
 *   不需要改任何实现代码（zlib 原生支持 -2）。
 */
#pragma once

namespace go2cpp::compress {

/// 不压缩，只打包（zlib 级别 0）。
inline constexpr int NoCompression = 0;

/// 最快速度、最低压缩率（zlib 级别 1）。
inline constexpr int BestSpeed = 1;

/// 最高压缩率、最慢速度（zlib 级别 9）。
inline constexpr int BestCompression = 9;

/// 默认级别，由 zlib 自行选择（数值 -1，实际等价于级别 6）。
inline constexpr int DefaultCompression = -1;

/**
 * @brief 级别是否可交给 zlib。
 * @param level 待检查的压缩级别。
 * @return `[-1, 9]` 内为 true。
 * @note 非法级别不会崩溃，也不会静默降级：`Writer` 会把
 *       `compress::ErrInvalidLevel()` 记为粘性错误，`Write`/`Flush`/`Close`
 *       都原样返回它。这是有意的——静默用默认级别会让「调参没生效」极难发现。
 */
inline constexpr bool IsValidLevel(int level) noexcept {
  return level >= DefaultCompression && level <= BestCompression;
}

}  // namespace go2cpp::compress
