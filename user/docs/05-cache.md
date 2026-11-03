# 缓存淘汰策略（交接文档）

> **定位**：header-only 的缓存淘汰策略库——7 种策略（LRU / LRU-K / 2Q / W-TinyLFU / GDSF / ARC / CLOCK）共用一个 `Cache<K,V>` 接口，外加线程安全与分片装饰器、可复现负载生成器。
> **源码**：`include/go2cpp/cache/**`（约 4000 行，**没有 .cpp**）　**构建**：`cmake/Go2CppCache.cmake`，目标 `go2cpp_cache`（INTERFACE 库，只依赖 `Threads::Threads`）
> **测试**：`tests/cache/`（16 个文件：`test_lru` / `test_lru_k` / `test_arc` / `test_tiny_lfu` / `test_random` / `test_stress` / `cache_bench` …）
> **设计全文**：[docs/modules/cache.md](../../docs/modules/cache.md)（策略与论文差异、复杂度、边界）
> **实测**：[docs/reports/vs-cache.md](../../docs/reports/vs-cache.md)；示例 [user/app/README.md](../app/README.md)（`cache_demo`）

## 1. 这个模块是什么

一句话：**把「淘汰策略」从「缓存容器」里拆出来，做成可对拍、可替换的策略库**。
业务代码只依赖 `Cache<K,V>` 的 6 个方法（`Get/Put/Erase/Peek/Clear/Stats`），
换策略只是换一个 `MakeCache<K,V>(Policy::kArc, opts)` 的参数。

它存在的理由是**选择需要数据**：同一个热点负载下，LRU 命中率 0.5580，LRU-K(2) 是 0.7904、
ARC 是 0.7817（+22 个百分点）；而在扫描抗污染场景里 CLOCK 只有 0.3507、LRU-K(2) 是 0.5197。
这些数字在同机、同请求序列、同容量下测出来才有意义——本模块自带负载生成器就是为了这个。

| 关注点 | 本模块的做法 |
|---|---|
| 策略与容器解耦 | `Cache<K,V>` 纯虚接口 + 7 份独立实现（各自可直接单测与对拍） |
| 线程安全 | `ThreadSafeCache<K,V,Lock>` 粗粒度互斥；`ShardedCache` 分片 |
| Fiber 友好 | `Lock` 模板参数可传 `go2cpp::sync::Mutex`，等待会 park 而不是阻塞 M |
| 可复现 | 固定 `seed` + `Workload` 生成器 → 7 策略吃到**逐请求相同**的序列 |
| 观测 | `CacheStats` 11 个累计计数（hits/misses/inserts/updates/evictions/ghost_hits/rejections/…） |

**不做什么**：没有 TTL/过期/刷新语义（要过期就在 value 上自己带时间戳）、不链接 runtime
（不需要 Scheduler/IO）、不进入 `install(EXPORT)`（只随 `include/` 安装）。

## 2. 架构 / 数据流

```
        业务代码（只依赖 Cache<K,V> 接口）
             │  Get(key,&v) / Put(key,v,weight) / Erase / Peek / Stats
             ▼
   ┌───────────────────────────────────────────────────────────┐
   │ ThreadSafeCache<K,V,Lock>（装饰器，粗粒度互斥）             │
   │   Lock = std::mutex（默认） 或 go2cpp::sync::Mutex（Fiber）  │
   └───────────────┬───────────────────────────────────────────┘
                   │ 或 ShardedCache（N 片，各片一把锁）
                   ▼
   ┌───────────────────────────────────────────────────────────┐
   │ Cache<K,V> 纯虚接口                                        │
   │   Get / Peek / Put / Erase / Clear / Size / Capacity /     │
   │   Weight / Stats / Name                                    │
   └───┬───────┬───────┬────────┬────────┬───────┬───────┬──────┘
       ▼       ▼       ▼        ▼        ▼       ▼       ▼
      LRU   LRU-K(2)  2Q   W-TinyLFU   GDSF    ARC    CLOCK
       │       │       │        │        │       │       │
       └───────┴───────┴────────┴────────┴───────┴───────┘
                  共用 detail/：
                  intrusive（侵入式链表/节点池）
                  bucket_map（索引）
                  sketch（Count-Min Sketch，TinyLFU 用）
                  time_wheel（256 桶时间轮）
                  hash（固定 seed 哈希）

  Get 路径：索引查 → 命中(提升 recency/频率) → 拷贝 value 返回
  Put 路径：已存在 → 更新 + 提升；不存在 → 插入 → 超容量则淘汰（策略各异）
```

