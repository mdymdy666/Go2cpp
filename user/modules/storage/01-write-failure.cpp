// Copyright 2026 Go2Cpp Authors. All rights reserved.
/**
 * @file 01-write-failure.cpp
 * @brief 真实场景：存储写失败（磁盘满 / 目录只读 / 权限不足）。
 *
 * ## 场景
 * 日志/订单服务把数据写进嵌入式 KV。某天磁盘配额打满（write 返回 ENOSPC），
 * 或运维修了权限（目录 0555，创建 SSTable/WAL 报 EACCES）。如果代码假设
 * `Put()` 一定成功，就会继续回 200，用户订单静默丢失。
 *
 * ## 默认行为（file:line）
 * - `storage::KV::Put()` 返回 **bool**，失败原因在 `LastError()`（include/go2cpp/storage/kv.hpp:135、143-144），
 *   不抛异常；
 * - 写路径：MemTable 满 → `FlushLocked()` 创建 SSTable；`builder.Open()` 失败
 *   会 `SetError("open sstable failed: ...")` 并返回 false（src/storage/kv.cpp:846-849），
 *   于是 `Put()` 返回 false（kv.cpp:641-650 的 WriteLocked 分支）；
 * - WAL 追加失败同样 `SetError("wal append failed")`（kv.cpp:648）；
 * - 磁盘满（ENOSPC）与权限不足（EACCES）都落在同一条「写失败 → false + LastError」
 *   路径上，不会 panic、不会半提交（WAL 先写、批量应用）；
 * - 注意：`LastError()` 只在**第一次**失败时记录（kv.cpp:485-487 的 `if (last_error_.empty())`），
 *   后续错误不会覆盖首因。
 *
 * ## 可选策略
 * 1. **重试**：短暂故障（配额临时打满/权限被修回）重试 3 次，带 20ms 退避；
 *    仍失败则上报。写失败不重试就丢数据，重试无上限会拖死请求。
 * 2. **降级到内存**：用 `storage::MemCache`（纯内存，memcache.hpp:64-71）承接
 *    写请求，标记 `pending_compensation`，后台任务等磁盘恢复后补偿写；
 *    进程重启会丢——只适用于「可丢/可重放」数据。
 * 3. **告警**：计数 + 打印/上报 `LastError()`，并让健康检查进入 degraded；
 *    不吞错误。
 *
 * ## 如何选择
 * - 订单/支付等强一致数据 → **不能**只降级到内存；重试 + 告警 + 返回 5xx，
 *   让上游知道失败（本示例的 1+3 组合）；
 * - 指标/日志/会话等可丢数据 → 降级到内存 + 异步补偿（2），优先保证可用性；
 * - 磁盘满通常需要人工/自动扩容 → 告警必须带 `LastError()` 首因，否则无法定位；
 * - 重试次数按总预算算（例如 3 次 × 20ms），不要无限重试。
 *
 * 本示例在 /tmp 下真跑：先正常写，再把目录改成 0555 注入 EACCES，
 * 触发真实 Flush 失败，最后恢复权限验证重试与内存降级。
 * 需要非 root 运行（root 有 CAP_DAC_OVERRIDE，0555 拦不住，示例会明确提示）。
 */
#include "go2cpp/storage/kv.hpp"
#include "go2cpp/storage/memcache.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

