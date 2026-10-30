/**
 * @file src/config/config_codec.cpp
 * @brief 配置文本编解码的非模板部分：旧配置兼容词表、浮点往返与状态码诊断。
 *
 * ## 模块定位
 * `include/go2cpp/config/config_codec.hpp` 的实现补充。模板部分（结构体/数组/
 * 标量分派）在头文件里完成；这里只放不依赖配置类型的辅助函数，避免每个
 * 包含 config 头文件的翻译单元都重复一份字符串处理逻辑。
 *
 * ## 为什么保留旧格式兼容
 * `go2cpp.ini`、示例配置与运维手写文件里存在 `1/0/yes/no/on/off`、`.5`
 * 这类写法；JSON 只接受 `true/false` 与 `0.5`。兼容逻辑集中在本文件，新格式
 * 的核心解析仍由 Reflect 的 JSON 解析器负责，两者不会互相污染。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/config/config_codec.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace go2cpp::config {
namespace detail {
namespace {

/// @brief 去掉首尾空白（含 `\r`），与 `IniFile` 的 Trim 语义一致。
std::string_view TrimView(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/// @brief 转小写；只用于 ASCII 词表比较。
std::string ToLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value >= static_cast<unsigned char>('A') &&
                       value <= static_cast<unsigned char>('Z')
                   ? static_cast<char>(value - 'A' + 'a')
                   : character;
    });
    return out;
}

}  // namespace

std::string DescribeCodecStatus(reflect::CodecStatus status) {
    switch (status) {
        case reflect::CodecStatus::kOk:
            return "成功";
        case reflect::CodecStatus::kInvalidArgument:
            return "参数非法";
        case reflect::CodecStatus::kTruncated:
            return "消息被截断";
        case reflect::CodecStatus::kBadMagic:
            return "不是 Reflect 消息";
        case reflect::CodecStatus::kBadVersion:
            return "消息版本不支持";
        case reflect::CodecStatus::kUnknownType:
            return "未知类型";
        case reflect::CodecStatus::kTypeMismatch:
            return "类型不匹配";
        case reflect::CodecStatus::kUnknownField:
            return "未知字段";
        case reflect::CodecStatus::kBadPayload:
            return "载荷非法";
        case reflect::CodecStatus::kBadText:
            return "文本格式非法";
        case reflect::CodecStatus::kUnsupportedEncoding:
            return "该类型不支持文本编码";
        case reflect::CodecStatus::kCompressionError:
            return "压缩流损坏";
        case reflect::CodecStatus::kTooLarge:
            return "超过大小上限";
        case reflect::CodecStatus::kFrameError:
            return "帧层错误";
    }
    return "未知状态";
}

bool DecodeLegacyBool(std::string_view text, bool* value) {
    if (value == nullptr) {
        return false;
    }
    const std::string clean = ToLower(TrimView(text));
    if (clean == "1" || clean == "true" || clean == "yes" || clean == "on") {
        *value = true;
        return true;
    }
    if (clean == "0" || clean == "false" || clean == "no" || clean == "off") {
        *value = false;
        return true;
    }
    return false;
}

bool DecodeLegacyDouble(std::string_view text, double* value) {
    if (value == nullptr) {
        return false;
    }
    const std::string_view clean = TrimView(text);
    if (clean.empty() || clean.front() == '+') {
        return false;
    }
    double parsed = 0.0;
    const auto result =
        std::from_chars(clean.data(), clean.data() + clean.size(), parsed);
    if (result.ec != std::errc() || result.ptr != clean.data() + clean.size()) {
        return false;
    }
    *value = parsed;
    return true;
}

bool EncodeDouble(double value, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    if (!std::isfinite(value)) {
        // JSON 没有 NaN/Inf 字面量；写回旧配置能识别的文本，保证 ToString
        // 仍然可读且 DumpIni 不会生成半个 JSON。
        if (std::isnan(value)) {
            *out = "nan";
        } else if (value > 0.0) {
            *out = "inf";
        } else {
            *out = "-inf";
        }
        return true;
    }
    char buffer[64] = {};
    // 不带格式的 to_chars 浮点重载输出「最短且可往返」的表示，避免
    // `std::chars_format::general` 默认 6 位有效数字导致的精度丢失。
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc()) {
        return false;
    }
    out->assign(buffer, result.ptr);
    return true;
}

}  // namespace detail
}  // namespace go2cpp::config