### 2.1 七种策略的一句话区别

| 策略 | 淘汰依据 | 一句话 |
|---|---|---|
| `LruCache` | 最近使用时间 | 基线；扫描会整片污染 |
| `LruKCache` | 第 K（默认 2）次访问的历史 | 二次访问才进主缓存，抗扫描最好 |
| `TwoQueueCache` | A1in/A1out/Am 三段 | 用 ghost 队列近似「访问两次」 |
| `TinyLfuCache` | 窗口 LRU + SLRU + Count-Min 频率 | 频率准入，W-TinyLFU 论文同构 |
| `GdsfCache` | `H = L + cost/size × freq` | **唯一真正消费 `weight` 的策略** |
| `ArcCache` | T1/T2/B1/B2 自适应 | 命中率最稳，内存 280 KiB 级 |
| `ClockCache` | 引用位轮转 | 最快（30–34 Mops/s），命中率略低于 LRU |

### 2.2 线程模型（交接重点）

- **裸策略类完全不是线程安全的**：并发 `Get/Put` 会破坏内部链表与索引，统计也会丢更新。
- `ThreadSafeCache` 是**一把锁**（一次只允许一个读者或写者）。读多写少要换 `ShardedCache`，
  代价是**跨分片语义消失**（全局 LRU 顺序、TinyLFU 全局频率都不再成立）。
- `ShardedCache` 按 key 哈希分片，片数与每片容量在构造时固定。
- 这些锁都是普通互斥；要 Fiber 语义必须显式传 `go2cpp::sync::Mutex`。

## 3. 对外 API（关键接口）

### 3.1 统一接口与配置

```cpp
// include/go2cpp/cache/cache.hpp（namespace go2cpp::cache）
struct CacheOptions {
    std::size_t capacity = 1024;       // 条目数上限，必须 >= 1（GDSF 允许 0 = 只按 max_weight）
    std::size_t max_weight = 0;        // 总权重上限；0 = 不限，仅 GDSF 消费
    std::uint64_t seed = 0x243F6A8885A308D3ULL;   // TinyLFU 素描哈希种子
    double fetch_cost = 1.0;           // GDSF 的 cost，默认 1.0
};

struct CacheStats {
    std::uint64_t hits, misses, inserts, updates, evictions;
    std::uint64_t ghost_hits;          // 2Q 的 A1out、ARC 的 B1/B2 命中
    std::uint64_t rejections;          // 准入失败 / 容量 0
    std::uint64_t removals;
    std::size_t size, capacity, weight, max_weight, ghost_size;
    double HitRate() const noexcept;   // hits / (hits + misses)
};

template <class K, class V>
class Cache {
public:
    virtual bool Get(const K& key, V* value) = 0;             // 命中提升 recency
    virtual bool Peek(const K& key, V* value) const = 0;      // 不提升、不计命中的只读探测
    virtual void Put(const K& key, V value, std::size_t weight = 1) = 0;
    virtual bool Erase(const K& key) = 0;
    virtual void Clear() = 0;
    virtual std::size_t Size() const = 0;
    virtual std::size_t Capacity() const = 0;
    virtual std::size_t Weight() const = 0;
    virtual CacheStats Stats() const = 0;
    virtual const char* Name() const = 0;                     // "LRU" / "ARC" / ...
};
```

`Stats()` 的计数是**自构造以来累计**，`Clear()` 不清零统计——这是为了让基准能在同一实例上
分段观察。`Peek()` 不计命中/未命中。

### 3.2 工厂与装饰器

```cpp
// include/go2cpp/cache/factory.hpp
enum class Policy { kLru, kLruK, kTwoQueue, kTinyLfu, kGdsf, kArc, kClock };
const char* PolicyName(Policy policy) noexcept;      // "LRU" / "LRU-K(2)" / "W-TinyLFU" ...

template <class K, class V>
std::unique_ptr<Cache<K, V>> MakeCache(Policy policy, const CacheOptions& options = {});

template <class K, class V, class Lock = std::mutex>
std::unique_ptr<Cache<K, V>> MakeThreadSafeCache(Policy policy, const CacheOptions& options = {});

template <class K, class V>          // 分片包装，跨分片无全局语义
class ShardedCache final : public Cache<K, V>;
```

`MakeCache` 对非法参数（容量 0 等）抛 `std::invalid_argument`。

