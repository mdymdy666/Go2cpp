// Copyright 2026 Go2Cpp Authors. All rights reserved.
/**
 * @file 02-read-miss.cpp
 * @brief 真实场景：读未命中——缓存穿透 / 记录不存在。
 *
 * ## 场景
 * 商品详情页按 id 读缓存：热点 key 失效后大量请求同时打到存储；或者客户端
 * 拿了一个不存在的 id 反复请求。若把「未命中」和「读失败」混为一谈，
 * 就会把空值当正常数据返回、或用旧值掩盖错误。
 *
 * ## 默认行为（file:line）
 * - `storage::KV::Get()` 未命中返回 **false**，`value` **保持原值不被修改**
 *   （include/go2cpp/storage/kv.hpp:136；实现见 src/storage/kv.cpp:769-821）；
 * - 未命中**不是错误**：`LastError()` 保持空（错误才写 LastError，kv.cpp:485-487）；
 * - `MemCache::Get()` 同样未命中返回 false（include/go2cpp/storage/memcache.hpp:80-81），
 *   命中会提升 LRU；`MemCacheStats::misses/hits` 可观测（memcache.hpp:60-62）；
 * - 注意：`KV::Get` 的 bool **不区分**「key 不存在」与「底层读失败」——需要区分时
 *   只能看 `LastError()`，或在业务层用显式状态码包装。
 *
 * ## 可选策略
 * 1. **回源**：未命中 → 查上游（DB/HTTP）→ 回填 KV + 缓存；
 * 2. **缓存**：L1 `MemCache`（TTL/LRU）+ L2 `KV` 两层；空结果也可以短暂缓存
 *    （「缓存空值」防穿透，本示例只说明不默认做）；
 * 3. **报错**：必须存在的记录（token、订单）未命中 → 明确返回 404/错误码，
 *    不要把空字符串交给下游。
 *
 * ## 如何选择
 * - 读多写少 + 可容忍短暂过期 → 回源 + 缓存（cache-aside，本示例）；
 * - 热点 key 并发回源（cache stampede）→ 加 single-flight/互斥，只放一个请求回源；
 * - 记录「不存在」是正常业务分支 → 返回 kNotFound（404），并考虑缓存空值；
 * - 底层读失败（LastError 非空）→ 返回 kError（5xx），**不要**降级成 404。
 *
 * 本示例真跑：真实 KV + 真实 MemCache + 模拟上游，验证 miss/hit/回源/404。
 */
#include "go2cpp/storage/kv.hpp"
#include "go2cpp/storage/memcache.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace {

