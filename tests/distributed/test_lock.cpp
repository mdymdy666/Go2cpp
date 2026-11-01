/**
 * @file test_lock.cpp
 * @brief 分布式锁：互斥、租约续期、超时释放、fencing token 与失效感知。
 *
 * 核心断言不是「能拿到锁」，而是：
 * - 任意时刻同一 key 最多一个 `valid()` 的句柄；
 * - 新持有者的 fencing token 一定大于历史持有者；
 * - 旧持有者在租约过期/被强制释放后，`lost()` 必须变 true，而不是继续
 *   以为自己持有（这是「不双主」在客户端侧的可见性）。
 */

#include "go2cpp/distributed/lock.hpp"

#include "distributed_test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;

void TestMutualExclusionAndFencing() {
    go2cpp_tests::announce("lock.mutex");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);

    auto first = locks.Acquire("job:42", "worker-a", 500ms);
    GO2CPP_REQUIRE(first.ok());
    LockHandle handle = std::move(first.value);
    GO2CPP_CHECK(handle.valid());
    GO2CPP_CHECK(handle.fencing_token() == 1);

    const auto conflict = locks.Acquire("job:42", "worker-b", 500ms);
    GO2CPP_CHECK(!conflict.ok());
    GO2CPP_CHECK(conflict.status == Status::kConflict);
    GO2CPP_CHECK(conflict.message.find("worker-a") != std::string::npos);

    handle.Release();
    GO2CPP_CHECK(!handle.valid());
    const auto second = locks.Acquire("job:42", "worker-b", 500ms);
    GO2CPP_REQUIRE(second.ok());
    GO2CPP_CHECK(second.value.fencing_token() > 1);
    GO2CPP_CHECK(locks.Query("job:42")->owner == "worker-b");
}

void TestAutoRenewKeepsLock() {
    go2cpp_tests::announce("lock.auto_renew");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    auto acquired = locks.Acquire("lease", "worker-a", 150ms);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    // 稳定跨过 3 个 TTL：后台续约必须让它一直有效。
    SleepMs(550);
    GO2CPP_CHECK(handle.valid());
    GO2CPP_CHECK(!handle.lost());
    GO2CPP_CHECK(handle.remaining() > std::chrono::milliseconds::zero());
    const auto blocked = locks.Acquire("lease", "worker-b", 150ms);
    GO2CPP_CHECK(!blocked.ok());
    GO2CPP_CHECK(blocked.status == Status::kConflict);
}

void TestExpiryReleasesLock() {
    go2cpp_tests::announce("lock.expiry");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    LockOptions manual;
    manual.auto_renew = false;
    auto acquired = locks.Acquire("expire", "worker-a", 100ms, manual);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    const std::uint64_t old_token = handle.fencing_token();

    GO2CPP_CHECK(WaitUntil([&] { return !handle.valid(); }, 1s));
    GO2CPP_CHECK(handle.lost());

    const auto stolen = locks.Acquire("expire", "worker-b", 300ms, manual);
    GO2CPP_REQUIRE(stolen.ok());
    GO2CPP_CHECK(stolen.value.fencing_token() > old_token);
    GO2CPP_CHECK(stolen.value.owner() == "worker-b");
    // 旧句柄不能通过一次迟到的续约「复活」并把新持有者顶掉。
    GO2CPP_CHECK(!handle.Renew().ok());
    GO2CPP_CHECK(locks.Query("expire")->owner == "worker-b");
}

void TestManualRenew() {
    go2cpp_tests::announce("lock.manual_renew");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    LockOptions manual;
    manual.auto_renew = false;
    auto acquired = locks.Acquire("manual", "worker-a", 120ms, manual);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    for (int attempt = 0; attempt < 6; ++attempt) {
        SleepMs(60);
        GO2CPP_REQUIRE(handle.Renew().ok());
        GO2CPP_CHECK(handle.valid());
    }
    GO2CPP_CHECK(locks.Acquire("manual", "worker-b", 120ms, manual).status == Status::kConflict);
}

