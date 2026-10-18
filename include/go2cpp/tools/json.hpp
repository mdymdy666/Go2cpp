/**
 * @file json.hpp
 * @brief nlohmann/json 的 Go2Cpp 薄封装：入口处做深度栅栏，超限即拒。
 *
 * @details 为什么需要这一层，而不是直接用 nlohmann/json：
 *
 *   1. **上游的"深度风险"不在解析，而在解析之后的递归操作。**
 *      实测（nlohmann/json v3.11.3，128 KiB Fiber 栈，本仓库实测数据）：
 *
 *        - **解析是迭代的**：`parse()` 走 `sax_parse_internal`，用堆上的
 *          `std::vector<bool> states` 作显式栈，而不是递归下降。实测 20 万层
 *          嵌套可正常解析，50 万层也安全。
 *        - **`dump()`（序列化）仍然是递归的**：128 KiB 栈上约 **1162 层**
 *          （Release/-O2）或 **609 层**（无优化）就触碰 guard page。
 *        - **拷贝一份 `json` 值同样递归**，且临界更低：约 **816 层**
 *          （Release）或 **185 层**（无优化）。
 *
 *      | 操作（128 KiB Fiber 栈） | Release/-O2 | 无优化 |
 *      |---|---|---|
 *      | 解析（`ParseJson` 内部） | ≥ 524288 层 | ≥ 524288 层 |
 *      | `dump()` | 1162 层 | 609 层 |
 *      | 拷贝 | 816 层 | 185 层 |
 *
 *   2. **爆栈不可恢复**。Fiber 栈固定 128 KiB 且不增长，栈底只有 1 页
 *      `PROT_NONE` guard page；全项目没有 SIGSEGV handler、也没有
 *      sigaltstack，越界即内核默认动作 —— **进程被立即终止（退出码 139）**。
 *      所以"解析成功"**不等于**"安全"：深文档一旦进入程序，之后任意一次
 *      `dump()` 或拷贝都可能让进程消失，而且崩点离入口很远、极难定位。
 *      把校验放在入口，是把"不可捕获的崩溃"换成"可处理的错误"的唯一位置。
 *
 *   3. **深嵌套还是资源放大器**。解析所需的堆内存与时间是 O(嵌套深度)，
 *      不设上限就等于把这个成本交给输入方（典型的 DoS 面）。
 *
 *   `ParseJson` 正是针对这三点：在**进入上游之前**先用一个**迭代**扫描器
 *   （不递归、不分配）量出嵌套深度，超限立即返回可处理的 `ErrorPtr`。
 *   默认上限 64 相对最坏实测（无优化下拷贝临界 185 层）仍留有约 3 倍余量。
 *
 * 上游来源（第三方，未做任何修改）：
 *   - 项目：https://github.com/nlohmann/json
 *   - 版本：v3.11.3（单头文件 `single_include/nlohmann/json.hpp`）
 *   - 许可证：MIT（见文件头 SPDX-License-Identifier: MIT）
 *   - 本地位置：`third_party/nlohmann/json.hpp`，由 CMake 的 third_party
 *     包含路径暴露为 `#include "nlohmann/json.hpp"`。
 *
 * @note 本头文件**不进入** `go2cpp/runtime.hpp` 聚合头：引入 900+ KB 的单头
 *       文件会显著增加所有使用者的编译时间，而 JSON 并非运行时语义的一部分。
 *       需要 JSON 的翻译单元显式包含 `go2cpp/json.hpp` 即可。
 * @note 本文件是 header-only 的薄封装：没有对应 .cpp，也不需要新增 CMake target。
 */

#pragma once

#include "nlohmann/json.hpp"

#include "go2cpp/error.hpp"

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

