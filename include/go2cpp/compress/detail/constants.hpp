/**
 * @file constants.hpp
 * @brief `compress` 模块的实现常量（缓冲尺寸、窗口位、内存级别）。
 *
 * @details 这些值都是**实现细节**，不属于公共 API：调用方不应该依赖 32 KiB /
 *   64 KiB 这两个数，它们只影响「一次向底层读写多少字节」与吞吐特性。
 *
 *   **窗口位（windowBits）是格式的唯一开关**：zlib 的 `inflateInit2` /
 *   `deflateInit2` 用同一个整数同时表达「格式」与「窗口大小」：
 *   - 负数：raw deflate（无 zlib 头尾，RFC 1951）；
 *   - 正数（8..15）：zlib 格式（RFC 1950，2 字节头 + adler32 尾）；
 *   - 正数 + 16（24..31）：gzip 格式（RFC 1952，10 字节头 + crc32/isize 尾）；
 *   - 正数 + 32：自动检测 zlib/gzip（**本模块不用**，因为调用方已明确选了
 *     `Zlib` 还是 `Gzip`，自动检测会把「格式选错」静默掩盖成「能解」）。
 *
 *   取 15 是最大窗口（32 KiB 历史窗口，压缩率最好），也是 Go 的 compress
 *   系列包与 zlib 默认值；没有理由调小。
 *
 * ## 模块定位
 * 只被 `detail` 层使用，公开头文件不暴露这里的任何名字：
 * - `Inflater` / `Deflater` 的构造函数取缓冲尺寸、memLevel、windowBits；
 * - `ReaderEngine` / `WriterEngine` 用 `kMaxIdleTransfers` 做空转保护，
 *   用 `kMaxChunkSize` 防止 `avail_in` / `avail_out` 的 `uInt` 截断。
 *
 * 因此改这里的值只会影响吞吐与内存，不会改变任何格式语义（windowBits 除外，
 * 它决定格式）。
 */
#pragma once

#include <cstddef>

namespace go2cpp::compress::detail {

/// inflate 输入缓冲：一次向底层 Reader 要多少字节（32 KiB）。
inline constexpr std::size_t kInputBufferSize = 32U * 1024U;

/// deflate 输出缓冲：一次 zlib 调用最多产出多少字节（64 KiB）。
/// 取 64 KiB 是为了让 1 MiB 数据的压缩只需约 16 次底层写。
inline constexpr std::size_t kOutputBufferSize = 64U * 1024U;

/**
 * @brief zlib 的 memLevel。
 *
 * 8 是 zlib 默认值与 Go flate 的取值。改小会降低内存但显著变慢，改大收益很小，
 * 因此固定为默认值——不暴露成参数，避免「能调但没人调」的伪灵活性。
 */
inline constexpr int kMemoryLevel = 8;

/// raw deflate：负数 windowBits 表示「不写 zlib/gzip 头尾」。
inline constexpr int kRawFlateWindowBits = -15;

/// zlib 格式：窗口 15 + zlib 头尾。
inline constexpr int kZlibWindowBits = 15;

/// gzip 格式：窗口 15 + 16 = zlib 走 gzip 封装。
inline constexpr int kGzipWindowBits = 15 + 16;

/**
 * @brief 连续空读 / 空写多少次后判定对端「没有进展」。
 *
 * 与 `streams` / `bufio` 模块保持一致（100 次），避免底层 Reader 永远返回
 * `(0, nil)` 时把解压循环挂死。
 */
inline constexpr int kMaxIdleTransfers = 100;

/**
 * @brief 单次 zlib 调用的最大字节数。
 *
 * zlib 的 `avail_in` / `avail_out` 是 `uInt`（通常是 32 位无符号），
 * 传入更大的 `size_t` 会截断。1 GiB 远大于任何现实中的单次读写块，
 * 同时保证截断不会发生。
 */
inline constexpr std::size_t kMaxChunkSize = 1U << 30;

}  // namespace go2cpp::compress::detail
