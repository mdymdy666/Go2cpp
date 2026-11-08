#pragma once

/**
 * @file escape.hpp
 * @brief JSON 字符串的 `\uXXXX` 转义解码、UTF-8 编码与反向转义。
 *
 * ## 背景：为什么这是一次「规范修正」
 * 早期版本的解析器把 `\u` 序列**原样保留**在字符串里：输入 `"\u00e9"` 读出来
 * 是 6 个 ASCII 字节 `\`、`u`、`0`、`0`、`e`、`9`，而不是 UTF-8 的 `é`
 * （`0xC3 0xA9`）。RFC 8259 §7 要求 `\uXXXX` 表示一个 Unicode 码元，解析器
 * 必须解码；把转义留着不处理会让「解析 → 比较 → 输出」三步全部偏离规范。
 *
 * 本模块把 `\u` 处理抽成独立单元（解析器与 writer 共用同一份实现），并明确
 * 支持 UTF-16 代理对：
 *
 * ```
 * "\uD83D\uDE00"  →  U+1F600  →  UTF-8  F0 9F 98 80
 * ```
 *
 * 因此 `tests/test_json_parser.cpp` 里期望「`\u` 原样保留」的断言已改成期望
 * 解码后的 UTF-8；这是**规范修正**，不是行为回归。
 *
 * ## 错误口径
 * 以下情况一律判为非法（返回 false 并写 `EscapeError`）：
 * - hex 位出现非 `[0-9A-Fa-f]` 字符；
 * - hex 位数不足 4 位；
 * - 孤立高代理（后面不是 `\uDC00`..`\uDFFF`）、孤立低代理；
 * - 组合结果超出 Unicode 范围（> U+10FFFF）或落在代理区。
 *
 * 解析器把这些失败映射为 `JsonFailure::kInvalidEscape`（残缺序列映射为
 * `kUnclosedString`，与原有错误位置口径一致）。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::json {

/// @brief `\uXXXX` 解码失败的具体原因；与错误文本分离，便于程序化分支。
enum class EscapeFailure : std::uint8_t {
  kNone = 0,               ///< 无错误。
  kBadHex,                 ///< hex 位出现非十六进制字符。
  kShortInput,             ///< hex 位数不足 4 位。
  kLoneSurrogate,          ///< 孤立代理项：低代理单独出现，或高代理后没有低代理。
  kCodePointOutOfRange,    ///< 组合结果超出 U+10FFFF，或码点落在代理区。
};

/// @brief 转义解码失败的详情。
struct EscapeError {
  EscapeFailure failure = EscapeFailure::kNone;  ///< 失败原因。
  std::size_t offset = 0;                        ///< 出错字节在传入 hex 视图中的下标。
};

/// @brief 一个码点最多编码成 4 个 UTF-8 字节。
constexpr std::size_t kMaxUtf8Length = 4;

/**
 * @brief 十六进制数字面值。
 * @param value [in] 待判定字节。
 * @return `0`..`15`；非十六进制字符返回 `-1`。
 */
inline int HexDigit(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

/// @brief 是否为 UTF-16 高代理（U+D800..U+DBFF）。
inline bool IsHighSurrogate(std::uint32_t code) noexcept {
  return code >= 0xD800u && code <= 0xDBFFu;
}

/// @brief 是否为 UTF-16 低代理（U+DC00..U+DFFF）。
inline bool IsLowSurrogate(std::uint32_t code) noexcept {
  return code >= 0xDC00u && code <= 0xDFFFu;
}

/**
 * @brief 把 Unicode 码点编码成 UTF-8，写入调用方提供的 4 字节缓冲。
 *
 * @param code_point [in] 码点；合法范围 `[0, 0x10FFFF]` 且不能落在代理区
 *        `[0xD800, 0xDFFF]`（代理项不是独立字符，必须成对组合）。
 * @param out [out] 至少 4 字节的可写缓冲；失败时不写入。
 * @return 写入的字节数 `1`..`4`；**非法码点返回 0**——调用方必须据此报错，
 *         不得把 0 当成空字符串继续。
 *
 * @note 这是解析器热路径唯一的 UTF-8 编码入口：无分配、无分支预测惩罚
 *       （码点分布通常是 ASCII 或 2–3 字节）。`std::string` 版本
 *       `AppendUtf8` 只是对它的封装。
 */
inline std::size_t EncodeUtf8(std::uint32_t code_point, char* out) noexcept {
  if (code_point <= 0x7Fu) {
    out[0] = static_cast<char>(code_point);
    return 1;
  }
  if (code_point <= 0x7FFu) {
    out[0] = static_cast<char>(0xC0u | (code_point >> 6));
    out[1] = static_cast<char>(0x80u | (code_point & 0x3Fu));
    return 2;
  }
  if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
    return 0;  // 代理项不能单独编码。
  }
  if (code_point <= 0xFFFFu) {
    out[0] = static_cast<char>(0xE0u | (code_point >> 12));
    out[1] = static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | (code_point & 0x3Fu));
    return 3;
  }
  if (code_point > 0x10FFFFu) {
    return 0;  // Unicode 上限。
  }
  out[0] = static_cast<char>(0xF0u | (code_point >> 18));
  out[1] = static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu));
  out[2] = static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
  out[3] = static_cast<char>(0x80u | (code_point & 0x3Fu));
  return 4;
}

