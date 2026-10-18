/**
 * @file path.cpp
 * @brief `go2cpp::path`（Go `path` 包对应实现）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（移植自 Go 标准库，详见 path.hpp 的来源说明）
 */

#include "go2cpp/path/path.hpp"

#include "match_impl.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace go2cpp::path {

namespace {

/**
 * @brief Go `path.lazybuf` 的 C++ 版本：输出与输入前缀相同时不分配内存。
 *
 * @details Clean 的绝大多数调用都不需要改动路径（已规范化的路径），
 *          逐个字符比较、遇到第一个不同才拷贝，可把「已规范路径」的
 *          Clean 降到零分配。这是从 Go 上游保留的设计意图，不是过早优化：
 *          `Join` 的每一步都会调用 `Clean`，已规范化输入是常见路径。
 */
class LazyBuffer final {
  public:
    explicit LazyBuffer(std::string_view source) : m_source(source) {}

    /// 读取已写入位置 i 的字节（可能在尚未物化的 buffer 里）。
    char Index(std::size_t i) const {
        return m_buffer.empty() ? m_source[i] : m_buffer[i];
    }

    /// 追加一个字节；与源串同位置字节相同时只前进写指针。
    void Append(char c) {
        if (m_buffer.empty()) {
            if (m_width < m_source.size() && m_source[m_width] == c) {
                ++m_width;
                return;
            }
            m_buffer.assign(m_source.begin(), m_source.end());
        }
        m_buffer[m_width] = c;
        ++m_width;
    }

    /// 回退一个字节（Clean 的 `..` 回溯用）。
    void Rewind() noexcept { --m_width; }

    std::size_t Width() const noexcept { return m_width; }

    std::string Str() const {
        if (m_buffer.empty()) {
            return std::string(m_source.substr(0, m_width));
        }
        return std::string(m_buffer.data(), m_width);
    }

  private:
    std::string_view m_source;
    std::string m_buffer;
    std::size_t m_width{0};
};

}  // namespace

std::string Clean(std::string_view path) {
    if (path.empty()) {
        return ".";
    }

    const bool rooted = path.front() == '/';
    const std::size_t n = path.size();

    // 不变量（沿用 Go 注释）：
    //   r 是下一个待读字节；w 是下一个待写位置；
    //   dotdot 是 buf 中 `..` 回溯必须停下的下标 —— 要么是前导 `/`，
    //   要么是前导 `../../..` 前缀的末尾。
    LazyBuffer out(path);
    std::size_t r = 0;
    std::size_t dotdot = 0;
    if (rooted) {
        out.Append('/');
        r = 1;
        dotdot = 1;
    }

    while (r < n) {
        if (path[r] == '/') {
            // 空元素
            ++r;
        } else if (path[r] == '.' && (r + 1 == n || path[r + 1] == '/')) {
            // `.` 元素
            ++r;
        } else if (path[r] == '.' && r + 1 < n && path[r + 1] == '.' &&
                   (r + 2 == n || path[r + 2] == '/')) {
            // `..` 元素：回退到上一个 `/`
            r += 2;
            if (out.Width() > dotdot) {
                // 可以回退
                out.Rewind();
                while (out.Width() > dotdot && out.Index(out.Width()) != '/') {
                    out.Rewind();
                }
            } else if (!rooted) {
                // 不能回退且不是根路径：保留这个 `..` 元素。
                if (out.Width() > 0) {
                    out.Append('/');
                }
                out.Append('.');
                out.Append('.');
                dotdot = out.Width();
            }
        } else {
            // 真实元素：需要时补 `/`
            if ((rooted && out.Width() != 1) || (!rooted && out.Width() != 0)) {
                out.Append('/');
            }
            while (r < n && path[r] != '/') {
                out.Append(path[r]);
                ++r;
            }
        }
    }

    if (out.Width() == 0) {
        return ".";
    }
    return out.Str();
}

std::pair<std::string, std::string> Split(std::string_view path) {
    const std::size_t i = path.rfind('/');
    if (i == std::string_view::npos) {
        return {std::string{}, std::string(path)};
    }
    return {std::string(path.substr(0, i + 1)), std::string(path.substr(i + 1))};
}

std::string Ext(std::string_view path) {
    for (std::size_t i = path.size(); i > 0; --i) {
        const char c = path[i - 1];
        if (c == '/') {
            break;
        }
        if (c == '.') {
            return std::string(path.substr(i - 1));
        }
    }
    return {};
}

std::string Base(std::string_view path) {
    if (path.empty()) {
        return ".";
    }
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    const std::size_t i = path.rfind('/');
    if (i != std::string_view::npos) {
        path.remove_prefix(i + 1);
    }
    if (path.empty()) {
        return "/";
    }
    return std::string(path);
}

std::string Dir(std::string_view path) {
    return Clean(Split(path).first);
}

bool IsAbs(std::string_view path) {
    return !path.empty() && path.front() == '/';
}

std::string JoinElements(std::initializer_list<std::string_view> elems) {
    std::size_t size = 0;
    for (const std::string_view e : elems) {
        size += e.size();
    }
    if (size == 0) {
        return {};
    }
    std::string buffer;
    buffer.reserve(size + elems.size() - 1);
    for (const std::string_view e : elems) {
        if (!buffer.empty() || !e.empty()) {
            if (!buffer.empty()) {
                buffer.push_back('/');
            }
            buffer.append(e.data(), e.size());
        }
    }
    return Clean(buffer);
}

std::string JoinElements(const std::vector<std::string>& elems) {
    std::size_t size = 0;
    for (const auto& e : elems) {
        size += e.size();
    }
    if (size == 0) {
        return {};
    }
    std::string buffer;
    buffer.reserve(size + (elems.empty() ? 0 : elems.size() - 1));
    for (const auto& e : elems) {
        if (!buffer.empty() || !e.empty()) {
            if (!buffer.empty()) {
                buffer.push_back('/');
            }
            buffer.append(e);
        }
    }
    return Clean(buffer);
}

std::string Join(const std::vector<std::string>& elems) {
    return JoinElements(elems);
}

MatchResult MatchChecked(std::string_view pattern, std::string_view name) {
    return detail::MatchWithSeparator(pattern, name, '/', true);
}

bool Match(std::string_view pattern, std::string_view name) {
    return MatchChecked(pattern, name).matched;
}

}  // namespace go2cpp::path
