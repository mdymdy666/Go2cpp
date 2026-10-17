/**
 * @file frame.cpp
 * @brief QUIC 帧层实现（RFC 9000 §19）。
 */
#include "go2cpp/network/quic/frame.hpp"

#include "go2cpp/network/quic/varint.hpp"

#include <cstring>
#include <utility>

namespace go2cpp::net::quic {

// ============================================================================
// 字符串转换
// ============================================================================

const char* ToString(FrameType type) noexcept {
    switch (type) {
        case FrameType::kPadding: return "PADDING";
        case FrameType::kPing: return "PING";
        case FrameType::kAck: return "ACK";
        case FrameType::kAckEcn: return "ACK_ECN";
        case FrameType::kResetStream: return "RESET_STREAM";
        case FrameType::kStopSending: return "STOP_SENDING";
        case FrameType::kCrypto: return "CRYPTO";
        case FrameType::kNewToken: return "NEW_TOKEN";
        case FrameType::kStreamBase: return "STREAM";
        case FrameType::kMaxData: return "MAX_DATA";
        case FrameType::kMaxStreamData: return "MAX_STREAM_DATA";
        case FrameType::kMaxStreamsBidi: return "MAX_STREAMS(bidi)";
        case FrameType::kMaxStreamsUni: return "MAX_STREAMS(uni)";
        case FrameType::kDataBlocked: return "DATA_BLOCKED";
        case FrameType::kStreamDataBlocked: return "STREAM_DATA_BLOCKED";
        case FrameType::kStreamsBlockedBidi: return "STREAMS_BLOCKED(bidi)";
        case FrameType::kStreamsBlockedUni: return "STREAMS_BLOCKED(uni)";
        case FrameType::kNewConnectionId: return "NEW_CONNECTION_ID";
        case FrameType::kRetireConnectionId: return "RETIRE_CONNECTION_ID";
        case FrameType::kPathChallenge: return "PATH_CHALLENGE";
        case FrameType::kPathResponse: return "PATH_RESPONSE";
        case FrameType::kConnectionCloseTransport: return "CONNECTION_CLOSE(transport)";
        case FrameType::kConnectionCloseApplication: return "CONNECTION_CLOSE(app)";
        case FrameType::kHandshakeDone: return "HANDSHAKE_DONE";
    }
    return "UNKNOWN";
}

const char* ToString(FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::kOk: return "ok";
        case FrameStatus::kNeedMore: return "need-more";
        case FrameStatus::kFrameEncodingError: return "frame-encoding-error";
        case FrameStatus::kUnknownType: return "unknown-type";
        case FrameStatus::kInvalidArgument: return "invalid-argument";
    }
    return "?";
}