namespace go2cpp {

/**
 * @brief JSON 值类型。
 * @details 直接沿用上游类型，不做二次包装 —— 这样调用方可以完整使用
 *          nlohmann 的 API，本模块只负责「进得来、不炸栈」这一步。
 */
using Json = ::nlohmann::json;

/**
 * @brief `ParseJson` 默认接受的嵌套深度上限。
 * @details 依据是本文件顶部表格里的实测数据：无优化构建下拷贝临界约 185 层、
 *          dump 临界约 609 层，取 64 留约 3 倍余量。
 *          需要更深的文档时显式传入更大的 `max_depth`，并自行确认该执行流的
 *          栈容量能承受后续的 dump/拷贝。
 */
inline constexpr std::size_t kDefaultJsonMaxDepth = 64U;

/**
 * @brief `JsonScanDepth` 的「不早退」哨兵：完整扫描整段文本。
 * @details 取 `std::size_t` 最大值，任何真实深度都不会超过它。
 */
inline constexpr std::size_t kJsonDepthUnlimited = static_cast<std::size_t>(-1);

/**
 * @brief 返回「嵌套深度超限」错误的进程级身份锚点。
 *
 * @details `ParseJson` 在超限时返回的是
 *          `Wrap(JsonDepthExceededError(), 具体说明)`，因此判断错误种类必须用
 *          **身份比较**（`Is(error, JsonDepthExceededError())`），
 *          不能用文本比较 —— 文本里带着具体的上限与扫描层数，每次都不同。
 *
 * @return 共享的、不可变的错误对象；同一进程内地址恒定。
 * @note 故意泄漏（`new` 后不析构）：与 `go.hpp` 的默认调度器同理，若使用函数
 *       局部 static 对象，程序退出阶段的静态析构会让这里的身份比较退化为
 *       「与空指针比较」，从而把「深度超限」静默误判成「没有错误」。
 */
inline ErrorPtr JsonDepthExceededError() {
    static const ErrorPtr* const kError = new ErrorPtr(
        NewError("json: nested depth exceeds the configured limit"));
    return *kError;
}

/**
 * @brief 迭代扫描 JSON 文本，返回观测到的最大嵌套深度。
 *
 * @details 实现是**单层循环 + 显式计数**，不递归、不分配，
 *          因此扫描本身与输入深度无关地安全（100 万层嵌套也没有额外栈开销）。
 *          扫描器跳过字符串字面量内部（包括 `\"` 转义），所以
 *          `"[[[["` 这样的字符串内容不会被计入深度。
 *
 *          扫描器**不做语法校验**：括号不匹配、非法字符都不会报错，只按字面
 *          计数（`}`/`]` 在深度为 0 时被忽略）。合法性交给 `ParseJson` 里的
 *          上游解析器判断 —— 这里只回答「嵌套有多深」。
 *
 * @param text 待扫描文本（可以是任意字节串，不要求是合法 JSON）。
 * @param stop_at 早退阈值：一旦观测深度**超过**该值就立即返回。
 *        此时返回值只是「已观测到的层数」，是真实深度的**下界**，
 *        **不是**全局最大值；默认 `kJsonDepthUnlimited`（完整扫描）。
 * @return 观测到的最大嵌套深度；空输入或纯标量返回 0。
 * @note 该函数是 `noexcept` 且无分配：可以安全地用在「必须先判断再决定是否
 *       解析」的栅栏位置。
 */
inline std::size_t JsonScanDepth(std::string_view text,
                                 std::size_t stop_at = kJsonDepthUnlimited) noexcept {
    std::size_t depth = 0;    // 当前尚未闭合的容器层数
    std::size_t deepest = 0;  // 历史最大层数
    bool in_string = false;
    bool escaped = false;
    for (const char ch : text) {
        if (in_string) {
            // 字符串内部只关心转义与结束引号：内容里的括号不计入深度。
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{' || ch == '[') {
            ++depth;
            if (depth > deepest) {
                deepest = depth;
                if (deepest > stop_at) {
                    return deepest;  // 已足以判定超限，无需扫完
                }
            }
            continue;
        }
        if ((ch == '}' || ch == ']') && depth > 0) {
            --depth;
        }
    }
    return deepest;
}

/**
 * @brief `ParseJson` 的结果：值 + 可检查的错误。
 * @details 与 `ChannelRecvResult<T>`、`FutureResult<T>` 同一种「结构化结果」风格：
 *          不使用异常表达解析失败。
 * @note 判定成功请用 `ok()`，**不要**用 `value.is_null()` —— 合法输入 `"null"`
 *       解析成功后 `value` 同样是 null。
 */
struct JsonParseResult {
    /** @brief 成功时的解析结果；失败时为 null。 */
    Json value{};
    /** @brief 失败原因：深度超限为 `Wrap(JsonDepthExceededError(), ...)`，
     *         语法/编码错误为 `NewError("JSON 解析失败: ...")`；成功时为空。 */
    ErrorPtr error;

    /** @brief 返回本次解析是否成功。 */
    bool ok() const noexcept { return !error; }
};

/**
 * @brief 带深度栅栏的 JSON 解析入口。
 *
 * @details 执行两步，顺序不可交换：
 *          1. `JsonScanDepth(text, max_depth)` 迭代求深度，超限即早退；
 *          2. 仅当未超限时才调用 `nlohmann::json::parse`。
 *
 * @note 上游的解析器本身是迭代的（见文件头实测表），所以这道栅栏**不是**
 *       为了让解析不爆栈 —— 它拦的是「深文档进入程序之后」的递归操作
 *       （`dump()`、拷贝等）与 O(深度) 的资源放大。放大 `max_depth` 就等于
 *       放弃这层保护，请自行确认目标执行流的栈容量。
 *
 * @param text 待解析文本；允许为空（空输入会走上游报错路径，不抛异常）。
 * @param max_depth 允许的最大嵌套深度，默认 `kDefaultJsonMaxDepth`（64）。
 * @return 结构化结果；失败信息在 `result.error`，不抛异常。
 * @note 上游的所有异常（`json::parse_error` 等）都在这里被转换成 `ErrorPtr`，
 *       与项目「不用异常做控制流」的约定一致。
 */
inline JsonParseResult ParseJson(std::string_view text,
                                 std::size_t max_depth = kDefaultJsonMaxDepth) {
    JsonParseResult result;
    // step1: 先用迭代扫描器量深度；超限直接返回可处理的错误。
    const std::size_t observed = JsonScanDepth(text, max_depth);
    if (observed > max_depth) {
        result.error =
            Wrap(JsonDepthExceededError(),
                 "JSON 嵌套深度超过上限 " + std::to_string(max_depth) +
                     "（扫描到第 " + std::to_string(observed) +
                     " 层即早退，完整深度未统计）");
        return result;
    }
    // step2: 确认不超限后才交给上游；异常一律转成 ErrorPtr。
    try {
        result.value = Json::parse(text.begin(), text.end());
    } catch (const std::exception& failure) {
        result.error = NewError(std::string("JSON 解析失败: ") + failure.what());
    } catch (...) {
        result.error = NewError("JSON 解析失败: 未知异常");
    }
    return result;
}

}  // namespace go2cpp
