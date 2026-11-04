# 缓存模块 · 深入学习

> 面向「要接手改 `include/go2cpp/cache/**` 的人」。
> 行号全部对 `include/go2cpp/cache/**` 与 `tests/cache/**` 复核过；性能只写「本机怎么测」，不写结论数字。

---

## 五层地图（这篇怎么读）

| 层 | 回答的问题 | 本文位置 |
|---|---|---|
| 层 1 · What | 缓存抽象解决什么、7 种策略各自的承诺与代价 | 第一部分 |
| 层 2 · How | 容器层（BucketMap/IntrusiveList/NodePool）+ 装饰层的协作 | 第二部分 |
| 层 3 · 源码 | LRU / 2Q / TinyLFU 逐函数、逐指针、逐条指令 | 第三部分 |
| 层 4 · 实战 | 并发淘汰 / 容量为 0 / 析构与哈希抛异常 | 第四部分 |
| 层 5 · 性能 | 73 / 373 / 306 条指令、0 锁 0 分配、怎么复现 | 第五部分 |

每部分末尾有「小结」；小结只写结论与边界。

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

| 入口（类/函数） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `enum class Policy` | `include/go2cpp/cache/factory.hpp:35` | 配置/基准 | 选 7 种策略之一 | 枚举值 |
| `MakeCache<K,V>(policy, options)` | `include/go2cpp/cache/factory.hpp:72` | 业务、基准、`ShardedCache` | `new` 具体策略对象 | `unique_ptr<Cache<K,V>>` |
| `MakeThreadSafeCache<K,V,Lock>` | `include/go2cpp/cache/factory.hpp:100` | 多线程业务 | 策略外套 `ThreadSafeCache` | `unique_ptr<Cache>` |
| `ShardedCache(policy, options, shards)` | `include/go2cpp/cache/factory.hpp:117` | 多核高吞吐场景 | 按 key 哈希切分 N 个独立缓存 | 分片缓存对象 |
| `Cache::Get/Peek/Put/Erase/Clear` | `include/go2cpp/cache/cache.hpp:149`（接口从 149 行起） | 所有上层 | 改变/查询策略状态 | `bool`/`void` |
| `ThreadSafeCache` | `include/go2cpp/cache/cache.hpp:209` | 多线程 | 每个方法一把粗粒度锁 | 转发 inner 的结果 |
| `LruCache::Get/Put` | `include/go2cpp/cache/lru.hpp:64` / `:89` | `MakeCache` | 命中移到 MRU；满时淘汰尾 | 命中 bool |
| `LruKCache::Get/Put` | `include/go2cpp/cache/lru_k.hpp:90` / `:128` | 抗扫描场景 | 第一次进 corral，第二次进主表 | 命中 bool |
| `TwoQueueCache::Get/Put` | `include/go2cpp/cache/two_queue.hpp:96`（ghost 分支）/ `:128` | 抗一次性流量 | A1in→A1out→Am | 命中 bool |
| `TinyLfuCache::Get/Put` | `include/go2cpp/cache/tiny_lfu.hpp:94` / `:135` | 通用高命中 | 窗口 + SLRU + 频率准入 | 命中 bool |
| `GdsfCache::Get/Put` | `include/go2cpp/cache/gdsf.hpp:68` / `:103` | 大小差异大的对象 | `H=L+cost/size*freq` 加权淘汰 | 命中 bool |
| `ArcCache::Get/Put` | `include/go2cpp/cache/arc.hpp:100`（ghost）/ `:136` | 读写混合、要自适应 | 用 B1/B2 调 `p` | 命中 bool |
| `ClockCache::Get/Put` | `include/go2cpp/cache/clock.hpp:74` / `:100` | 条目小、QPS 极高 | referenced 位二次机会 | 命中 bool |
| `detail::BucketMap` | `include/go2cpp/cache/detail/bucket_map.hpp:63` | 所有策略做 key→节点索引 | 链式哈希桶 | 节点指针 |
| `detail::IntrusiveList` | `include/go2cpp/cache/detail/intrusive.hpp:69` | 各策略的 LRU/FIFO 链 | 只改指针，不分配 | — |
| `detail::NodePool` | `include/go2cpp/cache/detail/intrusive.hpp:242` | 所有策略的节点来源 | 块分配 + free list 复用 | `T*` |
| `detail::TimestampWheel` | `include/go2cpp/cache/detail/time_wheel.hpp:63` | 仅 LRU-K | 按 reference_time 找最老条目 | `Entry*` |
| `detail::CountMinSketch4` | `include/go2cpp/cache/detail/sketch.hpp:41` | 仅 TinyLFU | 4 行 4-bit 频率素描 | 估计值 |
| `detail::BloomDoorkeeper` | `include/go2cpp/cache/detail/sketch.hpp:143` | 仅 TinyLFU | 一次性 key 不污染素描 | 是否第一次 |

> 编译开关：本模块 header-only，无独立 `.cpp`/无 `GO2CPP_WITH_*`。测试目标由 `cmake/Go2CppCache.cmake` 注册（`go2cpp_cache_test` / `go2cpp_cache_bench`）。

### 1.2 总架构图

```
调用方（业务 / 基准 / MemCache）
        │  Cache<K,V> 接口（cache.hpp:149）
        ▼
┌──────────────────────────────────────────────┐
│ 装饰层（可选，二选一或叠加）                  │
│  ThreadSafeCache（cache.hpp:209，单锁）       │
│  ShardedCache（factory.hpp:117，N 片各自锁）  │
└──────────────────────┬───────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────────────┐
│ 策略核心（无锁，单线程语义）                                  │
│  LRU / LRU-K / 2Q / TinyLFU / GDSF / ARC / CLOCK             │
│  ┌───────────────┐   ┌─────────────────────────────────┐    │
│  │ 条目索引       │   │ 策略元数据                       │    │
│  │ BucketMap     │   │ LRU-K: TimestampWheel            │    │
│  │ (bucket_map)  │   │ TinyLFU: Sketch + Doorkeeper     │    │
│  └───────┬───────┘   │ GDSF: priority_queue + version   │    │
│          │           │ ARC: p + T1/T2/B1/B2            │    │
│          ▼           └─────────────────────────────────┘    │
│  IntrusiveList（LRU/FIFO/ghost 链，只改 prev/next）           │
│  NodePool（64/块，稳态 Create/Destroy 命中 free list，0 malloc）│
└─────────────────────────────────────────────────────────────┘
        │ 不调用任何 syscall；不持有文件/网络 fd
        ▼
   纯用户态内存结构（无 I/O、无 Fiber、无锁；锁只在上面的装饰层）
```

线程边界：策略核心**不是**线程安全的（`cache.hpp:23`）；`ThreadSafeCache` 一次只允许一个读者/写者（`cache.hpp:25`）；`ShardedCache` 按 key 分片，跨片无全局顺序。

### 1.3 复杂场景详解

#### 场景 A：LRU 一次 `Get` 命中（12 步）