namespace {

/// 把一个帧类型码分类。
FrameType ClassifyType(std::uint8_t code) noexcept {
    if (IsStreamType(code)) {
        return FrameType::kStreamBase;
    }
    switch (code) {
        case 0x00: return FrameType::kPadding;
        case 0x01: return FrameType::kPing;
        case 0x02: return FrameType::kAck;
        case 0x03: return FrameType::kAckEcn;
        case 0x04: return FrameType::kResetStream;
        case 0x05: return FrameType::kStopSending;
        case 0x06: return FrameType::kCrypto;
        case 0x07: return FrameType::kNewToken;
        case 0x10: return FrameType::kMaxData;
        case 0x11: return FrameType::kMaxStreamData;
        case 0x12: return FrameType::kMaxStreamsBidi;
        case 0x13: return FrameType::kMaxStreamsUni;
        case 0x14: return FrameType::kDataBlocked;
        case 0x15: return FrameType::kStreamDataBlocked;
        case 0x16: return FrameType::kStreamsBlockedBidi;
        case 0x17: return FrameType::kStreamsBlockedUni;
        case 0x18: return FrameType::kNewConnectionId;
        case 0x19: return FrameType::kRetireConnectionId;
        case 0x1a: return FrameType::kPathChallenge;
        case 0x1b: return FrameType::kPathResponse;
        case 0x1c: return FrameType::kConnectionCloseTransport;
        case 0x1d: return FrameType::kConnectionCloseApplication;
        case 0x1e: return FrameType::kHandshakeDone;
        default: return FrameType::kPadding;   // 占位，调用方用 status 判定
    }
}

/// 类型码是否在本模块认识的范围（0x00..0x1e 且非 STREAM 之外的空洞）。
bool IsKnownTypeCode(std::uint8_t code) noexcept {
    if (IsStreamType(code)) return true;
    return code <= 0x1e && code != 0x0e;   // 0x0e/0x0f 已被 IsStreamType 覆盖
}

/// 读 varint，推进 offset。
bool ReadVarInt(const std::uint8_t* in, std::size_t length, std::size_t* offset,
                std::uint64_t* out, FrameStatus* status) noexcept {
    if (*offset > length) {
        *status = FrameStatus::kFrameEncodingError;
        return false;
    }
    const auto decoded = DecodeVarInt(in + *offset, length - *offset);
    if (decoded.status == VarIntStatus::kNeedMore) {
        *status = FrameStatus::kNeedMore;
        return false;
    }
    if (decoded.status != VarIntStatus::kOk) {
        *status = FrameStatus::kFrameEncodingError;
        return false;
    }
    *out = decoded.value;
    *offset += decoded.consumed;
    return true;
}

void AppendVarInt(std::vector<std::uint8_t>* out, std::uint64_t value) {
    std::uint8_t buffer[8] = {};
    const auto encoded = EncodeVarInt(value, buffer);
    if (encoded.status == VarIntStatus::kOk) {
        out->insert(out->end(), buffer, buffer + encoded.size);
    }
}

/// 读一段定长字节。
bool ReadBytes(const std::uint8_t* in, std::size_t length, std::size_t* offset,
               std::size_t count, std::vector<std::uint8_t>* out,
               FrameStatus* status) noexcept {
    if (*offset + count > length) {
        *status = FrameStatus::kNeedMore;
        return false;
    }
    out->assign(in + *offset, in + *offset + count);
    *offset += count;
    return true;
}

/// 收尾：成功时写入 consumed。
FrameDecoded FinishOk(Frame frame, std::size_t consumed) noexcept {
    FrameDecoded result;
    result.status = FrameStatus::kOk;
    result.frame = std::move(frame);
    result.consumed = consumed;
    return result;
}

FrameDecoded Fail(FrameStatus status) noexcept {
    FrameDecoded result;
    result.status = status;
    result.consumed = 0;
    return result;
}

}  // namespace

// ============================================================================
// 解析
// ============================================================================

