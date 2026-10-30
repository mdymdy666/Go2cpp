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
 *   ## 批量 flush（本项目扩展，Go 没有）
 *
 *   除「缓冲满自动 flush」之外，`Writer` 支持把多条逻辑消息合并成一次
 *   批量写：`SetBatchConfig()` 选择 `FlushPolicy`（满/定时/显式/混合），
 *   `FlushBatch()` 显式立即写出全部待发段。底层若实现
 *   `WriteBatch(const BatchEntry*, size_t)`（例如包装 `writev`/`sendmmsg`
 *   的适配器），N 条消息只需 1 次系统调用；否则退化为一次 `Write`。
 *
 *   - `Flush()` 仍是单次 `Write`（兼容旧行为，也是「立刻落地」的最强保证）；
 *   - 批量只改变**何时/分几次**调用底层，不改变字节顺序与内容；
 *   - 定时检查是惰性的（`Write` 返回前），没有后台线程；写完即停时必须
 *     显式 `Flush()`/`FlushBatch()` 收尾。
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

#include "go2cpp/bufio/batch.hpp"
#include "go2cpp/bufio/byte_span.hpp"
#include "go2cpp/bufio/limits.hpp"
#include "go2cpp/bufio/result.hpp"
#include "go2cpp/streams.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace go2cpp::bufio {

