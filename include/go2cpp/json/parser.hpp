#pragma once

/**
 * @file parser.hpp
 * @brief 迭代式 JSON 解析器：`bgo2cpp::json::BasicParser<Policy>`。
 *
 * ## 为什么迭代
 * JSON 的嵌套深度由输入决定，递归下降解析在深嵌套（攻击输入可轻松做到
 * 10 万层）时会耗尽原生栈。这里把「当前打开了哪些容器、容器处于什么语法
 * 位置」放进显式栈（`Policy::Stack<Frame>`），主循环逐 token 推进，原生栈
 * 深度与输入深度无关。解析结果写进 `Document` 的扁平池，`Document` 析构
 * 只是释放几个 `std::vector`，同样没有递归——「解析 → 使用 → 销毁」整条
 * 链路都不会因深嵌套爆栈。
 *
 * ## Policy 与 Config
 * - **Policy**（编译期）：帧栈容器。`InlinePolicy`（默认）/`CompactPolicy`
 *   用 `KInlineStack` 内联；`StdPolicy` 用 `std::vector`。
 * - **ParserConfig**（运行期）：`max_depth`、`allow_trailing_comma`、
 *   `timeout_ms`、`duplicate_key`、`max_string_bytes`/`validate_utf8`（token 上限）、
 *   `error_policy`/`max_errors`（错误恢复）。
 *
 * ## 零拷贝
 * 无转义的字符串直接把 `Text::Borrow` 指向输入缓冲；只有出现转义（含 `\u`）
 * 时才解码进 `Document::strings_` 池。因此 **输入缓冲必须比结果 `Document`
 * 活得久**。
 *
 * ## 错误
 * 失败返回 false 并写入 `ErrorPtr`（`JsonError`，带 offset/line/column）；
 * 失败时 `doc` 被清空（`Root().IsNull()`），`error` 先被清空。
 *
 * ## 错误恢复（场景 5）
 * `ParserConfig::error_policy` 为 `kSkipValue` / `kCollect` 时，容器内某个值坏掉
 * 不会中止整次解析：坏值位置填 `null`，兄弟成员/元素照常保留，错误按策略收进
 * `Document::Errors()`。实现上默认 `kAbort` 与恢复路径在**编译期**分开
 * （`ParseInternalBody<kChecks, kRecovering>`）：默认实例里恢复边界的记录/清除、
 * 失败分支的恢复尝试全部被 `if constexpr` 消除，生成的代码与加该功能前一致。
 * 恢复路径的细节与不可恢复清单见 `RecoverValue`。
 *
 * ## 行列口径
 * - `\n`、`\r` 都算换行；`\r\n` 只算一个换行；
 * - 列号从 1 开始，按字节计（UTF-8 多字节字符算多列）；
 * - 空白只认 `' ' '\t' '\n' '\r'` 四个字节，不用 `isspace`（后者受 locale 影响）。
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "go2cpp/json/core/text.hpp"
#include "go2cpp/tools/error.hpp"
#include "go2cpp/json/config.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/error.hpp"
#include "go2cpp/json/value.hpp"

namespace go2cpp::json {
namespace detail {

/// @brief 给编译器一个「此分支几乎不发生」的提示（恢复分派等冷分支用）。
inline bool Unlikely(bool value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_expect(value, 0);
#else
  return value;
#endif
}

/// @brief 容器帧的语法位置。
enum class ContainerState : std::uint8_t {
  kArrayFirst,        ///< `[` 之后：允许 `]` 或第一个值。
  kArrayValue,        ///< `,` 之后：必须有值（尾逗号由配置决定是否允许）。
  kArrayAfterValue,   ///< 值之后：`]` 或 `,`。
  kObjectFirst,       ///< `{` 之后：允许 `}` 或第一个键。
  kObjectKey,         ///< `,` 之后：必须有键（尾逗号由配置决定是否允许）。
  kObjectColon,       ///< 键之后：期望 `:`。
  kObjectValue,       ///< `:` 之后：必须有值。
  kObjectAfterValue,  ///< 值之后：`}` 或 `,`。
};

/**
 * @brief 解析栈的一帧：当前打开的容器节点 + 子表起点 + 语法位置 + 待配对键。
 *
 * @note 不持有 `Value`（只持有节点池下标），帧本身不参与值析构；节点池扩容
 *       不影响下标。`begin` 是该容器在 `members_` / `elements_` 里的子表起点，
 *       闭合时用「当前池大小 − begin」回填 `Span`。
 */
struct Frame {
  std::uint32_t node_index = 0;  ///< 容器节点在节点池中的下标。
  std::uint32_t scratch_begin = 0;  ///< 直接子项在 scratch 中的起点（对象 spill 后 / 数组）。
  std::uint32_t member_begin = 0;   ///< 对象：直接写入模式下成员区段在成员池的起点。
  std::uint32_t direct_count = 0;   ///< 对象：本容器已写入的直接成员数。
  std::string_view key;          ///< 对象中已读到的键，等待值。
  std::size_t key_offset = 0;    ///< 键开引号的 0 起字节偏移（重复键报错定位）。
  ContainerState state = ContainerState::kArrayFirst;  ///< 语法位置。
  bool is_object = false;        ///< true=对象帧，false=数组帧。
  bool spilled = false;          ///< 对象：成员区段被插入打断，已切换到 scratch 收集。
};

/// @brief 对象成员提交结果（`BasicParser::SubmitObjectMember` 返回值）。
enum class MemberSubmitResult : std::uint8_t {
  kAppended,  ///< 已追加为新成员（`kFirst` 下重复项也走这里，保持历史行为）。
  kMerged,    ///< `kLast`：已把同 key 已有成员的值改写为最后一个值，位置不变。
  kFailed,    ///< `kError`：已写入错误，调用方必须立即返回 false。
};

/**
 * @brief 一个「可恢复值」的恢复边界（`ErrorPolicy::kSkipValue` / `kCollect` 专用）。
 *
 * @details 解析器在读到值的开头时记录一次，值成功完成（标量提交 / 容器闭合）
 *          后清除。失败时凭它把帧栈、两级 scratch 与游标一起回退到值开始前，
 *          再跳过整棵坏值。边界始终指向**当前正在解析的值**：坏值在哪个成员/
 *          元素位置，就在那里填 `null`，兄弟成员/元素完全不受影响——与
 *          `docs/JSON_ENGINE_DESIGN.md` §3.2 的「括号深度计数、不解析内容」一致。
 *
 * @note `active == false` 时解析器完全走历史路径（`kAbort` 下恒为 false），
 *       因此默认策略的代码路径不增加任何判断分支之外的开销。
 */
struct RecoveryPoint {
  bool active = false;             ///< 当前是否处于某个值的解析范围内。
  std::size_t depth = 0;           ///< 值的父容器栈深度（弹帧目标 = 回到这一层）。
  std::size_t value_offset = 0;    ///< 值首个字节的 0 起偏移（同步扫描起点）。
  std::size_t member_before = 0;   ///< 值开始前的成员 scratch 深度（回滚目标）。
  std::size_t element_before = 0;  ///< 值开始前的元素 scratch 深度（回滚目标）。
  std::string_view key;            ///< 对象父容器中该值的成员名（借用）。
  std::size_t key_offset = 0;      ///< 成员名开引号的偏移（`kError` 重复键定位）。
  bool container_started = false;  ///< 值本身是容器且已压帧（决定何时清除边界）。
};

/// @brief `BasicParser::RecoverValue` 的结果。
enum class RecoverResult : std::uint8_t {
  kContinued,  ///< 已恢复：父容器仍打开，主循环继续。
  kRootClosed, ///< 已恢复且父容器是根——根值已完成，主循环进入尾随空白检查。
  kFailed,     ///< 不可恢复：调用方必须 `return false`（`error` 已就绪）。
};

/**
 * @brief `kAbort` 实例里的恢复边界替身（1 B）。
 *
 * @details `ParseInternalBody<kChecks, false>` 用 `std::conditional_t` 把它当成
 *          恢复边界类型：默认路径因此不会为 56 B 的 `RecoveryPoint` 预留栈槽位
 *          （实测这份栈帧会挤掉热路径寄存器，默认解析慢 1~2 ns）。恢复实例仍然
 *          使用真正的 `RecoveryPoint`。
 */
struct RecoveryStub {};

/**
 * @brief 该失败原因是否允许「跳过坏值」继续解析。
 * @param failure [in] 失败原因。
 * @return `true` = 可以尝试从值起点同步并继续；`false` = 必须立即中止。
 *
 * @details 不可恢复的五类，各自的中止理由：
 * - `kTimeout` / `kTooLarge`：全局预算已耗尽，继续解析只会更糟；
 * - `kDepthExceeded`：防爆栈防线被触发，不能用「跳过」绕过它；
 * - `kDuplicateKey`：`DuplicateKeyPolicy::kError` 的业务级失败，不该被吞掉；
 * - `kTrailingGarbage`：根值已经解析完，没有「值」可恢复。
 *
 * 其余（标量语法错误、容器内部的尾逗号/缺值/非法字符等）都按括号深度跳过：
 * 它们只影响坏值本身，兄弟成员/元素仍然是独立可用的数据。
 */
inline bool IsRecoverableFailure(JsonFailure failure) noexcept {
  switch (failure) {
    case JsonFailure::kTimeout:
    case JsonFailure::kTooLarge:
    case JsonFailure::kDepthExceeded:
    case JsonFailure::kDuplicateKey:
    case JsonFailure::kTrailingGarbage:
      return false;
    default:
      return true;
  }
}

namespace scanner_detail {

/// @brief 是否为 JSON 空白（只认 4 个字节）。
inline bool IsWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

/// @brief 是否为十进制数字。
inline bool IsDigit(char value) noexcept { return value >= '0' && value <= '9'; }

/**
 * @brief 一次把 8 个 ASCII 十进制数字转成数值（SWAR）。
 * @param chunk [in] 从数字串读出的 8 字节；按小端解释——低字节是最靠前的字符。
 * @return 0..99999999；块内任一字节不是 `'0'`..`'9'` 时返回 `~std::uint64_t{0}`。
 *
 * @details 为什么值得单独写：数字串里小数位普遍在 10 位以上（金额、坐标、
 *          时间戳小数），逐位「读-判-累加」每位约 6 条指令且带一条 2 周期的
 *          依赖链；这里用两道按字节并行的位运算校验 + 三级 BCD 合并，把 8 位
 *          压到 20 条指令左右，指令数与依赖链同时缩短。
 *
 * 校验为什么可以并行：先要求高半字节全等于 0x3（把字节限制在 0x30..0x3F），
 * 再要求低半字节 +6 不产生 0x10 进位（把 `':'`..`'?'` 挡掉）。两次判断都按
 * 字节独立——低半字节最大 15，+6 最多 21，不会跨字节进位。
 *
 * @note 依赖小端字节序；本项目目标是 x86-64 / AArch64 小端。
 */
inline std::uint64_t EightDigits(std::uint64_t chunk) noexcept {
  constexpr std::uint64_t kHigh = 0xF0F0F0F0F0F0F0F0ULL;
  constexpr std::uint64_t kLow = 0x0F0F0F0F0F0F0F0FULL;
  const std::uint64_t low = chunk & kLow;
  if ((((chunk & kHigh) ^ 0x3030303030303030ULL) |
       ((low + 0x0606060606060606ULL) & kHigh)) != 0) {
    return ~std::uint64_t{0};
  }
  // 三级合并：字节对 → 2 位/16 bit 段，再 → 4 位/32 bit 段，最后 → 8 位。
  // 每级只在段内乘 10/100/10000；段内最大值离段宽（16/32 bit）还很远，所以
  // 段与段之间不会互相溢出，不需要额外掩码来清理进位。
  const std::uint64_t pair = (low * 10 + (low >> 8)) & 0x00FF00FF00FF00FFULL;
  const std::uint64_t quad =
      (pair & 0x0000FFFF0000FFFFULL) * 100 + ((pair >> 16) & 0x0000FFFF0000FFFFULL);
  return (quad & 0xFFFFFFFFULL) * 10000 + (quad >> 32);
}

/// @brief 检测 8 字节块中是否存在 0 字节（SWAR）。
inline std::uint64_t HasZeroByte(std::uint64_t value) noexcept {
  return (value - 0x0101010101010101ULL) & ~value & 0x8080808080808080ULL;
}

/// @brief 检测 8 字节块中是否存在小于 0x20 的字节（ASCII 前提下的 SWAR）。
inline std::uint64_t HasLessThanControl(std::uint64_t value) noexcept {
  return (value - 0x2020202020202020ULL) & ~value & 0x8080808080808080ULL;
}

/**
 * @brief `ScanPlainRun` 的尾部：先用 8 字节 SWAR 块，最后逐字节收尾。
 * @param cursor [in] 起点；必须满足 `cursor <= end`。
 * @param end [in] 输入终点（不含）。
 * @return 第一个事件字节的位置；没有事件时返回 `end`。
 *
 * @note 只在剩余不足 16 字节时被调用（SSE2 路径），或整段作为非 SSE2 平台的
 *       完整实现。8 字节 SWAR 的借位传播可能让掩码在真事件**之后**多置位，
 *       但最低置位一定是真事件，所以 `ctz` 取到的位置始终正确；即便出现
 *       偏差，调用方也只会看到「不是闭引号」而回退慢速状态机，结果不受影响。
 *
 * @note 为什么 SSE2 路径下 `noinline`：`ScanPlainRun` 会内联到
 *       `Scanner::ParseString` / `ParseStringView`，而这两个又内联进
 *       `TryParseFlatObjectBody`。若尾部与本函数共存于同一内联体，热函数体积
 *       膨胀到超出 GCC 的内联阈值，`ParseString` 会被**整个**踢出内联（实测
 *       退化为独立函数 + 6 个 callee-saved 寄存器保存，每解析慢 6%）。尾部在
 *       547 B 的典型输入里几乎从不执行（`end` 是整段输入的末尾，短字符串第一个
 *       16 字节块就命中闭引号），挪出热函数是零代价。
 *       非 SSE2 平台（AArch64 等）本函数就是**唯一**实现，必须允许内联，
 *       否则每个字符串都多一次 call/ret。
 */
#if defined(__SSE2__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((noinline))
#endif
inline const char* ScanPlainRunTail(const char* cursor, const char* end) noexcept {
  while (end - cursor >= 8) {
    std::uint64_t chunk = 0;
    std::memcpy(&chunk, cursor, sizeof(chunk));
    const std::uint64_t mask = HasZeroByte(chunk ^ 0x2222222222222222ULL) |
                               HasZeroByte(chunk ^ 0x5C5C5C5C5C5C5C5CULL) |
                               HasLessThanControl(chunk);
    if (mask != 0) {
#if defined(__GNUC__) || defined(__clang__)
      const unsigned index = static_cast<unsigned>(__builtin_ctzll(mask)) / 8u;
#else
      unsigned index = 0;
      while ((mask & (0x80ULL << (index * 8u))) == 0) ++index;
#endif
      return cursor + index;
    }
    cursor += 8;
  }
  while (cursor != end) {
    const unsigned char byte = static_cast<unsigned char>(*cursor);
    if (byte == '"' || byte == '\\' || byte < 0x20u) {
      break;
    }
    ++cursor;
  }
  return cursor;
}

/**
 * @brief 从 `cursor` 扫到第一个需要停下的事件（引号 / 反斜杠 / 控制字符）。
 * @param cursor [in] 起点；必须满足 `cursor <= end`。
 * @param end [in] 输入终点（不含）。
 * @return 第一个事件字节的位置；没有事件时返回 `end`。
 *
 * @details x86-64 基线 SSE2 一次处理 **16 字节**：两条 `pcmpeqb` 分别判
 *          `"`/`\`，一条 `psubusb` + `pcmpeqb` 判 `< 0x20`（无符号饱和减，
 *          不会被 0x80 以上的 UTF-8 续字节误判；有符号 `pcmpgtb` 会），
 *          两次 `por` 合并、`pmovmskb` 出掩码，共 7 条指令覆盖 16 字节。
 *          对比 8 字节 SWAR 的 9 条指令覆盖 8 字节，**每字节指令数降到 1/2.7**。
 *
 *          这对本项目的最常见形态格外划算：配置/API 响应里的键与字符串值普遍
 *          只有 12–14 字节，SWAR 需要 2 个块（8+8）才能碰到闭引号，而一个
 *          16 字节 SIMD 块就足以覆盖「内容 + 闭引号」，**每字符串从 2 块降到 1 块**。
 *
 *          剩余不足 16 字节时交给 `ScanPlainRunTail`（8 字节 SWAR + 逐字节）。
 *          非 SSE2 平台（AArch64 等）整段由 `ScanPlainRunTail` 承担，语义一致。
 *
 * @note 与 SWAR 版本的关键差别是**没有借位假阳性**：SIMD 比较逐字节精确，
 *       返回的位置一定是真事件。两者对外行为一致（停在第一个事件上）。
 */
inline const char* ScanPlainRun(const char* cursor, const char* end) noexcept {
#if defined(__SSE2__)
  const __m128i quote = _mm_set1_epi8('"');
  const __m128i backslash = _mm_set1_epi8('\\');
  const __m128i control_max = _mm_set1_epi8(0x1F);
  const __m128i zero = _mm_setzero_si128();
  while (end - cursor >= 16) {
    const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cursor));
    // 三个事件并行判定；`subs_epu8` 对 0x00..0x1F 饱和减到 0，正好等价于 `< 0x20`。
    const __m128i is_quote = _mm_cmpeq_epi8(chunk, quote);
    const __m128i is_backslash = _mm_cmpeq_epi8(chunk, backslash);
    const __m128i is_control = _mm_cmpeq_epi8(_mm_subs_epu8(chunk, control_max), zero);
    const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(
        _mm_or_si128(_mm_or_si128(is_quote, is_backslash), is_control)));
    if (mask != 0) {
      return cursor + static_cast<std::ptrdiff_t>(__builtin_ctz(mask));
    }
    cursor += 16;
  }
