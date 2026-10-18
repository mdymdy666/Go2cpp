/**
 * @file reader.hpp
 * @brief `bufio::Reader` —— 给任意 Reader 加一层缓冲区（对应 Go 的 `bufio.Reader`）。
 *
 * @details **它解决的问题**：底层 Reader 每次 `Read` 都要一次系统调用
 *   （或一次网络往返）。逐行/逐字节解析时，N 次小读 = N 次昂贵调用。
 *   `bufio::Reader` 把「填充缓冲」与「交付调用方」解耦：一次底层读最多
 *   撑满 `size` 字节，之后 `ReadByte`/`Peek`/`ReadLine` 都只动内存。
 *
 *   ## 底层 Reader 的契约（模板参数 `ReaderT`）
 *
 *   `ReaderT` 必须提供：
 *   @code
 *   streams::ReadResult Read(char* buffer, std::size_t size);
 *   @endcode
 *   其中 `streams::ReadResult` 含 `count`（实际读到的字节数）与
 *   `error`（`ErrorPtr`，可为空）。这与项目 `streams::Reader` 接口、
 *   `bufio::Reader` 自身、以及测试里的桩类型都兼容，因此可以任意嵌套。
 *   **允许 `count > 0` 同时带错误**（Go 的 `io.Reader` 契约），错误会被
 *   记住并在缓冲耗尽后交付。
 *
 *   ## 与 Go 对齐的语义
 *
 *   - `Read(p)`：**最多触发一次底层读**（缓冲为空时）。`n` 可能小于
 *     `p.size()`——这正是 Go `bufio.Reader.Read` 的契约；要「读满」请循环
 *     或自行用 `Peek`/`Buffered` 组合。缓冲为空且 `p.size() >= Size()`
 *     时直接读进 `p`，省掉一次内存拷贝（Go 的快路径）。
 *   - `Peek(n)`：**零拷贝**返回指向内部缓冲的 `ByteSpan`，不消耗数据；
 *     `n > Size()` → `ErrBufferFull`（**这里不预读**，见下）。
 *   - `ReadLine()`：去掉行尾 `\n`，同时去掉 `\r`（CRLF）；末行无换行符
 *     时正常返回该行。**行长超过缓冲区容量 → `ErrTooLong`**。
 *   - `ReadString(delim)`：包含分隔符；可跨多次填充无限累积（Go 同）。
 *   - EOF 是**粘性**的：一旦底层返回 EOF，后续操作直接返回 EOF，
 *     可通过 `Err()` 查看。
 *
 *   ## 有意保留的差异（都写了理由，不是遗漏）
 *
 *   1. `Peek(n > Size())` 直接报错、**不预读**。Go 会先把缓冲填满再报
 *      `ErrBufferFull` 并返回已有数据；本实现返回纯错误。原因是
 *      `Result<ByteSpan>` 的失败态默认不带值——需要「部分数据 + 错误」的
 *      调用方本来就是少数，而"先把源读掉一截再报错"更难排查。
 *   2. `ReadLine()` 超长行返回 `ErrTooLong`。Go 的 `ReadLine` 通过
 *      `isPrefix=true` 把长行拆成多段返回（只有 `Scanner` 才用
 *      `ErrTooLong`）。本项目的接口签名 `Result<std::string>` 没有
 *      `isPrefix` 位置，按任务要求统一成错误；**需要处理任意长行请用
 *      `ReadString('\n')`**（它是无界的）。
 *   3. 连续 100 次「0 字节且无错误」的底层读 → `streams::IoErrNoProgress`，
 *      与项目 `streams` 模块的防挂死策略一致（Go 的 `bufio` 会无限循环）。
 */
#pragma once

#include "go2cpp/bufio/byte_span.hpp"
#include "go2cpp/bufio/errors.hpp"
#include "go2cpp/bufio/limits.hpp"
#include "go2cpp/bufio/result.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace go2cpp::bufio {

