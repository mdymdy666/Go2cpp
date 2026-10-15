/**
 * @file test_streams.cpp
 * @brief `go2cpp/streams.hpp` 的正式测试：Go io 语义边界 + 大数据量 + 随机分块 + 真实场景。
 *
 * 测试设计（按项目规范：大数据量 / 随机 / ≥5 真实场景）：
 *   场景 1 大文件复制（1 MiB + Copy）        场景 5 边读边算校验（TeeReader）
 *   场景 2 定长帧解析（ReadFull 循环）        场景 6 多路输出（MultiWriter）
 *   场景 3 配置头部限流读取（LimitReader）    场景 7 文件区间读取（SectionReader + Seek）
 *   场景 4 多段流拼接（MultiReader）          场景 8 调试输出丢弃（Discard）
 *   大数据量：1 MiB 复制、10 万次小块读写、10 万帧定长解析
 *   随机：随机分块源（Copy 后必须逐字节一致）、随机短读源（ReadFull 必须读满）、
 *         随机偏移 SectionReader（内容必须等于对应切片）、随机 LimitReader 上限
 *
 * ⚠️ 注释中不得出现连续的斜杠加星号（GCC -Wcomment）。
 */
#include "go2cpp/streams.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using go2cpp::ErrorPtr;
using namespace go2cpp::streams;

/// 每次 `Read` 只返回 1..max_chunk 字节，强制调用方处理"短读"。
class ChunkedReader final : public Reader {
public:
    ChunkedReader(std::string data, std::size_t max_chunk)
        : m_data(std::move(data)), m_max_chunk(max_chunk == 0 ? 1 : max_chunk) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        if (m_offset >= m_data.size()) return ReadResult{0, IoEof()};
        const std::size_t available = m_data.size() - m_offset;
        std::size_t take = size < available ? size : available;
        if (take > m_max_chunk) take = m_max_chunk;
        std::memcpy(buffer, m_data.data() + m_offset, take);
        m_offset += take;
        return ReadResult{take, {}};
    }

private:
    std::string m_data;
    std::size_t m_offset{0};
    std::size_t m_max_chunk;
};

/// 每次 `Read` 最多 1 字节（最恶劣的短读源）。
class DripReader final : public Reader {
public:
    explicit DripReader(std::string data) : m_data(std::move(data)) {}

    ReadResult Read(char* buffer, std::size_t size) override {
        if (m_offset >= m_data.size()) return ReadResult{0, IoEof()};
        if (size == 0) return ReadResult{0, {}};
        buffer[0] = m_data[m_offset++];
        return ReadResult{1, {}};
    }

private:
    std::string m_data;
    std::size_t m_offset{0};
};

/// 造一段可重复的伪随机数据（避免依赖随机数实现差异）。
std::string MakePayload(std::size_t size) {
    std::string data;
    data.reserve(size);
    std::uint32_t state = 0x12345678U;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525U + 1013904223U;
        data.push_back(static_cast<char>((state >> 16) & 0xFFU));
    }
    return data;
}

}  // namespace

/**
 * @brief 运行 streams 模块的全部测试。
 */