1. 业务调用 `Cache::Get`（虚接口，`cache.hpp:158`）→ 实际进入 `LruCache::Get`（`lru.hpp:64`）。
2. `LruCache::Get` 先 `index_.Find(key)`（`lru.hpp:65` 附近）→ `BucketMap::Find`（`bucket_map.hpp:87`）算 `HashKey`，再 `FindHashed` 沿桶链比较 key（`bucket_map.hpp:92`）。
3. 未命中：`stats_.RecordMiss()`，返回 false；不改任何链表。
4. 命中：拿到 `Entry*`，先判断 value 指针是否为空（允许只查存在性）。
5. 命中统计：`stats_.RecordHit()`。
6. 把条目移到链表头：`lru_.move_to_front(entry)`（`lru.hpp:75` 附近 → `intrusive.hpp:140`）。
7. `move_to_front` 先 `erase(node)`（`intrusive.hpp:133`，只改 4 个指针），再 `push_front`（`intrusive.hpp:87`）；节点不重新分配。
8. 写回 `*value = entry->value`（拷贝；`V` 大时这是主要成本）。
9. 返回 true。
10. 若上层是 `ThreadSafeCache::Get`（`cache.hpp:224`），整段在 `lock_guard<Lock>` 内；锁在函数返回时释放。
11. 若上层是 `ShardedCache::Get`（`factory.hpp:151`），先 `ShardIndex(key)`（`factory.hpp:240`）选片，再拿该片自己的 mutex。
12. 失败/阻塞路径：本模块不阻塞、不抛异常（除构造时参数非法）；`Get` 只有 true/false。

#### 场景 B：TinyLFU 一次 `Put` 触发准入/淘汰（14 步）

1. 调用 `TinyLfuCache::Put`（`tiny_lfu.hpp:135`），传入 key/value/weight。
2. 先查 `index_`：key 已存在 → 更新 value、`stats_.RecordUpdate()`，若是窗口条目则移到 window 头，返回。
3. 新 key：计算 64 位混合哈希，作为 `sketch_hash` 存进 `Entry`。
4. 记录频率：`RecordFrequency`（`tiny_lfu.hpp:306` 附近）先 `doorkeeper_.TestAndSet(hash)`（`sketch.hpp:153`）——第一次出现只置 Bloom 位，不写素描。
5. 第二次出现才 `sketch_.Increment(hash)`（`sketch.hpp:57`，4 路饱和到 15）。
6. 每累计 `sample_size_` 次记录，`sketch_.Halve()`（`sketch.hpp:100`）：所有 4-bit 计数器右移一位，历史频率指数衰减。
7. 新条目先 `window_.push_front(fresh)`（`tiny_lfu.hpp:172`），window 容量是总容量的 1%（`WindowCapacity`，构造在 `tiny_lfu.hpp:67`）。
8. 若 `window_.size() > window_capacity_`（`tiny_lfu.hpp:325`）：取 window 尾部作为 candidate，`window_.erase(candidate)`。
9. 若主缓存容量为 0（`main_capacity_ == 0`，`tiny_lfu.hpp:157`），candidate 直接丢弃并 `RecordEviction`。
10. 否则进入 `AdmitToMain(candidate,false)`（`tiny_lfu.hpp:350`）：若主缓存未满，candidate 进 probation；满则取 probation 尾部 victim。
11. 频率比较：`Estimate(candidate) > Estimate(victim)` 才允许替换；否则 `RecordRejection` 后丢弃 candidate（数据没进缓存）。
12. 允许替换：`EvictProbationTail()`（`tiny_lfu.hpp:414`）→ 摘索引、`pool_.Destroy(entry)`、`RecordEviction()`。
13. candidate 进 probation 头；`stats_.RecordInsert()`。
14. 失败路径：`Put` 返回 void——被拒绝时调用方**不会**收到错误，只能看 `Stats().rejections`；`weight` 超过 `max_weight` 时同样静默拒绝。

#### 场景 C：LRU-K 第二次访问晋升主表（12 步）

1. 首次 `Put`：新条目进 corral FIFO（`lru_k.hpp:128` 起），`in_main=false`。
2. corral 容量 `history_limit_ = max(4, capacity)`（`lru_k.hpp:74` 注释解释了为什么和主表同容量）。
3. corral 满：`pop_front` 最老条目，`pool_.Destroy`，`RecordEviction`。
4. 第一次 `Get`：命中 corral 条目（`lru_k.hpp:90`），`last_visit` 更新，但**不**晋升。
5. 第二次 `Get`：`PromoteToMain`（`lru_k.hpp:242`）把它从 corral 摘出。
6. 晋升时用 `reference_time = last_visit`（倒数第二次访问时间，`lru_k.hpp:237`），插入 `TimestampWheel`（`time_wheel.hpp:87`）。
7. 主表满：从时间轮 `Min()`（`time_wheel.hpp:84`）取 reference_time 最小的条目淘汰。
8. `Min()` 读锦标赛树根；树是 256 叶对应 256 桶，桶内 min 是链表的绝对最小。
9. 淘汰时 `Erase(entry)`（`time_wheel.hpp:110`）从桶链摘除并刷新桶 min + 自底向上刷 8 层树。
10. 桶号 = `(reference_time >> 8) & 255`；跨 65536 tick 的条目会落回同一桶，靠桶内绝对时间戳比较避免误判（`time_wheel.hpp:63` 注释）。
11. 命中主表：`move_to_front` 不适用（主表按 reference_time 排序），要 `Erase` 旧时间、改 `reference_time`、再 `Insert` 新时间。
12. 失败路径：容量 0 构造抛异常；`Erase`/`Clear` 必须遍历 `index_.ForEach` 逐个 `pool_.Destroy`（`lru_k.hpp:81` 析构即此逻辑），否则非平凡 `V` 泄漏。

### 1.4 关键不变量（破一条就出 bug）

| # | 不变量 | 位置 | 破了会怎样 |
|---|---|---|---|
| 1 | `index_.size()` 等于条目数，每个 key 只出现一次 | `bucket_map.hpp:75` `size_` | 双插入导致链表断裂/重复淘汰 |
| 2 | 每个 `Entry` 同一时刻只属于一条侵入式链（lru/window/probation/T1/T2/corral） | `intrusive.hpp:69` | prev/next 被覆盖，链成环或丢节点 |
| 3 | NodePool 的 `Create` 与 `Destroy` 必须配对；`Clear` 前先逐个 Destroy | `intrusive.hpp:263` / `:276` / `:282` | 非平凡 `V` 不析构（泄漏）；池内 double-free |
| 4 | GDSF 的 `version_` 单调递增，命中必须重写 `entry.version` | `gdsf.hpp:84` | 旧堆节点被当成当前值，热条目被自己淘汰 |
| 5 | 2Q 的 pending 只是「下次 Put 进 Am 的凭证」，不等于数据在缓存 | `two_queue.hpp:141` | 把 ghost 命中当命中，返回未加载的数据 |
| 6 | ARC 的 `p_ ∈ [0, capacity]`，B1 增 / B2 减 | `arc.hpp:112` | 方向调反，缓存偏向错误的工作集 |
| 7 | TinyLFU 的 `window + main` 总量受 `capacity_` 约束 | `tiny_lfu.hpp:439` | 段间搬家导致超容量 |
| 8 | 裸策略类不可并发调用；并发必须走装饰层 | `cache.hpp:23` | 链表/索引损坏、统计丢更新 |
| 9 | `ShardedCache` 的 `Capacity()` 是各片实际容量之和 | `factory.hpp:190` | 调用方以为总容量等于配置值 |
| 10 | `Stats()` 是累计值，`Clear()` 不清统计 | `cache.hpp:78` | 基准分段统计出错 |

---

### 1.5 层 1 小结：缓存抽象的四个承诺与四个不承诺

**承诺**：

1. `Get` 命中返回 true 并给出 value；未命中返回 false，不改 value。
2. 容量上限由 `CacheOptions::capacity` 约束（`ShardedCache` 是各片之和）。
3. 淘汰顺序由策略定义，且**只由策略层决定**；装饰层不改变顺序。
4. 统计口径固定：`hits+misses` 是命中率分母，`rejections`/`evictions` 不计入。

**不承诺**：

