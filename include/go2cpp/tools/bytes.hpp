/**
 * @file bytes.hpp
 * @brief Go `bytes` 包的 C++ 对应实现（字节切片操作 + Buffer/Reader）。
 *
 * @details **类型映射**：Go 的 `[]byte` 在 C++ 里映射为 `std::string`（字节容器，`char` 即 8 位字节），
 *   并给出别名 `go2cpp::bytes::Bytes = std::string`。Go 里 `string` 与 `[]byte` 是两个类型，
 *   C++ 里它们同为 `std::string`；因此本模块的函数参数用 `std::string_view`（可同时接受两者），
 *   返回新切片时返回 `Bytes`。`bytes.Trim(s, cutset)` 的 cutset 在 Go 里是 `string`，
 *   与 `strings.Trim` 一致，这里同样用 `std::string_view`。
 *
 * @details 复用优先的结论：
 *   - **函数家族直接复用 `go2cpp/strings.hpp`**：Go 的 `bytes` 与 `strings` 是同一套算法作用在不同类型上
 *     （`bytes.Split` 与 `strings.Split` 行为一致），因此本模块的函数是对 `strings` 同名函数的转发，
 *     避免两份实现漂移；
 *   - `Buffer`/`Reader` 两个类**独立实现**（`go2cpp::streams` 的 `BufferWriter`/`StringReader` 是 final 且
 *     只覆盖最小子集，而 `bytes.Buffer` 还需要 `Next`/`ReadString`/`ReadBytes`/`UnreadByte`/`Truncate`/
 *     `Grow`/`Bytes`/`WriterTo`/`ReadFrom`），实现时复用 `streams` 的**接口**（`Reader`/`Writer`/…）以保持互操作。
 *
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/bytes/`
 *   （Go 1.23.0，BSD-3-Clause），**独立实现**。已对齐的非显然语义：
 *     - `Buffer.Read` 在**缓冲为空**时返回 `io.EOF`（不是 0 字节无错误）；
 *     - `Buffer.Next(n)`：`n > Len()` 时返回剩余全部并**清空**；缓冲为空时返回空切片；
 *     - `Buffer.UnreadByte`：只在**上一次操作是读**时有效，否则返回错误（Go 的 `errors.New("bytes.Buffer: UnreadByte: previous operation was not a successful read")`）；
 *     - `Buffer.ReadBytes`/`ReadString` 把**分隔符包含在返回值里**；未找到时返回剩余全部并带 `io.EOF`；
 *     - `Buffer.Truncate(n)` 只保留前 `n` 字节（`n` 大于当前长度时报错）；
 *     - `Buffer.Grow(n)` 保证还能再写 `n` 字节而不重新分配。
 *
 * @note **边界**：未实现 `bytes.FieldsFunc`/`ContainsFunc`/`IndexFunc`/`LastIndexFunc`/`Map`/`ToValidUTF8`/
 *       `ToTitle`/`Title`（前四个需要回调且与 `strings` 的对应物同构，后续可与 `strings` 一起补）；
 *       `Reader` 未实现 `WriteTo`（Go 用它优化 `io.Copy`，本模块留待需要时补）；
 *       `Buffer.ReadFrom` 的「预分配/增长」策略比 Go 简单（不与 Go 的 `MinRead` 优化对齐）。
 */

#pragma once

#include "go2cpp/error.hpp"
#include "go2cpp/streams.hpp"
#include "go2cpp/strings.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::bytes {

/// 对应 Go 的 `[]byte`（C++ 里字节序列就是 `std::string`）。
using Bytes = std::string;

/// 对应 Go 的 `[][]byte`。
using ByteSlices = std::vector<Bytes>;

// ============================================================================
// 比较与判定（转发 strings，保证两模块行为不分叉）
// ============================================================================

/// 对应 `bytes.Equal`。
inline bool Equal(std::string_view left, std::string_view right) { return left == right; }

/// 对应 `bytes.Compare`（-1 / 0 / +1）。
inline int Compare(std::string_view left, std::string_view right) {
    return go2cpp::Compare(left, right);
}

/// 对应 `bytes.EqualFold`。
inline bool EqualFold(std::string_view left, std::string_view right) {
    return go2cpp::EqualFold(left, right);
}

/// 对应 `bytes.HasPrefix`。
inline bool HasPrefix(std::string_view data, std::string_view prefix) {
    return go2cpp::HasPrefix(data, prefix);
}

/// 对应 `bytes.HasSuffix`。
inline bool HasSuffix(std::string_view data, std::string_view suffix) {
    return go2cpp::HasSuffix(data, suffix);
}

// ============================================================================
// 查找
// ============================================================================

