/**
 * @file test_socket_whitelist.cpp
 * @brief socket 白名单、fd 复用（ABA）、审计 fail-closed、环形容量与并发。
 *
 * 覆盖：
 * 1. 未登记 / 已移出白名单的 fd 不允许关闭，且 fd 保持可用；
 * 2. 普通文件 fd 不能登记，也不能被 socket_close 关闭；
 * 3. fd 被复用为文件或新 socket 时，inode 校验拒绝旧记录（ABA）；
 * 4. 审计写入失败时按 fail-closed 拒绝关闭；
 * 5. 真实文件审计汇：JSON Lines、权限 0600；
 * 6. 事件环形缓冲溢出计数与查询截断；
 * 7. 外部关闭后 list 不再展示、events 保留 closed、stats 明确报错；
 * 8. 8 线程并发 list/stats/events + 与 close 竞争不崩溃。
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "socket_test_util.hpp"
#include "test_util.hpp"

#include "mcp/tools/socket_registry.hpp"
#include "mcp/tools/socket_tools.hpp"

namespace {

using go2cpp::mcp::Registry;
using go2cpp::mcp::json;
using go2cpp::mcp::test::CallTool;
using go2cpp::mcp::test::Fd;
using go2cpp::mcp::test::MakeTcpPair;
using go2cpp::mcp::test::ParseJson;
using go2cpp::mcp::test::TcpPair;
using go2cpp::mcp::tools::FileSocketAuditSink;
using go2cpp::mcp::tools::MemorySocketAuditSink;
using go2cpp::mcp::tools::SocketEventKind;
using go2cpp::mcp::tools::SocketRegistry;

void ResetAll() { SocketRegistry::Instance().Reset(); }

std::shared_ptr<MemorySocketAuditSink> SetupTools(Registry* registry) {
    auto audit = std::make_shared<MemorySocketAuditSink>();
    go2cpp::mcp::tools::RegisterSocketTools(*registry, audit);
    return audit;
}

/// 未登记、已移出白名单：拒绝关闭且 fd 保持可用。
void TestWhitelistDenied() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    auto audit = SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();

    const auto [is_error, text] = CallTool(
        registry, "socket_close", {{"fd", pair.server.get()}});
    CHECK(is_error);
    const json out = ParseJson(text);
    CHECK(out["closed"] == false);
    CHECK(out["error"].get<std::string>().find("未登记") != std::string::npos);
    CHECK(::fcntl(pair.server.get(), F_GETFD) != -1);
    CHECK(audit->size() == 1);
    CHECK(audit->Records()[0]["result"] == "denied");
    CHECK(audit->Records()[0]["protocol"] == "tcp");

    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "temporary", &error));
    CHECK(socket_registry.Unregister(pair.server.get()));
    const auto [second_error, second_text] = CallTool(
        registry, "socket_close", {{"fd", pair.server.get()}});
    CHECK(second_error);
    CHECK(::fcntl(pair.server.get(), F_GETFD) != -1);
    CHECK(audit->size() == 2);
}

/// 普通文件 fd：不能登记，也不能被关闭。
void TestNonSocketFd() {
    ResetAll();
    Fd file(::open("/dev/null", O_RDONLY));
    CHECK(file.valid());
    if (!file.valid()) {
        return;
    }
    Registry registry;
    auto audit = SetupTools(&registry);
    std::string error;
    CHECK(!SocketRegistry::Instance().Register(file.get(), "file", &error));
    CHECK(error.find("不是 socket") != std::string::npos);

    const auto [is_error, text] = CallTool(
        registry, "socket_close", {{"fd", file.get()}});
    CHECK(is_error);
    CHECK(::fcntl(file.get(), F_GETFD) != -1);
    CHECK(audit->size() == 1);
    CHECK(audit->Records()[0]["result"] == "denied");
}

/// fd 关闭后复用为文件/新 socket：旧登记记录不能误关闭新 fd。
void TestFdReuseAba() {
    ResetAll();
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    Registry registry;
    (void)SetupTools(&registry);
    std::string error;

    // 场景 A：旧 fd 被复用为普通文件。
    TcpPair first = MakeTcpPair();
    CHECK(first.valid());
    if (!first.valid()) {
        return;
    }
    CHECK(socket_registry.Register(first.server.get(), "aba-file", &error));
    const int raw = first.server.Release();
    CHECK(::close(raw) == 0);
    Fd file(::open("/dev/null", O_RDONLY));
    CHECK(file.valid());
    CHECK(::dup2(file.get(), raw) == raw);
    const auto [file_error, file_text] =
        CallTool(registry, "socket_close", {{"fd", raw}});
    CHECK(file_error);
    CHECK(::fcntl(raw, F_GETFD) != -1);

    // 场景 B：旧 fd 被复用为新 socket（inode 不同）。
    TcpPair second = MakeTcpPair();
    CHECK(second.valid());
    if (second.valid()) {
        CHECK(socket_registry.Register(second.server.get(), "aba-socket", &error));
        const int raw_socket = second.server.Release();
        CHECK(::close(raw_socket) == 0);
        Fd fresh(::socket(AF_INET, SOCK_STREAM, 0));
        CHECK(fresh.valid());
        CHECK(::dup2(fresh.get(), raw_socket) == raw_socket);
        const auto [socket_error, socket_text] =
            CallTool(registry, "socket_close", {{"fd", raw_socket}});
        CHECK(socket_error);
        CHECK(::fcntl(raw_socket, F_GETFD) != -1);
        ::close(raw_socket);
    }
    ::close(raw);
}

/// 审计写入失败：fail-closed，fd 不被关闭。
void TestAuditFailClosed() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    auto audit = SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "fail-closed", &error));
    audit->FailWith("disk full");

    const auto [is_error, text] =
        CallTool(registry, "socket_close", {{"fd", pair.server.get()}});
    CHECK(is_error);
    const json out = ParseJson(text);
    CHECK(out["closed"] == false);
    CHECK(out["error"].get<std::string>().find("fail-closed") !=
          std::string::npos);
    CHECK(::fcntl(pair.server.get(), F_GETFD) != -1);
    CHECK(audit->size() == 0);

    audit->ClearFailure();
    const auto [retry_error, retry_text] =
        CallTool(registry, "socket_close", {{"fd", pair.server.get()}});
    CHECK(!retry_error);
    CHECK(::fcntl(pair.server.get(), F_GETFD) == -1);
    pair.server.Release();
    CHECK(audit->size() == 1);
    CHECK(audit->Records()[0]["result"] == "ok");
}

/// 真实文件审计汇：JSON Lines + 0600。
void TestFileAuditSink() {
    ResetAll();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("go2cpp_socket_audit_" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    const std::filesystem::path path = directory / "audit.log";

    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "file-sink", &error));
    FileSocketAuditSink sink(path.string());
    const auto outcome =
        socket_registry.Close(pair.server.get(), "file sink", sink);
    CHECK(outcome.ok);
    CHECK(outcome.audit_sink == path.string());
    pair.server.Release();

    CHECK(std::filesystem::is_regular_file(path));
    std::ifstream input(path);
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    CHECK(content.find("\"action\":\"socket_close\"") != std::string::npos);
    CHECK(content.find("\"result\":\"ok\"") != std::string::npos);
    CHECK(content.find("\"reason\":\"file sink\"") != std::string::npos);
    CHECK(content.find("\"fd\":") != std::string::npos);
    CHECK(content.back() == '\n');

    struct stat status {};
    CHECK(::stat(path.c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    std::filesystem::remove_all(directory);
}

/// 事件环形容量与查询截断。
void TestEventRingOverflow() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "ring", &error));
    for (int index = 0; index < 300; ++index) {
        socket_registry.RecordRead(pair.server.get(), 1);
    }

    const auto stats = socket_registry.Stats(pair.server.get(), &error);
    CHECK(stats.has_value());
    if (stats.has_value()) {
        CHECK(stats->dropped_events >= 44);
        CHECK(stats->events <= SocketRegistry::kEventCapacity);
    }

    bool registered = false;
    bool found = false;
    const auto events = socket_registry.Events(pair.server.get(), 256, &registered,
                                               &found, &error);
    CHECK(events.size() == SocketRegistry::kEventCapacity);
    const auto clamped = socket_registry.Events(pair.server.get(), 10000,
                                                &registered, &found, &error);
    CHECK(clamped.size() == SocketRegistry::kEventCapacity);

    const auto [is_error, text] = CallTool(
        registry, "socket_events", {{"fd", pair.server.get()}, {"n", 256}});
    CHECK(!is_error);
    const json out = ParseJson(text);
    CHECK(out["count"] == SocketRegistry::kEventCapacity);
}

/// 外部关闭：list 过滤、events 保留 closed、stats 报错。
void TestExternalClose() {
    ResetAll();
    TcpPair pair = MakeTcpPair();
    CHECK(pair.valid());
    if (!pair.valid()) {
        return;
    }
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;
    CHECK(socket_registry.Register(pair.server.get(), "external", &error));
    const int raw = pair.server.Release();
    CHECK(::close(raw) == 0);

    bool listed = false;
    for (const auto& snapshot : socket_registry.List(true, &error)) {
        if (snapshot.fd == raw) {
            listed = true;
        }
    }
    CHECK(!listed);

    bool registered = true;
    bool found = false;
    const auto events =
        socket_registry.Events(raw, 256, &registered, &found, &error);
    CHECK(error.empty());
    CHECK(found);
    CHECK(!registered);
    bool closed = false;
    for (const auto& event : events) {
        if (event.kind == SocketEventKind::kClosed) {
            closed = true;
        }
    }
    CHECK(closed);

    std::string stats_error;
    CHECK(!socket_registry.Stats(raw, &stats_error).has_value());
    CHECK(!stats_error.empty());
}

/// 8 线程并发查询 + 与 close 竞争。
void TestConcurrentQueries() {
    ResetAll();
    Registry registry;
    (void)SetupTools(&registry);
    SocketRegistry& socket_registry = SocketRegistry::Instance();
    std::string error;

    std::vector<TcpPair> pairs;
    pairs.reserve(3);
    for (int index = 0; index < 3; ++index) {
        TcpPair pair = MakeTcpPair();
        CHECK(pair.valid());
        if (!pair.valid()) {
            return;
        }
        CHECK(socket_registry.Register(pair.server.get(), "concurrent", &error));
        pairs.push_back(std::move(pair));
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&registry, &pairs, &failures, worker]() {
            for (int index = 0; index < 100; ++index) {
                const int fd =
                    pairs[static_cast<std::size_t>(worker + index) % pairs.size()]
                        .server.get();
                if (index % 3 == 0) {
                    if (CallTool(registry, "socket_list", json::object()).first) {
                        ++failures;
                    }
                } else if (index % 3 == 1) {
                    if (CallTool(registry, "socket_stats", {{"fd", fd}}).first) {
                        ++failures;
                    }
                } else {
                    if (CallTool(registry, "socket_events",
                                 {{"fd", fd}, {"n", 10}})
                            .first) {
                        ++failures;
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(failures.load() == 0);

    // 与查询竞争关闭：统计可能报错，但进程不能崩溃，close 必须成功。
    std::atomic<bool> stop{false};
    std::thread reader([&registry, &pairs, &stop]() {
        while (!stop.load()) {
            (void)CallTool(registry, "socket_stats",
                           {{"fd", pairs[0].server.get()}});
        }
    });
    const auto [is_error, text] =
        CallTool(registry, "socket_close", {{"fd", pairs[0].server.get()}});
    stop.store(true);
    reader.join();
    CHECK(!is_error);
    pairs[0].server.Release();
}

}  // namespace

int main() {
    TestWhitelistDenied();
    TestNonSocketFd();
    TestFdReuseAba();
    TestAuditFailClosed();
    TestFileAuditSink();
    TestEventRingOverflow();
    TestExternalClose();
    TestConcurrentQueries();
    return go2cpp::mcp::test::Summary("socket_whitelist");
}
