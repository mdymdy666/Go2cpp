/**
 * @file writer.hpp
 * @brief `bufio::Writer` —— 把 N 次小写合并成 1 次大写的缓冲写入器
 *        （对应 Go 的 `bufio.Writer`）。
 *
 * @details **底层 Writer 的契约（模板参数 `WriterT`）**：
 *   @code
 *   streams::WriteResult Write(const char* data, std::size_t size);
 *   @endcode
 *   `streams::WriteResult` 含 `count`（实际写入字节数）与 `error`。
 *   `count` 可以小于 `size`（部分写），本类会**循环写完**，因此底层
 *   「一次只吐几个字节」的短写实现也能正确工作——这是任务明确要求的
 *   「`Flush()` 处理部分写」。Go 的 `bufio.Writer` 则相反：它只写一次，
 *   短写直接报 `io.ErrShortWrite`，把循环责任推给调用方。这个差异是
 *   有意为之，已在测试里用 `max 3 bytes/call` 的桩验证。
 *
 *   ## 与 Go 对齐的语义
 *   - 缓冲满时**自动 flush**；
 *   - `Buffered()` / `Available()` 语义同 Go（`Available = Size() - Buffered()`）；
 *   - 写大于缓冲的整块时，缓冲为空则**直接写底层**，不经过拷贝（Go 快路径）；
 *   - 错误是**粘性**的：一旦底层失败，后续调用直接返回同一个错误。
 *
 *   ## 析构**不**自动 flush（重要）
 *
 *   与 Go 一致（Go 也没有「析构」概念，`bufio.Writer` 必须显式 `Flush`）。
 *   理由：
 *   1. 析构里无法把错误交还给调用方——写失败只能静默丢弃或 `terminate`，
 *      两者都不可接受；
 *   2. 异常/提前 `return` 路径上自动 flush 会把「未完成的写」暴露给对端，
 *      产生半条记录；
 *   3. 显式 `Flush()` 让「数据何时离开进程」在代码里可审计。
 *
 *   因此 `Writer` 的析构函数只做默认清理（不碰底层）。**忘记 `Flush()`
 *   等于丢数据**，这是有意保留的与 Go 相同的坑，测试 `ScenarioLogWrite`
 *   专门验证了「不 flush 就析构 → 数据确实没落地」。
 *
 * @warning `Writer` 只**引用**底层对象，不拥有它。
 */
#pragma once

#include "go2cpp/bufio/byte_span.hpp"
#include "go2cpp/bufio/limits.hpp"
#include "go2cpp/bufio/result.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace go2cpp::bufio {

/**
 * @brief 带缓冲的 Writer 装饰器。
 * @tparam WriterT 底层 Writer 类型（需满足上面文件注释里的 `Write` 契约）。
 */
template <typename WriterT>
class Writer {
public:
    /// 用默认 4096 字节缓冲包装底层 Writer。
    explicit Writer(WriterT& destination)
        : Writer(destination, kDefaultBufferSize) {}

    /**
     * @brief 包装底层 Writer 并指定缓冲容量。
     * @param destination 底层 Writer，按引用保存。
     * @param size 缓冲容量；小于 `kMinBufferSize` 时提升到 16（Go 同）。
     */
    explicit Writer(WriterT& destination, std::size_t size)
        : m_destination(&destination),
          m_buffer(detail::ClampBufferSize(size)) {}

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    /**
     * @brief 缓冲区在析构时**不**自动 flush。
     * @note 有意保持空实现；见文件头的三条理由。数据安全由调用方显式
     *       `Flush()` 负责。
     */
    ~Writer() = default;

    /**
     * @brief 写入 `p`。
     * @return 被本对象**接受**的字节数。成功时等于 `p.size()`；失败时是
     *         出错前已接受的字节数（见 `Err()`）。
     * @note 返回值不代表「已到达底层」——只有 `Flush()` 成功才保证这一点。
     */
    std::size_t Write(ByteSpan p) {
        if (m_err) {
            return 0;
        }
        std::size_t accepted = 0;
        while (p.size() > Available() && !m_err) {
            std::size_t step = 0;
            if (Buffered() == 0) {
                // 大块直接写；部分写就继续循环，0 字节无错按短写处理，
                // 否则会死循环（Go 依赖 Writer 契约，这里不冒这个风险）。
                const streams::WriteResult result =
                    m_destination->Write(reinterpret_cast<const char*>(p.data()), p.size());
                // 底层若谎报「写得比给的多」，按本次请求长度截断——不信任它。
                step = std::min(result.count, p.size());
                if (result.error) {
                    m_err = result.error;
                } else if (step == 0) {
                    m_err = streams::IoErrShortWrite();
                }
            } else {
                step = std::min(p.size(), Available());
                std::memcpy(m_buffer.data() + m_n, p.data(), step);
                m_n += step;
                Flush();  // 失败时 m_err 已置，循环条件会退出
            }
            accepted += step;
            p = p.subspan(step);
        }
        if (m_err) {
            return accepted;
        }
        const std::size_t step = std::min(p.size(), Available());
        if (step > 0) {
            std::memcpy(m_buffer.data() + m_n, p.data(), step);
            m_n += step;
        }
        return accepted + step;
    }

    /// 与 `streams::Writer` 契约互操作（供 `streams::WriteString` 等复用）。
    streams::WriteResult Write(const char* data, std::size_t size) {
        const std::size_t count = Write(AsBytes(const_cast<char*>(data), size));
        if (m_err) {
            return streams::WriteResult{count, m_err};
        }
        return streams::WriteResult{count, {}};
    }