/**
 * @brief 带缓冲的 Reader 装饰器。
 * @tparam ReaderT 底层 Reader 类型（需满足上面文件注释里的 `Read` 契约）。
 *
 * @warning `Reader` 只**引用**底层对象，不拥有它。底层 Reader 的生命周期
 *          必须覆盖本对象。
 */
template <typename ReaderT>
class Reader {
public:
    /**
     * @brief 用默认 4096 字节缓冲包装底层 Reader。
     * @param source 底层 Reader，按引用保存。
     */
    explicit Reader(ReaderT& source) : Reader(source, kDefaultBufferSize) {}

    /**
     * @brief 包装底层 Reader 并指定缓冲容量。
     * @param source 底层 Reader，按引用保存。
     * @param size 缓冲容量；小于 `kMinBufferSize` 时提升到 16（Go 同）。
     */
    explicit Reader(ReaderT& source, std::size_t size)
        : m_source(&source), m_buffer(detail::ClampBufferSize(size)) {}

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /**
     * @brief 读入 `p`（至多一次底层读）。
     * @param p 目标缓冲。
     * @return 实际写入 `p` 的字节数；`0` 表示「当前没有数据」，
     *         用 `Err()` 区分正常 EOF 与错误。
     */
    std::size_t Read(ByteSpan p) {
        if (p.empty()) {
            return 0;
        }
        if (m_r == m_w) {
            if (m_err) {
                return 0;
            }
            // Go 的快路径：缓冲区空且目标足够大时直接读进目标，少一次拷贝。
            if (p.size() >= m_buffer.size()) {
                const streams::ReadResult result =
                    m_source->Read(reinterpret_cast<char*>(p.data()), p.size());
                if (result.count > 0) {
                    m_empty_reads = 0;
                    if (result.error) {
                        m_err = result.error;
                    }
                    return result.count;
                }
                NoteIdleRead(result.error);
                return 0;
            }
            fill();
            if (m_r == m_w) {
                return 0;
            }
        }
        const std::size_t take = std::min(p.size(), m_w - m_r);
        std::memcpy(p.data(), m_buffer.data() + m_r, take);
        m_r += take;
        return take;
    }

    /**
     * @brief `bufio::Reader` 也满足底层 Reader 契约，可与 `Scanner` 等组合。
     * @details 与 `Read(ByteSpan)` 的区别：按 Go 的 `(n, err)` 语义交付错误——
     *          有数据时先返回数据（错误留到下一次调用）。
     */
    streams::ReadResult Read(char* buffer, std::size_t size) {
        const std::size_t count = Read(AsBytes(buffer, size));
        if (count > 0) {
            return streams::ReadResult{count, {}};
        }
        return streams::ReadResult{0, m_err ? m_err : streams::IoEof()};
    }

    /**
     * @brief 读一个字节。
     * @return 成功时 `Value()` 为字节值；EOF/错误时 `Err()` 非空。
     */
    Result<std::uint8_t> ReadByte() {
        while (m_r == m_w && !m_err) {
            fill();
        }
        if (m_r == m_w) {
            return Result<std::uint8_t>(m_err ? m_err : streams::IoEof());
        }
        return Result<std::uint8_t>(m_buffer[m_r++]);
    }

