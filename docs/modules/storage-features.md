# 功能层：Queue / Subscribe / MemCache（共用一套 LSM）

在 KV 引擎（`include/go2cpp/storage/kv.hpp`，aecc513）之上补三个功能层：
顺序消息队列（Queue）、键空间变更订阅（Watch）、内存缓存（MemCache）。

三者**共用同一套 LSM**——不是三个独立存储：

| 功能 | 复用的 LSM 部件 | 落点 |
|---|---|---|
| Queue | WAL 顺序写 + SSTable 段索引 | 消息日志走 WAL 块格式；`topic/consumer` 游标是 KV 里的一条普通记录 |
| Watch | WAL 变更日志 + KV 写路径 | 事件写变更日志（同 WAL 格式）；订阅者游标是 KV 记录 |
| MemCache | MemTable 的槽/载荷分离 + 时间轮 | 可选写穿：`Set` 直接 `KV::Put`，`Get` miss 从 LSM 回填 |

代码量：`src/storage/{seglog,queue,watch,memcache}.cpp` 约 2.7k 行
（不含测试），不改 `kv.hpp` / `kv.cpp` / `wal.*` / `memtable.*` / `sstable.*`。

---

## 1. 目标与边界

| 项 | 取值 |
|---|---|
| Queue 语义 | at-least-once；`Consume` 只读、`Ack` 推进游标；崩溃后不丢已确认消息 |
| Queue 顺序 | 同一 topic 内严格按追加顺序投递；多 topic 共用一条日志、互不干扰 |
| Watch 语义 | 写成功后回调；同步模式顺序 = 写入顺序；异步模式 FIFO 单线程派发 |
| Watch 游标 | 每个订阅在 KV 里存 `watch/<id>/offset`，支持重启后 `Replay` 补课 |
| MemCache | TTL（毫秒精度）+ LRU 淘汰 + 分片锁；`persist` 写穿、`read_through` 回填 |
| 不支持 | Queue 的广播/分区再均衡、Watch 的通配符/正则、MemCache 的持久化恢复（重启后缓存为空） |

---

## 2. 架构

```
                    ┌──────────────────── 共用一套 LSM ────────────────────┐
                    │                                                      │
  Queue::Append ────┼──> SegLog（WAL 块格式 + 1MB 用户态缓冲）              │
  Queue::Ack ───────┼──> KV::Write（游标批量，一次 WriteBatch）             │
                    │                                                      │
  Watcher::Put ─────┼──> KV::Put ──> 事件写 SegLog ──> 回调（同步/异步）    │
  Watcher::Replay ──┼──> SegLog::Cursor（段索引定位）                       │
                    │                                                      │
  MemCache::Set ────┼──> 内存哈希 + LRU 链 + TTL 时间轮                     │
             └──────┼──> （可选）KV::Put 写穿                               │
                    └──────────────────────────────────────────────────────┘
```

### 2.1 SegLog：Queue 与 Watch 共用的「WAL + 段索引」层

`SegLog` 把 LSM 的两件事拆出来复用：

* 写端**直接复用 `WalWriter`**：64KB 块、7 字节记录头（CRC32C + 长度 + 类型）、
  1MB 用户态缓冲、块尾填充规则，与 KV 的 WAL 完全一致；
* 段封口时把**稀疏检查点写成 SSTable**（`NNNNNN.idx`，复用块/footer 格式）：
  key = `seq` 的大端 8 字节，value = `offset(u64 BE) + length(u32 BE)`。
  BE 编码让 SSTable 的字典序比较等价于数值序，冷启动可以从最近检查点开始重放；
* 读端自带一个按偏移定位的扫描器（同一块格式，约 120 行）。`WalReader`
  只能从文件头顺序扫，而 MQ 的消费者要能从任意 `seq` 开始，所以不能直接复用。

段滚动：写满 `segment_size`（默认 64MB）即封段、写索引、开新段。
恢复时按段号顺序扫描，**最后一段若尾部有不完整记录（kill -9 的典型残留）
会截断到最后一个完整记录的末尾**——否则新追加的记录会被尾部垃圾挡住读不出来。

---

## 3. Queue（MQ）

接口：`include/go2cpp/storage/queue.hpp`。

