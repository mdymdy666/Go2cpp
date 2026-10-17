/**
 * @file test_http2_stress.cpp
 * @brief HTTP/2 全栈压力测试（单进程 + MemoryTransport，排除网络抖动）。
 *
 * ## 为什么用 MemoryTransport 而不是真实 socket
 *
 * 压力测试要测的是**协议栈本身**（HPACK 编解码、帧处理、流状态机、流控），
 * 而不是内核 socket 的吞吐。用真实 socket 会把 TCP 分段、调度延迟、网卡中断
 * 全都混进来，测出的数字既不可复现也无法归因。`MemoryTransport` 把 IO 变成
 * 内存搬运，只留下协议逻辑的开销 —— 这是能定位性能问题的前提。
 *
 * 真实 socket 的正确性已经由 `test_fiber_transport.cpp` 覆盖，两者分工不重叠。
 *
 * ## 六个场景
 *
 * 1. **批量并发流**（100 / 1000 / 5000）：pipeline 模式，测总吞吐；
 * 2. **串行单请求延迟**：一发一收，测 p50/p90/p99（这才是用户感知的延迟）；
 * 3. **大 body 分片**（1MB / 10MB）：**真实走流控往返** —— 窗口只有 65535，
 *    每发一批就必须把 server 的 WINDOW_UPDATE 搬回 client 才能继续；
 * 4. **连接复用**：同一连接连续 N 个**相同**请求，验证 HPACK 动态表命中让
 *    单请求字节数大幅下降；
 * 5. **HPACK 动态表压力**：每请求用**唯一** header，迫使动态表不断插入与逐出，
 *    验证内存不失控；
 * 6. **CONTINUATION 压力**：1MB / 10MB 的 header value 分片 + 拼接。
 *
 * ## 数据采集
 *
 * 吞吐、p50/p90/p99、`VmRSS`（读 `/proc/self/status`）、loadavg。
 * 按项目规则，**loadavg > 1.5 的测量作废**，所以每个场景开头都会打印当次
 * loadavg，便于判断数据是否可信。
 */
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/frame.hpp"
#include "go2cpp/network/http2/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace go2cpp::net::http2;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[stress] FAILED: %s\n", what);
        ++g_failures;
    }
}

constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kPrefaceLen = sizeof(kPreface) - 1;

double NowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

/// 从 /proc/self/status 读 VmRSS（KB）。读不到返回 -1。
long ReadVmRssKb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            if (std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) {
                return kb;
            }
        }
    }
    return -1;
}

/// 从 /proc/loadavg 读 1 分钟负载。读不到返回 -1。
double ReadLoadAvg() {
    std::ifstream in("/proc/loadavg");
    double value = -1.0;
    if (in >> value) {
        return value;
    }
    return -1.0;
}

double Percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double pos = p * static_cast<double>(values.size() - 1);
    const auto index = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(index);
    if (index + 1 >= values.size()) {
        return values.back();
    }
    return values[index] * (1.0 - frac) + values[index + 1] * frac;
}

std::vector<std::uint8_t> Bytes(const Frame& frame) {
    std::vector<std::uint8_t> out(kFrameHeaderSize + frame.payload.size());
    EncodeFrameHeader(frame.header, out.data());
    if (!frame.payload.empty()) {
        std::memcpy(out.data() + kFrameHeaderSize, frame.payload.data(), frame.payload.size());
    }
    return out;
}

std::vector<std::uint8_t> PeerSettings() {
    return Bytes(MakeSettings({
        {static_cast<std::uint16_t>(SettingsId::kHeaderTableSize), 4096},
        {static_cast<std::uint16_t>(SettingsId::kMaxFrameSize), 16384},
        {static_cast<std::uint16_t>(SettingsId::kInitialWindowSize), 65535},
    }));
}

