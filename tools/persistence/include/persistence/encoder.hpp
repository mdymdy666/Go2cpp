#pragma once

/**
 * @file encoder.hpp
 * @brief 面向事件流热路径的定长记录编码器（G2CRPLY v5 兼容）。
 *
 * 为什么不用 `replay::EncodeEvent`：后者每条事件构造一个局部
 * `std::vector<uint8_t> payload`（reserve 64），1e6 条即 1e6 次分配 +
 * 逐字节 push_back，实测约 308 ns/条。资源事件是**定长**的（v4 88B、
 * v5 112B），可以一次 memcpy 写头、一次写 payload、两次 CRC，实测
 * 约 175 ns/条（含 CRC），是后续攒批/异步落盘能超过基线的前提。
 *
 * 约束：
 * - 只处理 `IsResourceEventType()` 为真的定长事件；其它类型返回 0，
 *   由调用方回退到 `EncodeGeneric`；
 * - 输出布局必须与 `replay::EncodeEvent` 逐字节一致（单测用
 *   `DecodeEvent` 校验，压测用 1e6 条全量 CRC 回读校验）；
 * - 不做对齐假设，全部用 memcpy 写小端（x86 上编译为单条 store）。
 */

#include "replay/format.hpp"

#include <cstddef>
#include <cstdint>

namespace go2cpp::persistence {

/// v5 资源事件记录大小：40B 记录头 + 72B payload。
inline constexpr std::size_t kResV5RecordSize = 112;
/// v4 资源事件记录大小：40B 记录头 + 48B payload。
inline constexpr std::size_t kResV4RecordSize = 88;

/// 该事件类型是否是本编码器支持的定长资源事件。
bool IsFixedResourceEvent(go2cpp::replay::EventType type) noexcept;

/// 定长记录字节数；不支持的返回 0。
std::size_t FixedRecordSize(go2cpp::replay::EventType type) noexcept;

/**
 * @brief 编码一条定长资源事件。
 * @param event 事件（seq/ts/vtime 必须已填好；本函数不修改它们）
 * @param dst 输出缓冲区，至少 `FixedRecordSize(type)` 字节
 * @return 写入字节数；类型不支持返回 0（不写 dst）
 */
std::size_t EncodeFixed(const go2cpp::replay::Event& event,
                        std::uint8_t* dst) noexcept;

/**
 * @brief 通用事件编码（回退路径）：先 EncodeEvent 到 thread_local 缓冲再拷贝。
 * @return 写入字节数；容量不足返回 0。
 */
std::size_t EncodeGeneric(const go2cpp::replay::Event& event,
                          std::uint8_t* dst, std::size_t capacity);

/// 廉价逻辑校验值（只混合 seq/type/value/resource_id，不做逐字节哈希）。
/// 写入端累加（每个生产者本地累加，避免共享原子），读取端累加；
/// 因为只做加法，累加顺序无关，多线程写入也能与顺序回读对上。
std::uint64_t MixValue(const go2cpp::replay::Event& event) noexcept;

}  // namespace go2cpp::persistence
