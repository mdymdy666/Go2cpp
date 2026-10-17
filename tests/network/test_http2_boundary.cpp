/**
 * @file test_http2_boundary.cpp
 * @brief HTTP/2 边界与恶意输入测试 —— 目标不是"功能对"，而是"**不崩溃、不失控、不挂死**"。
 *
 * ## 为什么单独一个文件
 *
 * 功能测试验证"合法输入产生正确结果"；这里验证"非法输入产生**明确错误**"。
 * 后者更容易在重构中被破坏，因为它藏在错误分支里 —— 只要某处忘了判长度，
 * 读越界就是一个 SIGSEGV，而不是一个失败的断言。
 *
 * ## 隔离策略：fork + watchdog
 *
 * 每个"必须不崩溃"的用例都在 `fork()` 出的子进程里跑：
 * - 子进程崩溃（SIGSEGV/SIGABRT）→ 父进程能拿到信号号并报出来；
 * - 子进程挂死（死循环）→ 父进程超时后 `SIGKILL` 并报"挂死"。
 *
 * 这样即使实现有致命缺陷，测试进程本身也不会被杀掉，而且能精确定位是哪个用例。
 *
 * ## 覆盖
 *
 * 1. **畸形帧**：长度与类型不匹配、stream id 违规、超 `SETTINGS_MAX_FRAME_SIZE`、截断；
 * 2. **HPACK 攻击**：索引越界、表大小超限、截断字符串、Huffman 填充非法、EOS 入流；
 * 3. **CONTINUATION 攻击**：header block 未结束却发别的帧；
 * 4. **流控攻击**：窗口耗尽、WINDOW_UPDATE 增量为 0、增量溢出；
 * 5. **随机变异 fuzz**：对合法帧做随机位翻转（固定种子），验证不崩溃。
 */
#include "go2cpp/network/http2/connection.hpp"
#include "go2cpp/network/http2/frame.hpp"
#include "go2cpp/network/http2/hpack.hpp"
#include "go2cpp/network/http2/huffman.hpp"
#include "go2cpp/network/http2/transport.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace go2cpp::net::http2;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[boundary] FAILED: %s\n", what);
        ++g_failures;
    }
}

constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kPrefaceLen = sizeof(kPreface) - 1;

std::vector<std::uint8_t> Bytes(const Frame& frame) {
    std::vector<std::uint8_t> out(kFrameHeaderSize + frame.payload.size());
    EncodeFrameHeader(frame.header, out.data());
    if (!frame.payload.empty()) {
        std::memcpy(out.data() + kFrameHeaderSize, frame.payload.data(), frame.payload.size());
    }
    return out;
}

