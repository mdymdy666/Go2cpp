/**
 * @file test_bufio.cpp
 * @brief `go2cpp::bufio` 模块测试：语义 / 边界 / 随机 / 真实场景。
 *
 * ## 覆盖策略（对照 PROJECT_RULES 第 3 节）
 *
 * - **随机测试**：`RandomReadLine` / `RandomRead` / `RandomPeekDiscard` /
 *   `RandomReadString` / `RandomScanner`，固定种子（可复现），随机长度、
 *   随机分块、随机分隔符、随机缓冲尺寸。
 * - **边界**：空输入、单字节、无分隔符、分隔符在首/尾、超长行、`0x00`
 *   分隔符、0 字节读、零进展源（防挂死）、底层注入错误。
 * - **真实场景 6 个**（覆盖 Reader/Writer/Scanner 全部 API）：
 *   1. 逐行读配置文件（注释 + 空行 + 7 字节分块读）
 *   2. 读 HTTP 响应（`Peek` 零拷贝找头，`Discard` 跳头，按
 *      `Content-Length` 精确读 body，验证不越读）
 *   3. 写日志（`Writer` 批量写 1 万行 → `Flush` → 回读校验；
 *      另外验证「不 Flush 就析构 → 数据确实没落地」）
 *   4. `Scanner` 扫 10 万行（行数 + 首末行 + 长度校验和 + 吞吐）
 *   5. 分块传输（1 MiB 随机分块，`Read` 循环到 EOF，校验总字节与内容）
 *   6. 慢速/短写对端（每次最多写 512 字节，256 KiB 数据完整落地）
 *
 * 压测（系统调用次数、吞吐）在 `test_bufio_stress.cpp`，不在本文件。
 *
 * ## 运行
 * ```
 * ctest --test-dir build-quality -R go2cpp_bufio_test --output-on-failure
 * # 或直接：
 * ./build-quality/go2cpp_bufio_test
 * ```
 */
#include "go2cpp/bufio/bufio.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace streams = go2cpp::streams;
using go2cpp::ErrorPtr;
using go2cpp::Is;
using go2cpp::NewError;
using go2cpp::bufio::AsBytes;
using go2cpp::bufio::AsStringView;
using go2cpp::bufio::ByteSpan;
using go2cpp::bufio::ErrBufferFull;
using go2cpp::bufio::ErrInvalidAdvance;
using go2cpp::bufio::ErrTooLong;
using go2cpp::bufio::Reader;
using go2cpp::bufio::Scanner;
using go2cpp::bufio::SplitResult;
using go2cpp::bufio::ToString;
using go2cpp::bufio::Writer;

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "[bufio] FAILED %s:%d: %s\n", file, line, expression);
    }
}

#define CHECK(condition) CheckImpl((condition), #condition, __FILE__, __LINE__)

void Announce(const char* name) {
    std::printf("[bufio] %s\n", name);
    std::fflush(stdout);
}

bool IsEof(const ErrorPtr& error) { return Is(error, streams::IoEof()); }

/// 打印实测吞吐，便于在 ctest 输出里核对（不是断言）。
void ReportRate(const char* label, std::size_t bytes, double seconds) {
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const double rate = seconds > 0.0 ? megabytes / seconds : 0.0;
    std::printf("[bufio] %s: %zu 字节, %.3f s, %.1f MiB/s\n", label, bytes,
                seconds, rate);
}

// ===========================================================================
// 测试桩：底层 Reader / Writer 的最小实现
// ===========================================================================

/**
 * @brief 内存 Reader；可限制每次返回的字节数或按计划循环分块。
 * @details 每调用一次 `Read` 计一次 `calls`——它是「系统调用次数」的逻辑
 *          等价物（压测里换成真实 `::read`，见 test_bufio_stress.cpp）。
 */
struct MemorySource {
    std::string data;
    std::size_t position{0};
    std::uint64_t calls{0};
    std::size_t max_chunk{0};        ///< 0 表示不限制每次读取量
    std::vector<std::size_t> plan;   ///< 非空时按计划循环分块
    std::size_t plan_position{0};

    explicit MemorySource(std::string text = std::string{}, std::size_t chunk = 0)
        : data(std::move(text)), max_chunk(chunk) {}

    streams::ReadResult Read(char* buffer, std::size_t size) {
        ++calls;
        if (position >= data.size()) {
            return streams::ReadResult{0, streams::IoEof()};
        }
        std::size_t take = std::min(size, data.size() - position);
        if (!plan.empty()) {
            const std::size_t planned = plan[plan_position % plan.size()];
            ++plan_position;
            take = std::min(take, planned);
        } else if (max_chunk > 0) {
            take = std::min(take, max_chunk);
        }
        if (take == 0) {
            return streams::ReadResult{0, {}};
        }
        std::memcpy(buffer, data.data() + position, take);
        position += take;
        return streams::ReadResult{take, {}};
    }
};

/// 谎报「读到的比请求多」的违约 Reader：ReadFrom 不能越界读栈缓冲。
struct OverreportingSource {
    std::uint64_t calls{0};
    streams::ReadResult Read(char*, std::size_t size) {
        ++calls;
        return streams::ReadResult{size + 1, {}};
    }
};

/// 永远返回 (0, nil) 的违约 Reader：验证防挂死保护。
struct ZeroSource {
    std::uint64_t calls{0};
    streams::ReadResult Read(char*, std::size_t) {
        ++calls;
        return streams::ReadResult{0, {}};
    }
};

/// 读到 `fail_at` 字节后注入错误，用于验证错误传播。
struct FailingSource {
    std::string data;
    std::size_t position{0};
    std::size_t fail_at{0};

    streams::ReadResult Read(char* buffer, std::size_t size) {
        if (position >= fail_at) {
            return streams::ReadResult{0, NewError("source: injected failure")};
        }
        const std::size_t take =
            std::min(std::min(size, data.size() - position), fail_at - position);
        std::memcpy(buffer, data.data() + position, take);
        position += take;
        return streams::ReadResult{take, {}};
    }
};

/// 内存 Writer；`max_write` 模拟「部分写」。
struct MemorySink {
    std::string data;
    std::uint64_t calls{0};
    std::size_t max_write{0};

    streams::WriteResult Write(const char* bytes, std::size_t size) {
        ++calls;
        if (max_write > 0) {
            size = std::min(size, max_write);
        }
        data.append(bytes, size);
        return streams::WriteResult{size, {}};
    }
};

/// 写到 `fail_after` 字节后返回错误。
struct FailingSink {
    std::string data;
    std::uint64_t calls{0};
    std::size_t fail_after{0};

    streams::WriteResult Write(const char* bytes, std::size_t size) {
        ++calls;
        if (data.size() >= fail_after) {
            return streams::WriteResult{0, NewError("sink: injected failure")};
        }
        const std::size_t take = std::min(size, fail_after - data.size());
        data.append(bytes, take);
        return streams::WriteResult{take, {}};
    }
};

/// 永远 (0, nil) 的违约 Writer：验证 Flush 不会死循环。
struct ZeroWriteSink {
    std::uint64_t calls{0};
    streams::WriteResult Write(const char*, std::size_t) {
        ++calls;
        return streams::WriteResult{0, {}};
    }
};

/// 谎报「写得比给的多」的违约 Writer：验证不会下溢/越界。
struct GreedySink {
    streams::WriteResult Write(const char*, std::size_t size) {
        return streams::WriteResult{size + 1, {}};
    }
};

/// 真实文件 Reader（每次 Read = 一次 `read(2)`）。
class FileSource {
public:
    explicit FileSource(const std::string& path)
        : m_descriptor(::open(path.c_str(), O_RDONLY)) {}
    ~FileSource() {
        if (m_descriptor >= 0) {
            ::close(m_descriptor);
        }
    }
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    bool valid() const { return m_descriptor >= 0; }

    streams::ReadResult Read(char* buffer, std::size_t size) {
        ++calls;
        const ssize_t received = ::read(m_descriptor, buffer, size);
        if (received < 0) {
            return streams::ReadResult{0, NewError("file: read failed")};
        }
        if (received == 0) {
            return streams::ReadResult{0, streams::IoEof()};
        }
        return streams::ReadResult{static_cast<std::size_t>(received), {}};
    }

    std::uint64_t calls{0};

private:
    int m_descriptor{-1};
};

/// 真实文件 Writer（每次 Write = 一次 `write(2)`）。
class FileSink {
public:
    explicit FileSink(int descriptor) : m_descriptor(descriptor) {}
    ~FileSink() {
        if (m_descriptor >= 0) {
            ::close(m_descriptor);
        }
    }
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    streams::WriteResult Write(const char* bytes, std::size_t size) {
        ++calls;
        const ssize_t written = ::write(m_descriptor, bytes, size);
        if (written < 0) {
            return streams::WriteResult{0, NewError("file: write failed")};
        }
        return streams::WriteResult{static_cast<std::size_t>(written), {}};
    }

