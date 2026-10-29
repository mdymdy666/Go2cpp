#pragma once

/**
 * @file fetch.hpp
 * @brief MOD-27：HTTP(S) 文件下发（下载、SHA-256 校验、重试、超时、
 *        大小上限与路径沙箱）。
 * @details 本模块解决的是「把远端的一个文件安全地落到本地目录」，不绑定
 *          Bento 链路：调用方可以把它接在命令通道、控制面或任意调度器后面。
 *
 *          ```
 *          Fetch(url, dest_dir, options)
 *            ├─ 解析 URL（http/https、host、port、path）
 *            ├─ 解析目标文件名（显式 filename 或 URL 最后一段）
 *            ├─ 路径沙箱：必须是 dest_dir 下的单段文件名
 *            ├─ 尝试（最多 1 + max_retries 次）
 *            │    ├─ http://  → 内置 HTTP/1.1 客户端（POSIX socket）
 *            │    └─ https:// → os::proc 调 curl（TLS 交给系统 curl）
 *            ├─ http：流式写 <目标>.go2cpp-part.<pid>.<seq>，同时算 SHA-256
 *            ├─ https：curl 写进 <目标>.go2cpp-part.<pid>.<seq>.XXXXXX/payload
 *            │         （0700 私有目录），校验类型/大小/摘要后再 rename
 *            ├─ 大小上限（Content-Length 预检 + 流式累计）
 *            ├─ 可选摘要比对（expected_sha256）
 *            └─ rename 原子落盘（失败清理临时文件）
 *          ```
 *
 * ## 为什么 http 自研、https 调 curl
 *
 * HTTP/1.1 的 GET 足够简单（无 TLS、无连接复用、无压缩），自己实现可以
 * 精确控制连接/读超时、大小上限和重定向，并且测试不依赖外部进程；HTTPS
 * 需要 TLS 栈，仓库约束是「不引入第三方库」，因此唯一可行的零链接依赖
 * 方案是调用系统 curl。两条路径共享外层的重试、沙箱、摘要与原子落盘，
 * 状态码与错误分类保持一致。curl 不存在时返回 `kBackendUnavailable`，
 * 不会悄悄降级成明文 HTTP。URL（可能含预签名 token）与输出路径都通过
 * stdin 的 `curl --config -` 传入，**不进入进程参数**，同机用户无法从
 * `/proc/<pid>/cmdline` 读到凭证。
 *
 * ## 路径沙箱（默认拒绝）
 *
 * 与 `bento::ResolveTransferPath` 的取舍一致：
 * - 文件名必须是**单段**：拒绝 `/`、`\`、`:`、控制字符、`.`、`..`；
 * - 拒绝绝对路径与空文件名；长度上限 200B（给 `.go2cpp-part.*` 后缀留余量）；
 * - `dest_dir` 必须已存在且是目录，解析后（含符号链接）目标必须仍在
 *   `dest_dir` 之下；
 * - http 路径的临时文件用 `O_CREAT|O_EXCL|O_NOFOLLOW` 创建；https 路径让
 *   curl 写进 `mkdtemp` 创建的 0700 私有目录，再由本进程用
 *   `O_RDONLY|O_NOFOLLOW` 打开、`fstat` 确认普通文件、同一 fd 上算摘要，
 *   最后 `rename` 成临时文件。两条路径都不会把写入重定向到沙箱外；
 *   目标落盘统一用 `rename`（替换链接本身，不跟随）。
 *
 * ## 重试语义
 *
 * `max_retries = 3` 表示最多 **3 次重试**、共 4 次尝试。可重试的错误：
 * DNS/连接失败、超时、响应截断、TLS/curl 临时失败、摘要不符、HTTP
 * 408/425/429/5xx。不可重试：参数/沙箱错误、目标已存在、超过大小上限、
 * 本地 IO 错误、4xx（除上面的例外）。用尽重试后返回 `kRetryExhausted`，
 * `message` 保留最后一次失败原因；`attempts` 记录实际尝试次数。
 *
 * ## 大小与超时默认值
 *
 * | 项 | 默认 | 硬上限 |
 * |---|---|---|
 * | 文件大小 | 64 MiB | 4 GiB |
 * | 连接超时 | 5s | 10min |
 * | 单次 IO 空闲超时 | 15s | 10min |
 * | 总超时 | 10min | 10min |
 * | 重试次数 | 3 | 16 |
 * | URL 长度 | 8192B | 8192B |
 * | 响应头 | 64KiB | 64KiB |
 *
 * ## 三层超时的实现方式
 *
 * - **连接超时**：非阻塞 `connect` + `poll`，同时受 `connect_timeout_ms` 与
 *   本次尝试的总截止时间约束；
 * - **单次 IO 空闲超时**：每次 `recv` 前 `poll`，空闲超过 `read_timeout_ms`
 *   即失败。HTTP 路径精确到毫秒；HTTPS 路径折成 curl 的
 *   `--speed-limit 1 --speed-time N`（N = `read_timeout_ms` 向上取整的秒数），
 *   语义是「N 秒窗口内平均速度低于 1 B/s 即中止」，属于近似而非精确计时；
 * - **总超时**：从本次尝试开始计时，覆盖建连、发送、等待与读取。重试时每次
 *   尝试重新计时，因此 `attempts` 次尝试各自受 `total_timeout_ms` 约束。
 *
 * @note `Fetch` 是同步阻塞的：在 Fiber 中调用会阻塞当前 M。需要并发下载时
 *       由调用方放到线程池（与 `os::proc` 的边界一致）。
 * @note 只支持 HTTP/1.1 的 `identity` 编码；响应声明 `gzip`/`br` 等编码时
 *       返回 `kProtocolError`（请求本身不发 `Accept-Encoding`，正常服务器
 *       不会压缩）。
 * @note HTTPS 重定向到 http 会被 curl 的 `--proto-redir =https` 拒绝
 *       （安全默认）；需要跨协议重定向的调用方应自行处理。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace go2cpp::bento::app {

// ---------------------------------------------------------------------------
// 常量与上限
// ---------------------------------------------------------------------------

/// 默认下载大小上限（64 MiB）。
inline constexpr std::uint64_t kFetchDefaultMaxBytes = 64ull * 1024 * 1024;
/// 下载大小硬上限（4 GiB）。
inline constexpr std::uint64_t kFetchMaxBytesCeiling = 4ull * 1024 * 1024 * 1024;
/// 默认连接超时（毫秒）。
inline constexpr std::uint32_t kFetchDefaultConnectTimeoutMs = 5 * 1000;
/// 默认单次 IO 空闲超时（毫秒）。
inline constexpr std::uint32_t kFetchDefaultReadTimeoutMs = 15 * 1000;
/// 默认总超时（毫秒，10 分钟）。
inline constexpr std::uint32_t kFetchDefaultTotalTimeoutMs = 600 * 1000;
/// 超时硬上限（毫秒，10 分钟）。
inline constexpr std::uint32_t kFetchMaxTimeoutMs = 600 * 1000;
/// 默认重试次数（不含首次尝试）。
inline constexpr int kFetchDefaultRetries = 3;
/// 重试次数硬上限。
inline constexpr int kFetchMaxRetries = 16;
/// URL 最大字节数。
inline constexpr std::size_t kFetchMaxUrlBytes = 8192;
/// 目标文件名最大字节数（留出临时文件后缀余量）。
inline constexpr std::size_t kFetchMaxFilenameBytes = 200;
/// 响应头累计最大字节数。
inline constexpr std::size_t kFetchMaxHeaderBytes = 64 * 1024;
/// 默认最大重定向次数。
inline constexpr std::size_t kFetchDefaultMaxRedirects = 5;
/// 重定向次数硬上限。
inline constexpr std::size_t kFetchMaxRedirects = 8;
/// 重试退避基数上限（毫秒）。
inline constexpr std::uint32_t kFetchMaxBackoffMs = 2000;

// ---------------------------------------------------------------------------
// 状态与配置
// ---------------------------------------------------------------------------

/**
 * @brief 一次下载的最终状态。
 *
 * @details 除 `kOk` 外都可携带 `message` 诊断。`kHttpError` 时
 *          `FetchResult::http_status` 是服务器返回的状态码；`kTimeout`
 *          可能是连接超时、读空闲超时或总超时，具体见 `message`。
 */