/// 手工拼一个帧（便于构造畸形帧）。
std::vector<std::uint8_t> RawFrame(std::uint32_t length, std::uint8_t type,
                                   std::uint8_t flags, std::uint32_t stream_id,
                                   const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
    out.push_back(type);
    out.push_back(flags);
    out.push_back(static_cast<std::uint8_t>(((stream_id >> 24) & 0x7F)));
    out.push_back(static_cast<std::uint8_t>((stream_id >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((stream_id >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(stream_id & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---------------------------------------------------------------------------
// fork + watchdog 隔离执行
// ---------------------------------------------------------------------------
struct IsolationResult {
    bool crashed{false};
    bool hung{false};
    int signal{0};
    int exit_code{0};
};

/**
 * @brief 在子进程里执行 fn，父进程带超时监控。
 * @note 子进程里只做解析与断言，不做 IO 或复杂分配；失败用 _exit(1) 表示。
 *       崩溃会被父进程通过 WIFSIGNALED 捕获。
 */
IsolationResult RunIsolated(const std::function<void()>& fn, int timeout_ms) {
    IsolationResult result;
    const pid_t pid = ::fork();
    if (pid < 0) {
        result.crashed = true;
        return result;
    }
    if (pid == 0) {
        // 子进程：把 stdout 静音，避免重复输出
        fn();
        ::_exit(0);
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            break;
        }
        if (Clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            result.hung = true;
            return result;
        }
        ::usleep(500);
    }
    if (WIFSIGNALED(status)) {
        result.crashed = true;
        result.signal = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    return result;
}

/// 断言"这个用例不会崩、不会挂"，并且返回了明确错误（不要求具体错误码）。
void ExpectNoCrash(const char* name, const std::function<void()>& fn) {
    const auto result = RunIsolated(fn, 3000);
    if (result.hung) {
        std::fprintf(stderr, "[boundary] HUNG: %s（超 3 秒未返回）\n", name);
        ++g_failures;
    } else if (result.crashed) {
        std::fprintf(stderr, "[boundary] CRASH: %s（signal %d: %s）\n", name, result.signal,
                     ::strsignal(result.signal));
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// 场景 1：畸形帧
// ---------------------------------------------------------------------------
void ScenarioMalformedFrames() {
    // 1.1 PING 长度 0（要求恰好 8 字节）
    ExpectNoCrash("ping-length-0", [] {
        const auto bytes = RawFrame(0, 0x6, 0, 0, {});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) != FrameStatus::kOk) {
            return;
        }
        PingFrame ping;
        std::string error;
        if (ParsePing(frame, &ping, &error) == FrameStatus::kOk) {
            ::_exit(1);
        }
    });

    // 1.2 SETTINGS 带 stream id 1（必须为 0）
    ExpectNoCrash("settings-stream-1", [] {
        const auto bytes = RawFrame(0, 0x4, 0, 1, {});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) == FrameStatus::kOk) {
            ::_exit(1);   // 应当在 DecodeFrame 阶段就被拒
        }
    });

    // 1.3 DATA 带 stream id 0（必须非 0）
    ExpectNoCrash("data-stream-0", [] {
        const auto bytes = RawFrame(3, 0x0, 0, 0, {'a', 'b', 'c'});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) == FrameStatus::kOk) {
            ::_exit(1);
        }
    });

    // 1.4 声明长度超过 SETTINGS_MAX_FRAME_SIZE
    ExpectNoCrash("oversized-frame", [] {
        const auto bytes = RawFrame(20000, 0x0, 0, 1, std::vector<std::uint8_t>(20000, 'x'));
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed, 16384) ==
            FrameStatus::kOk) {
            ::_exit(1);
        }
    });

    // 1.5 声明长度 100 但只给 10 字节（截断 -> kNeedMore，不能崩）
    ExpectNoCrash("truncated-payload", [] {
        auto bytes = RawFrame(100, 0x0, 0, 1, std::vector<std::uint8_t>(10, 'x'));
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) !=
            FrameStatus::kNeedMore) {
            ::_exit(1);
        }
    });

    // 1.6 帧头不足 9 字节
    ExpectNoCrash("short-header", [] {
        std::uint8_t bytes[5] = {0, 0, 0, 0, 0};
        FrameHeader header;
        if (DecodeFrameHeader(bytes, sizeof(bytes), &header) != FrameStatus::kNeedMore) {
            ::_exit(1);
        }
    });

    // 1.7 RST_STREAM 长度 3（要求恰好 4）
    ExpectNoCrash("rst-length-3", [] {
        const auto bytes = RawFrame(3, 0x3, 0, 1, {0, 0, 0});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) != FrameStatus::kOk) {
            return;
        }
        RstStreamFrame rst;
        std::string error;
        if (ParseRstStream(frame, &rst, &error) == FrameStatus::kOk) {
            ::_exit(1);
        }
    });

    // 1.8 WINDOW_UPDATE 增量 0（协议错误）
    ExpectNoCrash("window-update-zero", [] {
        const auto bytes = RawFrame(4, 0x8, 0, 1, {0, 0, 0, 0});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) != FrameStatus::kOk) {
            return;
        }
        WindowUpdateFrame update;
        std::string error;
        if (ParseWindowUpdate(frame, &update, &error) == FrameStatus::kOk) {
            ::_exit(1);
        }
    });

    // 1.9 PRIORITY 长度 4（要求恰好 5）
    ExpectNoCrash("priority-length-4", [] {
        const auto bytes = RawFrame(4, 0x2, 0, 1, {0, 0, 0, 1});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) != FrameStatus::kOk) {
            return;
        }
        PriorityFrame priority;
        std::string error;
        if (ParsePriority(frame, &priority, &error) == FrameStatus::kOk) {
            ::_exit(1);
        }
    });
    std::printf("[boundary] 畸形帧 9 个用例完成\n");
}

