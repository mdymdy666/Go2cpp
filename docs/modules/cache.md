# cache：7 种淘汰策略的统一缓存

> 状态：现行（2026-10-31，HEAD `6c08210`）。
> 源码：`include/go2cpp/cache/`（header-only）；测试：`tests/cache/`；构建：`cmake/Go2CppCache.cmake`。
> 算法综述、论文对照、完整基准方法与逐场景数据见
> [`docs/research/cache-algorithms.md`](../research/cache-algorithms.md)；本文是模块入口：
> 接口、策略差异、复杂度、构建测试与限制。

## 1. 定位

`go2cpp::cache` 是**只有头文件**的通用缓存模块，把「淘汰策略」与「缓存容器语义」解耦：
同一个 `Cache<K, V>` 纯虚接口后面挂 7 种策略实现，业务按工作负载切换策略即可，不必改调用点。
它不依赖 `go2cpp::runtime`（不依赖 Scheduler/Channel），只要求 C++17 与 `Threads::Threads`；
需要 Fiber 感知的互斥时，把 `go2cpp::sync::Mutex` 传给 `ThreadSafeCache` 的锁模板参数。

模块**不做**的事：持久化、TTL/过期、分布式一致性、跨进程共享、一致性哈希。这些属于调用方的
业务语义，不塞进缓存容器。

## 2. 目录与构建目标

```text
include/go2cpp/cache/
  cache.hpp           # Cache<K,V>、CacheOptions、CacheStats、StatsRecorder、ThreadSafeCache
  lru.hpp             # LruCache
  lru_k.hpp           # LruKCache（K=2）
  two_queue.hpp       # TwoQueueCache（2Q）
  tiny_lfu.hpp        # TinyLfuCache（W-TinyLFU）
  gdsf.hpp            # GdsfCache（GDSF）
  arc.hpp             # ArcCache（ARC）
  clock.hpp           # ClockCache（CLOCK）
  factory.hpp         # Policy、MakeCache、MakeThreadSafeCache、ShardedCache
  workload.hpp        # 5 种可复现负载生成器（测试与基准共用）
  detail/hash.hpp     # SplitMix64、NextPowerOfTwo
  detail/sketch.hpp   # CountMinSketch4、BloomDoorkeeper
tests/cache/          # 12 个测试 TU + 1 个基准（cache_bench.cpp）
cmake/Go2CppCache.cmake
```

`go2cpp_cache` 是 **INTERFACE** target（别名 `go2cpp::cache`）：不产出 `.so`，头文件随顶层
`install(DIRECTORY include/)` 一起安装，不单独登记 `install(EXPORT)`，因此不改变既有导出集合。

## 3. 统一接口

`Cache<K, V>` 的纯虚方法：

| 方法 | 语义 | 是否改变策略状态 |
| --- | --- | --- |
| `bool Get(key, V* value)` | 读取；`value` 可为 `nullptr`（只查存在性） | 是（LRU 置 MRU、频率 +1、ghost 提升） |
| `bool Peek(key, V* value) const` | 纯查询 | 否（不动顺序、不计命中） |
| `void Put(key, value, weight = 1)` | 插入或覆盖；`weight` 是占用单元（通常是字节） | 是 |
| `bool Erase(key)` | 删除一个条目 | 是（不计淘汰） |
| `void Clear()` | 清空条目与权重；**累计统计不清零** | 是 |
| `Size()` / `Capacity()` / `Weight()` | 当前水位 | 否 |
| `CacheStats Stats() const` | 累计统计 + 当前水位快照 | 否 |
| `const char* Name()` / `Describe()` | 策略短名 / 带内部参数（如 ARC 的 `p`） | 否 |

`CacheOptions`（`cache.hpp`）：

| 字段 | 默认 | 含义 |
| --- | --- | --- |
| `capacity` | `1024` | 条目数上限，必须 ≥ 1（GDSF 允许 0，表示只按 `max_weight` 限制） |
| `max_weight` | `0` | 总权重上限；0 表示不限制。**只有 GDSF 消费该字段** |
| `seed` | `0x243F6A8885A308D3` | TinyLFU 频率素描/Bloom 的哈希种子；固定后基准可复现 |
| `fetch_cost` | `1.0` | GDSF 的取回成本 `cost`（仅 GDSF 使用） |

`CacheStats` 计数器：`hits`、`misses`、`inserts`、`updates`、`evictions`、`ghost_hits`、
`rejections`、`removals`，水位字段 `size`/`capacity`/`weight`/`max_weight`/`ghost_size`，
以及 `Total()`、`HitRate()`、`MissRate()`。计数**自构造以来累计**，`Clear()` 不清零，
便于在同一个实例上分段观察。

### 线程模型