    std::uint64_t calls{0};

private:
    int m_descriptor{-1};
};

// ===========================================================================
// 通用小工具
// ===========================================================================

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string RandomBytes(std::mt19937& rng, std::size_t size) {
    std::string data(size, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        data[index] = static_cast<char>(rng() & 0xFFU);
    }
    return data;
}

std::string ReadWholeFile(const std::string& path) {
    FileSource source(path);
    if (!source.valid()) {
        return std::string{};
    }
    std::string content;
    char buffer[4096];
    while (true) {
        const streams::ReadResult result = source.Read(buffer, sizeof(buffer));
        if (result.count > 0) {
            content.append(buffer, result.count);
        }
        if (result.error) {
            break;
        }
    }
    return content;
}

/// 参考实现：从 `position` 起读到并包含 `delim`；找不到则返回剩余内容。
std::string ReferenceReadString(const std::string& data, std::size_t& position,
                                char delim, bool& reached_end) {
    const std::size_t found = data.find(delim, position);
    if (found == std::string::npos) {
        std::string rest = data.substr(position);
        position = data.size();
        reached_end = true;
        return rest;
    }
    std::string text = data.substr(position, found - position + 1);
    position = found + 1;
    reached_end = false;
    return text;
}

// ===========================================================================
// Reader：基础语义
// ===========================================================================

void TestReaderBasics() {
    Announce("Reader 基础语义（Size/Buffered/Read/ReadByte/Err）");

    {
        MemorySource source("0123456789");
        Reader<MemorySource> reader(source, 32);
        CHECK(reader.Size() == 32);
        CHECK(reader.Buffered() == 0);
        CHECK(source.calls == 0);  // 构造不预读

        std::string buffer(4, '\0');
        CHECK(reader.Read(AsBytes(buffer)) == 4);
        CHECK(buffer == "0123");
        CHECK(source.calls == 1);  // 一次底层读拿到 10 字节
        CHECK(reader.Buffered() == 6);
        CHECK(reader.Read(ByteSpan()) == 0);  // 0 字节请求不触发读
        CHECK(source.calls == 1);
    }

    {
        // 缓冲区为空且请求 >= 缓冲容量：直接读进目标（Go 快路径）
        MemorySource source("abcdef");
        Reader<MemorySource> reader(source, 16);
        std::string direct(40, '\0');
        CHECK(reader.Read(AsBytes(direct)) == 6);
        CHECK(direct.compare(0, 6, "abcdef") == 0);
        CHECK(source.calls == 1);
        CHECK(reader.Buffered() == 0);
        CHECK(reader.Read(AsBytes(direct)) == 0);
        CHECK(IsEof(reader.Err()));
    }

    {
        // 缓冲非空时，大请求只交付缓冲里的数据（Go：n 可以小于 len(p)）
        MemorySource source(std::string(100, 'z'));
        Reader<MemorySource> reader(source, 16);
        CHECK(reader.ReadByte().Value() == 'z');
        std::string buffer(50, '\0');
        CHECK(reader.Read(AsBytes(buffer)) == 15);  // 只剩 15 字节
        CHECK(buffer.compare(0, 15, std::string(15, 'z')) == 0);
    }

    {
        MemorySource source("AB");
        Reader<MemorySource> reader(source, 16);
        const auto first = reader.ReadByte();
        CHECK(first.ok());
        CHECK(first.Value() == 'A');
        const auto second = reader.ReadByte();
        CHECK(second.ok());
        CHECK(second.Value() == 'B');
        const auto third = reader.ReadByte();
        CHECK(!third.ok());
        CHECK(IsEof(third.Err()));
        const auto fourth = reader.ReadByte();
        CHECK(!fourth.ok());
        CHECK(IsEof(fourth.Err()));  // 粘性 EOF
    }

    {
        // 小缓冲跨填充逐字节读（每块 3 字节，缓冲 16）
        const std::string payload = "abcdefghijklmnopqrstuvwxyz0123456789";
        MemorySource source(payload, 3);
        Reader<MemorySource> reader(source, 16);
        std::string collected;
        while (true) {
            const auto byte = reader.ReadByte();
            if (!byte.ok()) {
                break;
            }
            collected.push_back(static_cast<char>(byte.Value()));
        }
        CHECK(collected == payload);
        CHECK(IsEof(reader.Err()));
    }

    {
        // 与 streams 契约互操作，并且可以嵌套
        MemorySource source("stacked");
        Reader<MemorySource> inner(source, 4);
        Reader<Reader<MemorySource>> outer(inner, 8);
        std::string collected;
        char buffer[3];
        while (true) {
            const streams::ReadResult result = outer.Read(buffer, sizeof(buffer));
            if (result.count == 0) {
                break;
            }
            collected.append(buffer, result.count);
        }
        CHECK(collected == "stacked");
        CHECK(IsEof(inner.Err()));
    }

    {
        // 尺寸钳制：0 / 1 都会提升到 16（Go 的 minReadBufferSize）
        MemorySource source("x");
        Reader<MemorySource> reader(source, 0);
        CHECK(reader.Size() == 16);
    }

    {
        // 默认构造使用 4096
        MemorySource source("x");
        Reader<MemorySource> reader(source);
        CHECK(reader.Size() == go2cpp::bufio::kDefaultBufferSize);
    }
}

// ===========================================================================
// Reader：Peek（零拷贝）
// ===========================================================================

void TestReaderPeek() {
    Announce("Reader::Peek 零拷贝 / ErrBufferFull / 短数据 + EOF");

    {
        MemorySource source("hello world");
        Reader<MemorySource> reader(source, 16);
        const auto first = reader.Peek(5);
        CHECK(first.ok());
        CHECK(ToString(first.Value()) == "hello");
        CHECK(reader.Buffered() == 11);
        CHECK(source.calls == 1);  // 一次填充

        const auto again = reader.Peek(5);
        CHECK(again.ok());
        CHECK(again.Value().data() == first.Value().data());  // 同一块内存
        CHECK(source.calls == 1);                              // 不再读

        CHECK(reader.Peek(0).ok());
        CHECK(reader.Peek(11).ok());
        CHECK(ToString(reader.Peek(11).Value()) == "hello world");

        // 消费后指针整体后移，但仍指向同一缓冲区
        CHECK(reader.Discard(6) == 6);
        const auto tail = reader.Peek(5);
        CHECK(tail.ok());
        CHECK(ToString(tail.Value()) == "world");
        CHECK(tail.Value().data() == first.Value().data() + 6);

        // 超过容量：直接报 ErrBufferFull，且不预读
        const std::uint64_t calls_before = source.calls;
        const auto too_large = reader.Peek(17);
        CHECK(!too_large.ok());
        CHECK(Is(too_large.Err(), ErrBufferFull()));
        CHECK(source.calls == calls_before);
    }

    {
        // 数据不足且已 EOF：返回「已有部分 + EOF」，不丢数据
        MemorySource source("abc");
        Reader<MemorySource> reader(source, 16);
        const auto partial = reader.Peek(10);
        CHECK(!partial.ok());
        CHECK(IsEof(partial.Err()));
        CHECK(partial.Value().size() == 3);
        CHECK(ToString(partial.Value()) == "abc");
        CHECK(reader.Buffered() == 3);  // Peek 不消费
    }

    {
        // 跨多次填充才凑够 Peek 的字节数（每次 2 字节，缓冲 16）
        MemorySource source("123456789", 2);
        Reader<MemorySource> reader(source, 16);
        const auto peeked = reader.Peek(6);
        CHECK(peeked.ok());
        CHECK(ToString(peeked.Value()) == "123456");
        CHECK(source.calls == 3);  // 2 + 2 + 2
    }
}

// ===========================================================================
// Reader：ReadLine
// ===========================================================================

