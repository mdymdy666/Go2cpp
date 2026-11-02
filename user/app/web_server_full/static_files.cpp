/**
 * @file static_files.cpp
 * @brief 静态文件读取与路径安全校验的实现。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include "static_files.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "middleware.h"

namespace app {
namespace {

/// @brief 十六进制字符转数值；非法返回 -1。
int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/// @brief 解码 %XX；遇到非法转义返回空串（调用方按 400/403 处理）。
std::string PercentDecode(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '%' && index + 2 < input.size()) {
            const int high = HexValue(input[index + 1]);
            const int low = HexValue(input[index + 2]);
            if (high < 0 || low < 0) {
                return {};
            }
            output.push_back(static_cast<char>((high << 4) | low));
            index += 2;
            continue;
        }
        output.push_back(input[index]);
    }
    return output;
}

/// @brief 检查任一段是否为 `..`；解码后再查，避免 `%2e%2e` 绕过。
bool ContainsParentSegment(const std::string& path) {
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string_view segment(path.data() + begin,
                                       (end == std::string::npos ? path.size() : end) - begin);
        if (segment == "..") {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return false;
}

}  // namespace

std::string ContentTypeFor(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return "application/octet-stream";
    }
    std::string extension(path.substr(dot));
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (extension == ".html" || extension == ".htm") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js") return "application/javascript; charset=utf-8";
    if (extension == ".json") return "application/json; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool TryServeStatic(const go2cpp::http::Request& request, const StaticOptions& options,
                    go2cpp::http::Response* response) {
    if (response == nullptr || options.root.empty()) {
        return false;
    }
    if (request.method != go2cpp::http::Method::Get &&
        request.method != go2cpp::http::Method::Head) {
        return false;
    }

    const std::string decoded = PercentDecode(request.path());
    if (decoded.empty()) {
        *response = MakeJsonError(go2cpp::http::Status::BadRequest, "非法 URL 编码");
        return true;
    }
    if (ContainsParentSegment(decoded)) {
        *response = MakeJsonError(go2cpp::http::Status::Forbidden, "禁止访问根目录之外");
        return true;
    }

    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.root, error);
    if (error) {
        return false;
    }
    std::filesystem::path target = root;
    std::string relative = decoded.front() == '/' ? decoded.substr(1) : decoded;
    if (relative.empty()) {
        relative = options.index;
    }
    target = std::filesystem::weakly_canonical(root / relative, error);
    if (error) {
        return false;
    }

    if (std::filesystem::is_directory(target, error)) {
        target = std::filesystem::weakly_canonical(target / options.index, error);
        if (error) {
            return false;
        }
    }
    const std::string root_text = root.string();
    const std::string target_text = target.string();
    if (target_text != root_text && target_text.rfind(root_text + '/', 0) != 0) {
        *response = MakeJsonError(go2cpp::http::Status::Forbidden, "禁止访问根目录之外");
        return true;
    }
    if (!std::filesystem::is_regular_file(target, error)) {
        return false;
    }

    const auto size = std::filesystem::file_size(target, error);
    if (error) {
        return false;
    }
    if (size > options.max_bytes) {
        *response = MakeJsonError(go2cpp::http::Status::PayloadTooLarge, "静态文件超过上限");
        return true;
    }
    std::ifstream input(target, std::ios::binary);
    if (!input) {
        *response = MakeJsonError(go2cpp::http::Status::InternalError, "读取静态文件失败");
        return true;
    }
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    response->status = go2cpp::http::Status::OK;
    response->header.set("Content-Type", ContentTypeFor(target.string()));
    response->header.set("X-Content-Type-Options", "nosniff");
    response->body = std::move(body);
    return true;
}

}  // namespace app
