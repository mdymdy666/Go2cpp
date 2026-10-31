/**
 * @file app_test_stream_table.cpp
 * @brief 流表测试：状态机 / 多路复用 / 优先级 / 流控 / 取消 / 关闭 / GOAWAY。
 *
 * @details 12 个场景全部在纯内存里驱动流表，不涉及传输与线程，确保失败
 *          可复现且能精确定位到状态转换/调度规则。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "app_test_support.hpp"
#include "go2cpp/app/stream_table.hpp"

namespace {

using namespace go2cpp::app;

/// 固定帧数/载荷的生产者：首帧 HEADERS，其余 DATA，最后一帧 END_STREAM。
class FixedProducer final : public FrameProducer {
   public:
    FixedProducer(std::size_t data_frames, std::uint32_t payload_bytes,
                  bool empty_headers = false)
        : m_data_frames(data_frames),
          m_payload_bytes(payload_bytes),
          m_empty_headers(empty_headers) {}

    bool Produce(std::uint32_t /*max_payload*/, Frame* out) override {
        if (m_done || out == nullptr) {
            return false;
        }
        if (!m_headers_sent) {
            out->type = FrameType::kHeaders;
            out->flags = kFlagEndHeaders;
            out->payload = m_empty_headers ? std::vector<std::uint8_t>{}
                                           : app_test::MakePattern(16, 1);
            m_headers_sent = true;
            if (m_data_frames == 0) {
                out->SetFlag(kFlagEndStream);
                m_done = true;
            }
            return true;
        }
        if (m_sent_data >= m_data_frames) {
            m_done = true;
            return false;
        }
        // 固定帧生产者：不按 max_payload 切分（用于验证「整帧等窗口」），
        // 真实资源/KV 生产者会切分。
        const std::uint32_t bytes = m_payload_bytes;
        out->type = FrameType::kData;
        out->flags = 0;
        out->payload.assign(bytes, static_cast<std::uint8_t>(m_sent_data & 0xff));
        ++m_sent_data;
        m_produced += bytes;
        if (m_sent_data >= m_data_frames) {
            out->SetFlag(kFlagEndStream);
            m_done = true;
        }
        return true;
    }

    bool done() const override { return m_done; }
    std::uint64_t produced_bytes() const override { return m_produced; }

   private:
    std::size_t m_data_frames = 0;
    std::uint32_t m_payload_bytes = 0;
    bool m_empty_headers = false;
    bool m_headers_sent = false;
    bool m_done = false;
    std::size_t m_sent_data = 0;
    std::uint64_t m_produced = 0;
};

/// 模拟连接层发送一帧：扣窗口、推进状态、推进 WFQ。
bool SendOne(StreamTable& table, Stream& stream) {
    Frame frame = std::move(*stream.pending);
    stream.pending.reset();
    const std::size_t bytes = frame.payload.size();
    if (frame.type == FrameType::kData && bytes > 0 &&
        !table.ConsumeSend(stream, bytes)) {
        return false;
    }
    bool ok = true;
    if (frame.type == FrameType::kData) {
        ok = table.SendData(stream, bytes, frame.EndStream());
        table.ChargeFinish(stream, bytes);
    } else if (frame.type == FrameType::kHeaders) {
        ok = table.SendHeaders(stream, frame.EndStream());
    }
    return ok;
}

void StateMachine() {
    APP_SCENARIO("stream table: state machine transitions");
    StreamTable table;
    Stream* stream = table.CreateLocal(StreamKind::kKv,
                                       std::make_unique<FixedProducer>(0, 0));
    APP_REQUIRE(stream != nullptr);
    APP_CHECK(stream->state == StreamState::kIdle);
    APP_CHECK(!stream->can_send());
    APP_CHECK(table.SendHeaders(*stream, false));
    APP_CHECK(stream->state == StreamState::kOpen);
    APP_CHECK(stream->can_send() && stream->can_receive());
    APP_CHECK(table.SendData(*stream, 10, true));
    APP_CHECK(stream->state == StreamState::kHalfClosedLocal);
    APP_CHECK(!stream->can_send());
    APP_CHECK(stream->can_receive());
    APP_CHECK(!table.SendData(*stream, 1, false));
    APP_CHECK(table.ReceiveData(*stream, 0, true));
    APP_CHECK(stream->state == StreamState::kClosed);
    APP_CHECK(!table.SendHeaders(*stream, false));
    APP_CHECK(!table.ReceiveHeaders(*stream, false));

    // 对端发起的流：idle -> open -> half_closed_remote。
    Stream* remote = table.CreateRemote(9, StreamKind::kBytes);
    APP_REQUIRE(remote != nullptr);
    APP_CHECK(table.ReceiveHeaders(*remote, false));
    APP_CHECK(remote->state == StreamState::kOpen);
    APP_CHECK(table.ReceiveData(*remote, 5, true));
    APP_CHECK(remote->state == StreamState::kHalfClosedRemote);
    APP_CHECK(!remote->can_receive());
    APP_CHECK(remote->can_send());
    APP_CHECK(table.SendData(*remote, 0, true));
    APP_CHECK(remote->state == StreamState::kClosed);
}

