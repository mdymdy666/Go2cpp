/**
 * @file path.cpp
 * @brief `go2cpp::path`（Go `path` 包对应实现）。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause（移植自 Go 标准库，详见 path.hpp 的来源说明）
 */

/**
 * ## 模块定位（补充）
 *
 * 本文件是 `go2cpp::path` 纯词法 API 的实现层：被 `Filepath`（Linux）转发，
 * 被 `tests/path/test_path.cpp` 直接验证。它不链接 `os`/`config`/`log`，也不
 * 创建线程，因此任何模块都可以安全地在静态初始化期调用。
 *
 * ## 本文件定义什么
 *
 * - `LazyBuffer`：Go `lazybuf` 的无分配/惰性物化版本，只服务 `Clean`；
 * - `Clean`：单遍扫描 + `..` 回溯，是 `Join`/`Dir` 的共同底座；
 * - `Split`/`Ext`/`Base`/`Dir`/`IsAbs`/`JoinElements`/`Join`：薄封装；
 * - `MatchChecked`/`Match`：转发到 `match_impl.hpp` 的 chunk/star 扫描。
 *
 * ## 阅读建议
 *
 * 先看 `LazyBuffer` 的三个操作（`Append`/`Rewind`/`Index`），再看 `Clean` 的
 * 主循环 —— `dotdot` 是理解「前导 `..` 为什么保留」的关键不变量；最后看
 * `JoinElements` 为何在 `size == 0` 时直接返回空串。
 *
 * ## 边界与陷阱
 *
 * - 全部按字节处理，不校验 UTF-8；`MatchChecked` 才按 rune 计 `?`；
 * - `Clean` 的回退只到 `dotdot`，越过它会错误吞掉前导 `..`；
 * - `JoinElements` 先保留中间空元素产生的 `/`，统一交给 `Clean` 收敛。
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
/**
 * ## 定位
 * `Clean` 的专用输出缓冲：输入已规范化时**零分配**，只有第一个不同的字节出现
 * 才把源串整体拷入 `m_buffer`。`Join` 的每一步都会调用 `Clean`，因此这是热路径。
 *
 * ## 不变量
 * - `m_buffer` 为空时，`[0, m_width)` 与 `m_source` 的前缀逐字节相同；
 * - `m_width` 永远不超过源串长度（`Rewind` 只有在可回退时才被调用）。
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
    std::string_view m_source;  ///< 源路径视图；只读，生命周期由 Clean 的调用方保证（本类不拥有）
    std::string m_buffer;  ///< 惰性物化缓冲区；仅当 Append 遇到与源串不同的字节时才分配
    std::size_t m_width{0};  ///< 已输出的有效前缀长度；Append/Rewind 修改，Str 按它截取结果
};

}  // namespace

/**
 * @brief 用 Go 的单遍 lazybuf + `..` 回溯算法，返回与输入等价的最短路径名。
 *
 * ## 步骤
 * step1. 空串直接返回 `"."`；记录 `rooted`，初始化读指针 `r` 与回溯下界 `dotdot`；
 * step2. 连续 `/` 与 `.` 元素不产生输出，只前进 `r`；
 * step3. 遇到 `..`：能回退就退到上一个 `/`；不能回退且非根时把 `..` 写回并推进 `dotdot`；
 * step4. 普通元素：按需补 `/`，整段拷贝到输出；
 * step5. 输出为空返回 `"."`，否则返回惰性缓冲的前缀。
 *
 * ## 不变量/边界
 * `dotdot` 之前的 `..` 是「不可消解的前导 ..」；回退永远不能越过它，否则
 * `Clean("a/../../b")` 会错误得到 `"b"` 而不是 `"../b"`。
 */
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

    // step2: 跳过连续 `/` 与 `.` 元素（它们不产生输出）
    while (r < n) {
        if (path[r] == '/') {
            // 空元素
            ++r;
        } else if (path[r] == '.' && (r + 1 == n || path[r + 1] == '/')) {
            // `.` 元素
            ++r;
        // step3: `..` 回溯；dotdot 是不可越过的下界
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
        // step4: 真实元素整段拷贝，元素之间补 `/`
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

    // step5: 空结果归一为 `.`，与 Go / POSIX 习惯一致
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
