/**
 * @file io.go.hpp
 * @brief Go `io` 包的 C++ 对应物（Reader / Writer / Copy / ReadFull / WriteString）。
 *
 * ## 定位
 * `go2cpp::streams`（`go2cpp/tools/streams.hpp`）已经完整实现 Go `io` 的核心：
 * `Reader`/`Writer`/`Closer`/`Seeker`、`Copy`/`CopyBuffer`/`CopyN`、
 * `ReadFull`/`ReadAtLeast`/`ReadAll`、`LimitReader`/`MultiReader`/`TeeReader`、
 * `Discard`、`WriteString`。本文件只做两件事：
 *   1. 用 `go::io::` 命名空间透出（避免与 `go2cpp::io` 的 IOManager 重名）；
 *   2. 提供 `net.Conn` → `io.Reader/Writer` 的适配器（JinShan relay 的
 *      `io.CopyBuffer(conn, peer)` 必须能直接落地）。
 *
 * ```go
 * io.Copy(dst, src)
 * n, err := io.ReadFull(conn, buf)
 * io.WriteString(conn, "READY\n")
 * ```
 *
 * 对应 C++：
 *
 * ```cpp
 * go::io::Copy(dst, src);
 * auto r = go::io::ReadFull(*conn, buf, sizeof(buf));
 * go::io::WriteString(*conn, "READY\n");
 * ```
 *
 * ## 与 Go 的差异
 * - Go 的 `io.EOF` 是唯一哨兵；本项目 `go::ErrEOF()` 与 `streams::IoEof()`
 *   是两个实例，适配器内部已转换，`io.Copy` 能正确识别。
 * - `io.Reader` 返回 `ReadResult{count, error}`，`error` 为 `ErrorPtr`。
 * - `io.Copy` 的目标/源是抽象基类引用，不拥有对象；生命周期由调用方保证。
 */

#pragma once

#include "go2cpp/go/error.go.hpp"
#include "go2cpp/go/net.go.hpp"
#include "go2cpp/tools/streams.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace go2cpp::go::io {

using ::go2cpp::streams::ByteReader;
using ::go2cpp::streams::ByteResult;
using ::go2cpp::streams::ByteWriter;
using ::go2cpp::streams::Closer;
using ::go2cpp::streams::DiscardWriter;
using ::go2cpp::streams::LimitedReader;
using ::go2cpp::streams::MultiReader;
using ::go2cpp::streams::MultiWriter;
using ::go2cpp::streams::ReadResult;
using ::go2cpp::streams::Reader;
using ::go2cpp::streams::ReaderAt;
using ::go2cpp::streams::ReaderFrom;
using ::go2cpp::streams::Seeker;
using ::go2cpp::streams::StringWriter;
using ::go2cpp::streams::TeeReader;
using ::go2cpp::streams::WriteResult;
using ::go2cpp::streams::Writer;
using ::go2cpp::streams::WriterAt;
using ::go2cpp::streams::WriterTo;

/** @brief 对应 `io.EOF`（与 `streams::IoEof()` 同一实例）。 */
inline error ErrEOF() {
    return ::go2cpp::streams::IoEof();
}

/** @brief 判断 err 是否为 EOF；兼容 `go::ErrEOF()` 与 `streams::IoEof()`。 */
inline bool IsEOF(const error& err) {
    return ::go2cpp::Is(err, ::go2cpp::streams::IoEof()) ||
           ::go2cpp::go::Is(err, ::go2cpp::go::ErrEOF());
}

/** @brief 对应 `io.ReadFull(r, buf)`。 */
inline ReadResult ReadFull(Reader& reader, void* buffer, std::size_t size) {
    return ::go2cpp::streams::ReadFull(reader, static_cast<char*>(buffer), size);
}

/** @brief 对应 `io.ReadAtLeast(r, buf, min)`。 */
inline ReadResult ReadAtLeast(Reader& reader, void* buffer, std::size_t size,
                              std::size_t min) {
    return ::go2cpp::streams::ReadAtLeast(reader, static_cast<char*>(buffer), size,
                                          min);
}

/** @brief 对应 `io.ReadAll(r)`，返回 `(bytes, error)`。 */
inline std::pair<std::string, error> ReadAll(Reader& reader) {
    return ::go2cpp::streams::ReadAll(reader);
}

