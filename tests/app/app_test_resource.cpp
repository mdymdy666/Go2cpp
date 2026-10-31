/**
 * @file app_test_resource.cpp
 * @brief 资源传输测试：元数据/分块/校验/断点续传/并发/索引/空资源。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/resource.hpp"

namespace {

using namespace go2cpp::app;
using app_test::Pipe;

std::string TempPath(const std::string& name) {
    return "/tmp/app_resource_" + std::to_string(::getpid()) + "_" + name;
}

/// 原地写大端 u32（PutU32 是追加语义，不适合已有缓冲）。
void StoreU32(std::vector<std::uint8_t>& buffer, std::size_t offset,
              std::uint32_t value) {
    buffer[offset] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
    buffer[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    buffer[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    buffer[offset + 3] = static_cast<std::uint8_t>(value & 0xFFu);
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

void RemoveFile(const std::string& path) { std::remove(path.c_str()); }

void MetadataRoundTrip() {
    APP_SCENARIO("resource: metadata k=v round trip + validation");
    ResourceMeta meta;
    meta.name = "hero/body.v1.pak";
    meta.size = 123456;
    meta.sha256_hex = Sha256Hex(reinterpret_cast<const std::uint8_t*>("hello"), 5);
    meta.chunk_size = 64 * 1024;
    meta.chunk_count = meta.size / meta.chunk_size +
                       (meta.size % meta.chunk_size == 0 ? 0 : 1);
    meta.crc32 = 0xdeadbeef;
    meta.version = 3;
    meta.extra.Set("x.asset_type", "mesh");
    meta.extra.Set("x.compression", "zstd");
    const KvTable table = meta.ToKv();
    APP_CHECK(table.Get(kAppKindKey) == "resource");
    APP_CHECK(table.Get("x.asset_type") == "mesh");
    ResourceMeta decoded;
    std::string error;
    APP_REQUIRE(ResourceMeta::FromKv(table, &decoded, &error));
    APP_CHECK(decoded.name == meta.name);
    APP_CHECK(decoded.size == meta.size);
    APP_CHECK(decoded.sha256_hex == meta.sha256_hex);
    APP_CHECK(decoded.chunk_size == meta.chunk_size);
    APP_CHECK(decoded.chunk_count == meta.chunk_count);
    APP_CHECK(decoded.crc32 == meta.crc32);
    APP_CHECK(decoded.version == 3);
    APP_CHECK(decoded.extra.Get("x.compression") == "zstd");
    APP_CHECK(decoded.Validate(&error));
    // 非法元数据。
    KvTable bad = table;
    bad.Set(kAppSha256Key, "not-a-hash");
    ResourceMeta invalid;
    APP_CHECK(!ResourceMeta::FromKv(bad, &invalid, &error));
    KvTable mismatch = table;
    mismatch.Set(kAppChunksKey, std::to_string(meta.chunk_count + 3));
    APP_CHECK(!ResourceMeta::FromKv(mismatch, &invalid, &error));
}

void ChunkedTransferToFile() {
    APP_SCENARIO("resource: 700 KiB chunked transfer + SHA/CRC verify");
    const std::vector<std::uint8_t> data = app_test::MakePattern(700 * 1024, 5);
    MemoryChunkSource source(data);
    const ResourceMeta meta =
        BuildResourceMeta("chunked.bin", source, 64 * 1024, {{"x.kind", "test"}});
    APP_CHECK(meta.chunk_count == 11);
    const std::string path = TempPath("chunked.bin");
    Pipe pipe;
    pipe.server().SetResourceSinkFactory(
        [path](const ResourceMeta&) { return std::make_unique<FileResourceSink>(path); });
    auto shared_source = std::make_shared<MemoryChunkSource>(data);
    const SendResult result = pipe.client().SendResource(meta, shared_source);
    APP_REQUIRE(result.ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 1, 20000));
    const Event* found = app_test::FindLastEvent(pipe.server_events(),
                                                 EventType::kResourceComplete);
    APP_REQUIRE(found != nullptr);
    const Event& event = *found;
    APP_CHECK(event.ok);
    APP_CHECK(event.resource_stats.verified);
    APP_CHECK(event.resource_stats.bytes == data.size());
    APP_CHECK(event.resource_stats.chunks == meta.chunk_count);
    APP_CHECK(event.resource_stats.sha256_hex == meta.sha256_hex);
    const std::vector<std::uint8_t> written = ReadFile(path);
    APP_CHECK(written == data);
    RemoveFile(path);
}

void CorruptChunkRejected() {
    APP_SCENARIO("resource: bad CRC is rejected and counted");
    ResourceMeta meta;
    meta.name = "corrupt.bin";
    meta.size = 1000;
    meta.chunk_size = 64 * 1024;
    meta.chunk_count = 1;
    ResourceReceiver receiver(meta, std::make_unique<MemoryResourceSink>());
    APP_REQUIRE(receiver.error().empty());
    std::vector<std::uint8_t> payload(kResourceChunkHeaderBytes + 1000);
    StoreU32(payload, 0, 0);
    StoreU32(payload, 4, 0);
    StoreU32(payload, 8, 0x12345678);  // 故意错误的 CRC
    const std::vector<std::uint8_t> body = app_test::MakePattern(1000, 3);
    std::copy(body.begin(), body.end(), payload.begin() + kResourceChunkHeaderBytes);
    std::string error;
    APP_CHECK(!receiver.OnData(payload.data(), payload.size(), &error));
    APP_CHECK(!error.empty());
    APP_CHECK(receiver.stats().crc_failures == 1);
    APP_CHECK(receiver.completed_chunks() == 0);

    // 块不连续同样拒绝。
    ResourceMeta two_chunks = meta;
    two_chunks.size = 2 * 64 * 1024;
    two_chunks.chunk_count = 2;
    ResourceReceiver gap_receiver(two_chunks);
    std::vector<std::uint8_t> first(kResourceChunkHeaderBytes + 64 * 1024);
    const std::vector<std::uint8_t> block = app_test::MakePattern(64 * 1024, 9);
    StoreU32(first, 0, 0);
    StoreU32(first, 4, 0);
    StoreU32(first, 8, Crc32Of(block.data(), block.size()));
    std::copy(block.begin(), block.end(), first.begin() + kResourceChunkHeaderBytes);
    APP_REQUIRE(gap_receiver.OnData(first.data(), first.size(), &error));
    std::vector<std::uint8_t> second = first;
    StoreU32(second, 0, 1);
    APP_REQUIRE(gap_receiver.OnData(second.data(), second.size(), &error));
    // 重复块被拒绝。
    APP_CHECK(!gap_receiver.OnData(first.data(), first.size(), &error));
}

void ResumeFromChunk() {
    APP_SCENARIO("resource: resume from chunk N (receiver + producer)");
    const std::vector<std::uint8_t> data = app_test::MakePattern(1024 * 1024, 11);
    MemoryChunkSource source(data);
    ResourceMeta meta = BuildResourceMeta("resume.bin", source, 64 * 1024);
    APP_CHECK(meta.chunk_count == 16);
    const std::string path = TempPath("resume.bin");
    // 第一段：接收前 8 块（不 Finish，模拟连接中断）。
    {
        ResourceProducer producer(meta, std::make_shared<MemoryChunkSource>(data));
        ResourceReceiver receiver(meta, std::make_unique<FileResourceSink>(path));
        Frame frame;
        std::uint32_t guard = 0;
        while (receiver.completed_chunks() < 8 && guard++ < 1000) {
            APP_REQUIRE(producer.Produce(64 * 1024, &frame));
            if (frame.type == FrameType::kData) {
                APP_REQUIRE(receiver.OnData(frame.payload.data(), frame.payload.size(),
                                            nullptr));
            }
        }
        APP_CHECK(receiver.completed_chunks() == 8);
        APP_CHECK(!receiver.finished());
    }
    // 第二段：新 receiver 从第 8 块续传（文件不截断）。
    {
        ResourceProducer producer(meta, std::make_shared<MemoryChunkSource>(data), 8);
        ResourceReceiver receiver(meta,
                                  std::make_unique<FileResourceSink>(path, false), 8);
        Frame frame;
        std::uint32_t guard = 0;
        while (!producer.done() && guard++ < 1000) {
            APP_REQUIRE(producer.Produce(64 * 1024, &frame));
            if (frame.type == FrameType::kData) {
                APP_REQUIRE(receiver.OnData(frame.payload.data(), frame.payload.size(),
                                            nullptr));
            }
        }
        std::string error;
        APP_CHECK(receiver.Finish(&error));
        APP_CHECK(receiver.completed_chunks() == 16);
        APP_CHECK(receiver.stats().chunks == 8);
    }
    const std::vector<std::uint8_t> written = ReadFile(path);
    APP_CHECK(written.size() == data.size());
    APP_CHECK(written == data);
    RemoveFile(path);
}

void ResumeOverConnection() {
    APP_SCENARIO("resource: resume through a fresh connection (app.resume)");
    const std::vector<std::uint8_t> data = app_test::MakePattern(512 * 1024, 21);
    MemoryChunkSource source(data);
    const ResourceMeta meta = BuildResourceMeta("resume2.bin", source, 64 * 1024);
    const std::string path = TempPath("resume2.bin");
    // 先完整收一次，作为续传的已有文件。
    {
        Pipe pipe;
        pipe.server().SetResourceSinkFactory([path](const ResourceMeta&) {
            return std::make_unique<FileResourceSink>(path);
        });
        APP_REQUIRE(pipe.client().SendResource(meta, std::make_shared<MemoryChunkSource>(data))
                        .ok());
        APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 1, 20000));
        const Event* found = app_test::FindLastEvent(pipe.server_events(),
                                                     EventType::kResourceComplete);
        APP_REQUIRE(found != nullptr);
        APP_CHECK(found->ok);
    }
    {
        Pipe pipe;
        pipe.server().SetResourceSinkFactory([path](const ResourceMeta&) {
            return std::make_unique<FileResourceSink>(path, false);
        });
        const SendResult result = pipe.client().SendResource(
            meta, std::make_shared<MemoryChunkSource>(data), 4);
        APP_REQUIRE(result.ok());
        APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 1, 20000));
        const Event* found = app_test::FindLastEvent(pipe.server_events(),
                                                     EventType::kResourceComplete);
        APP_REQUIRE(found != nullptr);
        const Event& event = *found;
        APP_CHECK(event.ok);
        APP_CHECK(event.resource_meta.resume_chunk == 4);
        APP_CHECK(event.resource_stats.chunks == meta.chunk_count - 4);
    }
    APP_CHECK(ReadFile(path) == data);
    RemoveFile(path);
}

void ConcurrentResources() {
    APP_SCENARIO("resource: 8 concurrent resources on separate streams");
    const std::string prefix = TempPath("concurrent");
    std::vector<std::vector<std::uint8_t>> blobs;
    std::vector<ResourceMeta> metas;
    for (int i = 0; i < 8; ++i) {
        blobs.push_back(app_test::MakePattern(80 * 1024 + static_cast<std::size_t>(i) * 977,
                                              static_cast<std::uint32_t>(i + 1)));
        MemoryChunkSource source(blobs.back());
        metas.push_back(BuildResourceMeta("res" + std::to_string(i) + ".bin", source,
                                          64 * 1024));
    }
    Pipe pipe;
    pipe.server().SetResourceSinkFactory([prefix](const ResourceMeta& meta) {
        return std::make_unique<FileResourceSink>(prefix + "_" + meta.name);
    });
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < 8; ++i) {
        auto source = std::make_shared<MemoryChunkSource>(blobs[static_cast<std::size_t>(i)]);
        const SendResult result = pipe.client().SendResource(metas[static_cast<std::size_t>(i)],
                                                             source);
        APP_REQUIRE(result.ok());
        ids.push_back(result.stream_id);
    }
    APP_CHECK(std::set<std::uint32_t>(ids.begin(), ids.end()).size() == 8);
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 8, 30000));
    std::size_t completed = 0;
    for (const Event& event : pipe.server_events()) {
        if (event.type != EventType::kResourceComplete) {
            continue;
        }
        APP_CHECK(event.ok);
        ++completed;
        const std::string path = prefix + "_" + event.resource_meta.name;
        APP_CHECK(ReadFile(path) == blobs[std::stoi(event.resource_meta.name.substr(3, 1))]);
        RemoveFile(path);
    }
    APP_CHECK(completed == 8);
}

void ResourceIndexTransfer() {
    APP_SCENARIO("resource: index frame transfer");
    ResourceIndex index;
    for (int i = 0; i < 3; ++i) {
        ResourceMeta meta;
        meta.name = "idx" + std::to_string(i);
        meta.size = 1000 + static_cast<std::uint64_t>(i);
        meta.chunk_size = 64 * 1024;
        meta.chunk_count = 1;
        meta.sha256_hex = Sha256Hex(reinterpret_cast<const std::uint8_t*>("abc"), 3);
        index.resources.push_back(meta);
    }
    const std::vector<std::uint8_t> encoded = index.Encode();
    ResourceIndex decoded;
    std::string error;
    APP_REQUIRE(ResourceIndex::Decode(encoded.data(), encoded.size(), &decoded, &error));
    APP_REQUIRE(decoded.resources.size() == 3);
    APP_CHECK(decoded.resources[2].name == "idx2");
    APP_CHECK(decoded.resources[1].size == 1001);
    Pipe pipe;
    APP_REQUIRE(pipe.client().SendResourceIndex(index).ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceIndex, 1, 5000));
    const Event* found = app_test::FindLastEvent(pipe.server_events(),
                                                 EventType::kResourceIndex);
    APP_REQUIRE(found != nullptr);
    const Event& event = *found;
    APP_CHECK(event.resource_index.resources.size() == 3);
    APP_CHECK(event.resource_index.resources[0].name == "idx0");
}

void EmptyResource() {
    APP_SCENARIO("resource: zero-byte resource completes");
    std::vector<std::uint8_t> empty;
    MemoryChunkSource source(empty);
    const ResourceMeta meta = BuildResourceMeta("empty.bin", source, 64 * 1024);
    APP_CHECK(meta.size == 0);
    APP_CHECK(meta.chunk_count == 0);
    APP_CHECK(meta.sha256_hex.size() == 64);
    Pipe pipe;
    APP_REQUIRE(pipe.client().SendResource(meta, std::make_shared<MemoryChunkSource>(empty))
                    .ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 1, 5000));
    const Event* found = app_test::FindLastEvent(pipe.server_events(),
                                                 EventType::kResourceComplete);
    APP_REQUIRE(found != nullptr);
    const Event& event = *found;
    APP_CHECK(event.ok);
    APP_CHECK(event.resource_stats.bytes == 0);
    APP_CHECK(event.resource_stats.chunks == 0);
    APP_CHECK(event.resource_stats.verified);
}

void SmallFrameManyPieces() {
    APP_SCENARIO("resource: 300 KiB over 8 KiB frames");
    ConnectionConfig config;
    config.max_frame_payload = 8 * 1024;
    config.table.max_frame_payload = 8 * 1024;
    config.max_bytes_per_flush = 64 * 1024;
    Pipe pipe(config, config);
    const std::vector<std::uint8_t> data = app_test::MakePattern(300 * 1024, 13);
    MemoryChunkSource source(data);
    const ResourceMeta meta = BuildResourceMeta("pieces.bin", source, 64 * 1024);
    const std::string path = TempPath("pieces.bin");
    pipe.server().SetResourceSinkFactory(
        [path](const ResourceMeta&) { return std::make_unique<FileResourceSink>(path); });
    APP_REQUIRE(pipe.client().SendResource(meta, std::make_shared<MemoryChunkSource>(data))
                    .ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 1, 20000));
    const Event* found = app_test::FindLastEvent(pipe.server_events(),
                                                 EventType::kResourceComplete);
    APP_REQUIRE(found != nullptr);
    APP_CHECK(found->ok);
    APP_CHECK(ReadFile(path) == data);
    RemoveFile(path);
}

void ThroughputSmoke() {
    APP_SCENARIO("resource: 4 MiB discard-sink throughput");
    const std::vector<std::uint8_t> data = app_test::MakePattern(4 * 1024 * 1024, 17);
    MemoryChunkSource source(data);
    const ResourceMeta meta = BuildResourceMeta("throughput.bin", source, 512 * 1024);
    Pipe pipe;
    const std::uint64_t start = app_test::NowMs();
    APP_REQUIRE(pipe.client().SendResource(meta, std::make_shared<MemoryChunkSource>(data))
                    .ok());
    APP_REQUIRE(pipe.WaitEvents(true, EventType::kResourceComplete, 1, 30000));
    const std::uint64_t elapsed = std::max<std::uint64_t>(1, app_test::NowMs() - start);
    const Event* found = app_test::FindLastEvent(pipe.server_events(),
                                                 EventType::kResourceComplete);
    APP_REQUIRE(found != nullptr);
    const Event& event = *found;
    APP_CHECK(event.ok);
    APP_CHECK(event.resource_stats.bytes == data.size());
    const double mbps =
        (static_cast<double>(data.size()) / (1024.0 * 1024.0)) /
        (static_cast<double>(elapsed) / 1000.0);
    app_test::Note("4 MiB transfer: " + std::to_string(elapsed) + " ms, " +
                   std::to_string(mbps) + " MiB/s");
    APP_CHECK(mbps > 1.0);
}

}  // namespace

int main() {
    MetadataRoundTrip();
    ChunkedTransferToFile();
    CorruptChunkRejected();
    ResumeFromChunk();
    ResumeOverConnection();
    ConcurrentResources();
    ResourceIndexTransfer();
    EmptyResource();
    SmallFrameManyPieces();
    ThroughputSmoke();
    return app_test::Finish("app_test_resource");
}