/**
 * @brief 建好一对已握手的 client/server 连接。
 *
 * 握手是同步阻塞的，两个 Connection 又互等对方的 SETTINGS，所以这里按
 * 「先给 server 喂 preface+SETTINGS → server 握手 → 把它的输出喂给 client →
 * client 握手」的顺序手动驱动，避免在同进程里互相等待。
 */
struct Pair {
    MemoryTransport client_transport;
    MemoryTransport server_transport;
    std::unique_ptr<Connection> client;
    std::unique_ptr<Connection> server;
    std::string error;
};

bool MakePair(Pair* pair, std::size_t initial_window = 65535,
              std::size_t max_header = 65536) {
    ConnectionConfig config;
    config.initial_window_size = static_cast<std::uint32_t>(initial_window);
    // 压测场景需要能收发超大 header，所以显式放宽。
    // 生产默认 64 KiB 是对的（防 CONTINUATION 泛滥与 HPACK 解压炸弹），
    // 但用默认值就没法测「1MB/10MB header 的吞吐与内存放大」—— 那正是本场景的目的。
    config.max_header_block_size = static_cast<std::uint32_t>(max_header);
    config.max_header_list_size = static_cast<std::uint32_t>(max_header);

    std::vector<std::uint8_t> server_inbound(kPreface, kPreface + kPrefaceLen);
    const auto settings = Bytes(MakeSettings({
        {static_cast<std::uint16_t>(SettingsId::kHeaderTableSize), 4096},
        {static_cast<std::uint16_t>(SettingsId::kMaxFrameSize), 16384},
        {static_cast<std::uint16_t>(SettingsId::kInitialWindowSize),
         static_cast<std::uint32_t>(initial_window)},
    }));
    server_inbound.insert(server_inbound.end(), settings.begin(), settings.end());
    pair->server_transport.PushInbound(server_inbound);

    pair->server = std::make_unique<Connection>(&pair->server_transport, Role::kServer, config);
    if (pair->server->Handshake(&pair->error) != ConnStatus::kOk) {
        return false;
    }
    pair->client_transport.PushInbound(pair->server_transport.TakeOutbound());
    pair->client = std::make_unique<Connection>(&pair->client_transport, Role::kClient, config);
    if (pair->client->Handshake(&pair->error) != ConnStatus::kOk) {
        return false;
    }
    // 客户端握手时会写出 preface + SETTINGS + ACK；服务端已经握手完成，
    // 这些字节不再需要（服务端不关心 ACK）。清掉以免干扰后续统计。
    (void)pair->client_transport.TakeOutbound();
    return true;
}

/// 把 client 写出的字节搬到 server，服务端读 n 个 HEADERS 事件。
int DrainServerHeaders(Pair* pair, int expected) {
    const auto out = pair->client_transport.TakeOutbound();
    if (!out.empty()) {
        pair->server_transport.PushInbound(out);
    }
    int seen = 0;
    IncomingEvent event;
    for (int guard = 0; guard < expected * 2 + 8 && seen < expected; ++guard) {
        if (pair->server->ReadEvent(&event, &pair->error) != ConnStatus::kOk) {
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            ++seen;
        }
    }
    return seen;
}