void TestReaderLine() {
    Announce("Reader::ReadLine（LF/CRLF/空行/末行无换行/超长行）");

    {
        MemorySource source("one\ntwo\r\n\nlast");
        Reader<MemorySource> reader(source, 16);
        const auto line1 = reader.ReadLine();
        CHECK(line1.ok());
        CHECK(line1.Value() == "one");
        const auto line2 = reader.ReadLine();
        CHECK(line2.ok());
        CHECK(line2.Value() == "two");  // CRLF 去掉 '\r'
        const auto line3 = reader.ReadLine();
        CHECK(line3.ok());
        CHECK(line3.Value().empty());  // 空行
        const auto line4 = reader.ReadLine();
        CHECK(line4.ok());
        CHECK(line4.Value() == "last");  // 末行无换行
        const auto line5 = reader.ReadLine();
        CHECK(!line5.ok());
        CHECK(IsEof(line5.Err()));
    }

    {
        // 恰好等于缓冲容量的行是合法的
        const std::string exact(16, 'x');
        MemorySource source(exact + "\n");
        Reader<MemorySource> reader(source, 16);
        const auto line = reader.ReadLine();
        CHECK(line.ok());
        CHECK(line.Value() == exact);
    }

    {
        // 超过缓冲容量的行 -> ErrTooLong（带已读到的前缀）
        const std::string too_long(20, 'y');
        MemorySource source(too_long + "\n");
        Reader<MemorySource> reader(source, 16);
        const auto line = reader.ReadLine();
        CHECK(!line.ok());
        CHECK(Is(line.Err(), ErrTooLong()));
        CHECK(line.Value().size() == 16);
    }

    {
        // CRLF 正好跨填充边界（缓冲 16，先读 15 字节）
        MemorySource source("0123456789abcde\r\nnext");
        Reader<MemorySource> reader(source, 16);
        const auto line = reader.ReadLine();
        CHECK(line.ok());
        CHECK(line.Value() == "0123456789abcde");
        const auto next = reader.ReadLine();
        CHECK(next.ok());
        CHECK(next.Value() == "next");
    }

    {
        MemorySource source("");
        Reader<MemorySource> reader(source, 16);
        const auto line = reader.ReadLine();
        CHECK(!line.ok());
        CHECK(IsEof(line.Err()));
    }

    {
        MemorySource source("single");
        Reader<MemorySource> reader(source, 16);
        const auto line = reader.ReadLine();
        CHECK(line.ok());
        CHECK(line.Value() == "single");
        CHECK(!reader.ReadLine().ok());
    }
}

// ===========================================================================
// Reader：ReadString
// ===========================================================================

void TestReaderString() {
    Announce("Reader::ReadString（含分隔符/无分隔符/0x00/无界累积）");

    {
        MemorySource source("a,b,,c");
        Reader<MemorySource> reader(source, 4);
        const auto first = reader.ReadString(',');
        CHECK(first.ok());
        CHECK(first.Value() == "a,");
        const auto second = reader.ReadString(',');
        CHECK(second.ok());
        CHECK(second.Value() == "b,");
        const auto third = reader.ReadString(',');
        CHECK(third.ok());
        CHECK(third.Value() == ",");
        const auto fourth = reader.ReadString(',');
        CHECK(!fourth.ok());
        CHECK(IsEof(fourth.Err()));
        CHECK(fourth.Value() == "c");  // 部分数据 + EOF
    }

    {
        // 分隔符在末尾
        MemorySource source("tail;");
        Reader<MemorySource> reader(source, 16);
        const auto first = reader.ReadString(';');
        CHECK(first.ok());
        CHECK(first.Value() == "tail;");
        const auto second = reader.ReadString(';');
        CHECK(!second.ok());
        CHECK(IsEof(second.Err()));
        CHECK(second.Value().empty());
    }

    {
        // 分隔符在开头
        MemorySource source(";ab");
        Reader<MemorySource> reader(source, 16);
        const auto first = reader.ReadString(';');
        CHECK(first.ok());
        CHECK(first.Value() == ";");
    }

    {
        // 0x00 作为分隔符（二进制协议常见）
        const std::string binary("a\0b", 3);
        MemorySource source(binary);
        Reader<MemorySource> reader(source, 16);
        const auto first = reader.ReadString('\0');
        CHECK(first.ok());
        CHECK(first.Value().size() == 2);
        CHECK(first.Value() == std::string("a\0", 2));
        const auto second = reader.ReadString('\0');
        CHECK(!second.ok());
        CHECK(IsEof(second.Err()));
        CHECK(second.Value() == "b");
    }

    {
        // 长度不受缓冲限制（与 Go 一致）：10000 字节一行 + ';'
        const std::string long_text(10000, 'q');
        MemorySource source(long_text + ";");
        Reader<MemorySource> reader(source, 16);
        const auto text = reader.ReadString(';');
        CHECK(text.ok());
        CHECK(text.Value().size() == 10001);
        CHECK(text.Value().compare(0, 10000, long_text) == 0);
        CHECK(text.Value().back() == ';');
    }
}

// ===========================================================================
// Reader：Discard / 错误路径
// ===========================================================================

void TestReaderDiscardAndFaults() {
    Announce("Reader::Discard / 零进展源 / 底层错误传播");

    {
        MemorySource source("0123456789");
        Reader<MemorySource> reader(source, 4);
        CHECK(reader.Discard(0) == 0);
        CHECK(reader.Discard(3) == 3);  // 跨填充
        const auto byte = reader.ReadByte();
        CHECK(byte.ok());
        CHECK(byte.Value() == '3');
        CHECK(reader.Discard(100) == 6);  // 只剩 6 字节
        CHECK(IsEof(reader.Err()));
        CHECK(reader.Discard(1) == 0);
    }

    {
        // 违约源：连续 100 次 (0, nil) -> IoErrNoProgress，不会挂死
        ZeroSource source;
        Reader<ZeroSource> reader(source, 16);
        const auto byte = reader.ReadByte();
        CHECK(!byte.ok());
        CHECK(Is(byte.Err(), streams::IoErrNoProgress()));
        CHECK(source.calls == 100);
    }

    {
        // 底层错误：先交付已有行，再把错误交出来
        FailingSource source;
        source.data = "line1\nline2\nmore";
        source.fail_at = 6;
        Reader<FailingSource> reader(source, 16);
        const auto first = reader.ReadLine();
        CHECK(first.ok());
        CHECK(first.Value() == "line1");
        const auto second = reader.ReadLine();
        CHECK(!second.ok());
        CHECK(!IsEof(second.Err()));  // 是注入错误而不是 EOF
    }

    {
        // 末行无换行 + 随后注入错误：先交付末行（Go 语义），下一次才报错
        FailingSource source;
        source.data = "abc";
        source.fail_at = 3;
        Reader<FailingSource> reader(source, 16);
        const auto first = reader.ReadLine();
        CHECK(first.ok());
        CHECK(first.Value() == "abc");
        const auto second = reader.ReadLine();
        CHECK(!second.ok());
        CHECK(!IsEof(second.Err()));
    }

    {
        // 底层错误：Read 返回 0，错误通过 Err() 暴露
        FailingSource source;
        source.data = "abc";
        source.fail_at = 0;
        Reader<FailingSource> reader(source, 16);
        std::string buffer(4, '\0');
        CHECK(reader.Read(AsBytes(buffer)) == 0);
        CHECK(reader.Err() != nullptr);
        CHECK(!IsEof(reader.Err()));
    }
}

// ===========================================================================
// Writer
// ===========================================================================

void TestWriterBasics() {
    Announce("Writer 基础语义（自动 flush / Buffered / Available）");

    {
        // 缓冲尺寸下限 16（Go 的 minReadBufferSize）：16 是能用的最小值
        MemorySink sink;
        Writer<MemorySink> writer(sink, 16);
        CHECK(writer.Size() == 16);
        CHECK(writer.Buffered() == 0);
        CHECK(writer.Available() == 16);
        CHECK(writer.WriteString("abc").ok());
        CHECK(writer.Buffered() == 3);
        CHECK(writer.Available() == 13);
        CHECK(sink.data.empty());  // 未 flush 不落盘
        CHECK(writer.WriteByte('d').ok());
        CHECK(writer.Buffered() == 4);
        CHECK(writer.Flush().ok());
        CHECK(sink.data == "abcd");
        CHECK(writer.Buffered() == 0);
        CHECK(sink.calls == 1);
    }

    {
        // 正好填满不自动 flush；再多 1 字节才触发
        MemorySink sink;
        Writer<MemorySink> writer(sink, 16);
        CHECK(writer.WriteString("0123456789abcdef").ok());
        CHECK(sink.calls == 0);
        CHECK(writer.Buffered() == 16);
        CHECK(writer.Available() == 0);
        CHECK(writer.WriteString("9").ok());
        CHECK(sink.data == "0123456789abcdef");
        CHECK(sink.calls == 1);
        CHECK(writer.Buffered() == 1);
        CHECK(writer.Flush().ok());
        CHECK(sink.data == "0123456789abcdef9");
        CHECK(writer.Buffered() == 0);
    }

    {
        // 大块（>= 缓冲容量）且缓冲为空：直接写底层，不经过拷贝
        MemorySink sink;
        Writer<MemorySink> writer(sink, 16);
        std::string large(100, 'x');
        CHECK(writer.Write(AsBytes(large)) == 100);
        CHECK(sink.data == large);
        CHECK(sink.calls == 1);
        CHECK(writer.Buffered() == 0);
    }

    {
        // 空写 / 空 flush 不触碰底层
        MemorySink sink;
        Writer<MemorySink> writer(sink, 16);
        CHECK(writer.Write(ByteSpan()) == 0);
        CHECK(writer.WriteString("").ok());
        CHECK(writer.Flush().ok());
        CHECK(sink.calls == 0);
    }

    {
        // 与 streams 契约互操作
        MemorySink sink;
        Writer<MemorySink> writer(sink, 16);
        const streams::WriteResult result = writer.Write("hi", 2);
        CHECK(result.count == 2);
        CHECK(!result.error);
        CHECK(writer.Flush().ok());
        CHECK(sink.data == "hi");
    }

    {
        // 尺寸钳制到 16
        MemorySink sink;
        Writer<MemorySink> writer(sink, 0);
        CHECK(writer.Size() == 16);
    }
}

