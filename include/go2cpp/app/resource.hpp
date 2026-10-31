#pragma once

/**
 * @file resource.hpp
 * @brief 游戏资源传输：元数据 + 分块 + 校验 + 断点续传 + 资源索引。
 *
 * @details 本文件把「一次资源下发」拆成可组合的部件：
 *
 *          ```
 *          ChunkSource ──> ResourceMeta（name/size/sha256/chunks/chunk_size）
 *                 │                    │
 *                 │              HEADERS 帧（k=v 元数据）
 *                 v                    v
 *          ResourceProducer ──> DATA 帧（块头 12B + 数据）──> ResourceReceiver
 *                                                             │
 *                                                      ResourceSink（内存/文件/丢弃）
 *          ```
 *
 *          设计直接对照真实游戏资源分发（CDN 分块 + 内容寻址 + 断点续传）：
 *
 *          - **内容寻址**：整资源 SHA-256；块级 CRC-32。二者都用仓库已有的
 *            `go2cpp::hash` 实现，不引入 OpenSSL 依赖；
 *          - **固定分块**：默认 512 KiB，可配置 64 KiB..4 MiB。块大小同时决定
 *            断点续传粒度与单块校验粒度；
 *          - **块头**：每个 DATA 载荷前 12 字节 = `chunk_index(4) + offset(4)
 *            + crc32(4)`。块可以跨多个 DATA 帧（受流控窗口与单帧上限约束），
 *            接收端按 `chunk_index + offset` 连续重组；
 *          - **断点续传**：`ResourceReceiver::completed_chunks()` 给出连续完成
 *            的块数；发送端用 `ResourceProducer(meta, source, start_chunk)`
 *            从该块继续。块头里的 index/offset 让接收端能明确拒绝跳块/重叠块；
 *          - **并发**：多资源各占一条 stream，由流表的连接/流两级窗口与 WFQ
 *            调度保证互不饿死（见 `stream_table.hpp`）。
 *
 * ## 资源元数据的 k=v 形式
 *
 *          ```
 *          app.kind=resource
 *          app.name=hero/body.v1.pak
 *          app.size=104857600
 *          app.sha256=<64 hex>
 *          app.chunk_size=524288
 *          app.chunks=200
 *          app.crc32=3735928559
 *          x.asset_type=mesh
 *          ```
 *
 *          `app.*` 是框架保留键，其余键（上面 `x.asset_type`）原样进入
 *          `ResourceMeta::extra`，可用于版本、CDN 标签、压缩算法等业务字段。
 *
 * ## 校验失败的处理
 *
 *          块 CRC 不符 → `OnData` 返回 false 并记录错误；连接层用
 *          `RST_STREAM(kChecksumError)` 取消这条流。整资源 SHA-256 不符 →
 *          `Finish` 返回 false，已写入的数据由调用方决定保留还是丢弃。
 *          **框架不会静默接受坏块**：任何校验失败都必须冒泡到事件层。
 *
 * ## 内存边界
 *
 *          `ResourceReceiver` 只保留「每块已收字节数」的计数（chunk_count
 *          个 u32）与一个 SHA-256 状态，不缓存整资源；1 GiB 传输的接收侧
 *          常驻内存是 O(chunk_count)，而不是 O(size)。发送侧由 `ChunkSource`
 *          按需读取，同样不整体加载。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/app/kv.hpp"
#include "go2cpp/app/stream_table.hpp"

namespace go2cpp::app {

/// 资源 DATA 帧载荷里的块头字节数：index(4) + offset(4) + crc32(4)。
inline constexpr std::size_t kResourceChunkHeaderBytes = 12;

/// 默认资源块大小：512 KiB。
inline constexpr std::uint32_t kDefaultResourceChunkSize = 512u * 1024u;

/// 允许的最小块大小：64 KiB。
inline constexpr std::uint32_t kMinResourceChunkSize = 64u * 1024u;

/// 允许的最大块大小：4 MiB。
inline constexpr std::uint32_t kMaxResourceChunkSize = 4u * 1024u * 1024u;

/// @brief 资源元数据（也对应 HEADERS 帧里的一组 k=v）。
struct ResourceMeta {
    std::string name;                 ///< 资源名（路径形态，业务自定义）。
    std::uint64_t size = 0;           ///< 资源字节数。
    std::string sha256_hex;           ///< 全资源 SHA-256（64 位小写 hex；空=不校验）。
    std::uint32_t chunk_size = kDefaultResourceChunkSize;  ///< 块大小。
    std::uint32_t chunk_count = 0;    ///< 块数量（0 时按 size/chunk_size 推导）。
    std::uint32_t crc32 = 0;          ///< 全资源 CRC-32（可选）。
    std::uint32_t version = 1;        ///< 元数据版本。
    std::uint32_t resume_chunk = 0;   ///< 断点续传起始块（0=从头）。
    KvTable extra;                    ///< 业务自定义键值。

    /// @brief 第 `index` 块的字节数（最后一块可能更短）。
    std::uint32_t ChunkBytes(std::uint32_t index) const noexcept;

    /// @brief 第 `index` 块在资源内的偏移。
    std::uint64_t ChunkOffset(std::uint32_t index) const noexcept {
        return static_cast<std::uint64_t>(index) * chunk_size;
    }

    /// @brief 编码为 k=v 表（`app.*` 保留键 + extra）。
    KvTable ToKv() const;

    /// @brief 从 k=v 表解析；缺字段/非法值时返回 false。
    static bool FromKv(const KvTable& table, ResourceMeta* out,
                       std::string* error = nullptr);

    /// @brief 校验元数据自洽性（size/chunk_size/chunk_count/hash 格式）。
    bool Validate(std::string* error = nullptr) const;
};

/**
 * @brief 资源数据源：按偏移随机读取。
 *
 * @note 实现必须支持乱序/重复读取（断点续传可能从任意块开始）。
 */