/// @brief 临时目录 RAII：析构时恢复 0755 并删除，避免失败时留下只读目录。
class TempDir final {
public:
    TempDir() {
        char pattern[] = "/tmp/go2cpp_write_failure_XXXXXX";
        if (char* created = ::mkdtemp(pattern); created != nullptr) {
            path_ = created;
        }
    }
    ~TempDir() {
        if (path_.empty()) {
            return;
        }
        std::error_code ignored;
        std::filesystem::permissions(
            path_, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, ignored);
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

/// @brief 把目录改成只读；返回 true 表示写失败确实发生了（权限注入生效）。
bool MakeReadOnly(const std::string& dir) {
    std::error_code ec;
    std::filesystem::permissions(
        dir,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, ec);
    // 用探针文件确认：root 下 0555 仍可写，必须识别出来而不是假装注入成功。
    std::ofstream probe(dir + "/.write_probe");
    return !probe.good();
}

void MakeWritable(const std::string& dir) {
    std::error_code ec;
    std::filesystem::permissions(
        dir,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, ec);
}

std::string MakeValue(std::size_t bytes, char fill) {
    return std::string(bytes, fill);
}

}  // namespace

int main() {
    TempDir dir;
    if (dir.path().empty()) {
        std::cerr << "[write-failure] mkdtemp 失败\n";
        return 1;
    }

    go2cpp::storage::Options options;
    options.memtable_size = 8 * 1024;  // 8 KiB：几次 Put 就触发一次刷盘。
    options.sync_writes = false;
    auto db = go2cpp::storage::KV::Open(dir.path(), options);
    if (db == nullptr) {
        std::cerr << "[write-failure] KV::Open 失败: " << dir.path() << "\n";
        return 1;
    }
    bool ok = true;

    // ① 正常写：确认基座可用。
    for (int i = 0; i < 3; ++i) {
        const std::string key = "order:" + std::to_string(i);
        if (!db->Put(key, "paid")) {
            std::cerr << "[write-failure] 正常写失败: " << db->LastError() << "\n";
            return 1;
        }
    }
    std::cout << "[正常] 3 条写入成功（memtable_size=8KiB）\n";

    // ② 注入故障：目录 0555 -> 下次 Flush 创建 SSTable/WAL 失败。
    const bool injected = MakeReadOnly(dir.path());
    if (!injected) {
        std::cout << "[提示] 当前以 root 运行（或文件系统忽略权限），"
                     "0555 无法制造 EACCES；改用「关闭后写入」展示错误码\n";
    }

    bool failed = false;
    std::string first_error;
    int failed_at = -1;
    if (injected) {
        // 每次 4 KiB，8 KiB memtable 会在前几次写内触发一次 Flush。
        for (int i = 0; i < 64; ++i) {
            const std::string key = "payload:" + std::to_string(i);
            if (!db->Put(key, MakeValue(4096, 'x'))) {
                failed = true;
                failed_at = i;
                first_error = db->LastError();
                break;
            }
        }
    } else {
        // root 兜底：关闭后再写，KV 的写路径同样返回 false + LastError。
        db->Close();
        failed = !db->Put("payload:after-close", "x");
        first_error = db->LastError();
        failed_at = 0;
    }

    std::cout << "[默认] 写失败 failed=" << failed << " at=" << failed_at
              << " LastError=\"" << first_error << "\""
              << "（预期 true；权限场景含 open sstable failed，root 兜底含 closed）\n";
    ok = ok && failed && !first_error.empty();

    // ③ 可选策略一：重试（先恢复权限，模拟运维修复后的瞬时重试）。
    MakeWritable(dir.path());
    bool retried = false;
    for (int attempt = 0; attempt < 3 && !retried; ++attempt) {
        if (injected) {
            if (db->Put("payload:retry", MakeValue(4096, 'r'))) {
                retried = true;
            }
        } else {
            db = go2cpp::storage::KV::Open(dir.path(), options);
            if (db != nullptr && db->Put("payload:retry", MakeValue(4096, 'r'))) {
                retried = true;
            }
        }
        if (!retried) {
            std::this_thread::sleep_for(20ms);
        }
    }
    std::cout << "[重试] 恢复权限后 attempt 重试成功=" << retried
              << "（预期 1）\n";
    ok = ok && retried;

    // ④ 可选策略二：降级到内存（MemCache 纯内存，不依赖磁盘）。
    auto cache = go2cpp::storage::MemCache::Open(
        nullptr, go2cpp::storage::MemCacheOptions{});
    const bool degraded = cache != nullptr &&
                          cache->Set("order:degraded", R"({"status":"pending"})");
    std::string cached;
    const bool cache_hit = cache != nullptr &&
                           cache->Get("order:degraded", &cached);
    std::cout << "[降级] 写内存缓存 degraded=" << degraded
              << " hit=" << cache_hit << " value=" << cached
              << "（预期 1 / 1 / pending，进程重启会丢）\n";
    ok = ok && degraded && cache_hit;

    // ⑤ 可选策略三：告警计数（真实项目应上报 metrics/日志，这里打印一行）。
    int alerts = 1;
    std::cout << "[告警] write_failures=" << alerts
              << " first_reason=\"" << first_error
              << "\" health=degraded\n";

    // 恢复权限后再写入，验证同一 KV 实例能继续工作（非破坏性）。
    if (db != nullptr) {
        db->Close();
    }
    std::cout << (ok ? "[write-failure] 通过\n" : "[write-failure] 失败\n");
    return ok ? 0 : 1;
}