/// 对应 `bytes.Contains`。
inline bool Contains(std::string_view data, std::string_view subslice) {
    return go2cpp::Contains(data, subslice);
}

/// 对应 `bytes.ContainsAny`。
inline bool ContainsAny(std::string_view data, std::string_view chars) {
    return go2cpp::ContainsAny(data, chars);
}

/// 对应 `bytes.ContainsRune`。
inline bool ContainsRune(std::string_view data, char32_t rune) {
    return go2cpp::ContainsRune(data, rune);
}

/// 对应 `bytes.Count`。
inline std::int64_t Count(std::string_view data, std::string_view separator) {
    return go2cpp::Count(data, separator);
}

/// 对应 `bytes.Index`。
inline std::int64_t Index(std::string_view data, std::string_view separator) {
    return go2cpp::Index(data, separator);
}

/// 对应 `bytes.IndexByte`。
inline std::int64_t IndexByte(std::string_view data, char value) {
    return go2cpp::IndexByte(data, value);
}

/// 对应 `bytes.IndexRune`。
inline std::int64_t IndexRune(std::string_view data, char32_t rune) {
    return go2cpp::IndexRune(data, rune);
}

/// 对应 `bytes.IndexAny`。
inline std::int64_t IndexAny(std::string_view data, std::string_view chars) {
    return go2cpp::IndexAny(data, chars);
}

/// 对应 `bytes.LastIndex`。
inline std::int64_t LastIndex(std::string_view data, std::string_view separator) {
    return go2cpp::LastIndex(data, separator);
}

/// 对应 `bytes.LastIndexByte`。
inline std::int64_t LastIndexByte(std::string_view data, char value) {
    return go2cpp::LastIndexByte(data, value);
}

/// 对应 `bytes.LastIndexAny`。
inline std::int64_t LastIndexAny(std::string_view data, std::string_view chars) {
    // strings 没有 LastIndexAny；这里按反向扫描实现（与 Go 的 bytes.LastIndexAny 语义一致）。
    if (chars.empty()) return -1;
    for (std::size_t end = data.size(); end > 0;) {
        std::size_t begin = end - 1;
        while (begin > 0 && (static_cast<unsigned char>(data[begin]) & 0xC0U) == 0x80U) --begin;
        const auto decoded = go2cpp::DecodeRune(data.substr(begin));
        if (go2cpp::CutsetContains(chars, decoded.first)) {
            return static_cast<std::int64_t>(begin);
        }
        end = begin;
    }
    return -1;
}

// ============================================================================
// 切分与合并
// ============================================================================

/// 对应 `bytes.Split`。
inline ByteSlices Split(std::string_view data, std::string_view separator) {
    return go2cpp::Split(data, separator);
}

/// 对应 `bytes.SplitN`。
inline ByteSlices SplitN(std::string_view data, std::string_view separator, std::int64_t n) {
    return go2cpp::SplitN(data, separator, n);
}

/// 对应 `bytes.SplitAfter`。
inline ByteSlices SplitAfter(std::string_view data, std::string_view separator) {
    return go2cpp::SplitAfter(data, separator);
}

/// 对应 `bytes.SplitAfterN`。
inline ByteSlices SplitAfterN(std::string_view data, std::string_view separator, std::int64_t n) {
    return go2cpp::SplitAfterN(data, separator, n);
}

/// 对应 `bytes.Fields`。
inline ByteSlices Fields(std::string_view data) { return go2cpp::Fields(data); }

/// 对应 `bytes.Join`。
inline Bytes Join(const ByteSlices& slices, std::string_view separator) {
    return go2cpp::Join(slices, separator);
}

// ============================================================================
// 变换与修剪
// ============================================================================

/// 对应 `bytes.Repeat`。
inline Bytes Repeat(std::string_view data, std::int64_t count) {
    return go2cpp::Repeat(data, count);
}

/// 对应 `bytes.ToUpper`。
inline Bytes ToUpper(std::string_view data) { return go2cpp::ToUpper(data); }

/// 对应 `bytes.ToLower`。
inline Bytes ToLower(std::string_view data) { return go2cpp::ToLower(data); }

/// 对应 `bytes.Replace`。
inline Bytes Replace(std::string_view data, std::string_view old_part, std::string_view new_part,
                     std::int64_t n) {
    return go2cpp::Replace(data, old_part, new_part, n);
}

/// 对应 `bytes.ReplaceAll`。
inline Bytes ReplaceAll(std::string_view data, std::string_view old_part,
                        std::string_view new_part) {
    return go2cpp::ReplaceAll(data, old_part, new_part);
}

