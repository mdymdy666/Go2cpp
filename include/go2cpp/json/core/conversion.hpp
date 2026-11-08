#pragma once

/**
 * @file conversion.hpp
 * @brief 字符串 → 强类型值：按 `TypeId` 注册的转换表（迁移自 week1 `csvp/conversion.hpp`）。
 *
 * 与反射解耦：反射层只描述「这是什么类型」（`reflect::TypeId`），转换层负责
 * 「怎么从字节解析成这个类型」。转换表是 `bgo2cpp::json::core::conversion` 的内部状态，
 * 调用方只通过 `RegisterConverter` / `FindConverter` / `Convert` 三个入口访问。
 *
 * ## 内置 5 种
 * 首次使用转换表时自动注册（线程安全的一次性初始化），对应 week1 的 5 个内置：
 *
 * | TypeId | 转换器 | `ItemValue` 分支 |
 * |---|---|---|
 * | `reflect::kBoolType` | `StringToBool` | `bool` |
 * | `reflect::kInt32Type` | `StringToInt` | `std::int64_t`（统一提升） |
 * | `reflect::kDoubleType` | `StringToDouble` | `double` |
 * | `reflect::kStringType` | `StringToString` | `Text`（借用，零拷贝） |
 * | `reflect::kDecimalType` | `StringToDecimal` | `Decimal` |
 *
 * week1 的 `kIntType` 在 week2 的反射体系里对应 `kInt32Type`（两者 id 都是 2），
 * `ItemValue` 只有 `std::int64_t` 一个整型分支，所以 int 与 int64 共用存储。
 * 注意：`kInt64Type` / `kUint32Type` / `kUint64Type` / `kBytesType` **没有**内置
 * 转换器（week1 同样只有 5 个），业务需要时走动态 id 注册。
 *
 * ## 自定义类型
 * 从 `kFirstDynamicType`（16）起，业务用 `RegisterConverter` 注册，转换结果放进
 * `ItemValue` 的最后一个分支（类型擦除的 `shared_ptr<const void>`）。
 * 内置 id（< 16）受保护、不可覆盖，防止同一 `TypeId` 的行为被静默改变。
 *
 * ## 失败语义
 * 转换失败返回 `false`，**不修改**输出参数、不抛异常（分配失败除外）；
 * 需要定位原因的调用方用带 `ErrorPtr&` 的注册重载或 `DecimalError`。
 */

#include "go2cpp/json/core/decimal.hpp"
#include "go2cpp/json/core/text.hpp"
#include "go2cpp/tools/error.hpp"
#include "go2cpp/reflect/type.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