1. **不承诺 `Put` 一定进缓存**：TinyLFU 频率准入、GDSF 超重、2Q/ARC 的 pending 都可能静默拒绝；接口故意不返回 bool。
2. **不承诺线程安全**：策略核心无锁、非线程安全（`cache.hpp:23`）；要么 `ThreadSafeCache`，要么 `ShardedCache`，要么调用方自己加锁。
3. **不承诺跨片全局顺序**：分片后 LRU/ARC 顺序、TinyLFU 频率都不再全局。
4. **不承诺 `Clear()` 清统计**：统计是累计值（`cache.hpp:78`）。

一句话：**它是「命中率数据结构」，不是「分布式缓存」**——没有过期、没有回源、没有持久化。
## 第二部分 · 分：逐块细讲

### 2.1 接口与装饰器（`cache.hpp`）

**职责**：定义 `Cache<K,V>` 的十个纯虚函数，提供 `StatsRecorder` 与 `ThreadSafeCache`。

**关键结构**：
- `CacheStats`（`cache.hpp:58` 起）：`hits/misses/inserts/updates/evictions/ghost_hits/rejections/removals` + 水位 `size/capacity/weight/max_weight/ghost_size`。
- `StatsRecorder`（`cache.hpp:104`）：8 个 `uint64_t` 计数器，`Snapshot()` 只拷贝计数，水位由各策略补。
- `ThreadSafeCache`（`cache.hpp:209`）：`mutable Lock mutex_` + `unique_ptr<Cache> inner_`；`Capacity()`/`Name()` 不加锁（不读可变状态），其余都加。

**状态机**：无。**失败路径**：构造时 `inner_` 为空抛 `invalid_argument`；运行时不抛。

**典型误用**：把 `ThreadSafeCache` 当读多写少优化——它连 `Peek/Get` 都拿同一把互斥锁，多核下是串行点；要高吞吐应用 `ShardedCache`（代价见 2.10）。

### 2.2 LRU（`lru.hpp`）

**职责**：基线策略，按最近访问时间淘汰；所有其它策略的对照。

**关键字段**（`lru.hpp:177`）：
```cpp
detail::IntrusiveList<Entry> lru_;   // head=MRU, tail=LRU
detail::BucketMap<K, Entry, Hash, Equal> index_;
detail::NodePool<Entry> pool_;
```
**关键函数**：`Get`（`:64`）Find→RecordHit→move_to_front→拷 value；`Put`（`:89`）已存在则更新+移到头，新条目满时先 `pop_front` 尾再 `Destroy`；`Erase`（`:110`）从两条结构摘除后 `Destroy`；`Clear`（`:123`）遍历销毁。

**失败路径**：容量 0 构造抛异常（各策略一致）。**已知限制**：对顺序扫描零抵抗——一次全表扫描把所有热点挤出（考试 Q05 的对照基线）。

**典型误用**：用 `Get` 做「只查询」——它会改 LRU 顺序；只想看用 `Peek`。

### 2.3 容器层（`detail/bucket_map.hpp`、`detail/intrusive.hpp`）

**BucketMap**：链式哈希（不是开放寻址，尽管某些注释这样写），`Find`（`:87`）/`Insert`（`:103`）/`Erase`（`:119`）/`Clear`（`:134`）。桶数按 `expected*2` 向上取 2 的幂（`:152`），`mask_` 做取模。节点由策略的 NodePool 提供，`Insert` 只挂链。
**IntrusiveList**：head/tail/count；`push_front`（`:87`）、`pop_front`（`:115`）、`erase`（`:133`）、`move_to_front`（`:140`）都只改指针；跨链搬家用 `move_to_front(src,node)`（`:158`）。
**NodePool**：`kBlockSize=64`（`:241`），`Create`（`:263`）先 free list 再块内 bump，`Destroy`（`:276`）只 `push_back` 到 free list、**不析构内存**（调用方负责 `T` 析构？实际 `Destroy` 会显式调用析构——见实现），`Clear`（`:282`）释放所有块。

**不变量**：节点只能属于一条链；`Destroy` 后不能再访问；池不缩容（RSS 不降）。
**典型误用**：`Clear()` 直接 `pool_.Clear()` 而不逐个 `Destroy`——非平凡 `V` 不析构。

### 2.4 LRU-K 与 TimestampWheel（`lru_k.hpp`、`detail/time_wheel.hpp`）

**职责**：K=2，用「倒数第二次访问时间」排序，抗扫描。
**结构**：corral FIFO（容量 `max(4,capacity)`）+ 主表（`TimestampWheel` + `BucketMap` 合并索引，`in_main` 标记）+ 节点池。
**状态机**：

| 当前 | 事件 | 下一步 |
|---|---|---|
| 不在缓存 | Put | corral 尾 |
| corral | 第 1 次 Get | 仍 corral（更新 last_visit） |
| corral | 第 2 次 Get | 晋升主表，reference_time=last_visit |
| 主表 | Get | 重插时间轮（新 reference_time） |
| 主表 | 容量满 | 淘汰时间轮 Min |

**时间轮**：256 桶（`time_wheel.hpp:63`）、桶内链 + 256 叶锦标赛树；`Min()` O(1)（`:84`）；跨圈靠绝对时间戳比较，不靠桶号。
**失败路径**：容量 0 抛异常；corral 溢出丢最老（不是错误）。
**典型误用**：对冷数据主动 `Put`——重复写也会晋升，LRU-K 的扫描抗性只对「读未命中+按需加载」有效。

### 2.5 2Q（`two_queue.hpp`）

**职责**：A1in（FIFO 准入）/ A1out（ghost，只存 key）/ Am（LRU 主缓存）。
**关键行**：Get 先查 Am/A1in（`:90` 附近），再查 ghost（`:96`）；ghost 命中写 pending（`:107`）后返回 false（`:110`）；Put 先 `TakePending`（`:141`）决定进 Am 还是 A1in。
**状态机**：新 key→A1in；A1in 满→最老进 A1out（ghost）；Get 命中 ghost→pending；下一次 Put→Am；Am 满→LRU 淘汰。
**失败路径**：ghost 容量 `capacity/2+1`（构造 `:59`），pending 容量同 ghost；满了丢最老（`:103`）。
**典型误用**：认为 ghost 命中会返回数据；认为 Get 会自动加载。

### 2.6 TinyLFU（`tiny_lfu.hpp` + `detail/sketch.hpp`）

**职责**：窗口 LRU（1%）+ SLRU（probation 20% / protected 80%）+ 频率准入。
**关键行**：构造算四段容量（`:67`-`:70`）；Get 命中窗口/probation 提升 protected（`:107` 起）；Put 先落窗口（`:172`），窗口满取 candidate（`:325`），`AdmitToMain`（`:350`）比较频率；`EvictProbationTail`（`:414`）/`EvictWindowTail`（`:426`）。
**Sketch**：`CountMinSketch4`（`sketch.hpp:41`）4 行 4-bit、饱和 15；`Increment`（`:57`）/`Halve`（`:100`）/`Estimate`；`BloomDoorkeeper`（`:143`）第一次只置 Bloom。
**失败路径**：`Put` 无返回值，准入失败只记 `rejections`；`main_capacity_==0` 时窗口条目直接丢。
**典型误用**：容量很小时期待 TinyLFU 有效——窗口/probation 都被压到 1 条，退化明显。

### 2.7 GDSF（`gdsf.hpp`）