/**
 * @brief 解析 4 位十六进制码元（`\uXXXX` 的 `XXXX` 部分）。
 *
 * @param hex4 [in] 十六进制字符视图；只读取前 4 位，长度可以大于 4。
 * @param code_point [out] 成功时写入 `0`..`0xFFFF` 的 UTF-16 码元；失败时保持原值。
 * @return 成功 true；位数不足或含非十六进制字符 false。
 *
 * @note 本函数**不做**代理对组合：返回值可能是高/低代理，语义上只是码元。
 *       完整序列解码请用 `DecodeUnicodeEscapeSequence`。
 */
bool DecodeUnicodeEscape(std::string_view hex4, std::uint32_t* code_point);

/**
 * @brief `DecodeUnicodeEscape` 的带错误详情版本。
 *
 * @param hex4 [in] 十六进制字符视图；只读取前 4 位。
 * @param code_point [out] 成功时写入码元；失败时**保持原值**。
 * @param error [out] 失败时写入原因与出错下标；成功时写入 `kNone`。不可为 nullptr。
 * @return 成功 true；位数不足（`kShortInput`）或含非十六进制字符（`kBadHex`）false。
 */
bool DecodeUnicodeEscape(std::string_view hex4, std::uint32_t* code_point,
                         EscapeError* error);

/**
 * @brief 解码完整的 `\u` 序列 hex 部分：4 位单码元，或 8 位代理对。
 *
 * @param hex [in] 去掉 `\u` 前缀后的十六进制视图：
 *        - 长度 ≥4：按第一个码元解码；若它是高代理，则要求再跟 4 位低代理
 *          （即长度 ≥8，例如 `"D83DDE00"`）；多出来的字符忽略。
 *        - 长度 <4：`kShortInput`。
 * @param code_point [out] 成功时写入 `0`..`0x10FFFF` 的完整码点（代理对已合成）；
 *        失败时保持原值。
 * @param error [out] 失败时写入原因；成功时 `kNone`。不可为 nullptr。
 * @return 成功 true。
 *
 * ## 定位
 * 这是 `\u` 语义的**唯一权威实现**：解析器、测试与未来的 builder 都通过它
 * 判断「一个转义序列是否合法」。若这里放宽校验（例如偷偷接受孤立代理），
 * 非法 JSON 会被静默接受，写出端再编码时就会产生非法 UTF-8。
 *
 * ## 步骤
 * step1. 校验长度 ≥4，解码第一个码元；非 hex 立即失败。
 * step2. 非代理：直接返回（含普通 BMP 字符）。
 * step3. 低代理：没有前置高代理，按 `kLoneSurrogate` 失败。
 * step4. 高代理：要求后面还有 4 位低代理，合成 `0x10000 + (高-0xD800)<<10 + (低-0xDC00)`。
 * step5. 组合结果必然是合法码点；仍做一次上限检查兜底。
 *
 * ## 并发/失败语义
 * - 纯函数，无共享状态，可被任意线程同时调用；
 * - 不分配、不抛异常；失败时只写 `error`，`code_point` 保持原值。
 */
bool DecodeUnicodeEscapeSequence(std::string_view hex, std::uint32_t* code_point,
                                 EscapeError* error);

/**
 * @brief 把一对 UTF-16 代理码元合成一个 Unicode 码点。
 *
 * @param high [in] 高代理，必须落在 `[0xD800, 0xDBFF]`。
 * @param low [in] 低代理，必须落在 `[0xDC00, 0xDFFF]`。
 * @param code_point [out] 成功时写入 `[0x10000, 0x10FFFF]`；失败时保持原值。
 * @param error [out] 失败时写入 `kLoneSurrogate` / `kCodePointOutOfRange`；成功时 `kNone`。
 *        不可为 nullptr。
 * @return 成功 true。
 */
bool CombineSurrogates(std::uint32_t high, std::uint32_t low, std::uint32_t* code_point,
                       EscapeError* error);

/**
 * @brief 把码点以 UTF-8 追加到字符串末尾。
 *
 * @param code_point [in] 码点；非法（代理区 / > U+10FFFF）时**不写入任何字节**。
 * @param out [in,out] 目标字符串，不可为 nullptr；容量由调用方保证（本函数
 *        只做 `push_back`，不 reserve）。
 * @return 无返回值；调用方若需判断是否写入，请先用 `EncodeUtf8`。
 *
 * @note 保留 `void` 签名是为了与任务约定一致；需要错误反馈时用 `EncodeUtf8`。
 */
void AppendUtf8(std::uint32_t code_point, std::string* out);

/**
 * @brief 把码点编码成 JSON `\uXXXX` 转义（writer 用）。
 *
 * @param code_point [in] 码点；非法（代理区 / > U+10FFFF）返回空字符串。
 * @return BMP 码点返回 6 字节 `\uXXXX`；补充平面返回 12 字节
 *         `\uD8xx\uDCxx`（大写 hex）。空字符串表示码点非法，调用方应先校验。
 */
std::string EncodeUnicodeEscape(std::uint32_t code_point);

}  // namespace go2cpp::json