// ---------------------------------------------------------------------------
// 场景 1：批量并发流（pipeline）—— 测吞吐
// ---------------------------------------------------------------------------
void ScenarioBulkStreams(int stream_count) {
    Pair pair;
    Check(MakePair(&pair), "bulk: MakePair failed");
    if (g_failures != 0) {
        return;
    }
    const long rss_before = ReadVmRssKb();
    const double load = ReadLoadAvg();

    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(stream_count));
    for (int i = 0; i < stream_count; ++i) {
        paths.push_back("/bulk/" + std::to_string(i));
    }

    const double start = NowMs();
    int issued = 0;
    for (int i = 0; i < stream_count; ++i) {
        std::uint32_t stream_id = 0;
        const std::vector<HeaderField> request = {
            {":method", "GET"},
            {":path", paths[static_cast<std::size_t>(i)]},
            {":authority", "stress.local"},
        };
        if (pair.client->StartRequest(request, true, &stream_id, &pair.error) !=
            ConnStatus::kOk) {
            std::fprintf(stderr, "[stress] bulk: StartRequest #%d failed: %s\n", i,
                         pair.error.c_str());
            break;
        }
        ++issued;
    }
    const auto out = pair.client_transport.TakeOutbound();
    pair.server_transport.PushInbound(out);

    IncomingEvent event;
    int received = 0;
    for (int guard = 0; guard < stream_count * 2 + 16 && received < issued; ++guard) {
        if (pair.server->ReadEvent(&event, &pair.error) != ConnStatus::kOk) {
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            ++received;
        }
    }
    const double elapsed = NowMs() - start;
    const long rss_after = ReadVmRssKb();

    std::printf("[stress] 并发流 %5d: 发出=%d 收到=%d 耗时=%.1fms 吞吐=%.0f req/s "
                "字节=%zu 内存=%+ldKB load=%.2f\n",
                stream_count, issued, received, elapsed,
                elapsed > 0 ? (received * 1000.0 / elapsed) : 0.0, out.size(),
                (rss_before >= 0 && rss_after >= 0) ? (rss_after - rss_before) : 0, load);
    Check(received == issued, "bulk: every request must be received");
}

// ---------------------------------------------------------------------------
// 场景 2：串行单请求延迟 —— 测 p50/p90/p99
// ---------------------------------------------------------------------------
void ScenarioSerialLatency(int iterations) {
    Pair pair;
    Check(MakePair(&pair), "serial: MakePair failed");
    if (g_failures != 0) {
        return;
    }
    const double load = ReadLoadAvg();
    std::vector<double> latencies_us;
    latencies_us.reserve(static_cast<std::size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        const double t0 = NowMs();
        std::uint32_t stream_id = 0;
        const std::vector<HeaderField> request = {
            {":method", "GET"},
            {":path", "/serial"},
            {":authority", "stress.local"},
        };
        if (pair.client->StartRequest(request, true, &stream_id, &pair.error) !=
            ConnStatus::kOk) {
            break;
        }
        if (DrainServerHeaders(&pair, 1) != 1) {
            break;
        }
        latencies_us.push_back((NowMs() - t0) * 1000.0);
    }

    if (latencies_us.empty()) {
        Check(false, "serial: no samples");
        return;
    }
    std::printf("[stress] 串行 %6d 次: p50=%.2fus p90=%.2fus p99=%.2fus max=%.2fus load=%.2f\n",
                iterations, Percentile(latencies_us, 0.50), Percentile(latencies_us, 0.90),
                Percentile(latencies_us, 0.99),
                *std::max_element(latencies_us.begin(), latencies_us.end()), load);
}