**职责**：按 `H = L + cost/size * freq` 淘汰，适合对象大小差异大。
**字段**：`index_`（unordered_map，权威）+ `heap_`（priority_queue 候选）+ `l_`（年龄水位）+ `version_`。
**关键函数**：Get（`:68`）freq++/重算 key_value/version++/push；Put（`:103`）更新时先摘旧再 `MakeRoomForInsert`（`:250`）；EvictOne（`:262` 起）懒惰删除，version 不匹配就 `continue`（`:264`）；`MaybeCompact`（`:276`）堆超过 `size*4+64` 重建。
**失败路径**：`weight==0` 按 1；`weight>max_weight` 直接拒绝；`capacity==0 && max_weight==0` 构造抛异常。
**典型误用**：以为堆里每 key 一条；忘记命中要 push 新版本。

### 2.8 ARC（`arc.hpp`）

**职责**：T1/T2 常驻 + B1/B2 ghost，用 ghost 命中调 `p`。
**关键行**：Get ghost 分支（`:100`）；B2 命中 p 减（`:104`-`:107`）；B1 命中 p 增（`:109`-`:112`）；Put ghost 分支（`:159`）`EvictForInsert` + `PushT2`。
**状态机**：新 key→T1；T1 满→尾进 B1；Get 命中 T1→T2；T2 满→尾进 B2；ghost 命中→pending，下次 Put 进 T2。
**失败路径**：`capacity==0` 抛异常；ghost 表容量 = capacity，`Erase` 会删 ghost + pending。
**典型误用**：p 方向调反；以为 ghost 命中返回数据。

### 2.9 CLOCK（`clock.hpp`）

**职责**：环 + referenced 位，低开销近似 LRU。
**字段**：`slots_`（vector<Slot>，`Slot` 含 optional<V>/IndexNode*/referenced/weight，`:202`）、`free_slots_`、`hand_`、`index_`、`pool_`。
**关键函数**：Get（`:74`）命中置 `referenced=true`（`:81`）；Put（`:100`）先找空槽，否则 `FindVictim`（`:213`）扫描；`FindVictim` 遇 1 清 0 跳过（`:221`）、遇 0/空槽选中；`Erase`（`:137`）归还槽位。
**失败路径**：容量 0 抛异常；全 1 时最多扫 2×capacity。
**已知限制**：扫描污染后恢复能力差（考试 Q05）。
**典型误用**：当严格 LRU 用；期待扫描后自动恢复。

### 2.10 ShardedCache（`factory.hpp`）

**职责**：按 key 哈希分片，每片独立锁。**容量切分**：`base/extra`（`:123`），空片提到 1（`:128`），`capacity_` 累加实际值（`:143`）。**语义损失**：全局 LRU/ARC 顺序、TinyLFU 频率素描都不再跨片。
**失败路径**：`shards=0` 被 `max(1,shards)` 兜底（`:120`）；单片容量 0 被提到 1。
**典型误用**：`capacity < shards` 时以为总容量不变；用分片跑需要全局频率的负载。

### 2.11 基准与负载（`tests/cache`）

- `cache_test`：单元（每策略一个 TU）+ 随机 + 5 场景 + 压测；`Announce` 打印场景名。
- `cache_bench`：`--quick`（ctest 默认）/`--full`/`--ops=/--capacity=`；输出命中率、recovery、Mops/s、peak_bytes。
- 本机怎么测（不写结论）：
  ```bash
  cd .build-bento-wsl
  ./go2cpp_cache_test
  ./go2cpp_cache_bench --quick
  ```
  记录 CPU 型号、是否绑核、轮数；换机器必须重测。

### 2.12 统计口径：`hits/misses` 与 `ghost_hits/rejections` 不是一回事

| 事件 | 计入 | 不计入 | 代码位置 |
|---|---|---|---|
| `Get` 命中真实条目 | `hits++` | — | 各策略 `RecordHit` |
| `Get` 未命中（含 ghost 命中） | `misses++` | — | `two_queue.hpp:110`、`arc.hpp:117` |
| 2Q/ARC ghost 命中 | 额外 `ghost_hits++` | 不增加 hits | `two_queue.hpp:109` |
| TinyLFU 准入被拒 | `rejections++` | 不算 miss（Put 不计 Total） | `tiny_lfu.hpp:350` 起 |
| GDSF `weight>max_weight` | `rejections++` | 不算 miss | `gdsf.hpp:107` |
| `Peek` | 什么都不计 | hits/misses 都不加 | `cache.hpp:160` |
| `Clear()` | 只清条目/权重 | **不清统计** | `cache.hpp:78` |

推论：`HitRate()` 的分母是 `hits+misses`，不含 `rejections`；「Put 被拒绝」不会让命中率变难看，但会让调用方以为缓存成功。基准里要同时看 `rejections` 与 `size`，否则会把「没缓存」误读成「缓存了但没命中」。

### 2.13 策略选择矩阵（按语义，不按性能数字）

| 负载特征 | 首选 | 原因（来自实现语义） | 已知反例 |
|---|---|---|---|
| 均匀随机、无扫描 | LRU | 最简单、命中率基线 | 周期扫描会把热点挤光 |
| 有周期性全表扫描 | LRU-K / 2Q / TinyLFU | 二次访问才晋升 / ghost 凭证 / 频率准入 | 冷数据被主动 Put 时仍会晋升 |
| 对象大小差异 1:1000 | GDSF | `cost/size*freq` 按收益密度淘汰 | 大小相近时不如 LRU 简单 |
| 读写混合、工作集漂移 | ARC | ghost 命中自动调 `p` | `p` 方向/容量极小时震荡 |
| 小条目、极高 QPS、能接受近似 | CLOCK | 无链表、命中只写 1 bit | 扫描后恢复差 |
| 多核吞吐优先 | ShardedCache | 每片独立锁 | 全局顺序/频率丢失、容量可能放大 |
| 需要绝对线程安全且简单 | ThreadSafeCache | 一把锁，语义不变 | 读也串行，16 核上是瓶颈 |

### 2.14 改这个模块前的检查清单

1. 新增策略：先实现 `Cache<K,V>` 全部纯虚函数（`cache.hpp:149`），再在 `MakeCache`（`factory.hpp:72`）注册，最后补 `tests/cache/test_<policy>.cpp` 与 `Announce` 场景。
2. 任何 `Entry` 只能挂一条链；跨链搬家必须用 `IntrusiveList::move_to_front/back`（`intrusive.hpp:158`），不能手动改 `prev/next`。
3. 析构/Clear 必须先 `index_.ForEach` 逐个 `pool_.Destroy`，再 `pool_.Clear`（参考 `lru_k.hpp:81`）；否则非平凡 `V` 泄漏。
4. 统计只在策略层记；装饰层不要重复计数（`ThreadSafeCache` 只转发）。
5. 新策略若要 ghost/pending，容量必须从 `CacheOptions` 推导并写进 `Stats().ghost_size`。
6. 任何「可能不插入」的 `Put` 语义必须记 `rejections`；不要改 `Put` 签名返回 bool（接口已固定）。
7. 分片/线程安全改动只放装饰层，不改策略核心（策略核心保持无锁单线程语义）。
8. 性能结论必须附命令与环境（见 2.11），文档里不写「比 X 快 N 倍」这种无出处的数字。

### 2.15 场景 D：`ShardedCache` 一次并发 `Get`（12 步）