class ChunkSource {
   public:
    virtual ~ChunkSource() = default;

    /// @brief 资源总字节数。
    virtual std::uint64_t size() const = 0;

    /**
     * @brief 读取 `[offset, offset + max_bytes)`。
     * @return 实际读取字节数；0 表示 EOF 或错误。
     */
    virtual std::size_t Read(std::uint64_t offset, std::uint8_t* out,
                             std::size_t max_bytes) = 0;

    /// @brief 是否可随机读（文件/内存 true；生成器 true）。
    virtual bool random_access() const { return true; }

    /**
     * @brief 零拷贝视图：若源能直接暴露底层内存，返回 `[offset, offset+max_bytes)`。
     * @details 默认返回 false（调用方用 `Read` 拷贝）。内存源实现它，
     *          发送端可直接对底层缓冲算块 CRC，省掉一次整块拷贝。
     * @param data 输出只读指针（视图生命周期 = 源对象）。
     * @param size 输出可用字节数。
     */
    virtual bool View(std::uint64_t offset, std::size_t max_bytes,
                      const std::uint8_t** data, std::size_t* size) {
        (void)offset;
        (void)max_bytes;
        (void)data;
        (void)size;
        return false;
    }
};

/// @brief 内存资源源（共享底层缓冲，复制廉价）。
class MemoryChunkSource final : public ChunkSource {
   public:
    explicit MemoryChunkSource(std::vector<std::uint8_t> data);
    explicit MemoryChunkSource(std::shared_ptr<const std::vector<std::uint8_t>> data);

    std::uint64_t size() const override;
    std::size_t Read(std::uint64_t offset, std::uint8_t* out,
                     std::size_t max_bytes) override;
    bool random_access() const override { return true; }
    bool View(std::uint64_t offset, std::size_t max_bytes, const std::uint8_t** data,
              std::size_t* size) override;

    /// @brief 底层数据（只读）。
    const std::vector<std::uint8_t>& data() const noexcept { return *m_data; }

   private:
    std::shared_ptr<const std::vector<std::uint8_t>> m_data;  ///< 数据。
};

/// @brief 文件资源源（pread，不整体加载）。
class FileChunkSource final : public ChunkSource {
   public:
    explicit FileChunkSource(std::string path);
    ~FileChunkSource() override;

    FileChunkSource(const FileChunkSource&) = delete;
    FileChunkSource& operator=(const FileChunkSource&) = delete;
    FileChunkSource(FileChunkSource&& other) noexcept;
    FileChunkSource& operator=(FileChunkSource&& other) noexcept;

