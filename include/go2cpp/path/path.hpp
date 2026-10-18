/**
 * @file path.hpp
 * @brief Go `path` 包（POSIX 斜杠路径，纯字符串操作）的 C++ 对应实现。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（移植自 Go 标准库，见下方来源说明）
 *
 * @details 复用优先的结论：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | Go 标准库 `path`（本项目参考本机 Go 1.27.1 的 `src/path/path.go`、
 *     `match.go`，BSD-3-Clause） | **采用并逐函数移植**：`Clean` 的 lazybuf 回溯算法、
 *     `Match` 的 chunk/star 扫描都是 Rob Pike 设计、经过 Go 全量测试与十余年生产验证的
 *     实现；语义边界（前导 `..`、`//`、空串）有明确定义，自造算法极易在边角出错 |
 *   | `std::filesystem::path::lexically_normal` | **不采用**：语义与 Go 不一致。
 *     `lexically_normal("")` 返回 `""`（Go 返回 `"."`）、`"a//b"` 的保留方式、
 *     前导 `..` 与根路径的处理都不同；且它按平台分隔符工作，无法表达
 *     「永远用 `/` 且不碰文件系统」的 POSIX 路径语义 |
 *   | Python `posixpath.normpath` | **仅作对拍 oracle**：除「恰好两个前导斜杠」的
 *     POSIX 特例（`normpath("//a") == "//a"`，Go 为 `"/a"`）外与 Go 一致 |
 *
 *   **来源**：Go 1.27.1 `src/path/path.go`、`src/path/match.go`
 *   （<https://github.com/golang/go>，BSD-3-Clause）。移植日期 2026-10-18。
 *   改动：Go 的 `string`/`[]byte`/`utf8.DecodeRuneInString` 换成
 *   `std::string_view`/`std::string` 与本地 UTF-8 解码；`(bool, error)` 返回值
 *   改为 `MatchResult` 结构；其余算法、变量名与注释意图保留。
 *   未删减任何边界检查。
 *
 * ## 本模块**不做什么**
 *
 * - **不碰文件系统**：不 `stat`、不 `readlink`、不解析符号链接；
 * - **不认平台分隔符**：永远只把 `/` 当分隔符，`\` 是普通字符；
 * - Windows 盘符 / UNC 属于 `path/filepath` 的职责，见 `filepath.hpp`。
 *
 * ## 关键语义（容易写错的地方）
 *
 * - `Clean("")` → `"."`；`Clean("/")` → `"/"`；`Clean("//a")` → `"/a"`；
 * - `Clean("a/../b")` → `"b"`；`Clean("a/./b")` → `"a/b"`；`Clean("a//b")` → `"a/b"`；
 * - **前导 `..` 永远保留至少一个**：`Clean("../a")` → `"../a"`，
 *   `Clean("a/../../b")` → `"../b"`，`Clean("abc/def/../../..")` → `".."`；
 * - `Base("/a/b/")` → `"b"`；`Base("/")` → `"/"`；`Base("")` → `"."`；
 * - `Join("a", "")` → `"a"`；`Join("", "")` → `""`（**空参数列表或全空 → 空串**，
 *   这是 `Join` 唯一不返回 `"."` 的情况）。
 */

