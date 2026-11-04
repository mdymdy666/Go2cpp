#pragma once

/**
 * @file protocol.hpp
 * @brief RPC 线上协议：在 `frame::FrameCodec` 之上定义请求/响应/流帧。
 *
 * @details
 * 帧层（`go2cpp::frame`）只负责「magic/version/长度/CRC + 半包重组」，本层
 * 决定每帧载荷怎么解释。设计目标是把**一次调用**压缩到最少字节：
 *
 * ```text
 * Request   : call_id(varint) | method(string) | args_len(varint) | args
 * Response  : call_id(varint) | status(u8) | message(string) | result_len(varint) | result
 * StreamData: call_id(varint) | seq(varint)  | len(varint) | payload
 * StreamEnd : call_id(varint) | status(u8)   | message(string)
 * Cancel    : call_id(varint)
 * Ping/Pong : call_id(varint)
 * ```
 *
 * - `call_id` 由客户端分配，同一条连接内唯一；服务端在响应里原样回填，
 *   客户端读线程据此把响应分派给等待的调用（多路复用）。
 * - `args` 是各参数 `Serialize` 结果的顺序拼接（每个参数前有 varint 长度）。
 *   参数类型/个数由服务端注册签名决定，客户端编译期调用 `Call<Result>(...)`
 *   时按同一签名打包。
 * - `status` 用 `StatusCode` 的 1 字节；`message` 只在错误时非空。
 * - `streaming` 放帧头 `flags` 的 bit0，不占载荷字节。
 * - 所有整数/长度都是 varint（LEB128），小整数 1 字节。
 *
 * ## 为什么不用 ReflectCodec 的 envelope
 *
 * `ReflectCodec` 的消息外壳会携带 `TypeId + 类型名 + magic`，适合「自描述消息
 * 路由」；RPC 的方法名已经承担路由职责，再加类型名是重复开销。这里只用
 * Reflect 的**字段序列化**（`Serialize/Deserialize`），不传类型名。
 *
 * ## 边界与校验
 *
 * 解码是严格的：长度必须与剩余字节完全相等、状态码必须在 0..15、
 * 方法名/错误文本有上限。任何畸形帧都返回 false，调用方应断开连接——
 * 帧层已经保证 CRC 正确，载荷语义错误只可能来自对端实现或协议版本不一致。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "go2cpp/frame/frame_codec.hpp"
#include "go2cpp/distributed/trace.hpp"
#include "go2cpp/reflect/buffer.hpp"
#include "go2cpp/rpc/status.hpp"

namespace go2cpp::rpc {

/// @brief 协议版本（写进帧头 `version`，不做协商降级）。
inline constexpr std::uint8_t kProtocolVersion = 1;
/// @brief 帧头 type 取值（本层语义）。
enum class FrameType : std::uint8_t {
    kRequest = 1,    ///< 客户端 → 服务端：一次调用。
    kResponse = 2,   ///< 服务端 → 客户端：调用结果。
    kStreamData = 3, ///< 服务端 → 客户端：流中的一条消息。
    kStreamEnd = 4,  ///< 服务端 → 客户端：流结束（带最终状态）。
    kCancel = 5,     ///< 客户端 → 服务端：取消某次调用/流。
    kPing = 6,       ///< 客户端 → 服务端：心跳。
    kPong = 7,       ///< 服务端 → 客户端：心跳回应。
};

/// @brief 请求帧的 flags：该调用是服务端流式。
inline constexpr std::uint16_t kFlagStreaming = 0x0001;
/// @brief 请求帧的 flags：载荷尾部带 32 字节追踪上下文（P0 新增）。
inline constexpr std::uint16_t kFlagTrace = 0x0002;
/// @brief 方法名上限（含 `"服务.方法"` 分隔符）。
inline constexpr std::size_t kMaxMethodBytes = 128;
/// @brief 错误文本上限（防止对端塞大段文本）。
inline constexpr std::size_t kMaxStatusMessageBytes = 4096;

/// @brief 帧类型短名（日志用）。
const char* FrameTypeName(FrameType type) noexcept;

/// @brief 解析后的请求。
struct RequestMessage {
    std::uint64_t call_id = 0;  ///< 调用 id。
    std::string method;         ///< 完整方法名。
    bool streaming = false;     ///< 是否流式请求。
    /// 追踪上下文；`has_trace == false` 时无效（旧客户端/未埋点调用）。
    distributed::TraceContext trace;
    /// 是否携带追踪上下文（对应 flags 的 kFlagTrace 位）。
    bool has_trace = false;
    reflect::Buffer args;       ///< 参数区（按签名顺序，带长度前缀的副本）。
};

/// @brief 解析后的响应。
struct ResponseMessage {
    std::uint64_t call_id = 0;  ///< 调用 id。
    Status status;              ///< 执行状态。
    reflect::Buffer payload;    ///< 结果字节（成功时可能有，失败时为空）。
};

/// @brief 解析后的流数据帧。
struct StreamDataMessage {
    std::uint64_t call_id = 0;            ///< 流 id。
    std::uint64_t sequence = 0;           ///< 序号（从 0 递增）。
    std::vector<std::uint8_t> payload;    ///< 消息字节。
};

/// @brief 解析后的流结束帧。
struct StreamEndMessage {
    std::uint64_t call_id = 0;  ///< 流 id。
    Status status;              ///< 最终状态。
};

/// @brief 编码请求载荷（不含帧头）。
/// @param out 输出缓冲（清零后写入）。
/// @param call_id 调用 id。
/// @param method 完整方法名。
/// @param args 参数区（可为空）。
/// @return 成功 true；方法名超限返回 false（`out` 为空）。
bool EncodeRequest(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                   std::string_view method, const reflect::Buffer& args);

/**
 * @brief 编码请求载荷并在尾部附带追踪上下文（调用方须在 flags 置 kFlagTrace）。
 * @param out [out] 输出缓冲（清零后写入）。
 * @param call_id [in] 调用 id。
 * @param method [in] 完整方法名；空或超限返回 false。
 * @param args [in] 参数区（可为空）。
 * @param trace [in] 追踪上下文；无效时不追加字节（等价旧编码）。
 * @return 成功 true；方法名超限返回 false（`out` 为空）。
 */
