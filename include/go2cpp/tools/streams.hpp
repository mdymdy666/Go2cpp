/**
 * @file streams.hpp
 * @brief Go `io` 包的 C++ 对应实现（流接口族 + 核心函数，语义对齐）。
 *
 * @details **命名说明（重要）**：Go 的 `io` 包提供的是「流式读写接口族」
 *   （`io.Reader` / `io.Writer` / `io.Copy` …），而本项目已经把这个名字用于
 *   **描述符与多路复用调度层**（`go2cpp::io::IOManager` / `FdManager`，
 *   见 `include/go2cpp/network/io.hpp`）。两者语义不同、不可合并，
 *   因此这里把 Go `io` 包的对应物放在命名空间 **`go2cpp::streams`**：
 *
 *   | Go | 本文件 |
 *   |---|---|
 *   | `io.Reader` | `streams::Reader` |
 *   | `io.WriteString(w, s)` | `streams::WriteString(w, s)` |
 *   | `io.EOF` | `streams::IoEof()` |
 *
 *   这是**有意保留的命名偏差**，已记入 `NextStep/dev-progress.md`；其余 API 与 Go 同名。
 *
 *   复用优先的取舍：C++ 侧没有与 Go `io` 语义对齐的现成库（`std::istream`/`std::ostream`
 *   用状态位而非错误值表达失败，且不支持「n>0 同时有错」这种 Go 契约），因此自研。
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/io/`（Go 1.23.0，BSD-3-Clause），
 *   **独立实现**。已对齐的非显然语义：
 *     - `ReadAtLeast`：`len(buf) < min` → `ErrShortBuffer`；读够 `min` 后**清除** `EOF`；
 *       `0 < n < min` 且遇 `EOF` → `ErrUnexpectedEOF`；
 *     - `ReadAll`：把 `EOF` 归一化为**成功**（错误为空）；
 *     - `CopyN`：写满 `n` 返回成功；`written < n` 且无错时返回 `EOF`；
 *     - `Copy`：优先使用 `WriterTo`（源）或 `ReaderFrom`（目标），否则 32 KiB 缓冲循环；
 *       连续 100 次空读 → `ErrNoProgress`；短写 → `ErrShortWrite`。
 *
 * @note **边界**：本轮未实现 `io.Pipe`（Go 用 goroutine 实现同步管道，C++ 侧需要与
 *       `core::ParkingCondition` 协作，留给后续）；`SectionReader` 已实现但不含 `Size()` 之外的
 *       `Outer` 语义；内存底座（`StringReader`/`BufferWriter`）是 `bytes.Reader`/`bytes.Buffer`
 *       的最小版本，完整版留给 `bytes` 模块。
 */

#pragma once

#include "go2cpp/error.hpp"
#include "go2cpp/strings.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::streams {

// ============================================================================
// 结果类型（对应 Go 的多返回值）
// ============================================================================

