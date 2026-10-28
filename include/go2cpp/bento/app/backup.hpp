#pragma once

/**
 * @file backup.hpp
 * @brief MOD-31：备份打包（目录 → store-only zip）与 HTTP 上传。
 * @details 这一层只做两件事，且刻意保持无状态：
 *
 *          1. **Backup**：把若干文件/目录递归打包成一个 zip。条目名取输入
 *             路径的 basename，因此归档结构与本机绝对路径无关；输出先写
 *             `<out>.part.<pid>.<seq>`（唯一名），成功后才 rename，失败不会
 *             破坏已有的旧包，并发备份也不会互相覆盖临时包。
 *          2. **Upload**：把 zip 通过 HTTP PUT/POST 发给 endpoint，失败按
 *             指数退避重试（默认 3 次），**任何失败都不删除本地包**。
 *
 * ## 上传实现与依赖
 *
 * 使用系统 `curl`（通过 `go2cpp::os::proc::Run`，argv 直传、不经过 shell）
 * 而不是自己实现 socket：HTTPS、代理、重定向、分块上传都由 curl 处理，
 * 避免在备份模块里重复实现 TLS。请求配置（URL、token、自定义 header）
 * 通过 **stdin** 传给 `curl --config -`，不进入进程参数，避免 token 出现在
 * `ps`/`/proc/<pid>/cmdline`。运行时需要 PATH 中有 `curl`；缺失时返回
 * `UploadStatus::kCurlMissing`，不会静默跳过上传。
 *
 * ## 重试语义
 *
 * `max_retries = 3` 表示「首次尝试 + 3 次重试」，最多 4 次请求。每次失败
 * 后的等待为 `retry_delay * 2^attempt`（封顶 30s）。只有 2xx 视为成功；
 * 非 2xx、连接失败、超时都会重试。重试耗尽返回最后一次失败的具体状态
 * （`kHttpRejected`/`kNetworkError`/`kTimeout`），`attempts` 记录总次数。
 *
 * ## 线程模型
 *
 * `Backup` 与 `Upload` 都是阻塞调用，非线程安全（每次调用独立创建内部
 * 对象）。`Upload` 的重试等待使用 `std::this_thread::sleep_for`，不会
 * 占用 CPU。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "go2cpp/bento/app/zip.hpp"

namespace go2cpp::bento::app {

// ---------------------------------------------------------------------------
// Backup
// ---------------------------------------------------------------------------

/// `Backup` 的状态码。
enum class BackupStatus : std::uint8_t {
    kOk = 0,              ///< 成功。
    kInvalidArgument = 1, ///< 参数非法（空输入、空输出、名字冲突）。
    kInputNotFound = 2,   ///< 输入路径不存在或不可读。
    kIoError = 3,         ///< 创建临时包/rename 失败。
    kTooLarge = 4,        ///< 超过条目数/单文件/总量上限。
    kZipError = 5,        ///< 低层 zip 写入失败。
};

/// 状态转文本。
const char* BackupStatusToString(BackupStatus status) noexcept;

/** @brief `Backup` 的统计（来自低层 zip 写入器）。 */
struct BackupStats final {
    std::size_t files{0};            ///< 打包的普通文件数。
    std::size_t directories{0};      ///< 打包的目录条目数。
    std::size_t symlinks_skipped{0}; ///< 跳过的符号链接数。
    std::size_t special_skipped{0};  ///< 跳过的特殊文件数。
    std::uint64_t input_bytes{0};    ///< 未压缩数据总字节。
    std::uint64_t archive_bytes{0};  ///< 生成的 zip 字节数。
};

/** @brief `Backup` 的结果。 */
struct BackupResult final {
    BackupStatus status{BackupStatus::kOk}; ///< 状态。
    BackupStats stats;                      ///< 统计。
    std::string archive_path;               ///< 成功时的 zip 绝对/输入路径。
    std::string error;                      ///< 失败原因。

    bool ok() const noexcept { return status == BackupStatus::kOk; }
};