    /// @brief 打开文件；失败返回 false。
    bool Open(std::string* error = nullptr);
    std::uint64_t size() const override { return m_size; }
    std::size_t Read(std::uint64_t offset, std::uint8_t* out,
                     std::size_t max_bytes) override;
    bool random_access() const override { return true; }
    bool valid() const noexcept { return m_fd >= 0; }

   private:
    std::string m_path;   ///< 路径。
    int m_fd = -1;        ///< 文件描述符。
    std::uint64_t m_size = 0;  ///< 文件大小。
};

/**
 * @brief 确定性字节生成源（零内存；1 GiB 负载测试用）。
 *
 * @details 第 8 字节对齐地使用 LCG 生成，接收端不需要保存也能重算。
 *          `Read` 对任意 offset 都能独立生成，满足随机访问。
 */
class PatternChunkSource final : public ChunkSource {
   public:
    /// @param size 资源字节数。
    /// @param seed 生成种子（不同资源用不同种子）。
    explicit PatternChunkSource(std::uint64_t size, std::uint64_t seed = 0x9e3779b97f4a7c15ULL);

    std::uint64_t size() const override { return m_size; }
    std::size_t Read(std::uint64_t offset, std::uint8_t* out,
                     std::size_t max_bytes) override;
    bool random_access() const override { return true; }

    /// @brief 生成指定偏移的 8 字节值（测试端可重算校验）。
    std::uint64_t ValueAt(std::uint64_t offset8) const noexcept;

   private:
    std::uint64_t m_size;  ///< 总大小。
    std::uint64_t m_seed;  ///< 种子。
};

/// @brief 资源接收落点。
class ResourceSink {
   public:
    virtual ~ResourceSink() = default;

    /// @brief 开始接收（预分配/打开文件）。失败返回 false。
    virtual bool Open(const ResourceMeta& meta, std::string* error);

    /// @brief 写入一段数据（按资源绝对偏移）。失败返回 false。
    virtual bool Write(std::uint64_t offset, const std::uint8_t* data,
                       std::size_t size) = 0;

    /// @brief 收尾（flush/截断）。失败返回 false。
    virtual bool Finish(std::string* error);
};

/// @brief 内存落点（完整缓存资源）。
class MemoryResourceSink final : public ResourceSink {
   public:
    bool Open(const ResourceMeta& meta, std::string* error) override;
    bool Write(std::uint64_t offset, const std::uint8_t* data,
               std::size_t size) override;
    bool Finish(std::string* error) override;

    /// @brief 已收数据。
    const std::vector<std::uint8_t>& data() const noexcept { return m_data; }
    /// @brief 已收字节数。
    std::size_t received() const noexcept { return m_received; }

   private:
    std::vector<std::uint8_t> m_data;  ///< 数据。
    std::size_t m_received = 0;        ///< 已写入字节。
};

/// @brief 文件落点（pwrite，支持断点续传）。
class FileResourceSink final : public ResourceSink {
   public:
    /// @param path 目标路径。
    /// @param truncate 是否清空已有文件（断点续传时传 false）。
    explicit FileResourceSink(std::string path, bool truncate = true);
    ~FileResourceSink() override;

    FileResourceSink(const FileResourceSink&) = delete;
    FileResourceSink& operator=(const FileResourceSink&) = delete;

    bool Open(const ResourceMeta& meta, std::string* error) override;
    bool Write(std::uint64_t offset, const std::uint8_t* data,
               std::size_t size) override;
    bool Finish(std::string* error) override;

    /// @brief 目标路径。
    const std::string& path() const noexcept { return m_path; }

   private:
    std::string m_path;   ///< 路径。
    int m_fd = -1;        ///< 文件描述符。
    std::uint64_t m_size = 0;  ///< 预分配大小。
    bool m_truncate = true;    ///< 是否清空已有文件。
};

/// @brief 丢弃落点（只算哈希；1 GiB 吞吐测试用，常驻内存 O(1)）。
class DiscardResourceSink final : public ResourceSink {
   public:
    bool Open(const ResourceMeta& meta, std::string* error) override;
    bool Write(std::uint64_t offset, const std::uint8_t* data,
               std::size_t size) override;
    bool Finish(std::string* error) override;