#endif
  return ScanPlainRunTail(cursor, end);
}

/// @brief 十六进制字符转数值；非法返回 -1。
inline int HexValue(char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

/**
 * @brief 校验一段字节是否是合法 UTF-8（RFC 3629 的结构校验）。
 *
 * @param content [in] 字符串引号之间的**原始**字节；借用，调用期间有效。
 * @param bad_offset [out] 非法时写入**首个非法序列首字节**相对 `content` 的偏移
 *        （0 起）；合法时不改动。调用方负责初始化。
 * @return 合法 true；非法 false。
 *
 * @details 接受的序列：
 *          - `00-7F`：单字节；
 *          - `C2-DF` + 1 个续字节；
 *          - `E0-EF` + 2 个续字节，且 `E0` 的第二个字节 ∈ `A0-BF`（否则超长编码）、
 *            `ED` 的第二个字节 ∈ `80-9F`（否则落入 UTF-16 代理区
 *            `U+D800..U+DFFF`，那是 CESU-8 而非 UTF-8）；
 *          - `F0-F4` + 3 个续字节，且 `F0` 的第二个字节 ∈ `90-BF`（否则超长编码）、
 *            `F4` 的第二个字节 ∈ `80-8F`（否则 > `U+10FFFF`）。
 *          续字节一律 `80-BF`；拒绝孤立续字节、`C0/C1`（超长编码）、`F5-FF`。
 *
 * @note 这是**结构性**校验，不拒绝 `U+FEFF` 等合法码点：字符串内的 BOM 字节
 *       按普通字符处理；输入开头的 BOM 由 `ParserConfig::allow_bom` 单独处理。
 *       不做 Unicode 规范等价（NFC/NFD）或非字符（`U+FFFE` 等）检查。
 *       只被冷函数 `Scanner::FinishString` 调用，不在默认热路径上。
 */
inline bool ValidateUtf8(std::string_view content, std::size_t& bad_offset) noexcept {
  const auto* const begin = reinterpret_cast<const unsigned char*>(content.data());
  const auto* const end = begin + content.size();
  const auto* cursor = begin;
  while (cursor != end) {
    const unsigned int lead = *cursor;
    if (lead <= 0x7Fu) {
      ++cursor;
      continue;
    }
    // 首字节决定后续字节数与**第二个字节**的可接受区间（默认 80-BF）。
    std::size_t tails = 0;
    unsigned int second_low = 0x80u;
    unsigned int second_high = 0xBFu;
    if (lead >= 0xC2u && lead <= 0xDFu) {
      tails = 1;
    } else if (lead == 0xE0u) {
      tails = 2;
      second_low = 0xA0u;
    } else if (lead >= 0xE1u && lead <= 0xECu) {
      tails = 2;
    } else if (lead == 0xEDu) {
      tails = 2;
      second_high = 0x9Fu;
    } else if (lead >= 0xEEu && lead <= 0xEFu) {
      tails = 2;
    } else if (lead == 0xF0u) {
      tails = 3;
      second_low = 0x90u;
    } else if (lead >= 0xF1u && lead <= 0xF3u) {
      tails = 3;
    } else if (lead == 0xF4u) {
      tails = 3;
      second_high = 0x8Fu;
    } else {
      // 80-BF 孤立续字节 / C0,C1 超长编码 / F5-FF 超出 Unicode 范围。
      bad_offset = static_cast<std::size_t>(cursor - begin);
      return false;
    }
    // 截断序列（字符串在序列中结束、闭引号成了续字节）按首字节定位。
    if (static_cast<std::size_t>(end - cursor) <= tails) {
      bad_offset = static_cast<std::size_t>(cursor - begin);
      return false;
    }
    if (cursor[1] < second_low || cursor[1] > second_high) {
      bad_offset = static_cast<std::size_t>(cursor - begin);
      return false;
    }
    for (std::size_t index = 2; index <= tails; ++index) {
      if (cursor[index] < 0x80u || cursor[index] > 0xBFu) {
        bad_offset = static_cast<std::size_t>(cursor - begin);
        return false;
      }
    }
    cursor += tails + 1;
  }
  return true;
}

}  // namespace scanner_detail

/**
 * @brief JSON 词法扫描器：输入游标 + 字符串/数字/字面量原语。
 *
 * @details 只负责「读一个 token」，不关心容器结构；结构由 `BasicParser` 的帧栈
 *          驱动。**热路径原语（`AtEnd`/`Peek`/`Advance`/`SkipWhitespace`）定义在
 *          头文件内联**，消除跨 TU 调用开销；重逻辑（字符串转义、数字分类）
 *          留在 `.cpp`。转义字符串与 Decimal 直接写绑定的 `Document` 池。
 *
 * ## 行列是「延迟计算」的
 * 为把每字节 3 个分支的行列维护从热路径拿掉，`Advance` 只推进游标。只有
 * `Fail` / `FailAt` / `FailAtOffset` 需要位置时，才用 `ComputePosition` 从输入
 * 起点重放到出错偏移（错误路径 O(n)，可接受）。行列口径与逐字节维护完全一致：
 * `\n`、`\r` 都算换行，`\r\n` 只算一个，`\r` 不占列宽。
 */
class Scanner {
 public:
  /**
   * @param input [in] 待扫描字节；**借用**，调用方保证扫描期间有效。
   * @param doc [in/out] 解析结果池；扫描器只追加字符串 / Decimal。
   * @param max_string_bytes [in] 字符串/数字 token 的内容字节上限；0 = 不限。
   * @param validate_utf8 [in] 是否严格校验字符串内 UTF-8；false = 透传。
   */
  Scanner(std::string_view input, Document& doc, std::size_t max_string_bytes = 0,
          bool validate_utf8 = false) noexcept
      : m_begin(input.empty() ? &kEmptyInput : input.data()),
        m_cursor(m_begin),
        m_end(m_begin + input.size()),
        m_doc(&doc),
        m_max_string_bytes(max_string_bytes),
        m_validate_utf8(validate_utf8) {}

  /**
   * @brief 字符串 token 读完后做收尾校验：长度上限 +（`validate_utf8` 时）UTF-8。
   *
   * @param token_offset [in] 开引号的 0 起字节偏移；超限错误位置取这里。
   * @param error [out] 失败时写入 `kStringTooLarge` / `kInvalidUtf8`；成功不改动。
   * @return 通过 true；失败 false（`error` 已写入）。
   *
   * @details 前置条件：字符串已完整读完（`m_cursor` 指向闭引号之后），因此内容
   *          长度 = `Offset() - token_offset - 2`（两侧引号各占 1 字节），内容
   *          起点 = `m_begin + token_offset + 1`。
   *          长度闸门先于内容校验：同时开两项时，超长且非法的字符串报超长。
   *
   * @note **调用点负责过滤默认配置**：`BasicParser` 只在编译期开关
   *       `kChecks == true` 的实例里调用本函数，默认实例的这段调用被
   *       `if constexpr` 整体消除；`noinline` 防止检查代码膨胀进热函数。
   *       失败时 `Document` 里可能已写入该字符串，由 `Parse` 统一 `Clear()`。
   */
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  bool FinishString(std::size_t token_offset, ErrorPtr& error) {
    // step1: 长度闸门——引号之间的**原始**字节数（含转义文本、不含两侧引号）。
    //        口径与 ParserConfig::max_string_bytes 一致：防转义膨胀，不按解码后长度。
    const std::size_t content_length = Offset() - token_offset - 2;
    if (m_max_string_bytes != 0 && content_length > m_max_string_bytes) {
      return FailAtOffset(JsonFailure::kStringTooLarge, token_offset, error);
    }
    // step2: 未开 UTF-8 校验时到此结束（默认口径：原样透传，不分类任何字节）。
    if (!m_validate_utf8) {
      return true;
    }
    // step3: 结构校验原始字节；失败位置 = 非法序列首字节在输入中的绝对偏移
    //        （内容起点是开引号后一字节，所以加回 token_offset + 1）。
    std::size_t bad_offset = 0;
    const std::string_view content(m_begin + token_offset + 1, content_length);
    if (scanner_detail::ValidateUtf8(content, bad_offset)) {
      return true;
    }
    return FailAtOffset(JsonFailure::kInvalidUtf8, token_offset + 1 + bad_offset, error);
  }

  /**
   * @brief 数字 token 读完后校验字面量字节数是否超过 `max_string_bytes`。
   *
   * @param token_offset [in] 数字首字符（`-` 或数字）的 0 起字节偏移。
   * @param error [out] 超限时写入 `kStringTooLarge`；未超限不改动。
   * @return 未超限 true；超限 false。
   *
   * @details 前置条件：数字已解析成功，`m_cursor` 停在 token 末尾之后，因此
   *          字面量长度 = `Offset() - token_offset`（含 `-`/小数/指数）。
   * @note `max_string_bytes == 0` 时恒真；调用点由 `kChecks` 编译期过滤。
   */
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  bool FinishNumber(std::size_t token_offset, ErrorPtr& error) {
    // 数字没有引号：字面量长度 = 游标位置 - token 起点（含 '-'/小数/指数）。
    if (m_max_string_bytes == 0 || Offset() - token_offset <= m_max_string_bytes) {
      return true;
    }
    return FailAtOffset(JsonFailure::kStringTooLarge, token_offset, error);
  }

  /// @brief 是否到达输入末尾。
  bool AtEnd() const noexcept { return m_cursor == m_end; }
  /// @brief 当前字节；`AtEnd()` 时返回 `'\0'`（调用方应先判 `AtEnd`）。
  char Peek() const noexcept { return m_cursor == m_end ? '\0' : *m_cursor; }