/**
 * ## 模块定位（补充）
 *
 * 本文件属于**基础设施最底层**的纯词法层：不分配 fd、不访问文件系统、不依赖
 * `go2cpp::os` / `config` / 调度器。`go2cpp::path::Filepath`（平台相关路径）在
 * Linux 上把 `Clean`/`Split`/`Ext`/`Base`/`Dir`/`IsAbs`/`Join`/`Match` 直接转发
 * 到本文件；而 `config` 的 INI 导出与 `os` 的目录操作**刻意不依赖**它，各自
 * 使用 `std::filesystem` 或裸 POSIX，避免底层模块之间形成环。
 *
 * ## 本文件定义什么
 *
 * - `Clean`/`Split`/`Ext`/`Base`/`Dir`/`IsAbs`/`JoinElements`/`Join`：Go `path`
 *   包的纯字符串路径操作，全部按字节处理、永远只认 `/`；
 * - `MatchResult`/`MatchChecked`/`Match`：shell 通配符匹配，并把「不匹配」与
 *   「模式非法」区分为两个字段；
 * - 模板 `Join`：只做参数转发，不引入额外语义。
 *
 * ## 阅读建议
 *
 * 先记住文件顶部「关键语义」里三个反直觉点：`Clean("") == "."`、相对路径的
 * 前导 `..` 至少保留一个、`Join` 全空参数返回空串而非 `"."`；再读 `Clean` 的
 * 四条规则，最后看 `MatchChecked` 的 UTF-8 rune 匹配。实现见
 * `src/path/path.cpp` 与 `src/path/match_impl.hpp`。
 *
 * ## 边界与陷阱
 *
 * - 只认 `/`：`\` 在此是普通字符，Windows 盘符/UNC 属于 `Filepath`；
 * - 不做 `stat`/`readlink`，`Clean("a/../b")` 不关心 `a` 是否存在；
 * - `Join()` 全空参数返回**空串**，与 `Clean("")` 的 `"."` 不是同一语义。
 */
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp::path {

/**
 * @brief 词法规范化：返回与 `path` 等价的最短路径名。
 *
 * @details 迭代应用四条规则直到稳定：
 *          1. 连续 `/` 合并为一个；
 *          2. 删除每个 `.` 元素；
 *          3. 删除「内部 `..` + 它前面的非 `..` 元素」；
 *          4. 根路径开头的 `/..` 归约为 `/`。
 *
 *          结果**仅当它是根 `/` 时**才以 `/` 结尾；若结果为空串则返回 `"."`。
 *          相对路径开头的 `..` 无法消解时会**保留**（至少一个）。
 *
 * @param path 输入路径（可含任意字节，按字节处理，不做 UTF-8 校验）。
 * @return 规范化后的路径；不分配新字符串（输入已规范时返回值与输入内容相同）。
 * @note 纯词法操作：不解析符号链接，不访问文件系统。
 */
std::string Clean(std::string_view path);

/**
 * @brief 拆分路径：在最后一个 `/` 之后切开，分界符归入 dir。
 * @param path 输入路径。
 * @return `{dir, file}`，满足 `dir + file == path`。
 *         - `"a/b"` → `{"a/", "b"}`；`"a/b/"` → `{"a/b/", ""}`；
 *         - 无 `/` 时 → `{"", path}`；`"/"` → `{"/", ""}`。
 */
std::pair<std::string, std::string> Split(std::string_view path);

/**
 * @brief 返回最后一个 `.` 起的扩展名（只看最后一个 `/` 之后的元素）。
 * @return 形如 `".go"`；无点返回空串。`"a.dir/b"` → `""`，`"a.dir/b.go"` → `".go"`。
 */
std::string Ext(std::string_view path);

/**
 * @brief 返回路径的最后一个元素（GNU basename 的「去掉尾部分隔符」版本）。
 * @return 空串输入返回 `"."`；全斜杠返回 `"/"`；`"/a/b/"` 返回 `"b"`。
 * @note 不做 `Clean`，因此 `Base("a/..")` 返回 `".."`（与 Go 一致）。
 */
std::string Base(std::string_view path);

/**
 * @brief 返回除最后一个元素之外的部分，并对结果做 `Clean`。
 * @return 空串 → `"."`；`"a"` → `"."`；`"/a"` → `"/"`；`"a/b"` → `"a"`。
 *         除根 `/` 外，返回值不以 `/` 结尾。
 */
std::string Dir(std::string_view path);

/**
 * @brief 判断是否为绝对路径（POSIX：非空且以 `/` 开头）。
 */
bool IsAbs(std::string_view path);

/**
 * @brief 用 `/` 连接若干路径元素，忽略空元素，最后做一次 `Clean`。
 *
 * @details 若参数列表为空、或**所有**元素都为空，返回**空串**（不是 `"."`）：
 *          - `Join("a", "")` → `"a"`；
 *          - `Join("", "")` → `""`；
 *          - `Join("a/", "b")` → `"a/b"`；
 *          - `Join("/", "a")` → `"/a"`。
 *
 * @note 实现保留了中间空元素产生的 `/`（例如 `Join("a", "", "b")` 内部先得到
 *       `"a//b"`），再统一由 `Clean` 收敛，与 Go 的 `Join` 逐字节一致。
 */
std::string JoinElements(std::initializer_list<std::string_view> elems);

/// @brief `Join` 的 vector 重载（便于从容器转发）。
std::string JoinElements(const std::vector<std::string>& elems);

/**
 * @brief 可变参数版 `Join(a, b, ...)`。
 * @tparam Args 任意可转换为 `std::string_view` 的类型（`const char*`/`std::string` 等）。
 */
template <typename... Args,
          typename = std::enable_if_t<
              (std::is_convertible_v<const Args&, std::string_view> && ...)>>
std::string Join(const Args&... args) {
    return JoinElements(std::initializer_list<std::string_view>{args...});
}

/// @brief `Join` 的 vector 重载（容器转发；不参与上面的可变参数模板）。
std::string Join(const std::vector<std::string>& elems);

/**
 * @brief `Match` 的结果：区分「不匹配」与「模式本身非法」。
 * @details Go 的 `Match` 返回 `(matched bool, err error)`，唯一可能的错误是
 *          `ErrBadPattern`。C++ 里用两个 bool 表达，避免为一条错误信息引入异常。
 */
/**
 * ## 定位
 * 被 `MatchChecked`/`Match` 返回，也被 `Filepath::Glob` 用于区分「模式非法」与
 * 「没有匹配」：否则用户写错 `[` 只会得到空列表，排查成本很高。
 *
 * ## 不变量
 * - `bad_pattern == true` 时 `matched` 恒为 false；
 * - `operator bool` 只表示 `matched`，**不**表示模式合法。
 */
struct MatchResult final {
    /// 是否整体匹配（模式必须匹配 name 的全部内容，不是子串）。
    bool matched{false};
    /// 模式是否非法（如 `[` 未闭合、`\\` 结尾）。为 true 时 `matched` 必为 false。
    bool bad_pattern{false};

    explicit operator bool() const noexcept { return matched; }
};

/**
 * @brief shell 通配符匹配（`*`/`?`/`[...]`/`\\` 转义），`/` 不参与 `*`/`?` 的跨度。
 *
 * @details 语法与 Go `path.Match` 完全一致：
 *          - `*` 匹配任意多个**非 `/`** 字符；
 *          - `?` 匹配单个**非 `/`** 字符（按 UTF-8 rune 计）；
 *          - `[^a-z]` / `[a-z]` 字符类（按 rune 比较，支持 `\\` 转义）；
 *          - `\\c` 转义为字面量 `c`。
 *
 *          模式必须匹配 `name` 的**全部内容**；`/` 只能由模式中的 `/` 字面量匹配。
 *
 * @param pattern 模式。
 * @param name 待匹配名字。
 * @return `matched` 是否匹配；`bad_pattern` 模式是否非法。模式非法时
 *         本函数**不抛异常**，调用方可通过返回值区分（`Match` 便捷版直接返回
 *         `false`）。
 */
MatchResult MatchChecked(std::string_view pattern, std::string_view name);

/**
 * @brief `MatchChecked` 的便捷版：模式非法时返回 `false`。
 * @note 需要区分「不匹配」与「模式写错」时用 `MatchChecked`（例如 `Glob` 的实现）。
 */
bool Match(std::string_view pattern, std::string_view name);

}  // namespace go2cpp::path