/// @brief 临时目录 RAII（与 01 一致，析构自动清理）。
class TempDir final {
public:
    TempDir() {
        char pattern[] = "/tmp/go2cpp_read_miss_XXXXXX";
        if (char* created = ::mkdtemp(pattern); created != nullptr) {
            path_ = created;
        }
    }
    ~TempDir() {
        if (path_.empty()) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

/// @brief 业务加载状态：把「未命中」与「读失败」分开，是可选策略「报错」的前提。
enum class LoadStatus { kOk, kNotFound, kError };

struct LoadResult {
    LoadStatus status{LoadStatus::kNotFound};
    std::string value;
    std::string source;  ///< cache / kv / upstream，便于观测命中路径。
};

/// @brief 模拟上游商品库（真实项目是 DB 或另一个 HTTP 服务）。
class Upstream final {
public:
    Upstream() {
        rows_["product:1"] = R"({"id":1,"name":"机械键盘"})";
        rows_["product:2"] = R"({"id":2,"name":"显示器"})";
    }

    LoadStatus Fetch(const std::string& key, std::string* value) {
        ++queries;
        const auto it = rows_.find(key);
        if (it == rows_.end()) {
            return LoadStatus::kNotFound;
        }
        *value = it->second;
        return LoadStatus::kOk;
    }

    std::uint64_t queries = 0;

private:
    std::map<std::string, std::string> rows_;
};

/// @brief cache-aside：L1 MemCache -> L2 KV -> 上游；miss 才回源并回填。
LoadResult LoadProduct(go2cpp::storage::KV* db, go2cpp::storage::MemCache* cache,
                       Upstream* upstream, const std::string& key) {
    std::string value;

    // ① L1：进程内缓存（命中会提升 LRU）。
    if (cache != nullptr && cache->Get(key, &value)) {
        return {LoadStatus::kOk, value, "cache"};
    }

    // ② L2：KV。未命中时 value 保持原值（这里是空串），LastError 仍为空。
    if (db->Get(key, &value)) {
        if (cache != nullptr) {
            cache->Set(key, value);  // 回填 L1，下一次直接命中。
        }
        return {LoadStatus::kOk, value, "kv"};
    }
    // 关键：未命中 ≠ 读失败。只有 LastError 非空才是底层错误。
    if (!db->LastError().empty()) {
        return {LoadStatus::kError, "", "kv-error"};
    }

    // ③ 回源：上游没有就明确 kNotFound（调用方回 404）。
    const LoadStatus status = upstream->Fetch(key, &value);
    if (status != LoadStatus::kOk) {
        return {status, "", "upstream"};
    }
    if (cache != nullptr) {
        cache->Set(key, value);
    }
    db->Put(key, value);  // 写穿 L2，下一次重启后仍能命中。
    return {LoadStatus::kOk, value, "upstream"};
}

const char* StatusName(LoadStatus status) noexcept {
    switch (status) {
        case LoadStatus::kOk: return "ok";
        case LoadStatus::kNotFound: return "not_found";
        case LoadStatus::kError: return "error";
    }
    return "unknown";
}

}  // namespace

int main() {
    TempDir dir;
    if (dir.path().empty()) {
        std::cerr << "[read-miss] mkdtemp 失败\n";
        return 1;
    }
    auto db = go2cpp::storage::KV::Open(dir.path());
    if (db == nullptr) {
        std::cerr << "[read-miss] KV::Open 失败\n";
        return 1;
    }
    bool ok = true;

    // 预置 L2 数据（模拟历史已缓存的数据）。
    db->Put("product:1", R"({"id":1,"name":"机械键盘"})");

    // ① 默认未命中：false + value 不变 + LastError 空。
    std::string sentinel = "sentinel";
    const bool found = db->Get("product:404", &sentinel);
    std::cout << "[默认] found=" << found << " value=\"" << sentinel
              << "\" last_error=\"" << db->LastError()
              << "\"（预期 0 / sentinel / 空：未命中不是错误）\n";
    ok = ok && !found && sentinel == "sentinel" && db->LastError().empty();

    // ② 可选策略：L1 缓存 + L2 KV + 回源。
    auto cache = go2cpp::storage::MemCache::Open(
        nullptr, go2cpp::storage::MemCacheOptions{});
    Upstream upstream;
    if (cache == nullptr) {
        std::cerr << "[read-miss] MemCache::Open 失败\n";
        return 1;
    }

    const LoadResult first = LoadProduct(db.get(), cache.get(), &upstream, "product:1");
    const LoadResult second = LoadProduct(db.get(), cache.get(), &upstream, "product:1");
    std::cout << "[回填] product:1 第一次 source=" << first.source
              << " 第二次 source=" << second.source
              << "（预期 kv / cache）\n";
    ok = ok && first.status == LoadStatus::kOk && first.source == "kv" &&
         second.status == LoadStatus::kOk && second.source == "cache";

    // ③ 回源：L2 也没有 → 上游 → 回填 L2+L1，第二次命中缓存。
    const LoadResult third = LoadProduct(db.get(), cache.get(), &upstream, "product:2");
    const LoadResult fourth = LoadProduct(db.get(), cache.get(), &upstream, "product:2");
    std::cout << "[回源] product:2 第一次 source=" << third.source
              << " 第二次 source=" << fourth.source
              << "（预期 upstream / cache）\n";
    ok = ok && third.status == LoadStatus::kOk && third.source == "upstream" &&
         fourth.source == "cache";

    // ④ 可选策略：报错——上游也没有，明确 not_found（映射 404），不是空值。
    const LoadResult missing =
        LoadProduct(db.get(), cache.get(), &upstream, "product:404");
    std::cout << "[报错] product:404 status=" << StatusName(missing.status)
              << " value=\"" << missing.value
              << "\"（预期 not_found / 空；调用方应回 404 而不是 200）\n";
    ok = ok && missing.status == LoadStatus::kNotFound && missing.value.empty();

    // ⑤ 观测：缓存命中/未命中计数（生产环境上报 metrics）。
    const auto stats = cache->GetStats();
    std::cout << "[统计] cache hits=" << stats.hits << " misses=" << stats.misses
              << " upstream_queries=" << upstream.queries
              << "（预期 hits=2 misses=3 queries=2）\n";
    ok = ok && stats.hits == 2 && stats.misses == 3 && upstream.queries == 2;

    db->Close();
    std::cout << (ok ? "[read-miss] 通过\n" : "[read-miss] 失败\n");
    return ok ? 0 : 1;
}
