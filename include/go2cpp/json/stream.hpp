#pragma once

/**
 * @file stream.hpp
 * @brief 顶层数组的流式分块解析：`bgo2cpp::json::ParseArrayStream`。
 *
 * ## 解决什么问题
 * `Parser::Parse` 会为**整份输入**建 DOM：节点池、成员池、元素索引池都按
 * `input.size()` 预分配（见 `Document::ReserveFor`）。GB 级的「一个大数组」
 * 因此需要与输入同量级（常数倍）的常驻内存，进程 RSS 直接跟着输入涨。
 *
 * 但「大数组」的消费方式通常是**逐元素独立处理**（落库、转发、聚合），
 * 每个元素处理完就可以丢掉。本文件提供这条路：在输入缓冲上按元素切分，
 * 每次只把**一个元素**喂给现有 `Parser`，解析完立刻回调，文档复用同一块
 * 池。峰值内存 = 单个元素的 DOM + 输入本身，与元素个数无关。
 *
 * ## 为什么是「切分 + 复用 Parser」而不是写一个新解析器
 * 切分只需要在**字节层面**判断元素边界（括号 / 字符串 / 转义），判定逻辑
 * 与 JSON 语法无关；把切出来的切片交给现有 `Parser::Parse`，语法校验、
 * 数值退化、转义解码、深度限制、错误行列全部沿用同一套实现——流式路径
 * 与整份解析对同一个切片给出**逐字节一致**的结论，不存在两套语义。
 *
 * ## 为什么不复制输入
 * 切分产出的是 `input.substr(begin, end - begin)`，即一个指向原缓冲的
 * `string_view`；`Parser` 的零拷贝字符串也同样借用输入。整个流程**没有
 * 任何一次整份输入的拷贝**，额外内存只有每个元素的 DOM。
 *
 * ## 出错位置的口径
 * `Parser` 报的偏移是相对**当前元素切片**的；本文件会把它换算回**整份输入**
 * （`begin + 切片内偏移`）并重算行列，因此调用方拿到的 `JsonError` 永远
 * 指向原始缓冲里的绝对位置。行列口径与 `Scanner::ComputePosition` 完全一致
 * （`\n`、`\r` 都算换行，`\r\n` 只算一个，列按字节计）。
 *
 * ## 已知限制
 * - **只支持顶层是数组**：顶层对象 / 标量无法切分，直接报 `kUnexpectedChar`
 *   （若确实要读单个大对象，那本来就必须建完整 DOM，请用 `Parser::Parse`）。
 * - 输入必须**整体驻留在内存**（`string_view`）：本文件解决的是 DOM 膨胀，
 *   不是把文件流进来。逐行流（NDJSON）用 `Parser::ParseMany`。
 * - `ParserConfig` 里两个与规模相关的字段在流式路径上语义不同，别按直觉用：
 *   `timeout_ms` 是**整次调用**的总预算（不是每个元素各花一次）；
 *   `max_input_bytes` 只约束**单个元素**（否则 GB 级输入会被整体拒掉）。
 *   详见 `ParseArrayStream` 的 `@param config`。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string_view>

#include "go2cpp/tools/error.hpp"
#include "go2cpp/json/config.hpp"
#include "go2cpp/json/document.hpp"
#include "go2cpp/json/error.hpp"
#include "go2cpp/json/parser.hpp"

namespace go2cpp::json {

/// @brief 顶层数组元素回调：每个元素一个独立 `Document`，回调返回 false 提前停止。
/// @note 回调持有的 `Document` 只在本次回调期间有效：下一次元素解析会复用并
///       `Clear()` 同一个文档，因此需要长期保存的数据必须在回调内拷走。
using ElementCallback = std::function<bool(Document&)>;

namespace stream_detail {

/// @brief 是否为 JSON 空白（只认 4 个字节，与 `Parser` 的口径一致）。
/// @param value [in] 待判定字节。
/// @return true 表示 `' '`、`'\t'`、`'\n'`、`'\r'` 之一。
inline bool IsWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

/**
 * @brief 从 `pos` 起跳过 JSON 空白。
 * @param input [in] 整份输入；借用，函数内不保留。
 * @param pos [in] 起始位置；必须 `<= input.size()`。
 * @return 第一个非空白字节的位置；全是空白时返回 `input.size()`。
 */
