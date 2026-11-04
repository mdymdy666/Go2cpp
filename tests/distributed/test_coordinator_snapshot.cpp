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

/**
 * @brief 残留 `<path>.tmp`（写完 tmp、rename 前掉电）不能影响恢复。
 *
 * 恢复只认正式路径；tmp 不是「更新的快照」，也不是「损坏的快照」——
 * 误把 tmp 当快照会让 fencing 回退到未提交的状态。
 */
void TestLeftoverTempFileIgnored() {
    go2cpp_tests::announce("coordinator_snapshot.temp_file_ignored");
    const std::string dir = MakeTempDir("go2cpp_snapshot_tmp");
    const std::string path = dir + "/coordinator.snapshot";
    std::uint64_t token = 0;
    {
        Coordinator coordinator(MakeOptions(path));
        token = AcquireToken(coordinator, "lock/tmp", "node-a");
        GO2CPP_CHECK(token > 0);
    }
    // 模拟掉电残骸：一半的新快照内容留在 tmp 里。
    {
        std::ofstream tmp(path + ".tmp", std::ios::binary | std::ios::trunc);
        tmp.write("G2CSNP01", 8);
        tmp.put('\0');
        tmp.put('\x01');
    }
    Coordinator restored(MakeOptions(path));
    GO2CPP_CHECK(restored.healthy());
    GO2CPP_CHECK(restored.load_error().empty());
    const std::uint64_t next = AcquireToken(restored, "lock/tmp", "node-b");
    GO2CPP_CHECK(next > token);
}

/**
 * @brief 快照写失败时：返回 kUnavailable、旧快照仍可恢复、token 不回退。
 *
 * 把 tmp 路径变成目录即可让 `open(tmp, O_CREAT)` 稳定失败（EISDIR），
 * 不需要制造真实磁盘满，也不破坏正式路径上的上一份快照。
 */
void TestWriteFailureKeepsPreviousSnapshot() {
    go2cpp_tests::announce("coordinator_snapshot.write_failure_keeps_previous");
    const std::string dir = MakeTempDir("go2cpp_snapshot_writefail");
    const std::string path = dir + "/coordinator.snapshot";
    std::uint64_t token = 0;
    {
        Coordinator coordinator(MakeOptions(path));
        token = AcquireToken(coordinator, "lock/fail", "node-a");
        GO2CPP_CHECK(token > 0);
        // 让 tmp 路径不可写：open 必然失败。
        std::error_code ignored;
        GO2CPP_CHECK(std::filesystem::create_directory(path + ".tmp", ignored));
        const auto failed = coordinator.Snapshot();
        GO2CPP_CHECK(!failed.ok());
        GO2CPP_CHECK(failed.status == Status::kUnavailable);
        GO2CPP_CHECK(!coordinator.persist_error().empty());
        GO2CPP_CHECK(std::filesystem::remove(path + ".tmp", ignored));
    }
    // 磁盘上仍是上一份完整快照：能加载，且新 token 严格大于旧 token。
    Coordinator restored(MakeOptions(path));
    GO2CPP_CHECK(restored.healthy());
    const std::uint64_t next = AcquireToken(restored, "lock/fail", "node-b");
    GO2CPP_CHECK(next > token);
}


/**
 * @brief 落盘失败时必须回滚内存授予（不能「报错却占着锁」）。
 *
 * 旧实现先授予锁再落盘：写盘失败返回 `kUnavailable`，但锁仍在内存里，
 * 调用方重试会拿到 `kConflict`——被自己挡一个 TTL。这里把 tmp 路径变成
 * 目录，让 `open(tmp, O_CREAT)` 稳定失败（EISDIR），不需要制造磁盘满。
 */
void TestAcquireLockRollbackOnPersistFailure() {
    go2cpp_tests::announce("coordinator_snapshot.acquire_rollback");
    const std::string dir = MakeTempDir("go2cpp_coord_rollback");
    const std::string path = dir + "/coordinator.snapshot";
    Coordinator coordinator(MakeOptions(path));
    std::error_code ignored;
    GO2CPP_CHECK(std::filesystem::create_directory(path + ".tmp", ignored));

    const auto failed = coordinator.AcquireLock("lock/rollback", "node-a", Duration(60000));
    GO2CPP_CHECK(!failed.ok());
    GO2CPP_CHECK(failed.status == Status::kUnavailable);
    // 关键断言：内存里不能留下这把锁。
    GO2CPP_CHECK(!coordinator.QueryLock("lock/rollback").has_value());

    // 重试仍然落盘失败（kUnavailable），但不能是被自己挡住的 kConflict。
    const auto retry = coordinator.AcquireLock("lock/rollback", "node-b", Duration(60000));
    GO2CPP_CHECK(retry.status == Status::kUnavailable);
    GO2CPP_CHECK(!coordinator.QueryLock("lock/rollback").has_value());

    // 修复落盘路径后重新抢锁：必须成功，token 跳过两次被回滚的授予（单调）。
    GO2CPP_CHECK(std::filesystem::remove(path + ".tmp", ignored));
    const auto recovered = coordinator.AcquireLock("lock/rollback", "node-c", Duration(60000));
    GO2CPP_REQUIRE(recovered.ok());
    GO2CPP_CHECK(recovered.value.fencing >= 3);
    GO2CPP_CHECK(coordinator.QueryLock("lock/rollback")->owner == "node-c");

    std::filesystem::remove_all(dir, ignored);
}