```cpp
auto kv = KV::Open("/data/kv");
QueueOptions opt;                       // segment_size / write_buffer / retain / ...
auto q = Queue::Open(kv.get(), "/data/kv/queue", opt);

std::uint64_t offset = 0;
q->Append("orders", "order-42");        // offset = 0
q->Append("orders", "order-43");        // offset = 1

QueueMessage msg;
while (q->Consume("orders", "worker-1", &msg)) {
  process(msg.value);
  q->Ack("orders", "worker-1", msg.offset);   // 游标推进到 msg.offset + 1
}
```

### 3.1 记录格式

```
[topic_len:varint][topic][offset:varint][payload]
```

一条记录一个 WAL 记录（超 64KB 自动分块）。topic 每次都写进记录里，
换来的是恢复时不需要额外的 topic 注册表：扫一遍日志就能重建每个 topic
的 `next_offset`。代价是每条多 ~10 字节，1e6 条约 10MB——比维护注册表
和它的崩溃一致性便宜。

### 3.2 消费模型

* **at-least-once**：`Consume` 不推进游标，`Ack` 才推进。未 `Ack` 前
  重复调用 `Consume` 会反复返回同一条（这是语义，不是 bug）。
* **多消费者独立游标**：`(topic, consumer)` 一个游标，存 KV：
  `queue/<topic>/<consumer>/offset`（8 字节小端）。
* **批量落盘**：每 `cursor_flush_interval`（默认 4096）次 `Ack` 用一次
  `WriteBatch` 把全部脏游标写进 KV。崩溃最多回退一个窗口 → 重复投递若干条，
  不会丢。`Sync()` 强制落盘。
* **内存队列**：追加成功即进内存 `deque`，消费是纯内存命中（~28ns）。
  所有消费者都确认过的条目会被回收（`ReclaimLocked`）。
  `retain` 可以给内存副本设上限，超出的转走日志冷读。

### 3.3 冷读

游标落后于内存窗口（重启、`Seek` 回退、`retain` 丢弃）时走日志：
`SegLog::Cursor` 从「最近的已知位置」开始顺序扫，一次预取 4096 条进内存队列。
预取批次**不做 retain 裁剪**——否则消费者永远追不上，每条消费都要从头扫，
退化成 O(n²)。

### 3.4 并发

`Append` 持 topic 锁（保证 offset 与日志顺序一致），日志写端另有 `log_mu_`。
`Consume` 只持 topic 锁。不同 topic 不共享锁；同一 topic 内串行（与 LSM 写锁同粒度）。

---

## 4. Subscribe（Watch）

接口：`include/go2cpp/storage/watch.hpp`。

```cpp
auto watcher = Watcher::Open(kv.get(), "/data/kv/watch");

std::uint64_t id = watcher->Watch("user/", [](const WatchEvent& e) {
  // 同步模式：此刻 KV 里已经能读到新值
  handle(e.key, e.value, e.deleted, e.seq);
});

watcher->Put("user/1", "alice");    // 写 KV + 触发回调
watcher->Delete("user/1");          // deleted = true
watcher->Unwatch(id);
```

### 4.1 写路径

`Put` / `Delete` / `Write(WriteBatch)` 是 `KV::Put` / `KV::Delete` / `KV::Write`
的包装：**先写 KV，成功后再发事件**。这样回调里不会看到「事件已发但值不存在」。

事件追加到变更日志（`SegLog`），`seq` 就是日志的全局序号。

**没有订阅者时，写路径只多一次原子读**（`sub_count_` 为 0 直接返回），
不写事件日志——这是 Subscribe 相对 Queue 的关键差别：通知是「有人听才记录」。

### 4.2 派发模式

| 模式 | 实现 | 顺序 | 适用 |
|---|---|---|---|
| 同步（默认） | 写线程内回调 | 严格 = 写入顺序，回调返回时数据已可见 | 强一致通知、审计 |
| 异步 | 单后台线程 + 有界队列 | FIFO，队列满时**阻塞**（背压）不丢 | 慢消费者、批量处理 |

异步模式刻意选择背压而不是丢弃：事件丢了就不是「通知」了。
`WatchStats.async_backpressure` 记录写线程被阻塞的次数。

### 4.3 订阅游标与 Replay

每个订阅在 KV 里存 `watch/<id>/offset`（已派发到的 `seq`）。
`Watch(prefix, cb, stable_id)` 传入稳定 id 时会从该游标续上；
`Replay(id, from_seq)` 从变更日志重放 `seq >= from_seq` 且匹配前缀的事件。

