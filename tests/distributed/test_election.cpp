/**
 * @file test_election.cpp
 * @brief 选主：先到者为主、主动退出/租约过期后的重选、任期单调、回调。
 *
 * 断言口径：
 * - `LeaderElection::Leader()` 直查协调者（强一致）；
 * - `Candidate::is_leader()` 是心跳缓存（最终一致），用 `WaitUntil` 等待；
 * - 每次 leader 变化 epoch 必须严格递增——它是选主场景的 fencing token。
 */

#include "go2cpp/distributed/election.hpp"

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

void TestFirstCandidateBecomesLeader() {
    go2cpp_tests::announce("election.first_leader");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    auto first = election.Join("primary", "node-1", 300ms);
    auto second = election.Join("primary", "node-2", 300ms);
    auto third = election.Join("primary", "node-3", 300ms);
    GO2CPP_REQUIRE(first.ok() && second.ok() && third.ok());

    const auto leader = election.Leader("primary");
    GO2CPP_REQUIRE(leader.has_value());
    GO2CPP_CHECK(leader->node == "node-1");
    GO2CPP_CHECK(leader->epoch == 1);
    GO2CPP_CHECK(first.value.is_leader());
    GO2CPP_CHECK(!second.value.is_leader());
    GO2CPP_CHECK(!third.value.is_leader());

    const ElectionState state = election.State("primary");
    GO2CPP_REQUIRE(state.candidates.size() == 3);
    GO2CPP_CHECK(state.leader == "node-1");
    for (std::size_t index = 0; index < state.candidates.size(); ++index) {
        GO2CPP_CHECK(state.candidates[index].node ==
                     "node-" + std::to_string(index + 1));
        GO2CPP_CHECK(state.candidates[index].leader == (index == 0));
    }
}

void TestLeaderLeaveTriggersReselection() {
    go2cpp_tests::announce("election.leave_reelect");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    auto first = election.Join("lease", "node-1", 300ms);
    auto second = election.Join("lease", "node-2", 300ms);
    GO2CPP_REQUIRE(first.ok() && second.ok());
    GO2CPP_CHECK(election.Leader("lease")->node == "node-1");
    const std::uint64_t epoch_before = election.Leader("lease")->epoch;

    first.value.Leave();
    GO2CPP_CHECK(!first.value.valid());
    GO2CPP_CHECK(!first.value.is_leader());
    const auto next = election.Leader("lease");
    GO2CPP_REQUIRE(next.has_value());
    GO2CPP_CHECK(next->node == "node-2");
    GO2CPP_CHECK(next->epoch > epoch_before);
    // node-2 的心跳缓存会跟上（最多一个心跳周期）。
    GO2CPP_CHECK(WaitUntil([&] { return second.value.is_leader(); }, 1s));
}

void TestLeaderExpiryTriggersReselection() {
    go2cpp_tests::announce("election.expiry_reelect");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    CandidateOptions manual;
    manual.auto_renew = false;
    auto leader = election.Join("expire", "node-1", 120ms, manual);
    auto backup = election.Join("expire", "node-2", 300ms);
    auto third = election.Join("expire", "node-3", 300ms);
    GO2CPP_REQUIRE(leader.ok() && backup.ok() && third.ok());
    GO2CPP_REQUIRE(election.Leader("expire").has_value());
    GO2CPP_CHECK(election.Leader("expire")->node == "node-1");

    // 主停止续约（模拟进程卡死/网络隔离），TTL 到期后必须自动把主判给 node-2。
    GO2CPP_CHECK(WaitUntil([&] { return !leader.value.valid(); }, 1s));
    GO2CPP_CHECK(leader.value.lost());
    GO2CPP_CHECK(WaitUntil(
        [&] {
            const auto current = election.Leader("expire");
            return current.has_value() && current->node == "node-2";
        },
        2s));
    GO2CPP_CHECK(election.Leader("expire")->epoch > 1);
    GO2CPP_CHECK(WaitUntil([&] { return backup.value.is_leader(); }, 1s));
    GO2CPP_CHECK(!third.value.is_leader());
}

void TestRejoinGoesToBack() {
    go2cpp_tests::announce("election.rejoin_back");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    auto node1 = election.Join("queue", "node-1", 500ms);
    auto node2 = election.Join("queue", "node-2", 500ms);
    auto node3 = election.Join("queue", "node-3", 500ms);
    GO2CPP_REQUIRE(node1.ok() && node2.ok() && node3.ok());
    GO2CPP_CHECK(election.Leader("queue")->node == "node-1");

    node1.value.Leave();
    GO2CPP_CHECK(election.Leader("queue")->node == "node-2");
    // node-1 重新加入：新租约编号更大，排在 node-3 之后；主仍是 node-2。
    auto rejoined = election.Join("queue", "node-1", 500ms);
    GO2CPP_REQUIRE(rejoined.ok());
    GO2CPP_CHECK(rejoined.value.lease() > node3.value.lease());
    GO2CPP_CHECK(election.Leader("queue")->node == "node-2");

    node2.value.Leave();
    GO2CPP_CHECK(election.Leader("queue")->node == "node-3");
}