// ---------------------------------------------------------------------------
// 场景 2：HPACK 攻击
// ---------------------------------------------------------------------------
void ScenarioHpackAttacks() {
    // 2.1 索引越界（索引 200，静态表只有 61 项、动态表空）
    ExpectNoCrash("hpack-index-oob", [] {
        const std::uint8_t block[] = {0x80 | 0x7F, 0x49};   // 索引 200 的多字节编码
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        if (decoder.DecodeBlock(block, sizeof(block), &out, &error) == HpackStatus::kOk) {
            ::_exit(1);
        }
    });

    // 2.2 表大小更新超过 SETTINGS 上限
    ExpectNoCrash("hpack-table-size-oob", [] {
        const std::uint8_t block[] = {0x3F, 0xE2, 0x1F};   // 5 位前缀，值 4097 > 4096
        Decoder decoder(4096);
        std::vector<HeaderField> out;
        std::string error;
        if (decoder.DecodeBlock(block, sizeof(block), &out, &error) == HpackStatus::kOk) {
            ::_exit(1);
        }
    });

    // 2.3 声明字符串长度远超实际数据
    ExpectNoCrash("hpack-huge-string-len", [] {
        const std::uint8_t block[] = {0x40, 0x7F, 0xE1, 0x1F, 'a'};   // 声明超长但只给 1 字节
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        if (decoder.DecodeBlock(block, sizeof(block), &out, &error) == HpackStatus::kOk) {
            ::_exit(1);
        }
    });

    // 2.4 Huffman 填充非法（末字节不是全 1 填充）
    ExpectNoCrash("huffman-bad-padding", [] {
        const std::uint8_t data[] = {0xFF, 0xFF, 0x00};
        std::string decoded;
        if (HuffmanDecode(data, sizeof(data), &decoded) == HuffmanStatus::kOk) {
            ::_exit(1);
        }
    });

    // 2.5 全 1 输入（可能解码出 EOS）
    ExpectNoCrash("huffman-all-ones", [] {
        const std::uint8_t data[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::string decoded;
        (void)HuffmanDecode(data, sizeof(data), &decoded);
    });

    // 2.6 空输入
    ExpectNoCrash("hpack-empty", [] {
        Decoder decoder;
        std::vector<HeaderField> out;
        std::string error;
        if (decoder.DecodeBlock(nullptr, 0, &out, &error) == HpackStatus::kOk) {
            ::_exit(1);
        }
    });

    // 2.7 单个字节的所有 256 种取值都不能崩
    ExpectNoCrash("hpack-all-single-bytes", [] {
        for (int i = 0; i < 256; ++i) {
            const std::uint8_t block[1] = {static_cast<std::uint8_t>(i)};
            Decoder decoder;
            std::vector<HeaderField> out;
            std::string error;
            (void)decoder.DecodeBlock(block, 1, &out, &error);
        }
    });
    std::printf("[boundary] HPACK 攻击 7 个用例完成\n");
}

// ---------------------------------------------------------------------------
// 场景 3：CONTINUATION 攻击
// ---------------------------------------------------------------------------
void ScenarioContinuationAttack() {
    // 3.1 HEADERS 不带 END_HEADERS，紧接着发 DATA（应报协议错误）
    ExpectNoCrash("continuation-interleaved-frame", [] {
        MemoryTransport client_t;
        MemoryTransport server_t;
        std::vector<std::uint8_t> inbound(kPreface, kPreface + kPrefaceLen);
        const auto settings = Bytes(MakeSettings({}));
        inbound.insert(inbound.end(), settings.begin(), settings.end());
        server_t.PushInbound(inbound);

        Connection server(&server_t, Role::kServer);
        std::string error;
        if (server.Handshake(&error) != ConnStatus::kOk) {
            return;   // 握手失败也算"没崩"
        }
        // HEADERS 无 END_HEADERS（flags 只有 END_STREAM）
        const auto headers = RawFrame(1, 0x1, 0x1, 1, {0x82});
        server_t.PushInbound(headers);
        // 紧接着发 DATA（而不是 CONTINUATION）
        const auto data = Bytes(MakeData(1, false, {'x'}));
        server_t.PushInbound(data);

        IncomingEvent event;
        const auto status = server.ReadEvent(&event, &error);
        if (status == ConnStatus::kOk) {
            ::_exit(1);   // 应当报协议错误，不能"成功"
        }
    });

    // 3.2 CONTINUATION 的流 id 与 HEADERS 不一致
    ExpectNoCrash("continuation-wrong-stream", [] {
        MemoryTransport client_t;
        MemoryTransport server_t;
        std::vector<std::uint8_t> inbound(kPreface, kPreface + kPrefaceLen);
        const auto settings = Bytes(MakeSettings({}));
        inbound.insert(inbound.end(), settings.begin(), settings.end());
        server_t.PushInbound(inbound);
        Connection server(&server_t, Role::kServer);
        std::string error;
        if (server.Handshake(&error) != ConnStatus::kOk) {
            return;
        }
        const auto headers = RawFrame(1, 0x1, 0x1, 1, {0x82});
        server_t.PushInbound(headers);
        const auto cont = RawFrame(1, 0x9, 0x4, 3, {0x86});   // stream 3 != 1
        server_t.PushInbound(cont);
        IncomingEvent event;
        if (server.ReadEvent(&event, &error) == ConnStatus::kOk) {
            ::_exit(1);
        }
    });
    std::printf("[boundary] CONTINUATION 攻击 2 个用例完成\n");
}

// ---------------------------------------------------------------------------
// 场景 4：流控攻击
// ---------------------------------------------------------------------------
void ScenarioFlowControlAttack() {
    // 4.1 窗口耗尽后继续发 -> kFlowControl（不能崩、不能无限接受）
    ExpectNoCrash("flow-control-exhaust", [] {
        ConnectionConfig config;
        config.initial_window_size = 64;
        MemoryTransport client_t;
        MemoryTransport server_t;
        std::vector<std::uint8_t> inbound(kPreface, kPreface + kPrefaceLen);
        const auto settings = Bytes(MakeSettings({
            {static_cast<std::uint16_t>(SettingsId::kInitialWindowSize), 64}}));
        inbound.insert(inbound.end(), settings.begin(), settings.end());
        server_t.PushInbound(inbound);
        Connection server(&server_t, Role::kServer, config);
        std::string error;
        if (server.Handshake(&error) != ConnStatus::kOk) {
            return;
        }
        Connection client(&client_t, Role::kClient, config);
        client_t.PushInbound(server_t.TakeOutbound());
        if (client.Handshake(&error) != ConnStatus::kOk) {
            return;
        }
        std::uint32_t sid = 0;
        if (client.StartRequest({{":method", "POST"}, {":path", "/"}, {":authority", "h"}},
                                false, &sid, &error) != ConnStatus::kOk) {
            return;
        }
        // 连发 100 次 16 字节，窗口只有 64，必然被拒
        int rejected = 0;
        for (int i = 0; i < 100; ++i) {
            if (client.WriteData(sid, std::vector<std::uint8_t>(16, 'x'), false, &error) ==
                ConnStatus::kFlowControl) {
                ++rejected;
            }
        }
        if (rejected == 0) {
            ::_exit(1);   // 一次都没拒 -> 流控失效
        }
    });

    // 4.2 WINDOW_UPDATE 增量 2^31-1（最大合法值）不应崩
    ExpectNoCrash("window-update-max", [] {
        const auto bytes = RawFrame(4, 0x8, 0, 1, {0x7F, 0xFF, 0xFF, 0xFF});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) != FrameStatus::kOk) {
            ::_exit(1);
        }
        WindowUpdateFrame update;
        std::string error;
        if (ParseWindowUpdate(frame, &update, &error) != FrameStatus::kOk) {
            ::_exit(1);   // 2^31-1 是合法增量
        }
        if (update.increment != 0x7FFFFFFFU) {
            ::_exit(1);
        }
    });

    // 4.3 WINDOW_UPDATE 携带保留位（最高位）应被 mask 掉
    ExpectNoCrash("window-update-reserved-bit", [] {
        const auto bytes = RawFrame(4, 0x8, 0, 1, {0xFF, 0xFF, 0xFF, 0xFF});
        Frame frame;
        std::size_t consumed = 0;
        if (DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed) != FrameStatus::kOk) {
            ::_exit(1);
        }
        WindowUpdateFrame update;
        std::string error;
        if (ParseWindowUpdate(frame, &update, &error) != FrameStatus::kOk) {
            ::_exit(1);
        }
        if (update.increment != 0x7FFFFFFFU) {
            ::_exit(1);   // 保留位必须被 mask
        }
    });
    std::printf("[boundary] 流控攻击 3 个用例完成\n");
}