1. 构造 `ShardedCache(policy, options, shards)`（`factory.hpp:117`），`shard_count_ = max(1, shards)`（`:120`）。
2. 计算每片容量 `base = capacity/shard_count_`、`extra = capacity%shard_count_`（`:123`-`:124`）。
3. 空片容量强制为 1（`:128`-`:129`），所以 `capacity < shards` 时总容量被放大。
4. 每片 `MakeCache(policy, shard_options)`（`:142`），`capacity_ += cache->Capacity()`（`:143`）。
5. 业务调用 `ShardedCache::Get`（`factory.hpp:151`）。
6. `ShardIndex(key)`（`:240`）用 `std::hash<K>` 取模选片。
7. `std::lock_guard<std::mutex>` 锁住该片（`:153`），不同 key 落在不同片时互不阻塞。
8. 片内 `inner_->Get` 执行策略逻辑（LRU 移动、TinyLFU 频率等），统计也记在片内。
9. 命中返回 true，解锁；片内 `StatsRecorder` 不共享，无需原子。
10. `Stats()`（`:200` 起）逐片加锁、把各片统计相加；这不是原子快照。
11. `Capacity()`（`:190`）直接返回成员，不加锁——容量构造后不变。
12. 失败路径：`std::hash<K>` 抛异常会直接穿透（没有 catch）；`Get` 本身不抛。

### 2.16 `Put` 失败矩阵（为什么「没报错」不等于「缓存成功」）

| 策略 | 失败条件 | 结果 | 可观察量 | 行号 |
|---|---|---|---|---|
| 全部 | 容量已满且无法淘汰 | 条目未插入 | `size` 不增 | 各策略 |
| TinyLFU | 候选频率 ≤ victim | 候选被丢弃 | `rejections++` | `tiny_lfu.hpp:350` |
| GDSF | `weight > max_weight` | 直接拒绝 | `rejections++` | `gdsf.hpp:107` |
| GDSF | 堆空且 `CanFit` 失败 | 拒绝 | `rejections++` | `gdsf.hpp:250` |
| 2Q | ghost 命中的 key 没有后续 Put | 只在 pending，没有数据 | `ghost_hits++`、`misses++` | `two_queue.hpp:110` |
| ARC | ghost 命中后没有 Put | 只在 pending | `ghost_hits++`、`misses++` | `arc.hpp:117` |
| Sharded | 哈希到某片但该片满 | 该片自己淘汰 | 片内 `evictions` | `factory.hpp:163` |

写业务代码时：需要「是否真的缓存了」的语义，必须在 `Put` 后 `Peek` 或看 `Stats().rejections`；接口不返回 bool 是有意为之（避免调用方为每次写分支）。

### 2.17 建议阅读顺序

1. `cache.hpp:149` 接口 → 2. `lru.hpp` + `detail/intrusive.hpp`（容器层）→ 3. `detail/bucket_map.hpp`；
2. `lru_k.hpp` + `detail/time_wheel.hpp`（最复杂的索引）；
3. `tiny_lfu.hpp` + `detail/sketch.hpp`（准入语义）；
4. `two_queue.hpp` / `arc.hpp`（ghost/pending 状态机）；
5. `gdsf.hpp`（惰性堆）；
6. `factory.hpp`（分片与线程安全边界）；
7. 最后跑 `tests/cache/test_scenarios.cpp`，把每个场景的 baseline/recovery 与代码对上。

### 2.18 三条「代码注释里写着」的真实踩坑（改代码前必读）

1. **GDSF 的 Get 曾经把自己淘汰**：旧实现只更新 map 里的 `key_value`、不 push 新堆节点也不递增 version，结果堆顶还是该 key 的旧代价，下一次淘汰就选中它。现注释在 `gdsf.hpp:79`-`:82`，修复是把 `version` 纳入堆节点并在 `EvictOne` 校验（`:264`）。
2. **TinyLFU 段间搬家曾经每次命中 3.93 次 `operator new`**：旧实现三段各一张 `unordered_map` + `std::list`，窗口命中提升/protected 降级都要新建 list 节点；现在三段共用一张 `BucketMap` + NodePool，段标记在条目里，搬家只改指针（`tiny_lfu.hpp` 文件头「容器层」段）。改这段时不要退回「每段一条 list」。
3. **2Q 的 Get 曾经查三次哈希**：旧顺序是 am -> a1in -> ghost，三个独立表；现在 A1in/Am 共用一张索引（条目带段标记），Get 降为两次查找（`two_queue.hpp:27`）。新增字段时别把两段又拆回两张表，否则热路径多一次哈希。

这三条都不是「性能优化故事」，而是**语义曾经坏掉**的记录：第 1 条会淘汰热数据，第 2/3 条会改变命中率与内存行为。改任何一条路径前先读对应注释。

### 2.19 术语对照与边界（读论文/注释时别混）

| 本实现 | 论文/常见叫法 | 边界（必须知道） | 位置 |
|---|---|---|---|
| corral | LRU-K 的 history queue | 容量 = `max(4,capacity)`，不是论文的「小比例」；总内存约 2×capacity | `lru_k.hpp:74` |
| A1out | 2Q 的 ghost queue | 只存 key，不存 value；命中仍是 miss | `two_queue.hpp:96` |
| pending | ghost 命中凭证 | 容量 = ghost 容量；没有后续 Put 就永远不提升 | `two_queue.hpp:103` |
| window | W-TinyLFU 的 window LRU | 1% 容量；容量很小时被压到 1 条 | `tiny_lfu.hpp:67` |
| probation/protected | SLRU 两段 | probation 20% / protected 80%；protected 满会降级 | `tiny_lfu.hpp:69` |
| `L` | GDSF 的 age/inflation | 淘汰时 `L += victim.key_value`，只影响相对顺序 | `gdsf.hpp:265` 附近 |
| `p` | ARC 的 T1 目标 | 单位是条目数，clamp 到 `[0,capacity]`；B1 增/B2 减 | `arc.hpp:112` |
| referenced | CLOCK 的 reference bit | 只有 1 bit，没有计数；扫描后恢复能力差 | `clock.hpp:81` |
| ghost_size | Stats 的 ghost 水位 | 2Q/ARC 是 ghost 表；GDSF 是「堆项超出条目数」的近似 | `gdsf.hpp` Stats |
| peak_bytes | 基准里的峰值内存 | 由测试的自定义 allocator 统计，不是进程 RSS | `tests/cache/cache_bench.cpp` |

三个常见混淆：
1. 「ghost 命中」不是「缓存命中」——2Q/ARC 都返回 false，数据要调用方自己加载再 Put。
2. 「rejections」不是「evictions」——前者是没进缓存，后者是进了又被淘汰；命中率分母都不含二者。
3. 「容量」在 `ShardedCache` 下是各片实际容量之和，可能与配置不同（2.10）。




### 2.20 层 2 小结：容器层决定了策略的上限

1. **索引只能是哈希**：`BucketMap` 链式 + `mask_` 取模，查找 O(1) 但有一次指针追逐；没有有序索引，所以任何「按分数找最小」的策略（LRU-K/GDSF）都要额外结构（时间轮/堆）。
2. **链表只能侵入式**：`Entry : Link<Entry>`，一个节点同一时刻只能挂一条链；这是「段间搬家只改指针」的前提，也是「重复挂链就断链」的根因。
3. **节点只能来自 NodePool**：64 个一块、`Destroy` 不归还内存只挂 free list，所以稳态 0 分配；代价是 RSS 不缩容。
4. **装饰层只做锁与分片**：`ThreadSafeCache` 一把锁、`ShardedCache` N 把锁；它们不改变策略语义，也不做跨片协调。

这四条决定了：**策略的正确性主要不在算法，而在「节点只属于一条链、Create/Destroy 配对、索引与链同步」**。层 3 的逐函数拆就是围绕这三件事。

---

## 第三部分 · 层 3：LRU / 2Q / TinyLFU 逐函数

> 行号是 2026-11-04 在真树上 `sed -n` 复核的值。现有 1.1 入口表里的
> `lru.hpp:64`、`two_queue.hpp:96`、`tiny_lfu.hpp:94` 是更早版本的口径，
> 与当前代码差 20–30 行；**改代码时以下面的行号为准**，读旧表要换算。
> 指令数出自 `docs/reports/asm-cache.md`（`int/int` 实例化、`-O3`、GCC 13.3.0）。