namespace go2cpp::json::core {

// Go2Cpp 的 `go2cpp/reflect/type.hpp` 把 1..15 预留给未来内置类型，
// 但没有 `kDecimalType`（JSON 库的 Decimal 是库内类型）。这里定义 9，
// 与 JSON 库原实现保持一致；若上游将来占用 9，需要改这里并做迁移。
inline constexpr go2cpp::reflect::TypeId kDecimalType = 9;

/**
 * @brief 一个字段 / 一个 JSON 值的解析结果。
 *
 * @details 前 6 个分支与内置类型一一对应；自定义类型统一走最后一个分支，
 *          由转换器决定 `shared_ptr` 指向的具体对象（转换器负责 cast）。
 *          默认构造是 `std::monostate`，表示「尚未解析」。
 */
using ItemValue =
    std::variant<std::monostate, Decimal, std::int64_t, double, bool, Text,
                 std::shared_ptr<const void>>;

/**
 * @brief 字符串 → `ItemValue` 的转换函数。
 * @note 必须是普通函数指针（不能带捕获），保证转换表是纯数据、可跨线程只读；
 *       转换器失败时必须保持 `to` 不变（`Convert` 会先写临时对象，但自定义
 *       转换器也可能被业务直接调用）。
 */
using Converter = bool (*)(std::string_view, ItemValue&);

/**
 * @brief 注册某个动态类型 id 的转换器。
 * @param id [in] 类型 id；必须是 `kFirstDynamicType` 及以上的已分配 id。
 * @param converter [in] 转换函数；不可为 nullptr。
 * @return 成功 true；id 非法、converter 为空、或 id 是内置类型返回 false。
 * @note 同名幂等：同一 id 重复注册保留首次注册的函数并返回 true（不覆盖）。
 */
bool RegisterConverter(reflect::TypeId id, Converter converter);

/**
 * @brief 带结构化错误的注册。
 * @param id [in] 类型 id。
 * @param converter [in] 转换函数。
 * @param error [out] 成功时清空，失败时写入 `extened::StringError`；必填引用。
 * @return 成功 true。
 * @note 失败分两类：id 非法 / converter 为空（invalid argument）；
 *       id 是内置类型（不允许覆盖）。`StringError` 只带文本，不区分错误码。
 */
bool RegisterConverter(reflect::TypeId id, Converter converter, ErrorPtr& error);

/**
 * @brief 查转换器。
 * @param id [in] 类型 id。
 * @return 已注册的转换函数；未注册（含内置 id 未覆盖到的类型）返回 nullptr。
 * @note 返回的函数指针在进程生命周期内有效；转换表只增不改，无失效问题。
 */
Converter FindConverter(reflect::TypeId id);

/**
 * @brief 按类型 id 解析字符串。
 * @param id [in] 目标类型 id。
 * @param from [in] 待解析字节；生命周期只需覆盖本次调用（`kStringType` 例外，
 *             见下）。
 * @param to [out] 成功时写入解析结果；失败时**保持原值**（调用方可以安全地
 *             拿旧值兜底）。
 * @return 成功 true；转换器缺失或解析失败 false。
 * @warning `kStringType` 走零拷贝：结果里存的是指向 `from` 的 `Text` 借用视图，
 *          因此 `from` 指向的缓冲必须比 `to` 活得久。需要结果独立存活时，
 *          自己 `Text::Own(std::string(view))`。
 */
bool Convert(reflect::TypeId id, std::string_view from, ItemValue& to);

/**
 * @brief 解析 32 位有符号整数。
 * @param from [in] 允许 `[+-]digits`；不接受空格、小数点、指数、分组符。
 * @param to [out] 成功时写入；失败时保持原值。
 * @return 成功 true；空串、非法字符、超出 `int` 范围返回 false。
 */
bool StringToInt(std::string_view from, int& to);

/**
 * @brief 解析双精度浮点。
 * @param from [in] 允许 `[+-]digits[.digits][eE[+-]digits]`（与 `strtod` 的
 *              常见子集一致，不接受 `nan` / `inf` / 十六进制）。
 * @param to [out] 成功时写入；失败时保持原值。
 * @return 成功 true；格式非法、溢出、以及「非零输入下溢成 0」都返回 false。
 */
bool StringToDouble(std::string_view from, double& to);

/**
 * @brief 解析布尔。
 * @param from [in] 只接受 `true/TRUE/1/yes` 与 `false/FALSE/0/no`（区分大小写，
 *              与 week1 一致；`True`、`on` 之类不接受）。
 * @param to [out] 成功时写入；失败时保持原值。
 * @return 成功 true。
 */
bool StringToBool(std::string_view from, bool& to);

/**
 * @brief 解析定点十进制。
 * @param from [in] 语法与上限见 `Decimal::Parse`。
 * @param to [out] 成功时写入；失败时保持原值。
 * @return 成功 true。
 */
bool StringToDecimal(std::string_view from, Decimal& to);

/**
 * @brief 字符串 → `Text`：**借用**，不分配、不拷贝。
 * @param from [in] 原始字节；缓冲必须比 `to` 活得久。
 * @param to [out] 借用态 `Text`，指向 `from`。
 * @return 恒为 true（任何字节序列都是合法的「字符串」）。
 * @note 名字沿用 week1 的 `StringToString`；week1 的目标类型是 `std::wstring`
 *       且做拷贝，这里改为零拷贝的 `Text`。
 */
bool StringToString(std::string_view from, Text& to);

}  // namespace go2cpp::json::core