- 裸策略类**不是**线程安全的：并发 `Get/Put` 会破坏内部链表/索引，统计也会丢更新。
- `ThreadSafeCache<K, V, Lock = std::mutex>`：装饰器加粗粒度互斥。`Lock` 可换
  `go2cpp::sync::Mutex` 获得 Fiber-aware 挂起语义（未受管线程上退化为普通阻塞）。
- `ShardedCache<K, V, Hash = std::hash<K>>`：按 key 哈希把请求分到多个独立加锁的子缓存，
  容量按分片均分（余数给前几片；`capacity < shards` 时每片至少 1 条，`Capacity()` 返回实际值）。
  **跨分片的全局 LRU/ARC 顺序不再成立**，TinyLFU 的素描也变成每片独立；要全局语义请用单个
  `ThreadSafeCache`。
- 构造工厂：`MakeCache<K, V>(Policy, CacheOptions)`、`MakeThreadSafeCache<K, V, Lock>(...)`；
  非法参数（如容量 0 的 LRU）抛 `std::invalid_argument`。

## 4. 7 种策略与论文差异

`Policy` 枚举：`kLru`、`kLruK`、`kTwoQueue`、`kTinyLfu`、`kGdsf`、`kArc`、`kClock`；
`PolicyName()` 给出 `LRU` / `LRU-K(2)` / `2Q` / `W-TinyLFU` / `GDSF` / `ARC` / `CLOCK`。

| 策略 | 类 | 关键机制 | 与论文/参考实现的关键差异 |
| --- | --- | --- | --- |
| LRU | `LruCache` | `list` + `map`，命中置 MRU | 无 |
| LRU-K（K=2） | `LruKCache` | 倒数第二次访问时间排序 + corral | K 固定 2；corral 容量取 `max(4, C)`（论文建议更小），内存约 2×；Get 命中 corral 直接提升 |
| 2Q | `TwoQueueCache` | A1in / A1out(ghost) / Am | ghost 命中只记 `pending_promote`，由下一次 `Put` 提升到 Am（接口的 Get 不负责加载） |
| W-TinyLFU | `TinyLfuCache` | 1% 窗口 + 20% main + 80% SLRU + 频率准入；4-bit 4 行素描 + Bloom doorkeeper | 读时计频、写不计频（与 Caffeine 一致）；1/20/80 比例固定 |
| GDSF | `GdsfCache` | 堆 + 惰性删除 + version；`H = L + cost/size × freq` | `L` 用累加 `L += H(victim)` 实现，未做周期性归一化 |
| ARC | `ArcCache` | T1/T2 + B1/B2 ghost，自适应 `p` | ghost 命中记 pending，由 `Put` 完成 T2 插入；ghost 总容量 = C |
| CLOCK | `ClockCache` | 槽位数组 + free list + 二次机会 | 不实现 CLOCK-Pro 的 LIR/HIR 分层 |

> 权重语义是**有意的不对称**：只有 GDSF 用 `weight` 做淘汰决策；LRU/2Q/ARC 仍按条目数淘汰，
> 权重只用于观测与容量核算——强行给它们加权重会破坏论文不变量（例如 ARC 的 |T1| 约束）。

## 5. 复杂度与内存

| 类 | Get | Put | 额外结构 |
| --- | --- | --- | --- |
| `LruCache` | O(1) | O(1) | `list` + `map` |
| `LruKCache` | O(log n) | O(log n) | `map(ref_time)` + corral FIFO |
| `TwoQueueCache` | O(1) | O(1) | 3 `list` + 3 `map` + pending set |
| `TinyLfuCache` | O(1) | O(1) | 3 `list` + 3 `map` + sketch + bloom |
| `GdsfCache` | O(log n) | O(log n) | `map` + 堆（惰性删除） |
| `ArcCache` | O(1) | O(1) | 2 `list` + 2 ghost `list` + 4 `map` + pending |
| `ClockCache` | O(1) | O(1) 均摊 | `vector` 槽位 + `map` |

## 6. 构建与测试