### 3.3 负载生成器（对拍用）

```cpp
// include/go2cpp/cache/workload.hpp
enum class WorkloadKind { kHotspot, kScan, kWeighted, kCdn, kMixed };

struct WorkloadOptions {
    std::size_t hot_keys = 1000, cold_keys = 100000, operations = 200000;
    std::uint64_t seed = 0x5DEECE66DULL;
    std::size_t scan_length = 2000, scan_period = 2000, burst_length = 8;
    std::size_t videos = 1000, segments_per_video = 100;
};

struct CacheRequest { std::uint64_t key = 0; std::uint32_t weight = 1; };

class Workload {
    explicit Workload(WorkloadKind kind, const WorkloadOptions& options = WorkloadOptions{});
    std::vector<CacheRequest> Generate();   // 长度 = options.operations
};
```

**五组场景的用途**（这是选型的依据，别只看 hotspot）：`kHotspot` 80/20 热点测基本命中率与
recency；`kScan` 热点中周期插入顺序扫描测抗污染；`kWeighted` 20% 大对象（1:1000）测权重语义；
`kCdn` Zipf 视频 + 分片 + 大小分层测长尾；`kMixed` 三者叠加测综合压力。

## 4. 最小用法（可编译示例）

```cpp
#include "go2cpp/cache/factory.hpp"
#include "go2cpp/cache/lru.hpp"
#include "go2cpp/cache/workload.hpp"

#include <cstdio>
#include <string>

int main() {
    using namespace go2cpp::cache;

    Workload workload(WorkloadKind::kHotspot, WorkloadOptions{});
    const auto requests = workload.Generate();          // 固定 seed，可复现

    CacheOptions opts;
    opts.capacity = 2000;
    auto lru = MakeCache<std::uint64_t, std::string>(Policy::kLru, opts);
    auto arc = MakeCache<std::uint64_t, std::string>(Policy::kArc, opts);

    for (const auto& req : requests) {
        std::string value;
        if (!lru->Get(req.key, &value)) {
            lru->Put(req.key, std::to_string(req.key), req.weight);   // cache-aside
        }
        if (!arc->Get(req.key, &value)) {
            arc->Put(req.key, std::to_string(req.key), req.weight);
        }
    }

    std::printf("LRU hit=%.4f  ARC hit=%.4f（容量 %zu，请求 %zu）\n",
                lru->Stats().HitRate(), arc->Stats().HitRate(),
                lru->Capacity(), requests.size());
    return 0;   // 实测参考：hotspot 场景 LRU 0.5580 / ARC 0.7817
}
```

编译：`g++ -std=c++17 -O2 -I include demo.cpp -lpthread`（header-only，无需链接库）。
要线程安全就把 `MakeCache` 换成 `MakeThreadSafeCache<uint64_t, std::string>(Policy::kArc, opts)`。

## 5. 源码位置

**全部是头文件，`src/cache/` 不存在**——这是有意的：策略是模板，且要能被业务零成本内联。

| 文件 | 行数 | 职责 |
|---|---:|---|
| `include/go2cpp/cache/cache.hpp` | 279 | `Cache` 纯虚接口、`CacheOptions`、`CacheStats`、`ThreadSafeCache` |
| `include/go2cpp/cache/factory.hpp` | 253 | `Policy` 枚举、`MakeCache`、`MakeThreadSafeCache`、`ShardedCache` |
| `include/go2cpp/cache/lru.hpp` | 185 | LRU：侵入式双向链表 + 哈希索引 |
| `include/go2cpp/cache/lru_k.hpp` | 301 | LRU-K(2)：corral（历史队列）+ 主缓存 |
| `include/go2cpp/cache/two_queue.hpp` | 360 | 2Q：A1in / A1out（ghost）/ Am |
| `include/go2cpp/cache/tiny_lfu.hpp` | 459 | W-TinyLFU：窗口 + SLRU + Count-Min 准入 |
| `include/go2cpp/cache/gdsf.hpp` | 300 | GDSF：`H = L + cost/size × freq`，最小堆 + 惰性删除 |
| `include/go2cpp/cache/arc.hpp` | 446 | ARC：T1/T2/B1/B2 自适应调整 p |
| `include/go2cpp/cache/clock.hpp` | 237 | CLOCK：引用位轮转（非 CLOCK-Pro） |
| `include/go2cpp/cache/workload.hpp` | 249 | 5 组可复现负载生成器 |
| `include/go2cpp/cache/detail/intrusive.hpp` | 329 | 侵入式链表、节点，避免每操作堆分配 |
| `include/go2cpp/cache/detail/bucket_map.hpp` | 166 | 分桶索引（开放寻址外的另一种选择） |
| `include/go2cpp/cache/detail/sketch.hpp` | 214 | Count-Min Sketch / Bloom（TinyLFU） |
| `include/go2cpp/cache/detail/time_wheel.hpp` | 192 | 256 桶时间轮（GDSF 的 L 衰减等用） |
| `include/go2cpp/cache/detail/hash.hpp` | 46 | 固定 seed 哈希（可复现） |

