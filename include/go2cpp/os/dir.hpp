/**
 * @file dir.hpp
 * @brief `os` 模块的**目录 / 路径 / 符号链接 / 时间戳**能力。
 *
 * ## 为什么单独一个文件而不是塞进 file.hpp
 *
 * `file.hpp` 的职责是「一个已打开的句柄」，而这里全是**按路径操作**的能力
 * （`opendir` / `mkdir` / `unlink` / `readlink` / `utimensat`）。职责不同，
 * 拆开后 `file.hpp` 那 300 多行的边界论证也不会被稀释。
 *
 * ## 关键结论：`opendir` / `readdir` **不需要绕过 Hook**
 *
 * `file.hpp` 里 `os::File` 的读写必须走 `::syscall(SYS_read/SYS_write/...)`，
 * 因为 `read`/`write` **在 `HookOp` 枚举里**（`kRead` / `kWrite`），会被 Hook 拦截。
 *
 * 但目录遍历不需要这样做，理由是查证过的：
 *
 * 1. `HookOp` 共 19 项（`kRead`/`kReadv`/`kRecv`/`kRecvfrom`/`kWrite`/`kWritev`/
 *    `kSend`/`kSendto`/`kConnect`/`kAccept`/`kSleep`/`kPoll`/`kSelect`/
 *    `kEpollWait`/`kClose`/`kShutdown`/`kFcntl`/`kDup`/`kDup2`），
 *    **没有 `opendir` / `readdir` / `getdents`**；
 * 2. `readdir` 底层是 `getdents64` 系统调用，**不是 `read`** —— 即使 Hook 拦了
 *    `read`，也不会碰到目录读取路径；
 * 3. `opendir` 内部虽然会 `open` 一个 fd，但那个 fd 之后只被 `getdents64` 使用，
 *    不会被 `read`/`write` 碰到，所以 `inspect_socket()` 也不会把它当 socket 接管。
 *
 * 因此本文件**直接用 libc 的 `opendir`/`readdir`/`mkdir`/`unlink`/`readlink`**，
 * 不用 `syscall(SYS_*)`。这不是偷懒 —— 绕 syscall 的唯一理由是"Hook 会拦"，
 * 而这些函数不会被拦，绕过去只会让代码难读且失去 glibc 的 `d_type` 优化。
 *
 * **唯一例外**：`SetTimes` 用 `syscall(SYS_utimensat)` —— 不是因为 Hook，
 * 而是因为 `utimensat` 在 glibc 里是 `utimensat`/`futimens` 两个名字，
 * 直接走 syscall 可以避开不同 glibc 版本对 `struct timespec` 的封装差异。
 *
 * ## 明确不负责的事
 *
 * - 不做路径规范化（`Join`/`Clean`/`Abs` —— 用 `std::filesystem::path`）；
 * - 不做权限/属主的便捷封装（`StatPath` 透传 `struct stat`）；
 * - 不跟随符号链接做递归（`RemoveAll` 用 `lstat`，删链接本身而不删目标 —— 与 `rm -rf` 一致）；
 * - POSIX only。
 */
/**
 * ## 模块定位（补充）
 *
 * `go2cpp::os` 的**按路径操作**子层：目录遍历、递归创建/删除、符号链接、时间戳、
 * `stat`。上游是 `tests/os/test_os_dir.cpp`、examples 以及未来的配置目录巡检；
 * 下游是 glibc `opendir`/`readdir`/`mkdir`/`unlink`/`readlink`（理由见文件头：
 * `HookOp` 没有 `getdents`，这些调用不会被 Hook 拦到等待路径）。
 *
 * ## 本文件定义什么
 *
 * - `DirEntry`：目录条目的名字 + 类型标记（`d_type` 直读，不额外 `lstat`）；
 * - `ReadDir`/`MkdirAll`/`RemoveAll`：目录级操作；
 * - `Readlink`/`Symlink`/`SetTimes`/`StatPath`：链接与元数据操作。
 *
 * ## 阅读建议
 *
 * 先读 `DirEntry::type_unknown` 的含义（网络文件系统可能不给 `d_type`），再看
 * `RemoveAll` 的「两阶段收集再删除」；`SetTimes` 是唯一走 `syscall` 的函数，
 * 原因与 Hook 无关，见函数注释。
 *
 * ## 边界与陷阱
 *
 * - 不做路径规范化，调用方自行 `Clean`/`Join`；
 * - 所有按路径的入口遇到**内嵌 NUL** 一律返回 `kInvalidArgument`（`EINVAL`）：
 *   POSIX API 只认到首个 NUL，不拦会静默操作另一个路径（路径注入）；
 * - `RemoveAll` 用 `lstat`：符号链接只删链接本身，绝不递归进目标；
 * - `MkdirAll` 中途失败不回滚，已创建目录保留（与 `mkdir -p` 一致）。
 */