/**
 * @brief `Reload()` 是坏快照修复后的恢复路径：成功必须清除降级并恢复写。
 */
void TestReloadClearsDegraded() {
    go2cpp_tests::announce("coordinator_snapshot.reload_recovers");
    const std::string dir = MakeTempDir("go2cpp_coord_reload");
    const std::string path = dir + "/coordinator.snapshot";
    const std::string backup = dir + "/backup.snapshot";
    std::uint64_t token = 0;
    {
        Coordinator coordinator(MakeOptions(path));
        token = AcquireToken(coordinator, "lock/reload", "node-a");
        GO2CPP_CHECK(token == 1);
    }
    std::error_code error;
    std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing,
                               error);
    GO2CPP_CHECK(!error);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2, error);
    GO2CPP_CHECK(!error);

    Coordinator degraded(MakeOptions(path));
    GO2CPP_CHECK(!degraded.healthy());
    GO2CPP_CHECK(!degraded.load_error().empty());
    GO2CPP_CHECK(degraded.AcquireLock("lock/reload", "node-b", Duration(60000)).status ==
                 Status::kUnavailable);

    // 运维修复文件后 Reload：解除降级、写操作恢复可用，且 fencing 不回退。
    std::filesystem::copy_file(backup, path, std::filesystem::copy_options::overwrite_existing,
                               error);
    GO2CPP_CHECK(!error);
    GO2CPP_CHECK(degraded.Reload().ok());
    GO2CPP_CHECK(degraded.healthy());
    GO2CPP_CHECK(degraded.load_error().empty());
    const std::uint64_t next = AcquireToken(degraded, "lock/reload", "node-c");
    GO2CPP_CHECK(next > token);
    // 幂等：再次 Reload 仍然成功，不是一次性开关。
    GO2CPP_CHECK(degraded.Reload().ok());
    GO2CPP_CHECK(degraded.healthy());

    std::filesystem::remove_all(dir, error);
}

/**
 * @brief 清扫删除租约必须合并落盘，而不是每轮（10ms）全量写盘。
 *
 * 把 `sweep_persist_interval` 设成 1 小时：窗口内无论删多少租约都不应写盘，
 * 文件 mtime 保持不变；显式 `Snapshot()` 才把累积删除一次提交。
 */
void TestSweepPersistCoalescing() {
    go2cpp_tests::announce("coordinator_snapshot.sweep_persist_coalesced");
    const std::string dir = MakeTempDir("go2cpp_coord_sweep_batch");
    const std::string path = dir + "/coordinator.snapshot";
    CoordinatorOptions options = MakeOptions(path);
    options.sweep_persist_interval = Duration(3600000);
    Coordinator coordinator(options);
    for (int index = 0; index < 3; ++index) {
        GO2CPP_REQUIRE(coordinator
                           .AcquireLock("lock/sweep-" + std::to_string(index), "node",
                                        Duration(10))
                           .ok());
    }
    const auto mtime_before = std::filesystem::last_write_time(path);
    ::usleep(40000);
    GO2CPP_CHECK(coordinator.SweepExpired() == 3);
    // 窗口内不写盘：这是「批量」的直接证据。
    GO2CPP_CHECK(std::filesystem::last_write_time(path) == mtime_before);
    GO2CPP_CHECK(coordinator.SweepExpired() == 0);
    GO2CPP_CHECK(std::filesystem::last_write_time(path) == mtime_before);
    // 显式快照把累积删除一次提交（并清空 pending）。
    GO2CPP_CHECK(coordinator.Snapshot().ok());
    GO2CPP_CHECK(std::filesystem::last_write_time(path) != mtime_before);

    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

/**
 * @brief 超过恢复侧上限（64 MiB）的快照文件必须被拒绝启动，而不是 OOM/截断。
 */
void TestOversizedSnapshotRejected() {
    go2cpp_tests::announce("coordinator_snapshot.oversized_rejected");
    const std::string dir = MakeTempDir("go2cpp_snapshot_oversize");
    const std::string path = dir + "/coordinator.snapshot";
    {
        std::ofstream create(path, std::ios::binary | std::ios::trunc);
        create.put('\0');
    }
    std::error_code error;
    std::filesystem::resize_file(path, (std::uint64_t{64} << 20) + 1, error);
    GO2CPP_CHECK(!error);
    Coordinator coordinator(MakeOptions(path));
    GO2CPP_CHECK(!coordinator.healthy());
    GO2CPP_CHECK(coordinator.load_error().find("size") != std::string::npos);
    // fail-stop：写操作一律 kUnavailable（不回退到空表）。
    const auto acquired = coordinator.AcquireLock("lock/big", "node", Duration(60000));
    GO2CPP_CHECK(acquired.status == Status::kUnavailable);
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
    TestLeftoverTempFileIgnored();
    TestWriteFailureKeepsPreviousSnapshot();
    TestAcquireLockRollbackOnPersistFailure();
    TestReloadClearsDegraded();
    TestSweepPersistCoalescing();
    TestOversizedSnapshotRejected();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_coordinator_snapshot_test: %d checks failed\n",
                     failures);
        return 1;
    }
    std::printf("distributed_coordinator_snapshot_test: all cases passed\n");
    return 0;
}