void TestWriterPartialAndFaults() {
    Announce("Writer 部分写 / 短写 / 粘性错误 / 违约底层");

    {
        // Flush 必须把缓冲全部写出去：底层每次只收 3 字节
        MemorySink sink;
        sink.max_write = 3;
        Writer<MemorySink> writer(sink, 16);
        for (int index = 0; index < 10; ++index) {
            CHECK(writer.WriteByte(static_cast<std::uint8_t>('a' + index)).ok());
        }
        CHECK(sink.data.empty());
        CHECK(writer.Flush().ok());
        CHECK(sink.data == "abcdefghij");
        CHECK(writer.Buffered() == 0);
        CHECK(sink.calls == 4);  // 3 + 3 + 3 + 1
    }

    {
        // 大块直写 + 部分写：循环补完，数据不丢不乱序
        MemorySink sink;
        sink.max_write = 3;
        Writer<MemorySink> writer(sink, 8);
        std::string large;
        for (int index = 0; index < 20; ++index) {
            large.push_back(static_cast<char>('A' + index));
        }
        CHECK(writer.Write(AsBytes(large)) == large.size());
        CHECK(writer.Flush().ok());
        CHECK(sink.data == large);
        CHECK(sink.calls >= 7);
    }

    {
        // 底层写失败：粘性错误，已写部分保留
        FailingSink sink;
        sink.fail_after = 4;
        Writer<FailingSink> writer(sink, 16);
        CHECK(writer.WriteString("abcdefgh").ok());  // 只进缓冲
        const auto flushed = writer.Flush();
        CHECK(!flushed.ok());
        CHECK(sink.data == "abcd");  // 4 字节落地，其余失败
        CHECK(!writer.WriteByte('x').ok());  // 粘性
        CHECK(!writer.Flush().ok());
        CHECK(!writer.WriteString("more").ok());
    }

    {
        // 底层永远 0 字节且无错误：100 次后按短写报错，不会死循环
        ZeroWriteSink sink;
        Writer<ZeroWriteSink> writer(sink, 16);
        CHECK(writer.WriteString("abc").ok());
        const auto flushed = writer.Flush();
        CHECK(!flushed.ok());
        CHECK(Is(flushed.Err(), streams::IoErrShortWrite()));
        CHECK(sink.calls == 100);
    }

    {
        // 底层谎报写入量：按短写报错，缓冲不变量不被破坏
        GreedySink sink;
        Writer<GreedySink> writer(sink, 8);
        CHECK(writer.WriteString("abcd").ok());
        const auto flushed = writer.Flush();
        CHECK(!flushed.ok());
        CHECK(Is(flushed.Err(), streams::IoErrShortWrite()));
        CHECK(writer.Buffered() == 4);
    }
}

void TestWriterDestructor() {
    Announce("Writer 析构不自动 Flush（有意对齐 Go）");

    MemorySink sink;
    {
        Writer<MemorySink> writer(sink, 64);
        CHECK(writer.WriteString("unflushed").ok());
        CHECK(writer.Buffered() == 9);
    }
    CHECK(sink.data.empty());  // 析构没有 flush
    CHECK(sink.calls == 0);

    // 显式 Flush 之后才落地
    {
        Writer<MemorySink> writer(sink, 64);
        CHECK(writer.WriteString("flushed").ok());
        CHECK(writer.Flush().ok());
    }
    CHECK(sink.data == "flushed");
}

// ===========================================================================
// Scanner
// ===========================================================================

void TestScannerBasics() {
    Announce("Scanner 默认按行 / 空行 / Text / Bytes / 正常 EOF");

    {
        MemorySource source("alpha\nbeta\r\ngamma");
        Scanner<MemorySource> scanner(source);
        CHECK(scanner.BufferSize() == go2cpp::bufio::kDefaultBufferSize);
        std::vector<std::string> tokens;
        while (scanner.Scan()) {
            tokens.push_back(scanner.Text());
        }
        CHECK(tokens.size() == 3);
        if (tokens.size() == 3) {
            CHECK(tokens[0] == "alpha");
            CHECK(tokens[1] == "beta");
            CHECK(tokens[2] == "gamma");
        }
        CHECK(!scanner.Err());  // 正常结束，Err 为空（Go 同）
    }

    {
        // 空行是合法 token
        MemorySource source("a\n\nb\n");
        Scanner<MemorySource> scanner(source);
        std::vector<std::string> tokens;
        while (scanner.Scan()) {
            tokens.push_back(scanner.Text());
        }
        CHECK(tokens.size() == 3);
        if (tokens.size() == 3) {
            CHECK(tokens[0] == "a");
            CHECK(tokens[1].empty());
            CHECK(tokens[2] == "b");
        }
    }

    {
        // Text 与 Bytes 内容一致；连续取 Bytes 指向同一块内存（零拷贝）
        MemorySource source("token\n");
        Scanner<MemorySource> scanner(source);
        CHECK(scanner.Scan());
        const ByteSpan first = scanner.Bytes();
        const ByteSpan second = scanner.Bytes();
        CHECK(first.data() == second.data());
        CHECK(first.size() == 5);
        CHECK(AsStringView(first) == "token");
        CHECK(scanner.Text() == "token");
    }

    {
        MemorySource source("");
        Scanner<MemorySource> scanner(source);
        CHECK(!scanner.Scan());
        CHECK(!scanner.Err());
        CHECK(scanner.Text().empty());
    }

    {
        MemorySource source("x");
        Scanner<MemorySource> scanner(source);
        CHECK(scanner.Scan());
        CHECK(scanner.Text() == "x");
        CHECK(scanner.Bytes().size() == 1);
        CHECK(!scanner.Scan());
        CHECK(!scanner.Err());
    }

    {
        // 分隔符在末尾：不产生尾部空 token
        MemorySource source("a\nb\n");
        Scanner<MemorySource> scanner(source);
        CHECK(scanner.Scan());
        CHECK(scanner.Scan());
        CHECK(!scanner.Scan());
    }

    {
        // 无分隔符：整体一个 token
        MemorySource source("no-delimiter-here");
        Scanner<MemorySource> scanner(source);
        CHECK(scanner.Scan());
        CHECK(scanner.Text() == "no-delimiter-here");
        CHECK(!scanner.Scan());
    }
}

void TestScannerSplits() {
    Announce("Scanner SplitFunc（ScanWords / ScanBytes / 自定义 / 参数校验）");

    {
        MemorySource source("  one\ttwo\r\nthree  ");
        Scanner<MemorySource> scanner(source);
        scanner.SetSplit(go2cpp::bufio::ScanWords);
        std::vector<std::string> tokens;
        while (scanner.Scan()) {
            tokens.push_back(scanner.Text());
        }
        CHECK(tokens.size() == 3);
        if (tokens.size() == 3) {
            CHECK(tokens[0] == "one");
            CHECK(tokens[1] == "two");
            CHECK(tokens[2] == "three");
        }
        CHECK(!scanner.Err());
    }

    {
        MemorySource source("ab");
        Scanner<MemorySource> scanner(source);
        scanner.SetSplit(go2cpp::bufio::ScanBytes);
        std::vector<std::string> tokens;
        while (scanner.Scan()) {
            tokens.push_back(scanner.Text());
        }
        CHECK(tokens.size() == 2);
        if (tokens.size() == 2) {
            CHECK(tokens[0] == "a");
            CHECK(tokens[1] == "b");
        }
        CHECK(!scanner.Err());
    }

    {
        // 自定义分隔符 '|'
        MemorySource source("a|b|c|");
        Scanner<MemorySource> scanner(source, 16);
        scanner.SetSplit([](ByteSpan data, bool at_eof) -> SplitResult {
            if (data.empty()) {
                return SplitResult{};
            }
            const auto* const found = static_cast<const std::uint8_t*>(
                std::memchr(data.data(), '|', data.size()));
            if (found != nullptr) {
                const std::size_t index =
                    static_cast<std::size_t>(found - data.data());
                SplitResult result;
                result.advance = index + 1;
                result.token = data.first(index);
                return result;
            }
            if (at_eof) {
                SplitResult result;
                result.advance = data.size();
                result.token = data;
                return result;
            }
            return SplitResult{};
        });
        std::vector<std::string> tokens;
        while (scanner.Scan()) {
            tokens.push_back(scanner.Text());
        }
        CHECK(tokens.size() == 3);
        if (tokens.size() == 3) {
            CHECK(tokens[0] == "a");
            CHECK(tokens[1] == "b");
            CHECK(tokens[2] == "c");
        }
        CHECK(!scanner.Err());
    }

    {
        // token 远大于初始缓冲：自动翻倍扩容
        const std::string line(500, 'k');
        MemorySource source(line + "\n" + line + "\n", 7);
        Scanner<MemorySource> scanner(source, 16, 4096);
        CHECK(scanner.Scan());
        CHECK(scanner.Text() == line);
        CHECK(scanner.Bytes().size() == 500);
        CHECK(scanner.Scan());
        CHECK(scanner.Text() == line);
        CHECK(!scanner.Scan());
        CHECK(!scanner.Err());
    }

    {
        // 超过 max_token_size -> ErrTooLong
        const std::string line(200, 'k');
        MemorySource source(line);
        Scanner<MemorySource> scanner(source, 16, 64);
        CHECK(!scanner.Scan());
        CHECK(Is(scanner.Err(), ErrTooLong()));
        CHECK(!scanner.Scan());  // 已终止
    }

    {
        // 违约 SplitFunc：advance 越界 -> ErrInvalidAdvance，不崩溃
        MemorySource source("abcdef");
        Scanner<MemorySource> scanner(source, 16);
        scanner.SetSplit([](ByteSpan data, bool) -> SplitResult {
            SplitResult result;
            result.advance = data.size() + 1;
            result.token = data.first(1);
            return result;
        });
        CHECK(!scanner.Scan());
        CHECK(Is(scanner.Err(), ErrInvalidAdvance()));
    }

    {
        // 底层错误传播
        FailingSource source;
        source.data = "line1\nline2\nhidden";
        source.fail_at = 6;
        Scanner<FailingSource> scanner(source);
        CHECK(scanner.Scan());
        CHECK(scanner.Text() == "line1");
        CHECK(!scanner.Scan());
        CHECK(scanner.Err() != nullptr);
        CHECK(!IsEof(scanner.Err()));
    }
}

