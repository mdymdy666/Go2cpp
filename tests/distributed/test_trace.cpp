/**
 * @file test_trace.cpp
 * @brief P0 三项验收：Trace 透传、父子 span、metrics 计数。
 *
 * 覆盖：
 * - `TraceId`/`SpanId` 生成唯一性与十六进制往返；
 * - `TraceContext` 文本格式（含 parent）与非法输入拒绝；
 * - 请求头 map 的 `InjectTrace`/`ExtractTrace` 下游一致性；
 * - 线程局部上下文 + `TraceScope` 嵌套恢复；
 * - 协调者/锁/租约会话的指标计数（grants/expires/fencing/renew/lost/
 *   acquire/acquire_fail/release）。
 */

#include "go2cpp/distributed/lease_session.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/metrics.hpp"
#include "go2cpp/distributed/trace.hpp"
#include "go2cpp/rpc/protocol.hpp"

#include "distributed_test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;
using namespace std::chrono_literals;

void TestIdGeneration() {
    go2cpp_tests::announce("trace.id_generation");
    std::vector<TraceId> traces;
    traces.reserve(100000);
    for (int index = 0; index < 100000; ++index) {
        traces.push_back(TraceId::Generate());
    }
    std::unordered_set<std::string> unique_traces;
    for (const auto& trace : traces) {
        GO2CPP_CHECK(trace.IsValid());
        unique_traces.insert(trace.ToString());
    }
    GO2CPP_CHECK(unique_traces.size() == traces.size());

    std::unordered_set<std::uint64_t> unique_spans;
    for (int index = 0; index < 100000; ++index) {
        const SpanId span = SpanId::Generate();
        GO2CPP_CHECK(span.IsValid());
        unique_spans.insert(span.value());
    }
    GO2CPP_CHECK(unique_spans.size() == 100000);
    std::printf("  trace ids=%zu unique, span ids=%zu unique\n", unique_traces.size(),
                unique_spans.size());
}

void TestIdParse() {
    go2cpp_tests::announce("trace.id_parse");
    const TraceId trace = TraceId::Generate();
    GO2CPP_CHECK(trace.ToString().size() == TraceId::kTextLength);
    GO2CPP_CHECK(TraceId::Parse(trace.ToString()) == trace);
    GO2CPP_CHECK(!TraceId::Parse("").IsValid());
    GO2CPP_CHECK(!TraceId::Parse(std::string(32, 'z')).IsValid());
    GO2CPP_CHECK(!TraceId::TryParse(trace.ToString(), nullptr));

    const SpanId span = SpanId::Generate();
    GO2CPP_CHECK(SpanId::Parse(span.ToString()) == span);
    GO2CPP_CHECK(!SpanId::Parse("not-a-span").IsValid());
    GO2CPP_CHECK(!SpanId::Parse("00000000000000zz").IsValid());
    GO2CPP_CHECK(SpanId::Parse("0000000000000000") == SpanId{});
}

void TestHeaderRoundTrip() {
    go2cpp_tests::announce("trace.header");
    const TraceContext root = TraceContext::CreateRoot();
    const std::string header = root.ToHeader();
    GO2CPP_CHECK(header.size() == 72);
    GO2CPP_CHECK(header.rfind("00-", 0) == 0);
    GO2CPP_CHECK(header.substr(70) == "01");
    const TraceContext parsed = TraceContext::ParseHeader(header);
    GO2CPP_CHECK(parsed.IsValid());
    GO2CPP_CHECK(parsed.trace == root.trace);
    GO2CPP_CHECK(parsed.span == root.span);
    GO2CPP_CHECK(!parsed.parent.IsValid());

    // 非法输入：版本、标志、段长、非十六进制都拒绝。
    GO2CPP_CHECK(!TraceContext::ParseHeader("").IsValid());
    GO2CPP_CHECK(!TraceContext::ParseHeader(std::string(71, '0')).IsValid());
    std::string bad_version = header;
    bad_version[0] = '1';
    GO2CPP_CHECK(!TraceContext::ParseHeader(bad_version).IsValid());
    std::string bad_flag = header;
    bad_flag[71] = '0';
    GO2CPP_CHECK(!TraceContext::ParseHeader(bad_flag).IsValid());
    std::string bad_hex = header;
    bad_hex[10] = 'z';
    GO2CPP_CHECK(!TraceContext::ParseHeader(bad_hex).IsValid());
}

