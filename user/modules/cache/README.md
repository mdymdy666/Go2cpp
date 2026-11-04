# 缓存（cache）

> **一句话定位**：7 种淘汰策略（LRU / LRU-K / 2Q / W-TinyLFU / GDSF / ARC / CLOCK）
> 的统一接口 + 线程安全/分片装饰器，header-only。
> 深入文档：[缓存模块 · 深入学习](../../deep/05-cache.md) ·
> 源码 [`include/go2cpp/cache/`](../../../include/go2cpp/cache/)

## 能力清单

| 能力 | 关键 API | 说明 |
| --- | --- | --- |
| 统一接口 | `Cache<K,V>::Get/Peek/Put/Erase/Clear/Stats` | 所有策略实现同一套虚函数 |
| 策略选择 | `enum class Policy`、`MakeCache<K,V>(policy, options)` | 7 种策略一行切换 |
| 线程安全 | `MakeThreadSafeCache<K,V,Lock>` | 装饰器，可换 `std::mutex` / `sync::Mutex` |
| 分片 | `ShardedCache(policy, options, shards)` | 按 key 哈希分片，各自加锁 |
| 统计 | `CacheStats`（hits/misses/evictions/ghost_hits/rejections/HitRate） | 自构造以来累计 |
| 权重淘汰 | `Put(key, value, weight)`、`CacheOptions::max_weight` | 仅 GDSF 消费 |

策略速记：

| 策略 | 适合 | 代价 |
| --- | --- | --- |
| LRU | 基线、访问局部性强 | 扫描流量会冲垮 |
| LRU-K(2) | 抗一次性扫描 | 需要两次访问才进主表 |
| 2Q | 抗一次性流量、通用 | 多一层 ghost 队列 |
| W-TinyLFU | 通用高命中 | 频率素描内存与哈希开销 |
| GDSF | 对象大小差异大 | 需要提供 cost/weight |
| ARC | 读写混合、要自适应 | 实现与调参最复杂 |
| CLOCK | 条目小、QPS 极高 | 命中率略低于 LRU |

## 最小示例

```cpp
#include "go2cpp/cache/factory.hpp"
#include <iostream>

int main() {
    using namespace go2cpp::cache;

    CacheOptions options;
    options.capacity = 128;                       // 条目数上限，必须 >= 1
    auto cache = MakeCache<std::string, int>(Policy::kLru, options);

    cache->Put("a", 1);
    int value = 0;
    if (cache->Get("a", &value)) std::cout << "hit: " << value << '\n';

    const CacheStats stats = cache->Stats();
    std::cout << cache->Name() << " hitrate=" << stats.HitRate() << '\n';
    return 0;
}
```

编译（header-only，链运行时库即可）：

```sh
g++ -std=c++17 -I include user/modules/cache/README-snippet.cpp \
    build/libgo2cpp_runtime.a -lpthread -o /tmp/cache_demo
```

运行 7 策略对比示例：

```sh
cmake --build build --target go2cpp_cache_demo -j
./build/user/app/go2cpp_cache_demo
```

## 3 个常见异常

### 1. 容量传 0 直接抛异常
- **现象**：`MakeCache` 抛 `std::invalid_argument`，或 GDSF 下行为异常。
- **原因**：`CacheOptions::capacity` 必须 >= 1（GDSF 允许 0，表示只按 `max_weight` 限制）。
- **处理**：构造前校验容量；从配置读入时给默认值并做下限检查。

### 2. 多线程共享裸 `Cache` 出现数据竞争
- **现象**：偶发崩溃、命中率统计错乱、size 与预期不符。
- **原因**：策略实现**不保证线程安全**（`StatsRecorder` 刻意用非原子计数）。
- **处理**：用 `MakeThreadSafeCache`（单锁）或 `ShardedCache`（多片各自加锁）；
  Fiber 场景传 `go2cpp::sync::Mutex` 作为 `Lock`。

### 3. 命中率远低于预期
- **现象**：压测命中率 60%，与论文/预期差很多。
- **原因**：策略与访问模式不匹配——扫描流量用 LRU 会被冲垮；对象大小差异大时
  按条目数计容会挤掉大对象；容量按「条目数」而不是字节数配置。
- **处理**：一次性流量换 LRU-K/2Q；大小差异大换 GDSF 并设置 `max_weight`；
  用 `CacheStats` 的 `evictions`/`ghost_hits`/`rejections` 定位是容量不足还是准入拒绝。

## 示例清单

| # | 位置 | 场景 |
| ---: | --- | --- |
| 01 | [`../app/cache_demo/`](../app/cache_demo/) | 7 种策略同一负载下的命中率对比（可执行程序） |
| 02 | `tests/cache/cache_bench.cpp` | 策略基准（吞吐/命中率） |
| 03 | `tests/cache/test_interface.cpp` | 统一接口契约 |
| 04 | `tests/cache/test_lru.cpp`、`test_lru_k.cpp`、`test_two_queue.cpp` | 单策略行为 |
| 05 | `tests/cache/test_tiny_lfu.cpp`、`test_gdsf.cpp`、`test_arc.cpp`、`test_clock.cpp` | 单策略行为 |

> 注意：`cache_demo` 位于 `user/modules/app/cache_demo/`，由 `user/modules/app/CMakeLists.txt`
> 构建，产物在 `build/user/app/go2cpp_cache_demo`。

## 深入阅读

- [缓存模块 · 深入学习](../../deep/05-cache.md)
- [交接摘要](../../docs/05-cache.md)
- 策略实现：`include/go2cpp/cache/{lru,lru_k,two_queue,tiny_lfu,gdsf,arc,clock}.hpp`

## 边界与注意

- **`Get` 与 `Peek` 语义不同**：`Get` 会改变 LRU 顺序/频率并计统计；`Peek` 是纯查询。
- **`Put` 覆盖已有 key 时也可能触发淘汰**（取决于策略与权重），不要假设「覆盖不淘汰」。
- **统计是累计值**：`Clear()` 不清零 `CacheStats`，分段观察要自己记录差值。
- **`ShardedCache` 的容量按分片均分**：`capacity < shards` 时每片至少 1 条，实际总量会大于配置。
- **不要缓存大对象**：`Put` 会拷贝/移动 value，大对象建议缓存 `shared_ptr` 或索引。