FrameDecoded DecodeFrame(const std::uint8_t* in, std::size_t length) noexcept {
    if (in == nullptr || length == 0) {
        return Fail(FrameStatus::kNeedMore);
    }

    const std::uint8_t type_code = in[0];

    // ---- PADDING：合并消费连续的 0x00 ----
    if (type_code == 0x00) {
        std::size_t count = 0;
        while (count < length && in[count] == 0x00) {
            ++count;
        }
        Frame frame;
        frame.type_code = 0x00;
        frame.type = FrameType::kPadding;
        frame.padding_count = count;
        return FinishOk(std::move(frame), count);
    }

    if (!IsKnownTypeCode(type_code)) {
        return Fail(FrameStatus::kUnknownType);
    }

    Frame frame;
    frame.type_code = type_code;
    frame.type = ClassifyType(type_code);

    std::size_t offset = 1;
    FrameStatus status = FrameStatus::kOk;
    std::uint64_t value = 0;

    switch (type_code) {
        // ---- 无字段 ----
        case 0x01:   // PING
        case 0x1e:   // HANDSHAKE_DONE
            return FinishOk(std::move(frame), offset);

        // ---- ACK / ACK_ECN ----
        case 0x02:
        case 0x03: {
            if (!ReadVarInt(in, length, &offset, &frame.largest_acked, &status)) {
                return Fail(status);
            }
            if (!ReadVarInt(in, length, &offset, &frame.ack_delay, &status)) {
                return Fail(status);
            }
            std::uint64_t range_count = 0;
            if (!ReadVarInt(in, length, &offset, &range_count, &status)) {
                return Fail(status);
            }
            if (!ReadVarInt(in, length, &offset, &frame.first_ack_range, &status)) {
                return Fail(status);
            }
            // 防御：range_count 不可能超过剩余字节数（每个区间至少 2 字节）。
            // 不检查的话，一个巨大的 range_count 会让循环跑很久。
            if (range_count > length) {
                return Fail(FrameStatus::kFrameEncodingError);
            }
            frame.ack_ranges.reserve(static_cast<std::size_t>(range_count));
            for (std::uint64_t i = 0; i < range_count; ++i) {
                AckRange ack_range;
                if (!ReadVarInt(in, length, &offset, &ack_range.gap, &status)) {
                    return Fail(status);
                }
                if (!ReadVarInt(in, length, &offset, &ack_range.length, &status)) {
                    return Fail(status);
                }
                frame.ack_ranges.push_back(ack_range);
            }
            if (type_code == 0x03) {
                if (!ReadVarInt(in, length, &offset, &frame.ect0_count, &status)) {
                    return Fail(status);
                }
                if (!ReadVarInt(in, length, &offset, &frame.ect1_count, &status)) {
                    return Fail(status);
                }
                if (!ReadVarInt(in, length, &offset, &frame.ce_count, &status)) {
                    return Fail(status);
                }
            }
            return FinishOk(std::move(frame), offset);
        }

        // ---- RESET_STREAM ----
        case 0x04: {
            if (!ReadVarInt(in, length, &offset, &frame.stream_id, &status)) {
                return Fail(status);
            }
            if (!ReadVarInt(in, length, &offset, &frame.app_error_code, &status)) {
                return Fail(status);
            }
            if (!ReadVarInt(in, length, &offset, &frame.final_size, &status)) {
                return Fail(status);
            }
            return FinishOk(std::move(frame), offset);
        }

        // ---- STOP_SENDING ----
        case 0x05: {
            if (!ReadVarInt(in, length, &offset, &frame.stream_id, &status)) {
                return Fail(status);
            }
            if (!ReadVarInt(in, length, &offset, &frame.app_error_code, &status)) {
                return Fail(status);
            }
            return FinishOk(std::move(frame), offset);
        }

        // ---- CRYPTO ----
        case 0x06: {
            if (!ReadVarInt(in, length, &offset, &frame.offset, &status)) {
                return Fail(status);
            }
            std::uint64_t data_length = 0;
            if (!ReadVarInt(in, length, &offset, &data_length, &status)) {
                return Fail(status);
            }
            // 长度字段超过包剩余 = 格式错误（不是 need-more：包边界已知）。
            if (data_length > length - offset) {
                return Fail(FrameStatus::kFrameEncodingError);
            }
            if (!ReadBytes(in, length, &offset, static_cast<std::size_t>(data_length),
                           &frame.data, &status)) {
                return Fail(status);
            }
            return FinishOk(std::move(frame), offset);
        }

        // ---- NEW_TOKEN ----
        case 0x07: {
            std::uint64_t token_length = 0;
            if (!ReadVarInt(in, length, &offset, &token_length, &status)) {
                return Fail(status);
            }
            // RFC 9000 §19.7：Token Length 为 0 是协议错误。
            if (token_length == 0) {
                return Fail(FrameStatus::kFrameEncodingError);
            }
            if (token_length > length - offset) {
                return Fail(FrameStatus::kFrameEncodingError);
            }
            if (!ReadBytes(in, length, &offset, static_cast<std::size_t>(token_length),
                           &frame.token, &status)) {
                return Fail(status);
            }
            return FinishOk(std::move(frame), offset);
        }

        // ---- STREAM 的 8 个变体 ----
        default: {
            if (IsStreamType(type_code)) {
                frame.has_offset = (type_code & StreamBits::kOffset) != 0;
                frame.has_length = (type_code & StreamBits::kLength) != 0;
                frame.fin = (type_code & StreamBits::kFin) != 0;

                if (!ReadVarInt(in, length, &offset, &frame.stream_id, &status)) {
                    return Fail(status);
                }
                if (frame.has_offset) {
                    if (!ReadVarInt(in, length, &offset, &frame.offset, &status)) {
                        return Fail(status);
                    }
                }
                if (frame.has_length) {
                    std::uint64_t data_length = 0;
                    if (!ReadVarInt(in, length, &offset, &data_length, &status)) {
                        return Fail(status);
                    }
                    if (data_length > length - offset) {
                        return Fail(FrameStatus::kFrameEncodingError);
                    }
                    if (!ReadBytes(in, length, &offset,
                                   static_cast<std::size_t>(data_length), &frame.data,
                                   &status)) {
                        return Fail(status);
                    }
                } else {
                    // LEN 位为 0：数据延伸到包尾（RFC 9000 §19.8）。
                    frame.data.assign(in + offset, in + length);
                    offset = length;
                }
                return FinishOk(std::move(frame), offset);
            }

            // ---- MAX_DATA / DATA_BLOCKED ----
            if (type_code == 0x10 || type_code == 0x14) {
                if (!ReadVarInt(in, length, &offset, &frame.max_data, &status)) {
                    return Fail(status);
                }
                return FinishOk(std::move(frame), offset);
            }

            // ---- MAX_STREAM_DATA / STREAM_DATA_BLOCKED ----
            if (type_code == 0x11 || type_code == 0x15) {
                if (!ReadVarInt(in, length, &offset, &frame.stream_id, &status)) {
                    return Fail(status);
                }
                if (!ReadVarInt(in, length, &offset, &frame.max_stream_data, &status)) {
                    return Fail(status);
                }
                return FinishOk(std::move(frame), offset);
            }

            // ---- MAX_STREAMS / STREAMS_BLOCKED ----
            if (type_code == 0x12 || type_code == 0x13 || type_code == 0x16 ||
                type_code == 0x17) {
                if (!ReadVarInt(in, length, &offset, &frame.max_streams, &status)) {
                    return Fail(status);
                }
                // RFC 9000 §19.11：超过 2^60 是 FRAME_ENCODING_ERROR。
                if (frame.max_streams > (1ULL << 60)) {
                    return Fail(FrameStatus::kFrameEncodingError);
                }
                return FinishOk(std::move(frame), offset);
            }

            // ---- NEW_CONNECTION_ID ----
            if (type_code == 0x18) {
                if (!ReadVarInt(in, length, &offset, &frame.sequence_number, &status)) {
                    return Fail(status);
                }
                if (!ReadVarInt(in, length, &offset, &frame.retire_prior_to, &status)) {
                    return Fail(status);
                }
                // RFC 9000 §19.15：Retire Prior To > Sequence Number 是错误。
                if (frame.retire_prior_to > frame.sequence_number) {
                    return Fail(FrameStatus::kFrameEncodingError);
                }
                if (offset >= length) {
                    return Fail(FrameStatus::kNeedMore);
                }
                const std::uint8_t cid_length = in[offset++];
                // RFC 9000 §19.15：Connection ID 长度必须在 1..20 之间，
                // 且 > 20 是 FRAME_ENCODING_ERROR（0 is allowed for CID? 见 §5.1）
                if (cid_length == 0 || cid_length > 20) {
                    return Fail(FrameStatus::kFrameEncodingError);
                }
                if (offset + cid_length + 16 > length) {
                    return Fail(FrameStatus::kNeedMore);
                }
                if (!ReadBytes(in, length, &offset, cid_length, &frame.connection_id,
                               &status)) {
                    return Fail(status);
                }
                if (!ReadBytes(in, length, &offset, 16, &frame.stateless_reset_token,
                               &status)) {
                    return Fail(status);
                }
                return FinishOk(std::move(frame), offset);
            }

            // ---- RETIRE_CONNECTION_ID ----
            if (type_code == 0x19) {
                if (!ReadVarInt(in, length, &offset, &frame.sequence_number, &status)) {
                    return Fail(status);
                }
                return FinishOk(std::move(frame), offset);
            }

            // ---- PATH_CHALLENGE / PATH_RESPONSE ----
            if (type_code == 0x1a || type_code == 0x1b) {
                if (offset + 8 > length) {
                    return Fail(FrameStatus::kNeedMore);
                }
                std::memcpy(frame.path_data.data(), in + offset, 8);
                offset += 8;
                return FinishOk(std::move(frame), offset);
            }

            // ---- CONNECTION_CLOSE ----
            if (type_code == 0x1c || type_code == 0x1d) {
                if (!ReadVarInt(in, length, &offset, &frame.error_code, &status)) {
                    return Fail(status);
                }
                if (type_code == 0x1c) {
                    if (!ReadVarInt(in, length, &offset, &frame.closing_frame_type,
                                    &status)) {
                        return Fail(status);
                    }
                }
                std::uint64_t reason_length = 0;
                if (!ReadVarInt(in, length, &offset, &reason_length, &status)) {
                    return Fail(status);
                }
                if (reason_length > length - offset) {
                    return Fail(FrameStatus::kFrameEncodingError);
                }
                frame.reason_phrase.assign(
                    reinterpret_cast<const char*>(in + offset),
                    static_cast<std::size_t>(reason_length));
                offset += static_cast<std::size_t>(reason_length);
                return FinishOk(std::move(frame), offset);
            }

            // 类型码 <= 0x1e 但落到了这里：理论上不可达（上面的分支已穷举）。
            return Fail(FrameStatus::kUnknownType);
        }
    }
}

