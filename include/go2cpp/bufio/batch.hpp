/**
 * @file batch.hpp
 * @brief `bufio::Writer` 的批量 flush 策略：`FlushPolicy` / `BatchConfig` /
 *        `BatchEntry`。
 *
 * @details `bufio.Writer` 原本只有「缓冲满或显式 `Flush()`」一条路径，每次
 *   `Flush()` 对应一次底层 `Write`。高频小消息（16~256 B）场景下，一次
 *   `Write` 只承载一条消息，系统调用次数与消息数 1:1，`send/write` 的
 *   固定开销（~0.3~1 µs）完全主导吞吐。
 *
 *   本文件把「何时把多条消息合并成一次批量写」变成**用户可调的策略**：
 *
 *   | 策略 | 触发条件 | 适用场景 |
 *   |---|---|---|
 *   | `kOnFull` | 缓冲达到 `threshold`（下一次写之前）或段数达到 `max_batch` | 纯吞吐优先，不关心尾部延迟 |
 *   | `kInterval` | 距上次 flush 超过 `interval`，或硬上限（段数/容量） | 低流量也要有界延迟（如游戏状态包） |
 *   | `kOnExplicit` | 只有显式 `Flush()` / `FlushBatch()`（硬上限除外） | 调用方自己掌握批次边界（RPC 批量） |
 *   | `kHybrid` | 上述「满」**或**「超时」任一满足（默认，推荐） | 通用：高流量自动攒批，低流量靠超时兜底 |
 *
 *   **硬上限对所有策略生效**：缓冲放不下、段数达到 `max_batch` 时必须
 *   flush，否则 `Write` 无法继续接受数据。`kInterval` / `kHybrid` 的定时
 *   检查是**惰性**的（在 `Write`/`WriteByte` 返回前检查），没有后台线程：
 *   数据写完就停时必须显式 `Flush()`/`FlushBatch()` 收尾。
 *
 *   **定时精度**：`interval` 是「两次检查之间的最小间隔」，实际触发时机
 *   取决于下一次写入；高频写入时误差为 µs 级，空闲时不会有线程替你 flush。
 *
 *   ## 段（batch entry）是什么
 *
 *   一次 `Write`/`WriteString` 调用（或一串连续 `WriteByte`）在缓冲中对应
 *   一个**段**。`FlushBatch()` 会把当前所有段通过底层可选的
 *   `WriteBatch(const BatchEntry*, size_t)` 一次交出（典型实现是 `writev`/
 *   `sendmmsg`），从而把 N 条逻辑消息压成 1 次系统调用。若底层没有
 *   `WriteBatch`，数据仍在同一块连续缓冲里，退化为一次 `Write`——系统
 *   调用次数同样是 1，只是少了「逐段边界」信息。
 *
 *   **段边界不改变字节流语义**：对 TCP 这类字节流，接收端看到的是按写入
 *   顺序拼接的字节，段只影响写入侧一次系统调用的 iovec 数量。
 */
#pragma once

#include <chrono>
#include <cstddef>

#include "go2cpp/bufio/limits.hpp"

namespace go2cpp::bufio {

/**
 * @brief 批量 flush 的触发策略。
 * @note 枚举值显式编号，便于配置/日志里稳定序列化。
 */
enum class FlushPolicy {
    /// 缓冲达到 `threshold`（下一次写之前）或段数达到 `max_batch` 时 flush。
    kOnFull = 0,
    /// 距上次 flush 超过 `interval` 时 flush；容量/段数硬上限仍然生效。
    kInterval = 1,
    /// 只有显式 `Flush()` / `FlushBatch()` 才写；容量/段数硬上限仍然生效。
    kOnExplicit = 2,
    /// 「满」或「超时」任一满足即 flush（默认，推荐）。
    kHybrid = 3,
};

/// `max_batch` 的默认值：一次最多合并 64 条逻辑写。
inline constexpr std::size_t kDefaultMaxBatch = 64;

/// `max_batch` 的上限：与 Linux `IOV_MAX`（1024）对齐，避免一次 `writev` 超限。
inline constexpr std::size_t kMaxBatchEntries = 1024;

/// `threshold` 的默认值：与缓冲容量默认值一致（4096）。
inline constexpr std::size_t kDefaultBatchThreshold = kDefaultBufferSize;

/// `interval` 的默认值：100 µs，兼顾低流量延迟与合并率。
inline constexpr std::chrono::microseconds kDefaultFlushInterval{100};

/**
 * @brief 批量 flush 的可调参数。
 *
 * @details 默认值即推荐配置：`kHybrid` + 64 条 + 4096 B + 100 µs。
 *   `threshold` 会按缓冲容量钳制（见 `Writer::SetBatchConfig`）：
 *   容量是硬边界，水位不可能超过容量。
 */
struct BatchConfig {
    /// 触发策略。
    FlushPolicy policy = FlushPolicy::kHybrid;
    /// 一次 flush 最多合并的逻辑段数（`1..kMaxBatchEntries`）。
    std::size_t max_batch = kDefaultMaxBatch;
    /// 缓冲水位：已有数据达到该值后，下一次写入前先 flush（`>= 1`）。
    std::size_t threshold = kDefaultBatchThreshold;
    /// 定时 flush 间隔（`>= 0`；0 表示每次写完都检查并 flush）。
    std::chrono::microseconds interval = kDefaultFlushInterval;
};

/**
 * @brief 批量写中的一个逻辑段（一次 `Write`/`WriteString` 或一串
 *        `WriteByte`）。
 *
 * @details 底层若实现 `streams::WriteResult WriteBatch(const BatchEntry*,
 *   std::size_t)`，`Writer::FlushBatch()` 会把待发段整理成该数组一次交出。
 *   `data` 指向 `Writer` 内部缓冲，**仅在本次 `WriteBatch` 调用期间有效**，
 *   底层不得保存指针（`Writer` 返回后可能被后续写覆盖）。
 */
struct BatchEntry {
    const char* data{nullptr};  ///< 段首地址（Writer 内部缓冲，不拥有）。
    std::size_t size{0};        ///< 段长度（字节，> 0）。
};

}  // namespace go2cpp::bufio