void Multiplexing() {
    APP_SCENARIO("stream table: 10 concurrent streams all make progress");
    StreamTableConfig config;
    config.max_concurrent_streams = 32;
    StreamTable table(config);
    std::vector<Stream*> streams;
    for (int i = 0; i < 10; ++i) {
        Stream* stream = table.CreateLocal(
            StreamKind::kKv, std::make_unique<FixedProducer>(3, 100));
        APP_REQUIRE(stream != nullptr);
        streams.push_back(stream);
    }
    std::size_t sent_frames = 0;
    while (true) {
        Stream* stream = table.PickNext();
        if (stream == nullptr) {
            break;
        }
        APP_REQUIRE(SendOne(table, *stream));
        ++sent_frames;
        if (sent_frames > 1000) {
            break;
        }
    }
    APP_CHECK(sent_frames == 10 * 4);  // 每条流 1 HEADERS + 3 DATA
    for (Stream* stream : streams) {
        APP_CHECK(stream->bytes_sent == 300);
        APP_CHECK(stream->local_closed);
    }
    APP_CHECK(table.total_bytes_sent() == 3000);
    APP_CHECK(!table.HasWork());
}

void PriorityWeights() {
    APP_SCENARIO("stream table: WFQ weight 4:1 splits bytes");
    StreamTableConfig config;
    config.initial_stream_window = 1024 * 1024;
    config.connection_window = 1024 * 1024;
    config.max_frame_payload = 1024 * 1024;
    StreamTable table(config);
    Stream* fast = table.CreateLocal(StreamKind::kBytes,
                                     std::make_unique<FixedProducer>(1000, 500), 64);
    Stream* slow = table.CreateLocal(StreamKind::kBytes,
                                     std::make_unique<FixedProducer>(1000, 500), 16);
    APP_REQUIRE(fast != nullptr && slow != nullptr);
    // 只发固定总帧数（100），让 WFQ 决定两者的份额；若各发完自己的
    // 1000 帧，最终总量必然相同，测不出权重。
    std::size_t frames = 0;
    while (frames < 100) {
        Stream* stream = table.PickNext();
        if (stream == nullptr) {
            break;
        }
        APP_REQUIRE(SendOne(table, *stream));
        ++frames;
    }
    APP_CHECK(frames == 100);
    APP_CHECK(fast->bytes_sent > slow->bytes_sent);
    const double ratio =
        static_cast<double>(fast->bytes_sent + 1) / static_cast<double>(slow->bytes_sent + 1);
    APP_CHECK(ratio > 2.5);
    APP_CHECK(ratio < 6.0);
    app_test::Note("weight 64:16 => bytes " + std::to_string(fast->bytes_sent) + "/" +
                   std::to_string(slow->bytes_sent) + " weights " +
                   std::to_string(fast->weight) + "/" + std::to_string(slow->weight) +
                   " finish " + std::to_string(fast->finish) + "/" +
                   std::to_string(slow->finish));
}

void DependencyTree() {
    APP_SCENARIO("stream table: strict parent priority + cycle rejection");
    StreamTableConfig config;
    config.initial_stream_window = 1024 * 1024;
    config.connection_window = 1024 * 1024;
    StreamTable table(config);
    Stream* parent = table.CreateLocal(StreamKind::kBytes,
                                       std::make_unique<FixedProducer>(2, 100));
    APP_REQUIRE(parent != nullptr);
    Stream* child = table.CreateLocal(StreamKind::kBytes,
                                      std::make_unique<FixedProducer>(2, 100), 16,
                                      parent->id);
    APP_REQUIRE(child != nullptr);
    std::string error;
    APP_CHECK(!table.SetPriority(child->id, child->id, 16, &error));
    APP_CHECK(!table.SetPriority(parent->id, child->id, 16, &error));  // 环
    APP_CHECK(table.SetPriority(child->id, parent->id, 16, &error));
    // 父节点还有待发帧时，子节点不得抢占。
    Stream* chosen = table.PickNext();
    APP_REQUIRE(chosen == parent);
    APP_REQUIRE(SendOne(table, *chosen));
    chosen = table.PickNext();
    APP_CHECK(chosen == parent);  // 父的 HEADERS + 第 1 帧 DATA 之后仍有 1 帧
    // 父流的数据帧发完后，子流才被调度。
    std::size_t guard = 0;
    while (!parent->producer_done && guard++ < 8) {
        Stream* next = table.PickNext();
        APP_REQUIRE(next == parent);
        APP_REQUIRE(SendOne(table, *next));
    }
    APP_CHECK(parent->producer_done);
    chosen = table.PickNext();
    APP_CHECK(chosen == child);
    // 删除父流后，子流不应崩（HasReadyAncestor 会提前退出）。
    table.Erase(parent->id);
    chosen = table.PickNext();
    APP_CHECK(chosen == child);
}