    /**
     * @brief 读一行（不含 `\n`，并去掉行尾 `\r`）。
     * @return 成功时为整行；末行无换行符时返回该行；
     *         行长超过 `Size()` 时为 `ErrTooLong`（可能同时带已读到的部分内容）。
     * @warning 错误返回时，缓冲区中**已消费的部分不会回滚**；超长行之后
     *          该 Reader 的语义是「从溢出点继续」，调用方通常应放弃它。
     * @note 末行**没有** `\n` 时，行尾的 `\r` 会保留——这是 Go `ReadLine`
     *       的原始行为（只有 `\r\n` 成对时才丢 `\r`）。`Scanner` 的
     *       `ScanLines` 则会在 EOF 时也丢掉 `\r`（同样对齐 Go）。
     */
    Result<std::string> ReadLine() {
        std::string line;
        const std::size_t max_length = m_buffer.size();
        while (true) {
            if (m_r < m_w) {
                const std::uint8_t* const start = m_buffer.data() + m_r;
                const std::size_t available = m_w - m_r;
                const void* const found = std::memchr(start, '\n', available);
                if (found != nullptr) {
                    const std::size_t take =
                        static_cast<std::size_t>(
                            static_cast<const std::uint8_t*>(found) - start) +
                        1;
                    const std::size_t consumed = take - 1;  // 本段不含 '\n' 的字节数
                    // 行尾的 '\r' 会被丢掉，不占「行长」预算；否则恰好等于
                    // 缓冲容量的 CRLF 行会被误判为超长。
                    std::size_t projected = line.size() + consumed;
                    const std::uint8_t trailing =
                        consumed > 0 ? start[consumed - 1]
                                     : (line.empty()
                                            ? std::uint8_t{0}
                                            : static_cast<std::uint8_t>(line.back()));
                    if (trailing == '\r') {
                        --projected;
                    }
                    if (projected > max_length) {
                        return Result<std::string>(std::move(line), ErrTooLong());
                    }
                    line.append(reinterpret_cast<const char*>(start), consumed);
                    m_r += take;
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    return Result<std::string>(std::move(line));
                }
                // 没有换行符：整段并入行缓冲。先判上限，避免「先吃后报」。
                // 末字节可能是即将被丢掉的 '\r'，先按「可能丢」估算，宁可少算 1
                // 也不要把合法的 CRLF 长行误判成超长（真正的上限仍由上面的
                // projected 精确检查兜底）。
                std::size_t projected = line.size() + available;
                if (available > 0 && start[available - 1] == '\r') {
                    --projected;
                }
                if (projected > max_length) {
                    return Result<std::string>(std::move(line), ErrTooLong());
                }
                line.append(reinterpret_cast<const char*>(start), available);
                m_r = m_w;
            }
            if (m_err) {
                // 与 Go 一致：末行没有换行符时照常交付，下次调用才是 EOF。
                if (!line.empty()) {
                    return Result<std::string>(std::move(line));
                }
                return Result<std::string>(m_err);
            }
            fill();
        }
    }

    /**
     * @brief 读到分隔符 `delim` 为止，**结果包含分隔符**（对应 Go 的 `ReadString`）。
     * @param delim 分隔字节（`char` 的位模式按 `uint8_t` 比较）。
     * @return 成功时为 `[起点, delim]`；EOF 前无分隔符时返回已读内容 + EOF；
     *         完全没有数据且已 EOF 时为纯 EOF 错误。
     * @note 长度不受缓冲容量限制（与 Go 一致）：内容会跨多次填充累积。
     */
    Result<std::string> ReadString(char delim) {
        const std::uint8_t target = static_cast<std::uint8_t>(delim);
        std::string text;
        while (true) {
            if (m_r < m_w) {
                const std::uint8_t* const start = m_buffer.data() + m_r;
                const std::size_t available = m_w - m_r;
                const void* const found = std::memchr(start, target, available);
                if (found != nullptr) {
                    const std::size_t take =
                        static_cast<std::size_t>(
                            static_cast<const std::uint8_t*>(found) - start) +
                        1;
                    text.append(reinterpret_cast<const char*>(start), take);
                    m_r += take;
                    return Result<std::string>(std::move(text));
                }
                text.append(reinterpret_cast<const char*>(start), available);
                m_r = m_w;
            }
            if (m_err) {
                return Result<std::string>(std::move(text), m_err);
            }
            fill();
        }
    }