void TestAllLeaveMeansNoLeader() {
    go2cpp_tests::announce("election.no_leader");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    auto node1 = election.Join("empty", "node-1", 300ms);
    auto node2 = election.Join("empty", "node-2", 300ms);
    GO2CPP_REQUIRE(node1.ok() && node2.ok());
    const std::uint64_t epoch = election.Leader("empty")->epoch;
    node1.value.Leave();
    GO2CPP_CHECK(election.Leader("empty")->node == "node-2");
    node2.value.Leave();
    GO2CPP_CHECK(!election.Leader("empty").has_value());
    GO2CPP_CHECK(election.State("empty").leader.empty());
    GO2CPP_CHECK(election.State("empty").epoch > epoch);

    // 再次加入：开启新任期，epoch 继续递增（不能回退到 1）。
    auto rejoined = election.Join("empty", "node-9", 300ms);
    GO2CPP_REQUIRE(rejoined.ok());
    const auto leader = election.Leader("empty");
    GO2CPP_REQUIRE(leader.has_value());
    GO2CPP_CHECK(leader->node == "node-9");
    GO2CPP_CHECK(leader->epoch > epoch);
}

void TestLeaderChangeCallback() {
    go2cpp_tests::announce("election.callback");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    auto node1 = election.Join("watch", "node-1", 200ms);
    auto node2 = election.Join("watch", "node-2", 200ms);
    GO2CPP_REQUIRE(node1.ok() && node2.ok());
    Signal signal;
    std::string observed;
    std::uint64_t observed_epoch = 0;
    node2.value.OnLeaderChange([&](const std::optional<LeaderInfo> &leader) {
        if (leader.has_value()) {
            observed = leader->node;
            observed_epoch = leader->epoch;
        }
        signal.Set();
    });
    node1.value.Leave();
    GO2CPP_CHECK(signal.WaitFor(2s));
    GO2CPP_CHECK(observed == "node-2");
    GO2CPP_CHECK(observed_epoch >= 2);
    GO2CPP_CHECK(node2.value.is_leader());
}

void TestEpochMonotonicAcrossChurn() {
    go2cpp_tests::announce("election.epoch_monotonic");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    std::uint64_t last_epoch = 0;
    for (int round = 0; round < 6; ++round) {
        const std::string node = "node-" + std::to_string(round);
        auto candidate = election.Join("churn", node, 200ms);
        GO2CPP_REQUIRE(candidate.ok());
        // 上一轮已经全部离开，本轮唯一候选者上任，开启新任期。
        const auto leader = election.Leader("churn");
        GO2CPP_REQUIRE(leader.has_value());
        GO2CPP_CHECK(leader->node == node);
        GO2CPP_CHECK(leader->epoch > last_epoch);
        last_epoch = leader->epoch;

        candidate.value.Leave();
        GO2CPP_CHECK(!election.Leader("churn").has_value());
        const ElectionState state = election.State("churn");
        GO2CPP_CHECK(state.leader.empty());
        GO2CPP_CHECK(state.epoch > last_epoch);
        last_epoch = state.epoch;
    }
}

void TestInvalidArguments() {
    go2cpp_tests::announce("election.invalid_args");
    Coordinator coordinator(MakeOptions());
    LeaderElection election(coordinator);
    GO2CPP_CHECK(election.Join("", "node", 100ms).status == Status::kInvalidArgument);
    GO2CPP_CHECK(election.Join("e", "", 100ms).status == Status::kInvalidArgument);
    GO2CPP_CHECK(election.Join("e", "node", 5ms).status == Status::kInvalidArgument);
    GO2CPP_CHECK(election.Join("e", "node", 11s).status == Status::kInvalidArgument);
    GO2CPP_CHECK(!election.Leader("unknown").has_value());
    GO2CPP_CHECK(election.State("unknown").candidates.empty());
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.election");
    TestFirstCandidateBecomesLeader();
    TestLeaderLeaveTriggersReselection();
    TestLeaderExpiryTriggersReselection();
    TestRejoinGoesToBack();
    TestAllLeaveMeansNoLeader();
    TestLeaderChangeCallback();
    TestEpochMonotonicAcrossChurn();
    TestInvalidArguments();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_election_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_election_test: all cases passed\n");
    return 0;
}