  /// @brief 当前游标在输入中的 0 起字节偏移。
  std::size_t Offset() const noexcept { return static_cast<std::size_t>(m_cursor - m_begin); }
  /// @brief 当前游标的 1 起行号（O(offset) 重放，错误路径用）。
  std::size_t Line() const noexcept;
  /// @brief 当前游标的 1 起列号（按字节计，错误路径用）。
  std::size_t Column() const noexcept;

  /// @brief 消耗当前字节（不维护行列；位置在报错时重放计算）。
  void Advance() noexcept { ++m_cursor; }

  /// @brief 跳过 `' ' '\t' '\n' '\r'`；不跳其它任何字节。
  void SkipWhitespace() noexcept {
    while (m_cursor != m_end && scanner_detail::IsWhitespace(*m_cursor)) {
      ++m_cursor;
    }
  }

  /**
   * @brief 从 `offset` 起用**括号深度计数**跳到当前值的结束位置（同步点）。
   *
   * @param offset [in] 同步起点（`RecoveryPoint::value_offset`，即值首个字节的
   *               0 起偏移）；大于输入长度时按输入长度处理。
   * @return
   * - `true`：游标已停在 `,` / `}` / `]` 上，或停在输入末尾（`AtEnd()` 为 true）——
   *   前者可作为恢复点，后者表示坏值一直延伸到输入末尾，调用方按不可恢复处理；
   * - `false`：扫描进了一个**未闭合的字符串**直到输入末尾，无法确定同步点；
   *   此时游标停在输入末尾，调用方必须按不可恢复处理。
   *
   * ## 定位
   * 错误恢复的唯一定位点：把「坏值在哪儿结束」变成「游标停在哪个同步字节」。
   * 若这里把字符串内的逗号误判成同步点，恢复会把半个字符串当成语法继续解析，
   * 结果是整棵文档结构错乱，比直接失败更糟。
   *
   * ## 步骤
   * step1. 字符串整体跳过（含转义：`\\` 后一个字节一律忽略），未闭合则失败。
   * step2. `{` / `[` 只增加深度（坏值内部本来就可能不配对，不校验类型）。
   * step3. 深度 0 的 `}` / `]` 就是同步点；深度 > 0 时配掉一个开符。
   * step4. 深度 0 的 `,` 是同步点；其余字节一律忽略。
   *
   * ## 为什么从「值起点」而不是「出错点」扫描
   * 错误若发生在字符串内部（如 `{"s":"\q",...}` 的非法转义），从出错点扫描会把
   * 该字符串的**闭合引号**当成新字符串的开引号，从而吞掉后续成员。从值起点
   * 扫描时整条字符串被当作一个词法单元跳过，同步点必然落在正确的分隔符上。
   *
   * @note 复杂度 O(值的字节数)，与已解析部分无关；只移动游标，不写文档。
   */
  bool SyncToValueEnd(std::size_t offset) noexcept {
    const std::size_t size = static_cast<std::size_t>(m_end - m_begin);
    const std::size_t start = offset < size ? offset : size;
    const char* cursor = m_begin + start;
    std::size_t depth = 0;
    while (cursor != m_end) {
      const char byte = *cursor;
      // step1: 字符串整体跳过；没有闭合引号就说明坏值延伸到输入末尾。
      if (byte == '"') {
        ++cursor;
        bool closed = false;
        while (cursor != m_end) {
          if (*cursor == '\\') {
            cursor += (m_end - cursor >= 2) ? 2 : 1;
            continue;
          }
          if (*cursor == '"') {
            ++cursor;
            closed = true;
            break;
          }
          ++cursor;
        }
        if (!closed) {
          m_cursor = m_end;
          return false;
        }
        continue;
      }
      // step2: 开括号只记深度，不要求括号类型配对（坏值内部可能本来就不配对）。
      if (byte == '{' || byte == '[') {
        ++depth;
        ++cursor;
        continue;
      }
      // step3: 深度 0 的闭括号就是同步点（父容器自己的闭合符）。
      if (byte == '}' || byte == ']') {
        if (depth == 0) {
          m_cursor = cursor;
          return true;
        }
        --depth;
        ++cursor;
        continue;
      }
      // step4: 深度 0 的逗号是同步点。
      if (byte == ',' && depth == 0) {
        m_cursor = cursor;
        return true;
      }
      ++cursor;
    }
    m_cursor = m_end;
    return true;
  }

  /**
   * @brief 用当前位置构造 `JsonError` 并返回 false。
   * @param failure [in] 失败原因。@param error [out] 写入错误对象。
   * @return 恒为 false（便于 `return scanner.Fail(...)`）。
   */
  bool Fail(JsonFailure failure, ErrorPtr& error);

  /**
   * @brief 在指定位置构造 `JsonError` 并返回 false（显式行列）。
   * @param failure [in] 失败原因。
   * @param offset [in] 0 起字节偏移。@param line [in] 1 起行号。
   * @param column [in] 1 起列号。@param error [out] 写入错误对象。
   * @return 恒为 false。
   */
  bool FailAt(JsonFailure failure, std::size_t offset, std::size_t line, std::size_t column,
              ErrorPtr& error);

  /**
   * @brief 在指定偏移构造 `JsonError`，行列由 `ComputePosition` 重放得到。
   * @param failure [in] 失败原因。
   * @param offset [in] 0 起字节偏移。
   * @param error [out] 写入错误对象。
   * @return 恒为 false。
   */
  bool FailAtOffset(JsonFailure failure, std::size_t offset, ErrorPtr& error);

  /**
   * @brief 从输入起点重放到 `offset`，计算 1 起行列。
   * @param offset [in] 目标字节偏移；大于输入长度时按输入长度处理。
   * @param line [out] 1 起行号。
   * @param column [out] 1 起列号（按字节计）。
   */
  void ComputePosition(std::size_t offset, std::size_t& line, std::size_t& column) const noexcept;

  /**
   * @brief 解析一个 JSON 字符串（当前字节必须是 `"`）。
   * @param out [out] 成功时写入；无转义为借用输入，有转义为文档池视图。
   * @param error [out] 失败时写入错误对象。
   * @return 成功 true；未闭合、裸控制字符、非法转义返回 false。
   *
   * @details 热路径内联：先用 SWAR 扫到引号，无转义就直接借用输入返回；
   *          一旦遇到转义/控制字符/未闭合，回退到 `.cpp` 的完整状态机。
   */
  bool ParseString(core::Text& out, ErrorPtr& error) {
    if (AtEnd() || *m_cursor != '"') {
      return Fail(JsonFailure::kUnexpectedChar, error);
    }
    const char* const content = m_cursor + 1;
    const char* const stop = scanner_detail::ScanPlainRun(content, m_end);
    if (stop != m_end && *stop == '"') {
      m_cursor = stop + 1;
      out = core::Text::Borrow(
          std::string_view(content, static_cast<std::size_t>(stop - content)));
      return true;
    }
    // 回退：把游标放回开引号，交给慢速状态机（转义 / 控制字符 / 未闭合）。
    return ParseStringSlow(out, error);
  }

  /**
   * @brief 解析字符串并只返回视图（对象键专用，省一次 `Text` 构造与拷贝）。
   * @param out [out] 成功时写入字节视图；借用输入或指向文档字符串池。
   * @param error [out] 失败时写入错误对象。
   * @return 成功 true。
   */
  bool ParseStringView(std::string_view& out, ErrorPtr& error) {
    if (AtEnd() || *m_cursor != '"') {
      return Fail(JsonFailure::kUnexpectedChar, error);
    }
    const char* const content = m_cursor + 1;
    const char* const stop = scanner_detail::ScanPlainRun(content, m_end);
    if (stop != m_end && *stop == '"') {
      m_cursor = stop + 1;
      out = std::string_view(content, static_cast<std::size_t>(stop - content));
      return true;
    }
    core::Text text;
    if (!ParseStringSlow(text, error)) {
      return false;
    }
    out = text.view();
    return true;
  }

  /**
   * @brief 解析一个 JSON 数字（当前字节必须是 `-` 或数字）。
   * @param out [out] 成功时写入 `kInt` / `kDouble` / `kDecimal` 节点；
   *              `kDecimal` 的 payload 是文档 Decimal 池下标。失败时不变。
   * @param error [out] 失败时写入错误对象。
   * @return 成功 true；语法非法或无法表示返回 false。
   *
   * @details 热路径（强制内联，见下）：无指数、≤15 位有效数字（或 ≤19 位整数）
   *          时在本函数内直接得到 int64 / double；其余（指数、超长、溢出）回退
   *          到 `.cpp` 的完整扫描器 + `from_chars` / `Decimal`。
   *
   * @note `always_inline` 是实测选择：本函数每次解析被调用 13 次，单次调用要
   *       付出 call/ret + 5 个 callee-saved 寄存器的保存/恢复（约 17 条指令），
   *       占它自身指令数的 ~10%；在两个调用点各内联一次的代码膨胀没有挤爆 L1i。
   */
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((always_inline))
#endif
  bool ParseNumber(Node& out, ErrorPtr& error) {
    if (AtEnd()) {
      return Fail(JsonFailure::kUnexpectedEnd, error);
    }
    const char* cursor = m_cursor;
    bool negative = false;
    if (*cursor == '-') {
      negative = true;
      ++cursor;
      if (cursor == m_end) {
        // 只有负号：交给慢速路径保持「出错偏移 = m_end」的既有语义。
        return ParseNumberSlow(out, error);
      }
    }

    std::uint64_t mantissa = 0;
    int digits = 0;
    int fraction_digits = 0;
    bool overflow = false;

    if (*cursor == '0') {
      ++cursor;
      if (cursor != m_end && scanner_detail::IsDigit(*cursor)) {
        return ParseNumberSlow(out, error);  // 前导零：交给慢速路径报错
      }
    } else if (*cursor >= '1' && *cursor <= '9') {
      // 单遍：判断「是不是数字」和「累加进尾数」在同一个循环体内完成。
      // 原实现是两段式（先只推进指针、再回头重读累加），每字符要走两遍、约
      // 13 条指令；这里一次走完，配合下面的 SWAR 块把小数部分再压一半。
      // 溢出检查用十进制位数而不是除法：19 位以内不可能溢出 uint64，
      // 所以 64 位除法不进热路径。
      do {
        if (digits < 19) {
          mantissa = mantissa * 10 + static_cast<unsigned>(*cursor - '0');
          ++digits;
        } else {
          overflow = true;
        }
        ++cursor;
      } while (cursor != m_end && scanner_detail::IsDigit(*cursor));
    } else {
      return ParseNumberSlow(out, error);  // `-` 后不是数字：慢速路径报错
    }

    if (cursor != m_end && *cursor == '.') {
      ++cursor;
      if (cursor == m_end) {
        return ParseNumberSlow(out, error);
      }
      if (!scanner_detail::IsDigit(*cursor)) {
        return ParseNumberSlow(out, error);
      }
      const char* const fraction_start = cursor;
      // 小数位通常比整数位长，叠一层 SWAR：一次 8 字节块把 8 个数字压成一个
      // 值，约 2.5 条/字符（逐位是 6~8 条）。
      // `digits <= 11` 保证 `mantissa * 1e8 + block` 最多 19 位、仍在 uint64 内；
      // 先看第 8 个字节是不是数字，挡掉短小数上「必然失败的块校验」。
      for (;;) {
        if (cursor + 8 <= m_end && digits <= 11 && scanner_detail::IsDigit(cursor[7])) {
          std::uint64_t chunk = 0;
          std::memcpy(&chunk, cursor, sizeof(chunk));
          const std::uint64_t block = scanner_detail::EightDigits(chunk);
          if (block != ~std::uint64_t{0}) {
            mantissa = mantissa * 100000000ULL + block;
            digits += 8;
            cursor += 8;
            continue;
          }
        }
        break;
      }
      // 尾巴不足 8 位（或第 8 个字节不是数字）时逐位收尾。
      // 注意 `fraction_digits` 无论是否溢出都必须精确：它是 Clinger 判决的输入。
      while (cursor != m_end && scanner_detail::IsDigit(*cursor)) {
        if (digits < 19) {
          mantissa = mantissa * 10 + static_cast<unsigned>(*cursor - '0');
          ++digits;
        } else {
          overflow = true;
        }
        ++cursor;
      }
      fraction_digits = static_cast<int>(static_cast<std::size_t>(cursor - fraction_start));
    }
    // `'e'` / `'E'` 用一次掩码比较判掉，省掉第二个分支。
    if (cursor != m_end && ((*cursor | 0x20) == 'e')) {
      return ParseNumberSlow(out, error);  // 指数形式交给慢速路径
    }

    // `fraction_digits == 0` 等价于「没出现过小数点」（出现 `.` 后必然至少有
    // 一位小数），因此可以省掉一个布尔量的维护。
    if (fraction_digits == 0) {
      constexpr std::uint64_t kInt64Max = static_cast<std::uint64_t>(INT64_MAX);
      if (!overflow && !negative && mantissa <= kInt64Max) {
        m_cursor = cursor;
        out.kind = NodeKind::kInt;
        out.payload.integer = static_cast<std::int64_t>(mantissa);
        return true;
      }
      if (!overflow && negative && mantissa <= kInt64Max + 1u) {
        m_cursor = cursor;
        out.kind = NodeKind::kInt;
        out.payload.integer =
            mantissa == kInt64Max + 1u ? INT64_MIN : -static_cast<std::int64_t>(mantissa);
        return true;
      }
      return ParseNumberSlow(out, error);
    }

    // Clinger 快速路径：≤15 位尾数 + 小数位 ≤22，一次除法即正确舍入。
    if (!overflow && digits <= 15 && fraction_digits <= 22) {
      static constexpr double kPow10Table[23] = {
          1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
          1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
      // `fraction_digits == 0` 的整数在上面的整数分支就返回了，走到这里必然
      // `fraction_digits > 0`——直接查表相除，省掉一次恒真分支（每数字一次）。
      double value = static_cast<double>(mantissa) / kPow10Table[fraction_digits];
      m_cursor = cursor;
      out.kind = NodeKind::kDouble;
      out.payload.real = negative ? -value : value;
      return true;
    }
    return ParseNumberSlow(out, error);
  }

  /**
   * @brief 解析 `true` / `false` / `null`（当前字节是首字母）。
   * @param literal [in] 期望的字面量（必须正好是这三个之一）。
   * @param out [out] 成功时写入 `kBool` / `kNull` 节点。
   * @param error [out] 失败时写入错误对象。
   * @return 成功 true；字节不匹配或提前结束返回 false。
   */
  bool ParseLiteral(std::string_view literal, Node& out, ErrorPtr& error);

 private:
  /// @brief 把 Unicode 码点按 UTF-8 写进文档字符串池。
  void AppendUtf8(std::uint32_t code);

  /// @brief 完整字符串状态机：处理转义、UTF-8 解码、未闭合与控制字符。
  bool ParseStringSlow(core::Text& out, ErrorPtr& error);

  /// @brief 完整数字扫描器：处理指数、超长尾数、int64/double 溢出与 Decimal 退化。
  bool ParseNumberSlow(Node& out, ErrorPtr& error);

  /// @brief 空输入时的替身，保证 `m_begin`/`m_end` 指向合法对象。
  inline static constexpr char kEmptyInput = '\0';

  const char* m_begin;  ///< 输入起点（空输入时指向静态 NUL）。
  const char* m_cursor; ///< 当前游标。
  const char* m_end;    ///< 输入终点（不含）。
  Document* m_doc;      ///< 解析结果池；不可为 nullptr。
  std::size_t m_max_string_bytes;  ///< 字符串/数字 token 内容上限；0 = 不限。
  bool m_validate_utf8;            ///< 是否严格校验字符串内 UTF-8；false = 透传。
};



/// @brief 检测 `Stack` 是否提供 `push(T&&)`（`KInlineStack` 接口）。
template <typename Stack, typename = void>
struct HasStackPush : std::false_type {};
template <typename Stack>
struct HasStackPush<Stack,
                   std::void_t<decltype(std::declval<Stack&>().push(std::declval<Frame>()))>>
    : std::true_type {};

/// @brief 检测 `Stack` 是否提供 `top()`（`KInlineStack` 接口）。
template <typename Stack, typename = void>
struct HasStackTop : std::false_type {};
template <typename Stack>
struct HasStackTop<Stack, std::void_t<decltype(std::declval<Stack&>().top())>> : std::true_type {};

/// @brief 检测 `Stack` 是否提供 `pop()`（`KInlineStack` 接口）。
template <typename Stack, typename = void>
struct HasStackPop : std::false_type {};
template <typename Stack>
struct HasStackPop<Stack, std::void_t<decltype(std::declval<Stack&>().pop())>> : std::true_type {};

/// @brief 检测 `Stack` 是否提供无参 `emplace()`（`KInlineStack` 接口）。
template <typename Stack, typename = void>
struct HasStackEmplace : std::false_type {};
template <typename Stack>
struct HasStackEmplace<Stack, std::void_t<decltype(std::declval<Stack&>().emplace())>>
    : std::true_type {};

/**
 * @brief 就地构造一个新的栈帧并返回引用（省掉「栈上临时 Frame → 移动」）。
 * @param stack [in,out] 帧栈。
 * @return 新帧引用；在下一次可能触发扩容的 push 前有效。
 *
 * @details 原先的写法是 `Frame next; ...字段赋值...; PushFrame(stack, std::move(next))`：
 *          先在调用方栈上构造 48 B，再整体移动进容器——每层两次 48 B 写。
 *          改为就地构造后只剩字段赋值本身（深嵌套 100 层实测 -4% 指令）。
 */
template <typename Stack>
Frame& EmplaceFrame(Stack& stack) {
  if constexpr (HasStackEmplace<Stack>::value) {
    return stack.emplace();
  } else {
    return stack.emplace_back();
  }
}

/// @brief 压入帧：`KInlineStack` 走 `push`，`std::vector` 走 `push_back`。
template <typename Stack>
void PushFrame(Stack& stack, Frame frame) {
  if constexpr (HasStackPush<Stack>::value) {
    stack.push(std::move(frame));
  } else {
    stack.push_back(std::move(frame));
  }
}

/// @brief 取栈顶帧：`KInlineStack` 走 `top`，`std::vector` 走 `back`。
template <typename Stack>
Frame& TopFrame(Stack& stack) {
  if constexpr (HasStackTop<Stack>::value) {
    return stack.top();
  } else {
    return stack.back();
  }
}

/// @brief 弹出栈顶帧：`KInlineStack` 走 `pop`，`std::vector` 走 `pop_back`。
template <typename Stack>
void PopFrame(Stack& stack) {
  if constexpr (HasStackPop<Stack>::value) {
    stack.pop();
  } else {
    stack.pop_back();
  }
}

}  // namespace detail

