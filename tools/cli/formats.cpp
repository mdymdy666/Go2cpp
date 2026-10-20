/**
 * @file formats.cpp
 * @brief `formats.hpp` 的实现。
 *
 * @details 探测顺序刻意把**二进制魔数**放在最前：`G2CRPLY\0` 与 `G2CMIG02`
 *          都包含 NUL，先做文本判断会把它们当成「无法识别的乱码」。之后的
 *          顺序是 trace 头 → JSON → ini，其中 `[scheduler]` 与 JSON 数组的
 *          歧义由 `IsIniSectionLine` 解决（详见该函数注释）。
 */

#include "formats.hpp"

#include "cli.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace go2cpp::cli {
namespace {

constexpr std::size_t kHeadBytes = 4096;
constexpr std::size_t kIniScanLines = 32;
constexpr std::size_t kProbeHeadBytes = kHeadBytes;

const std::string_view kMigrationMagicView(kMigrationMagic, sizeof(kMigrationMagic));
const std::string_view kReplayMagicView(kReplayMagic, sizeof(kReplayMagic));

/// 去掉 UTF-8 BOM；`head` 原地修改，避免复制。
void StripUtf8Bom(std::string* text) {
    if (text->size() >= 3 && static_cast<unsigned char>((*text)[0]) == 0xEF &&
        static_cast<unsigned char>((*text)[1]) == 0xBB &&
        static_cast<unsigned char>((*text)[2]) == 0xBF) {
        text->erase(0, 3);
    }
}

/// 取第一行（不含换行符），并去掉首尾空白。
std::string FirstLine(const std::string& text) {
    const auto newline = text.find('\n');
    return Trim(text.substr(0, newline == std::string::npos ? text.size() : newline));
}

/**
 * @brief 判断整行是否像 ini 的节名（`[scheduler]`）。
 * @details 关键是把它和 JSON 数组区分开：`[1,2,3]`、`["a"]`、`[{"x":1}]`
 *          都可能是合法 JSON 的首行，只有「方括号内是字母/下划线开头的简单
 *          标识符」才按 ini 处理。否则 `[scheduler]` 会被误判成 JSON 数组，
 *          随后报出令人费解的解析错误。
 */
bool IsIniSectionLine(const std::string& line) {
    if (line.size() < 3 || line.front() != '[' || line.back() != ']') {
        return false;
    }
    const std::string inner = line.substr(1, line.size() - 2);
    const auto first = static_cast<unsigned char>(inner.front());
    if (!(std::isalpha(first) != 0 || first == '_')) {
        return false;
    }
    for (char character : inner) {
        if (character == '[' || character == ']' || character == '{' || character == '}' ||
            character == '"' || character == ',' || character == ':') {
            return false;
        }
    }
    return true;
}

/// 宽松的 ini 判定：出现形如 `[name]` 的整行即认为像 ini。
bool LooksLikeIni(const std::string& text) {
    std::size_t line_begin = 0;
    std::size_t line_index = 0;
    while (line_begin < text.size() && line_index < kIniScanLines) {
        const auto newline = text.find('\n', line_begin);
        const auto line_end = newline == std::string::npos ? text.size() : newline;
        const std::string line = Trim(std::string_view(text).substr(line_begin, line_end - line_begin));
        if (IsIniSectionLine(line)) {
            return true;
        }
        ++line_index;
        if (newline == std::string::npos) {
            break;
        }
        line_begin = newline + 1;
    }
    return false;
}

[[noreturn]] void Fail(ExitCode code, const std::string& message) {
    throw CliError(code, message);
}

std::uint16_t ReadLeU16(const std::string& head, std::size_t offset) {
    if (head.size() < offset + 2) {
        return 0;
    }
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(head[offset]) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(head[offset + 1])) << 8));
}

std::uint32_t ReadLeU32(const std::string& head, std::size_t offset) {
    if (head.size() < offset + 4) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(head[offset + index]))
                 << (8 * index);
    }
    return value;
}

}  // namespace

const char* FileFormatName(FileFormat format) {
    switch (format) {
        case FileFormat::kJson:
            return "json";
        case FileFormat::kTraceText:
            return "trace-text";
        case FileFormat::kMigrationBinary:
            return "migration-binary";
        case FileFormat::kReplayBinary:
            return "replay-binary";
        case FileFormat::kIniText:
            return "ini";
        case FileFormat::kUnknown:
            break;
    }
    return "unknown";
}

std::uint16_t ReplayFormatVersion(const std::string& head) {
    // FileHeader: magic[0,8) + version u16@8 + header_size u16@10 + ...
    if (!StartsWith(head, kReplayMagicView)) {
        return 0;
    }
    return ReadLeU16(head, 8);
}

std::uint32_t MigrationFormatVersion(const std::string& head) {
    // Header: magic[0,8) + version u32@8 + header_size u32@12 + ...
    if (!StartsWith(head, kMigrationMagicView)) {
        return 0;
    }
    return ReadLeU32(head, 8);
}