`Replay` 前会先 `Flush` 写缓冲——否则刚产生、还在用户态缓冲里的事件读不到。

回调抛异常会被吞掉（记入 `callback_errors`），不会让写路径失败。

---

## 5. MemCache（内存缓存）

接口：`include/go2cpp/storage/memcache.hpp`。

```cpp
MemCacheOptions opt;
opt.capacity = 1u << 20;   // 条目上限
opt.shards = 8;            // 分片锁
opt.persist = true;        // 写穿 LSM
opt.read_through = true;   // miss 回填

auto cache = MemCache::Open(kv.get(), opt);
cache->Set("k", "v", std::chrono::seconds(30));   // ttl = 0 表示不过期
std::string v;
if (!cache->Get("k", &v)) { /* miss */ }
```

### 5.1 条目布局

```
偏移 0   : hash(4) klen(2) vlen(2) flags(1) pad(3)
偏移 16  : 内联 key+value（≤32B），超出则存 heap 指针
偏移 48  : TTL 轮 prev/next、LRU prev/next、桶链 hnext/hprev
偏移 96  : reference_time（到期毫秒）
```

按「查找路径的访问顺序」排：**hash 与内联 key 位于同一条 64B cache line**，
一次 miss 就能完成 hash + key 比较；指针字段（TTL 轮 / LRU 链 / 桶链）
放在后面，命中后才会写。

### 5.2 TTL：复用 `cache/detail/time_wheel.hpp`

256 桶时间轮 + 桶内侵入式链表 + 桶 min 的 256 叶锦标赛树：
插入/删除 O(1)、取最早到期 O(1)、回绕由绝对时间戳比较保护。
`MemCache` 的 `Entry` 满足时间轮的契约（`prev` / `next` / `reference_time`）。

过期判定用的是**绝对毫秒时间戳**（`steady_clock`），时间轮只负责
「找出最早到期的那一条」。命中路径上额外做一次单条判定，保证不会返回
已过期的值；批量清理在 `Set` 与「每 64 次 Get」时做（`purge_on_get`
可以切回旧行为）。

### 5.3 LRU：复用 `cache/detail/intrusive.hpp`

* 侵入式双向链表：命中 = 4 次指针改写，零分配；
* `NodePool<Entry>`：按 64 个一块分配，稳态（插一条淘汰一条）下
  malloc 次数为 0。

### 5.4 分片

按哈希高 16 位分片（2 的幂），每片独立的锁 / 桶数组 / LRU 链 / TTL 轮 / 节点池。

单线程场景下分片是负优化：多一层间接 + 桶数组更分散（实测 1 分片比
8 分片快 10~20%）。所以**分片数应按并发线程数配置**，单线程用 1。

---

## 6. 源码级优化与证据

（objdump 数据见 `docs/reports/asm-storage-features.md` 与 `/tmp/lsm_features.md`。）

### 6.1 Queue：thread_local topic 缓存

**优化前**：每次 `Append`/`Consume` 都 `std::string(topic)` 构造 + 全局
`topics_mu_` 锁 + `unordered_map` 查找。1e7 次追加 = 1e7 次小分配 + 1e7 次锁。

**优化后**：`thread_local TopicState* cached`，先按内容比较
（`memcmp`），命中直接返回，慢路径才进锁。`TopicState` 只增不删、指针稳定；
用内容比较而不是指针比较，避免调用方复用同一块 buffer 时误命中。

实测（n=2e6，taskset 0-3）：**188.1ns → 114.4ns（-39.2%）**，
`QueueOptions.topic_cache = false` 可以复现旧路径。

### 6.2 MemCache：条目重排（无显著收益，如实记录）

`hash`/`klen`/`vlen`/`flags` 与内联 key 放进同一条 cache line，
理论上少一次 cache miss。1e7 实测：`get_nocopy` 547.1ns（重排后）vs 之前
495ns 量级 —— **落在 ±25% 噪声内，没有统计显著收益**。
1e6 工作集（~104MB）的瓶颈是 DRAM 延迟本身，而不是少一次 miss。
保留新布局只因为它更符合访问顺序，不把它算作性能收益。

### 6.3 MemCache：Get 侧过期扫描节流（无显著收益，保留为默认）