/** @brief `Read` 的结果（对应 Go 的 `(n int, err error)`）。 */
struct ReadResult {
    std::size_t count{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

/** @brief `Write` 的结果。 */
struct WriteResult {
    std::size_t count{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

/** @brief `Seek` 的结果。 */
struct SeekResult {
    std::int64_t offset{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

/** @brief 读单字节的结果。 */
struct ByteResult {
    char value{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

/** @brief 读单个 rune 的结果（`size` 为消耗的字节数，可能为 0）。 */
struct RuneResult {
    char32_t value{0};
    std::size_t size{0};
    ErrorPtr error;
    bool ok() const noexcept { return !error; }
};

// ============================================================================
// 错误锚点（对应 Go 的 io.EOF 等哨兵错误）
// ============================================================================

/**
 * @brief 对应 `io.EOF`。
 * @note 故意泄漏（`new` 后不析构）：与 `StrconvErrSyntax()` 同理，
 *       避免静态析构顺序让退出阶段的身份比较退化；**判断一律用 `Is(err, IoEof())`**。
 */
inline ErrorPtr IoEof() {
    static const ErrorPtr* const kError = new ErrorPtr(NewError("io: EOF"));
    return *kError;
}

/// 对应 `io.ErrUnexpectedEOF`。
inline ErrorPtr IoErrUnexpectedEof() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("io: unexpected EOF"));
    return *kError;
}

/// 对应 `io.ErrShortWrite`。
inline ErrorPtr IoErrShortWrite() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("io: short write"));
    return *kError;
}

/// 对应 `io.ErrShortBuffer`。
inline ErrorPtr IoErrShortBuffer() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("io: short buffer"));
    return *kError;
}

/// 对应 `io.ErrNoProgress`。
inline ErrorPtr IoErrNoProgress() {
    static const ErrorPtr* const kError =
        new ErrorPtr(NewError("io: multiple Read calls return no data or error"));
    return *kError;
}

namespace detail {

/// 复制循环的缓冲大小（与 Go 的 `copyBuffer` 一致：32 KiB）。
inline constexpr std::size_t kCopyBufferSize = 32U * 1024U;

/// 连续空读次数的上限（与 Go 的 `copyBuffer` 一致：100 次）。
inline constexpr int kMaxConsecutiveEmptyReads = 100;

}  // namespace detail

/** @brief 对应 `io.SeekStart` / `io.SeekCurrent` / `io.SeekEnd`。 */
inline constexpr int kSeekStart = 0;
inline constexpr int kSeekCurrent = 1;
inline constexpr int kSeekEnd = 2;

// ============================================================================
// 接口族（Go 用隐式接口，C++ 用抽象基类）
// ============================================================================

/** @brief 对应 `io.Reader`。 */
class Reader {
public:
    virtual ~Reader() = default;
    /// 至多读入 `size` 字节；允许返回 `count > 0` 同时带错误（Go 契约）。
    virtual ReadResult Read(char* buffer, std::size_t size) = 0;
};

/** @brief 对应 `io.Writer`。 */
class Writer {
public:
    virtual ~Writer() = default;
    virtual WriteResult Write(const char* data, std::size_t size) = 0;
};

/** @brief 对应 `io.Closer`。 */
class Closer {
public:
    virtual ~Closer() = default;
    virtual ErrorPtr Close() = 0;
};

/** @brief 对应 `io.Seeker`。 */
class Seeker {
public:
    virtual ~Seeker() = default;
    virtual SeekResult Seek(std::int64_t offset, int whence) = 0;
};

/** @brief 对应 `io.ReaderAt`。 */
class ReaderAt {
public:
    virtual ~ReaderAt() = default;
    virtual ReadResult ReadAt(char* buffer, std::size_t size, std::int64_t offset) = 0;
};

/** @brief 对应 `io.WriterAt`。 */
class WriterAt {
public:
    virtual ~WriterAt() = default;
    virtual WriteResult WriteAt(const char* data, std::size_t size, std::int64_t offset) = 0;
};

/** @brief 对应 `io.ByteReader`。 */
class ByteReader {
public:
    virtual ~ByteReader() = default;
    virtual ByteResult ReadByte() = 0;
};

/** @brief 对应 `io.ByteWriter`。 */
class ByteWriter {
public:
    virtual ~ByteWriter() = default;
    virtual ErrorPtr WriteByte(char value) = 0;
};

/** @brief 对应 `io.RuneReader`。 */
class RuneReader {
public:
    virtual ~RuneReader() = default;
    virtual RuneResult ReadRune() = 0;
};

/** @brief 对应 `io.StringWriter`（Go 的优化接口，`WriteString` 可免去 []byte 转换）。 */
class StringWriter {
public:
    virtual ~StringWriter() = default;
    virtual WriteResult WriteString(std::string_view text) = 0;
};

/** @brief 对应 `io.WriterTo`（源端可直接写入目标，`Copy` 会优先使用）。 */
class WriterTo {
public:
    virtual ~WriterTo() = default;
    virtual WriteResult WriteTo(Writer& destination) = 0;
};

/** @brief 对应 `io.ReaderFrom`（目标端可直接从源读取，`Copy` 会优先使用）。 */
class ReaderFrom {
public:
    virtual ~ReaderFrom() = default;
    virtual ReadResult ReadFrom(Reader& source) = 0;
};

// ============================================================================
// 核心函数
// ============================================================================

/**
 * @brief 对应 `io.ReadAtLeast`。
 * @details `size < minimum` → `ErrShortBuffer`；读够 `minimum` 后**清除** `EOF`；
 *          `0 < count < minimum` 且遇 `EOF` → `ErrUnexpectedEOF`。
 */
inline ReadResult ReadAtLeast(Reader& reader, char* buffer, std::size_t size,
                              std::size_t minimum) {
    ReadResult result;
    if (size < minimum) {
        result.error = IoErrShortBuffer();
        return result;
    }
    ErrorPtr last_error;
    while (result.count < minimum && !last_error) {
        const ReadResult step = reader.Read(buffer + result.count, size - result.count);
        result.count += step.count;
        last_error = step.error;
    }
    if (result.count >= minimum) {
        return result;                              // 成功：清除 EOF（Go 语义）
    }
    if (result.count > 0 && Is(last_error, IoEof())) {
        result.error = IoErrUnexpectedEof();
        return result;
    }
    result.error = last_error;
    return result;
}

/// 对应 `io.ReadFull`：读满 `size` 字节，否则返回 `ErrUnexpectedEOF`。
inline ReadResult ReadFull(Reader& reader, char* buffer, std::size_t size) {
    return ReadAtLeast(reader, buffer, size, size);
}

/**
 * @brief 对应 `io.ReadAll`：读到 `EOF` 为止。
 * @return {数据, 错误}；`EOF` 被归一化为**成功**（错误为空）。
 * @note 与 Go 的差别：Go 在「连续空读且无错」时会一直循环（依赖 Reader 契约），
 *       这里加了 `ErrNoProgress` 保护（与 `Copy` 的 Go 语义一致），避免死循环。
 */
inline std::pair<std::string, ErrorPtr> ReadAll(Reader& reader) {
    std::string data;
    char buffer[512];
    int empty_reads = 0;
    while (true) {
        const ReadResult result = reader.Read(buffer, sizeof(buffer));
        if (result.count > 0) {
            data.append(buffer, result.count);
            empty_reads = 0;
        }
        if (result.error) {
            if (Is(result.error, IoEof())) return {data, {}};
            return {data, result.error};
        }
        if (result.count == 0) {
            if (++empty_reads >= detail::kMaxConsecutiveEmptyReads) {
                return {data, IoErrNoProgress()};
            }
        }
    }
}

/// 对应 `io.WriteString`：若目标实现 `StringWriter` 则直接调用，否则退化为 `Write`。
inline WriteResult WriteString(Writer& writer, std::string_view text) {
    if (auto* const specialized = dynamic_cast<StringWriter*>(&writer)) {
        return specialized->WriteString(text);
    }
    return writer.Write(text.data(), text.size());
}

/**
 * @brief 对应 `io.CopyBuffer`：用给定缓冲把 `source` 复制到 `destination`。
 * @details 短写 → `ErrShortWrite`；连续空读 100 次 → `ErrNoProgress`；`EOF` → 成功。
 * @note Go 在缓冲长度为 0 时会 panic，这里返回 `ErrShortBuffer`（明确拒绝而非崩溃）。
 */
inline ReadResult CopyBuffer(Writer& destination, Reader& source, char* buffer,
                             std::size_t size) {
    ReadResult result;
    if (size == 0) {
        result.error = IoErrShortBuffer();
        return result;
    }
    int empty_reads = 0;
    while (true) {
        const ReadResult read = source.Read(buffer, size);
        if (read.count > 0) {
            empty_reads = 0;
            const WriteResult write = destination.Write(buffer, read.count);
            result.count += write.count;
            if (write.error) {
                result.error = write.error;
                return result;
            }
            if (write.count != read.count) {
                result.error = IoErrShortWrite();
                return result;
            }
        }
        if (read.error) {
            if (Is(read.error, IoEof())) return result;      // EOF → 成功
            result.error = read.error;
            return result;
        }
        if (read.count == 0 && ++empty_reads >= detail::kMaxConsecutiveEmptyReads) {
            result.error = IoErrNoProgress();
            return result;
        }
    }
}

/**
 * @brief 对应 `io.Copy`。
 * @details 优先使用源端的 `WriterTo` 或目标端的 `ReaderFrom`（与 Go 一致），
 *          否则回落到 32 KiB 缓冲循环。
 */
inline ReadResult Copy(Writer& destination, Reader& source) {
    if (auto* const writer_to = dynamic_cast<WriterTo*>(&source)) {
        return ReadResult{writer_to->WriteTo(destination).count, {}};
    }
    if (auto* const reader_from = dynamic_cast<ReaderFrom*>(&destination)) {
        return reader_from->ReadFrom(source);
    }
    std::vector<char> buffer(detail::kCopyBufferSize);
    return CopyBuffer(destination, source, buffer.data(), buffer.size());
}

/// 对应 `io.CopyN`：复制恰好 `n` 字节；不足且无错时返回 `EOF`。
inline ReadResult CopyN(Writer& destination, Reader& source, std::int64_t n);

// ============================================================================
// 适配器
// ============================================================================

/**
 * @brief 对应 `io.LimitReader`：最多读 `limit` 字节，之后返回 `EOF`。
 */
class LimitedReader final : public Reader {
public:
    LimitedReader(Reader& source, std::int64_t limit) : m_source(&source), m_remaining(limit) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        if (m_remaining <= 0) return ReadResult{0, IoEof()};
        const auto allowed = static_cast<std::size_t>(m_remaining);
        const std::size_t take = size < allowed ? size : allowed;
        ReadResult result = m_source->Read(buffer, take);
        m_remaining -= static_cast<std::int64_t>(result.count);
        return result;
    }

    /// 剩余可读字节数（Go 的 `*LimitedReader` 有可导出字段 `N`）。
    std::int64_t Remaining() const noexcept { return m_remaining; }

private:
    Reader* m_source;
    std::int64_t m_remaining;
};

/// 对应 `io.LimitReader`。@note 返回的适配器由调用方持有（存活期需覆盖使用期）。
inline LimitedReader LimitReader(Reader& source, std::int64_t limit) {
    return LimitedReader(source, limit);
}

/**
 * @brief 对应 `io.MultiReader`：按顺序读多个源，前一个 `EOF` 后接下一个。
 */
class MultiReader final : public Reader {
public:
    explicit MultiReader(std::vector<Reader*> sources) : m_sources(std::move(sources)) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        while (m_index < m_sources.size()) {
            ReadResult result = m_sources[m_index]->Read(buffer, size);
            if (Is(result.error, IoEof())) {
                ++m_index;
                // Go 语义：若同时读到了数据，则**清掉 EOF** 先交付数据，
                // 下一次调用再切到下一个源（不能把 EOF 一起交出去）。
                if (result.count > 0) return ReadResult{result.count, {}};
                continue;
            }
            return result;
        }
        return ReadResult{0, IoEof()};
    }

private:
    std::vector<Reader*> m_sources;
    std::size_t m_index{0};
};