bool EncodeRequest(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                   std::string_view method, const reflect::Buffer& args,
                   const distributed::TraceContext& trace);

/// @brief 编码响应载荷（不含帧头）。
/// @return 成功 true；错误文本超限返回 false。
bool EncodeResponse(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                    const Status& status, const reflect::Buffer& payload);

/// @brief 编码流数据载荷。
/// @param out 输出缓冲。
/// @param call_id 流 id。
/// @param sequence 序号。
/// @param payload 消息字节（可为空）。
/// @param size 消息字节数。
/// @return 总是成功（长度上限由帧层 `max_payload` 保证）。
bool EncodeStreamData(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                      std::uint64_t sequence, const void* payload, std::size_t size);

/// @brief 编码流结束载荷。
bool EncodeStreamEnd(std::vector<std::uint8_t>& out, std::uint64_t call_id,
                     const Status& status);

/// @brief 编码取消帧载荷。
bool EncodeCancel(std::vector<std::uint8_t>& out, std::uint64_t call_id);

/// @brief 编码心跳帧载荷（Ping/Pong 共用）。
bool EncodePing(std::vector<std::uint8_t>& out, std::uint64_t call_id, bool pong);

/**
 * @brief 解析请求帧。
 * @param frame 已解出的完整帧（`type` 必须是 `kRequest`）。
 * @param out 输出；失败时内容未定义。
 * @param error 失败原因（可为 nullptr）。
 * @return 成功 true。
 */
bool DecodeRequest(const frame::Frame& frame, RequestMessage* out, std::string* error);

/// @brief 解析响应帧。
bool DecodeResponse(const frame::Frame& frame, ResponseMessage* out, std::string* error);

/// @brief 解析流数据帧。
bool DecodeStreamData(const frame::Frame& frame, StreamDataMessage* out, std::string* error);

/// @brief 解析流结束帧。
bool DecodeStreamEnd(const frame::Frame& frame, StreamEndMessage* out, std::string* error);

/// @brief 解析只有 `call_id` 的帧（Cancel/Ping/Pong）。
bool DecodeCallId(const frame::Frame& frame, std::uint64_t* call_id, std::string* error);

}  // namespace go2cpp::rpc