/**
 * @brief 迭代式 JSON 解析器。
 *
 * @tparam Policy 帧栈策略；见 `config.hpp` 的 `InlinePolicy` / `StdPolicy` /
 *                `CompactPolicy` / `AutoPolicy`。默认 `InlinePolicy`。
 *
 * @note 解析器本身无状态、可复用；每次 `Parse` 用独立的帧栈与扫描器，
 *       不同线程可以各自持有解析器实例（或共享同一个，因为 `Parse` 是 const）。
 *
 * ## 用法
 * @code
 * bgo2cpp::json::Parser parser;
 * bgo2cpp::json::Document document;
 * extened::ErrorPtr error;
 * if (!parser.Parse(input, document, error)) {
 *   std::fprintf(stderr, "%s\n", extened::ErrorMessage(error).c_str());
 * }
 * const bgo2cpp::json::Value& root = document.Root();
 * @endcode
 */
/// @brief 部分解析结果：成功标志 + 已消费字节数。
struct ParseOutcome {
  bool ok = false;           ///< 是否完整解析成功。
  std::size_t consumed = 0;  ///< 成功 = 输入长度；失败 = 出错字节偏移（可断点续读）。
};

template <typename Policy = InlinePolicy>
class BasicParser {
 public:
  /**
   * @brief 解析一段 JSON 文本。
   *
   * @param input [in] UTF-8 字节；**借用**——结果里的 borrowed `Text` 指向它，
   *              因此 `input` 必须比 `doc` 活得久，且解析期间不得修改。
   * @param doc [in,out] 解析结果池；入口先清空，成功时 `doc.Root()` 是根值，
   *              失败时被清空（`Root().IsNull()`）。所有权仍归调用方。
   * @param error [out] 失败时写入 `JsonError`；成功时清空。
   * @param config [in] 运行期配置；`max_depth == 0` 表示按输入长度自动；
   *              `duplicate_key` 默认 `kFirst`（重复成员全部保留、查找返回首个）。
   * @return 成功 true；语法错误 false（`error` 非空）。
   *
   * ## 定位
   * 这是 JSON 文本进入值模型的唯一入口。失败必须让 `doc` 处于可析构的确定
   * 状态（空文档），否则调用方误用半成品池会读到不完整的容器。
   *
   * ## 步骤
   * step1. 清空 `doc`/`error`，按输入长度预分配节点池与成员池。
   * step2. 主循环按帧栈状态推进：读键、读冒号、读值、读逗号、关容器。
   * step3. 读值是容器时做深度检查并压帧；标量直接写入父容器的成员/元素池。
   * step4. 根值完成后只允许尾随空白，否则报 kTrailingGarbage。
   *
   * ## 并发/失败语义
   * - `const`、无共享可变状态；同一实例可被多线程同时调用（每个线程用自己的
   *   `Document`）；
   * - 失败时 `doc.Empty() == true`、`error` 是带位置的 `JsonError`；
   * - 分配失败抛 `std::bad_alloc`，不静默截断。
   */
  bool Parse(std::string_view input, Document& doc, ErrorPtr& error,
             const ParserConfig& config = ParserConfig{}) const;

  /// @brief 部分解析：失败时**保留**已解析节点，并返回出错偏移（截断场景用）。
  /// @param input [in] 输入；可为截断的 JSON 片段（借用，调用期间有效）。
  /// @param doc [out] 成功时是完整文档；失败时是「已解析部分」（可能只有池节点、
  ///        没有 Root）；调用前内容被清空。
  /// @param error [out] 失败时写入带位置的 JsonError；成功时为空。
  /// @param config [in] 解析配置。
  /// @return `{ok, consumed}`：`ok` 时 `consumed == input.size()`；失败时
  ///         `consumed` 是出错字节偏移（调用方可据此续读）。
  ParseOutcome ParsePartial(std::string_view input, Document& doc, ErrorPtr& error,
                            const ParserConfig& config = ParserConfig{}) const;

  /// @brief 逐行解析（NDJSON）：每行一个完整 JSON，空行忽略（大文件流式场景）。
  /// @param input [in] 多行文本（借用）；行分隔符 `\n`，行尾 `\r` 自动去掉。
  /// @param on_document [in] 每成功解析一个文档回调一次；返回 false 提前停止。
  /// @param error [out] 失败时写入错误（offset 相对当前行）。
  /// @param config [in] 每行生效的配置。
  /// @return 成功解析的文档数（出错时 = 出错前的成功数）。
  std::size_t ParseMany(std::string_view input,
                        const std::function<bool(Document&)>& on_document, ErrorPtr& error,
                        const ParserConfig& config = ParserConfig{}) const;

 private:
  /**
   * @brief 默认 `kAbort` 路径的完整实现（`InlinePolicy` 专用，定义在本头文件末尾）。
   *
   * @param input [in] UTF-8 字节；**借用**，调用方保证解析期间有效。
   * @param doc [out] 解析结果池；入口重置，失败时由调用方统一清空。
   * @param error [out] 失败时写入 `JsonError`；成功时为空。
   * @param config [in] 运行期配置；此路径只读 `max_depth`/`allow_trailing_comma`/
   *        `timeout_ms`/`allow_bom`/`max_input_bytes`/`duplicate_key`/
   *        `max_string_bytes`/`validate_utf8`。
   * @return 成功 true；语法错误 false（`error` 非空）。
   *
   * ## 定位
   * 默认路径的**唯一入口**：它把 `kAbort` 与恢复路径在**调用图**上分开——默认
   * `Parser` 的调用方 TU 只会实例化 `ParseInternalBody<false, false>`（不含任何
   * 恢复代码），恢复状态机 `ParseInternalBody<true, true>` 被挡在 `parser.cpp`
   * 的 `ParseRecoverImpl` 后面，不会进入默认路径的编译单元。
   *
   * ## 为什么定义在头文件而不是 `.cpp`（实测结论）
   * 把这份实现整体挪到 `.cpp`（非模板、跨 TU 调用）看起来更干净，但实测默认解析
   * 的退休指令数从 574M 涨到 607M（+5.8%），`taskset` 绑核后 min 也从 206.5 ns
   * 涨到 212.9 ns。原因是 `Parse` 被内联进调用方循环后，GCC 的 IPA-SRA 能把
   * `Document&` 特化成 `[clone .isra.0]`；跨 TU 时这份克隆与别名信息全部丢失。
   * 因此默认路径的实现必须和调用方在同一个 TU——这里保留 `inline` 定义，只把
   * **恢复路径**外置（默认路径本来就不需要那段代码）。
   *
   * ## 并发/失败语义
   * - 只读 `config`、写 `doc`/`error`；与 `Parse` 一样可被多线程各自调用；
   * - 失败时 `doc` 可能已有半成品，由 `Parse` 统一 `Clear()`。
   */
  bool ParseAbortImpl(std::string_view input, Document& doc, ErrorPtr& error,
                      const ParserConfig& config) const;

  /**
   * @brief `kSkipValue` / `kCollect` 在默认 `InlinePolicy` 下的入口（定义在 `parser.cpp`）。
   *
   * @param input [in] UTF-8 字节；**借用**，调用方保证解析期间有效。
   * @param doc [in,out] 解析结果池；恢复成功时 `Root()` 可用、`Errors()` 带错误清单。
   * @param error [out] 不可恢复时写入错误；恢复成功时为空。
   * @param config [in] 运行期配置；读取 `error_policy` / `max_errors` 与 limits 相关项。
   * @return 恢复成功（至少根值可用）true；不可恢复 false（`error` 非空）。
   *
   * @details 为什么必须外置到 `.cpp`：若让 `Parse` 直接引用头文件里的恢复状态机，
   *          默认 `Parser` 的每个调用方 TU 都会实例化一整份恢复代码（bench 里
   *          默认路径的 I-cache 与 IPA-SRA 上下文随之改变，旧版实测 +5~9%）。
   *          这里用一次外部调用把恢复代码挡在默认 TU 之外。
   * @note `parser.cpp` 里只有 `BasicParser<InlinePolicy>` 的显式特化：另外三套
   *       Policy 的恢复路径由调用方 TU 自己实例化 `ParseInternalBody<..., true>`，
   *       不会在这里多生成三份恢复状态机（那样 `.text` 会多出约 15 KB）。
   * @note 恢复实例固定使用 `kChecks == true`（冷路径，token 检查在
   *       `max_string_bytes == 0 && !validate_utf8` 时恒真）：恢复路径不是性能
   *       目标，少一份实例化远比省掉几次冷函数调用重要；`kChecks` 与「是否恢复」
   *       两个维度因此保持正交，恢复路径同样完整支持 limits 检查。
   */
  bool ParseRecoverImpl(std::string_view input, Document& doc, ErrorPtr& error,
                        const ParserConfig& config) const;