    /**
     * @brief 查看缓冲区接下来的 `n` 字节，**零拷贝、不消费**。
     * @param n 期望看到的字节数。
     * @return 指向内部缓冲的 `ByteSpan`（长度 `n`）；`n > Size()` 时为
     *         `ErrBufferFull`；数据不足且已 EOF 时为「已有部分 + EOF」。
     * @warning 返回的 span 在下一次 `Read`/`Peek`/`ReadLine`/`Discard`
     *          之后失效。不要改写它指向的内容。
     */
    Result<ByteSpan> Peek(std::size_t n) {
        if (n > m_buffer.size()) {
            return Result<ByteSpan>(ErrBufferFull());
        }
        while (m_w - m_r < n && !m_err) {
            fill();
        }
        const std::size_t available = m_w - m_r;
        if (available >= n) {
            return Result<ByteSpan>(ByteSpan(m_buffer.data() + m_r, n));
        }
        // 数据不足：必定已经 EOF 或出错，否则循环不会退出。
        return Result<ByteSpan>(ByteSpan(m_buffer.data() + m_r, available),
                                m_err ? m_err : ErrBufferFull());
    }

    /**
     * @brief 丢弃 `n` 字节。
     * @return 实际丢弃的字节数；小于 `n` 说明中途遇到 EOF/错误（见 `Err()`）。
     */
    std::size_t Discard(std::size_t n) {
        std::size_t discarded = 0;
        while (discarded < n) {
            if (m_r < m_w) {
                const std::size_t take = std::min(n - discarded, m_w - m_r);
                m_r += take;
                discarded += take;
                continue;
            }
            if (m_err) {
                break;
            }
            fill();
        }
        return discarded;
    }

    /// 缓冲区里还剩多少字节可读（不触发底层读）。
    std::size_t Buffered() const noexcept { return m_w - m_r; }

    /// 缓冲区容量（构造时确定）。
    std::size_t Size() const noexcept { return m_buffer.size(); }

    /// 粘性错误：正常读到末尾时为 `streams::IoEof()`；一切正常时为空。
    const ErrorPtr& Err() const noexcept { return m_err; }

private:
    /// 记录「无进展」的底层读结果；连续太多次就判定底层违约。
    void NoteIdleRead(const ErrorPtr& error) {
        if (error) {
            m_err = error;
            return;
        }
        if (++m_empty_reads >= kMaxConsecutiveEmptyReads) {
            m_err = streams::IoErrNoProgress();
        }
    }

    /**
     * @brief 向底层要一次数据，尽量把缓冲填满。
     * @details 先把已消费的前缀压掉（`memmove`），再读到剩余空间。只在
     *          `m_err` 为空、缓冲未满时才会真正调用底层 `Read`。
     */
    void fill() {
        if (m_err) {
            return;
        }
        if (m_r > 0) {
            const std::size_t buffered = m_w - m_r;
            if (buffered > 0) {
                std::memmove(m_buffer.data(), m_buffer.data() + m_r, buffered);
            }
            m_w = buffered;
            m_r = 0;
        }
        const std::size_t room = m_buffer.size() - m_w;
        if (room == 0) {
            return;
        }
        const streams::ReadResult result = m_source->Read(
            reinterpret_cast<char*>(m_buffer.data() + m_w), room);
        if (result.count > 0) {
            m_w += result.count;
            m_empty_reads = 0;
            // 数据先留在缓冲里；错误记住，等缓冲耗尽再交付（Go 的 defer 语义）。
            if (result.error) {
                m_err = result.error;
            }
            return;
        }
        NoteIdleRead(result.error);
    }

    ReaderT* m_source;
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_r{0};          ///< 读游标（已交付给调用方的位置）
    std::size_t m_w{0};          ///< 写游标（已从底层读到的位置）
    std::size_t m_empty_reads{0};  ///< 连续「0 字节无错误」次数
    ErrorPtr m_err;              ///< 粘性错误（含 EOF）
};

}  // namespace go2cpp::bufio
