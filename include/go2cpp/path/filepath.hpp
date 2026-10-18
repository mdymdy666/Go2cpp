/**
 * @file filepath.hpp
 * @brief Go `path/filepath` 包（平台相关路径操作）的 C++ 对应实现。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（移植自 Go 标准库，见下方来源说明）
 *
 * @details 复用优先的结论：
 *   | 候选 | 结论 |
 *   |---|---|
 *   | Go 标准库 `path/filepath`（本机 Go 1.27.1 的 `src/path/filepath/` 目录，
 *     含 `path.go`、`match.go`、`symlink.go`，BSD-3-Clause） | **采用并移植**：
 *     `Rel` 的逐段对齐、`Walk` 的 SkipDir/SkipAll 传播、`EvalSymlinks` 的
 *     255 层链接上限、`Glob` 的 CVE-2022-30632 递归深度上限都是踩过坑的实现 |
 *   | `std::filesystem`（C++17） | **不采用**：异常/`error_code` 矩阵与 Go 的
 *     `(value, error)` 语义不同；`Absolute` 不保证 `Clean`、`relative` 在跨根时
 *     直接失败而不是给出 `../../x`；`recursive_directory_iterator` 默认**跟随**
 *     符号链接选项、遍历顺序也未定义，与本模块「不跟链接 + 词典序」的可验证约定冲突 |
 *   | `boost::filesystem` | **不采用**：额外依赖，收益与 `std::filesystem` 相同 |
 *
 *   **来源**：Go 1.27.1 `src/path/filepath/{path.go,match.go,symlink.go}`。
 *   移植日期 2026-10-18。改动：`os.Lstat`/`os.Readlink`/`os.Getwd` 换成
 *   POSIX `lstat`/`readlink`/`getcwd`；`[value, error]` 换成「bool 返回值 +
 *   输出参数 + 中文错误串」；Go 的 `fs.FileInfo` 收敛为 `WalkInfo`。
 *   **未删减任何边界检查**（含 255 层符号链接上限与 Glob 的 10000 层深度上限）。
 *
 * ## 平台语义
 *
 * - Linux：分隔符 `/`，`Clean`/`Join`/`Base`/`Dir`/`Ext`/`Split`/`IsAbs`/`Match`
 *   直接复用 `go2cpp::path` 的实现（Go 在 Unix 上正是这样分派的）；
 * - Windows：分隔符 `\`，另有一套 `#ifdef _WIN32` 的词法实现（盘符 / UNC / 正斜杠
 *   统一转换），**当前只保证 Linux 正确**，Windows 分支属于「能编译、按 Go 文档
 *   编写但未在真机验证」的状态，见交付报告「未做」一节。
 *
 * ## 与文件系统的关系
 *
 * `Abs`/`Rel`/`Walk`/`Glob`/`EvalSymlinks` 会访问文件系统；其余函数纯词法。
 * 全部函数**不依赖** `go2cpp::os` 模块（保持边界清晰，`os` 未做路径规范化）。
 *
 * ## 已知未支持项及原因（C 类，有意不做）
 *
 * | 未支持项 | 原因 |
 * |---|---|
 * | `WalkDir` | Go 的 `WalkDir` 用 `fs.DirEntry` 免去对每个条目额外 `lstat`；本项目没有 `fs` 抽象，`WalkInfo` 已经在遍历时携带 `lstat` 结果，`Walk` 覆盖同一用途。另起一套 API 只会造成两份相似实现漂移。 |
 * | Windows 未真机验证 | `#ifdef _WIN32` 分支按 Go 文档编写，但当前 CI/开发环境只有 Linux；Windows 的盘符/UNC/保留名（`NUL`、`CON`）语义无法在本机验证。`IsLocal` 的 Windows 分支**不检查**保留名，已在函数注释标明。 |
 * | `Localize` 的错误返回 | Go 的 `Localize(path) (string, error)` 在 Windows 上可能因非法卷名失败；本模块统一用「返回值」形态（Unix 恒成功），Windows 失败场景归入未验证项。 |
 * | `EvalSymlinks` 的 Windows 实现 | 用 `std::filesystem::weakly_canonical` 近似，对陈旧路径（不存在的尾段）与链接循环的行为与 Go 不同；Linux 走完整移植实现。 |
 */