enum class FetchStatus : std::uint8_t {
    kOk = 0,                 ///< 下载并校验成功，文件已落盘。
    kInvalidArgument = 1,    ///< URL/选项/摘要格式非法。
    kUnsupportedScheme = 2,  ///< 不是 http/https。
    kPathRejected = 3,       ///< 目标路径未通过沙箱校验。
    kTargetExists = 4,       ///< 目标已存在且未允许覆盖。
    kDnsError = 5,           ///< 域名解析失败。
    kConnectFailed = 6,      ///< TCP 连接失败/被拒绝。
    kTimeout = 7,            ///< 连接、读或总超时。
    kHttpError = 8,          ///< 非 2xx 响应（3xx 无 Location 也归此类）。
    kTooLarge = 9,           ///< 超过 max_bytes 上限。
    kIoError = 10,           ///< 本地文件创建/写入/rename 失败。
    kChecksumMismatch = 11,  ///< SHA-256 与 expected_sha256 不符。
    kTruncated = 12,         ///< 响应体短于 Content-Length 或意外 EOF。
    kProtocolError = 13,     ///< 响应格式非法/编码不支持/头部超限。
    kRetryExhausted = 14,    ///< 可重试错误用尽重试次数。
    kTlsError = 15,          ///< HTTPS 握手/证书失败（curl 路径）。
    kBackendUnavailable = 16,///< https 需要 curl，但系统里找不到可执行文件。
};

/// 状态转文本（诊断/日志用，英文短名与项目其他模块一致）。
const char* FetchStatusToString(FetchStatus status) noexcept;