/// 对应 `bytes.Trim`（`cutset` 是字符集合，与 `strings.Trim` 同义）。
inline Bytes Trim(std::string_view data, std::string_view cutset) {
    return go2cpp::Trim(data, cutset);
}

/// 对应 `bytes.TrimLeft`。
inline Bytes TrimLeft(std::string_view data, std::string_view cutset) {
    return go2cpp::TrimLeft(data, cutset);
}

/// 对应 `bytes.TrimRight`。
inline Bytes TrimRight(std::string_view data, std::string_view cutset) {
    return go2cpp::TrimRight(data, cutset);
}

/// 对应 `bytes.TrimSpace`。
inline Bytes TrimSpace(std::string_view data) { return go2cpp::TrimSpace(data); }

/// 对应 `bytes.TrimPrefix`。
inline Bytes TrimPrefix(std::string_view data, std::string_view prefix) {
    return go2cpp::TrimPrefix(data, prefix);
}

/// 对应 `bytes.TrimSuffix`。
inline Bytes TrimSuffix(std::string_view data, std::string_view suffix) {
    return go2cpp::TrimSuffix(data, suffix);
}

// ============================================================================
// Buffer（对应 bytes.Buffer）
// ============================================================================

/**
 * @brief 对应 `bytes.Buffer`：可读可写的字节缓冲，同时实现 `streams` 的读写接口。
 * @details 读游标与写位置分离：`Read` 从读游标推进，`Write` 追加到末尾（与 Go 一致）。
 */