void TestWrongOwnerRejected() {
    go2cpp_tests::announce("lock.wrong_owner");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    auto acquired = locks.Acquire("owner", "worker-a", 500ms);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    // 用错误 owner 续约/释放：协调者必须拒绝，锁不能易主。
    GO2CPP_CHECK(coordinator.RenewLease(handle.lease(), "worker-b").status ==
                 Status::kNotOwner);
    GO2CPP_CHECK(coordinator.ReleaseLease(handle.lease(), "worker-b").status ==
                 Status::kNotOwner);
    GO2CPP_CHECK(handle.valid());
    GO2CPP_CHECK(locks.Query("owner")->owner == "worker-a");
}

void TestReleaseIdempotentAndReacquire() {
    go2cpp_tests::announce("lock.release_idempotent");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    auto acquired = locks.Acquire("idem", "worker-a", 500ms);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    handle.Release();
    handle.Release();
    GO2CPP_CHECK(!handle.valid());
    GO2CPP_CHECK(!locks.Query("idem").has_value());
    auto again = locks.Acquire("idem", "worker-a", 500ms);
    GO2CPP_REQUIRE(again.ok());
    GO2CPP_CHECK(again.value.fencing_token() > handle.fencing_token());
}

void TestFencingMonotonic() {
    go2cpp_tests::announce("lock.fencing_monotonic");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    std::uint64_t previous = 0;
    for (int round = 0; round < 32; ++round) {
        auto acquired = locks.Acquire("mono", "worker-" + std::to_string(round % 3), 500ms);
        GO2CPP_REQUIRE(acquired.ok());
        GO2CPP_CHECK(acquired.value.fencing_token() > previous);
        previous = acquired.value.fencing_token();
        acquired.value.Release();
    }
}

void TestLostCallbackOnForcedRelease() {
    go2cpp_tests::announce("lock.lost_callback");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    auto acquired = locks.Acquire("callback", "worker-a", 150ms);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    Signal signal;
    handle.OnLost([&signal] { signal.Set(); });
    // 模拟「锁被判定过期/被管理端强制回收」：心跳的下一次续约会失败。
    GO2CPP_CHECK(coordinator.ReleaseLease(handle.lease(), "worker-a").ok());
    GO2CPP_CHECK(signal.WaitFor(2s));
    GO2CPP_CHECK(handle.lost());
    GO2CPP_CHECK(!handle.valid());
    GO2CPP_CHECK(!handle.Renew().ok());
}

void TestQueryLock() {
    go2cpp_tests::announce("lock.query");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    GO2CPP_CHECK(!locks.Query("query").has_value());
    auto acquired = locks.Acquire("query", "worker-a", 300ms);
    GO2CPP_REQUIRE(acquired.ok());
    LockHandle handle = std::move(acquired.value);
    const auto info = locks.Query("query");
    GO2CPP_REQUIRE(info.has_value());
    GO2CPP_CHECK(info->key == "query");
    GO2CPP_CHECK(info->owner == "worker-a");
    GO2CPP_CHECK(info->lease == handle.lease());
    GO2CPP_CHECK(info->fencing == handle.fencing_token());
    GO2CPP_CHECK(info->held);
    GO2CPP_CHECK(info->remaining > std::chrono::milliseconds::zero());
    GO2CPP_CHECK(locks.Locks().size() == 1);
    handle.Release();
    GO2CPP_CHECK(!locks.Query("query").has_value());
    GO2CPP_CHECK(locks.Locks().empty());
}

void TestInvalidArguments() {
    go2cpp_tests::announce("lock.invalid_args");
    Coordinator coordinator(MakeOptions());
    DistributedLock locks(coordinator);
    GO2CPP_CHECK(locks.Acquire("", "worker", 100ms).status == Status::kInvalidArgument);
    GO2CPP_CHECK(locks.Acquire("key", "", 100ms).status == Status::kInvalidArgument);
    GO2CPP_CHECK(locks.Acquire("key", "worker", 5ms).status == Status::kInvalidArgument);
    GO2CPP_CHECK(locks.Acquire("key", "worker", 11s).status == Status::kInvalidArgument);
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.lock");
    TestMutualExclusionAndFencing();
    TestAutoRenewKeepsLock();
    TestExpiryReleasesLock();
    TestManualRenew();
    TestWrongOwnerRejected();
    TestReleaseIdempotentAndReacquire();
    TestFencingMonotonic();
    TestLostCallbackOnForcedRelease();
    TestQueryLock();
    TestInvalidArguments();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_lock_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_lock_test: all cases passed\n");
    return 0;
}