// ============================================================================
// 序列化
// ============================================================================

FrameEncoded EncodeFrame(const Frame& frame) {
    FrameEncoded result;
    std::vector<std::uint8_t>& out = result.bytes;

    if (IsStreamType(frame.type_code)) {
        // STREAM：重算类型码低 3 位（以 bool 字段为准，避免 type_code 与字段不一致）
        std::uint8_t code = 0x08;
        if (frame.has_offset) code |= StreamBits::kOffset;
        if (frame.has_length) code |= StreamBits::kLength;
        if (frame.fin) code |= StreamBits::kFin;
        out.push_back(code);
        AppendVarInt(&out, frame.stream_id);
        if (frame.has_offset) {
            AppendVarInt(&out, frame.offset);
        }
        if (frame.has_length) {
            AppendVarInt(&out, frame.data.size());
        }
        out.insert(out.end(), frame.data.begin(), frame.data.end());
        return result;
    }

    switch (frame.type_code) {
        case 0x00: {   // PADDING
            if (frame.padding_count == 0) {
                result.status = FrameStatus::kInvalidArgument;
                return result;
            }
            out.assign(static_cast<std::size_t>(frame.padding_count), 0x00);
            return result;
        }
        case 0x01:   // PING
        case 0x1e:   // HANDSHAKE_DONE
            out.push_back(frame.type_code);
            return result;

        case 0x02:
        case 0x03: {   // ACK / ACK_ECN
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.largest_acked);
            AppendVarInt(&out, frame.ack_delay);
            AppendVarInt(&out, frame.ack_ranges.size());
            AppendVarInt(&out, frame.first_ack_range);
            for (const auto& ack_range : frame.ack_ranges) {
                AppendVarInt(&out, ack_range.gap);
                AppendVarInt(&out, ack_range.length);
            }
            if (frame.type_code == 0x03) {
                AppendVarInt(&out, frame.ect0_count);
                AppendVarInt(&out, frame.ect1_count);
                AppendVarInt(&out, frame.ce_count);
            }
            return result;
        }

        case 0x04:   // RESET_STREAM
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.stream_id);
            AppendVarInt(&out, frame.app_error_code);
            AppendVarInt(&out, frame.final_size);
            return result;

        case 0x05:   // STOP_SENDING
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.stream_id);
            AppendVarInt(&out, frame.app_error_code);
            return result;

        case 0x06:   // CRYPTO
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.offset);
            AppendVarInt(&out, frame.data.size());
            out.insert(out.end(), frame.data.begin(), frame.data.end());
            return result;

        case 0x07:   // NEW_TOKEN
            if (frame.token.empty()) {
                result.status = FrameStatus::kInvalidArgument;
                return result;
            }
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.token.size());
            out.insert(out.end(), frame.token.begin(), frame.token.end());
            return result;

        case 0x10:   // MAX_DATA
        case 0x14:   // DATA_BLOCKED
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.max_data);
            return result;

        case 0x11:   // MAX_STREAM_DATA
        case 0x15:   // STREAM_DATA_BLOCKED
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.stream_id);
            AppendVarInt(&out, frame.max_stream_data);
            return result;

        case 0x12:   // MAX_STREAMS(bidi)
        case 0x13:   // MAX_STREAMS(uni)
        case 0x16:   // STREAMS_BLOCKED(bidi)
        case 0x17:   // STREAMS_BLOCKED(uni)
            if (frame.max_streams > (1ULL << 60)) {
                result.status = FrameStatus::kInvalidArgument;
                return result;
            }
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.max_streams);
            return result;

        case 0x18: {   // NEW_CONNECTION_ID
            const std::size_t cid_length = frame.connection_id.size();
            if (cid_length == 0 || cid_length > 20 ||
                frame.stateless_reset_token.size() != 16 ||
                frame.retire_prior_to > frame.sequence_number) {
                result.status = FrameStatus::kInvalidArgument;
                return result;
            }
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.sequence_number);
            AppendVarInt(&out, frame.retire_prior_to);
            out.push_back(static_cast<std::uint8_t>(cid_length));
            out.insert(out.end(), frame.connection_id.begin(), frame.connection_id.end());
            out.insert(out.end(), frame.stateless_reset_token.begin(),
                       frame.stateless_reset_token.end());
            return result;
        }

        case 0x19:   // RETIRE_CONNECTION_ID
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.sequence_number);
            return result;

        case 0x1a:   // PATH_CHALLENGE
        case 0x1b:   // PATH_RESPONSE
            out.push_back(frame.type_code);
            out.insert(out.end(), frame.path_data.begin(), frame.path_data.end());
            return result;

        case 0x1c:   // CONNECTION_CLOSE(transport)
        case 0x1d: { // CONNECTION_CLOSE(app)
            out.push_back(frame.type_code);
            AppendVarInt(&out, frame.error_code);
            if (frame.type_code == 0x1c) {
                AppendVarInt(&out, frame.closing_frame_type);
            }
            AppendVarInt(&out, frame.reason_phrase.size());
            out.insert(out.end(), frame.reason_phrase.begin(), frame.reason_phrase.end());
            return result;
        }

        default:
            result.status = FrameStatus::kInvalidArgument;
            return result;
    }
}

