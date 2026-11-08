#pragma once

/**
 * @file scan.hpp
 * @brief 结构字符扫描：AVX2 一次 32 字节找引号/反斜杠/结构字符，标量参考实现用于对照。
 *
 * @details
 * 输出是「结构字符索引」与「字符串结束引号索引」，供 stage2（parser）消费。
 * 字符串内外由引号 + 反斜杠状态机判定；块边界保留状态（字符串可跨块）。
 * 非 AVX2 平台 `ScanAvx2` 返回 false，调用方回退 `ScanScalar`。
 */

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace go2cpp::json::detail {

/// @brief 结构扫描结果。
struct ScanResult {
  std::vector<std::uint32_t> structural;  ///< 字符串外的 `{ } [ ] : ,` 下标（升序）。
  std::vector<std::uint32_t> strings;     ///< 每个字符串的**结束引号**下标（升序）。
  bool ok = true;                         ///< 是否扫描成功。
  std::size_t error_offset = 0;           ///< 第一个错误位置（未闭合/控制字符）。
};

/// @brief 运行期检测 AVX2。
bool HasAvx2() noexcept;

/// @brief AVX2 扫描；编译期或运行期不支持时返回 false（调用方回退标量）。
bool ScanAvx2(std::string_view input, ScanResult& out);

/// @brief 标量参考实现（测试对照、非 AVX2 回退）。
void ScanScalar(std::string_view input, ScanResult& out);

}  // namespace go2cpp::json::detail
