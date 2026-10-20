/**
 * @file test_debug_dump.cpp
 * @brief 统一调试内省接口的测试：scheduler/fiber/channel/parking/sync/io
 *        + 事件环 + DebugServer 协议（text/JSON/socket）。
 *
 * 覆盖（按项目规范：真实场景 + 压测 + 随机 + 并发）：
 *   场景 1 scheduler：M/P/G、每 P runqueue、全局队列、metrics
 *   场景 2 fiber：状态、父 Fiber、栈范围/committed
 *   场景 3 channel：缓冲长度/容量、接收等待者、select 注册数
 *   场景 4 ParkingCondition：等待者数量与节点状态
 *   场景 5 sync::Mutex：owner（近似）与 kLocked/kWaiter/kStarving/kWoken
 *   场景 6 io::IOManager：FdSlot / epoll 注册 / 调度器计数
 *   场景 7 DebugServer：PING/LIST/STATUS/DUMP/JSON/错误码
 *   场景 8 事件环：OFF 构建 enabled=false；ON 构建有事件且可导出文件
 *   场景 9 socket 端到端（仅 ON 构建）
 *   压测   500 次 scheduler dump + 100 次 JSON + 4 线程并发 dump
 *   随机   500 次随机容量 channel 快照
 */

#include "go2cpp/debug.hpp"
#include "go2cpp/json.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using go2cpp::Channel;
using go2cpp::Fiber;
using go2cpp::Scheduler;
using go2cpp::SchedulerConfig;
using go2cpp::SuspendReason;
using go2cpp::Task;
using go2cpp::core::ParkingCondition;
using go2cpp::debug::DumpPayload;
using go2cpp::debug::DebugServer;
using go2cpp::debug::DumpChannel;
using go2cpp::debug::DumpEvents;
using go2cpp::debug::DumpFiber;
using go2cpp::debug::DumpIOManager;
using go2cpp::debug::DumpMutex;
using go2cpp::debug::DumpParkingCondition;
using go2cpp::debug::DumpScheduler;
using go2cpp::debug::EventRing;

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

std::uint64_t ParseHex(const std::string& text, const std::string& key) {
    const std::string marker = key + "=0x";
    const std::size_t position = text.find(marker);
    if (position == std::string::npos) {
        return 0;
    }
    const std::size_t begin = position + marker.size();
    std::size_t end = begin;
    while (end < text.size() &&
           ((text[end] >= '0' && text[end] <= '9') ||
            (text[end] >= 'a' && text[end] <= 'f'))) {
        ++end;
    }
    return std::stoull(text.substr(begin, end - begin), nullptr, 16);
}

struct Response {
    bool valid{false};
    bool ok{false};
    std::size_t bytes{0};
    std::string payload;
    std::string error;
};

Response ParseResponse(const std::string& raw) {
    Response response;
    const std::size_t newline = raw.find('\n');
    if (newline == std::string::npos) {
        return response;
    }
    const std::string header = raw.substr(0, newline);
    const std::string prefix = "GO2CPP-DEBUG/1 ";
    if (!StartsWith(header, prefix)) {
        return response;
    }
    const std::size_t ok_marker = header.find(" OK ");
    if (ok_marker != std::string::npos) {
        response.ok = true;
        response.bytes = static_cast<std::size_t>(
            std::stoull(header.substr(ok_marker + 4)));
        response.payload = raw.substr(newline + 1);
        response.valid = response.payload.size() == response.bytes;
        return response;
    }
    const std::size_t err_marker = header.find(" ERR ");
    if (err_marker != std::string::npos) {
        response.ok = false;
        response.error = header.substr(err_marker + 5);
        response.valid = true;
        return response;
    }
    return response;
}

bool WaitFor(const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

#if defined(__linux__)
std::string SendSocketRaw(const std::string& path, const std::string& command) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return {};
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }
    const std::string line = command + "\n";
    (void)::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
    std::string raw;
    char buffer[1024];
    for (;;) {
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(fd);
    return raw;
}

Response SendSocketCommand(const std::string& path, const std::string& command) {
    return ParseResponse(SendSocketRaw(path, command));
}