// ============================================================================
// 便捷构造器
// ============================================================================

Frame MakePadding(std::uint64_t count) {
    Frame frame;
    frame.type_code = 0x00;
    frame.type = FrameType::kPadding;
    frame.padding_count = count;
    return frame;
}

Frame MakePing() {
    Frame frame;
    frame.type_code = 0x01;
    frame.type = FrameType::kPing;
    return frame;
}

Frame MakeAck(std::uint64_t largest_acked, std::uint64_t ack_delay,
              std::uint64_t first_ack_range, const std::vector<AckRange>& ranges) {
    Frame frame;
    frame.type_code = 0x02;
    frame.type = FrameType::kAck;
    frame.largest_acked = largest_acked;
    frame.ack_delay = ack_delay;
    frame.first_ack_range = first_ack_range;
    frame.ack_ranges = ranges;
    return frame;
}

Frame MakeAckEcn(std::uint64_t largest_acked, std::uint64_t ack_delay,
                 std::uint64_t first_ack_range, const std::vector<AckRange>& ranges,
                 std::uint64_t ect0, std::uint64_t ect1, std::uint64_t ce) {
    Frame frame = MakeAck(largest_acked, ack_delay, first_ack_range, ranges);
    frame.type_code = 0x03;
    frame.type = FrameType::kAckEcn;
    frame.ect0_count = ect0;
    frame.ect1_count = ect1;
    frame.ce_count = ce;
    return frame;
}