#pragma once

#include "go2cpp/path/path.hpp"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp::path::Filepath {

/// 路径分隔符（Linux `/`，Windows `\`）。对应 Go `filepath.Separator`。
inline constexpr char Separator =
#ifdef _WIN32
    '\\';
#else
    '/';
#endif

/// `PATH` 类环境变量的列表分隔符（Linux `:`，Windows `;`）。
inline constexpr char ListSeparator =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

// ---------------------------------------------------------------------------
// 纯词法函数（与 path 同名；Linux 下直接转发）
// ---------------------------------------------------------------------------

/**
 * @brief 对应 Go `filepath.SplitList`：按 `ListSeparator` 拆分 `PATH` 类变量。
 * @param path 例如 `"/bin:/usr/bin"`。
 * @return 拆分结果；**空串返回空列表**（不是含一个空串的列表），与 Go 一致。
 *         `":"` 返回两个空串。
 */
std::vector<std::string> SplitList(std::string_view path);

/**
 * @brief 对应 Go `filepath.VolumeName`：返回卷名。
 * @details Unix 恒为空串；Windows 返回盘符（`C:`）或 UNC 共享（`\\host\share`）。
 * @note 该函数不 `Clean`、不解析符号链接，纯词法。
 */
std::string VolumeName(std::string_view path);

/**
 * @brief 对应 Go `filepath.ToSlash`：把平台分隔符换成 `/`。
 * @details Unix 是恒等变换；Windows 把 `\` 换成 `/`。
 */
std::string ToSlash(std::string_view path);

/**
 * @brief 对应 Go `filepath.FromSlash`：把 `/` 换成平台分隔符。
 * @details Unix 是恒等变换；Windows 把 `/` 换成 `\`。
 */
std::string FromSlash(std::string_view path);

/**
 * @brief 对应 Go `filepath.IsLocal`（Go 1.20+）：词法判断路径是否位于 base 子树内。
 * @details 判据与 Go 的 `unixIsLocal` 一致：
 *          - 空串、绝对路径 → false；
 *          - 含 `.`/`..` 段时先 `Clean`，结果为 `".."` 或以 `"../"` 开头 → false；
 *          - 其余 → true。
 *          因此 `IsLocal("a/../b") == true`、`IsLocal("a/../../b") == false`。
 * @note Windows 分支额外排除「去掉卷名后为空/绝对」的情况，但**不检查**
 *       `NUL`/`CON` 等保留名（未在 Windows 真机验证，见文件头 C 类清单）。
 */
bool IsLocal(std::string_view path);

/**
 * @brief 对应 Go `filepath.Localize`：把路径转成本地形式。
 * @details Unix 是恒等变换；Windows 近似为「去掉卷名」。
 * @note Go 的原型带 `error`（Windows 非法卷名可能失败）；本模块用返回值表达，
 *       Unix 不会失败，Windows 失败场景归入未验证项。
 */
std::string Localize(std::string_view path);

/// @brief 平台版 `Clean`。Windows 会保留盘符/UNC 前缀并统一为 `\`。
std::string Clean(std::string_view path);

/// @brief 平台版 `Split`：在最后一个分隔符之后切开，分隔符归入 dir。
std::pair<std::string, std::string> Split(std::string_view path);

/// @brief 平台版 `Ext`：最后一个分隔符之后、最后一个 `.` 起的后缀。
std::string Ext(std::string_view path);

/// @brief 平台版 `Base`：最后一个元素；空串 → `"."`，全分隔符 → 单个分隔符。
std::string Base(std::string_view path);

/// @brief 平台版 `Dir`：`Split` 的 dir 再 `Clean`。
std::string Dir(std::string_view path);

/// @brief 平台版 `IsAbs`：Linux 看 `/`；Windows 看盘符或前导分隔符。
bool IsAbs(std::string_view path);

