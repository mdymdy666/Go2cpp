/**
 * @file atomic_file.hpp
 * @brief 跨平台「原子文件操作」——`rename` / `unlink` 的契约差异封装。
 *
 * ## 契约差异（**Windows 上最容易踩的两个坑**）
 * | 操作 | Linux（POSIX） | Windows | 差异 |
 * |---|---|---|---|
 * | `rename(from, to)` | **目标存在时直接覆盖**（原子） | **目标存在时失败**（`ERROR_ALREADY_EXISTS`） | **必须用 `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`** |
 * | `unlink(path)` | 打开中的文件**也能删**（目录项移除，inode 延后释放） | **打开中的文件删不掉**（`ERROR_SHARING_VIOLATION`） | 打开时需 `FILE_SHARE_DELETE`，否则轮转/清理会失败 |
 *
 * ## 对本项目的影响
 * - **WAL/SST 的「写临时文件 → rename 覆盖」**：Windows 上 `::rename` 会失败 → 必须用 `AtomicRename`；
 * - **日志轮转的「unlink 旧文件」**：Windows 上如果日志文件还被打开 → 删不掉 → `DeleteFileIfExists` 返回失败，调用方需**重试或延后**；
 * - **崩溃一致性**：`MoveFileExW` 在 NTFS 上是**元数据原子**（不是数据原子）——数据必须先 `FlushFileBuffers`。
 *
 * 详见 `docs/reports/cross-platform-syscall-audit.md`（TOP6）。
 */
#pragma once

#include <string>

namespace go2cpp::platform {

/**
 * @brief 原子地把 `from` 重命名为 `to`（目标已存在时**覆盖**）。
 *
 * @param from [in] 源路径（UTF-8）；必须存在。
 * @param to [in] 目标路径（UTF-8）；可已存在（会被覆盖）。
 * @param error [out] 失败时写 errno 风格错误码（Linux `errno` / Windows `GetLastError`）；成功时保持原值。
 * @return true=成功；false=失败（error 已写）。
 *
 * ## 并发/失败语义
 * - 原子性：Linux `rename(2)` 是**原子**的；Windows `MoveFileExW` 在 NTFS 上**元数据原子**（数据需先 flush）；
 * - 失败后状态：**源文件不变**（不会出现「删了源但目标没写成」）；
 * - 跨卷：**不支持**（Linux `EXDEV` / Windows `ERROR_NOT_SAME_DEVICE`）——调用方应保证同卷。
 */
bool AtomicRename(const std::string& from, const std::string& to,
                  int* error = nullptr);

/**
 * @brief 删除文件；不存在视为成功（幂等）。
 *
 * @param path [in] 路径（UTF-8）。
 * @param error [out] 失败时写错误码（不存在也算成功，不写）。
 * @return true=已删除或本来不存在；false=删除失败（如 Windows 上文件被打开）。
 *
 * ## 契约差异
 * Windows 上**打开中的文件删不掉**（`ERROR_SHARING_VIOLATION`）——调用方需重试或延后。
 */
bool DeleteFileIfExists(const std::string& path, int* error = nullptr);

}  // namespace go2cpp::platform