/** @brief 下载选项；默认值即安全默认（不覆盖、64MiB、3 次重试）。 */
struct FetchOptions final {
    /// 目标文件名；为空时取 URL 最后一段（解码后仍须通过沙箱校验）。
    std::string filename;
    /// 期望的 SHA-256（64 位十六进制，大小写不敏感）；为空表示不校验。
    std::string expected_sha256;
    /// 允许的最大字节数（1 .. `kFetchMaxBytesCeiling`）。
    std::uint64_t max_bytes{kFetchDefaultMaxBytes};
    /// 连接超时（1 .. `kFetchMaxTimeoutMs` 毫秒）。
    std::uint32_t connect_timeout_ms{kFetchDefaultConnectTimeoutMs};
    /// 单次 recv 的空闲超时（1 .. `kFetchMaxTimeoutMs` 毫秒）。
    std::uint32_t read_timeout_ms{kFetchDefaultReadTimeoutMs};
    /// 整次尝试的总超时（1 .. `kFetchMaxTimeoutMs` 毫秒）。
    std::uint32_t total_timeout_ms{kFetchDefaultTotalTimeoutMs};
    /// 重试次数（0 .. `kFetchMaxRetries`）。
    int max_retries{kFetchDefaultRetries};
    /// 线性指数退避基数（0 .. `kFetchMaxBackoffMs` 毫秒）。
    std::uint32_t retry_backoff_ms{100};
    /// 目标已存在时是否允许覆盖（默认拒绝）。
    bool overwrite{false};
    /// 是否跟随 3xx 重定向（默认允许）。
    bool allow_redirects{true};
    /// 最大重定向跳数（0 .. `kFetchMaxRedirects`）。
    std::size_t max_redirects{kFetchDefaultMaxRedirects};
};

/** @brief 下载结果；失败时 `path` 为空、`message` 说明原因。 */
struct FetchResult final {
    /// 最终状态。
    FetchStatus status{FetchStatus::kOk};
    /// 成功时的目标绝对路径。
    std::string path;
    /// 成功时的文件字节数。
    std::uint64_t bytes{0};
    /// 实际下载内容的 SHA-256（64 位小写十六进制；成功时必有）。
    std::string sha256;
    /// 最后一次 HTTP 响应的状态码（0 表示没有收到响应）。
    int http_status{0};
    /// 实际尝试次数（1 表示首次就成功，不重试）。
    int attempts{0};
    /// 失败原因/诊断；`kRetryExhausted` 时包含最后一次失败原因。
    std::string message;

    /// 是否成功。
    bool ok() const noexcept { return status == FetchStatus::kOk; }
};

// ---------------------------------------------------------------------------
// 路径与摘要辅助（可单独测试）
// ---------------------------------------------------------------------------

/**
 * @brief 把下载目标解析为沙箱内的绝对路径。
 * @param dest_dir 已存在的目标目录。
 * @param filename 单段文件名（不能是空/`.`/`..`/含分隔符）。
 * @param max_path_bytes 目标文件名长度上限（<= `kFetchMaxFilenameBytes`）。
 * @param resolved 成功时写入规范化后的绝对路径（非空）。
 * @return `kOk`、`kInvalidArgument` 或 `kPathRejected`。
 */
FetchStatus ResolveFetchPath(const std::string& dest_dir, const std::string& filename,
                             std::size_t max_path_bytes, std::string* resolved);

/**
 * @brief 校验并规范化 64 位十六进制 SHA-256。
 * @param text 输入（大小写不敏感，前后允许空白）。
 * @param canonical_lower_hex 成功时写入 64 位小写十六进制。
 * @return 格式合法返回 true。
 */
bool ParseSha256Hex(std::string_view text, std::string* canonical_lower_hex);

// ---------------------------------------------------------------------------
// 下载入口
// ---------------------------------------------------------------------------

/**
 * @brief 下载 `url` 到 `dest_dir`。
 *
 * @param url 绝对 http(s) URL；不接受 userinfo、fragment 与裸 IPv6 之外的
 *            非标准写法（IPv6 字面量用 `[::1]`）。
 * @param dest_dir 已存在且可写的目录（不会自动创建）。
 * @param options 选项；默认值见 `FetchOptions`。
 * @return 结果对象；失败时保证不残留临时文件与目标文件（目标原本不存在时）。
 *
 * @note 目标已存在时由 `options.overwrite` 决定：false 直接返回
 *       `kTargetExists`，不会发起网络请求。并发写同一目标时，`overwrite=false`
 *       通过 `renameat2(RENAME_NOREPLACE)` 排他落盘：只有一个请求成功，其余在
 *       落盘阶段返回 `kTargetExists`（不覆盖、不残留临时文件）。
 * @note 成功落盘是原子的：内容先写临时文件，校验通过后 `rename`。
 */
FetchResult Fetch(std::string_view url, const std::string& dest_dir,
                  const FetchOptions& options = FetchOptions{});

}  // namespace go2cpp::bento::app