inline std::size_t SkipWhitespace(std::string_view input, std::size_t pos) noexcept {
  const std::size_t size = input.size();
  while (pos < size && IsWhitespace(input[pos])) {
    ++pos;
  }
  return pos;
}

/**
 * @brief 把 0 起字节偏移换算成 1 起行列。
 *
 * @param input [in] 偏移所参照的整份输入；借用。
 * @param offset [in] 0 起字节偏移；大于输入长度时按输入长度处理。
 * @param line [out] 1 起行号；函数返回后必定被写入。
 * @param column [out] 1 起列号（按字节计）；函数返回后必定被写入。
 *
 * @details 口径必须与 `Scanner::ComputePosition`（parser.cpp）逐字节一致，
 *          否则同一个坏元素在两条路径上会报出不同的行号：`\n`、`\r` 都算
 *          换行，`\r\n` 只算一个，`\r` 自身不占列宽。
 */
inline void ComputePosition(std::string_view input, std::size_t offset, std::size_t& line,
                            std::size_t& column) noexcept {
  line = 1;
  column = 1;
  const std::size_t limit = offset < input.size() ? offset : input.size();
  bool last_cr = false;
  for (std::size_t index = 0; index < limit; ++index) {
    const char value = input[index];
    if (value == '\r') {
      ++line;
      column = 1;
      last_cr = true;
    } else if (value == '\n') {
      // `\r\n` 算一个换行：`\n` 只在上一字节不是 `\r` 时递增行号。
      if (!last_cr) {
        ++line;
      }
      column = 1;
      last_cr = false;
    } else {
      ++column;
      last_cr = false;
    }
  }
}

/**
 * @brief 在整份输入的指定偏移处写一个 `JsonError`。
 * @param input [in] 整份输入；借用，用于换算行列。
 * @param failure [in] 失败原因。
 * @param offset [in] 0 起字节偏移（相对整份输入）。
 * @param error [out] 写入新错误对象；覆盖原值。
 * @return 恒为 false（便于 `return FailAt(...)`）。
 */
inline bool FailAt(std::string_view input, JsonFailure failure, std::size_t offset,
                   ErrorPtr& error) {
  std::size_t line = 1;
  std::size_t column = 1;
  ComputePosition(input, offset, line, column);
  error = NewJsonError(failure, offset, line, column);
  return false;
}

/**
 * @brief 把「相对元素切片」的错误位置换算成「相对整份输入」的绝对位置。
 *
 * @param input [in] 整份输入（已去 BOM）；借用，用于重算行列。
 * @param begin [in] 该元素切片在整份输入中的起点。
 * @param error [in,out] `Parser::Parse` 写入的错误。是 `JsonError` 时被替换成
 *              偏移为 `begin + 切片内偏移`、行列按整份输入重放的新对象；
 *              为空或不是 `JsonError` 时**保持原值**（不做猜测）。
 *
 * @details 为什么必须重算：`Parser` 看到的是 `input.substr(begin, len)`，它报的
 *          `offset/line/column` 全部相对切片；直接透传会让调用方在原始缓冲里
 *          定位到错误的位置（元素越靠后偏得越远）。`line` 不能靠「加上前面
 *          的行数」来修正——那还得先统计切片前的换行，反正都要扫一遍，不如
 *          直接用整份输入重放，保证与 `Scanner::ComputePosition` 逐字节一致。
 */
inline void RemapError(std::string_view input, std::size_t begin, ErrorPtr& error) {
  const auto* json_error = dynamic_cast<const JsonError*>(error.get());
  if (json_error == nullptr) {
    return;
  }
  // 切片内偏移必然 <= 切片长度，因此换算后的绝对偏移不会越过输入末尾。
  const std::size_t offset = begin + json_error->Offset();
  std::size_t line = 1;
  std::size_t column = 1;
  ComputePosition(input, offset, line, column);
  // 先取出全部字段再重建：赋值会先释放旧对象，之后不能再碰 json_error。
  error = NewJsonError(json_error->Failure(), offset, line, column);
}

