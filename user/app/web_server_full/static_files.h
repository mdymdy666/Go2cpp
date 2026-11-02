#pragma once

/**
 * @file static_files.h
 * @brief 静态文件服务：只读、限定在 web_root 内、防目录穿越。
 * @details 目录穿越是最常见的静态资源漏洞。这里先做百分号解码，再用
 *          `weakly_canonical` 解析 `..`，最后校验结果仍在 root 前缀内；
 *          任何一步失败都拒绝，而不是「尽力返回」。
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <string>
#include <string_view>

#include "go2cpp/network/http.hpp"

namespace app {

/// @brief 静态文件服务配置。
struct StaticOptions {
    std::string root;                          ///< 文档根目录（相对工作目录）。
    std::string index{"index.html"};           ///< 目录请求的默认文件。
    std::size_t max_bytes{8 * 1024 * 1024};    ///< 单文件上限，超过返回 413。
};

/// @brief 尝试用静态文件响应请求。
/// @return true 表示已生成响应（200/403/413/500）；false 表示文件不存在或方法不适用。
bool TryServeStatic(const go2cpp::http::Request& request, const StaticOptions& options,
                    go2cpp::http::Response* response);

/// @brief 按扩展名映射 Content-Type；未知类型回退 application/octet-stream。
std::string ContentTypeFor(std::string_view path);

}  // namespace app
