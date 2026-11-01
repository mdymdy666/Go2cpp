/**
 * @file test_random.cpp
 * @brief 随机操作序列 + 不变量检查（属性测试）。
 *
 * 每一步随机执行注册/续约/释放/抢锁/解锁/加入选举/退出选举/清扫，并在
 * 固定间隔检查四条全局不变量：
 *
 * 1. 服务发现不返回过期实例，也不返回重复实例；
 * 2. 锁快照只会出现 `remaining > 0` 的记录，且 fencing token 对同一 key
 *    严格单调（新授予 > 历史最大）；
 * 3. 选举 leader 若存在，必须是候选者中租约编号最小者（`QueryElection`
 *    的 candidates 已按编号升序，所以就是 `front()`）；
 * 4. 错误 owner 的续约/释放必须被拒绝，且不改变状态。
 *
 * 固定种子，失败可复现；随机 TTL 覆盖 40ms~220ms 的过期窗口。
 */

#include "go2cpp/distributed/coordinator.hpp"
#include "go2cpp/distributed/election.hpp"
#include "go2cpp/distributed/lock.hpp"
#include "go2cpp/distributed/registry.hpp"

#include "distributed_test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace go2cpp::distributed;
using namespace go2cpp::distributed::test;

constexpr int kSteps = 1500;
constexpr std::uint64_t kSeed = 0x5EED1234ULL;

struct Model {
    Coordinator coordinator;
    ServiceRegistry registry;
    DistributedLock locks;
    LeaderElection election;
    Rng rng{kSeed};

    std::map<std::string, std::uint64_t> max_fencing;
    std::unordered_set<LeaseId> leases;

    explicit Model() : registry(coordinator), locks(coordinator), election(coordinator) {}

    Duration RandomTtl() { return Duration(40 + static_cast<std::int64_t>(rng.Below(181))); }
    std::string RandomService() { return "svc-" + std::to_string(rng.Below(3)); }
    std::string RandomInstance() { return "inst-" + std::to_string(rng.Below(10)); }
    std::string RandomKey() { return "key-" + std::to_string(rng.Below(5)); }
    std::string RandomOwner() { return "owner-" + std::to_string(rng.Below(4)); }
    std::string RandomElection() { return "e-" + std::to_string(rng.Below(2)); }
    std::string RandomNode() { return "node-" + std::to_string(rng.Below(4)); }
};

void CheckServiceInvariants(Model &model) {
    for (int service = 0; service < 3; ++service) {
        const std::string name = "svc-" + std::to_string(service);
        const auto instances = model.registry.Lookup(name);
        std::set<std::string> seen;
        for (const ServiceInstance &instance : instances) {
            GO2CPP_CHECK(instance.remaining > Duration::zero());
            GO2CPP_CHECK(seen.insert(instance.instance).second);
            GO2CPP_CHECK(instance.lease != kInvalidLease);
        }
    }
}

void CheckLockInvariants(Model &model) {
    for (const LockInfo &info : model.locks.Locks()) {
        GO2CPP_CHECK(info.remaining > Duration::zero());
        GO2CPP_CHECK(info.held);
        GO2CPP_CHECK(info.fencing > 0);
        const auto query = model.locks.Query(info.key);
        GO2CPP_REQUIRE(query.has_value());
        GO2CPP_CHECK(query->lease == info.lease);
        GO2CPP_CHECK(query->owner == info.owner);
        // 当前 token 不可能超过历史授予过的最大值（历史值只增不减）。
        const auto previous = model.max_fencing.find(info.key);
        GO2CPP_REQUIRE(previous != model.max_fencing.end());
        GO2CPP_CHECK(info.fencing <= previous->second);
    }
}

void CheckElectionInvariants(Model &model) {
    for (const std::string &name : model.election.Elections()) {
        const ElectionState state = model.election.State(name);
        for (const CandidateInfo &candidate : state.candidates) {
            GO2CPP_CHECK(candidate.remaining > Duration::zero());
        }
        if (!state.leader.empty()) {
            GO2CPP_REQUIRE(!state.candidates.empty());
            // candidates 按 lease 编号升序，leader 必须是编号最小者。
            GO2CPP_CHECK(state.candidates.front().node == state.leader);
            GO2CPP_CHECK(state.candidates.front().leader);
            bool found = false;
            for (const CandidateInfo &candidate : state.candidates) {
                found = found || candidate.node == state.leader;
            }
            GO2CPP_CHECK(found);
        }
    }
}

void StepRegister(Model &model) {
    ServiceRegistration registration;
    registration.service = model.RandomService();
    registration.instance = model.RandomInstance();
    registration.endpoint = "10.9." + std::to_string(model.rng.Below(255)) + ":9000";
    registration.owner = registration.instance;
    registration.ttl = model.RandomTtl();
    const auto result = model.coordinator.RegisterService(registration);
    if (result.ok()) {
        model.leases.insert(result.value.id);
        GO2CPP_CHECK(result.value.remaining > Duration::zero());
        GO2CPP_CHECK(result.value.kind == LeaseKind::kService);
        GO2CPP_CHECK(result.value.name == registration.service);
        GO2CPP_CHECK(result.value.instance == registration.instance);
    }
}