/**
 * @brief 对应 `io.MultiWriter`：把同一份数据写到多个目标（任一失败即停止）。
 */
class MultiWriter final : public Writer {
public:
    explicit MultiWriter(std::vector<Writer*> destinations)
        : m_destinations(std::move(destinations)) {}

    WriteResult Write(const char* data, std::size_t size) override {
        for (Writer* destination : m_destinations) {
            const WriteResult result = destination->Write(data, size);
            if (result.error) return result;
            if (result.count != size) return WriteResult{result.count, IoErrShortWrite()};
        }
        return WriteResult{size, {}};
    }

private:
    std::vector<Writer*> m_destinations;
};

/**
 * @brief 对应 `io.TeeReader`：从源读取的同时把内容写给 `Writer`。
 */
class TeeReader final : public Reader {
public:
    TeeReader(Reader& source, Writer& tee) : m_source(&source), m_tee(&tee) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        ReadResult result = m_source->Read(buffer, size);
        if (result.count > 0) {
            const WriteResult write = m_tee->Write(buffer, result.count);
            if (write.error) return ReadResult{result.count, write.error};
            if (write.count != result.count) {
                return ReadResult{result.count, IoErrShortWrite()};
            }
        }
        return result;
    }

private:
    Reader* m_source;
    Writer* m_tee;
};

