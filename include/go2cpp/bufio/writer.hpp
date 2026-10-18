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
 * ## 模块定位
 *
 * `bufio` 的输出侧，与 `Reader` 对称：上游是「一次只写几个字节」的调用方
 * （日志行、协议序列化、HTTP 响应头），下游是任意满足
 * `Write(const char*, size_t)` 契约的 `WriterT`（`streams::Writer` 家族、
 * `os::File`、socket 适配器）。它把 N 次小写合并成 1 次大写，减少系统调用。
 *
 * ## 本文件定义什么
 *
 * `Writer<WriterT>`：缓冲、自动 flush、部分写循环、粘性错误、`ReadFrom`。
 * 没有独立的错误类型，失败一律复用 `streams::IoErrShortWrite()` /
 * `IoErrNoProgress()` 或底层透传的错误。
 *
 * ## 阅读建议
 *
 * 1. 先读上面的「析构**不**自动 flush」——这是本类最容易踩的坑；
 * 2. 再看 `Write` 的大块快路径与 `Flush` 的部分写循环；
 * 3. 最后看 `ReadFrom` 如何处理「数据 + 错误」与连续空读。
 *
 * ## 边界与陷阱
 *
 * - 忘记 `Flush()` 等于丢数据，析构不会补救；
 * - `Write` 返回的字节数只表示「本对象已接受」，不代表已到达底层；
 * - 底层短写会被循环重试，但返回 0 字节且无错误会被视为违约；
 * - `Reset()` 会**丢弃**未 flush 的缓冲并清除粘性错误。
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
 *
 * ## 定位
 * `bufio` 的输出侧装饰器：调用方写入的记录先落在 `m_buffer`，缓冲满或显式
 * `Flush()` 时才调用 `WriterT::Write`。`Writer::ReadFrom` 还可把任意
 * `Read(char*, size_t)` 源直接泵进来，供 `streams::Copy` 一类工具复用。
 *
 * ## 生命周期与线程模型
 * 构造时按引用绑定底层 Writer 并分配固定容量缓冲；不可拷贝/赋值。谁创建谁
 * 销毁，底层对象必须比它活得久。析构**不** flush（理由见文件头）。所有方法
 * 假定同一线程串行调用，内部不加锁。
 *
 * ## 不变量
 * - `0 <= m_n <= m_buffer.size()`，待写数据总在 `[0, m_n)`；
 * - `Buffered() == m_n`、`Available() == Size() - m_n`；
 * - `m_err` 非空后所有写操作直接失败，不再调用底层（粘性）；
 * - `m_err` 非空时缓冲里可能仍有未写出的数据，`Reset()` 会丢弃它们。
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
     *
     * ## 步骤
     * step1. 已有粘性错误直接返回 0；
     * step2. 剩余数据放不进缓冲时：缓冲空则把整块直写底层（Go 快路径），
     *        否则先填满缓冲再 `Flush()`，循环直到放得下或出错；
     * step3. 每轮只用「实际接受」的 `step` 推进输入；0 字节且无错误按短写报错；
     * step4. 最后把剩余数据拷入缓冲，返回累计接受字节数。
     */
    std::size_t Write(ByteSpan p) {
        // step1: 粘性错误后不再接受任何数据
        if (m_err) {
            return 0;
        }
        std::size_t accepted = 0;
        // step2: 数据放不进缓冲时，先腾空间（直写或填满后 Flush）
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
            // step3: 只按实际接受的字节数推进，短写/零写都走同一路径
            accepted += step;
            p = p.subspan(step);
        }
        if (m_err) {
            return accepted;
        }
        // step4: 剩余数据一定放得下，拷入缓冲即完成
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
     *
     * ## 步骤
     * step1. 已有粘性错误直接返回；
     * step2. 循环把 `[0, m_n)` 写到底层；成功写 `k` 字节就把剩余数据前移；
     * step3. 底层返回的计数超过 `m_n` 视为违约，直接报短写（防止下溢）；
     * step4. 有错误立即粘住；连续 100 次 0 字节无错才报短写。
     */
    Result<void> Flush() {
        // step1: 出错后不再尝试
        if (m_err) {
            return Result<void>(m_err);
        }
        std::size_t idle_writes = 0;
        // step2: 循环直到缓冲清空
        while (m_n > 0) {
            const streams::WriteResult result = m_destination->Write(
                reinterpret_cast<const char*>(m_buffer.data()), m_n);
            // 只接受 [0, m_n] 范围内的计数：超出说明底层违反契约，按短写报错，
            // 否则 m_n 会下溢成天文数字并触发越界读。
            // step3: 不信任底层计数，超出本次请求即判违约
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
            // step4: 连续无进展达到上限才报错，给「暂时不可写」留余地
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
     *
     * ## 步骤
     * step1. 已有粘性错误直接返回；用栈上 4 KiB 块做中转；
     * step2. 读一块；`count > chunk` 视为 Reader 违约（否则会越界读栈）；
     * step3. 有数据先 `Write`（缓冲满会自动 `Flush`）；「数据 + 错误」时
     *        数据先交付，错误留到下一轮或本次末尾返回；
     * step4. EOF 视为正常结束；其它错误返回；连续 100 次空读报 NoProgress。
     */
    template <typename SourceT>
    Result<std::size_t> ReadFrom(SourceT& source) {
        // step1: 粘性错误优先；chunk 放栈上，避免为拷贝分配
        if (m_err) {
            return Result<std::size_t>(m_err);
        }
        std::size_t total = 0;
        std::uint8_t chunk[4096];
        std::size_t empty_reads = 0;
        for (;;) {
            const streams::ReadResult result =
                source.Read(reinterpret_cast<char*>(chunk), sizeof(chunk));
            // step2: 校验 Reader 契约：n 不能超过本次请求
            if (result.count > sizeof(chunk)) {
                // io.Reader 契约要求 n <= len(p)。谎报更大的 n 会让我们
                // 越界读栈缓冲，必须拒绝而不是信任。
                return Result<std::size_t>(total, streams::IoErrShortWrite());
            }
            // step3: 有数据先写入本对象；错误按「数据优先」延后
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
            // step4: 连续空读达到上限，判定源违约
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
    WriterT* m_destination;            ///< 底层 Writer（不拥有）；Write()/Flush() 调用其 Write()
    std::vector<std::uint8_t> m_buffer; ///< 缓冲本体，容量固定=Size()；Write() 写 [m_n, end)，Flush() 写出 [0, m_n)
    std::size_t m_n{0};  ///< 缓冲内待写字节数（数据总在 [0, m_n)）；Write()/Flush()/Reset() 修改，Buffered() 读取
    ErrorPtr m_err;      ///< 粘性错误；Write()/Flush()/ReadFrom() 写入，Err() 读取；非空后写操作直接失败
};

}  // namespace go2cpp::bufio