// ===========================================================================
// 边界集合
// ===========================================================================

void TestEdgeCases() {
    Announce("边界：空输入/单字节/无分隔符/分隔符首尾/0 字节/0x00");

    {
        MemorySource source("");
        Reader<MemorySource> reader(source, 16);
        CHECK(reader.Read(ByteSpan()) == 0);
        CHECK(reader.ReadByte().Err() != nullptr);
        CHECK(reader.Peek(1).Err() != nullptr);
        CHECK(reader.ReadString(',').Err() != nullptr);
        CHECK(reader.ReadLine().Err() != nullptr);
        CHECK(reader.Discard(5) == 0);
    }

    {
        // 单字节 + ReadLine（无换行）
        MemorySource source("x");
        Reader<MemorySource> reader(source, 16);
        const auto line = reader.ReadLine();
        CHECK(line.ok());
        CHECK(line.Value() == "x");
        CHECK(!reader.ReadLine().ok());
    }

    {
        // 单字节 + ReadString（分隔符恰好是唯一的字节）
        MemorySource source("x");
        Reader<MemorySource> reader(source, 16);
        const auto text = reader.ReadString('x');
        CHECK(text.ok());
        CHECK(text.Value() == "x");
        CHECK(!reader.ReadString('x').ok());
    }

    {
        // 无分隔符：ReadString 返回全部 + EOF
        MemorySource source("no delimiter");
        Reader<MemorySource> reader(source, 16);
        const auto text = reader.ReadString(';');
        CHECK(!text.ok());
        CHECK(IsEof(text.Err()));
        CHECK(text.Value() == "no delimiter");
    }

    {
        // 二进制数据里的 0x00 不打断 Read/ReadLine 的字节语义
        const std::string binary("a\0b\nc\0d", 7);
        MemorySource source(binary);
        Reader<MemorySource> reader(source, 16);
        const auto line = reader.ReadLine();
        CHECK(line.ok());
        CHECK(line.Value().size() == 3);
        CHECK(line.Value() == std::string("a\0b", 3));
    }
}

// ===========================================================================
// 随机测试
// ===========================================================================

void RandomReadLine() {
    Announce("随机：ReadLine（随机行长/CRLF/随机分块/随机缓冲）");
    std::mt19937 rng(0x5EED1234U);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const std::size_t buffer_size =
            16 + static_cast<std::size_t>(rng() % 33);  // 16..48
        const int line_count = static_cast<int>(rng() % 9);  // 0..8
        std::vector<std::string> expected;
        std::string data;
        for (int index = 0; index < line_count; ++index) {
            const std::size_t length =
                (rng() % 2 == 0) ? 0 : (rng() % (buffer_size + 1));
            std::string line;
            for (std::size_t offset = 0; offset < length; ++offset) {
                line.push_back(static_cast<char>('a' + (rng() % 26)));
            }
            const bool last_line = index + 1 == line_count;
            const bool terminated = last_line ? (rng() % 2 == 0) : true;
            // 末行为空且没有终止符时，ReadLine 无法把它与「没有更多行」区分，
            // 参考集里也不能算它——否则就是在要求实现凭空造出一个空行。
            if (!last_line || terminated || !line.empty()) {
                expected.push_back(line);
            }
            data += line;
            if (terminated) {
                data += (rng() % 4 == 0) ? "\r\n" : "\n";
            }
        }

        MemorySource source(data);
        while (source.plan.size() < 8) {
            source.plan.push_back(1 + (rng() % 7));
        }
        Reader<MemorySource> reader(source, buffer_size);
        for (const std::string& want : expected) {
            const auto got = reader.ReadLine();
            CHECK(got.ok());
            CHECK(got.Value() == want);
        }
        const auto end = reader.ReadLine();
        CHECK(!end.ok());
        CHECK(IsEof(end.Err()));
    }
}

void RandomRead() {
    Announce("随机：Read（随机长度/随机请求尺寸/验证原始字节）");
    std::mt19937 rng(0x1234ABCDU);
    for (int iteration = 0; iteration < 120; ++iteration) {
        const std::size_t size = rng() % 150001;
        const std::string data = RandomBytes(rng, size);
        const std::size_t buffer_size = 16 + (rng() % 497);
        MemorySource source(data);
        while (source.plan.size() < 7) {
            source.plan.push_back(1 + (rng() % 97));
        }
        Reader<MemorySource> reader(source, buffer_size);
        std::string collected;
        collected.reserve(data.size());
        while (true) {
            std::string buffer(1 + (rng() % 600), '\0');
            const std::size_t got = reader.Read(AsBytes(buffer));
            if (got == 0) {
                break;
            }
            CHECK(got <= buffer.size());
            collected.append(buffer.data(), got);
        }
        CHECK(collected == data);
        CHECK(IsEof(reader.Err()));
        CHECK(reader.Buffered() == 0);
    }
}

void RandomPeekDiscard() {
    Announce("随机：Peek/Discard/ReadByte 混合（与参考位置比对）");
    std::mt19937 rng(0xC0FFEEU);
    for (int iteration = 0; iteration < 150; ++iteration) {
        const std::string data = RandomBytes(rng, rng() % 4001);
        const std::size_t buffer_size = 16 + (rng() % 113);
        MemorySource source(data);
        while (source.plan.size() < 6) {
            source.plan.push_back(1 + (rng() % 11));
        }
        Reader<MemorySource> reader(source, buffer_size);
        std::size_t position = 0;
        for (int step = 0; step < 24; ++step) {
            const std::size_t remaining = data.size() - position;
            const std::size_t peek_size = rng() % (buffer_size + 1);
            const auto peeked = reader.Peek(peek_size);
            if (remaining >= peek_size) {
                CHECK(peeked.ok());
            } else {
                CHECK(!peeked.ok());
                CHECK(IsEof(peeked.Err()));
            }
            const std::size_t expected_size = std::min(peek_size, remaining);
            CHECK(peeked.Value().size() == expected_size);
            CHECK(ToString(peeked.Value()) == data.substr(position, expected_size));

            const std::size_t choice = rng() % 3;
            if (choice == 0) {
                const std::size_t want = rng() % 40;
                const std::size_t discarded = reader.Discard(want);
                CHECK(discarded == std::min(want, data.size() - position));
                position += discarded;
            } else if (choice == 1) {
                const auto byte = reader.ReadByte();
                if (position < data.size()) {
                    CHECK(byte.ok());
                    CHECK(byte.Value() ==
                          static_cast<std::uint8_t>(data[position]));
                    ++position;
                } else {
                    CHECK(!byte.ok());
                }
            } else {
                const std::size_t want = 1 + (rng() % 200);
                std::string buffer(want, '\0');
                const std::size_t got = reader.Read(AsBytes(buffer));
                CHECK(got <= want);
                CHECK(got <= data.size() - position);
                CHECK(buffer.compare(0, got, data.substr(position, got)) == 0);
                position += got;
                if (got == 0) {
                    CHECK(IsEof(reader.Err()));
                }
            }
        }
    }
}