#pragma once

#include "go2cpp/os/file.hpp"

#include <sys/stat.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace go2cpp::os {

/**
 * @brief 一个目录条目。
 * @details 只带**类型标记**，不带完整 `struct stat` —— 那要额外 `lstat`，
 *          而调用方多数只想区分文件/目录/符号链接。需要完整信息时用
 *          `StatPath` 单独查。
 */
/**
 * ## 定位
 * 由 `os::ReadDir` 填充并返回给需要目录列表的上层（测试、示例、后续的静态
 * 资源/配置巡检）；`type_unknown` 让调用方自行决定是否再 `StatPath`。
 *
 * ## 不变量
 * - `name` 是纯文件名，不含父目录（与 `readdir` 的 `d_name` 一致）；
 * - 三个类型标记至多一个为 true；`DT_UNKNOWN` 时全部为 false 且
 *   `type_unknown == true`。
 */
struct DirEntry final {
    /// 条目的名字（**不含**父目录路径，与 `readdir` 的 `d_name` 一致）。
    std::string name;
    bool is_dir{false};  ///< 是否目录；ReadDir 按 d_type=DT_DIR 置位，块/字符设备等保持 false
    bool is_regular{false};  ///< 是否普通文件；ReadDir 按 DT_REG 置位，供调用方过滤目录
    bool is_symlink{false};  ///< 是否符号链接；ReadDir 按 DT_LNK 置位（不跟随链接）
    /// 文件系统返回 `DT_UNKNOWN` 时，类型标记全部为 false —— 调用方若需要
    /// 精确类型，应再用 `StatPath` 查。ext4/xfs/tmpfs 不会出现这种情况，
    /// 但某些网络文件系统会。
    bool type_unknown{false};  ///< d_type==DT_UNKNOWN；ReadDir 置位，调用方据此决定是否再 StatPath
};

/**
 * @brief 列出一个目录下的条目（不递归）。
 * @param path 目录路径。
 * @param out 成功时写入条目列表（**先清空**）。顺序为文件系统返回顺序，不排序
 *            —— 需要稳定顺序请自行按 `name` 排序。
 * @param error 失败时写入中文诊断，可为空。
 * @return 状态。目录不存在返回 `kOpenFailed` 且 `system_error == ENOENT`。
 * @note 自动跳过 `.` 与 `..`（与 Go 的 `os.ReadDir` 一致）。
 */
FileStatus ReadDir(const std::string& path, std::vector<DirEntry>* out,
                   std::string* error = nullptr);

/**
 * @brief 递归创建目录（等价 `mkdir -p`）。
 * @param path 目标路径。
 * @param mode 新建目录的权限（受 `umask` 影响，与 `mkdir(2)` 一致）。
 * @return 状态。**已存在且是目录时视为成功**；已存在但是普通文件返回 `kIoFailed`
 *         且 `system_error == EEXIST`。
 * @note 逐段创建，中途失败时**已创建的目录保留**（不做回滚，与 `mkdir -p` 一致）。
 */
FileStatus MkdirAll(const std::string& path, mode_t mode = 0755,
                    std::string* error = nullptr);