/**
 * @brief 对应 `io.SectionReader`：只暴露底层 `ReaderAt` 的一段区间。
 */
class SectionReader final : public Reader, public ReaderAt, public Seeker {
public:
    SectionReader(ReaderAt& source, std::int64_t offset, std::int64_t limit)
        : m_source(&source), m_base(offset), m_limit(limit) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        if (m_offset >= m_limit) return ReadResult{0, IoEof()};
        const std::int64_t available = m_limit - m_offset;
        const auto take = static_cast<std::size_t>(available);
        const std::size_t wanted = size < take ? size : take;
        ReadResult result = m_source->ReadAt(buffer, wanted, m_base + m_offset);
        m_offset += static_cast<std::int64_t>(result.count);
        return result;
    }

    ReadResult ReadAt(char* buffer, std::size_t size, std::int64_t offset) override {
        if (offset < 0 || offset >= m_limit) return ReadResult{0, IoEof()};
        const std::int64_t available = m_limit - offset;
        const auto take = static_cast<std::size_t>(available);
        const std::size_t wanted = size < take ? size : take;
        return m_source->ReadAt(buffer, wanted, m_base + offset);
    }

    SeekResult Seek(std::int64_t offset, int whence) override {
        std::int64_t target = 0;
        if (whence == 0) target = offset;                       // io.SeekStart
        else if (whence == 1) target = m_offset + offset;       // io.SeekCurrent
        else if (whence == 2) target = m_limit + offset;        // io.SeekEnd
        else return SeekResult{0, NewError("io: invalid whence")};
        if (target < 0) return SeekResult{0, NewError("io: negative position")};
        m_offset = target;
        return SeekResult{target, {}};
    }

    /// 对应 `SectionReader.Size`。
    std::int64_t Size() const noexcept { return m_limit; }

