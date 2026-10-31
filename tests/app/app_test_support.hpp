#pragma once

/**
 * @file app_test_support.hpp
 * @brief app 模块测试的轻量断言 + 双端连接驱动器。
 *
 * @details 不依赖项目根 tests/test_main.cpp：app 测试是独立可执行文件，
 *          每个文件自带 main。断言输出包含场景名与计数，便于从 ctest
 *          日志定位失败用例。
 *
 * @author go2cpp contributors
 * @date 2026-10-31
 * @copyright MIT License
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/app/connection.hpp"
#include "go2cpp/app/kv_stream.hpp"
#include "go2cpp/app/resource.hpp"
#include "go2cpp/app/stream_table.hpp"
#include "go2cpp/app/transport.hpp"

namespace app_test {

inline std::atomic<int> g_failures{0};
inline std::atomic<int> g_checks{0};
inline std::mutex g_output_mutex;

inline void Check(bool condition, const char* expression, const char* file, int line) {
    g_checks.fetch_add(1);
    if (condition) {
        return;
    }
    g_failures.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

inline void Require(bool condition, const char* expression, const char* file, int line) {
    g_checks.fetch_add(1);
    if (condition) {
        return;
    }
    g_failures.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::fprintf(stderr, "%s:%d: requirement failed: %s\n", file, line, expression);
    std::fflush(stderr);
    std::abort();
}

inline void Announce(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::printf("[scenario] %s\n", name.c_str());
    std::fflush(stdout);
}

inline void Note(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::printf("[info] %s\n", text.c_str());
    std::fflush(stdout);
}

inline std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline int Finish(const char* suite) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::printf("[%s] checks=%d failures=%d\n", suite, g_checks.load(),
                g_failures.load());
    std::fflush(stdout);
    return g_failures.load() == 0 ? 0 : 1;
}

/// 生成确定性测试字节。
inline std::vector<std::uint8_t> MakePattern(std::size_t size, std::uint32_t seed = 1) {
    std::vector<std::uint8_t> data(size);
    std::uint32_t state = seed == 0 ? 1u : seed;
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        data[i] = static_cast<std::uint8_t>(state >> 24);
    }
    return data;
}

/// 生成 n 个 `k<i>=<value>` 条目。
inline go2cpp::app::KvTable MakeKv(std::size_t count, std::size_t value_size = 8,
                                   std::string_view prefix = "k") {
    go2cpp::app::KvTable table;
    for (std::size_t i = 0; i < count; ++i) {
        std::string value;
        value.resize(value_size);
        for (std::size_t j = 0; j < value_size; ++j) {
            value[j] = static_cast<char>('a' + ((i + j) % 26));
        }
        table.Set(std::string(prefix) + std::to_string(i), value);
    }
    return table;
}

inline std::size_t CountEvents(const std::vector<go2cpp::app::Event>& events,
                               go2cpp::app::EventType type) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (event.type == type) {
            ++count;
        }
    }
    return count;
}

/// 取最后一条指定类型事件；没有返回 nullptr。
inline const go2cpp::app::Event* FindLastEvent(
    const std::vector<go2cpp::app::Event>& events, go2cpp::app::EventType type) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->type == type) {
            return &*it;
        }
    }
    return nullptr;
}

/**
 * @brief 一对通过 MemoryTransport 相连的 Connection（客户端 + 服务端）。
 *
 * @details IO 由单线程交替 `Step(0)` 驱动：发送与接收都在同一线程完成，
 *          测试完全确定性，不需要 sleep 竞争。没有数据可推进时让出 CPU
 *          50µs，避免忙等烧核。
 */
class Pipe {
   public:
    struct Peer {
        std::unique_ptr<go2cpp::app::Connection> conn;  ///< 连接。
        std::vector<go2cpp::app::Event> events;         ///< 收到的事件。
    };

    explicit Pipe(go2cpp::app::ConnectionConfig client_config = {},
                  go2cpp::app::ConnectionConfig server_config = {}) {
        auto pair = go2cpp::app::MemoryTransport::CreatePair();
        client_.conn = std::make_unique<go2cpp::app::Connection>(
            std::move(pair.first), go2cpp::app::Role::kClient, client_config);
        server_.conn = std::make_unique<go2cpp::app::Connection>(
            std::move(pair.second), go2cpp::app::Role::kServer, server_config);
        client_.conn->SetEventHandler(
            [this](const go2cpp::app::Event& event) { client_.events.push_back(event); });
        server_.conn->SetEventHandler(
            [this](const go2cpp::app::Event& event) { server_.events.push_back(event); });
    }