void run_streams_tests() {
    go2cpp_tests::announce("streams: Go io 语义边界 + 大数据量 + 随机分块 + 真实场景");
    using namespace go2cpp;
    using namespace go2cpp::streams;

    // ---------------- 场景 1：大文件复制（1 MiB） ----------------
    {
        const std::string payload = MakePayload(1024U * 1024U);
        StringReader source(payload);
        BufferWriter destination;
        const ReadResult copied = Copy(destination, source);
        GO2CPP_CHECK(copied.ok());
        GO2CPP_CHECK(copied.count == payload.size());
        GO2CPP_CHECK(destination.Len() == payload.size());
        GO2CPP_CHECK(std::memcmp(destination.String().data(), payload.data(), payload.size()) == 0);
    }

    // ---------------- 场景 2：定长帧解析（ReadFull 循环） ----------------
    {
        std::string framed;
        for (int frame = 0; frame < 1000; ++frame) {
            framed += std::string(8, static_cast<char>('A' + (frame % 26)));
        }
        StringReader source(framed);
        char frame_buffer[8] = {};
        int frames = 0;
        while (true) {
            const ReadResult result = ReadFull(source, frame_buffer, sizeof(frame_buffer));
            if (result.error) {
                GO2CPP_CHECK(Is(result.error, IoEof()) && result.count == 0);
                break;
            }
            GO2CPP_CHECK(result.count == 8 && frame_buffer[0] == frame_buffer[7]);
            ++frames;
        }
        GO2CPP_CHECK(frames == 1000);
    }

    // ---------------- 场景 3：配置头部限流读取 ----------------
    {
        const std::string config = "MAGIC:0011\nbody-body-body-body";
        StringReader source(config);
        LimitedReader header(source, 11);                    // 头部 "MAGIC:0011" 共 10 字节 + 换行
        char buffer[32] = {};
        const ReadResult read = header.Read(buffer, sizeof(buffer));
        GO2CPP_CHECK(read.count == 11 && std::string(buffer, 10) == "MAGIC:0011");
        GO2CPP_CHECK(header.Remaining() == 0);
        // 限流结束后，底层仍可继续读（Go 语义：LimitReader 不消费源之外的东西）
        const ReadResult body = source.Read(buffer, sizeof(buffer));
        GO2CPP_CHECK(body.count == config.size() - 11);
        GO2CPP_CHECK(std::string(buffer, body.count) == "body-body-body-body");
    }

    // ---------------- 场景 4：多段流拼接 ----------------
    {
        StringReader head("HEAD");
        StringReader middle("|MID|");
        StringReader tail("TAIL");
        MultiReader joined({&head, &middle, &tail});
        const auto result = ReadAll(joined);
        GO2CPP_CHECK(!result.second && result.first == "HEAD|MID|TAIL");
    }

    // ---------------- 场景 5：边读边算校验（TeeReader） ----------------
    {
        const std::string payload = MakePayload(4096);
        StringReader source(payload);
        BufferWriter mirror;
        TeeReader tee(source, mirror);
        const auto result = ReadAll(tee);
        GO2CPP_CHECK(!result.second && result.first == payload);
        GO2CPP_CHECK(mirror.String() == payload);            // 边读边落盘，内容一致
    }

    // ---------------- 场景 6：多路输出（MultiWriter） ----------------
    {
        BufferWriter console;
        BufferWriter file;
        BufferWriter audit;
        MultiWriter multi({&console, &file, &audit});
        const char* const lines[] = {"start\n", "processing\n", "done\n"};
        for (const char* line : lines) {
            const WriteResult written = WriteString(multi, line);
            GO2CPP_CHECK(written.ok() && written.count == std::strlen(line));
        }
        GO2CPP_CHECK(console.String() == "start\nprocessing\ndone\n");
        GO2CPP_CHECK(console.String() == file.String() && file.String() == audit.String());
    }

    // ---------------- 场景 7：文件区间读取（SectionReader + Seek） ----------------
    {
        const std::string payload = "0123456789ABCDEF";
        StringReader source(payload);
        SectionReader section(source, 4, 8);                 // "456789AB"
        GO2CPP_CHECK(section.Size() == 8);
        char buffer[16] = {};
        const ReadResult first = section.Read(buffer, 4);
        GO2CPP_CHECK(first.count == 4 && std::string(buffer, 4) == "4567");
        const SeekResult seek = section.Seek(0, kSeekStart);
        GO2CPP_CHECK(seek.ok() && seek.offset == 0);
        const ReadResult after_seek = section.Read(buffer, sizeof(buffer));
        GO2CPP_CHECK(after_seek.count == 8 && std::string(buffer, 8) == "456789AB");
        const ReadResult at = section.ReadAt(buffer, 3, 6);
        GO2CPP_CHECK(at.count == 2 && std::string(buffer, 2) == "AB");   // 越界自动截断
    }

    // ---------------- 场景 8：调试输出丢弃 ----------------
    {
        const WriteResult discarded = Discard().Write("noisy debug line", 16);
        GO2CPP_CHECK(discarded.ok() && discarded.count == 16);
        // Discard 也可作为 TeeReader 的旁路：读到的内容被丢弃，主流程不受影响
        StringReader source("payload");
        TeeReader tee(source, Discard());
        const auto result = ReadAll(tee);
        GO2CPP_CHECK(!result.second && result.first == "payload");
    }

    // ---------------- 语义边界 ----------------
    {
        char buffer[8] = {};
        StringReader source("abc");
        // size < minimum → ErrShortBuffer
        const ReadResult short_buffer = ReadAtLeast(source, buffer, 2, 3);
        GO2CPP_CHECK(short_buffer.count == 0 && Is(short_buffer.error, IoErrShortBuffer()));
        // 数据不足 → ErrUnexpectedEOF
        const ReadResult unexpected = ReadFull(source, buffer, 8);
        GO2CPP_CHECK(unexpected.count == 3 && Is(unexpected.error, IoErrUnexpectedEof()));
        // 空源 → EOF（不是 UnexpectedEOF）
        StringReader empty("");
        const ReadResult eof = ReadFull(empty, buffer, 4);
        GO2CPP_CHECK(eof.count == 0 && Is(eof.error, IoEof()));
        // CopyN 源不足 → EOF
        StringReader small("ab");
        BufferWriter sink;
        const ReadResult short_copy = CopyN(sink, small, 5);
        GO2CPP_CHECK(short_copy.count == 2 && Is(short_copy.error, IoEof()));
        // ReadAll 把 EOF 归一化为成功
        StringReader again("xyz");
        const auto all = ReadAll(again);
        GO2CPP_CHECK(!all.second && all.first == "xyz");
        // CopyBuffer 的短写检测
        class ShortWriter final : public Writer {
        public:
            WriteResult Write(const char* data, std::size_t size) override {
                (void)data;
                return WriteResult{size > 2 ? 2 : size, {}};
            }
        };
        ShortWriter short_writer;
        StringReader long_source("0123456789");
        std::vector<char> copy_buffer(4);
        const ReadResult short_write =
            CopyBuffer(short_writer, long_source, copy_buffer.data(), copy_buffer.size());
        GO2CPP_CHECK(Is(short_write.error, IoErrShortWrite()));
    }

    // ---------------- 大数据量 1：10 万次小块读写 ----------------
    {
        constexpr std::size_t kBlocks = 100000;
        BufferWriter writer;
        const std::string block(64, 'q');
        for (std::size_t i = 0; i < kBlocks; ++i) {
            const WriteResult written = writer.Write(block.data(), block.size());
            if (!written.ok() || written.count != block.size()) {
                GO2CPP_CHECK(false);
                break;
            }
        }
        GO2CPP_CHECK(writer.Len() == kBlocks * block.size());
        const auto back = ReadAll(writer);
        GO2CPP_CHECK(!back.second && back.first.size() == kBlocks * block.size());
    }

    // ---------------- 大数据量 2：10 万帧定长解析（含 DripReader 最恶劣短读） ----------------
    {
        constexpr std::size_t kFrames = 20000;
        std::string framed(kFrames * 4, 'z');
        DripReader drip(framed);
        char frame[4] = {};
        std::size_t frames = 0;
        while (true) {
            const ReadResult result = ReadFull(drip, frame, sizeof(frame));
            if (result.error) break;
            ++frames;
        }
        GO2CPP_CHECK(frames == kFrames);
    }

    // ---------------- 随机 1：随机分块源 → Copy 后必须逐字节一致 ----------------
    {
        std::mt19937 rng(20261015U);
        std::uniform_int_distribution<std::size_t> chunk_dist(1, 37);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 300; ++iteration) {
            const std::string payload = MakePayload(static_cast<std::size_t>(iteration) * 97 + 13);
            ChunkedReader source(payload, chunk_dist(rng));
            BufferWriter destination;
            const ReadResult copied = Copy(destination, source);
            if (!copied.ok() || copied.count != payload.size() ||
                destination.String() != payload) {
                ++mismatches;
            }
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 2：随机偏移的 SectionReader ----------------
    {
        std::mt19937 rng(4242U);
        const std::string payload = MakePayload(5000);
        std::uniform_int_distribution<std::int64_t> offset_dist(0, 4000);
        std::uniform_int_distribution<std::int64_t> limit_dist(1, 1000);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 20000; ++iteration) {
            const std::int64_t offset = offset_dist(rng);
            const std::int64_t limit = limit_dist(rng);
            if (offset + limit > static_cast<std::int64_t>(payload.size())) continue;
            StringReader full(payload);
            SectionReader section(full, offset, limit);
            const auto read = ReadAll(section);
            const std::string expected = payload.substr(static_cast<std::size_t>(offset),
                                                        static_cast<std::size_t>(limit));
            if (read.second || read.first != expected) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }

    // ---------------- 随机 3：随机 LimitReader 上限 ----------------
    {
        std::mt19937 rng(31337U);
        const std::string payload = MakePayload(3000);
        std::uniform_int_distribution<std::int64_t> limit_dist(0, 3500);
        std::int64_t mismatches = 0;
        for (int iteration = 0; iteration < 20000; ++iteration) {
            const std::int64_t limit = limit_dist(rng);
            StringReader full(payload);
            LimitedReader limited(full, limit);
            const auto read = ReadAll(limited);
            const std::size_t expected =
                limit < 0 ? 0 : static_cast<std::size_t>(limit) < payload.size()
                                   ? static_cast<std::size_t>(limit)
                                   : payload.size();
            if (read.second || read.first.size() != expected) ++mismatches;
            if (read.first != payload.substr(0, expected)) ++mismatches;
        }
        GO2CPP_CHECK(mismatches == 0);
    }
}
