/**
 * @file test_headers.cpp
 * @brief 头文件自包含与模板实例化检查。
 *
 * @details 所有公共头必须能单独编译（自包含），且 7 个策略类 + 工厂 +
 *          线程安全包装的模板成员都要被真实实例化——只 include 不实例化
 *          会漏掉模板体内才出现的类型错误。本 TU 单独加入测试目标，
 *          编译通过即视为「头文件层」验收通过。
 */
#include "go2cpp/cache/arc.hpp"
#include "go2cpp/cache/cache.hpp"
#include "go2cpp/cache/clock.hpp"
#include "go2cpp/cache/factory.hpp"
#include "go2cpp/cache/gdsf.hpp"
#include "go2cpp/cache/lru.hpp"
#include "go2cpp/cache/lru_k.hpp"
#include "go2cpp/cache/tiny_lfu.hpp"
#include "go2cpp/cache/two_queue.hpp"
#include "go2cpp/cache/workload.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace {

void Exercise(go2cpp::cache::Cache<std::string, std::string>& cache) {
    cache.Put("alpha", "1", 1);
    std::string value;
    (void)cache.Get("alpha", &value);
    (void)cache.Peek("alpha", &value);
    (void)cache.Erase("alpha");
    (void)cache.Size();
    (void)cache.Capacity();
    (void)cache.Weight();
    (void)cache.Stats();
    (void)cache.Name();
    (void)cache.Describe();
    cache.Clear();
}

}  // namespace

void RunHeaderTests() {
    using go2cpp::cache::CacheOptions;
    using go2cpp::cache::Policy;

    CacheOptions options;
    options.capacity = 8;

    go2cpp::cache::LruCache<std::string, std::string> lru(options);
    go2cpp::cache::LruKCache<std::string, std::string> lru_k(options);
    go2cpp::cache::TwoQueueCache<std::string, std::string> two_queue(options);
    go2cpp::cache::TinyLfuCache<std::string, std::string> tiny_lfu(options);
    go2cpp::cache::GdsfCache<std::string, std::string> gdsf(options);
    go2cpp::cache::ArcCache<std::string, std::string> arc(options);
    go2cpp::cache::ClockCache<std::string, std::string> clock(options);

    Exercise(lru);
    Exercise(lru_k);
    Exercise(two_queue);
    Exercise(tiny_lfu);
    Exercise(gdsf);
    Exercise(arc);
    Exercise(clock);

    for (Policy policy : {Policy::kLru, Policy::kLruK, Policy::kTwoQueue,
                          Policy::kTinyLfu, Policy::kGdsf, Policy::kArc,
                          Policy::kClock}) {
        auto cache = go2cpp::cache::MakeCache<std::string, std::string>(
            policy, options);
        Exercise(*cache);
        auto safe = go2cpp::cache::MakeThreadSafeCache<std::string,
                                                       std::string>(policy,
                                                                    options);
        Exercise(*safe);
    }

    go2cpp::cache::ShardedCache<std::string, std::string> sharded(
        Policy::kLru, options, 4);
    Exercise(sharded);

    go2cpp::cache::WorkloadOptions workload_options;
    workload_options.operations = 16;
    workload_options.hot_keys = 4;
    workload_options.cold_keys = 16;
    for (auto kind : {go2cpp::cache::WorkloadKind::kHotspot,
                      go2cpp::cache::WorkloadKind::kScan,
                      go2cpp::cache::WorkloadKind::kWeighted,
                      go2cpp::cache::WorkloadKind::kCdn,
                      go2cpp::cache::WorkloadKind::kMixed}) {
        go2cpp::cache::Workload workload(kind, workload_options);
        const auto requests = workload.Generate();
        (void)requests;
    }
}