## 6. 性能数据

来源：[docs/reports/vs-cache.md](../../docs/reports/vs-cache.md)（2026-11-01，WSL2 / GCC 13.3 `-O2`，
**单线程**，capacity=2000，300,000 操作，5 个固定 seed 场景，同一请求序列喂给 7 个策略）。

### 6.1 命中率（核心数据）

| 策略 | hotspot | scan | CDN | mixed | 峰值内存 |
|---|---:|---:|---:|---:|---:|
| LRU | 0.5580 | 0.3674 | 0.4771 | 0.5396 | 143 KiB |
| **LRU-K(2)** | **0.7904** | **0.5197** | 0.6676 | **0.6538** | 380 KiB |
| 2Q | 0.6484 | 0.4248 | 0.5352 | 0.6433 | 226 KiB |
| **W-TinyLFU** | 0.7821 | 0.5061 | 0.5883 | 0.4997 | 220 KiB |
| GDSF | 0.7693 | 0.4847 | **0.7483** | 0.4268 | 1591 KiB |
| **ARC** | 0.7817 | 0.5104 | 0.5561 | 0.6399 | 280 KiB |
| CLOCK | 0.5308 | 0.3507 | 0.4611 | 0.5322 | 174 KiB |

结论原样引用：热点场景 **比 LRU 高 21–23 个百分点**（ARC 0.7817 / LRU-K 0.7904 / W-TinyLFU 0.7821
vs LRU 0.5580）；扫描抗污染 **高 13.9–15.2 个百分点**；CDN 大对象场景 **GDSF 0.7483 vs LRU 0.4771
（+27.1 pp）**。mixed 场景里 GDSF 反而只有 0.4268（比 LRU 还低）——**没有全局最优策略**。

### 6.2 吞吐（单线程 Mops/s）

| 策略 | hotspot | scan | CDN | mixed |
|---|---:|---:|---:|---:|
| CLOCK | **30.20** | **31.41** | 25.37 | **33.95** |
| LRU | 28.39 | 30.21 | 21.37 | 33.23 |
| ARC | 23.90 | 16.86 | 12.77 | 16.00 |
| 2Q | 17.99 | 14.71 | 13.81 | 18.86 |
| GDSF | 18.41 | 17.89 | 16.94 | 14.58 |
| W-TinyLFU | 13.15 | 12.60 | 12.07 | 12.25 |
| LRU-K(2) | 6.68 | 8.87 | 7.11 | 10.38 |

相对自身优化前：**逐策略 1.04–2.80×，几何 1.91×**（LRU 2.57×、2Q 2.80×、ARC 2.58×、CLOCK 2.12×、
LRU-K 1.97×、W-TinyLFU 1.76×、GDSF 1.04×）；按这 7 个数复算几何均值是 **2.03×**，与汇总记录的
1.91× 差约 6%——报告已如实标注，别把 1.91× 当精确值。汇编级成果是**每次操作 0 次堆分配**
（命中率逐位一致）。

### 6.3 与 Caffeine 只能算「同量级」

Caffeine 官方 Read(100%) 8 线程 **181.7 Mops/s**、16 线程 **382.4 Mops/s**（Java/JIT，16 核 Xeon）；
折算每线程 ≈22.7–23.9 Mops/s，我们 LRU 28.39、CLOCK 30.20。**口径不同（语言/机器/线程数/是否含
淘汰），不作达标判定**。Ristretto / groupcache 官方无公开同口径数字 → **未测/待补**。

## 7. 边界与已知限制

1. **裸策略非线程安全**；`ThreadSafeCache` 是**一把锁**，高并发读多写少应换 `ShardedCache`。
2. **`weight` 只对 GDSF 生效**：其它策略的 `weight` 仅用于观测与容量核算，不参与淘汰——这是
   有意的（LRU/2Q/ARC 的论文语义是「按对象个数」，强加权重会破坏 ARC 的 |T1| 约束）。