### 3.1 LRU：73 条指令的命中路径

**结构**（`lru.hpp:250` 附近）：

```cpp
detail::IntrusiveList<Entry> lru_;   // head=MRU, tail=LRU
detail::BucketMap<K, Entry, Hash, Equal> index_;  // 链式哈希
detail::NodePool<Entry> pool_;       // 64/块
```

**`Get`（`lru.hpp:90`）逐行**：

1. `index_.Find(key)`：先 `HashKey`（64 位 mix 哈希），再 `FindHashed` 沿桶链比较。**一次哈希 + 平均 1.0–1.3 次节点比较**。
2. 未命中 → `RecordMiss()`，返回 false。不改链表、不分配。
3. 命中 → `RecordHit()`（一条 `addq $1, ...`）。
4. `lru_.move_to_front(entry)`：`erase`（4 个指针写）+ `push_front`（4 个指针写），**没有节点分配/释放**。
5. `*value = entry->value`：一次拷贝；`V=std::string` 时这是最大成本。

**为什么只有 73 条**（`asm-cache.md` §2.1）：整条路径 0 个 call、0 B 栈帧、0 个 `lock`。73 条里包含未命中分支（RecordMiss + `xor eax,eax` + `ret`）。对比 `std::list + unordered_map` 版本，命中要做一次 `unordered_map::find`（可能一次堆指针追逐）+ `list::splice`（6 个指针），指令数约 2–3 倍。

**`Put`（`lru.hpp:139`）**：227 条、4 个 call、88 B 栈帧。4 个 call 的落点：

| 符号 | 触发条件 |
|---|---|
| `operator new` | NodePool 扩块（每 64 个一次，稳态 0） |
| `vector::_M_realloc_insert` | 节点块表扩容（每 64 个一次） |
| `__stack_chk_fail` | 金丝雀失败（不可达） |

稳态（容量满、free list 非空）下 `Put` 走 `pool_.Create` 的自由表分支，**0 堆分配**。这解释了为什么「插入 0 分配」不是营销话：反汇编里 `operator new` 的调用点是分摊的。

**淘汰**：`Put` 满时 `pop_front` 尾节点 → `index_.Erase` → `pool_.Destroy`。`Destroy` 把节点挂回 free list，**不调用 `delete`**，但会显式调用 `V` 的析构（否则非平凡 value 泄漏）。Evict 没有独立符号，被内联进 `Put`。

**并发**：`Get`/`Put` 的 `lock` 前缀为 0。裸 `LruCache` 不是线程安全的；`lru.hpp:86-88` 写明由调用方保护。

**已知限制**：对顺序扫描零抵抗。一次全表扫描把 100% 命中打成 0%，且没有恢复机制（LRU 只看最近一次访问）。要抗扫描换 LRU-K/2Q/TinyLFU。

### 3.2 2Q：373 条指令 = 两次哈希的代价

**结构**（`two_queue.hpp`）：

| 段 | 容器 | 容量 | 存什么 |
|---|---|---|---|
| `am` | LRU 链 | 主体 | 真数据（热） |
| `a1in` | FIFO 链 | 一部分 | 真数据（新） |
| `a1out`（ghost） | FIFO 链 | `capacity/2+1` | **只存 key**，不存 value |
| `pending_` | 集合 | 同 ghost | ghost 命中的凭证 |

**`Get`（`two_queue.hpp:108`）逐行**：

1. 查 `am`：命中 → `move_to_front`、`RecordHit`、拷 value、返回 true。
2. 未命中 → 查 `a1in`：命中 → 移到 `am` 头（**晋升**）、`RecordHit`、返回 true。
3. 仍未命中 → 查 ghost：命中 → `RecordGhostHit` + `pending_.insert(key)`，**返回 false**；未命中 → `RecordMiss`，返回 false。

**为什么是 373 条**（LRU 的 5.1 倍）：两次独立哈希 + 两次桶链探测。第 3 步的 ghost 查是第三次哈希（如果 ghost 与主表不共用索引）。这是 2Q 用「多一次查找」换扫描抗性的必然成本，不是退化。

**`Put`（`two_queue.hpp:179`）**：251 条，比 `Get` 少。分支：

- key 已在 `am` → 更新值 + 移到头。
- key 在 `a1in` → 更新值 + 移到 `am` 头。
- `TakePending(key)` 为真（ghost 命中）→ `ReclaimForInsert()` → 直接进 `am` 头。
- 否则 → `ReclaimForInsert()` → 进 `a1in` 头；`a1in` 满时最老条目进 ghost。

**`ReclaimForInsert`（`two_queue.hpp:362`）**：287 条，包含两级回收：先 `EvictAmTail`（真正 `pool_.Destroy`），再 `EvictA1inTail`（降级进 ghost，**不 Destroy 节点**）。顺序不能反：先降级 a1in 会导致 am 满了却没释放。

**语义陷阱**：ghost 命中返回 false 是**故意的**。2Q 只知道「这个 key 以前被淘汰过」，不知道 value；调用方必须自己回源再 `Put`。把 ghost 命中当命中返回，会返回未初始化/过期的 value。

**`Put` 251 < `Get` 373 的原因**：更新分支不查 ghost；插入分支把淘汰集中到 `ReclaimForInsert`，`Put` 主体只做「查一次 + 跳转」。

### 3.3 TinyLFU：306 条指令 = 4 路素描

**结构**（`tiny_lfu.hpp`）：

| 段 | 容量 | 作用 |
|---|---|---|
| `window_` | 1% | 新条目落地，抗突发 |
| `probation_` | 20% | 试用区 |
| `protected_` | 79% | 保护区 |
| `sketch_` | 4 行 × 4 bit | count-min 频率估计，饱和 15 |
| `doorkeeper_` | Bloom | 第一次出现的 key 不污染素描 |

**`Get`（`tiny_lfu.hpp:121`）逐行**：

1. 算 64 位混合哈希（`sketch_hash`）。
2. `RecordFrequency(hash)`（`:356`）：先 `doorkeeper_.TestAndSet`，第一次只置 Bloom 位；第二次才 `sketch_.Increment`。
3. 查 `window_`：命中 → `move_to_front`，返回。
4. 查 `probation_`：命中 → 晋升 `protected_`（`protected_` 满时降级一个回 probation），返回。
5. 查 `protected_`：命中 → `move_to_front`，返回。
6. 都未命中 → `RecordMiss`，返回 false。

**为什么是 306 条**：除哈希与链表外，还要更新 4 路 sketch（4 次索引 + 4 次比较 + 年龄衰减判断）。它是三者中最重的 `Get`，但换来的是**扫描抗性 + 频率准入**：一次性 key 进不了主缓存。

**`Put`（`tiny_lfu.hpp:186`）**：420 条。新条目先 `window_.push_front`；窗口超限时取尾作为 candidate；`main_capacity_==0` 时直接丢弃；否则 `AdmitToMain`（`:424`）比较频率：

```text
candidate 频率 > victim 频率 ? 替换 : 丢弃（rejections++）
```

**`AdmitToMain`（`:424`）**：346 条，包含晋升 + 淘汰。`EvictProbationTail`（`:488`）摘索引、`pool_.Destroy`、`RecordEviction`。

**衰减**：每 `sample_size_` 次记录调 `sketch_.Halve()`（所有 4-bit 计数器右移一位）。这保证频率是「近期」而不是「历史总量」，否则老热点永远赖着不走。

