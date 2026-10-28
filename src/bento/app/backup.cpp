/**
 * @file backup.cpp
 * @brief MOD-31：备份打包与 HTTP 上传实现。
 * @details
 *
 * ## 为什么 Backup 先写临时包
 *
 * 备份通常覆盖上一次的包。如果直接截断目标文件，写到一半失败就会同时
 * 丢掉「旧包」和「新包」。这里先写 `<out>.part.<pid>.<seq>`（唯一名，
 * 同一输出路径的并发备份不会互相踩），`Close` 成功后才 rename；POSIX
 * rename 是原子的，失败路径只会留下半个临时文件（立即删除）。
 *
 * ## 为什么上传用 curl 而不是手写 socket
 *
 * 生产 endpoint 绝大多数是 HTTPS。手写 socket 需要额外引入 TLS 库，与
 * 「store-only zip 零依赖」的取舍冲突；curl 是部署环境的事实标准，且
 * `os::proc` 用 argv 直传，不经过 shell。请求配置走 stdin（`--config -`），
 * token 不会出现在 `ps` 输出里。
 *
 * ## 失败语义
 *
 * Backup 失败：旧包保持不变，临时文件删除。
 * Upload 失败：本地 zip 原样保留，调用方可稍后重试或人工处理。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/app/backup.hpp"

#include "go2cpp/bento/app/detail/retry.hpp"

#include "go2cpp/os/proc.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp::bento::app {
namespace {

namespace fs = std::filesystem;

constexpr int kMaxUploadRetries = 16;              ///< 重试次数硬上限
constexpr std::size_t kMaxUrlBytes = 8192;         ///< endpoint 长度上限
constexpr std::size_t kMaxHeaderBytes = 8192;      ///< 单个 header 长度上限
constexpr std::size_t kMaxErrorBytes = 512;        ///< 错误信息保留长度
constexpr std::chrono::seconds kMaxRetryDelay{30}; ///< 单次重试等待上限

/// 临时包序号；只在同一进程内保证唯一（与 fetch/file_browser 的做法一致）。
std::atomic<std::uint64_t> g_part_seq{0};

/// 去掉首尾空白。
std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

/// 大小写不敏感比较。
bool EqualsIgnoreCase(std::string_view first, std::string_view second) {
    if (first.size() != second.size()) {
        return false;
    }
    for (std::size_t i = 0; i < first.size(); ++i) {
        const auto a = static_cast<unsigned char>(first[i]);
        const auto b = static_cast<unsigned char>(second[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

/// 判断是否以 prefix 开头（大小写不敏感）。
bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

/// 是否含 CR/LF/NUL 或其它控制字符。
bool HasControl(std::string_view text) {
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20u || byte == 0x7fu) {
            return true;
        }
    }
    return false;
}

/// 取输入路径的 basename（`/a/b/` → `b`，`.` → 当前目录名）。
std::string BaseNameOf(const std::string& input) {
    std::string trimmed = input;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    const fs::path path(trimmed);
    std::string name = path.filename().string();
    if (name.empty() || name == "." || name == "..") {
        std::error_code ec;
        const fs::path absolute = fs::absolute(path, ec);
        if (!ec) {
            name = absolute.filename().string();
        }
    }
    return name;
}

/// zip 状态映射到 backup 状态。
BackupStatus MapZipStatus(ZipStatus status) {
    switch (status) {
        case ZipStatus::kOk:
            return BackupStatus::kOk;
        case ZipStatus::kInputNotFound:
            return BackupStatus::kInputNotFound;
        case ZipStatus::kInvalidArgument:
        case ZipStatus::kPathRejected:
            return BackupStatus::kInvalidArgument;
        case ZipStatus::kTooManyEntries:
        case ZipStatus::kEntryTooLarge:
        case ZipStatus::kArchiveTooLarge:
            return BackupStatus::kTooLarge;
        default:
            return BackupStatus::kZipError;
    }
}

/// 删除文件（不存在不算错误）。
void RemoveFile(const std::string& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    (void)fs::remove(path, ec);
}

// ---------------------------------------------------------------------------
// Upload 辅助
// ---------------------------------------------------------------------------

/// HTTP header 名字允许的 token 字符（RFC 7230）。
bool IsTokenChar(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) != 0) {
        return true;
    }
    switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

/// 校验 `"Name: value"`。
bool ValidateHeader(const std::string& header, std::string* error) {
    if (header.empty() || header.size() > kMaxHeaderBytes) {
        *error = "header 为空或过长";
        return false;
    }
    const std::size_t colon = header.find(':');
    if (colon == std::string::npos || colon == 0) {
        *error = "header 缺少冒号";
        return false;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        if (!IsTokenChar(header[i])) {
            *error = "header 名含非法字符";
            return false;
        }
    }
    const std::string value = header.substr(colon + 1);
    if (HasControl(value)) {
        *error = "header 值含控制字符";
        return false;
    }
    return true;
}

/// 是否已有指定 header（大小写不敏感）。
bool HasHeader(const std::vector<std::string>& headers, std::string_view name) {
    for (const std::string& header : headers) {
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (EqualsIgnoreCase(std::string_view(header).substr(0, colon), name)) {
            return true;
        }
    }
    return false;
}

/// 转义 curl 配置文件中的双引号字符串。
bool EscapeConfigValue(std::string_view value, std::string* out) {
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte == 0 || byte == '\r' || byte == '\n') {
            return false;
        }
    }
    out->clear();
    out->reserve(value.size() + 2);
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            out->push_back('\\');
        }
        out->push_back(ch);
    }
    return true;
}

/// 把毫秒转成 curl 需要的整数秒（至少 1s）。
long long SecondsCeil(std::chrono::milliseconds value) {
    const long long ms = static_cast<long long>(value.count());
    if (ms <= 1000) {
        return 1;
    }
    return (ms + 999) / 1000;
}

/// 构造 curl `--config -` 的内容。
bool BuildCurlConfig(const std::string& zip_path, const UploadRequest& request,
                     const UploadOptions& options, std::string* config,
                     std::string* error) {
    std::string url;
    std::string path;
    if (!EscapeConfigValue(request.endpoint, &url) || !EscapeConfigValue(zip_path, &path)) {
        *error = "endpoint/zip 路径含换行或 NUL";
        return false;
    }
    std::string text;
    text += "url = \"" + url + "\"\n";
    text += "request = \"";
    text += (request.method == HttpMethod::kPut ? "PUT" : "POST");
    text += "\"\n";
    text += "upload-file = \"" + path + "\"\n";
    text += "silent\n";
    text += "show-error\n";
    text += "globoff\n";
    text += "output = \"/dev/null\"\n";
    text += "write-out = \"%{http_code}\"\n";
    text += "connect-timeout = \"" + std::to_string(SecondsCeil(options.connect_timeout)) + "\"\n";
    text += "max-time = \"" + std::to_string(SecondsCeil(options.request_timeout)) + "\"\n";
    for (const std::string& header : request.headers) {
        std::string escaped;
        if (!EscapeConfigValue(header, &escaped)) {
            *error = "header 含换行或 NUL";
            return false;
        }
        text += "header = \"" + escaped + "\"\n";
    }
    if (!request.token.empty() && !HasHeader(request.headers, "Authorization")) {
        std::string escaped;
        if (!EscapeConfigValue("Authorization: Bearer " + request.token, &escaped)) {
            *error = "token 含换行或 NUL";
            return false;
        }
        text += "header = \"" + escaped + "\"\n";
    }
    if (!request.content_type.empty() && !HasHeader(request.headers, "Content-Type")) {
        std::string escaped;
        if (!EscapeConfigValue("Content-Type: " + request.content_type, &escaped)) {
            *error = "content_type 含换行或 NUL";
            return false;
        }
        text += "header = \"" + escaped + "\"\n";
    }
    *config = std::move(text);
    return true;
}

/// 解析 `--write-out "%{http_code}"` 的输出。
int ParseStatusCode(std::string_view text) {
    const std::string trimmed = Trim(text);
    if (trimmed.size() != 3) {
        return 0;
    }
    int value = 0;
    for (const char ch : trimmed) {
        if (ch < '0' || ch > '9') {
            return 0;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

/// 单次 curl 尝试的结果。
struct Attempt final {
    bool ok{false};           ///< 2xx。
    bool curl_missing{false}; ///< exec 失败（PATH 中无 curl）。
    bool local_error{false};  ///< 进程/管道创建失败，不重试。
    bool timed_out{false};    ///< 超时。
    int http_status{0};       ///< HTTP 状态码（0 表示没有响应）。
    std::string error;        ///< 诊断信息。
};

/// 截断错误信息，避免把整个 curl 输出带进结果。
std::string ShortError(std::string_view text) {
    std::string value = Trim(text);
    if (value.size() > kMaxErrorBytes) {
        value.resize(kMaxErrorBytes);
        value += "...";
    }
    return value;
}

/// 执行一次上传。
Attempt PerformCurl(const std::string& zip_path, const UploadRequest& request,
                    const UploadOptions& options) {
    Attempt attempt;
    std::error_code ec;
    const fs::path absolute = fs::absolute(zip_path, ec);
    if (ec) {
        attempt.local_error = true;
        attempt.error = "无法解析 zip 绝对路径";
        return attempt;
    }
    std::string config;
    if (!BuildCurlConfig(absolute.string(), request, options, &config, &attempt.error)) {
        attempt.local_error = true;
        return attempt;
    }
    go2cpp::os::ProcessOptions process;
    process.argv = {"curl", "--config", "-"};
    process.use_path_search = true;
    process.timeout = options.request_timeout + std::chrono::seconds(5);
    process.grace = std::chrono::seconds(1);
    process.max_output_bytes = 64 * 1024;
    const go2cpp::os::Result<go2cpp::os::RunResult> run =
        go2cpp::os::Run(process, config);
    if (!run) {
        if (run.error().code == go2cpp::os::ProcErrorCode::kExecFailed) {
            attempt.curl_missing = true;
            attempt.error = "找不到 curl";
        } else {
            attempt.local_error = true;
            attempt.error = "无法启动 curl";
        }
        return attempt;
    }
    const go2cpp::os::RunResult& result = *run;
    attempt.http_status = ParseStatusCode(result.standard_output);
    if (result.status.timed_out) {
        attempt.timed_out = true;
        attempt.error = "上传超时";
        return attempt;
    }
    if (!result.status.exited || result.status.exit_code != 0) {
        attempt.error = result.standard_error.empty()
                            ? "curl 退出码 " + std::to_string(result.status.exit_code)
                            : ShortError(result.standard_error);
        return attempt;
    }
    if (attempt.http_status >= 200 && attempt.http_status <= 299) {
        attempt.ok = true;
        return attempt;
    }
    attempt.error = "HTTP " + std::to_string(attempt.http_status);
    return attempt;
}

/// 校验上传参数。
bool ValidateUploadRequest(const std::string& zip_path, const UploadRequest& request,
                           const UploadOptions& options, std::string* error) {
    if (zip_path.empty()) {
        *error = "zip 路径为空";
        return false;
    }
    if (HasControl(zip_path)) {
        *error = "zip 路径含控制字符";
        return false;
    }
    if (request.endpoint.empty() || request.endpoint.size() > kMaxUrlBytes ||
        HasControl(request.endpoint) || request.endpoint.find('"') != std::string::npos ||
        request.endpoint.find('\\') != std::string::npos) {
        *error = "endpoint 非法";
        return false;
    }
    for (const char ch : request.endpoint) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            *error = "endpoint 含空白字符";
            return false;
        }
    }
    if (!StartsWithIgnoreCase(request.endpoint, "http://") &&
        !StartsWithIgnoreCase(request.endpoint, "https://")) {
        *error = "endpoint 必须是 http(s)://";
        return false;
    }
    if (options.max_retries < 0 || options.max_retries > kMaxUploadRetries) {
        *error = "max_retries 越界";
        return false;
    }
    if (options.retry_delay.count() < 0) {
        *error = "retry_delay 不能为负";
        return false;
    }
    if (options.connect_timeout.count() <= 0 ||
        options.connect_timeout > std::chrono::minutes(10)) {
        *error = "connect_timeout 越界";
        return false;
    }
    if (options.request_timeout.count() <= 0 ||
        options.request_timeout > std::chrono::hours(1)) {
        *error = "request_timeout 越界";
        return false;
    }
    for (const std::string& header : request.headers) {
        if (!ValidateHeader(header, error)) {
            return false;
        }
    }
    if (HasControl(request.token)) {
        *error = "token 含控制字符";
        return false;
    }
    if (HasControl(request.content_type)) {
        *error = "content_type 含控制字符";
        return false;
    }
    return true;
}


} // namespace

// ---------------------------------------------------------------------------
// Backup
// ---------------------------------------------------------------------------

const char* BackupStatusToString(BackupStatus status) noexcept {
    switch (status) {
        case BackupStatus::kOk:
            return "ok";
        case BackupStatus::kInvalidArgument:
            return "invalid_argument";
        case BackupStatus::kInputNotFound:
            return "input_not_found";
        case BackupStatus::kIoError:
            return "io_error";
        case BackupStatus::kTooLarge:
            return "too_large";
        case BackupStatus::kZipError:
            return "zip_error";
    }
    return "unknown";
}

BackupResult Backup(const std::vector<std::string>& paths, const std::string& out_zip,
                    const ZipLimits& limits) {
    BackupResult result;
    result.archive_path = out_zip;
    if (paths.empty() || out_zip.empty()) {
        result.status = BackupStatus::kInvalidArgument;
        result.error = "paths/out_zip 为空";
        return result;
    }
    std::error_code ec;
    if (fs::is_directory(out_zip, ec)) {
        result.status = BackupStatus::kInvalidArgument;
        result.error = "输出路径是目录";
        return result;
    }
    const std::string output_absolute = [&out_zip]() {
        std::error_code absolute_ec;
        const fs::path absolute = fs::absolute(out_zip, absolute_ec);
        return absolute_ec ? out_zip : absolute.lexically_normal().string();
    }();
    for (const std::string& input : paths) {
        std::error_code absolute_ec;
        const fs::path absolute = fs::absolute(input, absolute_ec);
        if (!absolute_ec && absolute.lexically_normal().string() == output_absolute) {
            result.status = BackupStatus::kInvalidArgument;
            result.error = "输入路径与输出 zip 相同";
            return result;
        }
    }
    // 唯一临时名：同一 out_zip 的并发 Backup 各自写自己的临时包，不会
    // 把对方写到一半的内容 rename 成正式包（D-06）。
    const std::uint64_t part_sequence = g_part_seq.fetch_add(1, std::memory_order_relaxed);
    const std::string temp_path =
        out_zip + ".part." + std::to_string(static_cast<long long>(::getpid())) + "." +
        std::to_string(static_cast<unsigned long long>(part_sequence));
    ZipWriter writer;
    ZipStatus zip_status = writer.Open(temp_path, limits);
    if (zip_status != ZipStatus::kOk) {
        result.status = MapZipStatus(zip_status);
        result.error = ZipStatusToString(zip_status);
        RemoveFile(temp_path);
        return result;
    }
    // 输出包自身与临时文件即使位于输入目录内也不能被打进归档。
    (void)writer.AddExclusion(out_zip);
    (void)writer.AddExclusion(temp_path);
    std::unordered_set<std::string> root_names;
    for (const std::string& input : paths) {
        if (input.empty()) {
            zip_status = ZipStatus::kInvalidArgument;
            result.error = "输入路径为空";
            break;
        }
        std::error_code status_ec;
        const fs::file_status status = fs::symlink_status(input, status_ec);
        if (status_ec || status.type() == fs::file_type::not_found) {
            zip_status = ZipStatus::kInputNotFound;
            result.error = "输入路径不存在: " + input;
            break;
        }
        if (status.type() == fs::file_type::symlink) {
            ++result.stats.symlinks_skipped;
            continue;
        }
        const std::string base = BaseNameOf(input);
        if (base.empty()) {
            zip_status = ZipStatus::kInvalidArgument;
            result.error = "无法从输入路径推导条目名: " + input;
            break;
        }
        if (status.type() == fs::file_type::directory) {
            if (!root_names.insert(base + "/").second) {
                zip_status = ZipStatus::kInvalidArgument;
                result.error = "顶层目录重名: " + base;
                break;
            }
            zip_status = writer.AddDirectory(input, base);
        } else if (status.type() == fs::file_type::regular) {
            if (!root_names.insert(base).second) {
                zip_status = ZipStatus::kInvalidArgument;
                result.error = "顶层文件重名: " + base;
                break;
            }
            zip_status = writer.AddFile(input, base);
        } else {
            ++result.stats.special_skipped;
            continue;
        }
        if (zip_status != ZipStatus::kOk) {
            result.error = ZipStatusToString(zip_status);
            break;
        }
    }
    if (zip_status != ZipStatus::kOk) {
        writer.Abort();
        RemoveFile(temp_path);
        result.status = MapZipStatus(zip_status);
        return result;
    }
    zip_status = writer.Close();
    if (zip_status != ZipStatus::kOk) {
        writer.Abort();
        RemoveFile(temp_path);
        result.status = MapZipStatus(zip_status);
        result.error = ZipStatusToString(zip_status);
        return result;
    }
    const ZipStats stats = writer.stats();
    fs::rename(temp_path, out_zip, ec);
    if (ec) {
        // POSIX rename 可以原子覆盖；走到这里说明无法替换（跨设备/目标被
        // 占用）。此时**不删除旧包**，只清理临时文件并报错，保证失败不会
        // 把上一份可用备份一起丢掉。
        RemoveFile(temp_path);
        result.status = BackupStatus::kIoError;
        result.error = "rename 失败: " + ec.message();
        return result;
    }
    result.stats.files = stats.files;
    result.stats.directories = stats.directories;
    result.stats.symlinks_skipped += stats.symlinks_skipped;
    result.stats.special_skipped += stats.special_skipped;
    result.stats.input_bytes = stats.input_bytes;
    result.stats.archive_bytes = stats.archive_bytes;
    result.status = BackupStatus::kOk;
    result.archive_path = out_zip;
    return result;
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

const char* HttpMethodToString(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::kPut:
            return "PUT";
        case HttpMethod::kPost:
            return "POST";
    }
    return "PUT";
}

const char* UploadStatusToString(UploadStatus status) noexcept {
    switch (status) {
        case UploadStatus::kOk:
            return "ok";
        case UploadStatus::kInvalidArgument:
            return "invalid_argument";
        case UploadStatus::kZipMissing:
            return "zip_missing";
        case UploadStatus::kCurlMissing:
            return "curl_missing";
        case UploadStatus::kNetworkError:
            return "network_error";
        case UploadStatus::kHttpRejected:
            return "http_rejected";
        case UploadStatus::kTimeout:
            return "timeout";
        case UploadStatus::kLocalIoError:
            return "local_io_error";
    }
    return "unknown";
}

UploadResult Upload(const std::string& zip_path, const UploadRequest& request,
                    const UploadOptions& options) {
    UploadResult result;
    std::string error;
    if (!ValidateUploadRequest(zip_path, request, options, &error)) {
        result.status = UploadStatus::kInvalidArgument;
        result.error = std::move(error);
        return result;
    }
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(zip_path, ec);
    if (ec || status.type() != fs::file_type::regular) {
        result.status = UploadStatus::kZipMissing;
        result.error = "本地 zip 不存在或不是普通文件";
        return result;
    }
    const int total_attempts = options.max_retries + 1;
    Attempt last;
    for (int attempt = 0; attempt < total_attempts; ++attempt) {
        if (attempt > 0) {
            detail::SleepBeforeRetry(
                options.retry_delay, attempt,
                std::chrono::duration_cast<std::chrono::milliseconds>(kMaxRetryDelay));
        }
        last = PerformCurl(zip_path, request, options);
        ++result.attempts;
        result.http_status = last.http_status;
        result.error = last.error;
        if (last.ok) {
            result.status = UploadStatus::kOk;
            result.error.clear();
            return result;
        }
        if (last.curl_missing) {
            result.status = UploadStatus::kCurlMissing;
            return result;
        }
        if (last.local_error) {
            result.status = UploadStatus::kLocalIoError;
            return result;
        }
    }
    if (last.timed_out) {
        result.status = UploadStatus::kTimeout;
    } else if (last.http_status > 0) {
        result.status = UploadStatus::kHttpRejected;
    } else {
        result.status = UploadStatus::kNetworkError;
    }
    return result;
}

} // namespace go2cpp::bento::app