/// @brief 平台版 `JoinElements`：空元素忽略，结果 `Clean`；全空 → 空串。
std::string JoinElements(std::initializer_list<std::string_view> elems);

/// @brief `JoinElements` 的 vector 重载。
std::string JoinElements(const std::vector<std::string>& elems);

/**
 * @brief 可变参数版 `Join(a, b, ...)`（平台分隔符）。
 * @note 与 `go2cpp::path::Join` 相同，参数全空时返回空串而非 `"."`。
 */
template <typename... Args,
          typename = std::enable_if_t<
              (std::is_convertible_v<const Args&, std::string_view> && ...)>>
std::string Join(const Args&... args) {
    return JoinElements(std::initializer_list<std::string_view>{args...});
}

/// @brief `Join` 的 vector 重载（容器转发；不参与上面的可变参数模板）。
std::string Join(const std::vector<std::string>& elems);

/// @brief 平台版通配符匹配结果，语义同 `go2cpp::path::MatchResult`。
using MatchResult = go2cpp::path::MatchResult;

/// @brief 平台版 `MatchChecked`。Windows 上 `\` 是分隔符而不是转义符。
MatchResult MatchChecked(std::string_view pattern, std::string_view name);

/// @brief 平台版 `Match` 便捷版（模式非法 → `false`）。
bool Match(std::string_view pattern, std::string_view name);

// ---------------------------------------------------------------------------
// 需要文件系统的函数
// ---------------------------------------------------------------------------

/**
 * @brief 返回绝对路径：相对路径拼上当前工作目录，结果 `Clean`。
 * @param path 输入路径。
 * @param out 成功时写入结果（**先清空**）。
 * @param error 失败时写入中文诊断，可为空。
 * @return 成功返回 true；`getcwd` 失败（如当前目录已被删除）返回 false。
 * @note 纯词法 + 一次 `getcwd`，**不解析符号链接** —— 与 Go 的 `Abs` 一致，
 *       因此同一文件可能有多个不同的绝对路径写法。
 */
bool Abs(std::string_view path, std::string* out, std::string* error = nullptr);

/**
 * @brief 计算 `target` 相对 `base` 的词法相对路径。
 *
 * @details 保证在 `base` 为目录的前提下
 *          `Join(base, Rel(base, target))` 与 `Clean(target)` 等价。
 *          - 两者相同 → `"."`；
 *          - 共享前缀后逐段 `..` 回退，再拼目标剩余段；
 *          - 一个绝对一个相对、或 Windows 卷不同 → 失败；
 *          - base 中残留前导 `..`（无法从根回退）→ 失败。
 *
 * @param base 基准路径。
 * @param target 目标路径。
 * @param out 成功时写入结果（**先清空**）。
 * @param error 失败时写入中文诊断，可为空。
 * @return 成功返回 true。
 */
bool Rel(std::string_view base, std::string_view target, std::string* out,
         std::string* error = nullptr);

/**
 * @brief 解析路径中的全部符号链接，返回规范化后的真实路径。
 *
 * @details 移植 Go `walkSymlinks`：逐个组件 `lstat`，遇到符号链接就 `readlink`
 *          并把链接内容拼回待处理路径；相对链接替换当前组件、绝对链接重置起点；
 *          组件 `..` 按已解析结果回退。**最多跟随 255 层链接**，超过报错
 *          （与 Go 的 `EvalSymlinks: too many links` 一致）。
 *
 * @param path 输入路径（相对路径的结果相对当前目录，可能含绝对链接跳转）。
 * @param out 成功时写入结果（**先清空**）。
 * @param error 失败时写入中文诊断（路径不存在 / 中间组件不是目录 / 链接过多），可为空。
 * @return 成功返回 true。
 */
bool EvalSymlinks(std::string_view path, std::string* out,
                  std::string* error = nullptr);

/**
 * @brief Walk 传给回调的条目信息（Go `fs.FileInfo` 的收敛版）。
 * @details 只保留路径处理常用字段；需要完整 `struct stat` 时调用方自行查询。
 */
