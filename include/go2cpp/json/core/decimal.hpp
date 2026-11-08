#pragma once

/**
 * @file decimal.hpp
 * @brief 任意精度定点十进制数：`bgo2cpp::json::core::Decimal`。
 *
 * 迁移自 week1 `csvp/decimal.h` + `src/decimal.cpp`（530 行）。定点数用
 * 「符号 + 十进制数字串 + 小数位数」表示，不依赖二进制浮点，因此
 * `0.1 + 0.2 == 0.3` 这类十进制语义是精确的——JSON 数值、金额、计量单位
 * 都要求这一点，`double` 做不到。
 *
 * ## 与 week1 的差异
 * - 命名空间 `csvp` → `extened::core`；
 * - **去掉 `wchar_t` 重载**：JSON 输入是 UTF-8 字节流，只保留 `std::string_view`；
 * - `DecimalError` 不再依赖 `CSVP_ERROR_CLASS` 宏，改为继承 `extened::Error`，
 *   `Message()` 返回带失败原因与原文（截断）的文本，`Failure()` 保留结构化枚举；
 * - `Parse` 现在能区分「格式非法（kInvalidChar）」与「数量级超限（kOutOfRange）」，
 *   week1 的 `kOutOfRange` 虽然定义过，实际从未被写入。
 *
 * ## 内部上限（沿用 week1，未放宽）
 * - 输入长度 ≤ 1 MiB（`kMaxInputBytes`，定义在 `.cpp`）；
 * - 小数位数 / 数量级 ≤ 2^20（`kMaxScale`，定义在 `.cpp`）；
 * - 指数绝对值 ≤ 10^6（`kMaxExponent`，定义在 `.cpp`）。
 *
 * ## 表示不变量（所有成员函数都依赖）
 * - `m_digits` 非空时：无前导零、无尾随零（规范化）；`m_digits` 为空即零值，
 *   且零值恒为 `m_scale == 0`、`m_negative == false`；
 * - `m_scale >= 0`，`m_digits.size()` 可以小于 `m_scale`（如 0.0001 存 `"1"` + 4）；
 * - 数值 = `(negative ? -1 : 1) * int(m_digits) * 10^-m_scale`。
 *
 * 规范化保证同一个数只有一种内部表示，`Compare` 因此可以先比「对齐后的位数」
 * 再逐位比较，零值也天然只有一种形态（`-0` 解析后就是正零）。
 */

#include "go2cpp/tools/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::json::core {

/// @brief `Decimal::Parse` 失败的具体原因；供上层区分「数据脏」与「数据太大」。
enum class DecimalFailure : std::uint8_t {
  kNone = 0,     ///< 成功。
  kEmpty,        ///< 输入为空串。
  kTooLong,      ///< 输入超过 1 MiB 上限。
  kInvalidChar,  ///< 格式非法：非法字符、多个小数点、指数残缺、尾随垃圾。
  kOutOfRange,   ///< 数量级超限：指数超过 10^6，或需要补的零超过 2^20。
};

/**
 * @brief `Decimal::Parse` 失败的结构化错误。
 *
 * @details 继承 `extened::Error`，可以直接放进 `ErrorPtr` 错误链；`Message()`
 *          给出面向用户的文本（含原因与原文片段），`Failure()` 供程序分支。
 *          对象构造后不可变，可跨线程共享。
 *
 * ## 定位
 * 转换层（`StringToDecimal`）不返回错误对象，只返回 `false`；需要定位「哪个字段
 * 为什么解析失败」的上层（JSON 读取层 / CSV 导入）用本类型构造 `ErrorPtr` 再
 * `Wrap` 上字段路径。文本被截断到 32 字节，避免 1 MiB 的坏字段把错误日志撑爆。
 */
class DecimalError final : public Error {
 public:
  /**
   * @brief 构造失败错误。
   * @param failure [in] 失败原因；`kNone` 表示调用方用错了（仍可构造，仅用于
   *                诊断，`Message()` 会退化成「invalid input」）。
   * @param text [in] 原始输入的所有权移交给本函数（用于生成错误文本）。
   */
  DecimalError(DecimalFailure failure, std::string text);

  /// @brief 返回带原因与原文（截断）的错误文本。
  std::string Message() const override;

  /// @brief 返回结构化失败原因；正常构造时不会是 `kNone`（传 `kNone` 属于调用方误用）。
  DecimalFailure Failure() const noexcept { return m_failure; }

  /// @brief 返回完整原文（可能很长，调用方自行决定是否输出）。
  const std::string& Text() const noexcept { return m_text; }

 private:
  DecimalFailure m_failure;  ///< 解析失败原因。
  std::string m_text;        ///< 原始输入；构造后不再修改。
};

/**
 * @brief 任意精度定点十进制数。
 *
 * @details 值语义：可拷贝、可赋值；所有算术返回新对象，`operator+=` 等就地修改。
 *          默认构造为零（`"0"`）。没有任何操作会抛异常以外的方式失败：
 *          `Parse` / `Divide` 返回 `bool`，其余算术假定输入合法（内存不足时
 *          由 `std::string` / `std::vector` 抛 `std::bad_alloc`）。
 *
 * ## 精度与上限
 * - 加减乘：位数随操作数位数增长，没有额外的位数上限（受内存约束）；
 * - `Divide`：必须指定结果小数位 `scale`，超出部分「远离零」四舍五入；
 * - `Parse`：见上方内部上限，超出即失败，绝不静默截断。
 */
class Decimal {
 public:
  Decimal() = default;