// ---------------------------------------------------------------------------
// 场景 3：大 body 分片，真实走流控往返
// ---------------------------------------------------------------------------
void ScenarioLargeBody(std::size_t total_bytes) {
    Pair pair;
    Check(MakePair(&pair), "large-body: MakePair failed");
    if (g_failures != 0) {
        return;
    }
    const double load = ReadLoadAvg();
    const long rss_before = ReadVmRssKb();

    // 开一条流（HEADERS 不带 END_STREAM），然后分片发 DATA
    std::uint32_t stream_id = 0;
    if (pair.client->StartRequest({{":method", "POST"}, {":path", "/upload"},
                                   {":authority", "stress.local"}},
                                  false, &stream_id, &pair.error) != ConnStatus::kOk) {
        Check(false, "large-body: StartRequest failed");
        return;
    }
    // 把 HEADERS 先送到服务端，让它建立流
    pair.server_transport.PushInbound(pair.client_transport.TakeOutbound());
    {
        IncomingEvent event;
        for (int guard = 0; guard < 8; ++guard) {
            if (pair.server->ReadEvent(&event, &pair.error) != ConnStatus::kOk) break;
            if (event.kind == IncomingEvent::Kind::kHeaders) break;
        }
    }

    constexpr std::size_t kChunk = 16375;   // max_frame_size(16384) - 9 帧头
    std::vector<std::uint8_t> chunk(kChunk, 'x');
    const double start = NowMs();

    std::size_t sent = 0;
    std::size_t received = 0;
    int window_updates = 0;
    // 循环上限：每轮最多因为窗口耗尽而「卡住」若干次，给足余量
    for (int guard = 0; guard < static_cast<int>(total_bytes / kChunk) + 256; ++guard) {
        if (sent >= total_bytes) {
            break;
        }
        const std::size_t remaining = total_bytes - sent;
        const std::size_t take = std::min(kChunk, remaining);
        const bool last = (sent + take >= total_bytes);
        const auto status = pair.client->WriteData(
            stream_id, std::vector<std::uint8_t>(chunk.begin(),
                                                 chunk.begin() + static_cast<std::ptrdiff_t>(take)),
            last, &pair.error);
        if (status == ConnStatus::kFlowControl) {
            // 窗口不足：必须先把服务端的 WINDOW_UPDATE 搬回来
        } else if (status != ConnStatus::kOk) {
            std::fprintf(stderr, "[stress] large-body: WriteData failed at %zu: %s\n", sent,
                         pair.error.c_str());
            break;
        } else {
            sent += take;
        }

        // 无论是否写成功，都把 client 的字节送到 server 并处理
        const auto client_out = pair.client_transport.TakeOutbound();
        if (!client_out.empty()) {
            pair.server_transport.PushInbound(client_out);
        }
        IncomingEvent event;
        for (int inner = 0; inner < 32; ++inner) {
            const auto s = pair.server->ReadEvent(&event, &pair.error);
            if (s != ConnStatus::kOk) break;
            if (event.kind == IncomingEvent::Kind::kData) {
                received += event.data.size();
            }
        }
        // 把服务端产生的 WINDOW_UPDATE 搬回客户端
        const auto server_out = pair.server_transport.TakeOutbound();
        if (!server_out.empty()) {
            pair.client_transport.PushInbound(server_out);
            IncomingEvent update;
            for (int inner = 0; inner < 32; ++inner) {
                const auto s = pair.client->ReadEvent(&update, &pair.error);
                if (s != ConnStatus::kOk) break;
                if (update.kind == IncomingEvent::Kind::kWindowUpdate) {
                    ++window_updates;
                }
            }
        }
    }
    const double elapsed = NowMs() - start;
    const long rss_after = ReadVmRssKb();

    std::printf("[stress] 大 body %5zuKB: 发出=%zuKB 收到=%zuKB 耗时=%.1fms "
                "吞吐=%.1f MB/s WINDOW_UPDATE=%d 内存=%+ldKB load=%.2f\n",
                total_bytes / 1024, sent / 1024, received / 1024, elapsed,
                elapsed > 0 ? (static_cast<double>(received) / 1048576.0 * 1000.0 / elapsed)
                            : 0.0,
                window_updates,
                (rss_before >= 0 && rss_after >= 0) ? (rss_after - rss_before) : 0, load);
    Check(received == total_bytes, "large-body: all bytes must arrive");
    Check(window_updates > 0, "large-body: must exercise flow control (WINDOW_UPDATE)");
}