    /// 写一个字节；缓冲满时先自动 flush。
    Result<void> WriteByte(std::uint8_t value) {
        if (m_err) {
            return Result<void>(m_err);
        }
        if (Available() == 0) {
            const Result<void> flushed = Flush();
            if (!flushed.ok()) {
                return flushed;
            }
        }
        m_buffer[m_n++] = value;
        return Result<void>();
    }

    /// 写一个字符串；语义与 `Write` 相同（不额外分配）。
    Result<void> WriteString(std::string_view text) {
        const std::size_t written =
            Write(ByteSpan(reinterpret_cast<std::uint8_t*>(const_cast<char*>(text.data())),
                           text.size()));
        if (m_err) {
            return Result<void>(m_err);
        }
        if (written != text.size()) {
            return Result<void>(streams::IoErrShortWrite());
        }
        return Result<void>();
    }

    /**
     * @brief 把缓冲区内容**全部**写到底层。
     * @details 循环处理部分写：每次成功写 `k` 字节就把剩余数据前移 `k`，
     *          直到缓冲清空。底层返回 0 字节且无错误时按短写报错（连续
     *          100 次仍未进展才报，给「暂时不可写」的实现留余地）。
     *          失败时**已写出的部分不回退**，未写出的部分仍留在缓冲里，
     *          错误保持粘性。
     */
    Result<void> Flush() {
        if (m_err) {
            return Result<void>(m_err);
        }
        std::size_t idle_writes = 0;
        while (m_n > 0) {
            const streams::WriteResult result = m_destination->Write(
                reinterpret_cast<const char*>(m_buffer.data()), m_n);
            // 只接受 [0, m_n] 范围内的计数：超出说明底层违反契约，按短写报错，
            // 否则 m_n 会下溢成天文数字并触发越界读。
            if (result.count > m_n) {
                m_err = streams::IoErrShortWrite();
                return Result<void>(m_err);
            }
            if (result.count > 0) {
                idle_writes = 0;
                if (result.count < m_n) {
                    std::memmove(m_buffer.data(), m_buffer.data() + result.count,
                                 m_n - result.count);
                }
                m_n -= result.count;
            } else {
                ++idle_writes;
            }
            if (result.error) {
                m_err = result.error;
                return Result<void>(m_err);
            }
            if (result.count == 0 &&
                idle_writes >= kMaxConsecutiveEmptyReads) {
                m_err = streams::IoErrShortWrite();
                return Result<void>(m_err);
            }
        }
        return Result<void>();
    }

    /**
     * @brief 对应 Go `Writer.ReadFrom`：从 `source` 一直读到 EOF 并写入本对象。
     * @return 成功时为读取并接受的字节数；出错时为出错前已接受的字节数
     *         （错误见 `Err()`）。EOF 视为正常结束。
     * @note 数据先经过本 Writer 的缓冲：调用方仍需 `Flush()` 才能保证落地。
     *       底层 Reader 连续 100 次返回 `(0, nil)` 时返回
     *       `streams::IoErrNoProgress()`，避免挂死。
     */
    template <typename SourceT>
    Result<std::size_t> ReadFrom(SourceT& source) {
        if (m_err) {
            return Result<std::size_t>(m_err);
        }
        std::size_t total = 0;
        std::uint8_t chunk[4096];
        std::size_t empty_reads = 0;
        for (;;) {
            const streams::ReadResult result =
                source.Read(reinterpret_cast<char*>(chunk), sizeof(chunk));
            if (result.count > sizeof(chunk)) {
                // io.Reader 契约要求 n <= len(p)。谎报更大的 n 会让我们
                // 越界读栈缓冲，必须拒绝而不是信任。
                return Result<std::size_t>(total, streams::IoErrShortWrite());
            }
            if (result.count > 0) {
                empty_reads = 0;
                total += Write(ByteSpan(chunk, result.count));
                if (m_err) {
                    return Result<std::size_t>(total, m_err);
                }
                if (result.error) {
                    // 「数据 + 错误」：数据已交付，错误在下一轮返回。
                    if (go2cpp::Is(result.error, streams::IoEof())) {
                        return Result<std::size_t>(total);
                    }
                    return Result<std::size_t>(total, result.error);
                }
                continue;
            }
            if (result.error) {
                if (go2cpp::Is(result.error, streams::IoEof())) {
                    return Result<std::size_t>(total);
                }
                return Result<std::size_t>(total, result.error);
            }
            if (++empty_reads >= kMaxConsecutiveEmptyReads) {
                return Result<std::size_t>(total, streams::IoErrNoProgress());
            }
        }
    }

    /**
     * @brief 对应 Go `Writer.Reset`：改绑到新目标并**丢弃**未刷出的缓冲数据。
     * @warning 与 Go 完全一致：缓冲里尚未 `Flush()` 的字节会被丢弃，不会写往
     *          旧目标。粘性错误也会被清除。
     */
    void Reset(WriterT& destination) noexcept {
        m_destination = &destination;
        m_n = 0;
        m_err = {};
    }

    /// 缓冲区中尚未写出的字节数。
    std::size_t Buffered() const noexcept { return m_n; }

    /// 缓冲区还能再放多少字节（`Size() - Buffered()`，Go 同）。
    std::size_t Available() const noexcept { return m_buffer.size() - m_n; }

    /// 缓冲区容量（构造时确定）。
    std::size_t Size() const noexcept { return m_buffer.size(); }

    /// 粘性错误；一切正常时为空。
    const ErrorPtr& Err() const noexcept { return m_err; }

private:
    WriterT* m_destination;
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_n{0};  ///< 缓冲内待写字节数（数据总在 [0, m_n)）
    ErrorPtr m_err;      ///< 粘性错误
};

}  // namespace go2cpp::bufio