/**
 * @brief 扫描一个数组元素在**外层数组视角**下的结束位置。
 *
 * @param input [in] 整份输入；借用。
 * @param begin [in] 元素起点（调用方已跳过空白，且已排除 `,` / `]`）；
 *              必须 `< input.size()`。
 * @return 元素的结束位置（不含），即外层数组里第一个「深度 0 的 `,` 或 `]`」
 *         的位置；扫描到输入末尾仍未找到时返回 `input.size()`。调用方据此
 *         做 `input.substr(begin, end - begin)`，拿到的是**恰好一个**元素的切片。
 *
 * ## 定位
 * 这是流式解析唯一的「切分点」。切错会连带把相邻元素（或外层 `]`）算进
 * 切片，`Parser` 随后报 `kTrailingGarbage`，用户看到的位置就全错了；切多了
 * 则把元素截断，报 `kUnexpectedEnd`。因此这里只依赖「括号深度」与「字符串
 * 边界」两个事实，**不判断**语法是否合法——切片内的语法一律交给 `Parser`。
 *
 * ## 步骤
 * step1. 维护 `depth`：`[` / `{` 加一，`]` / `}` 减一。深度为 0 时遇到 `,`
 *        或 `]` 就是元素边界，立即返回。
 * step2. 遇到 `"` 进入字符串：逐字节前进，`\\` 连跳两字节（转义字符），
 *        下一个裸 `"` 结束。字符串里的 `,` `]` `{` 全部按普通字节跳过，
 *        这是「`["a,b]c"]` 只有一个元素」的关键。
 * step3. 到输入末尾：返回 `size`。此时切片是「截断的元素」或「合法元素但
 *        数组缺 `]`」，两种情况由调用方的后续动作区分（`Parser` 报错 / 外层
 *        检查分隔符时报 `kUnexpectedEnd`）。
 *
 * ## 并发/失败语义
 * - 纯函数、无状态、`noexcept`；只读输入，任意线程可并发调用；
 * - 不分配内存，不抛异常。
 *
 * @note 本函数**不校验**括号配对：`[{"a":1]` 里 `]` 会被当作把 `{` 闭合掉，
 *       切片止于外层；切片 `{"a":1]` 交给 `Parser` 后会精确地在 `]` 处报
 *       `kUnexpectedChar`——错误位置仍然是正确的，这正是「切分不管语法」的
 *       好处：只有一处（`Parser`）负责语法判定。
 */
inline std::size_t ScanElementEnd(std::string_view input, std::size_t begin) noexcept {
  const std::size_t size = input.size();
  std::size_t pos = begin;
  std::size_t depth = 0;
  while (pos < size) {
    const char byte = input[pos];
    switch (byte) {
      case '"': {
        // 字符串：转义连跳两字节，其余原样前进，直到配对的裸引号。
        ++pos;
        while (pos < size) {
          const char inner = input[pos];
          if (inner == '\\') {
            pos += 2;
            continue;
          }
          if (inner == '"') {
            ++pos;
            break;
          }
          ++pos;
        }
        break;
      }
      case '[':
      case '{':
        ++depth;
        ++pos;
        break;
      case ']':
      case '}':
        if (depth == 0) {
          return pos;  // 外层数组的 `]`（或非法的 `}`）：元素到此为止
        }
        --depth;
        ++pos;
        break;
      case ',':
        if (depth == 0) {
          return pos;  // 外层数组的元素分隔符
        }
        ++pos;
        break;
      default:
        ++pos;
        break;
    }
  }
  return size;
}

}  // namespace stream_detail