void StreamFlowControl() {
    APP_SCENARIO("stream table: per-stream window blocks then resumes");
    StreamTableConfig config;
    config.initial_stream_window = 100;
    config.connection_window = 1000;
    config.max_frame_payload = 100;
    StreamTable table(config);
    Stream* stream = table.CreateLocal(StreamKind::kBytes,
                                       std::make_unique<FixedProducer>(10, 60));
    APP_REQUIRE(stream != nullptr);
    // 首帧 HEADERS 不受流控。
    Stream* chosen = table.PickNext();
    APP_REQUIRE(chosen == stream);
    APP_REQUIRE(SendOne(table, *chosen));
    APP_CHECK(stream->send_window == 100);
    // 第一帧 DATA 60 字节。
    chosen = table.PickNext();
    APP_REQUIRE(chosen == stream);
    APP_REQUIRE(SendOne(table, *chosen));
    APP_CHECK(stream->send_window == 40);
    // 第二帧 60 字节放不下：流表应在生产阶段就停住，而不是超发。
    APP_CHECK(table.PickNext() == nullptr);
    APP_CHECK(table.ReadyCount() == 0);
    std::string error;
    APP_REQUIRE(table.OnWindowUpdate(stream->id, 100, &error));
    APP_CHECK(stream->send_window == 140);
    std::size_t sent = 0;
    while (true) {
        Stream* next = table.PickNext();
        if (next == nullptr) {
            break;
        }
        APP_REQUIRE(SendOne(table, *next));
        if (++sent > 20) {
            break;
        }
    }
    APP_CHECK(stream->bytes_sent > 0);
    APP_CHECK(stream->send_window >= 0);
}

void ConnectionWindow() {
    APP_SCENARIO("stream table: connection window shared by 3 streams");
    StreamTableConfig config;
    config.initial_stream_window = 1000;
    config.connection_window = 120;
    config.max_frame_payload = 100;
    StreamTable table(config);
    std::vector<Stream*> streams;
    for (int i = 0; i < 3; ++i) {
        Stream* stream = table.CreateLocal(StreamKind::kBytes,
                                           std::make_unique<FixedProducer>(10, 50));
        APP_REQUIRE(stream != nullptr);
        streams.push_back(stream);
    }
    std::size_t frames = 0;
    while (true) {
        Stream* stream = table.PickNext();
        if (stream == nullptr) {
            break;
        }
        APP_REQUIRE(SendOne(table, *stream));
        if (++frames > 100) {
            break;
        }
    }
    APP_CHECK(table.connection_send_window() <= 20);
    APP_CHECK(frames >= 2);
    std::string error;
    APP_REQUIRE(table.OnWindowUpdate(0, 500, &error));
    APP_CHECK(table.connection_send_window() > 20);
    std::size_t resumed = 0;
    while (resumed < 100) {
        Stream* stream = table.PickNext();
        if (stream == nullptr) {
            break;
        }
        APP_REQUIRE(SendOne(table, *stream));
        ++resumed;
    }
    APP_CHECK(resumed > 0);
}

void ConcurrentStreamLimit() {
    APP_SCENARIO("stream table: max concurrent streams enforced");
    StreamTableConfig config;
    config.max_concurrent_streams = 2;
    StreamTable table(config);
    Stream* first = table.CreateLocal(StreamKind::kKv,
                                      std::make_unique<FixedProducer>(0, 0));
    Stream* second = table.CreateLocal(StreamKind::kKv,
                                       std::make_unique<FixedProducer>(0, 0));
    APP_REQUIRE(first != nullptr && second != nullptr);
    std::string error;
    APP_CHECK(table.CreateLocal(StreamKind::kKv, nullptr, 0, 0, &error) == nullptr);
    APP_CHECK(!error.empty());
    APP_CHECK(table.stream_count() == 2);
    // 关闭第一条（双向都关）后可以再创建。
    APP_REQUIRE(table.SendHeaders(*first, true));
    APP_REQUIRE(table.ReceiveHeaders(*first, true));
    APP_CHECK(first->closed());
    Stream* third = table.CreateLocal(StreamKind::kKv, nullptr, 0, 0, &error);
    APP_CHECK(third != nullptr);
}