  /**
   * @brief `Parse` 的恢复入口：调用恢复状态机，并在失败时清空文档。
   *
   * @param input [in] UTF-8 字节（借用）。@param doc [in,out] 结果池。
   * @param error [out] 不可恢复时的错误。@param config [in] 运行期配置。
   * @return 恢复成功 true；不可恢复 false（此时 `doc` 已清空）。
   *
   * @details 为什么单独一层且 `noinline`：`Parse` 的热路径必须与改动前**同形**
   *          （入口 `Clear` + 一次 `ParseInternal` + 失败 `Clear`）。若把恢复分支
   *          的失败 `Clear` 直接写在 `Parse` 里，`Parse` 的函数体估计会超过 GCC
   *          的自动内联阈值（实测 `max-inline-insns-auto limit reached`），
   *          `Parse` 就不再被内联进调用方循环、`Document&` 的 IPA-SRA 克隆
   *          也随之消失——每次解析多 1.5% 指令。把「恢复 + 失败清空」打包到
   *          这个冷函数后，`Parse` 里只剩一条冷调用。
   */
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  bool ParseRecoverEntry(std::string_view input, Document& doc, ErrorPtr& error,
                         const ParserConfig& config) const;
  /**
   * @brief 非默认 `Policy` 的恢复入口：按 limits 开关分派到两个恢复实例。
   *
   * @param input [in] UTF-8 字节（借用）。@param doc [in,out] 结果池。
   * @param error [out] 不可恢复时的错误。@param config [in] 运行期配置。
   * @return 同 `Parse`。
   *
   * @note 默认 `InlinePolicy` 不使用本函数（`Parse` 的 `if constexpr` 会丢弃该
   *       分支），因此不会给默认路径的调用方 TU 增加任何代码。
   */
  template <bool kRecovering>
  bool ParseRecoverLimits(std::string_view input, Document& doc, ErrorPtr& error,
                          const ParserConfig& config) const;

  /// @brief `Parse` 的实现主体；失败时可能留下半成品池，由 `Parse` 统一清空。
  bool ParseInternal(std::string_view input, Document& doc, ErrorPtr& error,
                     const ParserConfig& config) const;

  /**
   * @brief `ParseInternal` 的实际主体（两个编译期维度：token 检查 × 错误恢复）。
   *
   * @tparam kChecks [in] 编译期开关：false（默认配置）时所有检查代码被
   *        `if constexpr` 消除，生成的指令与未加该功能时**逐条一致**；true 时
   *        每个字符串/数字 token 多一次冷函数调用。运行期由 `ParseInternal`
   *        按 `config.max_string_bytes` / `config.validate_utf8` 二选一。
   * @tparam kRecovering [in] 编译期开关：false 时恢复边界的记录/清除、失败分支
   *        的恢复尝试全部被 `if constexpr` 消除（边界类型降级为 1 B 的
   *        `RecoveryStub`，默认路径不为 56 B 的 `RecoveryPoint` 预留栈槽位）；
   *        true 时同一份状态机额外携带恢复逻辑。
   * @param input [in] 待解析字节（借用）。@param doc [in,out] 结果池。
   * @param error [out] 失败时写入错误。@param config [in] 运行期配置。
   * @return 同 `Parse`。
   *
   * @note 为什么不用运行期 `bool` 成员做判断：实测每个 token 的
   *       「读标志 + 测试 + 跳转」让 20 字段小文档（547 B）回退约 3%；
   *       编译期分叉把默认路径的回退压到 0，代价是模板实例化若干份代码。
   *
   * @note 为什么不加 `always_inline`：实测强制内联会让本函数与外层 `Parse` 的
   *       代码布局变化，`perf stat` 的分支数比基线多 0.4%（每次解析多 4 个分支），
   *       周期数反而上浮约 2%；让 GCC 按普通内联启发式处理，分支数与基线
   *       **完全相同**，指令数少约 3%。
   */
  template <bool kChecks, bool kRecovering>
  bool ParseInternalBody(std::string_view input, Document& doc, ErrorPtr& error,
                         const ParserConfig& config) const;

  /**
   * @brief 冷入口：`config` 开启任一检查时调用 `ParseInternalBody<true, false>`。
   *
   * @param input [in] 待解析字节（借用）。@param doc [in,out] 结果池。
   * @param error [out] 失败时写入错误。@param config [in] 运行期配置。
   * @return 同 `Parse`。
   *
   * @note `noinline` 保证检查实例不会内联进默认路径的调用者；默认配置下这个
   *       函数根本不会被调用。
   */
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  bool ParseInternalChecked(std::string_view input, Document& doc, ErrorPtr& error,
                            const ParserConfig& config) const;

  /**
   * @brief 平铺对象快速路径：`{"k":标量, ...}` 的单层对象不走通用状态机。
   *
   * @param doc [in,out] 解析结果池；**失败时可能已写入部分数据**，调用方负责清空。
   * @param scanner [in,out] 扫描器；失败时游标位置未定义，调用方重建。
   * @param error [out] 失败时可能写入错误对象，调用方负责清空。
   * @param kChecks [in] 同 `ParseInternalBody`：false 时检查代码编译期消失。
   * @return 成功 true（根对象已写完）；遇到嵌套容器、非标量值、语法错误返回
   *         false——调用方回退到通用状态机重新解析，错误仍由通用路径报告。
   *
   * @note 调用方只在 `duplicate_key == kFirst` 时走本快路径；`kLast` / `kError`
   *       需要逐成员查重，统一交给通用状态机（本函数不做重复检测，因此对
   *       重复 key 输入它保持「全部追加」的历史行为）。
   *
   * ## 定位
   * 真实配置/API 响应里绝大多数是「单层对象 + 标量成员」。通用状态机每个成员
   * 要经过栈顶读取、状态判断、两轮循环，这条路径把它压成一个直线循环：
   * 读键 → 读冒号 → 读标量 → 写成员 → 读逗号/闭合。
   */
  template <bool kChecks>
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  bool TryParseFlatObjectBody(Document& doc, detail::Scanner& scanner, ErrorPtr& error) const;

  /// @brief 按 `kChecks` 编译期决定是否校验字符串 token；false 时整个调用消失。
  template <bool kChecks>
  static bool CheckStringToken(detail::Scanner& scanner, std::size_t token_offset,
                               ErrorPtr& error) {
    if constexpr (kChecks) {
      return scanner.FinishString(token_offset, error);
    } else {
      (void)scanner;
      (void)token_offset;
      (void)error;
      return true;
    }
  }

  /// @brief 按 `kChecks` 编译期决定是否校验数字 token；false 时整个调用消失。
  template <bool kChecks>
  static bool CheckNumberToken(detail::Scanner& scanner, std::size_t token_offset,
                               ErrorPtr& error) {
    if constexpr (kChecks) {
      return scanner.FinishNumber(token_offset, error);
    } else {
      (void)scanner;
      (void)token_offset;
      (void)error;
      return true;
    }
  }

  /**
   * @brief 关闭栈顶容器：把当前帧收集的直接子项提交到全局池并回填 `Span`。
   *
   * @param stack [in,out] 帧栈；栈顶必须是要关闭的容器帧，函数内部弹出。
   * @param doc [in,out] 解析结果池。
   * @return 被关闭的容器是否是根（`stack.size() == 1`）。
   *
   * ## 定位
   * 这是「容器子表」唯一的定稿点。若漏掉提交或回填错误的 `count`，
   * `Value::At/Size` 会读到父容器的成员，遍历出现环或越界。
   */
  template <typename Stack>
  bool CloseContainer(Stack& stack, Document& doc) const;

  /**
   * @brief 链式闭合：从当前栈顶开始连续闭合，并就地消费紧随的 `,`。
   *
   * @param stack [in,out] 帧栈；调用前栈顶容器的闭合符已被消费。
   * @param doc [in,out] 解析结果池；每次闭合都会回填 `Span`。
   * @param scanner [in,out] 扫描器；游标在返回时停在「下一个语法位置」。
   * @return 根容器是否已闭合（`root_done`）。
   *
   * ## 定位
   * 深嵌套唯一的「批量收尾」入口。逐轮状态机在每层闭合时要走一遍
   * `switch (state)` + `SkipWhitespace` + 分派（约 25~30 条指令），而深嵌套
   * 的闭合序列是完全确定的：闭合 → 父帧 AfterValue → 下一个字节必然是 `,`
   * 或闭合符。这里把整段连续闭合压成一个循环，每层只花一次 `CloseContainer`
   * 的代价。
   *
   * ## 步骤
   * step1. 闭合栈顶容器；栈空即根完成。
   * step2. 父帧不是 AfterValue 时退出（语法位置特殊，交回主状态机）。
   * step3. `,`：把父帧切回「读键/读值」并返回（成员之间）。
   * step4. 闭合符：消费后继续 step1；其它字节原样返回，由主循环报错。
   *
   * ## 并发/失败语义
   * - 只在解析线程内调用；不抛异常（分配失败由 `CloseContainer` 内部抛出）。
   * - 失败路径不改游标：非法字节直接返回，错误位置与逐轮推进一致。
   * - 只在 `stack.size() >= kChainMinDepth` 时被调用（小输入走原路径，
   *   避免为扁平文档增加 I-cache 压力）。
   */
  template <typename Stack>
  bool CloseChain(Stack& stack, Document& doc, detail::Scanner& scanner) const;

  /**
   * @brief 按 `ParserConfig::duplicate_key` 提交一个对象成员（成员入表的唯一提交点）。
   *
   * @param doc [in,out] 解析结果池；成功时追加成员或改写已有成员的值。
   * @param frame [in,out] 当前对象容器帧；`member_begin` 是直接写入模式的池起点，
   *        `direct_count` 是本容器已写入的成员数，`spilled` 表示已退回 scratch 收集。
   *        函数可能把 `spilled` 从 false 改为 true 并登记新的 `scratch_begin`。
   * @param key [in] 成员名（解码后的字节视图）；借用输入或文档字符串池，函数内不保留。
   * @param value_index [in] 成员值在节点池中的下标；必须 < `doc.ValueCount()`。
   * @param key_offset [in] 该 key 开引号的 0 起字节偏移；仅 `kError` 使用。
   * @param config [in] 运行期配置；只读取 `duplicate_key`。
   * @param scanner [in] 扫描器；仅用于在 `key_offset` 处构造带行列的 `JsonError`。
   * @param error [out] `kError` 且发现重复时写入 `JsonError`；其它情况不改动。
   *
   * @return
   * - `kAppended`：已追加为新成员（`kFirst` 下重复项也走这里，保持历史行为）；
   * - `kMerged`：`kLast` 且命中同 key 已有成员——值已改写，位置不变；调用方
   *   **不要**再追加；
   * - `kFailed`：`kError` 且发现重复——`error` 已写入，调用方**必须**立即返回 false。
   *
   * ## 定位
   * 对象成员进入成员池的唯一提交点（通用状态机有「标量值」「容器值」两处调用；
   * 平铺快路径只在 `kFirst` 下运行，等价于直接追加）。若这里漏判重复，
   * `kLast` 会留下两条同名成员、`kError` 会静默成功。
   *
   * ## 步骤
   * step1. 检测直接写入模式是否被打断：子容器把自己的成员提交到池尾后，
   *        本容器已写入的成员不再位于池尾；把它们复制进 scratch 并转为 scratch 模式。
   * step2. `kFirst`（默认）或本容器还没有成员：直接追加，零检测开销。
   * step3. 在 `[member_begin, +direct_count)`（直接模式）或
   *        `[scratch_begin, MemberScratchSize())`（scratch 模式）内线性查找同名 key；
   *        子容器闭合时已把自己的区间回滚/让出，区间内只有本容器的直接成员。
   * step4. `kError`：在 key 起始偏移处构造 `kDuplicateKey` 错误。
   * step5. `kLast`：保留首次出现的位置，把值改为最后一次出现的值。
   *
   * ## 并发/失败语义
   * - 只读 `config`，写 `doc`/`error`/`frame`；与 `Parse` 一样是 const、每线程独立文档；
   * - 失败时 `doc` 可能已有半成品，由 `Parse` 统一 `Clear()`；
   * - 不变量：返回 `kMerged` 时成员表项数与顺序不变，只有值下标被改写。
   */
  detail::MemberSubmitResult SubmitObjectMember(Document& doc, detail::Frame& frame,
                                                std::string_view key, std::uint32_t value_index,
                                                std::size_t key_offset,
                                                const ParserConfig& config,
                                                detail::Scanner& scanner, ErrorPtr& error) const;

  /// @brief `SubmitObjectMember` 的慢路径：非默认重复键策略下的线性查重。
  /// @details 只在 `duplicate_key != kFirst` 时进入，默认路径不会把这段代码内联进来。
  detail::MemberSubmitResult SubmitObjectMemberDedup(Document& doc, detail::Frame& frame,
                                                    std::string_view key,
                                                    std::uint32_t value_index,
                                                    std::size_t key_offset,
                                                    const ParserConfig& config,
                                                    detail::Scanner& scanner, ErrorPtr& error) const;

  /**
   * @brief 记录一个「可恢复值」的边界（`RecoverValue` 的配对起点）。
   *
   * @param stack [in] 帧栈；**只读**，栈顶必须是该值的父容器帧（调用方保证非空）。
   * @param doc [in] 解析结果池；只读两级 scratch 深度，作为回滚目标。
   * @param scanner [in] 扫描器；只读当前偏移，作为同步扫描起点。
   * @param recovery [out] 恢复边界；`active` 置 true，其余字段按当前状态写入。
   * @return 无。
   *
   * @note 强制不内联：它只在 `error_policy != kAbort` 时执行，但内联进主循环
   *       会给热路径增加约 8 条赋值的代码体积；挪到冷函数、并由 `if constexpr`
   *       在 `kRecovering == false` 的实例里整体消除后，默认路径不受影响。
   */
  template <typename Stack>
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((noinline))
#endif
  void BeginRecoveryPoint(Stack& stack, const Document& doc, detail::Scanner& scanner,
                          detail::RecoveryPoint& recovery) const;