**失败路径**：`Put` 返回 void，准入失败只记 `rejections`；`weight > max_weight` 静默拒绝。调用方要确认「真的缓存了」，必须 `Put` 后 `Peek` 或看 `Stats().rejections`。

### 3.4 三实现对照（结构 → 淘汰 → 并发 → 指令）

| 维度 | LRU | 2Q | TinyLFU |
|---|---|---|---|
| 索引 | 1 张 BucketMap | 1 张（条目带段标记）+ ghost 表 | 1 张（条目带段标记） |
| 链 | 1 条 LRU | am + a1in 两条 + ghost | window + probation + protected 三条 |
| 淘汰依据 | 最近访问时间 | am 尾 + a1in 满降级 | 窗口候选 vs probation 尾的频率 |
| 命中路径哈希次数 | 1 | 2–3 | 1 + sketch 4 路 |
| `Get` 指令 | **73** | **373** | **306** |
| `Put` 指令 | 227 | 251 | 420 |
| 栈帧（Get） | **0 B** | 56 B | 56 B |
| `lock` 前缀 | 0 | 0 | 0 |
| 稳态分配 | 0 | 0 | 0 |

**为什么 TinyLFU 的 `Get` 比 2Q 少**：2Q 要两次哈希定位两个段；TinyLFU 三段共用一张索引，段标记在条目里，只剩 sketch 更新。这是「容器层统一」带来的直接收益（`tiny_lfu.hpp` 文件头「容器层」段）。

### 3.5 容器层的口径澄清（注释 ≠ 代码）

`lru.hpp:9` 的注释写「开放寻址哈希表 `detail::FlatMap`」，但：

- `include/go2cpp/cache/detail/` 目录里**没有** `flat_map.hpp`，只有 `bucket_map.hpp`；
- `lru.hpp:250` 的实际成员是 `detail::BucketMap<K, Entry, Hash, Equal> index_`；
- `BucketMap` 是**链式哈希**（桶数组 + 节点 next 指针），不是开放寻址。

`asm-cache.md` §2.1 把 73 条指令解释成「`FlatMap` 线性探测」也沿用了这条旧注释。**实际反汇编的 73 条**是「64 位 mix 哈希 + 桶链探测 + move_to_front + 统计」，没有开放寻址的探测循环。这不影响性能结论（73 条是真的），但影响读者对数据结构的判断——改代码前以 `bucket_map.hpp` 为准。

同类漂移还有：现有 1.1 表把 `LruCache::Get` 标在 `lru.hpp:64`，实际在 `:90`。文档行号会随重构漂移，**任何「按行号跳转」的说法都要当天复核**。

### 3.6 层 3 小结：三条「改了就坏」的线

1. **一个 `Entry` 只能挂一条链**：`move_to_front` 只改 4 个指针；手动改 `prev/next` 会把链成环或丢节点。
2. **`Create` 与 `Destroy` 必须配对**：`Destroy` 既挂 free list 又调 `V` 析构；漏一个就泄漏或 double-free。
3. **ghost/pending 不是数据**：2Q/ARC 的 ghost 命中只能返回 false + 凭证；把它当命中会返回脏值。

---

## 第四部分 · 层 4：三个异常场景

### 4.1 场景一：并发淘汰（多线程同时 `Put` 触发淘汰）

**默认行为**：**未定义**。裸 `LruCache`/`TwoQueueCache`/`TinyLfuCache` 都没有锁（`lock` 前缀 0，`cache.hpp:23` 写明非线程安全）。两个线程同时 `Put` 触发淘汰时：

- 两个线程可能同时 `pop_front` 同一个尾节点 → 一个 `Destroy` 后另一个继续用 → **UAF**；
- 两个线程同时 `move_to_front` 同一节点 → `prev/next` 交叉写 → **链成环**，后续遍历死循环；
- `size_`/统计是非原子 `uint64_t` → 计数丢更新（不崩，但 `Stats()` 不可信）。

**可选行为**（三选一，不要混用）：

| 方案 | 做法 | 代价 | 适用 |
|---|---|---|---|
| `ThreadSafeCache` | 每个方法一把 `std::mutex` | 读也串行；16 核上是瓶颈 | 低并发、要简单 |
| `ShardedCache` | 按 key 哈希 N 片，每片独立锁 | 全局 LRU/频率丢失；`capacity<shards` 时总容量被放大 | 多核高吞吐 |
| 调用方分片 | 自己按线程/业务分 key，每线程一个 cache | 需要业务保证 key 不跨线程 | 线程本地缓存 |

**为什么不能「给策略加原子」**：`move_to_front` 是 4 个指针的复合操作，不是单字原子；要无锁化必须整条链用 CAS 重写（ Harris 链），复杂度远超收益。项目选择「策略核心无锁 + 装饰层加锁」，是明确的边界。

**调试**：

```bash
# 1) 先用 ThreadSanitizer 复现数据竞争
#    cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" ...
# 2) 链成环的典型现象：Size() 正常但 Get 某 key 死循环 / 遍历超时
# 3) 看 Stats：evictions 远大于 inserts 说明多线程在互相踩
```

**一个真实边界**：`ShardedCache::Stats()`（`factory.hpp:200` 起）逐片加锁再相加，**不是原子快照**——并发 `Put` 期间读到的 `size` 可能比真实值小。不要用它做精确容量判断。

### 4.2 场景二：容量为 0

**默认行为**：**构造抛异常**。各策略在构造函数里 `if (capacity == 0) throw std::invalid_argument(...)`（LRU 在 `lru.hpp:50` 附近，2Q/TinyLFU/GDSF/ARC/CLOCK 一致）。这是「快速失败」：容量 0 的缓存没有语义，允许它存在只会让 `Put` 静默丢弃。

**三个例外/放大点**：

1. `MakeCache`/`MakeThreadSafeCache` 不会替调用方兜底 0，异常直接穿透。
2. `ShardedCache` 把空片容量提到 1（`factory.hpp:128`）：`capacity < shards` 时总容量被**放大**到 `shards`，`Capacity()` 返回的是实际和（`:143`）。业务以为配了 4，实际 16。
3. TinyLFU 的 `main_capacity_` 可能算成 0（容量很小时 1% 窗口 + 20% probation 取整后为 0）；此时窗口条目直接丢弃（`tiny_lfu.hpp:157` 附近），不抛异常——**构造成功但什么都不缓存**。

**可选行为**：

- 业务侧把 `capacity` 夹到 `[shards, ...]`，或至少 `max(1, capacity)`；
- 需要「可关闭的缓存」时，用 `capacity=1` + 不 `Put`，而不是 0；
- 分片场景先算 `capacity/shards` 是否 ≥ 1，否则调小 `shards`。

**调试**：

```bash
# 构造异常：catch std::invalid_argument，打印 policy 与 capacity
# Sharded：打印 Capacity() 与配置值对比，差值 = 被提为 1 的空片数
# TinyLFU：看 Stats().size 是否长期为 0 且 rejections 增长
```

### 4.3 场景三：回调/仿函数抛异常（`Hash`、`Equal`、`V` 析构）

**先澄清**：缓存接口没有回调，最接近的三处是「`Hash` 仿函数」「`Equal` 仿函数」「`V` 的析构」。三者抛异常的后果完全不同：

| 抛出点 | 后果 | 标准行为 |
|---|---|---|
| `Hash`/`Equal` 抛 | 异常穿透 `Get/Put`；容器可能停在半更新状态（索引已插、链未挂） | 未定义强保证；调用方只能丢弃整个 cache |
| `V` 拷贝构造抛（`Put` 传值） | 条目未插入；已分配的 `Entry` 可能泄漏 | 基本保证（无泄漏需 `Destroy`） |
| `V` 析构抛（`Destroy`） | **`std::terminate`** | C++ 析构默认 `noexcept`；抛即终止 |

