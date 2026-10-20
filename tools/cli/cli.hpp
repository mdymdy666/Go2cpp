#pragma once

/**
 * @file cli.hpp
 * @brief go2cpp-cli 的公共类型：退出码、错误类型与纯格式化工具。
 *
 * @details 这里只放**跨子命令共享且无副作用**的东西：
 *   - `ExitCode`：CLI 对外的稳定退出码契约，脚本据此分支；
 *   - `CliError`：携带退出码的可捕获错误，`main` 统一打印并返回；
 *   - 字节数 / 纳秒 / 十六进制预览 / 字符串裁剪等纯函数。
 *
 * ## 为什么退出码要细分
 *
 * 「DebugServer 未启用」「文件格式不匹配」「目标进程返回错误」对使用者是三类
 * 完全不同的处理动作（去开调试服务 / 换文件 / 看远端日志）。如果全部退 1，
 * 脚本只能靠解析 stderr 文本，既脆弱又把中文文案变成隐式 API。因此固定为下表，
 * 并写入 `docs/cli.md`：
 *
 * | 值 | 含义 |
 * |---|---|
 * | 0 | 成功 |
 * | 1 | 一般失败（IO、子进程、协议内容异常） |
 * | 2 | 用法错误（参数缺失/未知/越界） |
 * | 3 | 后端不可用（DebugServer 未启用、迁移/重放后端缺失） |
 * | 4 | 文件格式不匹配（空文件、未知格式、JSON 语法错） |
 * | 5 | 目标进程明确返回失败（协议 `ok:false`） |
 *
 * ## 边界与陷阱
 *
 * - `FormatBytes` 以 1024 为进制，单位标签用 KiB/MiB；`0 B` 不带小数。
 * - `HexPreview` 只用于**错误诊断**，不解析内容；超过 `max_bytes` 的部分以
 *   `...` 结尾，避免把二进制整段喷到终端。
 * - 这些函数不得抛异常，也不得访问文件或环境变量。
 */

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace go2cpp::cli {

/// CLI 版本号；由 CMake 的 `GO2CPP_CLI_VERSION` 覆盖，未定义时回退到 0.1.0。
#ifndef GO2CPP_CLI_VERSION
#define GO2CPP_CLI_VERSION "0.1.0"
#endif

inline constexpr const char* kVersion = GO2CPP_CLI_VERSION;

/**
 * @brief CLI 退出码契约。
 *
 * @details 数值本身是契约的一部分（见文件头表格），不要随意调整既有取值；
 *          新增类别只能追加，不能复用已用过的数字。
 */
enum class ExitCode : int {
    kOk = 0,          ///< 成功。
    kFailure = 1,     ///< 一般失败：IO、子进程、协议内容异常。
    kUsage = 2,       ///< 用法错误：参数缺失/未知/越界。
    kUnavailable = 3, ///< 后端不可用：DebugServer 未启用、迁移/重放后端缺失。
    kFormat = 4,      ///< 文件格式不匹配：空文件、未知格式、JSON 语法错。
    kRemote = 5,      ///< 目标进程返回失败（协议 `ok:false`）。
};

/**
 * @brief 带退出码的 CLI 错误。
 *
 * @details 只在 CLI 内部逐层上抛；`main` 捕获后把 `what()` 写到 stderr，
 *          并以 `code()` 退出。**不要**用异常做正常控制流（例如帮助请求）。
 */
class CliError : public std::runtime_error {
public:
    CliError(ExitCode code, const std::string& message)
        : std::runtime_error(message), m_code(code) {}

    /// 该错误应映射到的进程退出码。
    ExitCode code() const noexcept { return m_code; }

private:
    ExitCode m_code;
};

/// 判断 `text` 是否以 `prefix` 开头。
inline bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// 去掉首尾的 ASCII 空白（空格、制表、CR、LF）；不做 Unicode 归一化。
inline std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    auto is_space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

/**
 * @brief 把字节数格式化成人类可读文本。
 * @details 以 1024 为进制；`0` 输出 `0 B`，其余整数字节输出 `N B`，
 *          到 KiB 及以上保留一位小数（例如 `12.3 MiB`）。
 */
inline std::string FormatBytes(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    char buffer[64];
    if (bytes < 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
        return buffer;
    }
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < 5) {
        value /= 1024.0;
        ++unit;
    }
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
    return buffer;
}

/**
 * @brief 把纳秒数格式化成人类可读文本。
 * @details `999 ns` / `1.5 us` / `12.3 ms` / `4.50 s`；调用方只会传无符号统计量。
 */
inline std::string FormatDurationNs(std::uint64_t ns) {
    char buffer[64];
    if (ns < 1000) {
        std::snprintf(buffer, sizeof(buffer), "%llu ns",
                      static_cast<unsigned long long>(ns));
    } else if (ns < 1000ULL * 1000ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f us", static_cast<double>(ns) / 1000.0);
    } else if (ns < 1000ULL * 1000ULL * 1000ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f ms",
                      static_cast<double>(ns) / (1000.0 * 1000.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f s",
                      static_cast<double>(ns) / (1000.0 * 1000.0 * 1000.0));
    }
    return buffer;
}

/**
 * @brief 生成字节串的十六进制预览，用于错误里展示「文件实际开头」。
 * @param raw 原始字节。
 * @param max_bytes 最多展示多少字节；超出部分以 `...` 结尾。
 * @return 形如 `23 20 67 6f 32 63 70 70 ...` 的字符串。
 */
inline std::string HexPreview(std::string_view raw, std::size_t max_bytes = 32) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    const std::size_t limit = raw.size() < max_bytes ? raw.size() : max_bytes;
    for (std::size_t index = 0; index < limit; ++index) {
        const auto byte = static_cast<unsigned char>(raw[index]);
        if (index != 0) {
            out.push_back(' ');
        }
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    if (raw.size() > limit) {
        out += " ...";
    }
    return out;
}

}  // namespace go2cpp::cli