  /**
   * @brief 从「坏值」错误中恢复：回退到值边界、填 `null`、跳到同步点。
   *
   * @param stack [in,out] 帧栈；**入口时可能已压入坏值内部的若干帧**，成功返回时
   *        深度已回到 `recovery.depth`（坏值容器被丢弃，不提交半成品子表）。
   * @param doc [in,out] 解析结果池；成功时在父容器里追加一个 `null` 成员/元素，
   *        失败时可能已有半成品（由 `Parse` 统一 `Clear`）。
   * @param scanner [in,out] 扫描器；成功时游标停在同步点**之后**（逗号/闭合符已消费）。
   * @param config [in] 运行期配置；读取 `error_policy` 与 `max_errors`。
   * @param error [in,out] 入口是本次失败的错误对象；成功时被移入 `doc.Errors()`
   *        并清空，失败时保持为「导致无法恢复的那个错误」。
   * @param recovery [in,out] 恢复边界；成功时 `active` 置 false。
   *
   * @return
   * - `kContinued`：已恢复，父容器仍然打开——调用方**必须** `continue` 主循环；
   * - `kRootClosed`：已恢复且父容器是根——调用方**必须**置 `root_done` 并 `continue`，
   *   下一轮进入尾随空白检查；
   * - `kFailed`：不可恢复（预算耗尽/深度超限/重复键/同步点与父容器不匹配/
   *   坏值延伸到输入末尾）——调用方**必须** `return false`，不要再改 `error`。
   *
   * ## 定位
   * 场景 5（错误恢复）的唯一回退点。它同时改三样东西：帧栈、两级 scratch、游标。
   * 漏掉任何一样都会让后续正常解析读到错位数据——scratch 没回滚会把子容器成员
   * 当成父容器成员提交；游标没对齐会把坏值尾部当语法；帧栈没回退会让闭合符
   * 匹配到错误的容器。因此本函数是**全有或全无**的：中途任何一步失败都返回
   * `kFailed`，由调用方丢弃整个文档。
   *
   * ## 步骤
   * step1. 丢弃坏值已经压入的容器帧（不提交，直接弹）。
   * step2. 把成员/元素 scratch 回滚到值开始前的快照。
   * step3. 在父容器里放 `null` 占位，走与正常路径相同的成员提交点（保持
   *        `kLast`/`kError` 语义）。
   * step4. 从值起点做括号深度同步，找不到同步点则失败。
   * step5. 同步点必须与父容器类型匹配：对象吃 `,`/`}`，数组吃 `,`/`]`。
   * step6. 记录错误（`kSkipValue` 只留首个、`kCollect` 收到 `max_errors` 上限）并清空 `error`。
   *
   * ## 并发/失败语义
   * - 与 `Parse` 一样无共享状态；每个线程用自己的栈/文档；
   * - 失败时不保证 `doc`/`stack` 处于哪个中间态——调用方只能丢弃（`Parse` 会清空）；
   * - 不变量：成功返回后 `stack.size() == recovery.depth`，且父容器状态已推进到
   *   「值之后」，主循环可直接继续。
   *
   * ## 已知限制
   * 只恢复「容器内某个值坏了」。以下情况按 `kAbort` 处理（`Parse` 返回 false）：
   * 根值本身坏、缺冒号/缺逗号/键不是字符串等结构错误、`kTimeout`、`kTooLarge`、
   * `kDepthExceeded`、`kDuplicateKey`（`kError` 策略）、`kTrailingGarbage`，以及
   * 坏值延伸到输入末尾（未闭合，没有同步点）。
   */
  template <typename Stack>
  detail::RecoverResult RecoverValue(Stack& stack, Document& doc, detail::Scanner& scanner,
                                     const ParserConfig& config, ErrorPtr& error,
                                     detail::RecoveryPoint& recovery) const;
};

/// @brief 默认解析器别名。
using Parser = BasicParser<InlinePolicy>;

// ---------------------------------------------------------------------------
// 实现（模板必须对调用方可见；非模板的 Scanner 原语在 parser.cpp）
// ---------------------------------------------------------------------------

/// @note `always_inline` 是实测选择：`Parse` 里多出一条恢复分派分支后，GCC 的
///       自动内联会在调用方循环里放弃内联（-O2 下 `--param max-inline-insns-auto
///       limit reached`），`Document&` 的 IPA-SRA 克隆随之消失，实测每次解析多
///       1.5% 指令、分支多 1.1%。强制内联后指令数 +0.15%、分支数与改动前**完全
///       相同**（547 B 小文档 × 10 万次实测）。代价是每个调用点展开一份状态机
///       （测试 TU 实测 +8 KB）。
template <typename Policy>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline bool BasicParser<Policy>::Parse(std::string_view input, Document& doc, ErrorPtr& error,
                                       const ParserConfig& config) const {
  // 入口与出口统一失败语义：失败后 doc 必须是空文档，error 只能是本次的错误。
  doc.Clear();
  error.reset();
  // error_policy 是运行期配置。默认 InlinePolicy 的分派写成「恢复路径早退」：
  // 恢复是冷路径，用 `detail::Unlikely` 把它挪出直线代码后，kAbort 分支与改动前的
  // 实现逐条一致（`ParseAbortImpl` 内联进来就是原来的状态机）。
  if (detail::Unlikely(config.error_policy != ErrorPolicy::kAbort)) {
    return ParseRecoverEntry(input, doc, error, config);
  }
  const bool ok = ParseAbortImpl(input, doc, error, config);
  if (!ok) {
    doc.Clear();  // 丢弃失败时可能已写入的半成品池（如 "[1," 的部分数组）。
  }
  return ok;
}

template <typename Policy>
ParseOutcome BasicParser<Policy>::ParsePartial(std::string_view input, Document& doc,
                                               ErrorPtr& error,
                                               const ParserConfig& config) const {
  // step1: 与 Parse 相同的入口清理；区别是失败时**不清空** doc，保留半成品。
  doc.Clear();
  error.reset();

  // step2: 走同一条解析主体；成功/失败由它决定（与 Parse 相同的分派规则）。
  ParseOutcome outcome;
  if constexpr (std::is_same_v<Policy, InlinePolicy>) {
    if (config.error_policy != ErrorPolicy::kAbort) {
      outcome.ok = ParseRecoverImpl(input, doc, error, config);
    } else {
      outcome.ok = ParseAbortImpl(input, doc, error, config);
    }
  } else {
    if (config.error_policy != ErrorPolicy::kAbort) {
      outcome.ok = ParseRecoverLimits<true>(input, doc, error, config);
    } else {
      outcome.ok = ParseInternal(input, doc, error, config);
    }
  }

  // step3: 成功时 consumed = 输入长度；失败时取 JsonError 的偏移（无错误对象则 0）。
  if (outcome.ok) {
    outcome.consumed = input.size();
    return outcome;
  }
  const auto* json_error = dynamic_cast<const JsonError*>(error.get());
  outcome.consumed = json_error != nullptr ? json_error->Offset() : 0;
  return outcome;
}

template <typename Policy>
std::size_t BasicParser<Policy>::ParseMany(
    std::string_view input, const std::function<bool(Document&)>& on_document, ErrorPtr& error,
    const ParserConfig& config) const {
  // step1: 逐行切分；每行独立解析，文档回调后立刻丢弃（内存 O(单行)）。
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset <= input.size()) {
    const std::size_t newline = input.find('\n', offset);
    const std::size_t end = newline == std::string_view::npos ? input.size() : newline;
    std::string_view line = input.substr(offset, end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const bool blank = line.find_first_not_of(" \t\r") == std::string_view::npos;
    // step2: 非空行才解析；失败立即返回已成功数（error 带当前行位置）。
    if (!blank) {
      Document doc;
      if (!Parse(line, doc, error, config)) {
        return count;
      }
      if (!on_document(doc)) {
        return count;
      }
      ++count;
    }
    if (newline == std::string_view::npos) {
      break;
    }
    offset = newline + 1;
  }
  return count;
}

template <typename Policy>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline detail::MemberSubmitResult BasicParser<Policy>::SubmitObjectMember(
    Document& doc, detail::Frame& frame, std::string_view key, std::uint32_t value_index,
    std::size_t key_offset, const ParserConfig& config, detail::Scanner& scanner,
    ErrorPtr& error) const {
  // step1: 直接写入模式只在「本容器成员仍占据成员池末尾」时成立。一旦子容器把
  //        自己的成员提交到池尾，本容器成员区段就不再连续：把已直接写入的成员
  //        复制进 scratch，后续成员改走「先收集、闭合时一次提交」的旧路径。
  //        被打断之前写入的成员一定连续（子段从池尾开始插入），复制是安全的。
  if (!frame.spilled && doc.MemberBegin() != frame.member_begin + frame.direct_count) {
    frame.scratch_begin = doc.BeginScratchFromMembers(frame.member_begin, frame.direct_count);
    frame.spilled = true;
  }

  // step2: 默认策略保持历史行为：重复成员全部保留，Find/operator[] 返回首个。
  //        本容器还没有成员时也不可能重复，一并走零检测分支。
  if (config.duplicate_key == DuplicateKeyPolicy::kFirst || frame.direct_count == 0) {
    if (frame.spilled) {
      doc.PushMemberScratch(key, value_index);
    } else {
      doc.PushMember(key, value_index);
    }
    ++frame.direct_count;
    return detail::MemberSubmitResult::kAppended;
  }

  // step3: 只在当前容器的直接成员区间内查重（O(成员数)，仅非默认策略触发）。
  //        拆到独立函数：默认 kFirst 路径因此可以被 `always_inline` 完整内联，
  //        调用点只剩「spill 检查 + 一次追加」，深嵌套每层省一次 call/ret 与
  //        寄存器保存。
  return SubmitObjectMemberDedup(doc, frame, key, value_index, key_offset, config, scanner, error);
}

template <typename Policy>
detail::MemberSubmitResult BasicParser<Policy>::SubmitObjectMemberDedup(
    Document& doc, detail::Frame& frame, std::string_view key, std::uint32_t value_index,
    std::size_t key_offset, const ParserConfig& config, detail::Scanner& scanner,
    ErrorPtr& error) const {
  for (std::uint32_t index = 0; index < frame.direct_count; ++index) {
    Member& member = frame.spilled ? doc.m_member_scratch[frame.scratch_begin + index]
                                   : doc.m_members[frame.member_begin + index];
    if (member.key != key) {
      continue;
    }
    // step4: kError：位置取重复出现的那个 key 的起始引号。
    if (config.duplicate_key == DuplicateKeyPolicy::kError) {
      (void)scanner.FailAtOffset(JsonFailure::kDuplicateKey, key_offset, error);
      return detail::MemberSubmitResult::kFailed;
    }
    // step5: kLast：保留首次出现的位置，值改为最后一次出现的值。
    member.value_index = value_index;
    return detail::MemberSubmitResult::kMerged;
  }

  if (frame.spilled) {
    doc.PushMemberScratch(key, value_index);
  } else {
    doc.PushMember(key, value_index);
  }
  ++frame.direct_count;
  return detail::MemberSubmitResult::kAppended;
}

template <typename Policy>
template <typename Stack>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void BasicParser<Policy>::BeginRecoveryPoint(Stack& stack, const Document& doc,
                                             detail::Scanner& scanner,
                                             detail::RecoveryPoint& recovery) const {
  const detail::Frame& parent = detail::TopFrame(stack);
  recovery.active = true;
  recovery.depth = stack.size();
  recovery.value_offset = scanner.Offset();
  recovery.member_before = doc.MemberScratchSize();
  recovery.element_before = doc.ElementScratchSize();
  recovery.key = parent.key;
  recovery.key_offset = parent.key_offset;
  recovery.container_started = false;
}

template <typename Policy>
template <typename Stack>
detail::RecoverResult BasicParser<Policy>::RecoverValue(Stack& stack, Document& doc,
                                                        detail::Scanner& scanner,
                                                        const ParserConfig& config,
                                                        ErrorPtr& error,
                                                        detail::RecoveryPoint& recovery) const {
  // 前置：调用方已把本次失败写入 error；recovery.active 说明失败发生在某个值内部。
  const auto* json_error = dynamic_cast<const JsonError*>(error.get());
  if (json_error == nullptr || !detail::IsRecoverableFailure(json_error->Failure())) {
    return detail::RecoverResult::kFailed;
  }

  // step1: 丢弃坏值已经压入的容器帧。它们没有闭合，不能提交；必须回到
  //        「值的父容器」这一层，step5 的同步点才能与父容器正确对齐。
  while (stack.size() > recovery.depth) {
    detail::PopFrame(stack);
  }

  // step2: 把两级 scratch 回滚到值开始前的快照。子容器帧里收集的成员/元素若残留，
  //        父容器闭合时会被误当成自己的直接子项提交（成员数翻倍、顺序错乱）。
  doc.RollbackScratch(recovery.member_before, recovery.element_before);

  // step3: 在父容器里给坏值留一个 null 占位。个数与顺序因此保持不变——调用方按
  //        原路径遍历时只会看到一个 null，而不是从坏值处开始整体错位。
  const std::uint32_t null_index = doc.PushNode(detail::Node(detail::NodeKind::kNull));
  detail::Frame& parent = detail::TopFrame(stack);
  if (parent.is_object) {
    // 走与正常路径同一个提交点，保持 kLast（合并）/ kError（报重复）的语义一致。
    if (SubmitObjectMember(doc, parent, recovery.key, null_index, recovery.key_offset, config,
                           scanner, error) == detail::MemberSubmitResult::kFailed) {
      return detail::RecoverResult::kFailed;
    }
    parent.key = std::string_view{};
    parent.state = detail::ContainerState::kObjectAfterValue;
  } else {
    doc.PushElementScratch(null_index);
    parent.state = detail::ContainerState::kArrayAfterValue;
  }

  // step4: 从值起点做括号深度同步（不是从出错点：出错点在字符串内部时，
  //        从出错点扫描会把闭合引号误当成开引号，吞掉后续成员）。
  if (!scanner.SyncToValueEnd(recovery.value_offset) || scanner.AtEnd()) {
    return detail::RecoverResult::kFailed;  // 无同步点：坏值延伸到输入末尾
  }

  // step5: 同步点必须与父容器类型匹配，否则说明结构已经错乱，不再猜。
  const char sync = scanner.Peek();
  detail::RecoverResult result = detail::RecoverResult::kContinued;
  if (sync == ',') {
    scanner.Advance();
    parent.state = parent.is_object ? detail::ContainerState::kObjectKey
                                    : detail::ContainerState::kArrayValue;
  } else if (sync == (parent.is_object ? '}' : ']')) {
    scanner.Advance();
    if (CloseContainer(stack, doc)) {
      result = detail::RecoverResult::kRootClosed;
    }
  } else {
    return detail::RecoverResult::kFailed;
  }

  // step6: 记录错误并按策略裁剪。kSkipValue 只留首个（调用方要完整清单请用
  //        kCollect）；max_errors == 0 表示「恢复但不记录」。恢复成功的 Parse
  //        必须满足「成功时 error 为空」的既有契约，所以这里移走并清空。
  const bool keep_error = config.max_errors != 0 &&
                          (config.error_policy == ErrorPolicy::kCollect
                               ? doc.ErrorCount() < config.max_errors
                               : doc.ErrorCount() == 0);
  if (keep_error) {
    doc.PushError(std::move(error));
  }
  error.reset();
  recovery.active = false;
  return result;
}