/**
 * @brief 只支持「顶层是数组」的输入：按元素切分，逐元素解析并回调。
 *
 * @param input [in] 整份 JSON 字节；**借用**——切片与结果里的 borrowed `Text`
 *              都指向它，因此 `input` 必须在整个调用期间有效且不被修改。
 * @param on_element [in] 每成功解析一个元素回调一次；**借用**，调用期间有效。
 *              返回 false 表示调用方已处理完毕、要求提前停止（当前元素**计入**
 *              返回值，`error` 保持为空——提前停止不是错误）。
 * @param error [out] 失败时写入带**整份输入**偏移/行列的 `JsonError`；成功或
 *              提前停止时为空。入口先清空。
 * @param config [in] 运行期配置；透传给每个元素的 `Parser::Parse`。
 *              其中两个字段的语义与整份解析不同，需要单独说明：
 *              - `timeout_ms`：整次流式调用的总预算（每个元素只分到剩余毫秒），
 *                而不是「每个元素各给一次」；
 *              - `max_input_bytes`：约束**单个元素切片**（整体输入本来就该超过它，
 *                否则流式就没有意义）。
 *              其余字段（`allow_trailing_comma` / `allow_bom` / `max_depth` /
 *              `duplicate_key`）沿用 `Parser::Parse` 的含义。
 *
 * @return 已处理（成功解析并回调）的元素数。出错时 = 出错前的成功数；
 *         提前停止时 = 含触发停止的那个元素。
 *
 * ## 定位
 * 这是「大数组 + 小元素」场景的唯一入口：GB 级输入如果走 `Parser::Parse`，
 * 节点池 / 成员池 / 元素索引池会按整份输入预分配，RSS 与输入同量级；本函数
 * 把 DOM 限制在**一个元素**的规模（`Document` 复用同一块池，只在遇到更大
 * 元素时扩容），因此内存上界与元素个数无关。
 *
 * 代价是**无法回头**：回调结束后文档即被下一次解析清空，也不能对整棵树做
 * 跨元素查询（求和 / 排序 / 二次遍历都要调用方自己累积）。
 *
 * ## 步骤
 * step1. 清空 `error`；按 `config.allow_bom` 去掉 UTF-8 BOM（之后所有偏移
 *        都相对去 BOM 后的视图，与 `Parser::Parse` 口径一致）。
 * step2. 跳过前导空白，要求第一个非空白字节是 `[`；否则报 `kUnexpectedChar`
 *        （空输入报 `kUnexpectedEnd`）——顶层对象/标量无法切分。
 * step3. 建立整体时间预算（`timeout_ms != 0` 时）。
 * step4. 循环：跳过空白 → 判定 `]`（含尾逗号检查）/ `,`（元素缺失）→
 *        用 `ScanElementEnd` 切出恰好一个元素 → 调 `Parser::Parse`
 *        （失败则把偏移换算回整份输入后返回）→ 回调。
 * step5. 元素之后只接受 `,`（继续）或 `]`（结束），其它报 `kUnexpectedChar`；
 *        数组闭合后只允许尾随空白，否则报 `kTrailingGarbage`。
 *
 * ## 并发/失败语义
 * - 无全局状态；不同线程可以用各自的 `Document` 并发调用（`input` 只读）；
 * - 失败时 `error` 是带绝对位置的 `JsonError`，返回值是**已成功**的元素数，
 *   调用方可以据此定位「第几个元素、哪个字节」出了问题；
 * - 不吞异常：`on_element` 抛出时按普通 C++ 语义向上传播，`error` 保持调用前的状态；
 * - 不变量：返回 `n` 且 `error` 为空时，恰好 `n` 个元素被完整解析并回调过。
 */