void Cancellation() {
    APP_SCENARIO("stream table: RST_STREAM closes and blocks further writes");
    StreamTable table;
    Stream* stream = table.CreateLocal(StreamKind::kResource,
                                       std::make_unique<FixedProducer>(100, 100));
    APP_REQUIRE(stream != nullptr);
    APP_REQUIRE(table.SendHeaders(*stream, false));
    APP_REQUIRE(table.Reset(*stream, ErrorCode::kCancel));
    APP_CHECK(stream->state == StreamState::kClosed);
    APP_CHECK(!stream->can_send());
    APP_CHECK(!table.SendData(*stream, 10, false));
    APP_CHECK(table.total_resets() == 1);
    APP_CHECK(table.PickNext() == nullptr);
    APP_CHECK(!table.Reset(*stream, ErrorCode::kCancel));
}

void GoAwayRefusesNewStreams() {
    APP_SCENARIO("stream table: GOAWAY refuses new local streams");
    StreamTable table;
    Stream* existing = table.CreateLocal(StreamKind::kKv,
                                         std::make_unique<FixedProducer>(0, 0));
    APP_REQUIRE(existing != nullptr);
    table.GoAway(7, ErrorCode::kCancel, "bye");
    APP_CHECK(table.goaway());
    APP_CHECK(table.goaway_last_stream_id() == 7);
    APP_CHECK(table.goaway_error() == ErrorCode::kCancel);
    APP_CHECK(table.goaway_debug() == "bye");
    std::string error;
    APP_CHECK(table.CreateLocal(StreamKind::kKv, nullptr, 0, 0, &error) == nullptr);
    APP_CHECK(!error.empty());
    // 已有流仍可被调度。
    APP_CHECK(table.HasWork());
}

void InitialWindowUpdates() {
    APP_SCENARIO("stream table: SETTINGS initial window adjusts live streams");
    StreamTable table;
    Stream* first = table.CreateLocal(StreamKind::kBytes, nullptr);
    Stream* second = table.CreateLocal(StreamKind::kBytes, nullptr);
    APP_REQUIRE(first != nullptr && second != nullptr);
    APP_CHECK(first->send_window == 65535);
    std::string error;
    APP_REQUIRE(table.SetInitialWindowSize(100000, &error));
    APP_CHECK(first->send_window == 100000);
    APP_CHECK(second->send_window == 100000);
    APP_REQUIRE(table.SetInitialWindowSize(50000, &error));
    APP_CHECK(first->send_window == 50000);
    APP_CHECK(!table.SetInitialWindowSize(0, &error));
    APP_CHECK(!table.SetInitialWindowSize(-1, &error));
}

void RetiredStreamPruning() {
    APP_SCENARIO("stream table: closed streams pruned beyond max_retired");
    StreamTableConfig config;
    config.max_retired_streams = 4;
    config.max_concurrent_streams = 1024;
    StreamTable table(config);
    for (int i = 0; i < 10; ++i) {
        Stream* stream = table.CreateLocal(StreamKind::kKv, nullptr);
        APP_REQUIRE(stream != nullptr);
        APP_REQUIRE(table.SendHeaders(*stream, true));
        APP_REQUIRE(table.ReceiveHeaders(*stream, true));
    }
    table.PruneClosedStreams();
    APP_CHECK(table.stream_count() <= 4);
    APP_CHECK(table.retired_streams() >= 6);
    APP_CHECK(table.active_streams() == 0);
}

void CountersAndWork() {
    APP_SCENARIO("stream table: HasWork/Ready/Blocked counters");
    StreamTableConfig config;
    config.initial_stream_window = 100;
    config.connection_window = 100;
    config.max_frame_payload = 100;
    StreamTable table(config);
    APP_CHECK(!table.HasWork());
    Stream* stream = table.CreateLocal(StreamKind::kBytes,
                                       std::make_unique<FixedProducer>(5, 60));
    APP_REQUIRE(stream != nullptr);
    APP_CHECK(table.HasWork());
    APP_CHECK(table.ReadyCount() == 0);
    APP_REQUIRE(table.FillPending(*stream));
    APP_CHECK(table.ReadyCount() == 1);
    APP_REQUIRE(SendOne(table, *stream));
    APP_REQUIRE(table.FillPending(*stream));
    APP_CHECK(table.ReadyCount() == 1);
    APP_REQUIRE(SendOne(table, *stream));
    APP_CHECK(table.ReadyCount() == 0);  // 窗口 40 < 60
    APP_CHECK(table.BlockedCount() == 1);
    APP_CHECK(table.PickNext() == nullptr);
}

}  // namespace

int main() {
    StateMachine();
    Multiplexing();
    PriorityWeights();
    DependencyTree();
    StreamFlowControl();
    ConnectionWindow();
    ConcurrentStreamLimit();
    Cancellation();
    GoAwayRefusesNewStreams();
    InitialWindowUpdates();
    RetiredStreamPruning();
    CountersAndWork();
    return app_test::Finish("app_test_stream_table");
}