template <typename Policy>
bool BasicParser<Policy>::ParseInternal(std::string_view input, Document& doc, ErrorPtr& error,
                                        const ParserConfig& config) const {
  // 运行期只分叉一次：默认配置（max=0 且 validate=false）走 kChecks=false 实例，
  // 生成的指令与未加检查功能时逐条一致；两项任一开启时走 noinline 的冷入口。
  if (config.max_string_bytes != 0 || config.validate_utf8) {
    return ParseInternalChecked(input, doc, error, config);
  }
  return ParseInternalBody<false, false>(input, doc, error, config);
}

template <typename Policy>
template <bool kRecovering>
bool BasicParser<Policy>::ParseRecoverLimits(std::string_view input, Document& doc,
                                             ErrorPtr& error, const ParserConfig& config) const {
  // 与 ParseInternal 同一条 limits 分派，只是恢复维度固定为 true。
  if (config.max_string_bytes != 0 || config.validate_utf8) {
    return ParseInternalBody<true, kRecovering>(input, doc, error, config);
  }
  return ParseInternalBody<false, kRecovering>(input, doc, error, config);
}

template <typename Policy>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool BasicParser<Policy>::ParseRecoverEntry(std::string_view input, Document& doc,
                                            ErrorPtr& error,
                                            const ParserConfig& config) const {
  bool recovered = false;
  if constexpr (std::is_same_v<Policy, InlinePolicy>) {
    recovered = ParseRecoverImpl(input, doc, error, config);
  } else {
    recovered = ParseRecoverLimits<true>(input, doc, error, config);
  }
  if (!recovered) {
    doc.Clear();  // 与 Parse 的失败语义一致：不可恢复时文档必须是空文档。
  }
  return recovered;
}

template <typename Policy>
bool BasicParser<Policy>::ParseInternalChecked(std::string_view input, Document& doc,
                                               ErrorPtr& error,
                                               const ParserConfig& config) const {
  return ParseInternalBody<true, false>(input, doc, error, config);
}

template <typename Policy>
template <bool kChecks, bool kRecovering>
bool BasicParser<Policy>::ParseInternalBody(std::string_view input, Document& doc,
                                            ErrorPtr& error,
                                            const ParserConfig& config) const {
  // step0: 可选 BOM 跳过（UTF-8 BOM = EF BB BF）。
  if (config.allow_bom && input.size() >= 3 &&
      static_cast<unsigned char>(input[0]) == 0xEF &&
      static_cast<unsigned char>(input[1]) == 0xBB &&
      static_cast<unsigned char>(input[2]) == 0xBF) {
    input.remove_prefix(3);
  }
  detail::Scanner scanner(input, doc, config.max_string_bytes, config.validate_utf8);
  // step0b: 输入字节上限（GB 级输入防护：超限直接报错，不进 DOM）。
  if (config.max_input_bytes != 0 && input.size() > config.max_input_bytes) {
    return scanner.Fail(JsonFailure::kTooLarge, error);
  }
  const std::size_t max_depth =
      config.max_depth != 0
          ? config.max_depth
          : (std::min)(static_cast<std::size_t>(input.size() / 2 + 1),
                       static_cast<std::size_t>(65536));

  doc.ReserveFor(input.size());

  // 常见形态：根是「成员值全为标量」的平铺对象。快速路径失败会回退到通用
  // 状态机重新解析，因此这里的失败不改变对外错误语义。
  // 非默认重复 key 策略下整体绕过快路径：快路径不做重复检测（默认路径零成本），
  // 由通用状态机统一处理，避免两条路径对同一输入给出不同结果。
  scanner.SkipWhitespace();
  if (config.duplicate_key == DuplicateKeyPolicy::kFirst && !scanner.AtEnd() &&
      scanner.Peek() == '{') {
    if (TryParseFlatObjectBody<kChecks>(doc, scanner, error)) {
      return true;
    }
    doc.Clear();
    error.reset();
    scanner = detail::Scanner(input, doc, config.max_string_bytes, config.validate_utf8);
  }

  typename Policy::template Stack<detail::Frame> stack;
  bool root_done = false;

  // 场景 5：错误恢复边界。`kRecovering == false`（默认 `kAbort`）时它不会被
  // 读写——恢复边界的记录/清除、失败分支的恢复尝试全部被 `if constexpr`
  // 编译期消除，`ParseInternalBody<..., false>` 里不含任何恢复代码；边界类型本身
  // 也降级成 1 B 的空结构，默认路径不为它预留 56 B 栈槽位。
  using RecoveryState =
      std::conditional_t<kRecovering, detail::RecoveryPoint, detail::RecoveryStub>;
  [[maybe_unused]] RecoveryState recovery;
  (void)recovery;

  // 失败统一出口（error 已由 scanner 写入）：返回 true = 已恢复，调用方 continue。
  auto try_recover = [&]() -> bool {
    if constexpr (!kRecovering) {
      return false;
    } else {
      if (!recovery.active) {
        return false;
      }
      const detail::RecoverResult result =
          RecoverValue(stack, doc, scanner, config, error, recovery);
      if (result == detail::RecoverResult::kFailed) {
        return false;
      }
      if (result == detail::RecoverResult::kRootClosed) {
        root_done = true;
      }
      return true;
    }
  };

  // 失败统一出口（原因版）：先写 error，再尝试恢复；true = 调用方 continue。
  auto try_fail = [&](JsonFailure failure) -> bool {
    (void)scanner.Fail(failure, error);
    return try_recover();
  };

  // 失败出口：`kAbort` 实例必须与历史实现逐字节一致——直接 `return scanner.Fail(...)`
  // （尾调用、错误路径零分支）。若写成「先 Fail 再判断 try_recover」的统一出口，
  // 每个错误点都会多出一次非尾调用与一个分支，实测默认解析 +2~3 ns。
  // 恢复实例才展开 `try_fail` / `try_recover`。宏在本函数末尾 #undef。
#define EXTNED_JSON_FAIL(failure)          \
  if constexpr (!kRecovering) {            \
    return scanner.Fail((failure), error); \
  } else if (try_fail(failure)) {          \
    continue;                              \
  } else {                                 \
    return false;                          \
  }

#define EXTNED_JSON_RECOVER() \
  if (try_recover()) {        \
    continue;                 \
  } else {                    \
    return false;             \
  }

  // step2.5: 可选时间预算；每 256 轮检查一次，避免每轮读时钟。
  using Clock = std::chrono::steady_clock;
  const bool has_deadline = config.timeout_ms != 0;
  const Clock::time_point deadline =
      has_deadline ? Clock::now() + std::chrono::milliseconds(config.timeout_ms)
                   : Clock::time_point{};
  std::uint32_t tick = 0;



  // step2: 每轮推进一个语法步骤；所有分支都必须消耗字节或改变帧状态。
  for (;;) {
    if (has_deadline && ((++tick & 255u) == 0u) && Clock::now() >= deadline) {
      return scanner.Fail(JsonFailure::kTimeout, error);
    }
    // 根值完成后，只允许尾随空白。
    if (stack.empty() && root_done) {
      scanner.SkipWhitespace();
      if (!scanner.AtEnd()) {
        return scanner.Fail(JsonFailure::kTrailingGarbage, error);
      }
      return true;
    }

    bool need_value = false;
    if (stack.empty()) {
      need_value = true;  // 根值
    } else {
      detail::Frame& frame = detail::TopFrame(stack);
      if (frame.is_object) {
        switch (frame.state) {
          case detail::ContainerState::kObjectFirst:
          case detail::ContainerState::kObjectKey: {
            scanner.SkipWhitespace();
            if (scanner.AtEnd()) {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
            }
            if (scanner.Peek() == '}') {
              if (frame.state == detail::ContainerState::kObjectKey &&
                  !config.allow_trailing_comma) {
                EXTNED_JSON_FAIL(JsonFailure::kTrailingComma);
              }
              scanner.Advance();
              if constexpr (!kRecovering) {
                if (stack.size() >= 8) {
                  root_done = CloseChain(stack, doc, scanner) || root_done;
                } else {
                  root_done = CloseContainer(stack, doc) || root_done;
                }
              } else {
                root_done = CloseContainer(stack, doc) || root_done;
                if (recovery.active && recovery.container_started &&
                    stack.size() == recovery.depth) {
                  recovery.active = false;
                }
              }
              continue;
            }
            if (scanner.Peek() != '"') {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
            }
            // 先记下键的起始偏移：kError 策略下重复 key 的错误位置指向这里
            // （开引号所在字节），而不是值解析完后的游标位置。
            const std::size_t key_offset = scanner.Offset();
            if (!scanner.ParseStringView(frame.key, error)) {
              EXTNED_JSON_RECOVER();
            }
            if (!CheckStringToken<kChecks>(scanner, key_offset, error)) {
              EXTNED_JSON_RECOVER();
            }
            frame.key_offset = key_offset;
            // 同一轮里继续读冒号并落到下面的「读值」逻辑：每个成员省掉
            // 两次完整的状态机迭代（冒号一轮、值一轮）。
            scanner.SkipWhitespace();
            if (scanner.AtEnd()) {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
            }
            if (scanner.Peek() != ':') {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
            }
            scanner.Advance();
            frame.state = detail::ContainerState::kObjectValue;
            need_value = true;
            break;
          }
          case detail::ContainerState::kObjectColon:
            scanner.SkipWhitespace();
            if (scanner.AtEnd()) {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
            }
            if (scanner.Peek() != ':') {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
            }
            scanner.Advance();
            frame.state = detail::ContainerState::kObjectValue;
            continue;
          case detail::ContainerState::kObjectValue:
            need_value = true;
            break;
          case detail::ContainerState::kObjectAfterValue:
            scanner.SkipWhitespace();
            if (scanner.AtEnd()) {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
            }
            if (scanner.Peek() == ',') {
              scanner.Advance();
              frame.state = detail::ContainerState::kObjectKey;
              continue;
            }
            if (scanner.Peek() == '}') {
              scanner.Advance();
              if constexpr (!kRecovering) {
                if (stack.size() >= 8) {
                  root_done = CloseChain(stack, doc, scanner) || root_done;
                } else {
                  root_done = CloseContainer(stack, doc) || root_done;
                }
              } else {
                root_done = CloseContainer(stack, doc) || root_done;
                if (recovery.active && recovery.container_started &&
                    stack.size() == recovery.depth) {
                  recovery.active = false;
                }
              }
              continue;
            }
            EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
          default:
            EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
        }
      } else {
        switch (frame.state) {
          case detail::ContainerState::kArrayFirst:
          case detail::ContainerState::kArrayValue:
            scanner.SkipWhitespace();
            if (scanner.AtEnd()) {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
            }
            if (scanner.Peek() == ']') {
              if (frame.state == detail::ContainerState::kArrayValue &&
                  !config.allow_trailing_comma) {
                EXTNED_JSON_FAIL(JsonFailure::kTrailingComma);
              }
              scanner.Advance();
              if constexpr (!kRecovering) {
                if (stack.size() >= 8) {
                  root_done = CloseChain(stack, doc, scanner) || root_done;
                } else {
                  root_done = CloseContainer(stack, doc) || root_done;
                }
              } else {
                root_done = CloseContainer(stack, doc) || root_done;
                if (recovery.active && recovery.container_started &&
                    stack.size() == recovery.depth) {
                  recovery.active = false;
                }
              }
              continue;
            }
            need_value = true;
            break;
          case detail::ContainerState::kArrayAfterValue:
            scanner.SkipWhitespace();
            if (scanner.AtEnd()) {
              EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
            }
            if (scanner.Peek() == ',') {
              scanner.Advance();
              frame.state = detail::ContainerState::kArrayValue;
              continue;
            }
            if (scanner.Peek() == ']') {
              scanner.Advance();
              if constexpr (!kRecovering) {
                if (stack.size() >= 8) {
                  root_done = CloseChain(stack, doc, scanner) || root_done;
                } else {
                  root_done = CloseContainer(stack, doc) || root_done;
                }
              } else {
                root_done = CloseContainer(stack, doc) || root_done;
                if (recovery.active && recovery.container_started &&
                    stack.size() == recovery.depth) {
                  recovery.active = false;
                }
              }
              continue;
            }
            EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
          default:
            EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
        }
      }
    }

    if (!need_value) {
      continue;
    }

    // step3: 读一个值的开头；容器压帧，标量直接写入父容器的成员/元素池。
    scanner.SkipWhitespace();
    // 场景 5：记录「当前正在解析的值」的恢复边界。每个值位置都记录，值完成后
    // （标量提交 / 容器闭合）清除——因此错误发生时边界总是指向**坏值本身**：
    // 坏值在哪个成员/元素位置，就在那里填 null，兄弟成员/元素不受影响。
    // 根值不记录（根坏掉没有父容器可回退，按不可恢复处理）。
    if constexpr (kRecovering) {
      if (!stack.empty()) {
        BeginRecoveryPoint(stack, doc, scanner, recovery);
      }
    }
    if (scanner.AtEnd()) {
      EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
    }
    const char first = scanner.Peek();
    if (first == '{' || first == '[') {
      if (stack.size() >= max_depth) {
        // 深度防线不可恢复：恢复路径不允许靠「跳过」绕过防爆栈限制。
        return scanner.Fail(JsonFailure::kDepthExceeded, error);
      }
      const bool is_object = first == '{';
      scanner.Advance();
      if constexpr (kRecovering) {
        if (recovery.active) {
          recovery.container_started = true;  // 值本身是容器：闭合时才清恢复边界
        }
      }
      const std::uint32_t node_index = doc.NodeBegin();
      doc.PushNodeSlotBare(is_object ? detail::NodeKind::kObject : detail::NodeKind::kArray);
      // 先把本容器挂到父容器（父帧的 scratch 追加一项），再记录子帧起点——
      // 顺序反了会把父容器的这一项算进子容器的直接子项区间。
      if (stack.empty()) {
        doc.SetRoot(node_index);
      } else {
        detail::Frame& parent = detail::TopFrame(stack);
        if (parent.is_object) {
          if (SubmitObjectMember(doc, parent, parent.key, node_index, parent.key_offset, config,
                                 scanner, error) == detail::MemberSubmitResult::kFailed) {
            return false;
          }
          parent.key = std::string_view{};
          parent.state = detail::ContainerState::kObjectAfterValue;
        } else {
          doc.PushElementScratch(node_index);
          parent.state = detail::ContainerState::kArrayAfterValue;
        }
      }
      detail::Frame& next = detail::EmplaceFrame(stack);
      next.node_index = node_index;
      next.member_begin = is_object ? doc.MemberBegin() : 0;
      next.scratch_begin =
          is_object ? 0 : static_cast<std::uint32_t>(doc.ElementScratchSize());
      next.state = is_object ? detail::ContainerState::kObjectFirst
                             : detail::ContainerState::kArrayFirst;
      next.is_object = is_object;
      continue;
    }

    const std::uint32_t value_index = doc.NodeBegin();
    detail::Node& value = doc.PushNodeSlotBare(detail::NodeKind::kNull);
    switch (first) {
      case '"': {
        // 默认实例里 `kChecks ? scanner.Offset() : 0` 折叠为常量 0，不留指令。
        const std::size_t string_offset = kChecks ? scanner.Offset() : 0;
        if (!scanner.ParseString(value.payload.text, error)) {
          doc.PopNode();
          EXTNED_JSON_RECOVER();
        }
        if (!CheckStringToken<kChecks>(scanner, string_offset, error)) {
          doc.PopNode();
          EXTNED_JSON_RECOVER();
        }
        value.kind = detail::NodeKind::kString;
        break;
      }
      case 't':
        if (!scanner.ParseLiteral("true", value, error)) {
          doc.PopNode();
          EXTNED_JSON_RECOVER();
        }
        break;
      case 'f':
        if (!scanner.ParseLiteral("false", value, error)) {
          doc.PopNode();
          EXTNED_JSON_RECOVER();
        }
        break;
      case 'n':
        if (!scanner.ParseLiteral("null", value, error)) {
          doc.PopNode();
          EXTNED_JSON_RECOVER();
        }
        break;
      default:
        if (first == '-' || (first >= '0' && first <= '9')) {
          const std::size_t number_offset = kChecks ? scanner.Offset() : 0;
          if (!scanner.ParseNumber(value, error)) {
            doc.PopNode();
            EXTNED_JSON_RECOVER();
          }
          if (!CheckNumberToken<kChecks>(scanner, number_offset, error)) {
            doc.PopNode();
            EXTNED_JSON_RECOVER();
          }
        } else {
          doc.PopNode();
          EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
        }
        break;
    }

    if (stack.empty()) {
      doc.SetRoot(value_index);
      root_done = true;
      continue;
    }

    detail::Frame& parent = detail::TopFrame(stack);
    if (parent.is_object) {
      if (SubmitObjectMember(doc, parent, parent.key, value_index, parent.key_offset, config,
                             scanner, error) == detail::MemberSubmitResult::kFailed) {
        return false;
      }
      parent.key = std::string_view{};
    } else {
      doc.PushElementScratch(value_index);
    }
    // 场景 5：标量值提交完成——它就是恢复边界对应的值，边界到此结束。
    if constexpr (kRecovering) {
      if (recovery.active && !recovery.container_started &&
          stack.size() == recovery.depth) {
        recovery.active = false;
      }
    }
    // 标量值之后就地消费 `,` 或闭合符号：把 `kObjectAfterValue` /
    // `kArrayAfterValue` 这一轮迭代省掉（常见输入的成员全是标量）。
    scanner.SkipWhitespace();
    if (scanner.AtEnd()) {
      EXTNED_JSON_FAIL(JsonFailure::kUnexpectedEnd);
    }
    const char next = scanner.Peek();
    if (next == ',') {
      scanner.Advance();
      parent.state = parent.is_object ? detail::ContainerState::kObjectKey
                                      : detail::ContainerState::kArrayValue;
      continue;
    }
    if (next == (parent.is_object ? '}' : ']')) {
      scanner.Advance();
      if constexpr (!kRecovering) {
        if (stack.size() >= 8) {
          root_done = CloseChain(stack, doc, scanner) || root_done;
        } else {
          root_done = CloseContainer(stack, doc) || root_done;
        }
      } else {
        root_done = CloseContainer(stack, doc) || root_done;
        if (recovery.active && recovery.container_started &&
            stack.size() == recovery.depth) {
          recovery.active = false;
        }
      }
      continue;
    }
    EXTNED_JSON_FAIL(JsonFailure::kUnexpectedChar);
  }
#undef EXTNED_JSON_FAIL
#undef EXTNED_JSON_RECOVER
}