Frame MakeResetStream(std::uint64_t stream_id, std::uint64_t app_error_code,
                      std::uint64_t final_size) {
    Frame frame;
    frame.type_code = 0x04;
    frame.type = FrameType::kResetStream;
    frame.stream_id = stream_id;
    frame.app_error_code = app_error_code;
    frame.final_size = final_size;
    return frame;
}

Frame MakeStopSending(std::uint64_t stream_id, std::uint64_t app_error_code) {
    Frame frame;
    frame.type_code = 0x05;
    frame.type = FrameType::kStopSending;
    frame.stream_id = stream_id;
    frame.app_error_code = app_error_code;
    return frame;
}

Frame MakeCrypto(std::uint64_t offset, std::vector<std::uint8_t> data) {
    Frame frame;
    frame.type_code = 0x06;
    frame.type = FrameType::kCrypto;
    frame.offset = offset;
    frame.data = std::move(data);
    return frame;
}

Frame MakeNewToken(std::vector<std::uint8_t> token) {
    Frame frame;
    frame.type_code = 0x07;
    frame.type = FrameType::kNewToken;
    frame.token = std::move(token);
    return frame;
}

Frame MakeStream(std::uint64_t stream_id, std::uint64_t offset, bool has_offset,
                 bool has_length, bool fin, std::vector<std::uint8_t> data) {
    Frame frame;
    std::uint8_t code = 0x08;
    if (has_offset) code |= StreamBits::kOffset;
    if (has_length) code |= StreamBits::kLength;
    if (fin) code |= StreamBits::kFin;
    frame.type_code = code;
    frame.type = FrameType::kStreamBase;
    frame.stream_id = stream_id;
    frame.has_offset = has_offset;
    frame.has_length = has_length;
    frame.fin = fin;
    frame.offset = has_offset ? offset : 0;
    frame.data = std::move(data);
    return frame;
}