private:
    ReaderAt* m_source;
    std::int64_t m_base;
    std::int64_t m_limit;
    std::int64_t m_offset{0};
};

/**
 * @brief 对应 `io.Discard`：丢弃一切写入，但报告"全部写入成功"。
 */
class DiscardWriter final : public Writer {
public:
    WriteResult Write(const char* data, std::size_t size) override {
        (void)data;
        return WriteResult{size, {}};
    }
};

/// 对应 `io.Discard`（进程级单例，故意泄漏以规避静态析构顺序问题）。
inline Writer& Discard() {
    static Writer* const kInstance = new DiscardWriter();
    return *kInstance;
}

/// 对应 `io.CopyN` 的实现（依赖上面的 `LimitedReader`，故放在其后）。
inline ReadResult CopyN(Writer& destination, Reader& source, std::int64_t n) {
    LimitedReader limited(source, n);
    ReadResult result = Copy(destination, limited);
    if (static_cast<std::int64_t>(result.count) == n) return result;
    if (!result.error) result.error = IoEof();
    return result;
}

// ============================================================================
// 内存底座（bytes.Reader / bytes.Buffer 的最小版本，供测试与示例使用）
// ============================================================================

/**
 * @brief 从内存字符串读取（对应 `bytes.NewReader` 的最小子集）。
 */