inline std::size_t ParseArrayStream(std::string_view input, const ElementCallback& on_element,
                                    ErrorPtr& error, const ParserConfig& config = ParserConfig{}) {
  // step1: 与 Parser::Parse 一致的入口语义：先清空错误；可选去 BOM。
  //        BOM 之后所有偏移都相对去 BOM 的视图，调用方若需映射回原缓冲需 +3。
  error.reset();
  if (config.allow_bom && input.size() >= 3 &&
      static_cast<unsigned char>(input[0]) == 0xEF &&
      static_cast<unsigned char>(input[1]) == 0xBB &&
      static_cast<unsigned char>(input[2]) == 0xBF) {
    input.remove_prefix(3);
  }

  // step2: 定位顶层 `[`。顶层不是数组就没有「元素」可言，直接拒绝。
  std::size_t pos = stream_detail::SkipWhitespace(input, 0);
  if (pos == input.size()) {
    stream_detail::FailAt(input, JsonFailure::kUnexpectedEnd, pos, error);
    return 0;
  }
  if (input[pos] != '[') {
    stream_detail::FailAt(input, JsonFailure::kUnexpectedChar, pos, error);
    return 0;
  }
  ++pos;

  // step3: 整体时间预算：每元素分到的是「剩余毫秒」，因此 timeout_ms 的含义
  //        与 Parser::Parse 相同——整次调用最多花这么久，而不是每元素各花一次。
  using Clock = std::chrono::steady_clock;
  const bool has_deadline = config.timeout_ms != 0;
  const Clock::time_point deadline =
      has_deadline ? Clock::now() + std::chrono::milliseconds(config.timeout_ms)
                   : Clock::time_point{};
  constexpr std::uint32_t kMaxTimeoutMs = std::numeric_limits<std::uint32_t>::max();

  // step4: 逐元素推进。文档与解析器实例全程复用：峰值内存 = 最大的那个元素。
  Document document;
  Parser parser;
  std::size_t count = 0;     // 已成功解析并回调的元素数
  bool after_comma = false;  // 上一个非空白字节是 `,`（尾逗号判定用）
  for (;;) {
    pos = stream_detail::SkipWhitespace(input, pos);
    if (pos == input.size()) {
      // 数组没有闭合就结束（`[1,2` / `[`）。
      stream_detail::FailAt(input, JsonFailure::kUnexpectedEnd, pos, error);
      return count;
    }
    const char byte = input[pos];
    if (byte == ']') {
      // 尾逗号：判定口径与 Parser 完全一致（默认报 kTrailingComma）。
      if (after_comma && !config.allow_trailing_comma) {
        stream_detail::FailAt(input, JsonFailure::kTrailingComma, pos, error);
        return count;
      }
      ++pos;
      break;
    }
    if (byte == ',') {
      // 元素缺失（`[,1]`、`[1,,2]`）：逗号所在字节就是出错位置。
      stream_detail::FailAt(input, JsonFailure::kUnexpectedChar, pos, error);
      return count;
    }

    // step4a: 切出恰好一个元素（零拷贝：只是原缓冲上的一个区间）。
    const std::size_t begin = pos;
    const std::size_t end = stream_detail::ScanElementEnd(input, begin);
    if (end == begin) {
      // 空切片只可能来自「值位置出现 `}`」这类终止符：那是语法错误，
      // 不是「输入提前结束」，因此报 kUnexpectedChar 而不是 kUnexpectedEnd。
      stream_detail::FailAt(input, JsonFailure::kUnexpectedChar, begin, error);
      return count;
    }

    // step4b: 把剩余预算分给这个元素；至少 1 ms，避免「舍入成 0」被误判成超时。
    ParserConfig element_config = config;
    if (has_deadline) {
      const Clock::time_point now = Clock::now();
      if (now >= deadline) {
        stream_detail::FailAt(input, JsonFailure::kTimeout, begin, error);
        return count;
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      element_config.timeout_ms =
          remaining <= 0 ? 1u
                         : (static_cast<std::uint64_t>(remaining) >
                                    static_cast<std::uint64_t>(kMaxTimeoutMs)
                                ? kMaxTimeoutMs
                                : static_cast<std::uint32_t>(remaining));
    }

    // step4c: 复用现有解析器：语法、转义、数值退化、深度限制全部同一套实现。
    if (!parser.Parse(std::string_view(input.data() + begin, end - begin), document, error,
                      element_config)) {
      // 失败偏移是相对切片的，这里换算成整份输入的绝对位置。
      stream_detail::RemapError(input, begin, error);
      return count;
    }

    // step4d: 回调；返回 false 表示调用方自行停止，当前元素算「已处理」。
    ++count;
    if (!on_element(document)) {
      return count;
    }

    // step4e: 元素之后的语法：`,` 继续、`]` 结束，其它都是错误。
    pos = stream_detail::SkipWhitespace(input, end);
    if (pos == input.size()) {
      stream_detail::FailAt(input, JsonFailure::kUnexpectedEnd, pos, error);
      return count;
    }
    if (input[pos] == ',') {
      ++pos;
      after_comma = true;
      continue;
    }
    if (input[pos] == ']') {
      ++pos;
      break;
    }
    stream_detail::FailAt(input, JsonFailure::kUnexpectedChar, pos, error);
    return count;
  }

  // step5: 数组闭合后只允许尾随空白（与 Parser 的 kTrailingGarbage 一致）。
  pos = stream_detail::SkipWhitespace(input, pos);
  if (pos != input.size()) {
    stream_detail::FailAt(input, JsonFailure::kTrailingGarbage, pos, error);
    return count;
  }
  return count;
}

}  // namespace go2cpp::json
