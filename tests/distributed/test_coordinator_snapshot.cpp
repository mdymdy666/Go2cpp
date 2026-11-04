/**
 * @file test_coordinator_snapshot.cpp
 * @brief Coordinator 快照：重启后 fencing token 严格单调 + 损坏快照拒绝启动。
 *
 * 这组用例直接对应 P1 的验收标准：
 * - 旧实例抢锁拿到 token=N，快照并销毁；新实例恢复后抢同一个 key，
 *   拿到的 token **必须** > N（不是重新从 1 开始）；
 * - 多次重启后 token 仍然单调（快照里的 m_fencing 被正确恢复）；
 * - 选举 epoch 同样只升不降；
 * - 快照被截断/翻转字节 → 新实例拒绝启动（healthy=false、load_error 非空、
 *   写操作返回 kUnavailable），绝不静默回退。
 */

#include "go2cpp/distributed/coordinator.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

using go2cpp::distributed::Coordinator;
using go2cpp::distributed::CoordinatorOptions;
using go2cpp::distributed::Duration;
using go2cpp::distributed::Status;

std::string MakeTempDir(const std::string &name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     (name + "_" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir.string();
}

CoordinatorOptions MakeOptions(const std::string &path) {
    CoordinatorOptions options;
    options.start_sweeper = false;  // 测试用同步 SweepExpired 精确控制过期。
    options.snapshot_path = path;
    options.snapshot_on_write = true;
    options.snapshot_fsync = true;
    return options;
}

/// 抢锁并返回 fencing token；失败返回 0 并打印状态。
std::uint64_t AcquireToken(Coordinator &coordinator, const std::string &key,
                           const std::string &owner) {
    const auto acquired = coordinator.AcquireLock(key, owner, Duration(60000));
    if (!acquired.ok()) {
        std::printf("acquire failed: %s (%s)\n", go2cpp::distributed::StatusName(acquired.status),
                    acquired.message.c_str());
        return 0;
    }
    return acquired.value.fencing;
}

void TestRestartKeepsFencing() {
    go2cpp_tests::announce("coordinator_snapshot.restart_fencing");
    const std::string dir = MakeTempDir("go2cpp_coord_fencing");
    const std::string path = dir + "/coordinator.snapshot";

    std::uint64_t old_token = 0;
    {
        Coordinator coordinator(MakeOptions(path));
        GO2CPP_CHECK(coordinator.healthy());
        old_token = AcquireToken(coordinator, "lock/order", "node-a");
        GO2CPP_CHECK(old_token == 1);
        // 旧持有者不释放就重启：恢复后旧租约视为失效，但 token 不能回退。
        GO2CPP_CHECK(coordinator.Snapshot().ok());
    }
    std::uint64_t new_token = 0;
    {
        Coordinator coordinator(MakeOptions(path));
        GO2CPP_CHECK(coordinator.healthy());
        GO2CPP_CHECK(coordinator.load_error().empty());
        new_token = AcquireToken(coordinator, "lock/order", "node-b");
        GO2CPP_CHECK(new_token > old_token);
    }
    std::printf("restart fencing: old token=%llu new token=%llu (monotonic=%s)\n",
                static_cast<unsigned long long>(old_token),
                static_cast<unsigned long long>(new_token),
                new_token > old_token ? "yes" : "no");

    // 第三次重启：token 继续递增（不是每次重启都重新从 1 开始）。
    std::uint64_t third_token = 0;
    {
        Coordinator coordinator(MakeOptions(path));
        third_token = AcquireToken(coordinator, "lock/order", "node-c");
        GO2CPP_CHECK(third_token > new_token);
    }
    std::printf("second restart fencing: prev=%llu new=%llu\n",
                static_cast<unsigned long long>(new_token),
                static_cast<unsigned long long>(third_token));

    // 不同锁 key 的 token 互不影响。
    {
        Coordinator coordinator(MakeOptions(path));
        const auto other = AcquireToken(coordinator, "lock/other", "node-d");
        GO2CPP_CHECK(other == 1);
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

void TestEpochRestored() {
    go2cpp_tests::announce("coordinator_snapshot.election_epoch");
    const std::string dir = MakeTempDir("go2cpp_coord_epoch");
    const std::string path = dir + "/coordinator.snapshot";

    std::uint64_t old_epoch = 0;
    {
        Coordinator coordinator(MakeOptions(path));
        const auto joined =
            coordinator.JoinElection("election/primary", "node-a", Duration(60000));
        GO2CPP_CHECK(joined.ok());
        GO2CPP_CHECK(joined.value.epoch == 1);
        GO2CPP_CHECK(joined.value.leader);
        old_epoch = joined.value.epoch;
    }
    {
        Coordinator coordinator(MakeOptions(path));
        GO2CPP_CHECK(coordinator.healthy());
        const auto state = coordinator.QueryElection("election/primary");
        GO2CPP_CHECK(state.epoch == old_epoch);
        // 新 leader 上任必须开出更大的任期（旧 epoch 永不复用）。
        const auto joined =
            coordinator.JoinElection("election/primary", "node-b", Duration(60000));
        GO2CPP_CHECK(joined.ok());
        GO2CPP_CHECK(joined.value.epoch > old_epoch);
        GO2CPP_CHECK(joined.value.leader);
    }
    std::printf("election epoch: old=%llu (new epoch checked > old)\n",
                static_cast<unsigned long long>(old_epoch));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

void TestMissingSnapshotStartsFresh() {
    go2cpp_tests::announce("coordinator_snapshot.missing_file");
    const std::string dir = MakeTempDir("go2cpp_coord_missing");
    const std::string path = dir + "/coordinator.snapshot";
    {
        Coordinator coordinator(MakeOptions(path));
        GO2CPP_CHECK(coordinator.healthy());
        GO2CPP_CHECK(coordinator.load_error().empty());
        GO2CPP_CHECK(AcquireToken(coordinator, "lock/new", "node-a") == 1);
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

void TestTruncatedSnapshotRejected() {
    go2cpp_tests::announce("coordinator_snapshot.truncated_rejected");
    const std::string dir = MakeTempDir("go2cpp_coord_trunc");
    const std::string path = dir + "/coordinator.snapshot";
    {
        Coordinator coordinator(MakeOptions(path));
        GO2CPP_CHECK(AcquireToken(coordinator, "lock/x", "node-a") == 1);
        GO2CPP_CHECK(coordinator.Snapshot().ok());
    }
    const auto size = std::filesystem::file_size(path);
    GO2CPP_CHECK(size > 20);
    std::filesystem::resize_file(path, size / 2);  // 截断一半。

    Coordinator coordinator(MakeOptions(path));
    GO2CPP_CHECK(!coordinator.healthy());
    GO2CPP_CHECK(!coordinator.load_error().empty());
    // fail-stop：不允许静默回退到「fencing 从 0 开始」。
    const auto acquired = coordinator.AcquireLock("lock/x", "node-b", Duration(60000));
    GO2CPP_CHECK(!acquired.ok());
    GO2CPP_CHECK(acquired.status == Status::kUnavailable);
    std::printf("truncated snapshot rejected: %s\n", coordinator.load_error().c_str());

    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

void TestCorruptedCrcRejected() {
    go2cpp_tests::announce("coordinator_snapshot.crc_rejected");
    const std::string dir = MakeTempDir("go2cpp_coord_crc");
    const std::string path = dir + "/coordinator.snapshot";
    {
        Coordinator coordinator(MakeOptions(path));
        GO2CPP_CHECK(AcquireToken(coordinator, "lock/y", "node-a") == 1);
        GO2CPP_CHECK(coordinator.Snapshot().ok());
    }
    {
        // 翻转载荷中间的一个字节（保持文件长度不变，只有 CRC 能发现）。
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekg(0, std::ios::end);
        const auto size = static_cast<std::streamoff>(file.tellg());
        file.seekp(size / 2);
        char byte = 0;
        file.seekg(size / 2);
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5A);
        file.seekp(size / 2);
        file.write(&byte, 1);
    }
    Coordinator coordinator(MakeOptions(path));
    GO2CPP_CHECK(!coordinator.healthy());
    GO2CPP_CHECK(coordinator.load_error().find("crc") != std::string::npos);
    const auto joined = coordinator.JoinElection("election/x", "node-b", Duration(60000));
    GO2CPP_CHECK(joined.status == Status::kUnavailable);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.coordinator_snapshot");
    TestRestartKeepsFencing();
    TestEpochRestored();
    TestMissingSnapshotStartsFresh();
    TestTruncatedSnapshotRejected();
    TestCorruptedCrcRejected();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_coordinator_snapshot_test: %d checks failed\n",
                     failures);
        return 1;
    }
    std::printf("distributed_coordinator_snapshot_test: all cases passed\n");
    return 0;
}
