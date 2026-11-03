# 05 cache 答卷（5 题）

> 环境：WSL2 / 12 vCPU；`go2cpp_cache_test`（Release）本机跑通 `all checks passed`。
> 下文的命中率/吞吐都来自本轮实跑，不是 docs 数字。

## Q01

**答**：GDSF 用惰性删除：堆节点是 `{key_value, version, key}`，索引 `index_` 才是权威，堆只是历史优先级候选集；同一 key 每次命中都 push 一个新节点，旧节点靠 `version` 判废。命中必须同时做三件事：`freq++`、重算 `key_value = cost/size*freq`、`version = ++version_`；少递增 version，下一次淘汰就会用旧节点的较小 key_value 把刚命中的热条目淘汰（项目真实发生过「Get 后反而把自己淘汰」）。`MaybeCompact` 在堆项超过 `index_.size()*4+64` 时 O(n) 重建，防长期膨胀。`Put` 更新已有 key 时先摘旧条目再 `MakeRoomForInsert`，否则「插入自己」会把自己当受害者。`weight==0` 按 1 处理（避免除零），`weight > max_weight` 直接拒绝并记 `rejections`，`Put` 返回 void（调用方拿不到失败信号）。

**代码证据**：
```text
include/go2cpp/cache/gdsf.hpp:78-81
  78  entry.key_value = KeyValue(entry.size, entry.freq);
  79  // 必须递增 version：堆里可能还有同一 key 的旧节点，旧 key_value
  80  // 更小，若不失效会在下一次淘汰时被误判为「当前值最小」而淘汰
  81  // 这个其实很热的条目（曾导致 Get 后反而把自己淘汰掉）。
include/go2cpp/cache/gdsf.hpp:264-265  if (it == index_.end() || it->second.version != node.version) { continue;  // 惰性删除
include/go2cpp/cache/gdsf.hpp:276-277  void MaybeCompact() { if (heap_.size() <= index_.size() * 4U + 64U) return;
```

**自证**：`.build-bento-wsl/go2cpp_cache_test` → `weighted GDSF hit=0.8061 avg_hit_w=198.2 weight=39091/40000`（LRU 同负载 `hit=0.5610`），说明权重参与淘汰且守住 `max_weight`；`stress gdsf weight budget: size=4496 weight=99903`。

**我没答出的**：没有临时删掉 `entry.version = ++version_` 复现命中率下降（题面说的半自证）。

## Q02

**答**：2Q 的三个队列：A1in（FIFO 准入）、A1out（ghost，只存 key 不存 value）、Am（LRU 主缓存）。ghost 命中只证明「这个 key 最近被淘汰过」，数据已不在缓存，所以 `Get` 记 `stats.misses++`/`ghost_hits++` 并返回 **false**；它把 key 写进 `pending_`，等**下一次 Put** 才直接提升到 Am。`pending_` 容量是 `a1out_capacity_`（`capacity/2+1`），满了 `DropOldestPending()`；`Erase`/`Clear` 也清 pending。A1in 命中**不**改变位置（FIFO），这是 2Q 与 LRU 的核心差异：只读一次的条目不该获得二次机会。只 Get 不 Put → 凭证留在 pending 直到被挤掉，该 key 从未真正回到缓存。

**代码证据**：
```text
include/go2cpp/cache/two_queue.hpp:96-99  GhostNode* ghost = LookupGhost(key); if (ghost != nullptr) { // ghost 命中：数据已不在缓存，记下来让下一次 Put 直接进 Am。
include/go2cpp/cache/two_queue.hpp:108-111  pending_index_.Insert(node); stats_.RecordGhostHit(); stats_.RecordMiss(); return false;
include/go2cpp/cache/two_queue.hpp:141-143  if (TakePending(key)) { ReclaimForInsert(); Insert(entry, kSegmentAm, ...);
```

**自证**：`go2cpp_cache_test` 的 `hotspot 2Q hit=0.6521`（低于 ARC/TinyLFU，符合 2Q 的定位）；测试里 `2Q a1in -> ghost -> am promotion` 段存在（未单独 grep 输出）。

**我没答出的**：没有单独打印 ghost→pending→Put 的断言序列。

## Q03

**答**：ARC 的 `p_` 是 T1 的目标大小（条目数，范围 `[0, capacity]`）。B1（只访问过一次的 ghost）命中说明 recency 不够 → 扩大 T1：`delta = max(1, |B2|/|B1|)`、`p_ = min(capacity, p_+delta)`；B2（至少访问两次）命中说明 frequency 不够 → 缩小 T1：`delta = max(1, |B1|/|B2|)`、`p_ = max(0, p_-delta)`。方向反了会让「只访问一次」的流量长期占住主缓存。ghost 命中同样是 miss，只把 key 记进 pending，等 Put 时直接进 T2；`EvictForInsert(from_b2)` 决定从 T1 还是 T2 淘汰：`|T1| > p` 或（`|T1| == p` 且来自 B2）时从 T1。ghost 表容量是 `capacity`，不是无限历史。

