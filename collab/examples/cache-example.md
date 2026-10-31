# 缓存能力示例（Go 标准库没有）

> Go2Cpp 提供 7 种缓存淘汰策略——**Go 标准库没有**（只有 `sync.Map`，无淘汰）。
> 这是 JinShan 可以直接复用的通用能力（如：invitation 缓存、会话缓存、文件块缓存）。

## 一、选型（根据场景）

| 场景 | 推荐策略 | 理由 |
|---|---|---|
| **Web/游戏/通用** | **W-TinyLFU** 或 **ARC** | 命中率最高（热点 0.78-0.79） |
| **大小差异大/CDN**（大文件/小文件混存） | **GDSF** | 带权重（`H = L + cost/size × freq`），CDN 命中率 0.748 |
| **扫描攻击**（冷数据冲刷热点） | **LRU-K(2)** 或 **2Q** | 防扫描（scan 场景 0.52 vs LRU 0.37） |
| **数据库/文件系统** | **LRU-K(2)** / **2Q** | 抗扫描 |
| **边缘/低开销** | **CLOCK** | 30 Mops/s（最快） |
| **多核高 QPS** | **ShardedCache** | 分片减竞争 |

## 二、Go 风格接口

```cpp
// 统一接口（模板）
template <typename K, typename V>
class Cache {
 public:
  virtual bool Get(const K& key, V* out) = 0;   // 命中返回 true
  virtual void Put(const K& key, V value) = 0;
  virtual bool Erase(const K& key) = 0;
  virtual Stats GetStats() const = 0;           // hits/misses/evictions/bytes
};

// 7 种实现
LruCache<K, V>       lru(capacity);
LruKCache<K, V>      lru_k(capacity, 2);        // 防扫描
TwoQueueCache<K, V>  two_q(capacity);           // 防扫描
TinyLfuCache<K, V>   tiny_lfu(capacity);        // W-TinyLFU
GdsfCache<K, V>      gdsf(capacity);            // 带权重
ArcCache<K, V>       arc(capacity);             // 自适应
ClockCache<K, V>     clock(capacity);           // 低开销

// 线程安全包装
ThreadSafeCache<K, V> ts(inner);
ShardedCache<K, V>    sharded(inner_factory, 16);  // 16 分片
```

## 三、示例 1：invitation 缓存（JinShan 场景）

```cpp
// 场景：每 Viewer 一个 invitation（XML 串），生成慢（COM 调用），要缓存
// 特点：读多写少、大小不一（XML 几百字节到几 KB）
ShardedCache<std::string, std::string> invitation_cache(
    [] { return std::make_unique<LruKCache<std::string, std::string>>(1024, 2); },
    8);  // 8 分片

// 取（命中直接用，未命中生成）
std::string invitation;
if (!invitation_cache.Get(viewer_id, &invitation)) {
    invitation = GenerateInvitation();   // 慢（COM）
    invitation_cache.Put(viewer_id, invitation);
}
```

## 四、示例 2：文件块缓存（JinShan 的 FR-18 文件传输）

```cpp
// 场景：文件分块下发（32 KiB/块），块大小固定，要防「大文件冲刷小文件」
// 特点：块大小固定（32 KiB）、总量大、有优先级（热文件块）
GdsfCache<uint64_t, std::vector<uint8_t>> block_cache(4096);  // 4096 块

// 取块
std::vector<uint8_t> block;
uint64_t block_id = file_id * 1'000'000 + chunk_index;
if (!block_cache.Get(block_id, &block)) {
    block = ReadBlock(file_id, chunk_index);   // 磁盘 IO
    block_cache.Put(block_id, std::move(block));  // GDSF 按 cost/size×freq 淘汰
}
```

## 五、示例 3：防扫描（冷数据冲刷）

```cpp
// 场景：攻击者用大量冷 key 冲刷缓存（JinShan 的 key 枚举攻击）
// LRU 会被冲垮（热点命中率 0.558 → 0.367）；LRU-K(2) 抗扫描（0.790 → 0.520）
LruKCache<std::string, Session> session_cache(10000, 2);
```

## 六、实测数据（Go2Cpp 基准，capacity=2000，300k ops）

| 场景 | LRU | LRU-K | 2Q | TinyLFU | GDSF | ARC | CLOCK |
|---|---:|---:|---:|---:|---:|---:|---:|
| 热点 | .558 | **.790** | .648 | .782 | .769 | .782 | .531 |
| 扫描 | .367 | **.520** | .425 | .506 | .485 | .510 | .351 |
| **权重 1:1000** | .558 | .791 | .649 | .784 | **.816** | .783 | .531 |
| **CDN** | .477 | .668 | .535 | .588 | **.748** | .556 | .461 |
| 混合 | .540 | **.654** | .643 | .500 | .427 | .640 | .532 |

**吞吐**（Mops/s）：CLOCK 30.2 / LRU 79.6（优化后）/ ARC 23.9 / GDSF 18.4 / 2Q 18.0 / TinyLFU 13.2 / LRU-K 6.7

## 七、JinShan 能直接用的地方

| JinShan 模块 | 缓存什么 | 推荐策略 |
|---|---|---|
| MOD-14/16（relay 配对） | key → session（配对表） | **LRU-K(2)**（防 key 枚举冲刷） |
| MOD-17/19（invitation） | viewer_id → invitation | **LRU**（读多写少） |
| MOD-25/26（命令/文件） | file_id → block | **GDSF**（大小差异 + 权重） |
| MOD-39（会话状态） | session_id → state | **W-TinyLFU**（热点） |

## 八、注意

- **容量有界**（所有策略都要求 capacity）；
- **GDSF 需条目上限**（实测 12 MB/44k 条——按容量控制）；
- **命中率逐位一致**（优化前后，无语义变化）；
- **线程安全**：用 `ThreadSafeCache`（或 Fiber 场景用 `ShardedCache`）。