    /// @brief 已写入字节数。
    std::uint64_t bytes() const noexcept { return m_bytes; }

   private:
    std::uint64_t m_bytes = 0;  ///< 计数。
};

/// @brief 资源接收统计。
struct ResourceStats {
    std::uint64_t bytes = 0;          ///< 已接收字节（数据，不含块头）。
    std::uint32_t chunks = 0;         ///< 已完成块数。
    std::uint32_t chunks_expected = 0;///< 期望块数。
    std::uint32_t crc_failures = 0;   ///< CRC 失败次数。
    std::string sha256_hex;           ///< 实际计算出的 SHA-256。
    std::uint64_t elapsed_ms = 0;     ///< 耗时。
    double throughput_mbps = 0.0;     ///< 吞吐（MiB/s）。
    bool verified = false;            ///< 是否通过整资源校验。
};

/**
 * @brief 资源发送源：把 `ChunkSource` 切成 HEADERS + DATA 帧。
 *
 * @details 流的发送语义（由 `Connection` 驱动）：
 *
 *          1. 首帧 HEADERS：`ResourceMeta::ToKv()`（含 `app.kind=resource`）；
 *          2. 逐块发送：每块读入 `chunk_size` 缓冲、计算块 CRC，再按
 *             `max_payload - 12` 切成一帧或多帧 DATA；
 *          3. 最后一帧 DATA 置 `END_STREAM`。
 *
 *          `start_chunk > 0` 时跳过前面的块（断点续传）；被跳过的块不会
 *          重新读取，也不会参与发送统计。
 */
class ResourceProducer final : public FrameProducer {
   public:
    /**
     * @param meta 资源元数据（chunk_count 为 0 时自动推导）。
     * @param source 数据源。
     * @param start_chunk 起始块（0=从头）。
     */
    ResourceProducer(ResourceMeta meta, std::shared_ptr<ChunkSource> source,
                     std::uint32_t start_chunk = 0);

    bool Produce(std::uint32_t max_payload, Frame* out) override;
    bool done() const override { return m_done; }
    std::uint64_t produced_bytes() const override { return m_produced_bytes; }

    /// @brief 规范化后的元数据。
    const ResourceMeta& meta() const noexcept { return m_meta; }
    /// @brief 起始块。
    std::uint32_t start_chunk() const noexcept { return m_start_chunk; }

   private:
    bool BuildHeaders(std::uint32_t max_payload, Frame* out);
    /// @brief 载入下一块并选择读写路径（直读 payload / 整块缓冲）。
    bool BeginChunk(std::uint32_t max_payload);

    ResourceMeta m_meta;                    ///< 元数据。
    std::shared_ptr<ChunkSource> m_source;  ///< 数据源。
    std::uint32_t m_start_chunk = 0;        ///< 起始块。
    std::uint32_t m_next_chunk = 0;         ///< 下一块。
    /// 兼容路径的整块缓冲（仅 `random_access()==false` 的源使用）。
    std::vector<std::uint8_t> m_chunk;
    std::uint64_t m_chunk_base = 0;   ///< 当前块在资源中的绝对偏移。
    std::uint32_t m_chunk_bytes = 0;  ///< 当前块总字节数。
    std::uint32_t m_chunk_index = 0;  ///< 当前块索引。
    std::uint32_t m_chunk_offset = 0; ///< 当前块内偏移。
    std::uint32_t m_chunk_crc = 0;    ///< 当前块 CRC。
    bool m_chunk_crc_ready = false;   ///< 当前块 CRC 是否已算出。
    bool m_direct = true;             ///< 源声明支持可重复随机读。
    bool m_chunk_direct = false;      ///< 当前块是否走「逐帧直读 payload」。
    bool m_headers_sent = false;      ///< HEADERS 是否已发。
    bool m_done = false;              ///< 是否完成。
    bool m_empty = false;             ///< 空资源。
    std::uint64_t m_produced_bytes = 0;  ///< 已生产的数据字节。
};

/**
 * @brief 资源接收器：重组块、校验 CRC、计算 SHA-256、支持续传。
 */