3. **`ShardedCache` 没有全局顺序**：跨分片的 LRU/ARC 语义、TinyLFU 频率都不再全局成立。
4. **LRU-K(2) 内存约 2×**（corral 与主缓存同容量），小容量下更明显。
5. **GDSF 的 `L` 不归一化**：长时间运行后数值单调增长；`H` 用 `double`，极端权重下精度下降。
   GDSF 峰值内存 1591 KiB（是 LRU 的 11 倍），选它要算内存账。
6. **无 TTL/过期/刷新语义**：过期需业务在 value 上自行判断（与 `storage::MemCache` 不同）。
7. **CLOCK 不是 CLOCK-Pro**：没有 LIR/HIR 保护，扫描场景命中率与 LRU 同档（0.3507）。
8. **未做无锁/RCU 实现**：并发扩展性靠分片而非原子操作。
9. **不进入 `install(EXPORT)`**：只随 `include/` 目录安装；需要 `find_package` 导出时在
   `Go2CppInstall.cmake` 的 `GO2CPP_MODULE_TARGETS` 里追加 `go2cpp_cache`（INTERFACE）。

## 8. 测试与验证方法

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build -R cache --output-on-failure
./build/go2cpp_cache_test                     # 汇总入口
./build/go2cpp_cache_bench --full             # 7 策略 × 5 场景 × 35 组命中率/吞吐
./build/go2cpp_cache_demo --ops 200000        # 人类可读的三场景对比
```

| 文件 | 覆盖 |
|---|---|
| `test_lru.cpp` / `test_lru_k.cpp` / `test_two_queue.cpp` / `test_tiny_lfu.cpp` / `test_gdsf.cpp` / `test_arc.cpp` / `test_clock.cpp` | 7 策略各自的语义：容量、淘汰顺序、ghost 计数、统计一致性 |
| `test_interface.cpp` / `test_headers.cpp` | 统一接口契约、头文件自包含 |
| `lru_k_reference.hpp` | LRU-K 的**参考实现**，与生产实现逐操作对拍 |
| `test_random.cpp` | 随机操作序列（随机 Get/Put/Erase）不崩、不超容 |
| `test_scenarios.cpp` | 5 组负载下的命中率区间断言 |
| `test_stress.cpp` | 多线程压测 `ThreadSafeCache` |
| `cache_test_support.hpp` / `cache_test_main.cpp` | 测试夹具与汇总入口 |
| `cache_bench.cpp` | `--full` 基准（生成 vs-cache.md 的数据） |

**加新策略的最低要求**：实现 `Cache<K,V>` 全部 10 个虚函数、在 `cache_bench` 里加一列、
让 `test_random.cpp` 的通用序列能跑通、并给出 hotspot/scan/CDN 三组命中率——否则无法选型。

## 9. 交接注意

- **先看 `test_scenarios.cpp` 的命中率区间再改算法**：策略实现的正确性判据不是「能编译」，
  而是热点/扫描/CDN 三档命中率落在断言窗口内；改淘汰逻辑几乎必然移动这些数字。
- **`detail/intrusive.hpp` 是性能地基**：它把节点内嵌进条目，做到「每次操作 0 次堆分配」。
  改成 `std::list`/`std::unordered_map` 会立刻丢掉 2–3× 吞吐（这正是优化前的状态）。
- **`LRU-K` 的 corral 容量 = 主缓存容量**，所以内存 2×；容量小于 100 时别用。
- **不要给非 GDSF 策略加权重淘汰**：会破坏 2Q 的 A1out 与 ARC 的 |T1| 约束，命中率反而降。
- **`ShardedCache` 的片数不是越多越好**：每片容量 = 总容量/片数，片数过多会让每片装不下热点，
  命中率断崖；当前默认按 CPU 数取，改之前跑 `cache_bench`。
- **`ThreadSafeCache` 里的 `Lock` 模板参数是唯一 Fiber 入口**：默认 `std::mutex` 在 G 上竞争会
  真阻塞 M；要 park 必须显式传 `go2cpp::sync::Mutex`（见 01-concurrency.md §3.5）。
- **未完成事项**：无 TTL、无 CLOCK-Pro、无无锁/RCU 版本、`find_package` 导出未接、并发扩展性曲线未测。
