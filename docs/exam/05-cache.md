# Go2Cpp 理解力考试 · 缓存卷（05 cache）

> **考的是「为什么 / 坏在哪 / 边界在哪」**，不是背 API 名。
> **本卷数字均需自测，文档数字仅作历史对照。** 本项目迭代极快，`docs/` 里的命中率/吞吐数字随时可能过时；
> 需要数字时自己跑 `go2cpp_cache_test` / `go2cpp_cache_bench`，并写明 capacity、ops、负载类型。
> 追查位置只指向 `include/go2cpp/cache/**`、`tests/cache/**`；不要拿 `docs/research/cache-algorithms.md` 的结论当答案。
>
> 覆盖：7 种策略的失效条件（本卷挑 GDSF / 2Q / ARC / CLOCK / 分片缓存五个最容易答错的设计）、
> `detail/TimestampWheel`、NodePool 复用与并发安全。
>
> 难度分布：L3 = 1、L4 = 4。

---

### Q01 【L4 陷阱】GDSF 的堆里为什么允许同一个 key 有多条记录？Get 命中后不 push 新版本会坏在哪？

- **痛点**：把 `std::priority_queue` 当成「每个 key 一条」的索引。GDSF 用的是惰性删除：每次命中都 push 一个新节点，旧节点留在堆里靠 `version` 判废；不递增 `version`，下一次淘汰就会用旧节点的较小 `key_value` 把刚命中的热条目淘汰掉。
- **追查位置**：
  - `include/go2cpp/cache/gdsf.hpp:68`（`GdsfCache::Get`：命中后 `freq++`、重算 `key_value`、`version = ++version_`、`PushHeap`）
  - `include/go2cpp/cache/gdsf.hpp:79`（注释：必须递增 version，否则「曾导致 Get 后反而把自己淘汰掉」）
  - `include/go2cpp/cache/gdsf.hpp:264`（`EvictOne`：`it->second.version != node.version` 的惰性删除校验）
  - `include/go2cpp/cache/gdsf.hpp:276`（`MaybeCompact`：堆项超过 `index_.size()*4+64` 时 O(n) 重建）
  - `include/go2cpp/cache/gdsf.hpp:250`（`MakeRoomForInsert` 循环淘汰到能放下）
- **验收标准**（答到这些算过）：
  1. 堆节点是 `{key_value, version, key}`，不是 `{key, value}`；索引 `index_` 才是唯一权威，堆只是「历史优先级」的候选集。
  2. 命中必须同时做三件事：`freq++`、`key_value = cost/size*freq` 重算、`version = ++version_`；少任何一步，`EvictOne` 的 version 校验就失效或用到旧代价。
  3. 惰性删除的代价是堆会膨胀；`MaybeCompact` 是防止长期运行堆无限增长的兜底，重建后每个 key 只保留当前版本。
  4. `Put` 更新已有 key 时先把旧条目摘出来、`MakeRoomForInsert` 再插回（`gdsf.hpp:103` 起），否则「插入自己」的容量检查会把旧条目当成要淘汰的受害者。
  5. `weight==0` 被当成 1（避免 `cost/size` 除零）；`weight > max_weight` 直接拒绝并记 `rejections`，`Put` 返回 void，调用方拿不到「没缓存成功」的信号。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_cache_test` → 观察 `scenario weighted 1:1000` 段：GDSF 的 `avg_hit_w`/`hit` 与 LRU 对比，说明权重确实参与了淘汰；`stress gdsf weight budget` 打印 `size`/`weight` 是否守住 `max_weight`。要观察 version 失效的坏结果，可临时去掉 `entry.version = ++version_` 重跑，GDSF 命中率会异常下降。
- **常见错误答案**：「堆里每个 key 只会有一条」「Get 只更新 map 里的值，堆不用动」「version 是给迭代器用的」——零分。

### Q02 【L4 陷阱】2Q 的 Get 命中了 ghost，为什么仍然返回 miss？只 Get 不 Put 会怎样？

- **痛点**：把 ghost 命中当成「缓存命中」。ghost（A1out）只存 key、不存 value；命中它只证明「这个 key 最近被淘汰过」，数据已经不在缓存里。本实现把它记进 `pending_`，等**下一次 Put** 才直接提升到 Am。
- **追查位置**：
  - `include/go2cpp/cache/two_queue.hpp:96`（`Get` 的 ghost 分支：摘除 ghost 节点、写 `pending_`、`RecordGhostHit`）
  - `include/go2cpp/cache/two_queue.hpp:110`（`RecordMiss()` 后 `return false`：ghost 命中仍然是 miss）
  - `include/go2cpp/cache/two_queue.hpp:103`（`pending_.size() >= a1out_capacity_` 时 `DropOldestPending()`）
  - `include/go2cpp/cache/two_queue.hpp:141`（`Put` 先 `TakePending(key)`：有凭证就直接进 Am）
  - `include/go2cpp/cache/two_queue.hpp:269`（`DropOldestPending`：pending 满了丢最老，凭证不是无限的）
- **验收标准**：
  1. 2Q 的三个队列职责：A1in（FIFO 准入）、A1out（ghost，只存 key）、Am（LRU 主缓存）；ghost 命中只影响「下一次 Put 的去向」。
  2. Get 命中 ghost 返回 false 且 `stats.misses++`、`stats.ghost_hits++`；调用方必须自己加载数据并 Put，否则这次访问什么都没缓存。
  3. pending 的容量是 `a1out_capacity_`（构造时 `capacity/2 + 1`），满了丢最老；`Erase`/`Clear` 也会清 pending。
  4. A1in 命中**不**改变位置（FIFO），这是 2Q 与 LRU 的核心差异：只读一次的条目不该获得二次机会。
  5. 「Get 命中 ghost 后没有 Put」→ 凭证留在 pending 直到被挤掉，期间该 key 从未真正回到缓存。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_cache_test` → 找 `2Q a1in -> ghost -> am promotion` 段：测试先让 key 进 ghost，`Get` 断言 `!hit` 且 `ghost_hits==1`，再 `Put` 后断言进了 Am。还有 `2Q ghost capacity` 段验证 ghost 满了以后旧 key 不再命中。