class Buffer final : public streams::Reader,
                     public streams::Writer,
                     public streams::ByteReader,
                     public streams::ByteWriter,
                     public streams::StringWriter,
                     public streams::ReaderFrom,
                     public streams::WriterTo,
                     public streams::Seeker {
public:
    Buffer() = default;

    /// 对应 `bytes.NewBuffer(buf)`：以已有内容初始化（写入位置在末尾）。
    explicit Buffer(std::string_view initial) : m_data(initial) {}

    /// 对应 `bytes.NewBufferString(s)`。
    static Buffer FromString(std::string_view text) { return Buffer(text); }

    // ---- 写 ----
    /// 对应 `Buffer.Write`。
    streams::WriteResult Write(const char* data, std::size_t size) override {
        m_data.append(data, size);
        return streams::WriteResult{size, {}};
    }
    /// 对应 `Buffer.WriteString`。
    streams::WriteResult WriteString(std::string_view text) override {
        m_data.append(text);
        return streams::WriteResult{text.size(), {}};
    }
    /// 对应 `Buffer.WriteByte`。
    ErrorPtr WriteByte(char value) override {
        m_data.push_back(value);
        m_last_op_was_read = false;
        return {};
    }
    /// 对应 `Buffer.WriteRune`。
    void WriteRune(char32_t rune) {
        go2cpp::AppendRune(m_data, rune);
        m_last_op_was_read = false;
    }

    // ---- 读 ----
    /// 对应 `Buffer.Read`：缓冲为空时返回 `io.EOF`。
    streams::ReadResult Read(char* out, std::size_t size) override {
        const std::size_t available = m_data.size() - m_read_offset;
        const std::size_t take = size < available ? size : available;
        if (take == 0) return streams::ReadResult{0, streams::IoEof()};
        std::memcpy(out, m_data.data() + m_read_offset, take);
        m_read_offset += take;
        m_last_op_was_read = true;
        return streams::ReadResult{take, {}};
    }
    /// 对应 `Buffer.ReadByte`。
    streams::ByteResult ReadByte() override {
        if (m_read_offset >= m_data.size()) {
            return streams::ByteResult{0, streams::IoEof()};
        }
        m_last_op_was_read = true;
        return streams::ByteResult{m_data[m_read_offset++], {}};
    }
    /// 对应 `Buffer.ReadRune`。
    streams::RuneResult ReadRune() {
        if (m_read_offset >= m_data.size()) {
            return streams::RuneResult{0, 0, streams::IoEof()};
        }
        const auto decoded = go2cpp::DecodeRune(std::string_view(m_data).substr(m_read_offset));
        m_read_offset += decoded.second;
        m_last_op_was_read = true;
        return streams::RuneResult{decoded.first, decoded.second, {}};
    }
    /**
     * @brief 对应 `Buffer.Next(n)`。
     * @details `n > Len()` 时返回剩余全部并清空缓冲；缓冲为空时返回空切片且**不带错误**（与 Go 一致）。
     */
    Bytes Next(std::size_t n) {
        const std::size_t available = m_data.size() - m_read_offset;
        const std::size_t take = n < available ? n : available;
        Bytes taken = m_data.substr(m_read_offset, take);
        m_read_offset += take;
        if (m_read_offset >= m_data.size()) {
            m_data.clear();
            m_read_offset = 0;
        }
        m_last_op_was_read = true;
        return taken;
    }
    /**
     * @brief 对应 `Buffer.ReadBytes(delim)`：返回**包含分隔符**的片段；未找到时返回剩余全部并带 `io.EOF`。
     */
    std::pair<Bytes, ErrorPtr> ReadBytes(char delimiter) {
        const auto position = m_data.find(delimiter, m_read_offset);
        if (position == std::string::npos) {
            Bytes rest = m_data.substr(m_read_offset);
            m_data.clear();
            m_read_offset = 0;
            m_last_op_was_read = true;
            return {rest, streams::IoEof()};
        }
        const std::size_t end = position + 1;
        Bytes part = m_data.substr(m_read_offset, end - m_read_offset);
        m_read_offset = end;
        m_last_op_was_read = true;
        return {part, {}};
    }
    /// 对应 `Buffer.ReadString(delim)`。
    std::pair<Bytes, ErrorPtr> ReadString(char delimiter) { return ReadBytes(delimiter); }

    /// 对应 `Buffer.UnreadByte`：只在**上一次是成功读**时有效。
    ErrorPtr UnreadByte() {
        if (!m_last_op_was_read || m_read_offset == 0) {
            return NewError("bytes.Buffer: UnreadByte: previous operation was not a successful read");
        }
        --m_read_offset;
        m_last_op_was_read = false;
        return {};
    }
    /// 对应 `Buffer.UnreadRune`（Go 要求上一次是 `ReadRune`；这里按"可回退一个 rune"实现）。
    ErrorPtr UnreadRune() {
        if (!m_last_op_was_read || m_read_offset == 0) {
            return NewError("bytes.Buffer: UnreadRune: previous operation was not a successful read");
        }
        std::size_t begin = m_read_offset - 1;
        while (begin > 0 && (static_cast<unsigned char>(m_data[begin]) & 0xC0U) == 0x80U) --begin;
        m_read_offset = begin;
        m_last_op_was_read = false;
        return {};
    }

    // ---- 与 streams 的互操作 ----
    /// 对应 `Buffer.ReadFrom`：把 `source` 的内容全部读入缓冲（返回读入字节数）。
    streams::ReadResult ReadFrom(streams::Reader& source) override {
        char chunk[4096];
        streams::ReadResult result;
        while (true) {
            const streams::ReadResult step = source.Read(chunk, sizeof(chunk));
            if (step.count > 0) {
                m_data.append(chunk, step.count);
                result.count += step.count;
            }
            if (step.error) {
                if (Is(step.error, streams::IoEof())) return result;      // EOF 归一化为成功
                result.error = step.error;
                return result;
            }
        }
    }
    /// 对应 `Buffer.WriteTo`：把**未读部分**写给 `destination`。
    streams::WriteResult WriteTo(streams::Writer& destination) override {
        const std::size_t available = m_data.size() - m_read_offset;
        if (available == 0) return streams::WriteResult{0, {}};
        const streams::WriteResult written =
            destination.Write(m_data.data() + m_read_offset, available);
        m_read_offset += written.count;
        return written;
    }
    /// 对应 `Buffer.Seek`（Go 1.20+ 的 `bytes.Buffer` 未实现 Seeker；这里提供便于与 streams 组合）。
    streams::SeekResult Seek(std::int64_t offset, int whence) override {
        std::int64_t target = 0;
        if (whence == streams::kSeekStart) target = offset;
        else if (whence == streams::kSeekCurrent) target = static_cast<std::int64_t>(m_read_offset) + offset;
        else if (whence == streams::kSeekEnd) {
            target = static_cast<std::int64_t>(m_data.size()) + offset;
        } else {
            return streams::SeekResult{0, NewError("bytes.Buffer: invalid whence")};
        }
        if (target < 0) return streams::SeekResult{0, NewError("bytes.Buffer: negative position")};
        m_read_offset = static_cast<std::size_t>(target);
        m_last_op_was_read = false;
        return streams::SeekResult{target, {}};
    }

    // ---- 容量与状态 ----
    /// 对应 `Buffer.Grow(n)`：保证还能再写 `n` 字节而不重新分配。
    void Grow(std::size_t extra) { m_data.reserve(m_data.size() + extra); }
    /// 对应 `Buffer.Len`：**未读**字节数。
    std::size_t Len() const noexcept { return m_data.size() - m_read_offset; }
    /// 对应 `Buffer.Cap`。
    std::size_t Cap() const noexcept { return m_data.capacity(); }
    /// 对应 `Buffer.Bytes`：返回未读部分（副本）。
    Bytes Bytes_() const { return m_data.substr(m_read_offset); }
    /// 对应 `Buffer.String`：返回未读部分。
    std::string String() const { return m_data.substr(m_read_offset); }
    /// 返回**全部**内容（含已读部分；便于查看缓冲全貌，Go 无对应方法）。
    const std::string& Raw() const noexcept { return m_data; }
    /// 对应 `Buffer.Reset`：清空但保留容量。
    void Reset() {
        m_data.clear();
        m_read_offset = 0;
        m_last_op_was_read = false;
    }
    /// 对应 `Buffer.Truncate(n)`：只保留前 `n` 字节（必须 `n <= Cap`）。
    ErrorPtr Truncate(std::size_t n) {
        if (n > m_data.size()) {
            return NewError("bytes.Buffer: truncation out of range");
        }
        m_data.resize(n);
        if (m_read_offset > n) m_read_offset = n;
        return {};
    }

private:
    std::string m_data;
    std::size_t m_read_offset{0};
    bool m_last_op_was_read{false};
};

