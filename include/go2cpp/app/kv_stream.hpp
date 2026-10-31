#pragma once

/**
 * @file kv_stream.hpp
 * @brief 通用 KV 流：把 `k1=v1;...;k5=v5` 映射到流表（小表/大表/每键一流）。
 *
 * @details 这是「通用应用层框架」面向业务的最上层。需求原文是
 *          「把 `k1=v1;k2=v2;k3=v3;k4=v4;k5=v5` 以流表方式传输」，本文件给出
 *          **三种可组合的承载方式**：
 *
 *          | 模式 | 线格式 | 适用 |
 *          |------|--------|------|
 *          | 一次性（小 KV） | 单 HEADERS 帧 + END_STREAM | ≤ 4 KiB 的元数据/命令 |
 *          | 流式（大 KV） | HEADERS（小值）+ 若干 DATA（大值子表）+ END_STREAM | 配置/清单/大字段 |
 *          | 每键一流 | 每个 k=v 一条独立 stream | 需要独立取消/优先级/并发的键 |
 *
 *          三种模式共用同一套 HEADERS/DATA 帧与同一个流表，**不需要新帧类型**；
 *          接收端从 HEADERS 的 `app.kv.mode` 就能分辨。这正是把业务需求
 *          「总结为通用东西」的落点：KV 只是流表的一种用法。
 *
 * ## 线格式
 *
 *          HEADERS 载荷 = `KvTable` 二进制编码，额外带：
 *
 *          - `app.kind=kv`：流类型；
 *          - `app.kv.mode=single|stream|perkey`：承载模式；
 *          - `app.kv.partial=1`：还有大值在后续 DATA 帧里；
 *          - `app.kv.keys=<n>`：逻辑条目总数（perkey 模式为 1）。
 *
 *          DATA 载荷（仅 stream 模式）= 大值条目的 `KvTable` 二进制编码，
 *          可以跨任意帧边界；接收端拼接后解析并合并进 HEADERS 表。
 *
 * ## 小/大值的判定
 *
 *          值长度 ≤ `small_value_limit`（默认 4 KiB）进入 HEADERS；更大的
 *          值进入 DATA。若 HEADERS 编码后仍超过单帧上限（例如 1000 个小键），
 *          生产者会**自动把更多条目移入 DATA**，保证 HEADERS 永远装得下。
 *          因此「混合」不需要调用方手动分区。
 *
 * ## 接收端
 *
 *          `KvAssembler` 由 `Connection` 在内部驱动：HEADERS 到达时建立，
 *          DATA 到达时追加，END_STREAM 时 `Finish` 产出完整 `KvTable`。
 *          对外只暴露一个 `kKvComplete` 事件，业务代码不需要关心分帧。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/app/frame.hpp"
#include "go2cpp/app/kv.hpp"
#include "go2cpp/app/stream_table.hpp"

namespace go2cpp::app {

/// 框架保留键：流类型。
inline constexpr char kAppKindKey[] = "app.kind";
/// 框架保留键：KV 承载模式。
inline constexpr char kKvModeKey[] = "app.kv.mode";
/// 框架保留键：存在后续大值。
inline constexpr char kKvPartialKey[] = "app.kv.partial";
/// 框架保留键：逻辑键数量。
inline constexpr char kKvKeysKey[] = "app.kv.keys";
/// 框架保留键：资源元数据（与 `app.kind=resource` 配套）。
inline constexpr char kAppNameKey[] = "app.name";
inline constexpr char kAppSizeKey[] = "app.size";
inline constexpr char kAppSha256Key[] = "app.sha256";
inline constexpr char kAppChunkSizeKey[] = "app.chunk_size";
inline constexpr char kAppChunksKey[] = "app.chunks";
inline constexpr char kAppCrc32Key[] = "app.crc32";
inline constexpr char kAppVersionKey[] = "app.version";

/// 默认小 KV 阈值：4 KiB。
inline constexpr std::size_t kDefaultSmallKvThreshold = 4096;

/// 默认小值上限：4 KiB（超过就走 DATA）。
inline constexpr std::size_t kDefaultSmallValueLimit = 4096;

/// KV 流承载模式。
enum class KvMode : std::uint8_t {
    kSingle = 0,  ///< 单 HEADERS（一次性）。
    kStream = 1,  ///< HEADERS + DATA（流式）。
    kPerKey = 2,  ///< 每键一条流。
};

/// @brief 模式名（`single`/`stream`/`perkey`）。
const char* KvModeName(KvMode mode) noexcept;
/// @brief 解析模式名。
bool ParseKvMode(std::string_view name, KvMode* mode) noexcept;

/**
 * @brief KV 发送源：把一张表按 `single`/`stream` 模式拆帧。
 *
 * @details 由 `Connection::SendKv` 创建并挂到流上。生产者保证：
 *          - HEADERS 帧永远不超过 `max_payload`（自动扩大 DATA 分区）；
 *          - 最后一帧带 END_STREAM；
 *          - `done()` 在最后一帧生产后返回 true。
 */
class KvProducer final : public FrameProducer {
   public:
    /**
     * @param table 待发送表。
     * @param small_threshold 小 KV 阈值（整表二进制 ≤ 该值 → 单帧）。
     * @param small_value_limit 单值进入 HEADERS 的上限。
     * @param forced_mode 强制模式；`kPerKey` 时即使单键也标记为 perkey，
     *        `kSingle` 时按大小自动选择 single/stream。
     */
    KvProducer(KvTable table, std::size_t small_threshold = kDefaultSmallKvThreshold,
               std::size_t small_value_limit = kDefaultSmallValueLimit,
               KvMode forced_mode = KvMode::kSingle);