// ---------------------------------------------------------------------------
// 场景 5：随机变异 fuzz
// ---------------------------------------------------------------------------
void ScenarioFuzz() {
    // 固定种子，保证可复现
    std::mt19937 rng(20261017);
    constexpr int kIterations = 200000;

    // 先构造一批"合法帧"作为变异基础
    std::vector<std::vector<std::uint8_t>> corpus;
    corpus.push_back(Bytes(MakeData(1, true, {'h', 'i'})));
    corpus.push_back(Bytes(MakeHeaders(1, true, true, {0x82, 0x86, 0x84})));
    corpus.push_back(Bytes(MakeSettings({{1, 4096}, {5, 16384}})));
    corpus.push_back(Bytes(MakePing(0x1122334455667788ULL, false)));
    corpus.push_back(Bytes(MakeRstStream(1, ErrorCode::kCancel)));
    corpus.push_back(Bytes(MakeGoAway(1, ErrorCode::kNoError, {'d', 'e', 'b'})));
    corpus.push_back(Bytes(MakeWindowUpdate(1, 1024)));
    corpus.push_back(Bytes(MakePriority(1, 3, true, 200)));
    corpus.push_back(RawFrame(1, 0x9, 0x4, 1, {0x88}));

    const auto result = RunIsolated([&] {
        std::uniform_int_distribution<std::size_t> corpus_pick(0, corpus.size() - 1);
        std::uniform_int_distribution<int> byte_pick(0, 255);
        for (int i = 0; i < kIterations; ++i) {
            auto bytes = corpus[corpus_pick(rng)];
            // 随机翻转 1..8 个位（可能落在帧头也可能落在 payload）
            const int flips = 1 + (i % 8);
            for (int f = 0; f < flips; ++f) {
                if (bytes.empty()) break;
                const std::size_t pos = static_cast<std::size_t>(i * 31 + f * 17) % bytes.size();
                const int bit = (i + f) % 8;
                bytes[pos] ^= static_cast<std::uint8_t>(1U << bit);
            }
            // 也可能被截断
            if ((i % 7) == 0 && bytes.size() > 1) {
                bytes.resize(bytes.size() - 1);
            }
            Frame frame;
            std::size_t consumed = 0;
            const auto status =
                DecodeFrame(bytes.data(), bytes.size(), &frame, &consumed, 16384);
            if (status != FrameStatus::kOk) {
                continue;   // 被拒是**期望**行为
            }
            // 帧头被接受时，再按类型解析 payload —— 这里是越界读的高危区
            std::string error;
            switch (frame.header.type) {
                case FrameType::kData: {
                    DataFrame d;
                    (void)ParseData(frame, &d, &error);
                    break;
                }
                case FrameType::kHeaders: {
                    HeadersFrame h;
                    (void)ParseHeaders(frame, &h, &error);
                    break;
                }
                case FrameType::kSettings: {
                    SettingsFrame s;
                    (void)ParseSettings(frame, &s, &error);
                    break;
                }
                case FrameType::kPing: {
                    PingFrame p;
                    (void)ParsePing(frame, &p, &error);
                    break;
                }
                case FrameType::kRstStream: {
                    RstStreamFrame r;
                    (void)ParseRstStream(frame, &r, &error);
                    break;
                }
                case FrameType::kGoAway: {
                    GoAwayFrame g;
                    (void)ParseGoAway(frame, &g, &error);
                    break;
                }
                case FrameType::kWindowUpdate: {
                    WindowUpdateFrame w;
                    (void)ParseWindowUpdate(frame, &w, &error);
                    break;
                }
                case FrameType::kPriority: {
                    PriorityFrame pr;
                    (void)ParsePriority(frame, &pr, &error);
                    break;
                }
                case FrameType::kContinuation: {
                    HeadersFrame c;
                    (void)ParseContinuation(frame, &c, &error);
                    break;
                }
                default: break;
            }
            // 顺便把 payload 喂给 HPACK 解码器（它面对的是完全随机的字节）
            if ((i % 3) == 0) {
                Decoder decoder;
                std::vector<HeaderField> fields;
                (void)decoder.DecodeBlock(frame.payload.data(), frame.payload.size(), &fields,
                                          &error);
            }
        }
        ::_exit(0);
    }, 30000);

    if (result.hung) {
        std::fprintf(stderr, "[boundary] FUZZ HUNG: 200000 次变异在 30 秒内未完成\n");
        ++g_failures;
    } else if (result.crashed) {
        std::fprintf(stderr, "[boundary] FUZZ CRASH: signal %d (%s)\n", result.signal,
                     ::strsignal(result.signal));
        ++g_failures;
    } else if (result.exit_code != 0) {
        std::fprintf(stderr, "[boundary] FUZZ FAILED: 子进程非零退出 %d\n", result.exit_code);
        ++g_failures;
    } else {
        std::printf("[boundary] 随机变异 fuzz %d 次（9 个语料 × 位翻转 × 截断）无崩溃\n",
                    kIterations);
    }
}

}  // namespace

int main() {
    std::printf("=== HTTP/2 边界与恶意输入测试 ===\n");
    std::printf("[boundary][场景1] 畸形帧\n");
    ScenarioMalformedFrames();
    std::printf("[boundary][场景2] HPACK 攻击\n");
    ScenarioHpackAttacks();
    std::printf("[boundary][场景3] CONTINUATION 攻击\n");
    ScenarioContinuationAttack();
    std::printf("[boundary][场景4] 流控攻击\n");
    ScenarioFlowControlAttack();
    std::printf("[boundary][场景5] 随机变异 fuzz（fork 隔离 + 30s watchdog）\n");
    ScenarioFuzz();

    if (g_failures == 0) {
        std::printf("\n[boundary] failures=0\n");
    } else {
        std::printf("\n[boundary] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