// ============================================================================
// Reader（对应 bytes.Reader）
// ============================================================================

/**
 * @brief 对应 `bytes.Reader`：只读、可定位的字节读取器。
 */
class Reader final : public streams::Reader,
                     public streams::ReaderAt,
                     public streams::Seeker,
                     public streams::ByteReader,
                     public streams::RuneReader {
public:
    explicit Reader(std::string_view data) : m_data(data) {}

    /// 对应 `Reader.Read`。
    streams::ReadResult Read(char* out, std::size_t size) override {
        if (m_offset >= m_data.size()) return streams::ReadResult{0, streams::IoEof()};
        const std::size_t available = m_data.size() - m_offset;
        const std::size_t take = size < available ? size : available;
        std::memcpy(out, m_data.data() + m_offset, take);
        m_offset += take;
        return streams::ReadResult{take, {}};
    }
    /// 对应 `Reader.ReadAt`（不改变读游标）。
    streams::ReadResult ReadAt(char* out, std::size_t size, std::int64_t offset) override {
        if (offset < 0 || static_cast<std::size_t>(offset) >= m_data.size()) {
            return streams::ReadResult{0, streams::IoEof()};
        }
        const std::size_t position = static_cast<std::size_t>(offset);
        const std::size_t available = m_data.size() - position;
        const std::size_t take = size < available ? size : available;
        std::memcpy(out, m_data.data() + position, take);
        return streams::ReadResult{take, {}};
    }
    /// 对应 `Reader.ReadByte`。
    streams::ByteResult ReadByte() override {
        if (m_offset >= m_data.size()) return streams::ByteResult{0, streams::IoEof()};
        return streams::ByteResult{m_data[m_offset++], {}};
    }
    /// 对应 `Reader.ReadRune`。
    streams::RuneResult ReadRune() override {
        if (m_offset >= m_data.size()) return streams::RuneResult{0, 0, streams::IoEof()};
        const auto decoded = go2cpp::DecodeRune(m_data.substr(m_offset));
        m_offset += decoded.second;
        return streams::RuneResult{decoded.first, decoded.second, {}};
    }
    /// 对应 `Reader.UnreadByte`。
    ErrorPtr UnreadByte() {
        if (m_offset == 0) {
            return NewError("bytes.Reader: UnreadByte: previous operation was not a successful read");
        }
        --m_offset;
        return {};
    }
    /// 对应 `Reader.Seek`。
    streams::SeekResult Seek(std::int64_t offset, int whence) override {
        std::int64_t target = 0;
        if (whence == streams::kSeekStart) target = offset;
        else if (whence == streams::kSeekCurrent) target = static_cast<std::int64_t>(m_offset) + offset;
        else if (whence == streams::kSeekEnd) {
            target = static_cast<std::int64_t>(m_data.size()) + offset;
        } else {
            return streams::SeekResult{0, NewError("bytes.Reader: invalid whence")};
        }
        if (target < 0) return streams::SeekResult{0, NewError("bytes.Reader: negative position")};
        m_offset = static_cast<std::size_t>(target);
        return streams::SeekResult{target, {}};
    }
    /// 对应 `Reader.Len`：剩余未读字节数。
    std::size_t Len() const noexcept { return m_data.size() - m_offset; }
    /// 对应 `Reader.Size`：底层数据总长度。
    std::size_t Size() const noexcept { return m_data.size(); }

private:
    std::string_view m_data;
    std::size_t m_offset{0};
};

}  // namespace go2cpp::bytes