std::string TrimTrailingNewlines(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

/// 建立一条 Unix socket 连接；失败返回 -1。
int ConnectUnix(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// 读一个完整的 `GO2CPP-DEBUG/1 <kind> <bytes>\n<payload>` 帧。
bool ReadFrame(int fd, std::string* kind, std::string* payload) {
    std::string header;
    char ch = 0;
    for (;;) {
        const ssize_t received = ::recv(fd, &ch, 1, 0);
        if (received <= 0) {
            return false;
        }
        if (ch == '\n') {
            break;
        }
        header.push_back(ch);
        if (header.size() > 512U) {
            return false;
        }
    }
    const std::string prefix = "GO2CPP-DEBUG/1 ";
    if (header.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::string rest = header.substr(prefix.size());
    const std::size_t space = rest.find(' ');
    if (space == std::string::npos) {
        return false;
    }
    *kind = rest.substr(0, space);
    const std::string tail = rest.substr(space + 1);
    if (*kind == "ERR") {
        *payload = tail;
        return true;
    }
    std::size_t bytes = 0;
    try {
        bytes = static_cast<std::size_t>(std::stoull(tail));
    } catch (...) {
        return false;
    }
    payload->assign(bytes, '\0');
    std::size_t offset = 0;
    while (offset < bytes) {
        const ssize_t received =
            ::recv(fd, &(*payload)[offset], bytes - offset, 0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

/// 把 text dump 里的事件行（7 列）拆成 token；非事件行返回空。
std::vector<std::string> EventLineTokens(const std::string& line) {
    std::vector<std::string> tokens;
    if (line.empty() || line.front() == '#' ||
        line.find('=') != std::string::npos) {
        return tokens;
    }
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && line[index] == ' ') {
            ++index;
        }
        const std::size_t begin = index;
        while (index < line.size() && line[index] != ' ') {
            ++index;
        }
        if (index > begin) {
            tokens.push_back(line.substr(begin, index - begin));
        }
    }
    return tokens;
}
#endif

bool JsonParses(const std::string& text, const char* expected_schema) {
    try {
        const auto value = go2cpp::Json::parse(text);
        return value.contains("schema") &&
               value["schema"].get<std::string>() == expected_schema;
    } catch (...) {
        return false;
    }
}

}  // namespace

void run_debug_dump_tests() {
    go2cpp_tests::announce("debug dump: unified read-only interfaces");
    using namespace go2cpp;
    using namespace go2cpp::debug;

    // -------------------------------------------------------------------
    // 场景 1：scheduler
    // -------------------------------------------------------------------
    {
        SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.min_workers = 2;
        Scheduler scheduler(config);
        scheduler.start();
        auto task = scheduler.spawn([] {});
        GO2CPP_REQUIRE(task != nullptr);
        GO2CPP_CHECK(task->wait());

        const DumpPayload payload = DumpScheduler(scheduler, {});
        GO2CPP_CHECK(Contains(payload.text, "schema=go2cpp.dump.scheduler/1"));
        GO2CPP_CHECK(Contains(payload.text, "processor_count=2"));
        GO2CPP_CHECK(Contains(payload.text, "processor.0.id=0"));
        GO2CPP_CHECK(Contains(payload.text, "processor.0.queued="));
        GO2CPP_CHECK(Contains(payload.text, "runqueue.local_total="));
        GO2CPP_CHECK(Contains(payload.text, "runqueue.global="));
        GO2CPP_CHECK(Contains(payload.text, "machine.0.id="));
        GO2CPP_CHECK(Contains(payload.text, "metrics.task_runs="));
        GO2CPP_CHECK(Contains(payload.text, "consistent=per_call"));
        GO2CPP_CHECK(JsonParses(payload.json, "go2cpp.dump.scheduler/1"));
        GO2CPP_CHECK(Contains(payload.json, "\"processors\":["));
        scheduler.shutdown();
    }

    // -------------------------------------------------------------------
    // 场景 2：fiber（状态 + 父 Fiber + 栈范围/committed）
    // -------------------------------------------------------------------
    {
        Fiber fiber([] { (void)Fiber::Suspend(SuspendReason::Yield); });
        GO2CPP_CHECK(fiber.resume());
        GO2CPP_CHECK(fiber.state() == FiberState::Suspended);
        const DumpPayload payload = DumpFiber(fiber);
        GO2CPP_CHECK(Contains(payload.text, "schema=go2cpp.dump.fiber/1"));
        GO2CPP_CHECK(Contains(payload.text, "state=kSuspended"));
        GO2CPP_CHECK(Contains(payload.text, "has_stack=true"));
        GO2CPP_CHECK(Contains(payload.text, "nesting_depth=1"));
        GO2CPP_CHECK(Contains(payload.text, "requested_bytes="));
        GO2CPP_CHECK(Contains(payload.text, "usable_bytes="));
        const std::uint64_t base = ParseHex(payload.text, "mapping_base");
        const std::uint64_t top = ParseHex(payload.text, "mapping_top");
        const std::uint64_t committed =
            ParseHex(payload.text, "committed_begin");
        GO2CPP_CHECK(base != 0 && top > base);
        GO2CPP_CHECK(committed >= base && committed <= top);
        GO2CPP_CHECK(JsonParses(payload.json, "go2cpp.dump.fiber/1"));
        GO2CPP_CHECK(fiber.resume());
        GO2CPP_CHECK(fiber.state() == FiberState::Completed);
        // 终态后栈已释放：如实报告 has_stack=false，而不是伪造地址。
        const DumpPayload completed = DumpFiber(fiber);
        GO2CPP_CHECK(Contains(completed.text, "has_stack=false"));
        GO2CPP_CHECK(Contains(completed.text, "state=kCompleted"));
    }

    // -------------------------------------------------------------------
    // 场景 3：channel（缓冲 + 接收等待者 + select 注册数）
    // -------------------------------------------------------------------
    {
        auto channel = Channel<int>::Create(2);
        (void)channel->Send(7);
        const DumpPayload buffered = DumpChannel(channel->DebugSnapshot());
        GO2CPP_CHECK(Contains(buffered.text, "schema=go2cpp.dump.channel/1"));
        GO2CPP_CHECK(Contains(buffered.text, "capacity=2"));
        GO2CPP_CHECK(Contains(buffered.text, "length=1"));
        GO2CPP_CHECK(Contains(buffered.text, "closed=false"));
        GO2CPP_CHECK(Contains(buffered.text, "select_senders="));
        GO2CPP_CHECK(Contains(buffered.text, "select_receivers="));
        GO2CPP_CHECK(JsonParses(buffered.json, "go2cpp.dump.channel/1"));

        auto rendezvous = Channel<int>::Create(0);
        std::thread waiter([rendezvous] {
            auto result = rendezvous->Recv();
            (void)result;
        });
        GO2CPP_CHECK(WaitFor(
            [&] { return rendezvous->DebugSnapshot().recv_waiters == 1; },
            std::chrono::milliseconds(2000)));
        const auto snapshot = rendezvous->DebugSnapshot();
        GO2CPP_CHECK(snapshot.recv_waiters == 1);
        GO2CPP_CHECK(snapshot.send_waiters == 0);
        rendezvous->Close();
        waiter.join();
    }

    // -------------------------------------------------------------------
    // 场景 4：ParkingCondition（等待者数量 + kWaiting 节点）
    // -------------------------------------------------------------------
    {
        ParkingCondition condition;
        std::mutex mutex;
        std::thread waiter([&] {
            std::unique_lock<std::mutex> lock(mutex);
            (void)condition.wait_for(lock, std::chrono::seconds(2),
                                     [] { return false; });
        });
        GO2CPP_CHECK(WaitFor(
            [&] { return condition.DebugSnapshot().waiters == 1; },
            std::chrono::milliseconds(2000)));
        const auto snapshot = condition.DebugSnapshot();
        GO2CPP_CHECK(snapshot.waiters == 1);
        GO2CPP_CHECK(snapshot.native_waiters == 1);
        GO2CPP_CHECK(snapshot.fiber_waiters == 0);
        GO2CPP_CHECK(snapshot.waiting_nodes == 1);
        GO2CPP_REQUIRE(snapshot.nodes.size() == 1);
        GO2CPP_CHECK(snapshot.nodes[0].native);
        GO2CPP_CHECK(snapshot.nodes[0].state == 0);
        const DumpPayload payload = DumpParkingCondition(condition);
        GO2CPP_CHECK(Contains(payload.text, "node.0.state=kWaiting"));
        GO2CPP_CHECK(
            JsonParses(payload.json, "go2cpp.dump.parking_condition/1"));
        condition.notify_all();
        waiter.join();
    }

    // -------------------------------------------------------------------
    // 场景 5：sync::Mutex（owner + 四位状态字）
    // -------------------------------------------------------------------
    {
        sync::Mutex mutex;
        GO2CPP_REQUIRE(mutex.Lock());
        {
            const DumpPayload payload = DumpMutex(mutex);
            GO2CPP_CHECK(Contains(payload.text, "schema=go2cpp.dump.mutex/1"));
            GO2CPP_CHECK(Contains(payload.text, "held=true"));
            GO2CPP_CHECK(Contains(payload.text, "locked=true"));
            GO2CPP_CHECK(Contains(payload.text, "state_bits=0x"));
            GO2CPP_CHECK(Contains(payload.text, "starving=false"));
            GO2CPP_CHECK(Contains(payload.text, "woken=false"));
            GO2CPP_CHECK(Contains(payload.text, "owner_known="));
            GO2CPP_CHECK(JsonParses(payload.json, "go2cpp.dump.mutex/1"));
        }
        std::thread waiter([&] {
            (void)mutex.Lock();
            mutex.Unlock();
        });
        GO2CPP_CHECK(WaitFor(
            [&] { return mutex.DebugSnapshotInternal().waiters >= 1; },
            std::chrono::milliseconds(2000)));
        const auto snapshot = mutex.DebugSnapshotInternal();
        GO2CPP_CHECK(snapshot.locked);
        GO2CPP_CHECK(snapshot.waiters >= 1);
        GO2CPP_CHECK(snapshot.native_waiters >= 1);
        mutex.Unlock();
        waiter.join();
    }

    // -------------------------------------------------------------------
    // 场景 6：io::IOManager（FdSlot / epoll / 调度器计数）
    // -------------------------------------------------------------------
    {
        io::IOManager manager;
        GO2CPP_REQUIRE(manager.start());
        const DumpPayload payload = DumpIOManager(manager);
        GO2CPP_CHECK(Contains(payload.text, "schema=go2cpp.dump.io/1"));
        GO2CPP_CHECK(Contains(payload.text, "running=true"));
        GO2CPP_CHECK(Contains(payload.text, "fd_slots="));
        GO2CPP_CHECK(Contains(payload.text, "epoll_registered="));
        GO2CPP_CHECK(Contains(payload.text, "reader_waiters="));
        GO2CPP_CHECK(Contains(payload.text, "writer_waiters="));
        GO2CPP_CHECK(Contains(payload.text, "timers="));
        GO2CPP_CHECK(Contains(payload.text, "event_ring_enabled="));
        GO2CPP_CHECK(JsonParses(payload.json, "go2cpp.dump.io/1"));
        manager.shutdown();
    }

    // -------------------------------------------------------------------
    // 场景 7：DebugServer 进程内协议（不依赖 socket）
    // -------------------------------------------------------------------
    {
        SchedulerConfig config;
        config.processor_count = 1;
        config.max_workers = 1;
        config.min_workers = 1;
        Scheduler scheduler(config);
        scheduler.start();
        DebugServer& server = DebugServer::Instance();
        GO2CPP_CHECK(RegisterScheduler("scheduler", scheduler));
        GO2CPP_CHECK(RegisterEventRing("events"));

        const Response ping = ParseResponse(server.HandleCommand("PING"));
        GO2CPP_CHECK(ping.valid && ping.ok);
        GO2CPP_CHECK(ping.payload == "pong");

        const Response list = ParseResponse(server.HandleCommand("LIST"));
        GO2CPP_CHECK(list.valid && list.ok);
        GO2CPP_CHECK(Contains(list.payload, "scheduler\tavailable"));
        GO2CPP_CHECK(Contains(list.payload, "fiber\tunavailable"));

        const Response status = ParseResponse(server.HandleCommand("STATUS"));
        GO2CPP_CHECK(status.valid && status.ok);
        GO2CPP_CHECK(Contains(status.payload, "protocol=GO2CPP-DEBUG/1"));
        GO2CPP_CHECK(Contains(status.payload, "debug_dump="));

        const Response dump = ParseResponse(server.HandleCommand("DUMP scheduler"));
        GO2CPP_CHECK(dump.valid && dump.ok);
        GO2CPP_CHECK(Contains(dump.payload, "schema=go2cpp.dump.scheduler/1"));

        const Response json = ParseResponse(server.HandleCommand("JSON scheduler"));
        GO2CPP_CHECK(json.valid && json.ok);
        GO2CPP_CHECK(JsonParses(json.payload, "go2cpp.dump.scheduler/1"));

        const Response all = ParseResponse(server.HandleCommand("DUMP ALL"));
        GO2CPP_CHECK(all.valid && all.ok);
        GO2CPP_CHECK(Contains(all.payload, "schema=go2cpp.dump.all/1"));
        GO2CPP_CHECK(Contains(all.payload, "[component scheduler]"));
        GO2CPP_CHECK(Contains(all.payload, "[component events]"));

        const Response missing =
            ParseResponse(server.HandleCommand("DUMP does_not_exist"));
        GO2CPP_CHECK(missing.valid && !missing.ok);
        GO2CPP_CHECK(Contains(missing.error, "unknown-component"));

        const Response bad = ParseResponse(server.HandleCommand("BOGUS"));
        GO2CPP_CHECK(bad.valid && !bad.ok);
        GO2CPP_CHECK(Contains(bad.error, "bad-command"));

        GO2CPP_CHECK(server.Unregister("scheduler"));
        GO2CPP_CHECK(server.Unregister("events"));
        scheduler.shutdown();
    }

    // -------------------------------------------------------------------
    // 场景 7b：JSON 协议兼容层（CLI 占位协议 {"id","cmd"}）
    // -------------------------------------------------------------------
    {
        SchedulerConfig config;
        config.processor_count = 1;
        config.max_workers = 1;
        config.min_workers = 1;
        Scheduler scheduler(config);
        scheduler.start();
        DebugServer& server = DebugServer::Instance();
        GO2CPP_CHECK(RegisterScheduler("scheduler", scheduler));

        const std::string status = TrimTrailingNewlines(
            server.HandleJsonCommand(
                "{\"id\":7,\"cmd\":\"status\"}"));
        const auto status_parsed = go2cpp::ParseJson(status);
        GO2CPP_CHECK(status_parsed.ok());
        if (status_parsed.ok()) {
            GO2CPP_CHECK(status_parsed.value["id"].get<int>() == 7);
            GO2CPP_CHECK(status_parsed.value["ok"].get<bool>());
            GO2CPP_CHECK(status_parsed.value["data"].contains("goroutines"));
            GO2CPP_CHECK(status_parsed.value["data"].contains("runqueue"));
            GO2CPP_CHECK(
                status_parsed.value["data"]["goroutines"].contains("total"));
        }

        const std::string dump = TrimTrailingNewlines(
            server.HandleJsonCommand(
                "{\"id\":8,\"cmd\":\"dump\",\"format\":\"json\","
                "\"sections\":[\"scheduler\"]}"));
        const auto dump_parsed = go2cpp::ParseJson(dump);
        GO2CPP_CHECK(dump_parsed.ok());
        if (dump_parsed.ok()) {
            GO2CPP_CHECK(
                dump_parsed.value["data"]["schema"].get<std::string>() ==
                "go2cpp.dump.scheduler/1");
        }

        const std::string migrate = TrimTrailingNewlines(
            server.HandleJsonCommand(
                "{\"id\":9,\"cmd\":\"migrate.dump\",\"gid\":42}"));
        const auto migrate_parsed = go2cpp::ParseJson(migrate);
        GO2CPP_CHECK(migrate_parsed.ok());
        if (migrate_parsed.ok()) {
            GO2CPP_CHECK(!migrate_parsed.value["ok"].get<bool>());
            GO2CPP_CHECK(
                migrate_parsed.value["error"]["code"].get<std::string>() ==
                "not_supported");
        }

        GO2CPP_CHECK(server.Unregister("scheduler"));
        scheduler.shutdown();
    }

    // -------------------------------------------------------------------
    // 场景 8：事件环（OFF 构建 enabled=false；ON 构建有事件 + 文件导出）
    // -------------------------------------------------------------------
    {
        const DumpPayload payload = DumpEvents(0);
        GO2CPP_CHECK(Contains(payload.text, "schema=go2cpp.dump.events/1"));
        GO2CPP_CHECK(Contains(payload.text, "capacity=256"));
        GO2CPP_CHECK(JsonParses(payload.json, "go2cpp.dump.events/1"));
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_CHECK(EventRing::Enabled());
        GO2CPP_CHECK(Contains(payload.text, "enabled=true"));
        // 前面的 scheduler 场景已经产生 park/wake/runqueue 事件。
        const auto events = EventRing::Snapshot(0);
        GO2CPP_CHECK(!events.empty());
        GO2CPP_CHECK(Contains(payload.text, "event_count="));
#else
        GO2CPP_CHECK(!EventRing::Enabled());
        GO2CPP_CHECK(Contains(payload.text, "enabled=false"));
        GO2CPP_CHECK(EventRing::Snapshot(0).empty());
#endif
        const std::string path = "/tmp/go2cpp_debug_dump_test_events.txt";
        std::string error;
        GO2CPP_CHECK(EventRing::WriteToFile(path, false, &error, 0));
        std::FILE* file = std::fopen(path.c_str(), "rb");
        GO2CPP_CHECK(file != nullptr);
        if (file != nullptr) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fclose(file);
            GO2CPP_CHECK(size > 0);
        }
        (void)std::remove(path.c_str());
    }

    // -------------------------------------------------------------------
    // 场景 9：socket 端到端（仅 GO2CPP_DEBUG_DUMP=ON）
    // -------------------------------------------------------------------
    {
        DebugServer& server = DebugServer::Instance();
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
#if defined(__linux__)
        const std::string path =
            "/tmp/go2cpp-debug-test-" + std::to_string(::getpid()) + ".sock";
        GO2CPP_CHECK(server.Start(path));
        GO2CPP_CHECK(server.running());
        const Response ping = SendSocketCommand(path, "PING");
        GO2CPP_CHECK(ping.valid && ping.ok);
        GO2CPP_CHECK(ping.payload == "pong");
        const Response dump = SendSocketCommand(path, "JSON runtime");
        GO2CPP_CHECK(dump.valid && dump.ok);
        GO2CPP_CHECK(JsonParses(dump.payload, "go2cpp.dump.runtime/1"));
        const Response events = SendSocketCommand(path, "EVENTS 32");
        GO2CPP_CHECK(events.valid && events.ok);
        GO2CPP_CHECK(Contains(events.payload, "schema=go2cpp.dump.events/1"));
        // JSON 协议（CLI 占位协议）端到端
        const std::string raw_json = TrimTrailingNewlines(SendSocketRaw(
            path, "{\"id\":5,\"cmd\":\"status\"}"));
        const auto json_parsed = go2cpp::ParseJson(raw_json);
        GO2CPP_CHECK(json_parsed.ok());
        if (json_parsed.ok()) {
            GO2CPP_CHECK(json_parsed.value["id"].get<int>() == 5);
            GO2CPP_CHECK(json_parsed.value["ok"].get<bool>());
            GO2CPP_CHECK(json_parsed.value["data"].contains("pid"));
        }
        server.Stop();
        GO2CPP_CHECK(!server.running());
#endif
#else
        GO2CPP_CHECK(!server.Start());
        GO2CPP_CHECK(server.last_error() == "debug-dump-disabled");
        GO2CPP_CHECK(!server.running());
#endif
    }

    // -------------------------------------------------------------------
    // 场景 10：事件带 pid（P 归属）；dump 文本/JSON 同步导出
    // -------------------------------------------------------------------
    {
        SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.min_workers = 2;
        Scheduler scheduler(config);
        scheduler.start();
        std::vector<std::shared_ptr<Task>> tasks;
        for (int i = 0; i < 16; ++i) {
            tasks.push_back(scheduler.spawn([] {}));
        }
        for (auto& task : tasks) {
            GO2CPP_CHECK(task->wait());
        }
        // 在 worker 仍存活时采集：线程退出后它的 ring 会从全局表摘除。
        const auto events = EventRing::Snapshot(0);
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        std::size_t bound_pid_events = 0;
        std::size_t unbound_pid_events = 0;
        bool pid_out_of_range = false;
        for (const auto& event : events) {
            if (event.pid == kInvalidProcessorId) {
                ++unbound_pid_events;
            } else {
                ++bound_pid_events;
                if (event.pid >= 2U) {
                    pid_out_of_range = true;
                }
            }
        }
        GO2CPP_CHECK(bound_pid_events > 0);
        GO2CPP_CHECK(!pid_out_of_range);
        const DumpPayload payload = DumpEvents(0);
        GO2CPP_CHECK(Contains(payload.text, "critical_capacity="));
        GO2CPP_CHECK(Contains(payload.text, "total_capacity="));
        GO2CPP_CHECK(Contains(payload.text, "seq_last="));
        GO2CPP_CHECK(
            Contains(payload.text, "# seq ts_ns tid code gid arg2 pid"));
        GO2CPP_CHECK(Contains(payload.json, "\"pid\":"));
        GO2CPP_CHECK(Contains(payload.json, "\"critical_capacity\":"));
        // 每行事件恰好 7 列（seq ts_ns tid code gid arg2 pid）。
        std::size_t event_lines = 0;
        std::size_t bad_lines = 0;
        std::size_t position = 0;
        while (position < payload.text.size()) {
            std::size_t end = payload.text.find('\n', position);
            if (end == std::string::npos) {
                end = payload.text.size();
            }
            const std::string line =
                payload.text.substr(position, end - position);
            position = end + 1;
            if (line.empty() || line.front() == '#' ||
                line.find('=') != std::string::npos) {
                continue;
            }
            const auto tokens = EventLineTokens(line);
            ++event_lines;
            if (tokens.size() != 7U) {
                ++bad_lines;
            }
        }
        GO2CPP_CHECK(event_lines > 0);
        GO2CPP_CHECK(bad_lines == 0);
        GO2CPP_CHECK(unbound_pid_events + bound_pid_events ==
                     events.size());
#else
        GO2CPP_CHECK(events.empty());
        const DumpPayload payload = DumpEvents(0);
        GO2CPP_CHECK(Contains(payload.text, "enabled=false"));
        GO2CPP_CHECK(Contains(payload.text, "# seq ts_ns tid code gid arg2 pid"));
#endif
        scheduler.shutdown();
    }

    // -------------------------------------------------------------------
    // 场景 11：关键事件不被普通事件覆盖（两段环形）
    // -------------------------------------------------------------------
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
    {
        EventRing::Reset();
        GO2CPP_CHECK(EventRing::Snapshot(0).empty());
        const std::size_t pressure = EventRing::kCapacity * 4U + 32U;
        for (std::size_t i = 0; i < pressure; ++i) {
            EventRing::Record(SchedEventCode::kParkEnter, i + 1, 0);
        }
        // 普通区已回绕多次；关键事件仍应写入关键区并保留。
        EventRing::Record(SchedEventCode::kWakeRejected, 777, 2);
        EventRing::Record(SchedEventCode::kRunQueueClaimFail, 778, 0);
        const auto first = EventRing::Snapshot(0);
        bool found_wake = false;
        bool found_claim = false;
        std::size_t ordinary_count = 0;
        for (const auto& event : first) {
            if (event.code ==
                    static_cast<std::uint32_t>(SchedEventCode::kWakeRejected) &&
                event.gid == 777) {
                found_wake = true;
            }
            if (event.code == static_cast<std::uint32_t>(
                                  SchedEventCode::kRunQueueClaimFail) &&
                event.gid == 778) {
                found_claim = true;
            }
            if (event.code ==
                static_cast<std::uint32_t>(SchedEventCode::kParkEnter)) {
                ++ordinary_count;
            }
        }
        GO2CPP_CHECK(found_wake);
        GO2CPP_CHECK(found_claim);
        // 普通区仍然只有 kCapacity 条（回绕生效），关键区不占普通区容量。
        GO2CPP_CHECK(ordinary_count <= EventRing::kCapacity);

        // 再用普通事件冲一遍：关键事件必须仍然存在。
        for (std::size_t i = 0; i < pressure; ++i) {
            EventRing::Record(SchedEventCode::kRunQueuePush, i + 1, 0);
        }
        const auto second = EventRing::Snapshot(0);
        bool still_there = false;
        for (const auto& event : second) {
            if (event.gid == 777 &&
                event.code == static_cast<std::uint32_t>(
                                   SchedEventCode::kWakeRejected)) {
                still_there = true;
            }
        }
        GO2CPP_CHECK(still_there);

        // 关键区自身写满后按「覆盖最旧」处理，且条数不超过关键区容量。
        EventRing::Reset();
        const std::size_t critical_pressure =
            EventRing::kCriticalCapacity + 10U;
        for (std::size_t i = 0; i < critical_pressure; ++i) {
            EventRing::Record(SchedEventCode::kWakeRejected, i + 1, 2);
        }
        std::size_t kept = 0;
        for (const auto& event : EventRing::Snapshot(0)) {
            if (event.code == static_cast<std::uint32_t>(
                                  SchedEventCode::kWakeRejected)) {
                ++kept;
            }
        }
        GO2CPP_CHECK(kept == EventRing::kCriticalCapacity);

        // dump 头部如实报告两段容量与关键事件条数。
        const DumpPayload payload = DumpEvents(0);
        GO2CPP_CHECK(Contains(payload.text, "critical_policy=isolated"));
        GO2CPP_CHECK(Contains(payload.text, "critical_event_count="));
        GO2CPP_CHECK(Contains(payload.text, "normal_event_count="));
    }

    // -------------------------------------------------------------------
    // 场景 12：ReadSince 增量游标（订阅的底层语义）
    // -------------------------------------------------------------------
    {
        EventRing::Reset();
        const std::uint64_t cursor = EventRing::PublishSeq();
        GO2CPP_CHECK(cursor == 0);
        for (int i = 0; i < 10; ++i) {
            EventRing::Record(SchedEventCode::kParkEnter,
                              static_cast<std::uint64_t>(i + 1), 0);
        }
        const auto first_batch = EventRing::ReadSince(cursor, 4);
        GO2CPP_REQUIRE(first_batch.size() == 4U);
        GO2CPP_CHECK(first_batch.front().gid == 1);
        GO2CPP_CHECK(first_batch.back().gid == 4);
        for (std::size_t i = 1; i < first_batch.size(); ++i) {
            GO2CPP_CHECK(first_batch[i - 1].seq < first_batch[i].seq);
        }
        const auto rest = EventRing::ReadSince(first_batch.back().seq, 0);
        GO2CPP_REQUIRE(rest.size() == 6U);
        GO2CPP_CHECK(rest.front().gid == 5);
        GO2CPP_CHECK(rest.back().gid == 10);
        GO2CPP_CHECK(EventRing::ReadSince(rest.back().seq, 0).empty());
    }
#endif

    // -------------------------------------------------------------------
    // 场景 13：dump 一致性标记（seq_at_start/seq_at_end）
    // -------------------------------------------------------------------
    {
        SchedulerConfig config;
        config.processor_count = 1;
        config.max_workers = 1;
        config.min_workers = 1;
        Scheduler scheduler(config);
        scheduler.start();
        const DumpPayload scheduler_dump = DumpScheduler(scheduler, {});
        GO2CPP_CHECK(Contains(scheduler_dump.text, "seq_at_start="));
        GO2CPP_CHECK(Contains(scheduler_dump.text, "seq_at_end="));
        GO2CPP_CHECK(Contains(scheduler_dump.text, "changed_during_dump="));
        GO2CPP_CHECK(Contains(scheduler_dump.text, "event_seq_supported="));
        GO2CPP_CHECK(Contains(scheduler_dump.json, "\"seq_at_start\":"));
        GO2CPP_CHECK(Contains(scheduler_dump.json, "\"seq_at_end\":"));
        GO2CPP_CHECK(Contains(scheduler_dump.json, "\"changed_during_dump\":"));
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP
        GO2CPP_CHECK(Contains(scheduler_dump.text, "event_seq_supported=true"));
#else
        GO2CPP_CHECK(Contains(scheduler_dump.text, "event_seq_supported=false"));
#endif
        DebugServer& server = DebugServer::Instance();
        GO2CPP_CHECK(RegisterScheduler("scheduler", scheduler));
        const Response all = ParseResponse(server.HandleCommand("DUMP ALL"));
        GO2CPP_CHECK(all.valid && all.ok);
        GO2CPP_CHECK(Contains(all.payload, "schema=go2cpp.dump.all/1"));
        GO2CPP_CHECK(Contains(all.payload, "seq_at_start="));
        GO2CPP_CHECK(Contains(all.payload, "seq_at_end="));
        GO2CPP_CHECK(Contains(all.payload, "changed_during_dump="));
        const Response json_all = ParseResponse(server.HandleCommand("JSON ALL"));
        GO2CPP_CHECK(json_all.valid && json_all.ok);
        GO2CPP_CHECK(Contains(json_all.payload, "\"seq_at_start\":"));
        GO2CPP_CHECK(server.Unregister("scheduler"));
        scheduler.shutdown();
    }

    // -------------------------------------------------------------------
    // 场景 14：SUBSCRIBE 增量推送（仅 ON + Linux）
    // -------------------------------------------------------------------
#if defined(GO2CPP_DEBUG_DUMP) && GO2CPP_DEBUG_DUMP && defined(__linux__)
    {
        DebugServer& server = DebugServer::Instance();
        const std::string path =
            "/tmp/go2cpp-debug-sub-" + std::to_string(::getpid()) + ".sock";
        GO2CPP_REQUIRE(server.Start(path));
        const int fd = ConnectUnix(path);
        GO2CPP_REQUIRE(fd >= 0);
        timeval timeout{};
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout));
        const std::string command = "SUBSCRIBE events 5 64\n";
        GO2CPP_CHECK(::send(fd, command.data(), command.size(),
                            MSG_NOSIGNAL) ==
                     static_cast<ssize_t>(command.size()));
        std::string kind;
        std::string payload;
        GO2CPP_REQUIRE(ReadFrame(fd, &kind, &payload));
        GO2CPP_CHECK(kind == "OK");
        GO2CPP_CHECK(
            Contains(payload, "schema=go2cpp.dump.events.subscription/1"));
        GO2CPP_CHECK(Contains(payload, "protocol=GO2CPP-DEBUG-PUSH/1"));
        GO2CPP_CHECK(Contains(payload, "cursor_start="));

        // 订阅活跃时，普通命令连接不被阻塞（订阅在独立线程里跑）。
        const Response status = SendSocketCommand(path, "STATUS");
        GO2CPP_CHECK(status.valid && status.ok);
        GO2CPP_CHECK(Contains(status.payload, "subscribers=1"));

        // 产生事件：订阅游标之后的新事件应被推送。
        SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.min_workers = 2;
        Scheduler scheduler(config);
        scheduler.start();
        std::vector<std::shared_ptr<Task>> tasks;
        for (int i = 0; i < 24; ++i) {
            tasks.push_back(scheduler.spawn([] {}));
        }
        for (auto& task : tasks) {
            GO2CPP_CHECK(task->wait());
        }
        std::string push_payload;
        bool got_push = false;
        for (int attempt = 0; attempt < 20 && !got_push; ++attempt) {
            GO2CPP_REQUIRE(ReadFrame(fd, &kind, &push_payload));
            if (kind == "PUSH" &&
                Contains(push_payload, "schema=go2cpp.dump.events.push/1")) {
                got_push = true;
            }
        }
        GO2CPP_CHECK(got_push);
        if (got_push) {
            GO2CPP_CHECK(Contains(push_payload, "first_seq="));
            GO2CPP_CHECK(Contains(push_payload, "last_seq="));
            GO2CPP_CHECK(Contains(push_payload, "dropped_total="));
            GO2CPP_CHECK(Contains(push_payload, "dropped_backpressure="));
            GO2CPP_CHECK(
                Contains(push_payload, "# seq ts_ns tid code gid arg2 pid"));
        }
        scheduler.shutdown();
        ::close(fd);
        server.Stop();
        GO2CPP_CHECK(!server.running());
    }
#endif

    // -------------------------------------------------------------------
    // 压测：scheduler dump 500 次 + JSON 100 次
    // -------------------------------------------------------------------
    {
        SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.min_workers = 2;
        Scheduler scheduler(config);
        scheduler.start();
        std::atomic<bool> stop{false};
        std::thread worker([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto task = scheduler.spawn([] {
                    std::this_thread::yield();
                });
                if (task) {
                    (void)task->wait();
                }
            }
        });
        std::size_t sink = 0;
        for (int i = 0; i < 500; ++i) {
            sink += DumpScheduler(scheduler, {}).text.size();
        }
        for (int i = 0; i < 100; ++i) {
            sink += DumpScheduler(scheduler, {}).json.size();
        }
        stop.store(true, std::memory_order_relaxed);
        worker.join();
        GO2CPP_CHECK(sink > 0);
        scheduler.shutdown();
    }

    // -------------------------------------------------------------------
    // 并发：4 线程同时 dump scheduler（不得崩溃/死锁）
    // -------------------------------------------------------------------
    {
        SchedulerConfig config;
        config.processor_count = 2;
        config.max_workers = 2;
        config.min_workers = 2;
        Scheduler scheduler(config);
        scheduler.start();
        std::atomic<int> done{0};
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back([&] {
                for (int round = 0; round < 200; ++round) {
                    const auto payload = DumpScheduler(scheduler, {});
                    if (!Contains(payload.text, "schema=")) {
                        go2cpp_tests::g_failures.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& reader : readers) {
            reader.join();
        }
        GO2CPP_CHECK(done.load() == 4);
        scheduler.shutdown();
    }

    // -------------------------------------------------------------------
    // 随机：500 次随机容量 channel 快照
    // -------------------------------------------------------------------
    {
        std::mt19937_64 random(20261020);
        std::uniform_int_distribution<int> capacity_dist(0, 8);
        std::uniform_int_distribution<int> count_dist(0, 8);
        for (int round = 0; round < 500; ++round) {
            const std::size_t capacity =
                static_cast<std::size_t>(capacity_dist(random));
            const int send_count = count_dist(random);
            auto channel = Channel<int>::Create(capacity);
            int sent = 0;
            for (int i = 0; i < send_count; ++i) {
                const auto result = channel->TrySend(i);
                if (result.status == ChannelStatus::kReady) {
                    ++sent;
                } else {
                    break;
                }
            }
            const auto snapshot = channel->DebugSnapshot();
            GO2CPP_CHECK(snapshot.capacity == capacity);
            GO2CPP_CHECK(snapshot.length ==
                         static_cast<std::size_t>(sent));
            GO2CPP_CHECK(snapshot.length <= snapshot.capacity);
            GO2CPP_CHECK(!snapshot.closed);
            channel->Close();
            GO2CPP_CHECK(channel->DebugSnapshot().closed);
        }
    }
}
