/**
 * @file scanner.hpp
 * @brief `bufio::Scanner` —— 按分隔符切分输入流（对应 Go 的 `bufio.Scanner`）。
 *
 * @details **切分模型与 Go 完全一致**：`Scanner` 维护一块**自己的**
 *   缓冲区，反复把「尚未切分的窗口」交给 `SplitFunc`：
 *
 *   ```
 *   SplitFunc(data, atEOF) -> { advance, token, error }
 *   ```
 *
 *   - `advance`：从 `data` 头部消费多少字节（可以消费掉分隔符）；
 *   - `token`：当前切出的 token，**必须指向 `data` 内部**（零拷贝）；
 *   - `error`：非空即终止扫描，`Err()` 可读。
 *
 *   若 `advance == 0 && token 为空`，表示「数据还不够，继续读」；在
 *   `atEOF == true` 时这表示「没有更多 token」。
 *
 *   **为什么 token 是指针而不是拷贝**：Go 的 `Scanner.Bytes()` 直接返回
 *   缓冲切片；`Text()` 才拷贝。本实现保持同样分工，便于大文件逐行处理
 *   不产生每行一次分配。
 *
 *   ## 与 Go 对齐的语义
 *   - 默认按行（`ScanLines`），去掉行尾 `\n` 与 `\r`；
 *   - 缓冲从 `buffer_size`（默认 4096）开始，token 放不下时**翻倍增长**，
 *     上限 `max_token_size`（默认 64 KiB，Go 的 `MaxScanTokenSize`）；
 *     超限 → `ErrTooLong`；
 *   - 正常读到流末尾时 `Scan()` 返回 false，且 `Err()` 为**空**
 *     （Go 的 `Scanner.Err` 只在真实错误时非空）；
 *   - `Bytes()` 返回的视图在下一次 `Scan()` 后失效；`Text()` 返回拷贝。
 *
 *   ## 与 Go 的差异
 *   1. `ScanWords` 只按 **ASCII 空白**（空格、`\t`、`\n`、`\r`、`\v`、`\f`）
 *      切分。Go 用 `unicode.IsSpace` 走 UTF-8 解码；本项目尚未提供 rune
 *      解码依赖，按 ASCII 实现并在测试里固定行为。
 *   2. 自定义 `SplitFunc` 的 `advance` 越界、或 `token` 落到窗口外时，
 *      报 `ErrInvalidAdvance` 而**不是 panic**——C++ 侧不能让用户回调
 *      把进程带走。
 */
#pragma once

#include "go2cpp/bufio/byte_span.hpp"
#include "go2cpp/bufio/errors.hpp"
#include "go2cpp/bufio/limits.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::bufio {

/**
 * @brief `SplitFunc` 的返回值。
 * @details 与 Go 的 `(advance int, token []byte, err error)` 一一对应。
 */
struct SplitResult {
    /// 从窗口头部消费的字节数；0 表示「需要更多数据」。
    std::size_t advance{0};
    /**
     * @brief 切出的 token；**必须**指向传入窗口内部。
     * @details 用 `data != nullptr` 判定「本轮有没有 token」——这样
     *          「空 token」（例如连续两个 `\n` 之间的空行）才是可表达的；
     *          `advance == 0` 的空 token 会被当成「数据还不够」。
     *          默认构造的 `ByteSpan`（`data == nullptr`）表示没有 token。
     */
    ByteSpan token{};
    /// 非空即终止扫描。
    ErrorPtr error;
};

/// 切分函数类型（Go 的 `bufio.SplitFunc`）。
using SplitFunc = std::function<SplitResult(ByteSpan data, bool at_eof)>;

/// 按行切分：去掉行尾 `\n`，若存在 `\r` 也去掉（Go 的 `ScanLines`）。
SplitResult ScanLines(ByteSpan data, bool at_eof);

/// 按 ASCII 空白切分，跳过前导空白（Go 的 `ScanWords` 的 ASCII 版）。
SplitResult ScanWords(ByteSpan data, bool at_eof);

/// 每个字节一个 token（Go 的 `ScanBytes`）。
SplitResult ScanBytes(ByteSpan data, bool at_eof);

/**
 * @brief 把底层 Reader 切成 token 的扫描器。
 * @tparam ReaderT 底层 Reader（契约同 `bufio::Reader`：`Read(char*, size_t)`）。
 *
 * @warning 只引用底层 Reader，不拥有它。
 */
template <typename ReaderT>
class Scanner {
public:
    /**
     * @brief 用默认缓冲（4096）与默认上限（64 KiB）构造扫描器。
     * @param source 底层 Reader。
     */
    explicit Scanner(ReaderT& source) : Scanner(source, kDefaultBufferSize) {}