/**
 * @brief 把若干文件/目录打包成 store-only zip。
 *
 * @param paths 待备份路径；目录递归、文件单独成条目。至少一个。
 * @param out_zip 输出 zip 路径；父目录不存在时创建。已存在的同名包会
 *        在成功后被原子替换（POSIX rename 语义），失败时旧包保持原样。
 * @param limits 条目数/单条/总量上限（透传给 `ZipWriter`）。
 *
 * @details 条目名是输入路径的 basename；同一输入里出现同名顶层条目时
 *          返回 `kInvalidArgument`。符号链接与特殊文件跳过并计数。
 *          输出 zip 自身（以及 `.part.*` 临时文件）即使位于输入目录内也
 *          不会被打进归档。
 *
 * @return 成功时 `stats` 与 `archive_path` 有效；失败时旧包不受影响。
 */
BackupResult Backup(const std::vector<std::string>& paths, const std::string& out_zip,
                    const ZipLimits& limits = ZipLimits{});

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

/// 上传使用的 HTTP 方法。
enum class HttpMethod : std::uint8_t {
    kPut = 0, ///< PUT（默认，幂等，适合对象存储）。
    kPost = 1,///< POST（表单/网关上传）。
};

/// 方法转文本。
const char* HttpMethodToString(HttpMethod method) noexcept;

/// `Upload` 的状态码。
enum class UploadStatus : std::uint8_t {
    kOk = 0,               ///< 2xx，上传成功。
    kInvalidArgument = 1,  ///< endpoint/header/token/配置非法。
    kZipMissing = 2,       ///< 本地 zip 不存在或不是普通文件。
    kCurlMissing = 3,      ///< PATH 中找不到 curl。
    kNetworkError = 4,     ///< 连接失败/DNS/TLS/传输中断。
    kHttpRejected = 5,     ///< 服务端返回非 2xx。
    kTimeout = 6,          ///< 请求超时（重试耗尽时保留最后一次失败分类）。
    kLocalIoError = 7,     ///< 本地进程创建/管道失败。
};

/// 状态转文本。
const char* UploadStatusToString(UploadStatus status) noexcept;

/** @brief 一次上传请求。 */
struct UploadRequest final {
    /// `http(s)://host[:port]/path`；禁止空白、控制字符与双引号。
    std::string endpoint;
    /// HTTP 方法。
    HttpMethod method{HttpMethod::kPut};
    /// 附加 header，每项 `"Name: value"`；值中禁止 CR/LF/NUL。
    std::vector<std::string> headers;
    /// 非空时追加 `Authorization: Bearer <token>`（已有 Authorization 时不加）。
    std::string token;
    /// Content-Type；为空表示不显式发送（curl 默认 `application/octet-stream`）。
    std::string content_type{"application/octet-stream"};
};

/** @brief 上传重试与超时配置。 */
struct UploadOptions final {
    /// 首次尝试之外的额外重试次数，0 .. 16。
    int max_retries{3};
    /// 首次重试前等待；之后按 2 的幂递增，单次封顶 30s。
    std::chrono::milliseconds retry_delay{200};
    /// curl 连接超时。
    std::chrono::milliseconds connect_timeout{10000};
    /// curl 单次请求总超时。
    std::chrono::milliseconds request_timeout{120000};
};

/** @brief `Upload` 的结果。 */
struct UploadResult final {
    UploadStatus status{UploadStatus::kOk}; ///< 状态。
    int attempts{0};                        ///< 实际请求次数（含首次）。
    int http_status{0};                     ///< 最后一次 HTTP 状态码（0=无响应）。
    std::string error;                      ///< 最后一次失败原因。

    bool ok() const noexcept { return status == UploadStatus::kOk; }
};

/**
 * @brief 把本地 zip 上传到 HTTP(S) endpoint，失败重试且保留本地包。
 *
 * @param zip_path 本地 zip；函数只读，不会删除或修改它。
 * @param request endpoint、方法、header 与 token。
 * @param options 重试次数/间隔/超时。
 *
 * @details
 * - 2xx 成功；其它状态、连接失败、超时都会重试；
 * - 重试耗尽后返回最后一次失败的分类，`attempts == max_retries + 1`；
 * - curl 缺失立即返回 `kCurlMissing`（不重试、不删除包）；
 * - token 只通过 stdin 配置传递，不进入 argv。
 */
UploadResult Upload(const std::string& zip_path, const UploadRequest& request,
                    const UploadOptions& options = UploadOptions{});

} // namespace go2cpp::bento::app