void RandomReadString() {
    Announce("随机：ReadString（随机分隔符 0x00..'|' / 随机分块）");
    std::mt19937 rng(0xBEEF01U);
    const char delimiters[] = {',', ';', '|', '\n', '\0', 'a', '\r'};
    for (int iteration = 0; iteration < 150; ++iteration) {
        std::string data = RandomBytes(rng, rng() % 3001);
        if (!data.empty()) {
            for (int count = 0; count < 5; ++count) {
                data[rng() % data.size()] = delimiters[rng() % 7];
            }
        }
        const char delim = delimiters[rng() % 7];
        MemorySource source(data);
        while (source.plan.size() < 5) {
            source.plan.push_back(1 + (rng() % 9));
        }
        Reader<MemorySource> reader(source, 16 + (rng() % 49));
        std::size_t position = 0;
        while (true) {
            const auto got = reader.ReadString(delim);
            bool reached_end = false;
            const std::string want =
                ReferenceReadString(data, position, delim, reached_end);
            CHECK(got.Value() == want);
            if (reached_end) {
                CHECK(!got.ok());
                CHECK(IsEof(got.Err()));
                break;
            }
            CHECK(got.ok());
        }
    }
}

void RandomScanner() {
    Announce("随机：Scanner（随机行数/行长/CRLF/随机缓冲，自动扩容）");
    std::mt19937 rng(0x77770001U);
    for (int iteration = 0; iteration < 100; ++iteration) {
        const std::size_t buffer_size = 16 + (rng() % 49);
        const int line_count = static_cast<int>(rng() % 13);
        std::vector<std::string> expected;
        std::string data;
        for (int index = 0; index < line_count; ++index) {
            const std::size_t length = rng() % 201;
            std::string line;
            for (std::size_t offset = 0; offset < length; ++offset) {
                line.push_back(static_cast<char>('a' + (rng() % 26)));
            }
            const bool last_line = index + 1 == line_count;
            const bool terminated = last_line ? (rng() % 2 == 0) : true;
            if (!last_line || terminated || !line.empty()) {
                expected.push_back(line);
            }
            data += line;
            if (terminated) {
                data += (rng() % 3 == 0) ? "\r\n" : "\n";
            }
        }

        MemorySource source(data);
        while (source.plan.size() < 6) {
            source.plan.push_back(1 + (rng() % 13));
        }
        Scanner<MemorySource> scanner(source, buffer_size, 4096);
        std::size_t index = 0;
        while (scanner.Scan()) {
            CHECK(index < expected.size());
            if (index < expected.size()) {
                CHECK(scanner.Text() == expected[index]);
                CHECK(AsStringView(scanner.Bytes()) == expected[index]);
                CHECK(scanner.Bytes().size() == expected[index].size());
            }
            ++index;
        }
        CHECK(index == expected.size());
        CHECK(!scanner.Err());
    }
}

// ===========================================================================
// 真实场景
// ===========================================================================

/// 场景 1：逐行读配置文件（注释 / 空行 / 7 字节分块）。
void ScenarioConfigFile() {
    Announce("场景1：逐行读配置文件（ReadLine + 注释/空行）");
    const std::string config =
        "# go2cpp demo config\n"
        "\n"
        "name = go2cpp\n"
        "  # indented comment\n"
        "workers=4\n"
        "timeout = 30s\n";

    MemorySource source(config, 7);  // 模拟 7 字节一块的文件读
    Reader<MemorySource> reader(source, 32);
    std::map<std::string, std::string> parsed;
    std::size_t line_count = 0;
    while (true) {
        const auto line = reader.ReadLine();
        if (!line.ok()) {
            break;
        }
        ++line_count;
        const std::string text = Trim(line.Value());
        if (text.empty() || text[0] == '#') {
            continue;
        }
        const std::size_t equals = text.find('=');
        CHECK(equals != std::string::npos);
        if (equals == std::string::npos) {
            continue;
        }
        parsed[Trim(text.substr(0, equals))] = Trim(text.substr(equals + 1));
    }
    CHECK(line_count == 6);
    CHECK(IsEof(reader.Err()));
    CHECK(parsed.size() == 3);
    CHECK(parsed["name"] == "go2cpp");
    CHECK(parsed["workers"] == "4");
    CHECK(parsed["timeout"] == "30s");
    std::printf("[bufio]   配置行数=%zu, 有效键=%zu, 底层调用=%llu\n", line_count,
                parsed.size(), static_cast<unsigned long long>(source.calls));
}

/// 场景 2：读 HTTP 响应 —— Peek 找头、Discard 跳头、按 Content-Length 精确读。
void ScenarioHttpResponse() {
    Announce("场景2：HTTP 响应（Peek 零拷贝解析头 + Content-Length 精确读 body）");
    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 11\r\n"
        "\r\n";
    const std::string body = "hello world";
    const std::string extra = "EXTRA-BYTES";
    // 5 字节一块，模拟 TCP 分片
    MemorySource source(header + body + extra, 5);
    Reader<MemorySource> reader(source, 4096);

    // 第一次 Peek 把能拿到的都拿过来；因为总长 < 4096，会带 EOF
    const auto window = reader.Peek(reader.Size());
    CHECK(!window.ok());
    CHECK(IsEof(window.Err()));
    const std::string_view text = AsStringView(window.Value());
    const std::size_t header_end = text.find("\r\n\r\n");
    CHECK(header_end != std::string_view::npos);
    if (header_end == std::string_view::npos) {
        return;
    }

    const std::string_view marker = "Content-Length: ";
    const std::size_t marker_pos = text.find(marker);
    CHECK(marker_pos != std::string_view::npos);
    std::size_t digit_begin = marker_pos + marker.size();
    std::size_t digit_end = digit_begin;
    while (digit_end < text.size() && text[digit_end] >= '0' &&
           text[digit_end] <= '9') {
        ++digit_end;
    }
    CHECK(digit_end > digit_begin);
    const std::size_t declared = static_cast<std::size_t>(
        std::stoul(std::string(text.substr(digit_begin, digit_end - digit_begin))));
    CHECK(declared == body.size());

    // 跳过头部（含空行），零拷贝窗口随之失效
    CHECK(reader.Discard(header_end + 4) == header_end + 4);

    // 按 Content-Length 精确读 body：不越读
    std::string body_read(body.size(), '\0');
    std::size_t received = 0;
    while (received < body_read.size()) {
        const std::size_t got = reader.Read(
            AsBytes(body_read.data() + received, body_read.size() - received));
        if (got == 0) {
            break;
        }
        received += got;
    }
    CHECK(received == body.size());
    CHECK(body_read == body);

    // 剩余字节仍在缓冲里，没有被 body 读取吞掉
    CHECK(reader.Buffered() == extra.size());
    const std::size_t buffered_before_extra = reader.Buffered();
    std::string rest(extra.size(), '\0');
    CHECK(reader.Read(AsBytes(rest)) == extra.size());
    CHECK(rest == extra);
    std::printf("[bufio]   头长=%zu, Content-Length=%zu, 读 body 后缓冲=%zu, "
                "底层调用=%llu\n",
                header_end + 4, declared, buffered_before_extra,
                static_cast<unsigned long long>(source.calls));
}