void TestChildSpanAndPropagation() {
    go2cpp_tests::announce("trace.propagation");
    const TraceContext client = TraceContext::CreateRoot();
    const TraceContext server = client.ChildSpan();
    GO2CPP_CHECK(server.IsValid());
    GO2CPP_CHECK(server.trace == client.trace);
    GO2CPP_CHECK(server.parent == client.span);
    GO2CPP_CHECK(server.span != client.span);

    // 请求头 map：客户端注入 → 服务端提取，下游拿到的 trace 必须一致。
    std::map<std::string, std::string> headers;
    InjectTrace(headers, client);
    GO2CPP_CHECK(headers.count(std::string(kTraceHeader)) == 1);
    TraceContext downstream;
    GO2CPP_CHECK(ExtractTrace(headers, &downstream));
    GO2CPP_CHECK(downstream.trace == client.trace);
    GO2CPP_CHECK(downstream.span == client.span);
    const TraceContext downstream_child = downstream.ChildSpan();
    GO2CPP_CHECK(downstream_child.trace == client.trace);
    GO2CPP_CHECK(downstream_child.parent == client.span);

    // 无效上下文不写入头；空 map 提取失败且不改出参。
    std::map<std::string, std::string> empty_headers;
    InjectTrace(empty_headers, TraceContext{});
    GO2CPP_CHECK(empty_headers.empty());
    TraceContext untouched;
    GO2CPP_CHECK(!ExtractTrace(empty_headers, &untouched));
    GO2CPP_CHECK(!ExtractTrace(headers, nullptr));
}

void TestRpcProtocolPropagation() {
    go2cpp_tests::announce("trace.rpc_protocol");
    const TraceContext client = TraceContext::CreateRoot();
    const TraceContext server = client.ChildSpan();

    go2cpp::reflect::Buffer args;
    std::vector<std::uint8_t> payload;
    GO2CPP_REQUIRE(go2cpp::rpc::EncodeRequest(payload, 7, "Demo.Call", args, server));

    go2cpp::frame::Frame frame_value;
    frame_value.type = static_cast<std::uint8_t>(go2cpp::rpc::FrameType::kRequest);
    frame_value.flags = go2cpp::rpc::kFlagTrace;
    frame_value.payload = payload;

    go2cpp::rpc::RequestMessage decoded;
    std::string error;
    GO2CPP_REQUIRE(go2cpp::rpc::DecodeRequest(frame_value, &decoded, &error));
    GO2CPP_CHECK(decoded.has_trace);
    GO2CPP_CHECK(decoded.trace.IsValid());
    // 下游拿到的 trace 与客户端一致，父子关系与发送端一致。
    GO2CPP_CHECK(decoded.trace.trace == client.trace);
    GO2CPP_CHECK(decoded.trace.span == server.span);
    GO2CPP_CHECK(decoded.trace.parent == client.span);

    // 旧编码（不带 trace）仍然可解码，且不误报 has_trace。
    std::vector<std::uint8_t> legacy_payload;
    GO2CPP_REQUIRE(go2cpp::rpc::EncodeRequest(legacy_payload, 8, "Demo.Call", args));
    go2cpp::frame::Frame legacy_frame;
    legacy_frame.type = static_cast<std::uint8_t>(go2cpp::rpc::FrameType::kRequest);
    legacy_frame.flags = 0;
    legacy_frame.payload = legacy_payload;
    go2cpp::rpc::RequestMessage legacy_decoded;
    GO2CPP_REQUIRE(go2cpp::rpc::DecodeRequest(legacy_frame, &legacy_decoded, &error));
    GO2CPP_CHECK(!legacy_decoded.has_trace);
    GO2CPP_CHECK(!legacy_decoded.trace.IsValid());
}

void TestThreadLocalScope() {
    go2cpp_tests::announce("trace.thread_local");
    GO2CPP_CHECK(!CurrentTraceContext().IsValid());
    const TraceContext outer = TraceContext::CreateRoot();
    {
        const TraceScope scope(outer);
        GO2CPP_CHECK(CurrentTraceContext().trace == outer.trace);
        const TraceContext inner = CurrentTraceContext().ChildSpan();
        {
            const TraceScope nested(inner);
            GO2CPP_CHECK(CurrentTraceContext().trace == outer.trace);
            GO2CPP_CHECK(CurrentTraceContext().span == inner.span);
        }
        GO2CPP_CHECK(CurrentTraceContext().span == outer.span);
    }
    GO2CPP_CHECK(!CurrentTraceContext().IsValid());

    // 另一个线程没有继承当前上下文（thread_local 不跨线程）。
    bool other_thread_sees_context = true;
    {
        const TraceScope scope(outer);
        std::thread other([&other_thread_sees_context]() {
            other_thread_sees_context = CurrentTraceContext().IsValid();
        });
        other.join();
    }
    GO2CPP_CHECK(!other_thread_sees_context);
}