- **常见错误答案**：「ghost 命中就是命中，应该返回 value」「Get 会自动把数据加载回来」「pending 没有上限」——零分。

### Q03 【L4 陷阱】ARC 的 `p` 为什么在 B1 命中时增大、在 B2 命中时减小？调反了会怎样？

- **痛点**：把 `p` 当成「随便自适应的一个数」。ARC 的 `p` 是 T1 的目标大小：B1（只访问过一次的 ghost）命中说明「recency 不够」，要扩大 T1；B2（至少访问过两次的 ghost）命中说明「frequency 不够」，要缩小 T1、扩大 T2。调反会让缓存往错误的方向偏。
- **追查位置**：
  - `include/go2cpp/cache/arc.hpp:100`（`Get` 的 ghost 分支：`from_b2` 决定方向）
  - `include/go2cpp/cache/arc.hpp:103`（B2 命中：`delta = max(1, |B1|/|B2|)`，`p_ = max(0, p_ - delta)`）
  - `include/go2cpp/cache/arc.hpp:109`（B1 命中：`delta = max(1, |B2|/|B1|)`，`p_ = min(capacity, p_ + delta)` 在 112 行）
  - `include/go2cpp/cache/arc.hpp:159`（`Put` 的 ghost 分支：B2 → `PushT2`，并且 `EvictForInsert(ghost_in_b2)` 决定从哪条链淘汰）
  - `tests/cache/test_arc.cpp:53`（测试名就叫 `ARC adaptive p (b1 up / b2 down)`）