struct WalkInfo final {
    /// 是否是目录（`lstat` 结果，**不跟随**符号链接）。
    bool is_dir{false};
    /// 是否是普通文件。
    bool is_regular{false};
    /// 是否是符号链接（此时 `is_dir`/`is_regular` 均为 false，即使链接指向目录）。
    bool is_symlink{false};
    /// 文件大小（字节；目录/链接的值由文件系统决定）。
    std::uintmax_t size{0};
};

/**
 * @brief Walk 回调的返回动作。
 * @details 与 Go 的 `SkipDir`/`SkipAll` 一致：
 *          - `Continue`：继续；
 *          - `SkipDir`：若当前是目录则跳过其子树；若当前是文件则跳过**父目录**
 *            剩余兄弟节点（Go 的既有语义，见 `Walk` 文档）；
 *          - `SkipAll`：立即结束整个遍历（顶层返回成功）。
 */
enum class WalkAction { Continue = 0, SkipDir = 1, SkipAll = 2 };

/**
 * @brief Walk 回调：`(path, info, error_code)`。
 *
 * @details `error_code != 0` 表示对该路径 `lstat`（或读取目录内容）失败：
 *          - 根路径 `lstat` 失败：`info` 为零值，`error_code` 为 errno；
 *          - 子项 `lstat` 失败：`info` 为零值，`error_code` 为 errno；
 *          - 目录内容读取失败：`info` 为该目录信息，`error_code` 为 errno，
 *            且**不会**继续递归该目录。
 *          回调可以忽略错误并返回 `Continue`（Go 也是这样把错误处理权交给调用方）。
 *
 * @note 回调在**遍历线程**同步调用；`path` 以 `root` 为前缀并用 `Join` 拼接
 *       （因此 `root = "x/../dir"` 时回调看到的是 `dir/...`，与 Go 一致）。
 */
using WalkFn =
    std::function<WalkAction(const std::string& path, const WalkInfo& info,
                             int error_code)>;

/**
 * @brief 递归遍历目录树，**先访问根，再按词典序访问子项**（深度优先）。
 *
 * @details 与 Go `filepath.Walk` 对齐：
 *          - 回调顺序为「父目录 → 子项（按名字升序）」，深度优先；
 *          - **不跟随符号链接**：符号链接作为一个普通条目回调，不递归进目标；
 *          - `root` 自身的 `lstat` 失败也会回调一次（`error_code != 0`）；
 *          - 目录读取失败时，先回调该目录（带 errno），然后不进入它。
 *
 * @param root 遍历根。
 * @param fn 回调（不可为空；为空时返回 false 并写 error）。
 * @param error 失败时写入中文诊断（仅当回调为空等调用方错误），可为空。
 * @return 遍历正常结束（含 SkipAll / 回调忽略错误）返回 true。
 * @warning `Walk` 在**单线程**里同步递归，深度等于目录深度；极深的树可能耗尽栈。
 */
bool Walk(std::string_view root, const WalkFn& fn, std::string* error = nullptr);

/**
 * @brief shell 通配符展开（语法同 `Match`，模式可跨目录）。
 *
 * @details 与 Go `filepath.Glob` 对齐：
 *          - 结果按目录逐层展开，每个目录内**按名字升序**；无匹配时返回空列表；
 *          - 文件系统错误（目录不存在、无权限、I/O 错误）**被忽略**，不报错；
 *          - 唯一可能的错误是模式非法（`bad pattern`）；
 *          - 递归深度上限 10000（CVE-2022-30632 的防护）。
 *
 * @param pattern 模式，例如 `"src/" 下的 *.cpp`、`"sub/" 下的 *.h`、
 *                `/usr/<前缀>/bin/ed`。
 * @param matches 成功时写入匹配结果（**先清空**）。
 * @param error 失败时写入中文诊断，可为空。
 * @return 成功（含无匹配）返回 true；模式非法返回 false。
 */
bool Glob(std::string_view pattern, std::vector<std::string>* matches,
          std::string* error = nullptr);

}  // namespace go2cpp::path::Filepath
