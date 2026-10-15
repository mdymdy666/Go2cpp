/**
 * @file bytes.hpp
 * @brief Go `bytes` 包的 C++ 对应实现 —— **只保留 C++ 标准库/本项目已有部分之外的东西**。
 *
 * @details 为什么这个模块很小（2026-10-15 按使用者反馈修订）：
 *   Go 的 `bytes` 与 `strings` 是「两个类型（`[]byte` / `string`）上的同一套算法」，所以 Go 需要两份 API；
 *   而 C++ 里 `std::string` 本身就是字节序列，二者**是同一个类型** ——
 *   把 `bytes.Contains`/`bytes.Split`/`bytes.Trim*` 之类再转发一遍，只会得到「同一份实现的第二个名字」，
 *   没有任何价值。因此本模块**只提供 Go 特有的 `Buffer`**（读写游标分离、`Next`/`ReadBytes`/
 *   `UnreadByte`/`Truncate`/`Grow`/`ReadFrom`/`WriteTo` 这些语义在 C++ 里没有直接对应物）。
 *
 *   被删除的内容与它们的替代写法（**不需要封装**）：
 *
 *   | 已删除的 Go API | 直接替代 |
 *   |---|---|
 *   | `bytes.Equal` / `bytes.Compare` | `a == b` / `a.compare(b)`（`std::string` 原生） |
 *   | `bytes.Contains` / `ContainsAny` / `ContainsRune` / `HasPrefix` / `HasSuffix` | `go2cpp/strings.hpp` 的同名函数 |
 *   | `bytes.Index*` / `LastIndex*` / `Count` | 同上 |
 *   | `bytes.Split` / `SplitN` / `SplitAfter` / `Fields` / `Join` | 同上（参数传 `std::string` 即可） |
 *   | `bytes.Trim*` / `Replace*` / `Repeat` / `ToUpper` / `ToLower` / `EqualFold` | 同上 |
 *   | `bytes.NewReader(data)` / `bytes.Reader` | `go2cpp::streams::StringReader`（`go2cpp/streams.hpp`，功能覆盖 Read/ReadAt/Seek/ReadByte/ReadRune） |
 *   | `bytes.Buffer` | **保留在本模块**（下述 `Buffer` 类） |
 *
 *   语义对照来源：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/bytes/`（Go 1.23.0，BSD-3-Clause），
 *   **独立实现**。已对齐的非显然语义：
 *     - `Buffer.Read` 在**缓冲为空**时返回 `io.EOF`（不是 0 字节且无错误）；
 *     - `Buffer.Next(n)`：`n > Len()` 时返回剩余全部并**清空**；缓冲为空时返回空且**不带错误**；
 *     - `Buffer.ReadBytes`/`ReadString`：**包含分隔符**；未找到时返回剩余全部并带 `io.EOF`；
 *     - `Buffer.UnreadByte`/`UnreadRune`：只在**上一次操作是成功读**时有效，否则报错；
 *     - `Buffer.Truncate(n)`：`n` 超过当前长度时报错；`Buffer.Grow(n)`：保证还能再写 `n` 字节不重新分配。
 *
 * @note 边界：`Buffer.ReadFrom` 的预分配策略比 Go 简单（未对齐 Go 的 `MinRead` 优化）；
 *       `Bytes()` 因与 C++ 别名 `Bytes` 冲突，改名为 `Bytes_()`（`String()` 语义与 Go 相同）。
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

namespace go2cpp::bytes {

/// 对应 Go 的 `[]byte`（C++ 里字节序列就是 `std::string`；保留此别名只为转译时语义清晰）。
using Bytes = std::string;

/**
 * @brief 对应 `bytes.Buffer`：可读可写的字节缓冲，同时实现 `streams` 的读写接口。
 * @details 读游标与写位置分离：`Read` 从读游标推进，`Write` 追加到末尾（与 Go 一致）。
 *          继承 `streams::Reader`/`Writer`/… 是为了与 IOCopy 家族（`streams::Copy`/`ReadAll`/`TeeReader` 等）
 *          直接互操作，而不是重新发明一套接口。
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
        m_last_op_was_read = false;
        return streams::WriteResult{size, {}};
    }
    /// 对应 `Buffer.WriteString`。
    streams::WriteResult WriteString(std::string_view text) override {
        m_data.append(text);
        m_last_op_was_read = false;
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
        AppendRune(m_data, rune);
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
        const auto decoded = DecodeRune(std::string_view(m_data).substr(m_read_offset));
        m_read_offset += decoded.second;
        m_last_op_was_read = true;
        return streams::RuneResult{decoded.first, decoded.second, {}};
    }
    /**
     * @brief 对应 `Buffer.Next(n)`。
     * @details `n > Len()` 时返回剩余全部并清空缓冲；缓冲为空时返回空且**不带错误**（与 Go 一致）。
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
            return NewError(
                "bytes.Buffer: UnreadByte: previous operation was not a successful read");
        }
        --m_read_offset;
        m_last_op_was_read = false;
        return {};
    }
    /// 对应 `Buffer.UnreadRune`：回退一个 UTF-8 rune（要求上一次是成功读）。
    ErrorPtr UnreadRune() {
        if (!m_last_op_was_read || m_read_offset == 0) {
            return NewError(
                "bytes.Buffer: UnreadRune: previous operation was not a successful read");
        }
        std::size_t begin = m_read_offset - 1;
        while (begin > 0 && (static_cast<unsigned char>(m_data[begin]) & 0xC0U) == 0x80U) --begin;
        m_read_offset = begin;
        m_last_op_was_read = false;
        return {};
    }

    // ---- 与 streams 的互操作 ----
    /// 对应 `Buffer.ReadFrom`：把 `source` 的内容全部读入缓冲（`EOF` 归一化为成功）。
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
                if (Is(step.error, streams::IoEof())) return result;
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
    /// 对应 `Buffer.Seek`（Go 的 `bytes.Buffer` 没有该方法；提供它是为了与 `streams::Seeker` 组合）。
    streams::SeekResult Seek(std::int64_t offset, int whence) override {
        std::int64_t target = 0;
        if (whence == streams::kSeekStart) {
            target = offset;
        } else if (whence == streams::kSeekCurrent) {
            target = static_cast<std::int64_t>(m_read_offset) + offset;
        } else if (whence == streams::kSeekEnd) {
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
    /// 对应 `Buffer.Bytes`（因与别名 `Bytes` 冲突而改名）：返回未读部分的副本。
    Bytes Bytes_() const { return m_data.substr(m_read_offset); }
    /// 对应 `Buffer.String`：返回未读部分。
    std::string String() const { return m_data.substr(m_read_offset); }
    /// 返回**全部**内容（含已读部分；Go 无对应方法，便于查看缓冲全貌）。
    const std::string& Raw() const noexcept { return m_data; }
    /// 对应 `Buffer.Reset`：清空但保留容量。
    void Reset() {
        m_data.clear();
        m_read_offset = 0;
        m_last_op_was_read = false;
    }
    /// 对应 `Buffer.Truncate(n)`：只保留前 `n` 字节。
    ErrorPtr Truncate(std::size_t n) {
        if (n > m_data.size()) return NewError("bytes.Buffer: truncation out of range");
        m_data.resize(n);
        if (m_read_offset > n) m_read_offset = n;
        return {};
    }

private:
    std::string m_data;
    std::size_t m_read_offset{0};
    bool m_last_op_was_read{false};
};

}  // namespace go2cpp::bytes