    /**
     * @brief 指定初始缓冲尺寸构造扫描器。
     * @param source 底层 Reader。
     * @param buffer_size 初始缓冲；小于 16 提升到 16。
     * @param max_token_size 单 token 上限；小于初始缓冲时提升到初始缓冲。
     */
    explicit Scanner(ReaderT& source, std::size_t buffer_size,
                     std::size_t max_token_size = kMaxScanTokenSize)
        : m_source(&source),
          m_buffer(detail::ClampBufferSize(buffer_size)),
          m_initial_buffer_size(detail::ClampBufferSize(buffer_size)),
          m_max_token_size(std::max(detail::ClampBufferSize(buffer_size),
                                    max_token_size)),
          m_split(ScanLines) {}

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    /**
     * @brief 推进到下一个 token。
     * @return 切到 token 返回 true；流结束或有错误返回 false。
     * @note 返回 false 后：`Err()` 为空表示正常结束，否则是终止原因。
     */
    bool Scan() {
        if (m_done) {
            return false;
        }
        m_token = ByteSpan();
        while (true) {
            if (m_end > m_start || m_err) {
                const ByteSpan window(m_buffer.data() + m_start, m_end - m_start);
                const SplitResult result = m_split(window, m_err != nullptr);
                if (result.error) {
                    SetErr(result.error);
                    Shutdown();
                    return false;
                }
                if (result.advance > window.size()) {
                    SetErr(ErrInvalidAdvance());
                    Shutdown();
                    return false;
                }
                // 判定「有 token」用指针而不是长度：空行是合法 token
                // （Go 里 token != nil 与 len(token) == 0 是两个概念）。
                if (result.token.data() != nullptr) {
                    const std::uint8_t* const window_begin = window.data();
                    const std::uint8_t* const window_end = window_begin + window.size();
                    if (result.token.data() < window_begin ||
                        result.token.data() + result.token.size() > window_end) {
                        SetErr(ErrInvalidAdvance());
                        Shutdown();
                        return false;
                    }
                    // advance 必须覆盖 token，否则同一个 token 会被无限重复交付。
                    const std::size_t token_offset =
                        static_cast<std::size_t>(result.token.data() - window_begin);
                    if (result.advance < token_offset + result.token.size()) {
                        SetErr(ErrInvalidAdvance());
                        Shutdown();
                        return false;
                    }
                    // 空 token 且 advance == 0 无法推进，按「需要更多数据」处理，
                    // 否则 Scan 会永远返回同一个空 token。
                    if (!(result.token.empty() && result.advance == 0)) {
                        // token 指向缓冲区内部：直接把视图交给调用方（零拷贝）。
                        m_token = result.token;
                        m_start += result.advance;
                        return true;
                    }
                }
                if (result.advance > 0) {
                    m_start += result.advance;
                    continue;
                }
            }
            if (m_err) {
                // 已经到流末尾且窗口里再也切不出 token：正常结束。
                Shutdown();
                if (go2cpp::Is(m_err, streams::IoEof())) {
                    m_err = {};
                }
                return false;
            }
            if (!ReadMore()) {
                // 缓冲已到上限仍切不出 token：ErrTooLong（或空闲读超限）。
                Shutdown();
                return false;
            }
        }
    }

    /// 当前 token 的拷贝（Go 的 `Scanner.Text`）。
    std::string Text() const {
        if (m_token.empty()) {
            return std::string{};
        }
        return std::string(reinterpret_cast<const char*>(m_token.data()),
                           m_token.size());
    }

    /// 当前 token 的零拷贝视图（Go 的 `Scanner.Bytes`）；下次 `Scan` 后失效。
    ByteSpan Bytes() const noexcept { return m_token; }

    /// 终止原因；正常结束为空。
    const ErrorPtr& Err() const noexcept { return m_err; }

    /// 替换切分函数（默认 `ScanLines`）。
    void SetSplit(SplitFunc split) { m_split = std::move(split); }

    /// 当前切分函数。
    const SplitFunc& Split() const noexcept { return m_split; }

    /// 初始缓冲尺寸（不含后续增长）。
    std::size_t BufferSize() const noexcept {
        return m_initial_buffer_size;
    }

private:
    /// 记录第一个错误（EOF 可以被真实错误覆盖，与 Go 的 `setErr` 一致）。
    void SetErr(const ErrorPtr& error) {
        if (!m_err || go2cpp::Is(m_err, streams::IoEof())) {
            m_err = error;
        }
    }

    /// 终止后清空窗口：保留尺寸，丢弃内容。
    void Shutdown() {
        m_start = 0;
        m_end = 0;
        m_done = true;
    }

    /**
     * @brief 压缩窗口并读入更多数据；必要时扩容。
     * @return false 表示「无法再前进」——`ErrTooLong` 已置入 `Err()`。
     */
    bool ReadMore() {
        if (m_start > 0) {
            const std::size_t remaining = m_end - m_start;
            if (remaining > 0) {
                std::memmove(m_buffer.data(), m_buffer.data() + m_start, remaining);
            }
            m_start = 0;
            m_end = remaining;
        }
        if (m_end == m_buffer.size()) {
            if (m_buffer.size() >= m_max_token_size) {
                SetErr(ErrTooLong());
                return false;
            }
            const std::size_t doubled = m_buffer.size() * 2;
            const std::size_t next =
                doubled < m_max_token_size ? doubled : m_max_token_size;
            if (next <= m_buffer.size()) {
                SetErr(ErrTooLong());
                return false;
            }
            m_buffer.resize(next);
        }
        const std::size_t room = m_buffer.size() - m_end;
        if (room == 0) {
            SetErr(ErrTooLong());
            return false;
        }
        const streams::ReadResult result = m_source->Read(
            reinterpret_cast<char*>(m_buffer.data() + m_end), room);
        if (result.count > 0) {
            m_end += std::min(result.count, room);
            m_empty_reads = 0;
            // 数据与错误同时返回时，错误先记住；本轮循环会以 atEOF=true 再切一次。
            if (result.error) {
                m_err = result.error;
            }
            return true;
        }
        if (result.error) {
            m_err = result.error;
            return true;
        }
        if (++m_empty_reads >= kMaxConsecutiveEmptyReads) {
            m_err = streams::IoErrNoProgress();
        }
        return true;
    }

    ReaderT* m_source;
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_initial_buffer_size{0};
    std::size_t m_max_token_size{kMaxScanTokenSize};
    std::size_t m_start{0};  ///< 窗口起点（尚未切分的位置）
    std::size_t m_end{0};    ///< 窗口终点（缓冲区已读到的位置）
    std::size_t m_empty_reads{0};
    bool m_done{false};
    ByteSpan m_token{};
    ErrorPtr m_err;
    SplitFunc m_split;
};

}  // namespace go2cpp::bufio