- **验收标准**：
  1. `p_` 的单位是「条目数」，范围被夹在 `[0, capacity]`；`delta` 至少 1，且用两个 ghost 表的长度比做步长（哪个 ghost 更长，说明对应方向缺得更多）。
  2. B1 命中 → p 增（给 T1 更多空间）；B2 命中 → p 减（给 T2 更多空间）；方向反了会让「只访问一次」的流量长期占住主缓存。
  3. ghost 命中同样是 miss（数据不在 T1/T2），只是把 key 记进 pending，等 `Put` 时直接进 T2。
  4. `EvictForInsert(from_b2)` 决定淘汰 T1 还是 T2：`|T1| > p` 或（`|T1| == p` 且来自 B2）时从 T1 淘汰，否则从 T2。
  5. ghost 表容量是 `capacity`（构造时 `ghost_index_(options.capacity)`），不是无限历史；`Erase` 也会删 ghost 并清 pending。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_cache_test` → `ARC adaptive p (b1 up / b2 down)` 段直接验证两个方向；`scenario scan pollution / recovery` 段打印 `scan ARC baseline=... recovery=...`，观察它在扫描后能不能恢复。
- **常见错误答案**：「p 是命中率，越大越好」「B1/B2 命中都该增大 p」「ghost 命中会直接把数据放回 T2」——零分。

### Q04 【L3 陷阱】`ShardedCache` 的 `capacity` 为什么可能大于你配置的值？分片后哪些语义不再成立？

- **痛点**：以为分片只是「加几把锁」。容量是按分片切分的，每片至少 1 条；当 `capacity < shards` 时，实际总容量被放大到 `shards`，而 `Capacity()` 返回的是实际值。全局 LRU 顺序、TinyLFU 的频率素描也都变成「每片独立」。
- **追查位置**：
  - `include/go2cpp/cache/factory.hpp:120`（`ShardedCache` 构造：按 key 哈希分片）
  - `include/go2cpp/cache/factory.hpp:123`（`base = capacity / shards`、`extra = capacity % shards`）
  - `include/go2cpp/cache/factory.hpp:128`（分片容量为 0 时强制提升到 1）
  - `include/go2cpp/cache/factory.hpp:143`（`capacity_ += cache->Capacity()`：累加的是**每片实际容量**）
  - `include/go2cpp/cache/factory.hpp:190`（`Capacity()` 返回实际总容量，不是构造参数）
  - `include/go2cpp/cache/cache.hpp:21`（线程模型：裸策略不是线程安全的，`ThreadSafeCache` 是粗粒度锁，分片是另一种取舍）
- **验收标准**：
  1. 容量切分公式：前 `extra` 片多 1 条；任何一片为 0 都被提到 1，所以 `capacity < shards` 时实际容量 = `shards`。
  2. `max_weight` 同样按片切分并做「至少 1」兜底，权重上限也会被放大；GDSF 的 `max_weight` 语义在分片下不再是全局预算。
  3. 跨分片的全局 LRU/ARC 顺序不成立：每个分片独立淘汰，热点 key 哈希到不同片时可能互相挤占不到对方的空间。
  4. TinyLFU 的 Count-Min Sketch 与 doorkeeper 也是每片独立的（容量按片传入），跨片频率不共享，命中率会与单实例不同。
  5. `Get/Put/Erase/Clear/Size/Stats` 都要逐片加锁；`Size()` 不是原子快照，并发下各片累加可能来自不同时刻。
- **怎么自证**：`grep -n "shard_options.capacity" include/go2cpp/cache/factory.hpp` 看「至少 1」的兜底，`grep -n "Capacity()" include/go2cpp/cache/factory.hpp` 看返回的是累加值；再用 `./go2cpp_cache_test` 的 `stress thread-safe 8 threads` 段观察并发路径。容量放大本身没有现成断言，需要自己写 3 行构造一个 `ShardedCache<...>(Policy::kLru, {.capacity=4}, 16)` 打印 `Capacity()`——属于半自证。
- **常见错误答案**：「分片后总容量还是 capacity」「每片容量 = capacity / shards 向下取整，0 就 0」「分片不影响 TinyLFU 的素描」——零分。

### Q05 【L4 陷阱】CLOCK 为什么在顺序扫描后命中率恢复不了？它和 LRU 的失效条件差在哪？

- **痛点**：把 CLOCK 当成「省掉链表的 LRU」。CLOCK 只有 1 个 referenced 位，没有访问频率；一次全表扫描把所有槽位都置 1，之后淘汰只能一圈圈清位、退化成 FIFO，热数据已经被换出去了。
- **追查位置**：
  - `include/go2cpp/cache/clock.hpp:74`（`Get` 命中：`slot.referenced = true`，没有频率、没有链表重排）
  - `include/go2cpp/cache/clock.hpp:213`（`FindVictim`：referenced=1 → 清 0 跳过；referenced=0 或空槽 → 选中）
  - `include/go2cpp/cache/clock.hpp:221`（`slot.referenced = false`：第二次机会只给一次）
  - `tests/cache/test_scenarios.cpp:114`（`scenario scan pollution / recovery`：先跑热点建立 baseline，再全扫描，再测恢复）
  - `tests/cache/test_scenarios.cpp:141`（`RecoveryRate` 的计算与打印）
- **验收标准**：
  1. CLOCK 的淘汰最多扫 `2×capacity` 个槽位（一圈清位、一圈替换）；命中只写 1 个 bit，不做任何位置调整。
  2. 顺序扫描会把所有槽位 `referenced=1`；扫描结束后第一轮淘汰全部清位，第二轮才开始真正替换——在这期间所有新条目都能活过一轮，热数据没有保护。
  3. 命中率恢复依赖「重新访问热数据把 bit 置回」，但 CLOCK 没有频率历史，无法像 LRU-K/TinyLFU 那样识别「这个 key 以前很热」；测试里 CLOCK 的 recovery 会明显低于 LRU-K/TinyLFU（具体数值自己跑，别背）。
  4. CLOCK 的容量必须 `>=1`（构造抛 `invalid_argument`），槽位是 `vector<Slot>`、索引走 `BucketMap` + NodePool，稳态无逐条目堆分配。
  5. 适用场景是「条目小、QPS 极高、能接受命中率略低」；不适合有周期性全扫描、又要求扫描后立刻恢复的负载。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_cache_test` → `scenario scan pollution / recovery` 段打印每个策略的 `baseline` 与 `recovery`；CLOCK 的 `recovery` 会显著偏低（本机跑出来是 `0.000`，但**以你跑到的为准**）。再跑 `./go2cpp_cache_bench --quick` 对比 `Mops/s` 与 `peak_bytes`，验证「省链表」换来的吞吐/内存收益。
- **常见错误答案**：「CLOCK 就是 LRU 的等价实现」「referenced 位等于访问计数」「扫描后把所有 bit 清掉就能恢复」——零分。

---

## 命题备注（给自己人看）

- Q01/Q02/Q03 的「错误分支」在现有测试里只覆盖了正确路径（version 校验、ghost→pending、p 方向），要观察坏结果需要临时改代码或写复现，属于半自证。
- Q05 的 `recovery=0.000` 是本机一次实测输出，不是文档数字；换负载/容量会变，验收时只要求「跑出来并解释趋势」。
- Q04 的容量放大没有现成测试，明确标为半自证。