Frame MakeMaxData(std::uint64_t max_data) {
    Frame frame;
    frame.type_code = 0x10;
    frame.type = FrameType::kMaxData;
    frame.max_data = max_data;
    return frame;
}

Frame MakeMaxStreamData(std::uint64_t stream_id, std::uint64_t max_stream_data) {
    Frame frame;
    frame.type_code = 0x11;
    frame.type = FrameType::kMaxStreamData;
    frame.stream_id = stream_id;
    frame.max_stream_data = max_stream_data;
    return frame;
}

Frame MakeMaxStreams(std::uint64_t max_streams, bool bidirectional) {
    Frame frame;
    frame.type_code = bidirectional ? 0x12 : 0x13;
    frame.type = bidirectional ? FrameType::kMaxStreamsBidi : FrameType::kMaxStreamsUni;
    frame.max_streams = max_streams;
    return frame;
}

Frame MakeDataBlocked(std::uint64_t max_data) {
    Frame frame;
    frame.type_code = 0x14;
    frame.type = FrameType::kDataBlocked;
    frame.max_data = max_data;
    return frame;
}

Frame MakeStreamDataBlocked(std::uint64_t stream_id, std::uint64_t max_stream_data) {
    Frame frame;
    frame.type_code = 0x15;
    frame.type = FrameType::kStreamDataBlocked;
    frame.stream_id = stream_id;
    frame.max_stream_data = max_stream_data;
    return frame;
}

Frame MakeStreamsBlocked(std::uint64_t max_streams, bool bidirectional) {
    Frame frame;
    frame.type_code = bidirectional ? 0x16 : 0x17;
    frame.type = bidirectional ? FrameType::kStreamsBlockedBidi
                               : FrameType::kStreamsBlockedUni;
    frame.max_streams = max_streams;
    return frame;
}

Frame MakeNewConnectionId(std::uint64_t sequence_number, std::uint64_t retire_prior_to,
                          std::vector<std::uint8_t> connection_id,
                          std::vector<std::uint8_t> stateless_reset_token) {
    Frame frame;
    frame.type_code = 0x18;
    frame.type = FrameType::kNewConnectionId;
    frame.sequence_number = sequence_number;
    frame.retire_prior_to = retire_prior_to;
    frame.connection_id = std::move(connection_id);
    frame.stateless_reset_token = std::move(stateless_reset_token);
    return frame;
}

Frame MakeRetireConnectionId(std::uint64_t sequence_number) {
    Frame frame;
    frame.type_code = 0x19;
    frame.type = FrameType::kRetireConnectionId;
    frame.sequence_number = sequence_number;
    return frame;
}

Frame MakePathChallenge(const std::array<std::uint8_t, 8>& data) {
    Frame frame;
    frame.type_code = 0x1a;
    frame.type = FrameType::kPathChallenge;
    frame.path_data = data;
    return frame;
}

Frame MakePathResponse(const std::array<std::uint8_t, 8>& data) {
    Frame frame;
    frame.type_code = 0x1b;
    frame.type = FrameType::kPathResponse;
    frame.path_data = data;
    return frame;
}

Frame MakeConnectionCloseTransport(std::uint64_t error_code,
                                   std::uint64_t closing_frame_type,
                                   std::string reason_phrase) {
    Frame frame;
    frame.type_code = 0x1c;
    frame.type = FrameType::kConnectionCloseTransport;
    frame.error_code = error_code;
    frame.closing_frame_type = closing_frame_type;
    frame.reason_phrase = std::move(reason_phrase);
    return frame;
}

Frame MakeConnectionCloseApplication(std::uint64_t error_code,
                                     std::string reason_phrase) {
    Frame frame;
    frame.type_code = 0x1d;
    frame.type = FrameType::kConnectionCloseApplication;
    frame.error_code = error_code;
    frame.reason_phrase = std::move(reason_phrase);
    return frame;
}

Frame MakeHandshakeDone() {
    Frame frame;
    frame.type_code = 0x1e;
    frame.type = FrameType::kHandshakeDone;
    return frame;
}

}  // namespace go2cpp::net::quic