// ---------------------------------------------------------------------------
// 场景 4：连接复用 —— 相同请求重复 N 次，HPACK 动态表应让字节数大降
// ---------------------------------------------------------------------------
void ScenarioConnectionReuse(int iterations) {
    Pair pair;
    Check(MakePair(&pair), "reuse: MakePair failed");
    if (g_failures != 0) {
        return;
    }
    const double load = ReadLoadAvg();
    const long rss_before = ReadVmRssKb();

    std::size_t total_bytes = 0;
    std::size_t first_bytes = 0;
    const double start = NowMs();
    int done = 0;
    for (int i = 0; i < iterations; ++i) {
        std::uint32_t stream_id = 0;
        const std::vector<HeaderField> request = {
            {":method", "GET"},
            {":path", "/reuse/same/path"},
            {":authority", "stress.local"},
            {"user-agent", "go2cpp-stress/1.0"},
        };
        if (pair.client->StartRequest(request, true, &stream_id, &pair.error) !=
            ConnStatus::kOk) {
            break;
        }
        const auto bytes = pair.client_transport.TakeOutbound();
        if (i == 0) {
            first_bytes = bytes.size();
        }
        total_bytes += bytes.size();
        pair.server_transport.PushInbound(bytes);
        IncomingEvent event;
        for (int guard = 0; guard < 4; ++guard) {
            if (pair.server->ReadEvent(&event, &pair.error) != ConnStatus::kOk) break;
            if (event.kind == IncomingEvent::Kind::kHeaders) break;
        }
        ++done;
    }
    const double elapsed = NowMs() - start;
    const long rss_after = ReadVmRssKb();

    const double avg = done > 0 ? static_cast<double>(total_bytes) / done : 0.0;
    std::printf("[stress] 连接复用 %5d 次: 首帧=%zuB 平均=%.1fB 总字节=%zuB 耗时=%.1fms "
                "内存=%+ldKB load=%.2f\n",
                done, first_bytes, avg, total_bytes, elapsed,
                (rss_before >= 0 && rss_after >= 0) ? (rss_after - rss_before) : 0, load);
    // 动态表命中后，后续请求应显著小于首个请求
    Check(done == iterations, "reuse: all requests must complete");
    Check(avg < static_cast<double>(first_bytes) * 0.5,
          "reuse: HPACK dynamic table should shrink later requests");
}

// ---------------------------------------------------------------------------
// 场景 5：HPACK 动态表压力 —— 唯一 header 迫使持续插入与逐出
// ---------------------------------------------------------------------------
void ScenarioHpackTablePressure(int iterations) {
    Pair pair;
    Check(MakePair(&pair), "hpack: MakePair failed");
    if (g_failures != 0) {
        return;
    }
    const double load = ReadLoadAvg();
    const long rss_before = ReadVmRssKb();

    const double start = NowMs();
    std::size_t total_bytes = 0;
    int done = 0;
    for (int i = 0; i < iterations; ++i) {
        std::uint32_t stream_id = 0;
        // 唯一 header：每次都是新值，动态表必然插入并逐出旧条目
        const std::vector<HeaderField> request = {
            {":method", "GET"},
            {":path", "/unique/" + std::to_string(i)},
            {":authority", "stress.local"},
            {"x-request-id", "id-" + std::to_string(i) + "-abcdefghijklmnop"},
        };
        if (pair.client->StartRequest(request, true, &stream_id, &pair.error) !=
            ConnStatus::kOk) {
            break;
        }
        const auto bytes = pair.client_transport.TakeOutbound();
        total_bytes += bytes.size();
        pair.server_transport.PushInbound(bytes);
        IncomingEvent event;
        for (int guard = 0; guard < 4; ++guard) {
            if (pair.server->ReadEvent(&event, &pair.error) != ConnStatus::kOk) break;
            if (event.kind == IncomingEvent::Kind::kHeaders) break;
        }
        ++done;
    }
    const double elapsed = NowMs() - start;
    const long rss_after = ReadVmRssKb();

    std::printf("[stress] HPACK 动态表 %5d 次唯一 header: 总字节=%zuB 耗时=%.1fms "
                "内存=%+ldKB load=%.2f\n",
                done, total_bytes, elapsed,
                (rss_before >= 0 && rss_after >= 0) ? (rss_after - rss_before) : 0, load);
    Check(done == iterations, "hpack: all unique-header requests must complete");
    // 动态表大小为 4096，条目会被逐出；内存不应随迭代次数线性增长
    if (rss_before >= 0 && rss_after >= 0) {
        const long growth_kb = rss_after - rss_before;
        std::printf("[stress] HPACK 内存增长 %ld KB（%.2f KB/请求）\n", growth_kb,
                    done > 0 ? static_cast<double>(growth_kb) / done : 0.0);
    }
}