class StringReader final : public Reader, public ReaderAt, public Seeker, public ByteReader,
                           public StringWriter {
public:
    explicit StringReader(std::string_view text) : m_text(text) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        if (m_offset >= m_text.size()) return ReadResult{0, IoEof()};
        const std::size_t available = m_text.size() - m_offset;
        const std::size_t take = size < available ? size : available;
        std::memcpy(buffer, m_text.data() + m_offset, take);
        m_offset += take;
        return ReadResult{take, {}};
    }

    ReadResult ReadAt(char* buffer, std::size_t size, std::int64_t offset) override {
        if (offset < 0 || static_cast<std::size_t>(offset) >= m_text.size()) {
            return ReadResult{0, IoEof()};
        }
        const std::size_t position = static_cast<std::size_t>(offset);
        const std::size_t available = m_text.size() - position;
        const std::size_t take = size < available ? size : available;
        std::memcpy(buffer, m_text.data() + position, take);
        return ReadResult{take, {}};
    }

    SeekResult Seek(std::int64_t offset, int whence) override {
        std::int64_t target = 0;
        if (whence == 0) target = offset;
        else if (whence == 1) target = static_cast<std::int64_t>(m_offset) + offset;
        else if (whence == 2) target = static_cast<std::int64_t>(m_text.size()) + offset;
        else return SeekResult{0, NewError("io: invalid whence")};
        if (target < 0) return SeekResult{0, NewError("io: negative position")};
        m_offset = static_cast<std::size_t>(target);
        return SeekResult{target, {}};
    }

    ByteResult ReadByte() override {
        if (m_offset >= m_text.size()) return ByteResult{0, IoEof()};
        return ByteResult{m_text[m_offset++], {}};
    }

    WriteResult WriteString(std::string_view text) override {
        (void)text;
        return WriteResult{0, NewError("io: StringReader is read-only")};
    }

    /// 对应 `bytes.Reader.Size` / `Len`。
    std::int64_t Size() const noexcept { return static_cast<std::int64_t>(m_text.size()); }
    std::int64_t Len() const noexcept {
        return static_cast<std::int64_t>(m_text.size() - m_offset);
    }

private:
    std::string_view m_text;
    std::size_t m_offset{0};
};

/**
 * @brief 写入内存缓冲（对应 `bytes.Buffer` 的最小子集），并实现 `StringWriter`。
 */
class BufferWriter final : public Writer, public StringWriter, public ByteWriter,
                           public Reader {
public:
    WriteResult Write(const char* data, std::size_t size) override {
        m_buffer.append(data, size);
        return WriteResult{size, {}};
    }

    WriteResult WriteString(std::string_view text) override {
        m_buffer.append(text);
        return WriteResult{text.size(), {}};
    }

    ErrorPtr WriteByte(char value) override {
        m_buffer.push_back(value);
        return {};
    }

    /// 从已写入内容中读取（对应 `bytes.Buffer.Read` 消费语义）。
    ReadResult Read(char* buffer, std::size_t size) override {
        const std::size_t available = m_buffer.size() - m_read_offset;
        const std::size_t take = size < available ? size : available;
        if (take == 0) return ReadResult{0, IoEof()};
        std::memcpy(buffer, m_buffer.data() + m_read_offset, take);
        m_read_offset += take;
        return ReadResult{take, {}};
    }

    /// 对应 `bytes.Buffer.String`。
    const std::string& String() const noexcept { return m_buffer; }
    /// 对应 `bytes.Buffer.Len`。
    std::size_t Len() const noexcept { return m_buffer.size() - m_read_offset; }
    /// 对应 `bytes.Buffer.Reset`。
    void Reset() {
        m_buffer.clear();
        m_read_offset = 0;
    }

private:
    std::string m_buffer;
    std::size_t m_read_offset{0};
};

/// 便捷：把整个 `Reader` 的内容读成 `std::string`（失败时返回空串）。
inline std::string ReadString(Reader& reader) {
    const auto result = ReadAll(reader);
    return result.second ? std::string{} : result.first;
}

}  // namespace go2cpp::streams