模块随主构建默认可用（header-only）；测试由 `GO2CPP_BUILD_TESTS` 控制，注册 2 个 ctest：

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_TESTS=ON
cmake --build .build-bento-wsl --target go2cpp_cache_test go2cpp_cache_bench -j 8
ctest --test-dir .build-bento-wsl -R cache --output-on-failure   # 2/2
```

- `go2cpp_cache_test`（TIMEOUT 900）：单元 + 随机对拍 + 5 个真实场景 + 并发压测，见 §6.1；
- `go2cpp_cache_bench`（TIMEOUT 600）：5 场景 × 7 策略，ctest 默认 `--quick`，完整数据用 `--full`。

### 6.1 测试覆盖（`tests/cache/`，12 个 TU）

| 维度 | 覆盖 |
| --- | --- |
| 单元 | 每个算法的 `Get/Put/Erase/Peek/Clear`、淘汰顺序、统计、权重、边界（容量 1、容量 0 异常、空 `out` 指针、超大值） |
| 随机 | 7 策略 × 20,000 次随机操作，对拍「命中值 == 最后 Put 值」、容量不变量、`hits+misses` 守恒；小容量 1..7；字符串 key/value；`ThreadSafeCache` 包装 |
| 场景 | 5 个：热点、扫描、权重、CDN、混合；断言策略语义（谁该抗扫描、GDSF 该偏向小对象） |
| 压测 | LRU 100 万次读穿透（16.6 Mops/s）、LRU-K 20 万次写入（corral 有界）、GDSF 20 万次权重压力、ARC 30 万次（ghost 有界）、TinyLFU 8 线程 × 5 万（统计守恒）、CLOCK 50 轮 Clear |
| 头文件 | 所有头自包含；7 个策略 + 工厂 + 分片模板实例化 |

## 7. 实测摘要

环境：WSL2 Ubuntu 24.04、GCC 13.3、`-O2`、单线程；capacity=2000，ops=300,000
（scan 场景为预热 50k + 扫描 50k + 恢复 50k）。完整 5 场景 × 7 策略表见
[`docs/research/cache-algorithms.md`](../research/cache-algorithms.md) §8。

### 命中率（hotspot 场景）

| 策略 | hit_rate | Mops/s | 峰值内存 KiB |
| --- | --- | --- | --- |
| LRU | 0.5580 | 28.39 | 143 |
| LRU-K(2) | 0.7904 | 6.68 | 380 |
| 2Q | 0.6484 | 17.99 | 226 |
| W-TinyLFU | 0.7821 | 13.15 | 220 |
| GDSF | 0.7693 | 18.41 | 415 |
| ARC | 0.7817 | 23.90 | 280 |
| CLOCK | 0.5308 | 30.20 | 174 |

### 抗扫描（scan 场景）

| 策略 | hit_rate | 恢复窗口（ops） | Mops/s |
| --- | --- | --- | --- |
| LRU | 0.3674 | 4000 | 30.21 |
| LRU-K(2) | 0.5197 | 2000 | 8.87 |
| 2Q | 0.4248 | 2000 | 14.71 |
| W-TinyLFU | 0.5061 | 4000 | 12.60 |
| GDSF | 0.4847 | 2000 | 17.89 |
| ARC | 0.5104 | 2000 | 16.86 |
| CLOCK | 0.3507 | 4000 | 31.41 |

> 结论：热点场景 ARC/W-TinyLFU/LRU-K 明显优于 LRU/CLOCK；扫描场景同理。GDSF 在 weighted
> 场景按权重偏向小对象（`size` 会超过 capacity，因为只受 `max_weight` 约束），代价是内存与
> O(log n) 操作。CLOCK 最快但命中率最低，适合「缓存只是加速、丢失可回源」且 CPU 敏感的场景。

## 8. 已知限制

1. **裸策略非线程安全**；`ThreadSafeCache` 是粗粒度互斥，高并发读多写少应改用 `ShardedCache`。
2. **权重只对 GDSF 生效**：其它策略的 `weight` 仅观测，不参与淘汰。
3. **LRU-K(2) 内存约 2×**（corral 与主缓存同容量），小容量下更明显。
4. **GDSF 的 `L` 不归一化**，长时间运行后数值单调增长；`H` 用 `double`，极端权重下精度下降。
5. **ShardedCache 无全局顺序**：跨分片的 LRU/ARC 语义、TinyLFU 频率都不再全局成立。
6. **无 TTL/过期/刷新语义**：过期需业务在 value 上自行判断。
7. **CLOCK 不实现 CLOCK-Pro**：没有 LIR/HIR 保护，扫描场景命中率与 LRU 同档。
8. **未做无锁/RCU 实现**：并发扩展性靠分片而非原子操作。
9. **不进入 `install(EXPORT)`**：只随 `include/` 目录安装；需要 `find_package` 导出时在
   `Go2CppInstall.cmake` 的 `GO2CPP_MODULE_TARGETS` 中追加 `go2cpp_cache`（INTERFACE）。

## 9. 与既有模块的边界

- 只依赖 C++17 标准库与 `Threads::Threads`；不链接 `go2cpp::runtime`、不依赖 Scheduler/IO；
- 与 `go2cpp::container`（开放寻址哈希表等）无耦合：cache 自带索引结构，便于独立对拍；
- 与 `mcp/` 的检索/向量缓存无耦合：MCP 侧有自己的缓存层，本模块是通用策略库；
- 不改动任何既有模块的源码、导出清单或构建默认值。

## 10. 变更记录

### 2026-10-31：初版模块文档

- 汇总 `include/go2cpp/cache/**` 的接口、7 策略差异、复杂度、测试覆盖与实测摘要；
- 把 `docs/research/cache-algorithms.md` 的论文对照与完整基准作为延伸阅读，不重复其正文；
- 明确三条边界：header-only、权重仅 GDSF、不进入 `install(EXPORT)`。