/** @brief 对应 `io.Copy(dst, src)`，返回 `(written, error)`。 */
inline ReadResult Copy(Writer& destination, Reader& source) {
    return ::go2cpp::streams::Copy(destination, source);
}

/** @brief 对应 `io.CopyBuffer(dst, src, buf)`。 */
inline ReadResult CopyBuffer(Writer& destination, Reader& source, void* buffer,
                             std::size_t size) {
    return ::go2cpp::streams::CopyBuffer(destination, source,
                                         static_cast<char*>(buffer), size);
}

/** @brief 对应 `io.CopyN(dst, src, n)`。 */
inline ReadResult CopyN(Writer& destination, Reader& source, std::int64_t n) {
    return ::go2cpp::streams::CopyN(destination, source, n);
}

/** @brief 对应 `io.WriteString(w, s)`。 */
inline WriteResult WriteString(Writer& writer, std::string_view text) {
    return ::go2cpp::streams::WriteString(writer, text);
}

/** @brief 对应 `io.LimitReader(r, n)`。 */
inline LimitedReader LimitReader(Reader& source, std::int64_t limit) {
    return ::go2cpp::streams::LimitReader(source, limit);
}

/** @brief 对应 `io.Discard`。 */
inline Writer& Discard() {
    return ::go2cpp::streams::Discard();
}

/**
 * @brief 把 `net.Conn` 适配成 `io.Reader`。
 * @details 共享持有 Conn（`shared_ptr`），因此 Reader 存活期间连接不会析构。
 *          EOF 会被转换为 `streams::IoEof()`，保证 `io.Copy` 正确结束。
 */
class ConnReader final : public Reader {
public:
    explicit ConnReader(::go2cpp::go::net::ConnPtr conn) : m_conn(std::move(conn)) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        if (!m_conn) {
            return {0, ::go2cpp::streams::IoEof()};
        }
        const auto result = m_conn->Read(buffer, size);
        if (!result.err) {
            return {result.n, {}};
        }
        if (::go2cpp::go::Is(result.err, ::go2cpp::go::ErrEOF())) {
            return {result.n, ::go2cpp::streams::IoEof()};
        }
        return {result.n, result.err};
    }

    const ::go2cpp::go::net::ConnPtr& Conn() const noexcept { return m_conn; }

private:
    ::go2cpp::go::net::ConnPtr m_conn;
};

/**
 * @brief 把 `net.Conn` 适配成 `io.Writer`。
 * @details Go 的 `net.Conn.Write` 与 `io.Writer.Write` 语义一致（可能部分写）。
 */
class ConnWriter final : public Writer {
public:
    explicit ConnWriter(::go2cpp::go::net::ConnPtr conn) : m_conn(std::move(conn)) {}

    WriteResult Write(const char* data, std::size_t size) override {
        if (!m_conn) {
            return {0, ErrClosed()};
        }
        const auto result = m_conn->Write(data, size);
        return {result.n, result.err};
    }

    const ::go2cpp::go::net::ConnPtr& Conn() const noexcept { return m_conn; }

private:
    ::go2cpp::go::net::ConnPtr m_conn;
};

/**
 * @brief 同时实现 `Reader` / `Writer` / `Closer` 的连接适配器。
 * @details 便于 `io.Copy(conn_io, peer_io)` 这种双向中继场景。
 */
class ConnIO final : public Reader, public Writer, public Closer {
public:
    explicit ConnIO(::go2cpp::go::net::ConnPtr conn) : m_conn(std::move(conn)) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        if (!m_conn) {
            return {0, ::go2cpp::streams::IoEof()};
        }
        const auto result = m_conn->Read(buffer, size);
        if (!result.err) {
            return {result.n, {}};
        }
        if (::go2cpp::go::Is(result.err, ::go2cpp::go::ErrEOF())) {
            return {result.n, ::go2cpp::streams::IoEof()};
        }
        return {result.n, result.err};
    }

    WriteResult Write(const char* data, std::size_t size) override {
        if (!m_conn) {
            return {0, ErrClosed()};
        }
        const auto result = m_conn->Write(data, size);
        return {result.n, result.err};
    }

    error Close() override {
        if (!m_conn) {
            return {};
        }
        return m_conn->Close();
    }

private:
    ::go2cpp::go::net::ConnPtr m_conn;
};

}  // namespace go2cpp::go::io