void StepRelease(Model &model) {
    if (model.leases.empty()) {
        return;
    }
    auto it = model.leases.begin();
    std::advance(it, static_cast<long>(model.rng.Below(model.leases.size())));
    const LeaseId lease = *it;
    const auto result = model.coordinator.ReleaseLease(lease, "wrong-owner");
    // 未知 owner 不允许释放任何东西。
    GO2CPP_CHECK(result.status == Status::kNotOwner || result.status == Status::kNotFound);
    // 用真实 owner 释放需要知道它；这里只验证「错误 owner 无害」，真正的
    // 释放由过期清扫与下面的锁/选举路径覆盖。
}

void StepRenew(Model &model) {
    if (model.leases.empty()) {
        return;
    }
    auto it = model.leases.begin();
    std::advance(it, static_cast<long>(model.rng.Below(model.leases.size())));
    const LeaseId lease = *it;
    const auto wrong = model.coordinator.RenewLease(lease, "wrong-owner");
    GO2CPP_CHECK(wrong.status == Status::kNotOwner || wrong.status == Status::kNotFound);
}

void StepAcquireLock(Model &model) {
    const std::string key = model.RandomKey();
    const Duration ttl = model.RandomTtl();
    const auto result = model.locks.Acquire(key, model.RandomOwner(), ttl);
    if (!result.ok()) {
        GO2CPP_CHECK(result.status == Status::kConflict);
        return;
    }
    const std::uint64_t token = result.value.fencing_token();
    const auto previous = model.max_fencing.find(key);
    if (previous != model.max_fencing.end()) {
        GO2CPP_CHECK(token > previous->second);
    }
    model.max_fencing[key] = token;
    model.leases.insert(result.value.lease());
    // 立刻释放：把「授予 → 释放 → 再授予」的 fencing 单调路径也覆盖到。
    GO2CPP_CHECK(
        model.coordinator.ReleaseLease(result.value.lease(), result.value.owner()).ok());
}

void StepReleaseLock(Model &model) {
    const std::string key = model.RandomKey();
    const auto info = model.locks.Query(key);
    if (!info.has_value()) {
        return;
    }
    const auto result = model.coordinator.ReleaseLease(info->lease, info->owner);
    GO2CPP_CHECK(result.ok() || result.status == Status::kNotFound);
}

void StepJoinElection(Model &model) {
    const auto result = model.election.Join(model.RandomElection(), model.RandomNode(),
                                            model.RandomTtl());
    if (result.ok()) {
        model.leases.insert(result.value.lease());
        GO2CPP_CHECK(result.value.valid());
    }
}

void StepLeaveElection(Model &model) {
    const std::string name = model.RandomElection();
    const ElectionState state = model.election.State(name);
    if (state.candidates.empty()) {
        return;
    }
    const auto &candidate =
        state.candidates[model.rng.Below(state.candidates.size())];
    const auto result = model.coordinator.ReleaseLease(candidate.lease, candidate.node);
    GO2CPP_CHECK(result.ok() || result.status == Status::kNotFound);
}

void TestRandomizedInvariants() {
    go2cpp_tests::announce("random.invariants");
    Model model;
    // 先放几个长期候选者，让选举检查有内容；Candidate 是 RAII 句柄，
    // 必须持有到测试结束（临时对象析构会立即 Leave）。
    std::vector<Candidate> baseline;
    for (int node = 0; node < 3; ++node) {
        auto joined =
            model.election.Join("baseline", "node-" + std::to_string(node), 5s);
        GO2CPP_REQUIRE(joined.ok());
        baseline.push_back(std::move(joined.value));
    }

    for (int step = 0; step < kSteps; ++step) {
        switch (model.rng.Below(9)) {
        case 0:
        case 1:
            StepRegister(model);
            break;
        case 2:
            StepRenew(model);
            break;
        case 3:
            StepRelease(model);
            break;
        case 4:
        case 5:
            StepAcquireLock(model);
            break;
        case 6:
            StepReleaseLock(model);
            break;
        case 7:
            StepJoinElection(model);
            break;
        default:
            if (model.rng.Chance(50)) {
                StepLeaveElection(model);
            } else {
                model.coordinator.SweepExpired();
            }
            break;
        }
        if (model.rng.Chance(35)) {
            SleepMs(1);
        }
        if (step % 25 == 0) {
            CheckServiceInvariants(model);
            CheckLockInvariants(model);
            CheckElectionInvariants(model);
        }
    }

    // 收尾：清理所有锁与选举候选者，等最长 TTL（220ms）过去后清扫服务租约。
    for (const LockInfo &info : model.locks.Locks()) {
        model.coordinator.ReleaseLease(info.lease, info.owner);
    }
    for (const std::string &name : model.election.Elections()) {
        const ElectionState state = model.election.State(name);
        for (const CandidateInfo &candidate : state.candidates) {
            model.coordinator.ReleaseLease(candidate.lease, candidate.node);
        }
    }
    for (Candidate &candidate : baseline) {
        candidate.Leave();
    }
    model.coordinator.SweepExpired();
    GO2CPP_CHECK(model.locks.Locks().empty());
    // 服务注册没有主动释放路径（模拟进程退出），等租约自然过期再清扫。
    SleepMs(320);
    model.coordinator.SweepExpired();
    for (int service = 0; service < 3; ++service) {
        GO2CPP_CHECK(model.registry.Lookup("svc-" + std::to_string(service)).empty());
    }
    GO2CPP_CHECK(model.coordinator.Stats().active_leases == 0);
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.random");
    TestRandomizedInvariants();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_random_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_random_test: all cases passed\n");
    return 0;
}