旧行为每次 `Get` 都扫一次时间轮根（`wheel.Min()`）。改成每 64 次扫一次 +
命中时单条判过期（保证语义精确）。1e7 实测：purge=1 是 859.8ns，
purge=0 是 906.9ns —— 噪声内，没有收益（TTL 轮为空时 `Min()` 只是一次 load）。
保留为默认只因为它语义上更省：过期判定仍然精确（命中时逐条检查）。

### 6.4 一个明确的负优化记录

MemCache 默认 8 分片在单线程下比 1 分片慢（1e7：980.9ns vs 859.8ns，+14%）。
原因是多一层 `vector<unique_ptr<Shard>>` 解引用 + 桶数组分片后局部性变差。
文档里明确写「分片数按并发度配置」，而不是无脑开 8。

### 6.5 已识别但未做的优化（诚实标注）

`objdump` 显示 `MemCache::Impl::Get` 有 **7 个 `lock` 前缀**，全部来自
`GetStats` 用的 `std::atomic` 计数器。改成「分片内普通计数 + GetStats 汇总」
预计省 5~7 次 `lock add`（无竞争每次 7~10ns，合计 ~8%）。没做的原因：
本次 1e7 测量噪声 ±25%，收益落在噪声内，给不出可信的前后对比。

---

## 7. 性能

详见 `/tmp/lsm_features.md`（含 1e7 数据、下限与「离下限几倍」）。
本节只给结论：

1e7、`taskset -c 0-3`、WSL ext4，单次运行（完整日志 `/tmp/bench_1e7.log`）：

| 操作 | ns/op | ops/s |
|---|---:|---:|
| Queue append（40B 消息） | 196.1 | 509 万/s |
| Queue consume（内存命中） | 24.2 | 4125 万/s |
| Queue consume + ack | 69.7 | 1434 万/s |
| Queue 冷读（日志重放） | 2 169.6 | 46 万/s |
| Watch 写 + 同步回调 | 785.4 | 127 万/s（回调本身 53ns） |
| Watch 写（无订阅者） | 533.8 | 187 万/s（KV::Put 基线 460.8） |
| MemCache Get（含 LRU + 锁 + 拷贝） | 859.8 | 116 万/s |
| MemCache Get（Peek，纯查找） | 570.9 | 175 万/s |
| MemCache Set | 721.5 | 139 万/s |
| 对照：项目 LruCache::Get | 585.3 | 171 万/s |
| 对照：unordered_map::find | 143.9 | 695 万/s |

**结论**：MemCache 的纯查找（570.9ns）与项目现有 LruCache（585.3ns）持平，
多出来的 ~275ns 是分片锁 + TTL 判定 + LRU 链改写 + `std::string` 赋值
（16B 超过 SSO，每次一次 malloc）。Queue 的 append 距裸 WAL 追加 2.7×，
consume 已到 24ns（接近内存拷贝下限）。

---

## 8. 测试

| 测试 | 覆盖 | 规模 |
|---|---|---|
| `tests/storage/queue_test.cpp` | 基本收发/多消费者/多 topic/冷读/游标持久化/段滚动/尾部截断/**kill -9 崩溃恢复**/1e6 全量 | 1e6 |
| `tests/storage/watch_test.cpp` | 前缀匹配/多订阅者/批写/无订阅快路径/Replay/游标持久化/异步/1e5 写回调 | 1e5 |
| `tests/storage/memcache_test.cpp` | 基本读写/TTL/LRU 淘汰/多线程/写穿/回填/1e7 Get+Set | 1e7 |

```bash
ctest --test-dir .build-bento-wsl -R 'go2cpp_(queue|watch|memcache)_test' --output-on-failure
```

崩溃恢复的语义边界：

* 队列日志在 `Sync()` 后 fdatasync 完成，kill -9 不丢；
* 游标是 KV 记录，`Ack` 后批量写、`Sync()` 强制落盘；KV 自身的 WAL
  缓冲需要 `KV::Close()`（或 `sync_writes = true`）才 fdatasync。
  测试里模拟的是「业务正常关闭引擎、随后进程被强杀」。

---

## 9. 限制与后续

* Queue 没有分区/再均衡，单 topic 的消费是串行的（一把 topic 锁）；
* 段淘汰只做了「读时跳过」，没有按时间/大小做保留策略；
* Watch 的前缀匹配是线性比较，订阅者很多时应该建前缀树；
* MemCache 重启后是空的（`persist` 只保证写穿，不做启动预热）；
* `Replay` 会重放整个日志区间，没有做「按需分段」。