void TestMetrics() {
    go2cpp_tests::announce("metrics.counters");
    CoordinatorOptions options;
    options.start_sweeper = false;
    options.min_ttl = 10ms;
    options.max_ttl = 1000ms;
    Coordinator coordinator(options);

    const std::uint64_t granted_before = metrics::LeasesGranted().Value();
    const std::uint64_t fencing_before = metrics::FencingAdvanced().Value();
    const std::uint64_t expired_before = metrics::LeasesExpired().Value();
    const std::uint64_t acquire_before = metrics::LockAcquire().Value();
    const std::uint64_t acquire_fail_before = metrics::LockAcquireFail().Value();
    const std::uint64_t release_before = metrics::LockRelease().Value();
    const std::uint64_t renew_before = metrics::LeaseRenew().Value();
    const std::uint64_t lost_before = metrics::LeaseLost().Value();

    ServiceRegistration registration;
    registration.service = "metrics-svc";
    registration.instance = "node-1";
    registration.endpoint = "127.0.0.1:1";
    registration.owner = "owner-1";
    registration.ttl = 20ms;
    GO2CPP_REQUIRE(coordinator.RegisterService(registration).ok());
    // 同 owner 重复注册是幂等续约，也应计入授予计数。
    GO2CPP_REQUIRE(coordinator.RegisterService(registration).ok());
    GO2CPP_CHECK(metrics::LeasesGranted().Value() - granted_before == 2);

    // 锁：第一把授予 +1 fencing；冲突只计 acquire_fail。
    DistributedLock locks(coordinator);
    auto first = locks.Acquire("metrics-lock", "owner-1", 500ms);
    GO2CPP_REQUIRE(first.ok());
    GO2CPP_CHECK(metrics::LeasesGranted().Value() - granted_before == 3);
    GO2CPP_CHECK(metrics::FencingAdvanced().Value() - fencing_before == 1);
    auto conflict = locks.Acquire("metrics-lock", "owner-2", 500ms);
    GO2CPP_CHECK(!conflict.ok());
    GO2CPP_CHECK(metrics::LockAcquire().Value() - acquire_before == 2);
    GO2CPP_CHECK(metrics::LockAcquireFail().Value() - acquire_fail_before == 1);
    first.value.Release();
    GO2CPP_CHECK(metrics::LockRelease().Value() - release_before == 1);
    // 幂等重复释放不再计数。
    first.value.Release();
    GO2CPP_CHECK(metrics::LockRelease().Value() - release_before == 1);

    // 过期：短 TTL 租约到期后由 SweepExpired 回收。
    std::this_thread::sleep_for(40ms);
    const std::size_t swept = coordinator.SweepExpired();
    GO2CPP_CHECK(swept >= 1);
    GO2CPP_CHECK(metrics::LeasesExpired().Value() - expired_before == swept);

    // 续约/失效：直接驱动 LeaseSession，避免依赖后台线程时序。
    detail::LeaseSession session(
        100ms, 0ms,
        [&coordinator, &registration]() {
            return coordinator.RenewLease(1, registration.owner);
        },
        nullptr);
    session.Start(false);
    // 租约 1 已被清扫，续约会失败并触发 lost；成功路径用重新注册的租约验证。
    const auto renewed = coordinator.RegisterService(registration);
    GO2CPP_REQUIRE(renewed.ok());
    detail::LeaseSession renewing(
        100ms, 0ms,
        [&coordinator, &registration, lease = renewed.value.id]() {
            return coordinator.RenewLease(lease, registration.owner);
        },
        nullptr);
    renewing.Start(false);
    GO2CPP_REQUIRE(renewing.RenewNow().ok());
    GO2CPP_CHECK(metrics::LeaseRenew().Value() - renew_before == 1);
    renewing.MarkLost("test");
    GO2CPP_CHECK(metrics::LeaseLost().Value() - lost_before == 1);
    renewing.MarkLost("again");
    GO2CPP_CHECK(metrics::LeaseLost().Value() - lost_before == 1);
    session.MarkLost("expired-path");
    GO2CPP_CHECK(metrics::LeaseLost().Value() - lost_before == 2);

    std::printf("  granted=%llu expired=%llu fencing=%llu acquire=%llu fail=%llu "
                "release=%llu renew=%llu lost=%llu\n",
                static_cast<unsigned long long>(metrics::LeasesGranted().Value()),
                static_cast<unsigned long long>(metrics::LeasesExpired().Value()),
                static_cast<unsigned long long>(metrics::FencingAdvanced().Value()),
                static_cast<unsigned long long>(metrics::LockAcquire().Value()),
                static_cast<unsigned long long>(metrics::LockAcquireFail().Value()),
                static_cast<unsigned long long>(metrics::LockRelease().Value()),
                static_cast<unsigned long long>(metrics::LeaseRenew().Value()),
                static_cast<unsigned long long>(metrics::LeaseLost().Value()));
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.trace");
    TestIdGeneration();
    TestIdParse();
    TestHeaderRoundTrip();
    TestChildSpanAndPropagation();
    TestRpcProtocolPropagation();
    TestThreadLocalScope();
    TestMetrics();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_trace_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_trace_test: all cases passed\n");
    return 0;
}