/// 场景 3：写日志 —— 批量写、Flush、回读校验；并验证「不 Flush 就析构」。
void ScenarioLogWrite() {
    Announce("场景3：写日志（Writer 批量写 1 万行 -> Flush -> 回读校验）");
    // 并发 ctest 会让两个测试进程共享 /tmp；固定名字会互相截断/删除。
    const std::string process_tag = std::to_string(::getpid());
    const std::string path = "/tmp/go2cpp_bufio_log_" + process_tag + ".txt";
    const std::string unflushed_path =
        "/tmp/go2cpp_bufio_log_unflushed_" + process_tag + ".txt";
    ::unlink(path.c_str());
    ::unlink(unflushed_path.c_str());

    std::size_t written_lines = 0;
    std::uint64_t sink_calls = 0;
    {
        const int descriptor =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(descriptor >= 0);
        if (descriptor < 0) {
            return;
        }
        FileSink sink(descriptor);
        Writer<FileSink> writer(sink, 4096);
        for (int index = 0; index < 10000; ++index) {
            const std::string line =
                "[INFO] record " + std::to_string(index) + "\n";
            CHECK(writer.WriteString(line).ok());
            ++written_lines;
        }
        CHECK(writer.Buffered() > 0);  // 还有没落地的数据
        CHECK(sink.calls < 200);       // 远少于 1 万次系统调用
        CHECK(writer.Flush().ok());
        CHECK(writer.Buffered() == 0);
        sink_calls = sink.calls;
    }

    const std::string content = ReadWholeFile(path);
    std::size_t line_count = 0;
    std::size_t offset = 0;
    std::string first_line;
    std::string last_line;
    while (offset < content.size()) {
        const std::size_t newline = content.find('\n', offset);
        if (newline == std::string::npos) {
            break;
        }
        const std::string line = content.substr(offset, newline - offset);
        if (line_count == 0) {
            first_line = line;
        }
        last_line = line;
        ++line_count;
        offset = newline + 1;
    }
    CHECK(written_lines == 10000);
    CHECK(line_count == 10000);
    CHECK(first_line == "[INFO] record 0");
    CHECK(last_line == "[INFO] record 9999");
    CHECK(offset == content.size());  // 没有半行残留

    // 析构不 Flush：数据确实没落地
    {
        const int descriptor =
            ::open(unflushed_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(descriptor >= 0);
        if (descriptor >= 0) {
            FileSink sink(descriptor);
            Writer<FileSink> writer(sink, 4096);
            CHECK(writer.WriteString("never flushed\n").ok());
            CHECK(writer.Buffered() == 14);
        }
    }
    CHECK(ReadWholeFile(unflushed_path).empty());

    std::printf("[bufio]   写入行数=%zu, 回读行数=%zu, 底层 write=%llu 次, "
                "未 flush 文件=%zu 字节\n",
                written_lines, line_count,
                static_cast<unsigned long long>(sink_calls),
                ReadWholeFile(unflushed_path).size());
}

/// 场景 4：Scanner 扫 10 万行。
void ScenarioScannerLargeFile() {
    Announce("场景4：Scanner 扫 10 万行（行数/首末行/长度校验和）");
    constexpr int kLines = 100000;
    std::string data;
    data.reserve(static_cast<std::size_t>(kLines) * 14);
    std::size_t expected_checksum = 0;
    for (int index = 0; index < kLines; ++index) {
        const std::string line = "line-" + std::to_string(index);
        data += line;
        data.push_back('\n');
        expected_checksum += line.size();
    }

    MemorySource source(data, 8192);  // 模拟 8 KiB 文件读
    Scanner<MemorySource> scanner(source, 4096);
    const auto start = std::chrono::steady_clock::now();
    std::size_t count = 0;
    std::size_t checksum = 0;
    std::string first;
    std::string last;
    while (scanner.Scan()) {
        const std::string token = scanner.Text();
        if (count == 0) {
            first = token;
        }
        last = token;
        checksum += token.size();
        ++count;
    }
    const auto finish = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(finish - start).count();

    CHECK(count == static_cast<std::size_t>(kLines));
    CHECK(checksum == expected_checksum);
    CHECK(first == "line-0");
    CHECK(last == "line-99999");
    CHECK(!scanner.Err());
    ReportRate("Scanner 10万行", data.size(), seconds);
    std::printf("[bufio]   行数=%zu, 校验和=%zu, 底层调用=%llu\n", count, checksum,
                static_cast<unsigned long long>(source.calls));
}

/// 场景 5：分块传输 —— 1 MiB 随机分块，Read 循环到 EOF。
void ScenarioChunkedTransfer() {
    Announce("场景5：分块传输（1 MiB 随机分块 -> Read 循环到 EOF）");
    constexpr std::size_t kTotal = 1024 * 1024;
    std::string payload(kTotal, '\0');
    for (std::size_t index = 0; index < kTotal; ++index) {
        payload[index] = static_cast<char>((index * 31U + 7U) & 0xFFU);
    }

    MemorySource source(payload);
    source.plan = {1, 3, 7, 13, 64, 1024, 4096};  // 不规则分块
    Reader<MemorySource> reader(source, 4096);

    const auto start = std::chrono::steady_clock::now();
    std::string received;
    received.reserve(kTotal);
    char buffer[512];
    while (true) {
        const std::size_t got = reader.Read(AsBytes(buffer, sizeof(buffer)));
        if (got == 0) {
            break;
        }
        received.append(buffer, got);
    }
    const auto finish = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(finish - start).count();

    CHECK(received.size() == kTotal);
    CHECK(received == payload);
    CHECK(IsEof(reader.Err()));
    // 512 字节请求 × 2048 次，但底层只被调用「分块次数」量级（远小于 1M）
    CHECK(source.calls < kTotal / 10);
    ReportRate("分块传输 1MiB", kTotal, seconds);
    std::printf("[bufio]   Read 调用=%zu, 底层 Read=%llu, 每次请求=512B\n",
                kTotal / 512, static_cast<unsigned long long>(source.calls));
}

/// 场景 6：慢速对端（每次最多 512 字节）写 256 KiB。
void ScenarioSlowSink() {
    Announce("场景6：慢速/短写对端（每次最多 512B，写 256 KiB）");
    MemorySink sink;
    sink.max_write = 512;
    Writer<MemorySink> writer(sink, 4096);

    std::string expected;
    constexpr int kLines = 2600;
    for (int index = 0; index < kLines; ++index) {
        const std::string line =
            "upload-chunk-" + std::to_string(index) + "-payload\n";
        expected += line;
        CHECK(writer.WriteString(line).ok());
    }
    CHECK(writer.Buffered() > 0);
    CHECK(writer.Flush().ok());
    CHECK(writer.Buffered() == 0);
    CHECK(sink.data == expected);
    CHECK(sink.calls >= expected.size() / 512);  // 确实是多次短写
    CHECK(sink.calls < expected.size());          // 但远少于逐字节
    std::printf("[bufio]   总字节=%zu, 短写次数=%llu, 平均每次=%.1f 字节\n",
                expected.size(), static_cast<unsigned long long>(sink.calls),
                static_cast<double>(expected.size()) /
                    static_cast<double>(sink.calls));
}

// ---------------------------------------------------------------------------
// 7. ReadSlice / ReadBytes / UnreadByte / 零长度 Read
// ---------------------------------------------------------------------------
void TestReaderSliceBytesUnread() {
    Announce("reader: ReadSlice / ReadBytes / UnreadByte / zero-length Read");
    {
        MemorySource source("line1\nline2\r\nline3");
        Reader<MemorySource> reader(source, 16);
        auto first = reader.ReadSlice('\n');
        CHECK(first.ok());
        CHECK(ToString(first.Value()) == "line1\n");
        auto second = reader.ReadSlice('\n');
        CHECK(second.ok());
        CHECK(ToString(second.Value()) == "line2\r\n");  // ReadSlice 不剥 \r
        auto third = reader.ReadSlice('\n');
        CHECK(!third.ok() && IsEof(third.Err()));
        CHECK(ToString(third.Value()) == "line3");
        auto fourth = reader.ReadSlice('\n');
        CHECK(!fourth.ok() && IsEof(fourth.Err()));
        CHECK(fourth.Value().empty());
    }
    {
        // 缓冲满仍未遇到分隔符：Go 的 ErrBufferFull，且已读字节被消费。
        MemorySource source(std::string(100, 'a') + "\n");
        Reader<MemorySource> reader(source, 16);
        auto chunk = reader.ReadSlice('\n');
        CHECK(!chunk.ok());
        CHECK(Is(chunk.Err(), go2cpp::bufio::ErrBufferFull()));
        CHECK(chunk.Value().size() == 16);
        std::string assembled = ToString(chunk.Value());
        for (int i = 0; i < 20; ++i) {
            auto next = reader.ReadSlice('\n');
            assembled += ToString(next.Value());
            if (next.ok() || IsEof(next.Err())) {
                break;
            }
            CHECK(Is(next.Err(), go2cpp::bufio::ErrBufferFull()));
        }
        CHECK(assembled == std::string(100, 'a') + "\n");
    }
    {
        MemorySource source("alpha,beta,gamma");
        Reader<MemorySource> reader(source, 8);
        auto token = reader.ReadBytes(',');
        CHECK(token.ok() && token.Value() == "alpha,");
        auto rest = reader.ReadBytes(',');
        CHECK(rest.ok() && rest.Value() == "beta,");
        auto tail = reader.ReadBytes(',');
        CHECK(!tail.ok() && IsEof(tail.Err()) && tail.Value() == "gamma");
    }
    {
        MemorySource source("xyz");
        Reader<MemorySource> reader(source, 16);
        auto b1 = reader.ReadByte();
        CHECK(b1.ok() && b1.Value() == 'x');
        auto b2 = reader.ReadByte();
        CHECK(b2.ok() && b2.Value() == 'y');
        CHECK(reader.UnreadByte().ok());
        auto b3 = reader.ReadByte();
        CHECK(b3.ok() && b3.Value() == 'y');
        CHECK(reader.UnreadByte().ok());
        auto line = reader.ReadLine();
        CHECK(line.ok() && line.Value() == "yz");
        auto invalid = reader.UnreadByte();
        CHECK(!invalid.ok() &&
              Is(invalid.Err(), go2cpp::bufio::ErrInvalidUnreadByte()));
        // 读 0 字节必须是 (0, nil)，不能报 EOF（Go 的 io.Reader 契约）。
        char dummy = 0;
        const streams::ReadResult zero = reader.Read(&dummy, 0);
        CHECK(zero.count == 0 && !zero.error);
    }
}

// ---------------------------------------------------------------------------
// 8. ReadRune / UnreadRune
// ---------------------------------------------------------------------------
void TestReaderRune() {
    Announce("reader: ReadRune / UnreadRune (UTF-8)");
    {
        MemorySource source(std::string("A") + "\xC3\xA9" + "\xE2\x82\xAC" +
                            "\xF0\x9F\x98\x80");
        Reader<MemorySource> reader(source, 16);
        auto ascii = reader.ReadRune();
        CHECK(ascii.ok() && ascii.rune == U'A' && ascii.size == 1);
        auto e_acute = reader.ReadRune();
        CHECK(e_acute.ok() && e_acute.rune == 0xE9 && e_acute.size == 2);
        CHECK(reader.UnreadRune().ok());
        auto e_acute_again = reader.ReadRune();
        CHECK(e_acute_again.ok() && e_acute_again.rune == 0xE9 &&
              e_acute_again.size == 2);
        auto euro = reader.ReadRune();
        CHECK(euro.ok() && euro.rune == 0x20AC && euro.size == 3);
        auto emoji = reader.ReadRune();
        CHECK(emoji.ok() && emoji.rune == 0x1F600 && emoji.size == 4);
        auto eof = reader.ReadRune();
        CHECK(!eof.ok() && IsEof(eof.error));
    }
    {
        // 非法字节：Go 返回 RuneError + size 1，且**不是**错误。
        MemorySource source("\xFF\x41", 1);
        Reader<MemorySource> reader(source, 16);
        auto invalid = reader.ReadRune();
        CHECK(invalid.ok() &&
              invalid.rune == go2cpp::bufio::RuneError && invalid.size == 1);
        auto ascii = reader.ReadRune();
        CHECK(ascii.ok() && ascii.rune == U'A' && ascii.size == 1);
    }
    {
        // 码位跨填充边界：15 个 ASCII 后跟 2 字节 UTF-8，缓冲 16。
        MemorySource source(std::string(15, 'x') + "\xC3\xA9");
        Reader<MemorySource> reader(source, 16);
        CHECK(reader.Discard(15) == 15);
        auto rune = reader.ReadRune();
        CHECK(rune.ok() && rune.rune == 0xE9 && rune.size == 2);
    }
    {
        // 截断序列（EOF）：逐字节返回 RuneError + size 1。
        MemorySource source("\xE2\x82");
        Reader<MemorySource> reader(source, 16);
        auto first = reader.ReadRune();
        CHECK(first.ok() && first.rune == go2cpp::bufio::RuneError &&
              first.size == 1);
        auto second = reader.ReadRune();
        CHECK(second.ok() && second.rune == go2cpp::bufio::RuneError &&
              second.size == 1);
    }
    {
        MemorySource source("ab");
        Reader<MemorySource> reader(source, 16);
        CHECK(!reader.UnreadRune().ok());
        auto a = reader.ReadRune();
        CHECK(a.ok());
        auto b = reader.ReadByte();
        CHECK(b.ok());
        auto invalid = reader.UnreadRune();
        CHECK(!invalid.ok() &&
              Is(invalid.Err(), go2cpp::bufio::ErrInvalidUnreadRune()));
    }
}

// ---------------------------------------------------------------------------
// 9. Writer.ReadFrom / Writer.Reset
// ---------------------------------------------------------------------------
void TestWriterReadFromReset() {
    Announce("writer: ReadFrom / Reset");
    {
        MemorySink sink;
        MemorySource source("hello world");
        Writer<MemorySink> writer(sink, 8);
        const auto count = writer.ReadFrom(source);
        CHECK(count.ok() && count.Value() == 11);
        CHECK(writer.Flush().ok());
        CHECK(sink.data == "hello world");
    }
    {
        // 源在 3 字节后注入错误：数据先交付，错误作为返回值。
        MemorySink sink;
        FailingSource source;
        source.data = "abc";
        source.fail_at = 3;
        Writer<MemorySink> writer(sink);
        const auto count = writer.ReadFrom(source);
        CHECK(!count.ok());
        CHECK(count.Value() == 3);
        CHECK(writer.Flush().ok());
        CHECK(sink.data == "abc");
    }
    {
        // 底层 Reader 违约（n > len(p)）：必须报短写，不能越界读 chunk。
        MemorySink sink;
        OverreportingSource source;
        Writer<MemorySink> writer(sink);
        const auto count = writer.ReadFrom(source);
        CHECK(!count.ok());
        CHECK(Is(count.Err(), streams::IoErrShortWrite()));
        CHECK(sink.data.empty());
        CHECK(source.calls == 1);
    }
    {
        // Reset 丢弃未刷出的缓冲，并改绑到新目标。
        MemorySink first;
        MemorySink second;
        Writer<MemorySink> writer(first, 8);
        CHECK(writer.WriteString("discarded").ok());
        CHECK(writer.Buffered() > 0);
        writer.Reset(second);
        CHECK(writer.Buffered() == 0);
        CHECK(writer.Err() == nullptr);
        CHECK(first.data.empty());
        CHECK(writer.WriteString("kept").ok());
        CHECK(writer.Flush().ok());
        CHECK(second.data == "kept");
    }
}

// ---------------------------------------------------------------------------
// 10. Scanner.Buffer(max) / ErrFinalToken
// ---------------------------------------------------------------------------
void TestScannerBufferFinalToken() {
    Announce("scanner: Buffer(max) / ErrFinalToken");
    {
        MemorySource source(std::string(64, 'a') + "\n");
        Scanner<MemorySource> scanner(source, 16);
        scanner.Buffer(32);
        CHECK(scanner.MaxTokenSize() == 32);
        CHECK(!scanner.Scan());
        CHECK(Is(scanner.Err(), go2cpp::bufio::ErrTooLong()));
    }
    {
        MemorySource source(std::string(31, 'a') + "\n");
        Scanner<MemorySource> scanner(source, 16);
        scanner.Buffer(32);
        CHECK(scanner.Scan());
        CHECK(scanner.Text().size() == 31);
        CHECK(!scanner.Scan());
        CHECK(scanner.Err() == nullptr);
    }
    {
        // ErrFinalToken：最后一次 Scan 返回 true（交付 token），随后 false + 空错误。
        MemorySource source("tail");
        Scanner<MemorySource> scanner(source, 16);
        scanner.SetSplit([](ByteSpan data, bool at_eof) {
            SplitResult result;
            if (at_eof && !data.empty()) {
                result.advance = data.size();
                result.token = data;
                result.error = go2cpp::bufio::ErrFinalToken();
            }
            return result;
        });
        CHECK(scanner.Scan());
        CHECK(scanner.Text() == "tail");
        CHECK(!scanner.Scan());
        CHECK(scanner.Err() == nullptr);
        CHECK(scanner.Text() == "tail");  // 最终 token 保留到下一次 Scan
    }
    {
        // 普通错误仍然终止扫描并写 Err()。
        MemorySource source("data");
        Scanner<MemorySource> scanner(source, 16);
        scanner.SetSplit([](ByteSpan, bool at_eof) {
            SplitResult result;
            if (at_eof) {
                result.error = NewError("split: injected");
            }
            return result;
        });
        CHECK(!scanner.Scan());
        CHECK(scanner.Err() != nullptr);
        CHECK(!scanner.Scan());
    }
}

}  // namespace

int main() {
    std::printf("[bufio] ===== go2cpp::bufio 测试开始 =====\n");

    TestReaderBasics();
    TestReaderPeek();
    TestReaderLine();
    TestReaderString();
    TestReaderDiscardAndFaults();
    TestWriterBasics();
    TestWriterPartialAndFaults();
    TestWriterDestructor();
    TestScannerBasics();
    TestScannerSplits();
    TestEdgeCases();
    TestReaderSliceBytesUnread();
    TestReaderRune();
    TestWriterReadFromReset();
    TestScannerBufferFinalToken();

    RandomReadLine();
    RandomRead();
    RandomPeekDiscard();
    RandomReadString();
    RandomScanner();

    ScenarioConfigFile();
    ScenarioHttpResponse();
    ScenarioLogWrite();
    ScenarioScannerLargeFile();
    ScenarioChunkedTransfer();
    ScenarioSlowSink();

    std::printf("[bufio] ===== checks=%d failures=%d =====\n", g_checks,
                g_failures);
    if (g_failures != 0) {
        std::fprintf(stderr, "[bufio] FAILED: %d 个断言未通过\n", g_failures);
        return 1;
    }
    std::printf("[bufio] all checks passed\n");
    return 0;
}