**为什么析构抛异常是致命的**：`NodePool::Destroy` 在淘汰路径上调用 `~V()`；淘汰可能发生在 `Put` 内、也可能在 `Clear` 内。异常从析构里逃出 → `std::terminate`，进程直接死，没有栈展开。**所以缓存的 `V` 必须是「析构不抛」的类型**（`std::string`/`shared_ptr`/POD 都满足）。

**可选行为**：

1. `Hash` 用 `noexcept` 的哈希（`std::hash` 对标准类型是 noexcept；自定义类型要显式标）。
2. 需要「可能失败」的 value 初始化，用 `std::optional<V>` 或 `shared_ptr<V>`，让失败发生在构造前而不是析构时。
3. 不要在缓存里存「析构要写日志/刷盘」的对象；那种对象应该由外部所有权管理。

**调试**：

```bash
# 1) 析构抛异常：进程收到 SIGABRT，栈里有 std::terminate
#    gdb -ex 'bt' core  → 看 ~V 与 Destroy 的调用链
# 2) Hash 抛：catch 到异常后 cache 不可再用，重建即可；不要试图修链
# 3) 用 -fno-exceptions 构建时，任何 throw 都是编译错误（本项目部分目标这样编）
```

### 4.4 层 4 小结：异常语义的三句话

1. **构造期快速失败**（容量 0 抛），**运行期静默**（准入失败只记 `rejections`）——这是有意的分工。
2. **并发不是异常，是未定义**：不加锁就是数据竞争，没有「偶尔出错」这回事。
3. **析构必须 noexcept**：缓存的 value 类型选择比策略选择更容易埋雷。

---

## 第五部分 · 层 5：性能（指令级证据与复现）

### 5.1 数据口径

| 项 | 值 |
|---|---|
| 对象 | `include/go2cpp/cache/{lru,two_queue,tiny_lfu}.hpp`（header-only） |
| 实例化 | `K=int, V=int`，来自 `tests/cache/*.o` |
| 编译 | `-O3 -DNDEBUG`（CMake 里 `-O2` 后出现，后者生效）GCC 13.3.0 |
| 反汇编 | `objdump -d -C --no-show-raw-insn`，剔除 `nop`/对齐填充 |
| 出处 | `docs/reports/asm-cache.md` §1 |

### 5.2 指令数（9 个函数）

| 实现 | 函数 | 指令 | call | `lock` | 栈帧 |
|---|---|---:|---:|---:|---:|
| LRU | `Get` | **73** | 0 | 0 | **0 B** |
| | `Put` | 227 | 4 | 0 | 88 B |
| | `Erase` | 95 | 2 | 0 | 24 B |
| 2Q | `Get` | **373** | 6 | 0 | 56 B |
| | `Put` | 251 | 7 | 0 | 72 B |
| | `ReclaimForInsert` | 287 | 6 | 0 | 72 B |
| TinyLFU | `Get` | **306** | 2 | 0 | 56 B |
| | `Put` | 420 | 8 | 0 | 88 B |
| | `AdmitToMain` | 346 | 5 | 0 | 104 B |

**怎么读这张表**：

- **0 个 `lock`**：三者都不是线程安全的，锁在装饰层；这不是「优化掉了锁」，是「没打算在这里加」。
- **0 次稳态分配**：`operator new` 只在 NodePool 扩块（每 64 节点）；淘汰 `Destroy` 只挂 free list。
- **栈帧**：LRU `Get` 0 B 是「全寄存器」的直接证据；其他 24–104 B 是保存被调寄存器 + 金丝雀，没有大栈对象。
- **Evict 无独立符号**：`EvictTail`/`EvictAmTail`/`EvictProbationTail` 都被内联进唯一调用点；表里的 227/287/346 是含淘汰循环的完整入口。

### 5.3 与「朴素实现」的差距（定性，不抄无出处的倍数）

`std::list + unordered_map` 的 LRU：

- 命中 `unordered_map::find` 一次（桶 + 链，可能一次指针追逐）+ `list::splice`（6 个指针）；
- 插入要 `new` 一个 list 节点 + `unordered_map` 节点（**2 次/插入**）；
- 淘汰要 `delete` 节点（**1 次/淘汰**）。

本实现：

- 命中 1 次哈希 + 桶链探测 + 4 个指针；
- 插入/淘汰稳态 0 分配（NodePool 复用）；
- 链表节点就是 `Entry` 本身，没有额外节点。

这就是 `lru.hpp:12` 说的「插入 0 分配」；`asm-cache.md` §2.1 用反汇编确认了 `operator new` 的分摊性，但没有给出「比 std::list 快 N 倍」的端到端数字——**本文也不编**。要报倍数必须跑 `cache_bench --full` 并附机器/容量/负载，项目里目前没有这份对照数据。

### 5.4 怎么复现

```bash
# 1) 单元 + 场景 + 压力
cd .build-bento-wsl
./go2cpp_cache_test

# 2) 基准（quick 是 ctest 默认）
./go2cpp_cache_bench --quick
./go2cpp_cache_bench --full --ops=5000000 --capacity=100000

# 3) 指令级取证（需要 .o 与 asmstat.py）
python3 /tmp/asmstat.py \
  .build-bento-wsl/CMakeFiles/go2cpp_cache_test.dir/tests/cache/test_lru.cpp.o \
  "LruCache<int, int"
# 2Q / TinyLFU 同理换 .o 与符号前缀
```

记录：CPU 型号、是否绑核、`--ops`、`capacity`、策略、`K/V` 类型。`int/int` 的指令数与 `string/string` 不可直接比（哈希与拷贝更重）。

### 5.5 性能边界：为什么 LRU 的 73 条不是「还能再砍一半」

- 73 条里约 12 条是哈希、约 20 条是桶链探测、约 16 条是指针写、其余是分支/统计/返回；
- 再砍只能砍统计（会改变 `Stats` 语义）或砍 `value` 拷贝（`V` 由调用方决定，砍不掉）；
- 真正的瓶颈在 `V` 的拷贝与哈希质量，不在指令数。

**结论**：缓存模块的性能工作已经做完「架构 + 容器」两层；剩下的是「按负载选策略」而不是「继续抠指令」。选错策略（例如用 LRU 扛扫描）损失的是命中率数量级，指令优化救不回来。

### 5.6 层 5 小结

1. **73 / 373 / 306** 是三种策略命中路径的真实成本，差异来自「查几次索引」，不是实现好坏。
2. **0 锁 / 0 稳态分配 / 0 B 栈帧（LRU Get）** 是容器层设计的直接结果。
3. 报性能必须带类型与负载；`int/int` 的指令数不能外推到 `string/string`。
4. 选策略 > 抠指令：扫描负载下 LRU 的命中率会崩，这不是 73 条能救的。

---

## 收尾：自检清单

1. 能不能说清 LRU `Get` 的 73 条里，「哈希 / 探测 / 指针写 / 统计」各占多少？
2. 2Q 的 373 条为什么是 LRU 的 5 倍？ghost 命中为什么必须返回 false？
3. TinyLFU 的 306 条里，4 路 sketch 与三段链各占多少？`Halve()` 为什么必须存在？
4. `BucketMap` 是链式还是开放寻址？注释与代码哪个为准？
5. 容量 0 时哪个策略「不抛但什么都不缓存」？`ShardedCache` 为什么会放大总容量？
6. 为什么缓存的 `V` 必须「析构不抛」？抛了会发生什么？
7. 三种装饰方案各适合什么并发度？`ShardedCache::Stats()` 为什么不是原子快照？