    ~Pipe() { Close(); }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    bool Start() {
        std::string error;
        if (!client_.conn->Start(&error)) {
            return false;
        }
        if (!server_.conn->Start(&error)) {
            return false;
        }
        // 完成握手（preface + SETTINGS + ACK）。
        return PumpUntil(
            [this] {
                return client_.conn->ready() && server_.conn->ready();
            },
            3000);
    }

    go2cpp::app::Connection& client() { return *client_.conn; }
    go2cpp::app::Connection& server() { return *server_.conn; }
    Peer& client_peer() { return client_; }
    Peer& server_peer() { return server_; }
    const std::vector<go2cpp::app::Event>& client_events() const {
        return client_.events;
    }
    const std::vector<go2cpp::app::Event>& server_events() const {
        return server_.events;
    }

    /// 追加一个服务端事件回调（与测试事件收集器并存）。
    void AddServerHandler(std::function<void(const go2cpp::app::Event&)> handler) {
        server_.conn->SetEventHandler(
            [this, handler = std::move(handler)](const go2cpp::app::Event& event) {
                if (handler) {
                    handler(event);
                }
                server_.events.push_back(event);
            });
    }

    /// 追加一个客户端事件回调（与测试事件收集器并存）。
    void AddClientHandler(std::function<void(const go2cpp::app::Event&)> handler) {
        client_.conn->SetEventHandler(
            [this, handler = std::move(handler)](const go2cpp::app::Event& event) {
                if (handler) {
                    handler(event);
                }
                client_.events.push_back(event);
            });
    }

    /// 驱动两端直到条件成立或超时。
    bool PumpUntil(const std::function<bool()>& done, std::uint32_t timeout_ms) {
        const std::uint64_t deadline = NowMs() + timeout_ms;
        int idle = 0;
        while (!done()) {
            if (NowMs() > deadline) {
                return done();
            }
            const int client_frames = client_.conn->Step(0);
            const int server_frames = server_.conn->Step(0);
            if (client_frames == 0 && server_frames == 0) {
                ++idle;
                if (idle >= 4) {
                    std::this_thread::sleep_for(std::chrono::microseconds(30));
                }
            } else {
                idle = 0;
            }
            if (client_.conn->closed() && server_.conn->closed()) {
                return done();
            }
        }
        return true;
    }

    /// 驱动固定时长（用于等待窗口更新/心跳等异步推进）。
    void PumpFor(std::uint32_t duration_ms) {
        const std::uint64_t deadline = NowMs() + duration_ms;
        while (NowMs() < deadline) {
            client_.conn->Step(1);
            server_.conn->Step(1);
        }
    }

    /// 等待某一端收到指定数量的某类事件。
    bool WaitEvents(bool on_server, go2cpp::app::EventType type, std::size_t count,
                    std::uint32_t timeout_ms) {
        return PumpUntil(
            [this, on_server, type, count] {
                const auto& events = on_server ? server_.events : client_.events;
                std::size_t seen = 0;
                for (const auto& event : events) {
                    if (event.type == type) {
                        ++seen;
                    }
                }
                return seen >= count;
            },
            timeout_ms);
    }

    void Close() {
        if (client_.conn) {
            client_.conn->Close();
        }
        if (server_.conn) {
            server_.conn->Close();
        }
    }

   private:
    Peer client_;
    Peer server_;
};

/// 常见配置：小窗口用于流控测试。
inline go2cpp::app::ConnectionConfig SmallWindowConfig() {
    go2cpp::app::ConnectionConfig config;
    config.table.initial_stream_window = 8 * 1024;
    config.table.connection_window = 32 * 1024;
    config.max_frame_payload = 4 * 1024;
    config.max_bytes_per_flush = 64 * 1024;
    return config;
}

}  // namespace app_test

#define APP_CHECK(expr) ::app_test::Check((expr), #expr, __FILE__, __LINE__)
#define APP_REQUIRE(expr) ::app_test::Require((expr), #expr, __FILE__, __LINE__)
#define APP_SCENARIO(name) ::app_test::Announce(name)