template <typename Policy>
template <bool kChecks>
bool BasicParser<Policy>::TryParseFlatObjectBody(Document& doc, detail::Scanner& scanner,
                                                 ErrorPtr& error) const {
  // 前置条件：当前字节是 '{'（调用方已确认）。
  scanner.Advance();
  const std::uint32_t node_index = doc.NodeBegin();
  doc.PushNodeSlotBare(detail::NodeKind::kObject);
  // 平铺对象是整次解析里唯一的容器帧：成员直接追加到全局池即可保持连续，
  // 不需要 scratch 中转（省掉每成员一次 24 B 的临时写与一次块拷贝）。
  const std::uint32_t member_begin = doc.MemberBegin();

  bool has_member = false;
  for (;;) {
    scanner.SkipWhitespace();
    if (scanner.AtEnd()) {
      return false;
    }
    if (scanner.Peek() == '}') {
      // 尾逗号：交给通用路径按配置决定是否接受（并给出准确错误位置）。
      if (has_member) {
        return false;
      }
      scanner.Advance();
      break;
    }
    if (scanner.Peek() != '"') {
      return false;
    }

    std::string_view key;
    const std::size_t key_offset = kChecks ? scanner.Offset() : 0;
    if (!scanner.ParseStringView(key, error)) {
      return false;
    }
    if (!CheckStringToken<kChecks>(scanner, key_offset, error)) {
      return false;
    }
    scanner.SkipWhitespace();
    if (scanner.AtEnd() || scanner.Peek() != ':') {
      return false;
    }
    scanner.Advance();
    scanner.SkipWhitespace();
    if (scanner.AtEnd()) {
      return false;
    }

    const std::uint32_t value_index = doc.NodeBegin();
    detail::Node& value = doc.PushNodeSlotBare(detail::NodeKind::kNull);
    const char first = scanner.Peek();
    switch (first) {
      case '"': {
        const std::size_t string_offset = kChecks ? scanner.Offset() : 0;
        if (!scanner.ParseString(value.payload.text, error)) {
          return false;
        }
        if (!CheckStringToken<kChecks>(scanner, string_offset, error)) {
          return false;
        }
        value.kind = detail::NodeKind::kString;
        break;
      }
      case 't':
        if (!scanner.ParseLiteral("true", value, error)) {
          return false;
        }
        break;
      case 'f':
        if (!scanner.ParseLiteral("false", value, error)) {
          return false;
        }
        break;
      case 'n':
        if (!scanner.ParseLiteral("null", value, error)) {
          return false;
        }
        break;
      default:
        if (first == '-' || (first >= '0' && first <= '9')) {
          const std::size_t number_offset = kChecks ? scanner.Offset() : 0;
          if (!scanner.ParseNumber(value, error)) {
            return false;
          }
          if (!CheckNumberToken<kChecks>(scanner, number_offset, error)) {
            return false;
          }
        } else {
          // 容器或其他非标量：回退通用状态机。
          return false;
        }
        break;
    }
    doc.PushMember(key, value_index);
    has_member = true;

    scanner.SkipWhitespace();
    if (scanner.AtEnd()) {
      return false;
    }
    const char next = scanner.Peek();
    if (next == ',') {
      scanner.Advance();
      continue;
    }
    if (next == '}') {
      scanner.Advance();
      break;
    }
    return false;
  }

  const std::uint32_t begin = member_begin;
  const std::uint32_t count = doc.MemberBegin() - begin;
  doc.AdvanceMemberCount(count);
  doc.PatchSpan(node_index, begin, count);
  doc.SetRoot(node_index);
  scanner.SkipWhitespace();
  return scanner.AtEnd();
}

template <typename Policy>
template <typename Stack>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline bool BasicParser<Policy>::CloseContainer(Stack& stack, Document& doc) const {
  const detail::Frame& frame = detail::TopFrame(stack);
  const std::uint32_t node_index = frame.node_index;
  const std::uint32_t scratch_begin = frame.scratch_begin;
  const std::uint32_t member_begin = frame.member_begin;
  const std::uint32_t direct_count = frame.direct_count;
  const bool spilled = frame.spilled;
  const bool is_object = frame.is_object;
  const bool is_root = stack.size() == 1;
  detail::PopFrame(stack);

  // 对象：直接写入模式的成员已经连续位于成员池 [member_begin, +direct_count)，
  // 只需补记「已提交」计数；被插入打断过的容器才把 scratch 段整块提交到池尾。
  std::uint32_t begin = 0;
  std::uint32_t count = 0;
  if (is_object) {
    if (spilled) {
      begin = doc.CommitMembers(scratch_begin);
    } else {
      begin = member_begin;
      doc.AdvanceMemberCount(direct_count);
    }
    count = direct_count;
  } else {
    count = static_cast<std::uint32_t>(doc.ElementScratchSize() - scratch_begin);
    begin = doc.CommitElements(scratch_begin);
  }
  doc.PatchSpan(node_index, begin, count);
  return is_root;
}

template <typename Policy>
template <typename Stack>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool BasicParser<Policy>::CloseChain(Stack& stack, Document& doc,
                                     detail::Scanner& scanner) const {
  for (;;) {
    // step1: 闭合当前容器；返回 true 说明刚闭合的是根。
    if (CloseContainer(stack, doc)) {
      return true;
    }
    // step2: 父帧不在 AfterValue 说明闭合序列中断（如恢复路径的占位帧）。
    detail::Frame& up = detail::TopFrame(stack);
    if (up.state != detail::ContainerState::kObjectAfterValue &&
        up.state != detail::ContainerState::kArrayAfterValue) {
      return false;
    }
    scanner.SkipWhitespace();
    if (scanner.AtEnd()) {
      return false;
    }
    const char next = scanner.Peek();
    // step3: 逗号——本容器还有兄弟成员/元素，交回主状态机读下一个值。
    if (next == ',') {
      scanner.Advance();
      up.state = up.is_object ? detail::ContainerState::kObjectKey
                              : detail::ContainerState::kArrayValue;
      return false;
    }
    // step4: 闭合符——父容器也结束，继续向上闭合；其它字节交给主循环报错。
    if (next != (up.is_object ? '}' : ']')) {
      return false;
    }
    scanner.Advance();
  }
}

/**
 * @brief 默认 `kAbort` 路径的实现体（`InlinePolicy` 专用；见类内声明的详细说明）。
 *
 * @details 为什么放在头文件：默认 `Parser` 的调用方 TU 需要看到这份定义，才能让
 *          `Parse` 与状态机一起内联、让 GCC 的 IPA-SRA 把 `[clone .isra.0]`
 *          特化版本做出来（与改动前的基线处于同一优化上下文）。恢复路径
 *          `ParseInternalBody<..., true>` 则被 `ParseRecoverImpl`（定义在 `parser.cpp`）
 *          挡住，不会进入默认 TU。
 *
 * @note `kAbort` 下**不含任何恢复代码**：恢复边界的记录/清除、失败分支的恢复
 *       尝试全部被 `if constexpr (kRecovering)` 在编译期消除。
 */
template <typename Policy>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline bool BasicParser<Policy>::ParseAbortImpl(std::string_view input, Document& doc,
                                                ErrorPtr& error,
                                                const ParserConfig& config) const {
  return ParseInternal(input, doc, error, config);
}

}  // namespace go2cpp::json