    bool Produce(std::uint32_t max_payload, Frame* out) override;
    bool done() const override { return m_done; }
    std::uint64_t produced_bytes() const override { return m_produced_bytes; }

    /// @brief 实际选择的模式。
    KvMode mode() const noexcept { return m_mode; }
    /// @brief 逻辑条目数（不含保留键）。
    std::size_t key_count() const noexcept { return m_key_count; }

   private:
    bool ProduceHeaders(std::uint32_t max_payload, Frame* out);
    bool ProduceData(std::uint32_t max_payload, Frame* out);
    void Partition(std::uint32_t max_payload);

    KvTable m_table;                       ///< 原始表。
    std::size_t m_small_threshold;         ///< 小表阈值。
    std::size_t m_small_value_limit;       ///< 小值上限。
    KvTable m_header_table;                ///< HEADERS 携带的部分。
    std::vector<std::uint8_t> m_body;      ///< DATA 携带的部分（二进制）。
    std::size_t m_body_offset = 0;         ///< 已发送偏移。
    std::size_t m_key_count = 0;           ///< 逻辑键数。
    KvMode m_mode = KvMode::kSingle;       ///< 承载模式。
    KvMode m_forced_mode = KvMode::kSingle;///< 调用方强制模式。
    bool m_headers_sent = false;           ///< 是否已发 HEADERS。
    bool m_done = false;                   ///< 是否完成。
    std::uint64_t m_produced_bytes = 0;    ///< 已生产字节。
};

/**
 * @brief KV 接收聚合器。
 *
 * @details 接收 HEADERS 初始表与后续 DATA 子表，END_STREAM 时合并。所有
 *          错误都以返回值/`error()` 报告，不抛异常；超过 `max_bytes` 的
 *          输入会被拒绝（防止对端用超大 KV 撑爆内存）。
 */
class KvAssembler {
   public:
    /// @param max_bytes 大值数据总量上限（默认 64 MiB）。
    explicit KvAssembler(std::size_t max_bytes = 64u * 1024u * 1024u);

    /**
     * @brief 处理 HEADERS 表（含 `app.kv.*` 保留键）。
     * @return 成功返回 true；必须含 `app.kind=kv`。
     */
    bool OnHeaders(const KvTable& table, std::string* error = nullptr);

    /**
     * @brief 追加一段 DATA。
     * @return 成功返回 true；超限返回 false。
     */
    bool OnData(const std::uint8_t* data, std::size_t size, std::string* error = nullptr);

    /**
     * @brief 结束并产出合并后的表（已过滤 `app.kv.*` 保留键）。
     * @return 成功返回 true；没有 HEADERS/解析失败返回 false。
     */
    bool Finish(KvTable* out, std::string* error = nullptr);

    /// @brief 承载模式（HEADERS 未到达时返回 single）。
    KvMode mode() const noexcept { return m_mode; }
    /// @brief 是否已收到 HEADERS。
    bool has_headers() const noexcept { return m_has_headers; }
    /// @brief 已缓存的大值字节数。
    std::size_t buffered_bytes() const noexcept { return m_buffer.size(); }
    /// @brief 最近一次错误。
    const std::string& error() const noexcept { return m_error; }

    /// @brief 过滤框架保留键（`app.kv.*` 与 `app.kind`）。
    static void StripReserved(KvTable* table);

   private:
    KvTable m_headers;              ///< HEADERS 表（含保留键）。
    std::vector<std::uint8_t> m_buffer;  ///< DATA 累积。
    std::size_t m_max_bytes;        ///< 上限。
    KvMode m_mode = KvMode::kSingle;///< 模式。
    bool m_has_headers = false;     ///< 是否收到 HEADERS。
    std::string m_error;            ///< 错误。
};

/**
 * @brief 每键一流模式的收集器（把 N 条流的 KV 合并成一张表）。
 *
 * @details `Connection` 每完成一条 perkey 流就投递一个 `kKvComplete` 事件，
 *          业务用本类聚合。`expected` 为 0 时表示按收到的流动态收集。
 */
class KvBatchCollector {
   public:
    /// @param expected 期望的流数（0=不限）。
    explicit KvBatchCollector(std::size_t expected = 0);

    /// @brief 加入一条流的结果。
    void Add(std::uint32_t stream_id, KvTable table);

    /// @brief 是否收齐。
    bool complete() const noexcept {
        return m_expected != 0 && m_entries.size() >= m_expected;
    }
    /// @brief 已收到条目数。
    std::size_t size() const noexcept { return m_entries.size(); }
    /// @brief 期望条目数。
    std::size_t expected() const noexcept { return m_expected; }

    /// @brief 合并为单表（按到达顺序，重复键后者覆盖）。
    KvTable Merge() const;

    /// @brief 原始条目（流 id + 表）。
    const std::vector<std::pair<std::uint32_t, KvTable>>& entries() const noexcept {
        return m_entries;
    }

   private:
    std::size_t m_expected = 0;                                  ///< 期望数量。
    std::vector<std::pair<std::uint32_t, KvTable>> m_entries;    ///< 条目。
};

}  // namespace go2cpp::app