/**
 * @brief 递归删除文件或目录（等价 `rm -rf`）。
 * @param path 目标路径。
 * @return 状态。**路径不存在视为成功**（与 Go 的 `os.RemoveAll` 一致）。
 * @warning **拒绝 `"."` / `".."`**（以及 `"a/."` 这类最后一段是点段的路径），
 *          返回 `kInvalidArgument`（`EINVAL`）。这类调用只会清空当前/父目录，
 *          然后在 `rmdir` 阶段失败 —— 没有成功可能。注意这与 Go 的
 *          `os.RemoveAll(".")` 不同：Go 会先删子项再报错，本实现选择不删。
 * @warning 本函数**不做路径规范化**：`RemoveAll("/")` 会尝试清空根目录
 *          （有权限时），`RemoveAll("..")` 已在上面被拒绝，但 `RemoveAll("../x")`
 *          仍按传入路径操作。调用方必须自己保证路径来自可信输入。
 * @warning 用 `lstat` 判断类型：**符号链接删链接本身，不递归进目标** ——
 *          这点与 `rm -rf` 一致。若链接指向目录，只删链接。
 * @note 用"先收集子项再递归"的两阶段写法：`readdir` 期间删除条目在某些
 *       文件系统上行为未定义，所以先把名字拷贝出来再删。
 */
FileStatus RemoveAll(const std::string& path, std::string* error = nullptr);

/**
 * @brief 读取符号链接的目标（不跟随）。
 * @param path 链接路径。
 * @param out 成功时写入目标（**先清空**）。
 * @return 状态。不是链接时返回 `kIoFailed` 且 `system_error == EINVAL`。
 * @note 内部用**动态扩容**重试：先按 256 字节读，若返回值等于缓冲区大小
 *       说明可能被截断，翻倍重试（最多 8 次，即 32 KiB）。
 */
FileStatus Readlink(const std::string& path, std::string* out,
                    std::string* error = nullptr);

/**
 * @brief 创建符号链接。
 * @param target 链接指向的路径（**不要求存在** —— 允许悬空链接）。
 * @param link_path 要创建的链接路径。
 * @return 状态。目标已存在返回 `kIoFailed` 且 `system_error == EEXIST`
 *         （`symlink(2)` 不会覆盖已有路径）。
 */
FileStatus Symlink(const std::string& target, const std::string& link_path,
                   std::string* error = nullptr);

/**
 * @brief 设置文件的访问时间与修改时间。
 * @param path 路径。**跟随符号链接**：实现是 `utimensat(..., flags=0)`，
 *        对链接调用时改的是**目标文件**的时间，链接自身的 times 不变
 *        （与 Go 的 `os.Chtimes` 一致）。要改链接自身的 times 需要
 *        `AT_SYMLINK_NOFOLLOW`，本 API 不提供 —— POSIX 也只保证对普通文件
 *        可靠可设。悬空链接返回 `ENOENT`。
 * @param atime 访问时间。
 * @param mtime 修改时间。
 * @return 状态。
 * @note 用 `syscall(SYS_utimensat)` 是为了避开不同 glibc 版本对
 *       `utimensat`/`futimens` 的封装差异，不是因为 Hook。
 * @note 若想"只设 mtime、保留 atime"，传 `UTIME_OMIT` 需要用
 *       `SetTimesRaw`（本文件未提供 —— 需要时再加，避免过早抽象）。
 * @warning 跟随语义意味着：把 `SetTimes` 用在不可信路径上时，符号链接会把
 *          修改作用到链接目标（与 `os.Chtimes` 相同的 TOCTOU 面）；需要
 *          "只改链接/不跟随"时先用 `StatPath(..., false)` 判断或改用
 *          `AT_SYMLINK_NOFOLLOW`（本模块暂未暴露）。
 */
FileStatus SetTimes(const std::string& path, const struct timespec& atime,
                    const struct timespec& mtime, std::string* error = nullptr);

/**
 * @brief 按路径取 `struct stat`。
 * @param path 路径。
 * @param out 成功时写入（**调用方负责传入非空指针**）。
 * @param follow_symlink `true` 用 `stat`（跟随链接），`false` 用 `lstat`（不跟随）。
 * @return 状态。
 */
FileStatus StatPath(const std::string& path, struct ::stat* out,
                    bool follow_symlink = true, std::string* error = nullptr);

}  // namespace go2cpp::os