class ResourceReceiver {
   public:
    /**
     * @brief 构造。
     * @param meta 资源元数据。
     * @param sink 落点；为空时使用内部丢弃落点。
     * @param start_chunk 续传起始块（前面块视为已完成，不再接收）。
     */
    ResourceReceiver(ResourceMeta meta, std::unique_ptr<ResourceSink> sink = nullptr,
                     std::uint32_t start_chunk = 0);

    ~ResourceReceiver();

    ResourceReceiver(const ResourceReceiver&) = delete;
    ResourceReceiver& operator=(const ResourceReceiver&) = delete;

    /**
     * @brief 处理一段 DATA 载荷（含 12B 块头）。
     * @return 成功返回 true；块头非法/CRC 失败/重复块返回 false。
     */
    bool OnData(const std::uint8_t* payload, std::size_t size,
                std::string* error);

    /// @brief 收尾并校验（块数/size/SHA-256）。
    bool Finish(std::string* error);

    /// @brief 是否已完成。
    bool finished() const noexcept { return m_finished; }
    /// @brief 连续完成的块数（断点续传起点）。
    std::uint32_t completed_chunks() const noexcept { return m_completed_chunks; }
    /// @brief 期望块数。
    std::uint32_t expected_chunks() const noexcept { return m_meta.chunk_count; }
    /// @brief 元数据。
    const ResourceMeta& meta() const noexcept { return m_meta; }
    /// @brief 统计。
    const ResourceStats& stats() const noexcept { return m_stats; }
    /// @brief 错误信息（最近一次失败）。
    const std::string& error() const noexcept { return m_error; }
    /// @brief 实际 SHA-256（完成前为空）。
    const std::string& sha256_hex() const noexcept { return m_stats.sha256_hex; }

   private:
    bool FlushChunk(std::string* error);

    ResourceMeta m_meta;                            ///< 元数据。
    std::unique_ptr<ResourceSink> m_sink;           ///< 落点。
    std::vector<std::uint8_t> m_chunk_buffer;       ///< 当前块缓冲。
    std::uint32_t m_start_chunk = 0;                ///< 续传起点。
    std::uint32_t m_completed_chunks = 0;           ///< 连续完成块数。
    std::uint64_t m_started_at_ms = 0;              ///< 开始时间。
    bool m_finished = false;                        ///< 是否完成。
    bool m_partial = false;                         ///< 是否续传（哈希不完整）。
    struct Sha256State;                             ///< 增量哈希状态（实现私有）。
    std::unique_ptr<Sha256State> m_sha;             ///< 哈希。
    ResourceStats m_stats;                          ///< 统计。
    std::string m_error;                            ///< 错误。
};

/**
 * @brief 扫描数据源，生成完整元数据（SHA-256/CRC/块数）。
 * @param name 资源名。
 * @param source 数据源（会被完整读取一遍）。
 * @param chunk_size 块大小（0=默认）。
 * @param extra 业务附加键值。
 * @param error 非空时写入错误。
 */
ResourceMeta BuildResourceMeta(std::string name, ChunkSource& source,
                               std::uint32_t chunk_size = 0,
                               const KvTable& extra = {},
                               std::string* error = nullptr);

/// @brief 计算一段字节的 SHA-256 小写 hex。
std::string Sha256Hex(const std::uint8_t* data, std::size_t size);
/// @brief 计算一段字节的 CRC-32/IEEE。
std::uint32_t Crc32Of(const std::uint8_t* data, std::size_t size);

/**
 * @brief 资源索引（k=v 形式，随连接下发）。
 *
 * @details 编码为 `count + r<i>.*` 的 KvTable，再由 `kResourceIndex` 帧承载。
 *          客户端可以先拿索引，再决定按优先级/断点下载哪些资源。
 */
class ResourceIndex {
   public:
    /// @brief 资源列表。
    std::vector<ResourceMeta> resources;

    /// @brief 编码为 k=v 表。
    KvTable ToKv() const;

    /// @brief 从 k=v 表解析。
    static bool FromKv(const KvTable& table, ResourceIndex* out,
                       std::string* error = nullptr);

    /// @brief 编码为二进制（`kResourceIndex` 帧载荷）。
    std::vector<std::uint8_t> Encode() const;

    /// @brief 解析二进制。
    static bool Decode(const std::uint8_t* data, std::size_t size, ResourceIndex* out,
                       std::string* error = nullptr);
};

}  // namespace go2cpp::app