  /**
   * @brief 解析十进制文本；失败返回 false 且**不修改** `out`。
   * @param text [in] 待解析字节；允许 `[+-]digits[.digits][eE[+-]digits]`。
   * @param out [out] 成功时写入解析结果；失败时保持原值。
   * @return 成功 true；格式非法或超出内部上限 false。
   * @note 不跳过首尾空白（调用方自己 trim），不接受 `nan` / `inf` / 十六进制。
   */
  static bool Parse(std::string_view text, Decimal& out);

  /**
   * @brief 带结构化失败原因的解析；语义同上。
   * @param text [in] 待解析字节。
   * @param out [out] 成功时写入；失败时保持原值。
   * @param failure [out] 成功时写 `kNone`，失败时写具体原因；必填。
   * @return 成功 true。
   */
  static bool Parse(std::string_view text, Decimal& out, DecimalFailure& failure);

  /// @brief 是否为零；零值只有一种表示（`m_digits` 为空）。
  bool IsZero() const noexcept { return m_digits.empty(); }

  /// @brief 是否为负；零值恒为 false（`-0` 解析后是正零）。
  bool IsNegative() const noexcept { return m_negative; }

  /// @brief 是否为整数（小数位为零；零值也算整数）。
  bool IsInteger() const noexcept { return m_scale == 0; }

  /// @brief 三路比较：小于返回 -1，相等 0，大于 1。零与负零相等。
  int Compare(const Decimal& other) const noexcept;

  bool operator==(const Decimal& other) const noexcept { return Compare(other) == 0; }
  bool operator!=(const Decimal& other) const noexcept { return Compare(other) != 0; }
  bool operator<(const Decimal& other) const noexcept { return Compare(other) < 0; }
  bool operator<=(const Decimal& other) const noexcept { return Compare(other) <= 0; }
  bool operator>(const Decimal& other) const noexcept { return Compare(other) > 0; }
  bool operator>=(const Decimal& other) const noexcept { return Compare(other) >= 0; }

  /// @brief 规范化十进制文本；零值返回 `"0"`（不含负号、不含小数点）。
  std::string ToString() const;

  /// @brief 向下取整（向负无穷）：-1.5 → -2，1.5 → 1。
  Decimal Floor() const;

  /// @brief 向上取整（向正无穷）：-1.5 → -1，1.5 → 2。
  Decimal Ceil() const;

  /// @brief 向零取整（丢弃小数部分）：-1.5 → -1，1.5 → 1。
  Decimal Truncate() const;

  /**
   * @brief 四舍五入到 `scale` 位小数。
   * @param scale [in] 保留的小数位数；负数按 0 处理（Decimal 不支持负 scale）；
   *              大于等于当前小数位时原样返回（不做补零）。
   * @return 舍入后的新值。
   * @note 舍入规则是「远离零」：0.5 → 1，-0.5 → -1，与 `Divide` 一致。
   */
  Decimal Round(std::int32_t scale) const;

  /// @brief 绝对值；零值保持正零。
  Decimal Abs() const;

  /// @brief 取负；零值仍为零（不产生 `-0`）。
  Decimal operator-() const;

  Decimal operator+(const Decimal& other) const;
  Decimal operator-(const Decimal& other) const;
  Decimal operator*(const Decimal& other) const;

  Decimal& operator+=(const Decimal& other);
  Decimal& operator-=(const Decimal& other);
  Decimal& operator*=(const Decimal& other);

  /**
   * @brief 除法：保留 `scale` 位小数，末位「远离零」四舍五入。
   * @param lhs [in] 被除数。
   * @param rhs [in] 除数；为零时返回 false。
   * @param scale [in] 结果小数位；负数按 0 处理（兼容旧语义），需要报错请用
   *              下面带 `error` 的重载。
   * @param out [out] 成功时写入商；失败时保持原值。
   * @return 成功 true；除零或结果超出内部上限 false。
   */
  static bool Divide(const Decimal& lhs, const Decimal& rhs, std::int32_t scale, Decimal& out);

  /**
   * @brief 带错误文本的除法。
   * @param lhs [in] 被除数。
   * @param rhs [in] 除数。
   * @param scale [in] 结果小数位；为负、超过 2^20 都算失败。
   * @param out [out] 成功时写入商；失败时保持原值。
   * @param error [out] 成功时清空，失败时写入原因文本。
   * @return 成功 true。
   * @note `scale` 非负时与上面的重载结果完全一致；上面那个重载会把负 scale
   *       归零，因此不能靠它报告「scale 非法」。
   */
  static bool Divide(const Decimal& lhs, const Decimal& rhs, std::int32_t scale, Decimal& out,
                     std::string& error);

 private:
  /// @brief 用「符号 + 数字串 + scale」构造并规范化；全零输入折叠成零值。
  static Decimal Make(bool negative, std::string digits, std::int32_t scale);

  /// @brief 解析状态机；返回 `kNone` 表示成功。实现只在 `.cpp` 内（非模板）。
  static DecimalFailure ParseImpl(std::string_view text, Decimal& out);

  /// @brief 比较绝对值（忽略符号）；零值视为 `"0"`。
  static int CompareAbs(const Decimal& lhs, const Decimal& rhs) noexcept;

  bool m_negative = false;   ///< 符号；零值恒为 false。
  std::string m_digits;      ///< 规范化的十进制数字串；空串表示零。
  std::int32_t m_scale = 0;  ///< 小数位数，非负。
};

}  // namespace go2cpp::json::core
