/**
 * @file test_metadata_store.cpp
 * @brief `MetadataStore`：KV 往返、revision 单调、Watch 顺序、WAL 恢复与损坏拒绝。
 *
 * 覆盖点（对应 P1 验收）：
 * - Put/Get/Delete/List 的 KV 语义与 revision 严格递增；
 * - Watch 前缀过滤 + 按 revision 顺序投递（异步派发 + `Flush` 排空）；
 * - 回调抛异常不破坏 store（后续写仍然成功、错误被计数）；
 * - `Replay` 从历史 revision 补发；
 * - WAL 重启恢复（revision 续接）；
 * - WAL 尾部截断可恢复、中间损坏拒绝打开。
 */

#include "go2cpp/distributed/metadata_store.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using go2cpp::distributed::MetadataEvent;
using go2cpp::distributed::MetadataStore;
using go2cpp::distributed::MetadataStoreOptions;

std::string MakeTempDir(const std::string &name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     (name + "_" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir.string();
}

void TestKvRoundTrip() {
    go2cpp_tests::announce("metadata_store.kv_roundtrip");
    auto opened = MetadataStore::OpenMemory();
    GO2CPP_CHECK(opened.ok());
    auto store = std::move(opened.value);

    std::uint64_t first = 0;
    GO2CPP_CHECK(store->Put("a", "1", &first).ok());
    GO2CPP_CHECK(first == 1);
    auto got = store->Get("a");
    GO2CPP_CHECK(got.ok());
    GO2CPP_CHECK(got.value.value == "1");
    GO2CPP_CHECK(got.value.revision == 1);

    std::uint64_t second = 0;
    GO2CPP_CHECK(store->Put("a", "2", &second).ok());
    GO2CPP_CHECK(second == 2);
    got = store->Get("a");
    GO2CPP_CHECK(got.ok() && got.value.value == "2" && got.value.revision == 2);

    std::uint64_t deleted = 0;
    GO2CPP_CHECK(store->Delete("a", &deleted).ok());
    GO2CPP_CHECK(deleted == 3);
    GO2CPP_CHECK(!store->Get("a").ok());

    // 删除不存在的 key 是幂等的，但同样消耗一个 revision（删除屏障语义）。
    std::uint64_t again = 0;
    GO2CPP_CHECK(store->Delete("a", &again).ok());
    GO2CPP_CHECK(again == 4);
    GO2CPP_CHECK(store->Revision() == 4);
}

void TestRevisionMonotonic() {
    go2cpp_tests::announce("metadata_store.revision_monotonic");
    auto opened = MetadataStore::OpenMemory();
    GO2CPP_CHECK(opened.ok());
    auto store = std::move(opened.value);

    std::uint64_t previous = 0;
    for (int index = 0; index < 100; ++index) {
        std::uint64_t revision = 0;
        GO2CPP_CHECK(store->Put("k" + std::to_string(index), "v", &revision).ok());
        GO2CPP_CHECK(revision == previous + 1);
        previous = revision;
    }
    GO2CPP_CHECK(store->Revision() == 100);

    // 前缀 List：字典序、只含匹配项。
    GO2CPP_CHECK(store->Put("cfg/a", "1").ok());
    GO2CPP_CHECK(store->Put("cfg/b", "2").ok());
    GO2CPP_CHECK(store->Put("other/c", "3").ok());
    const auto listed = store->List("cfg/");
    GO2CPP_CHECK(listed.size() == 2);
    GO2CPP_CHECK(listed[0].key == "cfg/a" && listed[1].key == "cfg/b");
    GO2CPP_CHECK(listed[0].revision == 101 && listed[1].revision == 102);
}

void TestWatchOrderAndPrefix() {
    go2cpp_tests::announce("metadata_store.watch_order_prefix");
    auto opened = MetadataStore::OpenMemory();
    GO2CPP_CHECK(opened.ok());
    auto store = std::move(opened.value);

    std::mutex mutex;
    std::vector<MetadataEvent> events;
    const auto id = store->Watch("cfg/", [&](const MetadataEvent &event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    });
    GO2CPP_CHECK(id != 0);

    GO2CPP_CHECK(store->Put("cfg/a", "1").ok());
    GO2CPP_CHECK(store->Put("cfg/b", "2").ok());
    GO2CPP_CHECK(store->Put("other/c", "3").ok());  // 前缀不匹配，不应投递。
    GO2CPP_CHECK(store->Delete("cfg/a").ok());
    GO2CPP_CHECK(store->Flush());

    std::lock_guard<std::mutex> lock(mutex);
    GO2CPP_CHECK(events.size() == 3);
    if (events.size() == 3) {
        GO2CPP_CHECK(events[0].key == "cfg/a" && events[0].value == "1");
        GO2CPP_CHECK(events[1].key == "cfg/b" && events[1].value == "2");
        GO2CPP_CHECK(events[2].key == "cfg/a" && events[2].deleted);
        GO2CPP_CHECK(events[0].revision < events[1].revision);
        GO2CPP_CHECK(events[1].revision < events[2].revision);
    }
    GO2CPP_CHECK(store->Unwatch(id));
    GO2CPP_CHECK(!store->Unwatch(id));
}

void TestCallbackExceptionIsolated() {
    go2cpp_tests::announce("metadata_store.callback_exception");
    auto opened = MetadataStore::OpenMemory();
    GO2CPP_CHECK(opened.ok());
    auto store = std::move(opened.value);

    const auto id = store->Watch("", [](const MetadataEvent &) {
        throw std::runtime_error("callback boom");
    });
    GO2CPP_CHECK(id != 0);
    // 回调抛异常不能传播到写路径，更不能破坏 store。
    GO2CPP_CHECK(store->Put("x", "1").ok());
    GO2CPP_CHECK(store->Put("y", "2").ok());
    GO2CPP_CHECK(store->Flush());
    GO2CPP_CHECK(store->Flush());
    GO2CPP_CHECK(store->Get("x").ok());
    GO2CPP_CHECK(store->Get("y").ok());
    const auto stats = store->GetStats();
    GO2CPP_CHECK(stats.callback_errors >= 2);
    GO2CPP_CHECK(stats.last_revision == 2);
}

void TestReplay() {
    go2cpp_tests::announce("metadata_store.replay");
    MetadataStoreOptions options;
    options.max_events = 64;
    auto opened = MetadataStore::Open(options);
    GO2CPP_CHECK(opened.ok());
    auto store = std::move(opened.value);

    GO2CPP_CHECK(store->Put("cfg/a", "1").ok());
    GO2CPP_CHECK(store->Put("cfg/b", "2").ok());
    GO2CPP_CHECK(store->Put("cfg/c", "3").ok());

    std::mutex mutex;
    std::vector<MetadataEvent> events;
    // 订阅注册在写之后：只收新事件；历史用 Replay 补。
    const auto id = store->Watch("cfg/", [&](const MetadataEvent &event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    });
    GO2CPP_CHECK(id != 0);
    GO2CPP_CHECK(store->Replay(id, 2).ok());
    {
        std::lock_guard<std::mutex> lock(mutex);
        GO2CPP_CHECK(events.size() == 2);
        if (events.size() == 2) {
            GO2CPP_CHECK(events[0].revision == 2);
            GO2CPP_CHECK(events[1].revision == 3);
        }
    }
    // 历史窗口之外：需要调用方重建订阅。
    GO2CPP_CHECK(!store->Replay(id, 0).ok());
    GO2CPP_CHECK(store->Replay(9999, 0).status ==
                 go2cpp::distributed::Status::kNotFound);
}

void TestWalRestart() {
    go2cpp_tests::announce("metadata_store.wal_restart");
    const std::string dir = MakeTempDir("go2cpp_metadata_wal");
    MetadataStoreOptions options;
    options.wal_path = dir + "/meta.wal";
    {
        auto opened = MetadataStore::Open(options);
        GO2CPP_CHECK(opened.ok());
        auto store = std::move(opened.value);
        GO2CPP_CHECK(store->Put("a", "1").ok());
        GO2CPP_CHECK(store->Put("b", "2").ok());
        GO2CPP_CHECK(store->Delete("a").ok());
        GO2CPP_CHECK(store->Revision() == 3);
    }
    {
        auto opened = MetadataStore::Open(options);
        GO2CPP_CHECK(opened.ok());
        auto store = std::move(opened.value);
        GO2CPP_CHECK(store->Revision() == 3);
        GO2CPP_CHECK(!store->Get("a").ok());
        const auto b = store->Get("b");
        GO2CPP_CHECK(b.ok() && b.value.value == "2" && b.value.revision == 2);
        const auto stats = store->GetStats();
        GO2CPP_CHECK(stats.recover_records == 3);
        GO2CPP_CHECK(stats.truncated_bytes == 0);
        // 重启后 revision 续接，而不是从 1 重来。
        std::uint64_t revision = 0;
        GO2CPP_CHECK(store->Put("c", "3", &revision).ok());
        GO2CPP_CHECK(revision == 4);
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

void TestWalTruncatedTailRecovers() {
    go2cpp_tests::announce("metadata_store.wal_truncated_tail");
    const std::string dir = MakeTempDir("go2cpp_metadata_trunc");
    MetadataStoreOptions options;
    options.wal_path = dir + "/meta.wal";
    {
        auto opened = MetadataStore::Open(options);
        GO2CPP_CHECK(opened.ok());
        auto store = std::move(opened.value);
        GO2CPP_CHECK(store->Put("a", "1").ok());
        GO2CPP_CHECK(store->Put("b", "2").ok());
        GO2CPP_CHECK(store->Put("c", "3").ok());
    }
    const auto size = std::filesystem::file_size(options.wal_path);
    std::filesystem::resize_file(options.wal_path, size - 5);  // 撕裂最后一条记录。
    {
        auto opened = MetadataStore::Open(options);
        GO2CPP_CHECK(opened.ok());
        auto store = std::move(opened.value);
        GO2CPP_CHECK(store->Revision() == 2);
        GO2CPP_CHECK(store->Get("b").ok());
        GO2CPP_CHECK(!store->Get("c").ok());
        GO2CPP_CHECK(store->GetStats().truncated_bytes > 0);
        GO2CPP_CHECK(store->Put("d", "4").ok());
        GO2CPP_CHECK(store->Revision() == 3);
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

void TestWalMiddleCorruptionRejected() {
    go2cpp_tests::announce("metadata_store.wal_middle_corruption");
    const std::string dir = MakeTempDir("go2cpp_metadata_corrupt");
    MetadataStoreOptions options;
    options.wal_path = dir + "/meta.wal";
    {
        auto opened = MetadataStore::Open(options);
        GO2CPP_CHECK(opened.ok());
        auto store = std::move(opened.value);
        GO2CPP_CHECK(store->Put("a", "1").ok());
        GO2CPP_CHECK(store->Put("b", "2").ok());
        GO2CPP_CHECK(store->Put("c", "3").ok());
    }
    // 翻转第一条第 30 字节（key/value 区域，必在第一条记录内）。
    {
        std::fstream file(options.wal_path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(30);
        const char flipped = static_cast<char>(0x5A);
        file.write(&flipped, 1);
    }
    auto opened = MetadataStore::Open(options);
    GO2CPP_CHECK(!opened.ok());
    GO2CPP_CHECK(opened.status == go2cpp::distributed::Status::kProtocolError);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

} // namespace

int main() {
    go2cpp_tests::announce("distributed.metadata_store");
    TestKvRoundTrip();
    TestRevisionMonotonic();
    TestWatchOrderAndPrefix();
    TestCallbackExceptionIsolated();
    TestReplay();
    TestWalRestart();
    TestWalTruncatedTailRecovers();
    TestWalMiddleCorruptionRejected();

    const int failures = go2cpp_tests::g_failures.load();
    if (failures != 0) {
        std::fprintf(stderr, "distributed_metadata_store_test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("distributed_metadata_store_test: all cases passed\n");
    return 0;
}