namespace detail {

/**
 * @brief 探测底层 Writer 是否提供批量写接口。
 *
 * @details 约定签名为
 *   `streams::WriteResult WriteBatch(const BatchEntry*, std::size_t)`，
 *   返回「已写出字节数 + 错误」。检出时 `Writer::FlushBatch()` 走一次
 *   批量调用（典型实现 `writev`/`sendmmsg`）；否则退化为一次 `Write`。
 *   这里要求返回类型**精确**是 `streams::WriteResult`，不做隐式转换，
 *   避免把同名但语义不同的方法误当成批量接口。
 */
template <typename T, typename = void>
struct HasWriteBatch : std::false_type {};

template <typename T>
struct HasWriteBatch<T, std::void_t<decltype(std::declval<T&>().WriteBatch(
                           std::declval<const BatchEntry*>(),
                           std::declval<std::size_t>()))>>
    : std::is_same<decltype(std::declval<T&>().WriteBatch(
                       std::declval<const BatchEntry*>(),
                       std::declval<std::size_t>())),
                   streams::WriteResult> {};

}  // namespace detail

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
 * 销毁，底层对象必须比它活得久。析构**不** flush（理由见文件头）。
 *
 * ## 批量策略与线程模型
 * `SetBatchConfig()` 可调策略/批大小/水位/间隔；`FlushBatch()` 立即把当前
 * 所有段交给底层（有 `WriteBatch` 时一次系统调用，否则一次 `Write`）。
 * 策略只影响「何时自动写」；容量不足与段数达到 `max_batch` 是硬上限，
 * 任何策略下都会触发。定时检查没有后台线程，由 `Write`/`WriteByte`
 * 返回前惰性触发。
 *
 * 所有方法假定同一线程串行调用，内部不加锁；多个线程共享同一个 `Writer`
 * 必须由调用方加锁（每个线程各用一个 `Writer` 则天然无竞争）。
 *
 * ## 不变量
 * - `0 <= m_n <= m_buffer.size()`，待写数据总在 `[0, m_n)`；
 * - `m_segments` 按顺序铺满 `[0, m_n)`：无空洞、不重叠、总和等于 `m_n`；
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
          m_buffer(detail::ClampBufferSize(size)),
          m_last_flush(std::chrono::steady_clock::now()) {
        // 水位不可能超过容量：容量是硬边界，配置值按容量钳制。
        m_config.threshold = std::min(m_config.threshold, m_buffer.size());
        m_segments.reserve(m_config.max_batch);
        m_entries.reserve(m_config.max_batch);
    }

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
     * @note 返回值不代表「已到达底层」——只有 `Flush()`/`FlushBatch()`
     *       成功才保证这一点（自动 flush 失败时错误同样粘在 `Err()`）。
     *
     * ## 步骤
     * step1. 已有粘性错误直接返回 0；
     * step2. 水位检查：`kOnFull`/`kHybrid` 下旧数据已达到 `threshold` 就先
     *        批量发出，给新数据腾出干净的空间（不改变本批接受语义）；
     * step3. 剩余数据放不进缓冲时：缓冲空则整块直写底层（Go 快路径），
     *        否则填满后再 `FlushBatch()`，循环直到放得下或出错；
     * step4. 每轮只用「实际接受」的 `step` 推进输入；0 字节且无错误按短写报错；
     * step5. 写完后按策略做一次惰性检查（段数硬上限 / 定时）。
     */
    std::size_t Write(ByteSpan p) {
        // step1: 粘性错误后不再接受任何数据
        if (m_err) {
            return 0;
        }
        // step2: 旧数据到达水位，先按批量策略发出去
        if (m_n > 0 && PolicyUsesThreshold() && m_n >= m_config.threshold) {
            const Result<void> flushed = FlushBatch();
            if (!flushed.ok()) {
                return 0;
            }
        }
        std::size_t accepted = 0;
        // 本次 Write 在缓冲里对应的段是否已经创建：同一调用的多块数据合并
        // 成一个段，flush 之后重新开段。
        bool segment_open = false;
        while (!p.empty()) {
            const std::size_t available = Available();
            if (p.size() <= available) {
                CopyIn(p.data(), p.size(), segment_open);
                accepted += p.size();
                break;
            }
            if (m_n == 0) {
                // 大块直接写；部分写就继续循环，0 字节无错按短写处理，
                // 否则会死循环（Go 依赖 Writer 契约，这里不冒这个风险）。
                const streams::WriteResult result =
                    m_destination->Write(reinterpret_cast<const char*>(p.data()), p.size());
                // 底层若谎报「写得比给的多」，按本次请求长度截断——不信任它。
                const std::size_t step = std::min(result.count, p.size());
                accepted += step;
                if (result.error) {
                    m_err = result.error;
                    return accepted;
                }
                if (step == 0) {
                    m_err = streams::IoErrShortWrite();
                    return accepted;
                }
                p = p.subspan(step);
                segment_open = false;
                continue;
            }
            if (available == 0) {
                // 防御分支：水位检查通常已处理；容量满时先发再继续。
                const Result<void> flushed = FlushBatch();
                if (!flushed.ok()) {
                    return accepted;
                }
                segment_open = false;
                continue;
            }
            // 缓冲还有空间但放不下整块：先填满，再批量发出，继续处理剩余部分。
            const std::size_t step = std::min(p.size(), available);
            CopyIn(p.data(), step, segment_open);
            accepted += step;
            p = p.subspan(step);
            const Result<void> flushed = FlushBatch();
            if (!flushed.ok()) {
                return accepted;
            }
            segment_open = false;
        }
        // step5: 段数硬上限 / 定时策略的惰性检查
        MaybeAutoFlush();
        return accepted;
    }

    /// 与 `streams::Writer` 契约互操作（供 `streams::WriteString` 等复用）。
    streams::WriteResult Write(const char* data, std::size_t size) {
        const std::size_t count = Write(AsBytes(const_cast<char*>(data), size));
        if (m_err) {
            return streams::WriteResult{count, m_err};
        }
        return streams::WriteResult{count, {}};
    }

    /**
     * @brief 写一个字节；缓冲达到水位或满时先按策略 flush。
     * @details 连续的 `WriteByte` 属于同一条逻辑消息，合并进同一个段，
     *          避免把逐字节序列化的一条消息拆成 N 个 iovec。
     */
    Result<void> WriteByte(std::uint8_t value) {
        if (m_err) {
            return Result<void>(m_err);
        }
        if (m_n > 0 && PolicyUsesThreshold() && m_n >= m_config.threshold) {
            const Result<void> flushed = FlushBatch();
            if (!flushed.ok()) {
                return flushed;
            }
        }
        if (Available() == 0) {
            const Result<void> flushed = FlushBatch();
            if (!flushed.ok()) {
                return flushed;
            }
        }
        m_buffer[m_n++] = value;
        // 连续 WriteByte 接到上一个段尾部；中间夹了 Write/WriteString
        // 就重新开段，保留「一次 Write 一条消息」的段边界。
        if (m_last_op_byte && !m_segments.empty() &&
            m_segments.back().offset + m_segments.back().size == m_n - 1) {
            ++m_segments.back().size;
        } else {
            m_segments.push_back(Segment{m_n - 1, 1});
        }
        m_last_op_byte = true;
        MaybeAutoFlush();
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
     * @brief 把缓冲区内容**全部**写到底层（单次 `Write`，兼容旧语义）。
     * @details 循环处理部分写：每次成功写 `k` 字节就把剩余数据前移 `k`，
     *          直到缓冲清空。底层返回 0 字节且无错误时按短写报错（连续
     *          100 次仍未进展才报，给「暂时不可写」的实现留余地）。
     *          失败时**已写出的部分不回退**，未写出的部分仍留在缓冲里，
     *          错误保持粘性。与 `FlushBatch()` 的区别是**不使用**底层的
     *          批量接口：这是最保守、最可预测的落地方式。
     */
    Result<void> Flush() {
        if (m_err) {
            return Result<void>(m_err);
        }
        const Result<void> result = FlushLinear();
        if (result.ok()) {
            m_segments.clear();
            m_last_op_byte = false;
        }
        m_last_flush = std::chrono::steady_clock::now();
        return result;
    }

    /**
     * @brief 立即把当前所有待发段合并成一次批量写。
     *
     * @details 底层提供 `WriteBatch(const BatchEntry*, std::size_t)` 时，
     *          每个段对应一个 `BatchEntry`（典型实现是 `writev` /
     *          `sendmmsg`），N 条消息只产生 1 次系统调用；否则因为所有
     *          段在缓冲里连续，退化为一次 `Write`。
     *
     *          与 `Flush()` 一样处理部分写，并保持错误粘性。显式调用不受
     *          `FlushPolicy` 限制——策略只决定**自动** flush 的时机。
     *          空缓冲时直接成功，不触碰底层。
     */
    Result<void> FlushBatch() {
        if (m_err) {
            return Result<void>(m_err);
        }
        Result<void> result;
        if constexpr (detail::HasWriteBatch<WriterT>::value) {
            result = FlushBatchImpl();
        } else {
            result = FlushLinear();
        }
        if (result.ok()) {
            m_segments.clear();
            m_last_op_byte = false;
        }
        m_last_flush = std::chrono::steady_clock::now();
        return result;
    }

    /**
     * @brief 设置批量 flush 策略（随时可调，立即生效）。
     * @param config 新配置。规范化规则：
     *   - `max_batch == 0` → 1；`threshold == 0` → 1；
     *   - `threshold > 缓冲容量` → 缓冲容量（容量是硬边界）；
     *   - `interval < 0` → 0（每次写完都检查）。
     * @note 调用后定时策略从当前时刻重新计时，避免旧时间戳立刻触发一次。
     */
    void SetBatchConfig(BatchConfig config) {
        config.max_batch = std::max<std::size_t>(config.max_batch, 1);
        config.max_batch = std::min(config.max_batch, kMaxBatchEntries);
        config.threshold = std::max<std::size_t>(config.threshold, 1);
        config.threshold = std::min(config.threshold, m_buffer.size());
        if (config.interval.count() < 0) {
            config.interval = std::chrono::microseconds{0};
        }
        m_config = config;
        if (m_segments.capacity() < m_config.max_batch) {
            m_segments.reserve(m_config.max_batch);
        }
        if (m_entries.capacity() < m_config.max_batch) {
            m_entries.reserve(m_config.max_batch);
        }
        m_last_flush = std::chrono::steady_clock::now();
    }

    /// 当前批量配置（已按缓冲容量规范化）。
    const BatchConfig& GetBatchConfig() const noexcept { return m_config; }

    /// 当前待发段数（下一次 `FlushBatch()` 最多提交这么多个批次/iovec）。
    std::size_t PendingBatches() const noexcept { return m_segments.size(); }

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
        m_segments.clear();
        m_last_op_byte = false;
        m_last_flush = std::chrono::steady_clock::now();
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
    /// 缓冲内的一个逻辑段；`offset` 是 `m_buffer` 下标，`size > 0`。
    struct Segment {
        std::size_t offset{0};  ///< 段首在缓冲中的位置
        std::size_t size{0};    ///< 段长度（字节）
    };

    /// 当前策略是否把 `threshold` 当作自动 flush 的触发条件。
    bool PolicyUsesThreshold() const noexcept {
        return m_config.policy == FlushPolicy::kOnFull ||
               m_config.policy == FlushPolicy::kHybrid;
    }

    /// 当前策略是否按时间自动 flush。
    bool PolicyUsesInterval() const noexcept {
        return m_config.policy == FlushPolicy::kInterval ||
               m_config.policy == FlushPolicy::kHybrid;
    }

    /// 把 `size` 字节拷到缓冲尾部，并按需新建/合并本次 Write 的段。
    void CopyIn(const std::uint8_t* data, std::size_t size, bool& segment_open) {
        if (size == 0) {
            return;
        }
        std::memcpy(m_buffer.data() + m_n, data, size);
        if (segment_open && !m_segments.empty() &&
            m_segments.back().offset + m_segments.back().size == m_n) {
            m_segments.back().size += size;
        } else {
            m_segments.push_back(Segment{m_n, size});
            segment_open = true;
        }
        m_n += size;
        m_last_op_byte = false;
    }

    /// 消费已成功写出的前 `count` 字节：前移缓冲并同步段表。
    void ConsumeFront(std::size_t count) {
        if (count == 0) {
            return;
        }
        std::size_t left = count;
        std::size_t consumed = 0;
        while (left > 0 && consumed < m_segments.size()) {
            Segment& segment = m_segments[consumed];
            if (left >= segment.size) {
                left -= segment.size;
                ++consumed;
            } else {
                segment.offset += left;
                segment.size -= left;
                left = 0;
            }
        }
        if (consumed > 0) {
            m_segments.erase(
                m_segments.begin(),
                m_segments.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
        std::memmove(m_buffer.data(), m_buffer.data() + count, m_n - count);
        for (Segment& segment : m_segments) {
            segment.offset -= count;
        }
        m_n -= count;
    }

    /// 单次 `Write` 循环；`Flush()` 与「底层无批量接口」的 `FlushBatch()` 共用。
    Result<void> FlushLinear() {
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
                ConsumeFront(result.count);
            } else {
                ++idle_writes;
            }
            if (result.error) {
                m_err = result.error;
                return Result<void>(m_err);
            }
            // 连续无进展达到上限才报错，给「暂时不可写」留余地
            if (result.count == 0 && idle_writes >= kMaxConsecutiveEmptyReads) {
                m_err = streams::IoErrShortWrite();
                return Result<void>(m_err);
            }
        }
        return Result<void>();
    }

    /// 走底层 `WriteBatch` 的批量写循环（由 `HasWriteBatch` 保证可用）。
    Result<void> FlushBatchImpl() {
        std::size_t idle_writes = 0;
        while (m_n > 0) {
            m_entries.clear();
            for (const Segment& segment : m_segments) {
                m_entries.push_back(BatchEntry{
                    reinterpret_cast<const char*>(m_buffer.data() + segment.offset),
                    segment.size});
            }
            if (m_entries.empty()) {
                // 不变量要求段表铺满 [0, m_n)；真出现空洞时退回线性写，
                // 宁可少一次批量调用，也不能漏数据。
                return FlushLinear();
            }
            const streams::WriteResult result =
                m_destination->WriteBatch(m_entries.data(), m_entries.size());
            if (result.count > m_n) {
                m_err = streams::IoErrShortWrite();
                return Result<void>(m_err);
            }
            if (result.count > 0) {
                idle_writes = 0;
                ConsumeFront(result.count);
            } else {
                ++idle_writes;
            }
            if (result.error) {
                m_err = result.error;
                return Result<void>(m_err);
            }
            if (result.count == 0 && idle_writes >= kMaxConsecutiveEmptyReads) {
                m_err = streams::IoErrShortWrite();
                return Result<void>(m_err);
            }
        }
        return Result<void>();
    }

    /// 写入路径末尾的惰性检查：段数硬上限 + 定时策略。
    void MaybeAutoFlush() {
        if (m_err || m_n == 0) {
            return;
        }
        // 硬上限：一次批量写最多 max_batch 段，任何策略都必须遵守。
        if (m_segments.size() >= m_config.max_batch) {
            FlushBatch();
            return;
        }
        if (!PolicyUsesInterval()) {
            return;
        }
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        if (now - m_last_flush >= m_config.interval) {
            FlushBatch();
        }
    }

    WriterT* m_destination;             ///< 底层 Writer（不拥有），Write()/Flush() 调用
    std::vector<std::uint8_t> m_buffer; ///< 缓冲本体，容量固定=Size()；待发数据在 [0, m_n)
    std::size_t m_n{0};                 ///< 缓冲内待写字节数；Buffered() 读取
    std::vector<Segment> m_segments;    ///< 逻辑段表，按顺序铺满 [0, m_n)
    BatchConfig m_config;               ///< 批量策略（SetBatchConfig 规范化）
    std::vector<BatchEntry> m_entries;  ///< FlushBatch 复用的 iovec 数组，避免每次分配
    std::chrono::steady_clock::time_point m_last_flush;  ///< 上次成功/尝试 flush 的时刻
    bool m_last_op_byte{false};         ///< 上一次写是否 WriteByte（决定段合并）
    ErrorPtr m_err;                     ///< 粘性错误；非空后写操作直接失败
};

}  // namespace go2cpp::bufio
