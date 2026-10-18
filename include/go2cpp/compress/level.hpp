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
 *   | `HuffmanOnly` | -2 | 只做 Huffman 编码、不做 LZ77 匹配（本模块映射到 `Z_HUFFMAN_ONLY`） |
 *
 *   `HuffmanOnly` 是 Go `compress/flate` 的正式常量（`-2`）。zlib 的
 *   `deflateInit2` 不接受 `level = -2`，因此实现里把它翻译成
 *   「`Z_DEFAULT_COMPRESSION` + `Z_HUFFMAN_ONLY` 策略」，语义与 Go 相同。
 *
 * ## 模块定位
 * 本文件是 `compress` 最底层的常量层，只依赖字面量，被所有格式共享：
 * `Flate` / `Zlib` / `Gzip` 把这里的常量再导出一次（同名 `static constexpr`），
 * `detail::WriterEngine` 的构造与 `Reset` 用 `IsValidLevel` 做入参校验，
 * `detail::Deflater` 用 `HuffmanOnly` 决定是否改走 `Z_HUFFMAN_ONLY` 策略。
 *
 * ## 阅读建议
 * 只需记住「数值就是 zlib `deflateInit2` 的 level」这一件事；随后看
 * `IsValidLevel` 的 `@note`——非法级别是粘性错误而不是静默降级。
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

/// 只做 Huffman 编码、不做 LZ77 匹配（数值 -2，对应 Go `flate.HuffmanOnly`）。
inline constexpr int HuffmanOnly = -2;

/**
 * @brief 级别是否可交给本模块。
 * @param level 待检查的压缩级别。
 * @return `[-2, 9]` 内为 true（`-2` 映射到 zlib 的 Huffman-only 策略）。
 * @note 非法级别不会崩溃，也不会静默降级：`Writer` 会把
 *       `compress::ErrInvalidLevel()` 记为粘性错误，`Write`/`Flush`/`Close`
 *       都原样返回它。这是有意的——静默用默认级别会让「调参没生效」极难发现。
 */
inline constexpr bool IsValidLevel(int level) noexcept {
  return level >= HuffmanOnly && level <= BestCompression;
}

}  // namespace go2cpp::compress