// ---------------------------------------------------------------------------
// 场景 6：CONTINUATION 压力 —— 超大 header 分片与拼接
// ---------------------------------------------------------------------------
void ScenarioContinuationPressure(std::size_t header_bytes) {
    Pair pair;
    // 放宽到 16 MiB：要测的是 1MB/10MB header 的分片吞吐，
    // 而默认 64 KiB 上限（安全闸门）会直接拒绝它们。
    // 上限本身的行为由 tests/network/test_connection.cpp 场景 11 单独验证。
    Check(MakePair(&pair, 65535, 16u * 1024u * 1024u),
          "continuation: MakePair failed");
    if (g_failures != 0) {
        return;
    }
    const double load = ReadLoadAvg();
    const long rss_before = ReadVmRssKb();

    const std::string big_value(header_bytes, 'z');
    std::uint32_t stream_id = 0;
    const double start = NowMs();
    const auto status = pair.client->StartRequest(
        {{":method", "GET"}, {":path", "/big-header"}, {"x-big", big_value}}, true,
        &stream_id, &pair.error);
    Check(status == ConnStatus::kOk, "continuation: StartRequest failed");
    if (status != ConnStatus::kOk) {
        std::fprintf(stderr, "[stress] continuation 错误: %s\n", pair.error.c_str());
        return;
    }
    const auto out = pair.client_transport.TakeOutbound();
    pair.server_transport.PushInbound(out);

    IncomingEvent event;
    bool got = false;
    std::size_t value_len = 0;
    for (int guard = 0; guard < 64 && !got; ++guard) {
        if (pair.server->ReadEvent(&event, &pair.error) != ConnStatus::kOk) {
            std::fprintf(stderr, "[stress] continuation 读失败: %s\n", pair.error.c_str());
            break;
        }
        if (event.kind == IncomingEvent::Kind::kHeaders) {
            got = true;
            for (const auto& field : event.headers) {
                if (field.name == "x-big") value_len = field.value.size();
            }
        }
    }
    const double elapsed = NowMs() - start;
    const long rss_after = ReadVmRssKb();

    std::printf("[stress] CONTINUATION %4zuKB: 编码后=%zuB 帧=%zu 解码值=%zuB "
                "耗时=%.1fms 内存=%+ldKB load=%.2f\n",
                header_bytes / 1024, out.size(), out.size() / 16384 + 1, value_len, elapsed,
                (rss_before >= 0 && rss_after >= 0) ? (rss_after - rss_before) : 0, load);
    Check(got, "continuation: server must assemble the header block");
    Check(value_len == header_bytes, "continuation: assembled value length mismatch");
}

}  // namespace

int main() {
    std::printf("=== HTTP/2 压力测试（MemoryTransport，单进程）===\n");
    std::printf("[stress] 初始 loadavg=%.2f VmRSS=%ldKB\n", ReadLoadAvg(), ReadVmRssKb());
    std::printf("[stress] 注意：项目规则 loadavg > 1.5 的测量作废\n\n");

    ScenarioBulkStreams(100);
    ScenarioBulkStreams(1000);
    ScenarioBulkStreams(5000);

    ScenarioSerialLatency(1000);

    ScenarioLargeBody(1024 * 1024);          // 1 MB
    ScenarioLargeBody(10 * 1024 * 1024);     // 10 MB

    ScenarioConnectionReuse(1000);
    ScenarioHpackTablePressure(5000);

    ScenarioContinuationPressure(1024 * 1024);        // 1 MB header
    ScenarioContinuationPressure(10 * 1024 * 1024);   // 10 MB header

    if (g_failures == 0) {
        std::printf("\n[stress] failures=0 loadavg=%.2f VmRSS=%ldKB\n", ReadLoadAvg(),
                    ReadVmRssKb());
    } else {
        std::printf("\n[stress] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
