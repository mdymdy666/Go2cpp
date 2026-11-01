#pragma once

/**
 * @file rpc_test_support.hpp
 * @brief RPC 测试的公共基础设施：迷你断言框架 + 测试服务 + 统计工具。
 *
 * @details 与仓库其它独立测试保持同一风格：不引入 gtest，失败累计后统一
 *          返回非零；服务/消息类型在头文件里定义，协议/端到端/流式/基准
 *          四个可执行文件共用。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "go2cpp/rpc/client.hpp"
#include "go2cpp/rpc/protocol.hpp"
#include "go2cpp/rpc/registry.hpp"
#include "go2cpp/rpc/server.hpp"
#include "go2cpp/rpc/status.hpp"

namespace rpctest {

// ---------------------------------------------------------------------------
// 迷你测试框架
// ---------------------------------------------------------------------------

inline int g_failures = 0;
inline std::size_t g_checks = 0;

/// @brief 打印用例名。
inline void Announce(const char* name) {
    std::printf("[rpc] %s\n", name);
    std::fflush(stdout);
}

/// @brief 记录一次断言失败。
inline void Fail(const char* file, int line, const char* expression) {
    ++g_failures;
    std::printf("  FAIL %s:%d: %s\n", file, line, expression);
    std::fflush(stdout);
}

/// @brief 用例结束时打印汇总并返回进程退出码。
inline int Summary(const char* suite) {
    std::printf("[rpc] %s: %zu checks, %d failures\n", suite, g_checks, g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}

/// @brief 断言表达式为真（累计失败，不中断）。
#define RPC_CHECK(expression)                                                        \
    do {                                                                             \
        ++::rpctest::g_checks;                                                       \
        if (!(expression)) {                                                         \
            ::rpctest::Fail(__FILE__, __LINE__, #expression);                        \
        }                                                                            \
    } while (false)

/// @brief 断言两值相等。
#define RPC_CHECK_EQ(left, right) RPC_CHECK((left) == (right))

/// @brief 取环境变量整数（测试规模可调）。
inline std::int64_t EnvInt(const char* name, std::int64_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || value <= 0) {
        return fallback;
    }
    return static_cast<std::int64_t>(value);
}

/// @brief 当前时间（微秒，steady_clock）。
inline double NowMicros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) /
           1000.0;
}

/// @brief 分位数（`sorted` 必须已升序；空表返回 0）。
inline double Percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const std::size_t index = static_cast<std::size_t>(position);
    return sorted[index];
}

/// @brief 正态/长尾统计摘要。
struct LatencySummary {
    double min_us = 0.0;
    double p50_us = 0.0;
    double p90_us = 0.0;
    double p99_us = 0.0;
    double p999_us = 0.0;
    double max_us = 0.0;
    double mean_us = 0.0;
};

/// @brief 从未排序样本计算摘要。
inline LatencySummary Summarize(std::vector<double> samples) {
    LatencySummary summary;
    if (samples.empty()) {
        return summary;
    }
    std::sort(samples.begin(), samples.end());
    double total = 0.0;
    for (const double value : samples) {
        total += value;
    }
    summary.min_us = samples.front();
    summary.p50_us = Percentile(samples, 0.50);
    summary.p90_us = Percentile(samples, 0.90);
    summary.p99_us = Percentile(samples, 0.99);
    summary.p999_us = Percentile(samples, 0.999);
    summary.max_us = samples.back();
    summary.mean_us = total / static_cast<double>(samples.size());
    return summary;
}

// ---------------------------------------------------------------------------
// 测试消息类型
// ---------------------------------------------------------------------------

/// @brief 用户 id。
struct UserId {
    std::int64_t id = 0;
};

/// @brief 用户信息。
struct User {
    std::int64_t id = 0;
    std::string name;
    std::int32_t level = 0;
};

/// @brief 创建用户请求。
struct CreateRequest {
    std::string name;
    std::int64_t seed = 0;
};

/// @brief 流式订阅请求。
struct WatchRequest {
    std::int32_t count = 0;
    std::int32_t interval_us = 0;
};

/// @brief 流消息。
struct ChatMessage {
    std::int32_t seq = 0;
    std::string text;
};

/// @brief 计数器快照。
struct CounterSnapshot {
    std::int64_t calls = 0;
    std::int64_t touches = 0;
};

/// @brief 大载荷请求。
struct BigRequest {
    std::int32_t size = 0;
    std::uint8_t fill = 0;
};

}  // namespace rpctest

// ---------------------------------------------------------------------------
// 字段反射（消息都是普通结构体，字段表独立于类型定义）
// ---------------------------------------------------------------------------

namespace go2cpp::reflect {

template <>
struct FieldList<rpctest::UserId> {
    static constexpr TypeId kId = 200;
    static constexpr const char* kName = "rpc.UserId";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(rpctest::UserId, id),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<rpctest::User> {
    static constexpr TypeId kId = 201;
    static constexpr const char* kName = "rpc.User";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(rpctest::User, id),
            GO2CPP_REFLECT_FIELD(rpctest::User, name),
            GO2CPP_REFLECT_FIELD(rpctest::User, level),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<rpctest::CreateRequest> {
    static constexpr TypeId kId = 202;
    static constexpr const char* kName = "rpc.CreateRequest";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(rpctest::CreateRequest, name),
            GO2CPP_REFLECT_FIELD(rpctest::CreateRequest, seed),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<rpctest::WatchRequest> {
    static constexpr TypeId kId = 203;
    static constexpr const char* kName = "rpc.WatchRequest";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(rpctest::WatchRequest, count),
            GO2CPP_REFLECT_FIELD(rpctest::WatchRequest, interval_us),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<rpctest::ChatMessage> {
    static constexpr TypeId kId = 204;
    static constexpr const char* kName = "rpc.ChatMessage";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(rpctest::ChatMessage, seq),
            GO2CPP_REFLECT_FIELD(rpctest::ChatMessage, text),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<rpctest::CounterSnapshot> {
    static constexpr TypeId kId = 205;
    static constexpr const char* kName = "rpc.CounterSnapshot";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(rpctest::CounterSnapshot, calls),
            GO2CPP_REFLECT_FIELD(rpctest::CounterSnapshot, touches),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct FieldList<rpctest::BigRequest> {
    static constexpr TypeId kId = 206;
    static constexpr const char* kName = "rpc.BigRequest";
    static const FieldDescriptor* Fields(std::size_t& count) {
        static const FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(rpctest::BigRequest, size),
            GO2CPP_REFLECT_FIELD(rpctest::BigRequest, fill),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

}  // namespace go2cpp::reflect

namespace rpctest {

namespace rpc = ::go2cpp::rpc;
using rpc::Status;
using rpc::StatusCode;

// ---------------------------------------------------------------------------
// 测试服务：覆盖值返回/out 参数/void/多参数/错误/大载荷/流式
// ---------------------------------------------------------------------------

/// @brief 测试服务（方法表由 GO2CPP_RPC_SERVICE 生成）。
class UserService {
   public:
    /// @brief 值返回。
    User Get(const UserId& id) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        User user;
        user.id = id.id;
        user.name = "user-" + std::to_string(id.id);
        user.level = static_cast<std::int32_t>(id.id % 100);
        return user;
    }

    /// @brief 输出参数 + 状态。
    Status Create(const CreateRequest& request, User* out) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        if (out == nullptr) {
            return Status(StatusCode::kInternal, "null out");
        }
        if (request.name.empty()) {
            return Status(StatusCode::kInvalidArgument, "empty name");
        }
        const std::int64_t id = m_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
        out->id = id;
        out->name = request.name;
        out->level = static_cast<std::int32_t>(request.seed % 10);
        return Status::Ok();
    }

    /// @brief 仅状态。
    Status Remove(const UserId& id) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        if (id.id == 0) {
            return Status(StatusCode::kNotFound, "user not found");
        }
        return Status::Ok();
    }

    /// @brief 仅动作。
    void Touch(const UserId& id) {
        (void)id;
        m_touches.fetch_add(1, std::memory_order_relaxed);
    }

    /// @brief 服务端推流。
    Status Watch(const WatchRequest& request, rpc::ServerStream* stream) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        if (stream == nullptr) {
            return Status(StatusCode::kInternal, "null stream");
        }
        for (std::int32_t index = 0; index < request.count; ++index) {
            if (stream->cancelled()) {
                m_cancelled.fetch_add(1, std::memory_order_relaxed);
                return Status(StatusCode::kCancelled, "watch cancelled");
            }
            ChatMessage message;
            message.seq = index;
            message.text = "chunk-" + std::to_string(index);
            if (!stream->Send(message)) {
                m_send_failures.fetch_add(1, std::memory_order_relaxed);
                return Status(StatusCode::kUnavailable, "stream send failed");
            }
            if (request.interval_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(request.interval_us));
            }
        }
        return Status::Ok();
    }

    /// @brief 慢 handler（用于超时/取消测试）。
    Status Slow(const UserId& id) {
        (void)id;
        m_calls.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return Status::Ok();
    }

    /// @brief 业务错误。
    Status Fail(const UserId& id) {
        (void)id;
        m_calls.fetch_add(1, std::memory_order_relaxed);
        return Status(StatusCode::kPermissionDenied, "permission denied");
    }

    /// @brief 多参数 + 输出参数。
    Status Add(std::int64_t left, std::int64_t right, std::int64_t* out) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        if (out == nullptr) {
            return Status(StatusCode::kInternal, "null out");
        }
        *out = left + right;
        return Status::Ok();
    }

    /// @brief 字节数组往返。
    Status EchoBytes(const std::vector<std::uint8_t>& input,
                     std::vector<std::uint8_t>* out) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        if (out == nullptr) {
            return Status(StatusCode::kInternal, "null out");
        }
        *out = input;
        return Status::Ok();
    }

    /// @brief 字符串往返。
    Status EchoString(const std::string& input, std::string* out) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        if (out == nullptr) {
            return Status(StatusCode::kInternal, "null out");
        }
        *out = input;
        return Status::Ok();
    }

    /// @brief 大载荷（1 MiB 级）。
    Status Big(const BigRequest& request, std::vector<std::uint8_t>* out) {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        if (out == nullptr) {
            return Status(StatusCode::kInternal, "null out");
        }
        if (request.size < 0) {
            return Status(StatusCode::kInvalidArgument, "negative size");
        }
        out->assign(static_cast<std::size_t>(request.size), request.fill);
        return Status::Ok();
    }

    /// @brief 计数快照（第二次验证值返回类型）。
    CounterSnapshot Snapshot() {
        CounterSnapshot snapshot;
        snapshot.calls = m_calls.load(std::memory_order_relaxed);
        snapshot.touches = m_touches.load(std::memory_order_relaxed);
        return snapshot;
    }

    /// @brief 已观察到的取消次数（服务端感知取消的证据）。
    std::int64_t cancelled_count() const {
        return m_cancelled.load(std::memory_order_relaxed);
    }

    /// @brief 流发送失败次数。
    std::int64_t send_failures() const {
        return m_send_failures.load(std::memory_order_relaxed);
    }

   private:
    std::atomic<std::int64_t> m_calls{0};
    std::atomic<std::int64_t> m_touches{0};
    std::atomic<std::int64_t> m_cancelled{0};
    std::atomic<std::int64_t> m_send_failures{0};
    std::atomic<std::int64_t> m_next_id{0};
};

}  // namespace rpctest

namespace rpctest {

// 宏必须在服务类型所在命名空间展开：`AddService` 用 ADL 找这个重载。
GO2CPP_RPC_SERVICE(UserService, Get, Create, Remove, Touch, Watch, Slow, Fail, Add,
                   EchoBytes, EchoString, Big, Snapshot)

/// @brief 新建并启动一个监听回环端口的服务端。
/// @param server 服务端对象（调用方持有）。
/// @param service 测试服务（生命周期必须覆盖 server）。
/// @param address 输出：`"127.0.0.1:port"`。
/// @return 成功 true。
inline bool StartTestServer(rpc::Server& server, UserService& service, std::string* address) {
    if (!server.Register("User", &service)) {
        return false;
    }
    rpc::Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = 0;
    if (!server.Listen(endpoint)) {
        return false;
    }
    if (address != nullptr) {
        *address = "127.0.0.1:" + std::to_string(server.port());
    }
    return true;
}

}  // namespace rpctest