FileProbe ProbeFile(const std::string& path, std::uint64_t max_bytes) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        Fail(ExitCode::kFailure, "无法读取文件 '" + path + "'：" + std::strerror(errno));
    }
    if (!S_ISREG(info.st_mode)) {
        Fail(ExitCode::kFailure, "不是普通文件：'" + path + "'（目录、管道与设备不被接受）");
    }
    if (info.st_size == 0) {
        Fail(ExitCode::kFormat, "文件为空（0 字节）：'" + path + "'，无法判断格式");
    }
    if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
        Fail(ExitCode::kFailure, "文件过大：'" + path + "' 为 " + FormatBytes(static_cast<std::uint64_t>(info.st_size)) +
                                     "，上限 " + FormatBytes(max_bytes));
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        Fail(ExitCode::kFailure, "无法打开文件 '" + path + "'：" + std::strerror(errno));
    }
    FileProbe probe;
    probe.path = path;
    probe.size = static_cast<std::uint64_t>(info.st_size);
    probe.head.resize(kProbeHeadBytes);
    input.read(&probe.head[0], static_cast<std::streamsize>(probe.head.size()));
    probe.head.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad()) {
        Fail(ExitCode::kFailure, "读取文件 '" + path + "' 失败");
    }
    StripUtf8Bom(&probe.head);

    // 二进制魔数优先：两个格式的魔数都含 NUL 或非文本字节。
    if (StartsWith(probe.head, kMigrationMagicView)) {
        probe.format = FileFormat::kMigrationBinary;
        probe.detail = "迁移快照（G2CMIG02，版本 " +
                       std::to_string(MigrationFormatVersion(probe.head)) + "）";
        return probe;
    }
    if (StartsWith(probe.head, kReplayMagicView)) {
        probe.format = FileFormat::kReplayBinary;
        probe.detail = "重放记录（G2CRPLY，版本 " +
                       std::to_string(ReplayFormatVersion(probe.head)) + "）";
        return probe;
    }

    const std::string first_line = FirstLine(probe.head);
    if (StartsWith(first_line, "# go2cpp-trace")) {
        probe.format = FileFormat::kTraceText;
        probe.detail = "go2cpp-trace 文本记录";
    } else if (StartsWith(first_line, "# go2cpp-replay")) {
        probe.format = FileFormat::kTraceText;
        probe.detail = "go2cpp-replay 文本记录";
    } else if (!first_line.empty() && first_line.front() == '{') {
        probe.format = FileFormat::kJson;
        probe.detail = "JSON 文本（首字符 '{'）";
    } else if (!first_line.empty() && first_line.front() == '[' && !IsIniSectionLine(first_line)) {
        probe.format = FileFormat::kJson;
        probe.detail = "JSON 文本（首字符 '['）";
    } else if (LooksLikeIni(probe.head)) {
        probe.format = FileFormat::kIniText;
        probe.detail = "ini 文本（含 [section] 行）";
    } else {
        probe.format = FileFormat::kUnknown;
        probe.detail = "无法识别的文件头：" + HexPreview(probe.head, 16);
    }
    return probe;
}

std::string ReadTextFile(const std::string& path, std::uint64_t max_bytes,
                         std::uint64_t* size_out) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        Fail(ExitCode::kFailure, "无法读取文件 '" + path + "'：" + std::strerror(errno));
    }
    if (!S_ISREG(info.st_mode)) {
        Fail(ExitCode::kFailure, "不是普通文件：'" + path + "'");
    }
    if (static_cast<std::uint64_t>(info.st_size) > max_bytes) {
        Fail(ExitCode::kFailure, "文件过大：'" + path + "' 为 " + FormatBytes(static_cast<std::uint64_t>(info.st_size)) +
                                     "，上限 " + FormatBytes(max_bytes));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        Fail(ExitCode::kFailure, "无法打开文件 '" + path + "'：" + std::strerror(errno));
    }
    std::string content;
    content.resize(static_cast<std::size_t>(info.st_size));
    if (!content.empty()) {
        input.read(&content[0], static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(input.gcount()));
    }
    if (input.bad()) {
        Fail(ExitCode::kFailure, "读取文件 '" + path + "' 失败");
    }
    if (size_out != nullptr) {
        *size_out = static_cast<std::uint64_t>(content.size());
    }
    return content;
}

std::string ReadFilePrefix(const std::string& path, std::size_t bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        Fail(ExitCode::kFailure, "无法打开文件 '" + path + "'：" + std::strerror(errno));
    }
    std::string prefix;
    prefix.resize(bytes);
    input.read(&prefix[0], static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<std::size_t>(input.gcount()));
    if (prefix.size() < bytes) {
        Fail(ExitCode::kFailure, "文件短于预期：'" + path + "' 只有 " +
                                     std::to_string(prefix.size()) + " 字节，需要 " +
                                     std::to_string(bytes));
    }
    return prefix;
}

void WriteTextFileAtomic(const std::string& path, std::string_view data) {
    const std::filesystem::path target(path);
    std::filesystem::path directory = target.parent_path();
    if (directory.empty()) {
        directory = ".";
    }
    std::error_code ignored;
    if (!std::filesystem::is_directory(directory, ignored)) {
        Fail(ExitCode::kFailure, "输出目录不存在：'" + directory.string() + "'");
    }

    const std::string temporary =
        (directory / (target.filename().string() + ".tmp." + std::to_string(::getpid()))).string();
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        Fail(ExitCode::kFailure, "无法创建临时文件 '" + temporary + "'：" + std::strerror(errno));
    }

    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t chunk = ::write(fd, data.data() + written, data.size() - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::close(fd);
            ::unlink(temporary.c_str());
            Fail(ExitCode::kFailure, "写入临时文件 '" + temporary + "' 失败：" + std::strerror(saved));
        }
        written += static_cast<std::size_t>(chunk);
    }
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        ::unlink(temporary.c_str());
        Fail(ExitCode::kFailure, "fsync 失败：'" + temporary + "'：" + std::strerror(saved));
    }
    if (::close(fd) != 0) {
        const int saved = errno;
        ::unlink(temporary.c_str());
        Fail(ExitCode::kFailure, "关闭临时文件 '" + temporary + "' 失败：" + std::strerror(saved));
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        ::unlink(temporary.c_str());
        Fail(ExitCode::kFailure, "重命名 '" + temporary + "' → '" + path + "' 失败：" + std::strerror(saved));
    }
}

}  // namespace go2cpp::cli