**代码证据**：
```text
include/go2cpp/cache/arc.hpp:100-107  GhostNode* ghost = LookupGhost(key); if (ghost) { const bool from_b2 = ghost->in_b2; if (from_b2) { delta = max(1, |B1|/|B2|); p_ = max(0, p_ - delta);
include/go2cpp/cache/arc.hpp:109-112  } else { delta = max(1, |B2|/|B1|); p_ = min(capacity, p_ + delta); }
tests/cache/test_arc.cpp:53  go2cpp_cache_test::Announce("ARC adaptive p (b1 up / b2 down)");
```

**自证**：`go2cpp_cache_test` → `scan ARC baseline=1.000 recovery=0.990`、`hotspot ARC hit=0.7764`；`stress arc ghost bound: ARC size=512 ghost=512`（ghost 有界）。

**我没答出的**：没有单独断言 p 的数值变化方向（只跑总用例）。

## Q04

**答**：容量按分片切分：`base = capacity/shard_count`、`extra = capacity%shard_count`（前 extra 片多 1 条）；每片容量为 0 时强制提到 1，所以 `capacity < shards` 时实际总容量 = `shards`，`Capacity()` 返回 `capacity_`（各片实际容量累加），不是构造参数。`max_weight` 同样按片切分并「至少 1」兜底，GDSF 的 `max_weight` 在分片下不再是全局预算。分片后跨片全局 LRU/ARC 顺序不成立（各片独立淘汰），TinyLFU 的 Count-Min Sketch 与 doorkeeper 也是每片独立、跨片频率不共享。`Get/Put/...` 逐片加锁，`Size()` 不是原子快照。

**代码证据**：
```text
include/go2cpp/cache/factory.hpp:120  ShardedCache(Policy policy, CacheOptions options, std::size_t shards) : ... shard_count_(std::max(1, shards)) {
include/go2cpp/cache/factory.hpp:122-123  base = options.capacity / shard_count_; extra = options.capacity % shard_count_;
include/go2cpp/cache/factory.hpp:129-130  shard_options.capacity = 1;
include/go2cpp/cache/factory.hpp:190  std::size_t Capacity() const override { return capacity_; }
```

**自证**：读 `factory.hpp:118-145` 的切分与兜底；`go2cpp_cache_test` 的 `stress thread-safe 8 threads x 50k` 通过（`TinyLFU mt 267.5 ms`）。容量放大没有现成断言。

**我没答出的**：没有写 3 行 `ShardedCache(LRU, {.capacity=4}, 16)` 打印 `Capacity()` 的复现。

## Q05

**答**：CLOCK 只有一个 `referenced` 位，没有频率、没有链表重排。顺序扫描把所有槽位都置 1；扫描后第一轮淘汰只清位、第二轮才真正替换，期间所有新条目都能活过一轮，热数据没有保护。命中率恢复依赖「重新访问热数据把 bit 置回」，但 CLOCK 没有频率历史，无法识别「这个 key 以前很热」；所以扫描后 recovery 会明显低于 LRU-K/TinyLFU。淘汰最多扫 `2×capacity` 槽位（一圈清位、一圈替换）；测试里 CLOCK 的 recovery 为 0.000。适用「条目小、QPS 极高、能接受命中率略低」，不适合周期全扫描后要求立刻恢复的负载。

**代码证据**：
```text
include/go2cpp/cache/clock.hpp:213-222  std::size_t FindVictim() { for (;;) { Slot& slot = slots_[hand_]; ... slot.referenced = false; AdvanceHand(); }
include/go2cpp/cache/clock.hpp:74  bool Get(...) { ... slot.referenced = true（命中只置位）
tests/cache/test_scenarios.cpp:114  Announce("scenario scan pollution / recovery");
```

**自证**：`go2cpp_cache_test` 本轮输出：
```text
scan LRU        baseline=1.000 recovery=0.000
scan LRU-K(2)   baseline=1.000 recovery=1.000
scan W-TinyLFU  baseline=0.990 recovery=0.990
scan ARC        baseline=1.000 recovery=0.990
scan CLOCK      baseline=1.000 recovery=0.000
stress lru 1M ops: 51.37 Mops/s, hit=0.1987, size=10000
```

**我没答出的**：没有跑 `go2cpp_cache_bench --quick` 对比 CLOCK 与 LRU 的吞吐/内存（只跑了 test 内的 stress）。
