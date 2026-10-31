# 缓存淘汰算法专题：从 LRU 到 W-TinyLFU 与 GDSF

> 日期：2026-10-31 · 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL）
> 范围：学术综述 + 7 种策略实现 + 多场景基准 + 选型指南。
> 配套代码：`include/go2cpp/cache/**`、`tests/cache/**`、`cmake/Go2CppCache.cmake`。
> 本文所有实测数字来自本仓库 `go2cpp_cache_bench --full`（单线程、GCC 13.3、`-O2`），
> 只用于**相对比较**，不是跨机器可复现的绝对值。

---

## 0. 摘要与结论

缓存淘汰算法研究了几十年，但真正落到工程里的问题始终只有三个：

1. **该淘汰谁**：最近最少使用（recency）、最不经常使用（frequency）、还是
   单位成本收益最低的对象？
2. **该不该让新对象进来**：扫描、一次性回填会把冷数据塞满缓存，淘汰策略再
   好也救不回来；因此现代缓存把「准入（admission）」看得和「淘汰（eviction）」
   一样重要。
3. **对象不一样大**：按条目数计费时，1 MB 的对象和 1 KB 的对象占同一个
   「名额」，字节利用率会被大对象拖垮。

本专题的结论可以压缩成四句话：

- **没有全局最优算法**。LRU 在均匀热点下仍然能打；LRU-K / 2Q / TinyLFU /
  ARC 的收益只在「扫描、一次性流量、突发」出现时才体现，且各自有代价。
- **防冷数据污染的核心是「准入」而不是「淘汰」**。LRU-K 用「访问 K 次才
  晋升」，2Q 用 ghost 队列，TinyLFU 用频率素描，ARC 用 ghost 自适应；四种
  机制的共同点是：**新对象先待在一个小缓冲区，证明自己值得留下**。
- **带权重场景必须用 GDSF 一类按 `cost/size` 计价的策略**。本仓库实测
  「1:1000 大小差异」负载下，GDSF 命中率 0.8158，LRU 只有 0.5579；
  CDN 场景 GDSF 0.7483，LRU 0.4771。代价是 GDSF 的条目数会膨胀，必须同时
  设置内存/条目上限。
- **工程选型优先看「流量形态 + 对象大小分布 + 内存预算」**，而不是论文
  名气。Web 元数据、游戏资源、CDN、数据库 buffer pool 的最优选择并不相同，
  具体见表 5-2。

实测速览（完整表见 §8）：

| 场景 | LRU | LRU-K(2) | 2Q | W-TinyLFU | GDSF | ARC | CLOCK |
|---|---|---|---|---|---|---|---|
| 热点 80/20 | 0.5580 | **0.7904** | 0.6484 | 0.7821 | 0.7693 | 0.7817 | 0.5308 |
| 扫描污染 | 0.3674 | 0.5197 | 0.4248 | 0.5061 | 0.4847 | **0.5104** | 0.3507 |
| 权重 1:1000 | 0.5579 | 0.7914 | 0.6493 | 0.7835 | **0.8158** | 0.7828 | 0.5308 |
| CDN（Zipf） | 0.4771 | 0.6676 | 0.5352 | 0.5883 | **0.7483** | 0.5561 | 0.4611 |
| 混合（热点+扫描+突发） | 0.5396 | **0.6538** | 0.6433 | 0.4997 | 0.4268 | 0.6399 | 0.5322 |

---

## 1. 问题定义与评价维度

### 1.1 缓存替换的形式化

设缓存容量为 `C` 个条目（或 `B` 字节），请求序列为 `r_1, r_2, ..., r_N`，
每个 `r_i` 对应对象 `o_i`、大小 `s_i`、取回成本 `c_i`。一次访问命中当且
仅当 `o_i` 当前在缓存中；未命中时按策略决定「是否装入、淘汰谁」。

评价一个策略，至少要看六个维度：

| 维度 | 含义 | 本专题的度量 |
|---|---|---|
| 命中率 | 命中次数 / 总请求数 | `hits / (hits + misses)` |
| 字节命中率 | 命中字节 / 总请求字节 | GDSF 场景关注 |
| 扫描抗性 | 顺序扫描后热点保留程度 | 扫描后热 key 命中率、恢复窗口 |
| 时间复杂度 | 每次访问的均摊开销 | Get/Put/Erase 的渐进复杂度 |
| 空间开销 | 元数据 / 条目 | 基准里的 peak KiB |
| 可预测性 | 参数是否随负载漂移 | ARC 自适应 vs LRU-K 固定 K |

### 1.2 为什么「命中率」不是唯一指标

缓存不是孤立组件：一次 miss 可能意味着 1 ms 的 SSD 读、100 ms 的跨洋
RTT，或者一次昂贵的模型推理。GDSF 显式把 `cost` 放进公式，就是为了让
「省下的时间/带宽」而不是「省下的条目数」成为优化目标。本文在 §4 单独
讨论这一点。

### 1.3 本专题的边界

- 只讨论**单机、单层**缓存的淘汰/准入，不涉及多级缓存、一致性哈希、
  分布式复制。
- 只讨论**读缓存**（cache-aside / read-through）；写缓存、write-back
  的语义差异在 §3.7 说明。
- 不讨论持久化、TTL、预热；这些与淘汰策略正交。

---

## 2. 算法谱系

### 2.1 LRU（Least Recently Used）

**论文与动机。** LRU 本身没有单篇「发明论文」，它的理论化来自 Mattson、
Gecsei、Slutz、Traiger 1970 年的 *Evaluation Techniques for Storage
Hierarchies*（IBM Systems Journal）。该文提出**栈算法（stack algorithm）**
的概念：LRU 的驻留集合是「按最近访问排序后取前 C 个」，因此容量从 C 增加
到 C+1 时，命中集合只会扩大不会缩小——这就是**栈性质**。

**原理。** 维护一个按最近访问时间排序的链表，命中把节点移到表头，淘汰
表尾。直觉是「最近用过的，未来还会用」（temporal locality）。

**数据结构。** 哈希表 `key -> list::iterator` + 双向链表。命中时 `splice`
只改指针，O(1)。

**伪代码。**

```text
Get(k):
  it = map.find(k)
  if it == end: return MISS
  list.splice(list.begin(), list, it->second)
  return HIT

Put(k, v):
  if k in map: update; move to front
  else:
    while list.size() >= C: evict(list.back())
    list.push_front(k); map[k] = list.begin()
```

**复杂度。** Get / Put / Erase 均摊 O(1)，空间 O(C)。

**优点。** 实现简单、无参数、对均匀热点和短时局部性极好；命中率对容量
单调（栈性质）；工业界默认基线（memcached、Redis、Guava 早期版本）。

**缺点。** 对扫描零抵抗：一次全表扫描会把所有热点挤出；对「访问一次就
再也不来」的流量没有过滤能力；对突发流量（新 key 大量涌入）会抖动。

**适用场景。** 工作集稳定、对象大小均匀、没有明显扫描的 Web 会话/元数据
缓存；作为其它算法的对照基线。

**引用。** Mattson, R., Gecsei, J., Slutz, D., Traiger, I. *Evaluation
Techniques for Storage Hierarchies.* IBM Systems Journal, 9(2):78–117, 1970.

---

### 2.2 LFU（Least Frequently Used）

**论文与动机。** LFU 的思想可追溯到 1970 年代的虚拟内存研究；工程上常
引用 Karedla、Love、Wherry 1994 年的 *Caching Strategies to Improve Disk
System Performance*（IEEE Computer）。它假设「访问次数多的对象更值得
保留」，用频率对抗扫描。

**原理。** 每个对象维护访问计数，淘汰计数最小的；计数相同时用 LRU 打破
平局。为了避免历史计数永不衰减，工业实现通常加**老化（aging）**：每隔
一段时间把所有计数减半，或按时间窗口重置。

**数据结构。** 哈希表 + 每个频率一个双向链表（frequency bucket），再维护
一个指向最小频率的指针。命中时把对象从频率 `f` 的链表移到 `f+1`，O(1)；
淘汰取最小频率链表的尾部。

**复杂度。** Get / Put O(1)，空间 O(C + 不同频率数)。

**优点。** 对「热点长期稳定」的负载命中率高于 LRU；天然抗扫描（扫描
只访问一次，频率低）。

**缺点。** **历史包袱**：一个曾经很热、现在已冷的对象会长期占坑
（cache pollution by stale frequency）；对新热点反应慢；需要老化参数；
实现比 LRU 复杂。

**适用场景。** 内容推荐、广告素材、小而稳定的热数据集；作为 TinyLFU 的
「精确版本」对照。

**引用。** Karedla, R., Love, J., Wherry, B. *Caching Strategies to
Improve Disk System Performance.* IEEE Computer, 27(3):38–46, 1994.

---

### 2.3 LRU-K（1993，防扫描）

**论文与动机。** O'Neil、O'Neil、Weikum 在 SIGMOD 1993 的 *The LRU-K Page
Replacement Algorithm for Database Disk Buffering* 中提出：数据库的顺序
扫描（全表扫描、索引扫描）会让 LRU 把所有热页冲掉。LRU-K 用**第 K 次
最近访问**（而不是最近一次）作为排序依据：只访问过一次的页，无论多新，
都排在热页后面。

**原理。** 每个页维护最近 K 次访问的时间戳；缓存中的页按「第 K 次最近
访问时间」排序（K=2 时就是**倒数第二次访问时间**）。不在缓存中的页，其
访问历史保存在一个小的 **corral（历史队列）** 里；只有累计访问达到 K 次，
才允许进入主缓存。

**数据结构。**
- 主缓存：按 `reference_time` 排序的平衡树/堆 + 哈希索引；
- corral：FIFO 队列 + 哈希表，保存「访问过但未达 K 次」的页。

**伪代码（K=2）。**

```text
Get(k):
  if k in main:
     ref_time[k] = last_visit[k]      # 倒数第二次
     last_visit[k] = now
     return HIT
  if k in corral:
     count[k] += 1
     if count[k] >= 2:
        move k to main with ref_time = last_visit[k]
     return HIT
  return MISS

Put(k):
  if k in main: update; return
  if k in corral: promote if count >= 2; return
  insert k into corral with count = 1
  if corral.size > corral_cap: evict corral tail
```

**复杂度。** 严格实现用平衡树：Get / Put O(log n)；用时间轮/堆可做到
O(log n)，工程近似 O(1)。空间 O(C + corral)。

**优点。** 对顺序扫描几乎免疫；保留 LRU 的 recency 语义；K 可调
（K 越大越接近 LFU，抗扫描越强、对新热点越迟钝）。

**缺点。** 需要维护访问历史，内存约为 LRU 的 2 倍；K 是固定参数，不能
自适应；对「首次访问就要缓存」的写路径不友好（写缓存需要额外规则）。

**适用场景。** 数据库 buffer pool、文件系统页缓存、任何有全表扫描的
存储系统；也是本专题「防污染」的基准算法。

**本仓库实现。** `LruKCache`，K=2，主表用 `std::map<uint64_t, K>` 按
reference time 排序；corral 与主缓存**同容量**（见 §6.4 的差异说明）。

**引用。** O'Neil, E., O'Neil, P., Weikum, G. *The LRU-K Page Replacement
Algorithm for Database Disk Buffering.* SIGMOD 1993, pp. 297–306.

---

### 2.4 2Q（1994）

**论文与动机。** Johnson 与 Shasha 在 VLDB 1994 的 *2Q: A Low Overhead
High Performance Buffer Management Replacement Algorithm* 中指出：LRU 的
问题不是「最近性」本身，而是它把「只访问一次」的页和「访问多次」的页
同等对待。2Q 用三个队列把冷启动、一次性流量和热数据分开。

**原理。**
- **A1in**：FIFO 准入队列，新页先进这里；命中不改变位置（FIFO 的好处是
  一次性页不会因为一次命中就长期占坑）。
- **A1out**：只存 key 的 ghost 队列；从 A1in 被挤出的页在这里留下历史。
- **Am**：LRU 主缓存；只有命中过 A1out 的页（说明它至少被访问两次，且
  跨过一次淘汰）才进入 Am。

**数据结构。** 两个双向链表（A1in、Am）+ 一个 key 链表（A1out）+ 三个
哈希表。A1in 目标份额 25%，A1out 约 50%，Am 约 75%。

**复杂度。** Get / Put 均摊 O(1)，空间 O(C + ghost)。

**优点。** 低开销；对扫描和一次性流量有良好过滤；Am 用 LRU 保留热数据；
不需要频率素描。

**缺点。** 参数（25%/50%）是经验值；冷启动时 Am 增长慢；ghost 命中依赖
「页被淘汰后又被访问」的模式，纯随机负载下收益有限；对突发新热点反应
不如 TinyLFU 快。

**适用场景。** 数据库/存储的 buffer pool、代理缓存、内存受限的嵌入式
缓存；是 LRU-K 的「低开销替代」。

**引用。** Johnson, T., Shasha, D. *2Q: A Low Overhead High Performance
Buffer Management Replacement Algorithm.* VLDB 1994, pp. 439–450.

---

### 2.5 ARC（2003，自适应）

**论文与动机。** Megiddo 与 Modha 在 FAST 2003 的 *ARC: A Self-Tuning,
Low Overhead Replacement Cache* 中提出：与其手工调 LRU/LFU 的权重，不如
让缓存自己学。ARC 维护四个列表，用 ghost 命中率自动调节 recency 与
frequency 的分配。

**原理。**
- **T1**：只访问过一次的常驻页（recency）；
- **T2**：至少访问过两次的常驻页（frequency）；
- **B1/B2**：T1/T2 的 ghost（只存 key）；
- 参数 `p`：T1 的目标大小。B1 命中（淘汰过的一次性页又被访问）说明
  recency 不够，`p` 增大；B2 命中（淘汰过的热页又被访问）说明 frequency
  不够，`p` 减小。
- REPLACE：当 `|T1| > p`（或待插入 key 在 B2 且 `|T1| == p`）时从 T1 尾部
  淘汰到 B1，否则从 T2 尾部淘汰到 B2。

**复杂度。** Get / Put 均摊 O(1)，空间 O(C + ghost)。

**优点。** 无需手工调参；对扫描、突发、热点迁移都有自适应能力；实现
只比 LRU 多两个 ghost 链表；命中率通常接近「离线最优」的 90%+。

**缺点。** ghost 需要额外内存（通常 1×C）；专利历史（IBM 的 ARC 专利，
2023 年前后有争议，工程采用前应做法律评估）；`p` 的浮点调整在极端负载
下可能抖动；对「对象大小差异」无感知。

**适用场景。** 通用 Web 缓存、CDN 边缘、数据库 buffer pool；需要「少
调参、自适应」的场合。

**引用。** Megiddo, N., Modha, D. *ARC: A Self-Tuning, Low Overhead
Replacement Cache.* FAST 2003, pp. 115–130.

---

### 2.6 LIRS（2002）

**论文与动机。** Jiang 与 Zhang 在 SIGMETRICS 2002 的 *LIRS: An Efficient
Low Inter-reference Recency Set Replacement Policy to Improve Buffer Cache
Performance* 中指出：LRU 用「最近性」近似「未来会被访问」，但在
「访问一次后隔很久再访问」的负载（数据库索引扫描、文件系统元数据）上
表现很差。LIRS 用**重用距离（reuse distance / IRR）** 代替最近性。

**原理。** 每个页维护「两次访问之间的间隔」IRR；IRR 小的页是 LIR（低
重用距离，值得常驻），IRR 大的是 HIR（高重用距离，作为缓冲）。LIRS 用
一个栈（stack）动态维护 IRR 排序，并保证所有 LIR 常驻、HIR 只占少量
空间。核心操作是「栈剪枝（stack pruning）」：当 LIR 页被访问时，把它
上面的 HIR 页状态更新。

**复杂度。** 均摊 O(1)，但常数比 LRU 大；空间 O(C)。

**优点。** 在「弱局部性、扫描与热点混合」的负载上显著优于 LRU；对
IRR 分布变化自适应；不需要 ghost 或频率素描。

**缺点。** 实现复杂（栈剪枝、HIR/LIR 状态机），正确性难保证；对突发
新热点的反应不如 ARC；工业落地案例少于 ARC/TinyLFU。

**适用场景。** 文件系统页缓存、数据库 buffer pool；Linux 内核的
active/inactive list 是 LIRS 的近似思想。

**引用。** Jiang, S., Zhang, X. *LIRS: An Efficient Low Inter-reference
Recency Set Replacement Policy to Improve Buffer Cache Performance.*
SIGMETRICS 2002, pp. 31–42.

---

### 2.7 CLOCK-Pro（2005）

**论文与动机。** Jiang、Chen、Zhang 在 USENIX ATC 2005 的 *CLOCK-Pro: An
Effective Improvement of the CLOCK Replacement* 中提出：CLOCK 便宜但只是
LRU 的近似；LIRS 命中率高但实现复杂。CLOCK-Pro 用 CLOCK 的引用位机制
**模拟 LIRS 的 IRR 语义**，把两者的优点合在一起。

**原理。** 每个页有引用位和「是否 LIR」标记；用一个 hand 扫描环，遇到
引用位为 1 的清零并跳过，遇到 0 的根据 LIR/HIR 状态决定淘汰或降级。
CLOCK-Pro 维护 `m` 个「热页」名额，用 ghost 页的引用位近似 IRR：被再次
访问的 ghost 页说明 IRR 小，提升为 LIR。

**复杂度。** Get / Put 均摊 O(1)，空间 O(C + ghost)，无逐条目链表分配。

**优点。** 命中率接近 LIRS，开销接近 CLOCK；对扫描和弱局部性负载都比
LRU 好；Linux 内核后来引入了 CLOCK-Pro 的变体。

**缺点。** 状态机比 CLOCK 复杂；参数 `m`（热页比例）需要调；论文的
伪代码在多核并发下需要额外同步。

**适用场景。** 操作系统页缓存、嵌入式缓存、任何「低开销 + 抗扫描」的
场景。

**引用。** Jiang, S., Chen, F., Zhang, X. *CLOCK-Pro: An Effective
Improvement of the CLOCK Replacement.* USENIX ATC 2005, pp. 323–336.

---

### 2.8 TinyLFU / W-TinyLFU（2015，Caffeine）

**论文与动机。** Einziger、Friedman、Manes 的 *TinyLFU: A Highly Efficient
Cache Admission Policy*（2014 会议版，2017 ACM TOS 期刊版）提出：与其
改进淘汰，不如改进**准入**——用一个极小的频率素描判断「新对象是否比
当前受害者更值得保留」。Caffeine（2015）把 TinyLFU 与一个 1% 的窗口
LRU 结合，形成 **W-TinyLFU**，成为现代 Java 缓存的事实标准。

**原理。**
- **窗口（window）**：占 1%，新对象先落这里；它吸收突发流量，避免
  频率素描被一次性 key 污染。
- **主缓存**：SLRU 两段——probation（20%）是准入区，protected（80%）是
  热数据区。
- **频率素描**：4 行 Count-Min Sketch，4-bit 饱和计数器，定期整体右移
  一位（halving）做老化；**doorkeeper**（Bloom）让第一次出现的 key 不
  进入素描，只有第二次才计数。
- **准入**：窗口淘汰出的候选与 probation 尾部的受害者比较频率，胜者
  留下；候选胜则淘汰受害者，否则拒绝候选。

**数据结构。** 三个双向链表（window/probation/protected）+ 三个哈希表 +
`CountMinSketch4` + `BloomDoorkeeper`。

**复杂度。** Get / Put 均摊 O(1)（素描固定 4 路），空间 O(C) + 素描
（约 16 B/entry 量级）。

**优点。** 扫描抗性极强（一次性 key 频率为 0，进不了主缓存）；对突发
友好（窗口吸收）；频率自适应老化；命中率通常比 LRU 高 5–15 个百分点；
内存开销可控。

**缺点。** 频率素描是近似（只高估不低估）；参数（1%/20%/80%、sample
size）需要按负载调；对「所有对象都只访问一次」的负载没有收益；4-bit
计数器在极端热点下会饱和。

**适用场景。** Web/API 响应缓存、CDN、游戏资源、任何「热点 + 扫描 +
突发」混合的通用缓存；Caffeine、Guava 后继、Ristretto 等都用它。

**引用。** Einziger, G., Friedman, R., Manes, B. *TinyLFU: A Highly
Efficient Cache Admission Policy.* ACM Transactions on Storage, 13(4),
2017（会议版 PDP 2014）；Caffeine 的 W-TinyLFU 实现见
<https://github.com/ben-manes/caffeine/wiki/Efficiency>。

---

### 2.9 GDSF（1997，权重/大小）

**论文与动机。** Cao 与 Irani 在 USITS 1997 的 *Cost-Aware WWW Proxy
Caching Algorithms* 中提出 GreedyDual 家族：代理缓存里的对象大小从几百
字节到几 MB，按条目数淘汰会让大对象挤占大量字节却只贡献一次命中。
GDSF（GreedyDual-Size-Frequency）给每个对象算「单位字节的收益密度」。

**公式。**

```text
H(key) = L + cost(key) / size(key) × freq(key)
```

- `cost`：取回成本（默认 1；可以是延迟、带宽费用、后端 QPS 成本）；
- `size`：对象字节数；
- `freq`：访问频率；
- `L`：全局「年龄/通胀」水位，初始 0；每次淘汰令 `L = H(victim)`。

淘汰 `H` 最小的对象。把 `L` 提出来后，实际只需比较
`key_value = cost/size × freq`；淘汰后 `L += key_value(victim)`。

**例子。** `cost=1`：
- 对象 A：1 MB，访问 10 次 → `10 / 1,000,000 = 1e-5`；
- 对象 B：10 KB，访问 5 次 → `5 / 10,000 = 5e-4`。
B 的密度是 A 的 50 倍，内存紧张时先淘汰 A——尽管 A 更「热」。

**数据结构。** 哈希表 + 小顶堆（key_value 排序）；命中/更新时 push 新
节点，旧节点惰性删除（按 version 跳过）；堆项超过条目数 4 倍时 O(n)
重建。

**复杂度。** Get / Put / Erase 均摊 O(log n)，空间 O(C + 堆)。

**优点。** 显式支持大小/成本差异；字节利用率高；对 CDN、图片、视频
分片、模型文件等「大小长尾」场景收益显著；公式简单、可解释。

**缺点。** 频率偏差（历史热点难淘汰）；大对象可能饥饿；`L` 单调增长
有浮点精度问题；堆带来额外内存和 O(log n) 开销；需要同时设置字节预算
和条目上限，否则小对象会让条目数爆炸（本仓库实测 weighted 场景
44,618 条、12 MB 元数据）。

**适用场景。** CDN/代理缓存、对象存储、游戏资源包、图片/视频分片、
任何对象大小差异超过 10× 的缓存。

**引用。** Cao, P., Irani, S. *Cost-Aware WWW Proxy Caching Algorithms.*
USITS 1997, pp. 193–206.

---

### 2.10 S3-FIFO（2023，SOSP）

**论文与动机。** Yang、Zhang、Qiu、Yue、Vinayak 在 SOSP 2023 的 *FIFO
Queues Are All You Need for Cache Eviction* 中提出 S3-FIFO：现代缓存
（尤其是 SSD/对象存储）的瓶颈是**写放大和元数据开销**，不是命中率小数点
后几位。S3-FIFO 只用三个 FIFO 队列，几乎不做「提升/移动」，在保持
竞争力的同时把每次访问的开销压到最低。

**原理。**
- **S（small）**：约 10% 的小 FIFO，新对象先进这里；
- **M（main）**：约 90% 的主 FIFO；
- **G（ghost）**：被 S 淘汰的 key，用于判断「是否值得进 M」；
- 对象在 S 中被访问两次（或命中 ghost）才晋升 M；M 中淘汰时按引用位
  做一次「快速降级」，不维护严格 LRU。

**复杂度。** Get / Put 均摊 O(1)，空间 O(C + ghost)，无堆、无频率素描。

**优点。** 极低开销；对扫描和突发有良好鲁棒性；在 SSD 缓存上减少写
放大；实现比 ARC/TinyLFU 简单。

**缺点。** 命中率通常略低于 W-TinyLFU/ARC（论文在部分负载上持平）；
对「强 recency」负载不如 LRU；ghost 需要额外内存；2023 年才发表，
工业验证还不如 ARC/TinyLFU。

**适用场景。** 大规模对象缓存、CDN 边缘、SSD 缓存、KV 存储；任何
「QPS 极高、元数据开销敏感」的场景。

**引用。** Yang, J., Zhang, Y., Qiu, Z., Yue, Y., Vinayak, R. *FIFO
Queues Are All You Need for Cache Eviction.* SOSP 2023.

---

### 2.11 对比总表

| 算法 | 年份 | Get | Put | 扫描抗性 | 权重感知 | 自适应 | 额外内存 | 实现复杂度 |
|---|---|---|---|---|---|---|---|---|
| LRU | 1970 | O(1) | O(1) | 无 | 否 | 否 | 0 | 低 |
| LFU | 1990s | O(1) | O(1) | 强 | 否 | 老化参数 | 频率桶 | 中 |
| LRU-K | 1993 | O(log n) | O(log n) | 强 | 否 | K 固定 | corral | 中 |
| 2Q | 1994 | O(1) | O(1) | 强 | 否 | 份额固定 | ghost | 中 |
| ARC | 2003 | O(1) | O(1) | 强 | 否 | p 自适应 | ghost | 中 |
| LIRS | 2002 | O(1) | O(1) | 很强 | 否 | IRR 自适应 | 栈/状态 | 高 |
| CLOCK-Pro | 2005 | O(1) | O(1) | 很强 | 否 | IRR 近似 | ghost | 高 |
| W-TinyLFU | 2015 | O(1) | O(1) | 很强 | 否 | halving | 素描+Bloom | 中高 |
| GDSF | 1997 | O(log n) | O(log n) | 中 | **是** | freq 老化 | 堆 | 中 |
| S3-FIFO | 2023 | O(1) | O(1) | 强 | 否 | 引用位 | ghost | 低 |

---

## 3. 防冷数据污染（scan resistance）

### 3.1 污染是怎么发生的

LRU 的驻留集合由「最近 C 次不同访问」决定。假设热点是 100 个 key，
缓存容量 1000，应用每分钟做一次全表扫描（10 万个 key）。扫描开始后，
每个 key 只访问一次，LRU 会把它们依次塞进缓存，把 100 个热点全部挤出去；
扫描结束后，热点需要重新「预热」。这就是**冷数据污染**。

关键点：**不是 LRU 的链表写错了，而是「最近性」这个信号本身被一次性
流量污染**。任何只看「最近一次访问」的策略都会中招。

### 3.2 LRU-K：用「第 K 次访问」过滤

LRU-2 的准入规则是「访问两次才进主缓存」。扫描 key 只访问一次，永远
留在 corral，随 FIFO 被挤出；主缓存中的热点不受影响。

本仓库实测（容量 2000，扫描 5 万个唯一 key）：
- LRU 扫描阶段整体命中率 0.3674，扫描结束后的恢复需要 2 个窗口（4000 请求）；
- LRU-K 扫描阶段 0.5197，恢复只需 1 个窗口（2000 请求）；
- 扫描后热 key 命中率：LRU 0.000，LRU-K 1.000（`test_scenarios` 断言）。

**代价。** corral 必须足够大。我们最初把 corral 设为 `capacity/4`，结果
工作集（100 个热 key）大于 corral（25）时，热 key 在第二次访问前就被
挤出，主缓存永远填不满——实测预热后命中率只有 0.25。改成 corral 与主
缓存同容量后恢复到 1.00。这是一个值得写进文档的实现陷阱。

### 3.3 2Q：用 ghost 过滤

2Q 的 A1in 是 FIFO，命中不改变位置——这一点常被误解。FIFO 的意义是：
一次性 key 在 A1in 里「排队等死」，不会因为一次命中就获得 LRU 的二次
机会。只有命中过 A1out 的 key（至少被访问两次、跨过一次淘汰）才进 Am，
而 Am 是真正的 LRU 保护区。

**实测。** 扫描场景 2Q 为 0.4248，高于 LRU 的 0.3674，但低于 LRU-K/
TinyLFU/ARC。原因是本仓库的 2Q 在「热工作集恰好等于容量」时 Am 增长
慢；这是 2Q 的固有特性，不是实现错误（见 §8.5）。

### 3.4 TinyLFU：用频率准入

W-TinyLFU 的抗扫描来自三层：
1. **窗口只有 1%**：扫描 key 在窗口里互相淘汰，不会直接冲击主缓存；
2. **doorkeeper**：第一次出现的 key 不进入频率素描，所以扫描 key 的
   估计频率是 0；
3. **准入比较**：候选频率 0 < 受害者频率（热点被访问过多次），候选被拒。

本仓库实测扫描场景 0.5061，恢复 2 个窗口。`rejections` 计数在扫描阶段
持续增长，直接证明准入在起作用。

### 3.5 ARC：用 ghost 自适应

ARC 把「只访问过一次」的 key 放 T1，把「访问过两次以上」的放 T2。扫描
key 进入 T1 后很快被 REPLACE 到 B1，T2 的热点不受影响；B1 命中又会把
`p` 拉大，让 T1 获得更多空间。本仓库实测扫描场景 0.5104，恢复 1 个
窗口，与 LRU-K 相当。

### 3.6 实测：扫描场景

`go2cpp_cache_bench --full` 的 scan 行（容量 2000，预热 5 万、扫描 5 万、
恢复 5 万）：

| 策略 | 整体命中率 | 恢复窗口位置 | 说明 |
|---|---|---|---|
| LRU | 0.3674 | 4000 | 扫描冲刷，恢复慢 |
| LRU-K(2) | 0.5197 | 2000 | corral 挡住扫描 |
| 2Q | 0.4248 | 2000 | A1in/ghost 挡住扫描，Am 增长慢 |
| W-TinyLFU | 0.5061 | 4000 | 频率准入拒绝扫描 |
| GDSF | 0.4847 | 2000 | 频率+大小，扫描 key 密度低 |
| ARC | 0.5104 | 2000 | T2 保护热点 |
| CLOCK | 0.3507 | 4000 | 与 LRU 同病 |

### 3.7 工程边界：写缓存与 read-through

上述抗扫描结论成立的前提是**读穿透**：Get miss 后应用加载并 Put。
如果应用对扫描 key 也主动 Put（write-through/write-back 缓存），
LRU-K 的 corral 会被写入填满，2Q 的 A1in 也会被写入冲刷，抗扫描能力
下降。工程上的做法是：
- 写路径单独走「写缓存」策略（例如 write-back + 延迟淘汰）；
- 或在 Put 时检查 `cost`，只缓存「值得」的对象；
- 或使用 GDSF 的 `cost` 参数把「取回成本」纳入决策。

---

## 4. 带权重的淘汰（size/cost）

### 4.1 为什么按条目数不公平

设缓存有 1000 个条目预算，对象大小分布是 80% 1 KB、20% 1 MB。按条目
数，缓存可以放 800 个小对象 + 200 个大对象，占用约 200 MB；但 200 个
大对象只服务 20% 的请求，而 800 个小对象服务 80% 的请求。按字节预算，
同样 200 MB 可以放 20 万个小对象，命中率会显著提高。

**命中率（按请求）和字节命中率（按字节）是两个不同的目标**，GDSF 优化
的是前者在字节约束下的版本。

### 4.2 GDSF 公式推导

目标：在总字节 `B` 的约束下，最大化「单位字节的命中收益」。GreedyDual
的做法是给每个对象一个「收益密度」`H`，每次淘汰密度最小的：

```text
H(key) = L + cost(key) / size(key) × freq(key)
```

`L` 是全局水位，作用是让「新对象」和「老对象」在同一尺度上比较：
每次淘汰后 `L` 更新为被淘汰对象的 `H`，于是新对象的 `H` 总是从当前
水位开始，不会因为历史 `L` 累积而永远排不上。

**实现等价形式。** 设 `kv(key) = cost/size × freq`，则 `H = L + kv`；
`L` 对所有对象相同，所以堆按 `kv` 排序即可。淘汰时
`L ← L + kv(victim)`。本仓库的 `GdsfCache` 就是这样实现的。

### 4.3 实现细节

- 每个条目保存 `value / size / freq / key_value / version`；
- 命中：`freq++`（饱和到 `2^40`），重算 `key_value`，push 新堆节点，
  `version++`；
- 淘汰：弹出堆顶，若 `version` 不匹配则跳过（惰性删除）；
  匹配则 `L += key_value`，删除条目；
- 更新：先摘出旧条目、按新 size 重新申请空间，再插回（避免更新时把
  自己淘汰）；
- 保护：`weight > max_weight` 直接拒绝；堆项超过 `4×条目数+64` 时
  O(n) 重建。

### 4.4 与其它加权策略对比

| 策略 | 公式/思想 | 优点 | 缺点 |
|---|---|---|---|
| GDSF | `L + cost/size × freq` | 简单、可解释、支持 cost | 频率偏差、堆开销 |
| GDS | `L + cost/size` | 更平滑、对频率不敏感 | 不适合热点差异大的负载 |
| GD* | `L + cost/size` 的变体 | 对大小更鲁棒 | 实现复杂 |
| Hyperbolic | `value / (size × age)` | 对年龄连续衰减 | 参数多、难调 |
| LRU-Size | 按大小加权 LRU | 简单 | 不是最优密度 |

### 4.5 实测：1:1000 场景

`go2cpp_cache_bench --full` 的 weighted 行（80% 1 单位、20% 1000 单位）：

| 策略 | 命中率 | 条目数 | 总权重 | 峰值内存 |
|---|---|---|---|---|
| LRU | 0.5579 | 2000 | 408,593 | 143 KiB |
| LRU-K | 0.7914 | 4000 | 759,244 | 380 KiB |
| 2Q | 0.6493 | 2000 | 376,625 | 226 KiB |
| W-TinyLFU | 0.7835 | 2000 | 384,617 | 220 KiB |
| **GDSF** | **0.8158** | 44,618 | 401,261 | 12,294 KiB |
| ARC | 0.7828 | 2000 | 420,581 | 280 KiB |
| CLOCK | 0.5308 | 2000 | 415,586 | 174 KiB |

**解读。**
- GDSF 比 LRU 高 25.8 个百分点，比最好的非加权策略（LRU-K）高 2.4 个
  百分点；它的优势来自「小对象优先」，而不是频率。
- GDSF 的条目数是其它策略的 22 倍（44,618 vs 2,000），峰值内存 12 MB。
  原因是字节预算下每个 1 单位小对象都值得留，元数据（哈希节点 + 堆节点 +
  条目）远大于对象本身。**生产环境必须给 GDSF 同时设置条目上限**，或者
  把 `weight` 设为「对象大小 + 元数据开销」。
- CDN 场景同样：GDSF 0.7483 vs LRU 0.4771，条目 6,570，内存 1.6 MB。

### 4.6 参数与陷阱

- **`cost` 的语义**：默认 1。如果后端是跨地域对象存储，`cost` 可以设为
  RTT/带宽成本；如果所有对象成本相同，保持 1 即可。
- **大对象饥饿**：GDSF 会长期淘汰大对象。如果业务要求「大对象至少保留
  一份」，需要叠加一个「保护集」或分层缓存。
- **权重与条目上限**：GDSF 的 `capacity=0` 表示只按字节预算；此时条目数
  由对象大小分布决定，可能爆炸。建议 `capacity` 也设为「字节预算 / 最小
  对象大小」的合理上限。
- **`L` 的数值稳定性**：`L` 单调增长，长时间运行可能损失精度；可以定期
  把所有 `key_value` 减去一个常数、或把 `L` 重置为 0（相对顺序不变）。
  本仓库未实现该优化，文档标为已知限制。

---

## 5. 多场景选型指南

### 5.1 决策树

```text
对象大小差异 > 10×？
├── 是 → 需要字节预算？
│        ├── 是 → GDSF（+ 条目上限），如 CDN/图片/视频/游戏资源
│        └── 否 → W-TinyLFU 或 ARC
└── 否 → 有明显扫描/一次性流量？
         ├── 是 → 数据库/文件系统：LRU-K 或 2Q；通用：W-TinyLFU/ARC
         └── 否 → 追求极低开销：CLOCK；追求命中率：W-TinyLFU/ARC
```

### 5.2 场景 → 推荐算法

| 场景 | 推荐 | 备选 | 关键参数 | 理由 |
|---|---|---|---|---|
| Web/API 响应缓存（对象小、热点集中） | W-TinyLFU | ARC、LRU | window 1%、sketch 容量 10×C | 抗扫描、抗突发、命中率高 |
| 静态资源/图片（大小差异大） | GDSF | W-TinyLFU + 大小分桶 | 字节预算 + 条目上限 | 单位字节收益最大化 |
| CDN 边缘（Zipf、分片、大对象） | GDSF | ARC + 大小感知 | `cost=1`、字节预算 | 实测 GDSF 0.7483 vs LRU 0.4771 |
| 数据库 buffer pool（页、扫描） | LRU-K(2) | 2Q、CLOCK-Pro | K=2、corral≈C | 扫描免疫、页大小均匀 |
| 文件系统页缓存 | CLOCK-Pro / LIRS | LRU-K、CLOCK | 热页比例 m | 低开销 + 抗扫描 |
| 游戏资源包（大对象、热更新） | GDSF | ARC | 字节预算 + 版本化 key | 大对象成本高、命中收益大 |
| 边缘/嵌入式（内存小、低开销） | CLOCK | 2Q、S3-FIFO | 引用位、无堆 | 元数据少、O(1) |
| KV/元数据（小对象、高 QPS） | CLOCK 或 W-TinyLFU | 分片 LRU | 分片数 = 核数×2 | 锁竞争优先 |
| 特征/模型缓存（大对象、成本高） | GDSF | ARC | `cost`=推理成本 | 成本感知 |
| 突发新闻/社交（突发 + 长尾） | W-TinyLFU | ARC | 窗口 1% | 窗口吸收突发 |
| 顺序日志/流式分析（扫描为主） | S3-FIFO / LRU-K | 2Q | 不要用 LRU | 避免污染 |
| 多核高吞吐（读多写少） | 分片 W-TinyLFU 或分片 CLOCK | 分片 LRU | 分片数 = 核数 | 降低全局锁 |

### 5.3 推荐参数

- **W-TinyLFU**：window = 1%，protected = 80% × main，probation = 20%；
  sketch 宽度 = `next_pow2(8 × C)`，上限 1M；sample size = `10 × C`；
  doorkeeper 位图 = `next_pow2(8 × C)`。
- **LRU-K**：K=2；corral = C（本仓库结论；论文建议小比例，但工程上
  工作集大于 corral 时会退化）。
- **2Q**：A1in = 25%，A1out = 50%，Am = 75%；如果热工作集 ≤ C，
  可以先把热数据「预热」进 Am。
- **ARC**：ghost = C；`p` 初始 0；不需要调参。
- **GDSF**：`cost=1`；`max_weight` = 可用内存 × 0.6；`capacity` =
  `max_weight / 最小对象大小` 的 2~4 倍作为上限。
- **CLOCK**：capacity = 内存 / 平均条目大小；无参数。

### 5.4 反模式

- **用 LRU 做 CDN/大对象缓存**：字节利用率低，实测命中率只有 GDSF 的
  六成。
- **用 LFU 不做老化**：历史热点永远不淘汰，冷启动后缓存被「僵尸 key」
  占满。
- **给 GDSF 只设字节预算、不设条目上限**：小对象条目数爆炸，元数据
  内存可能超过缓存对象本身（本仓库 weighted 场景 12 MB vs 400 KB 权重）。
- **用 ARC 但不评估专利**：ARC 有 IBM 专利历史，商用前做法律评估。
- **在扫描型负载上用 LRU**：每轮扫描都重新预热，命中率长期低位。

---

## 6. 本仓库实现

### 6.1 目录与接口

```text
include/go2cpp/cache/
  cache.hpp          # Cache<K,V>、CacheOptions、CacheStats、ThreadSafeCache
  lru.hpp            # LruCache
  lru_k.hpp          # LruKCache（K=2）
  two_queue.hpp      # TwoQueueCache（2Q）
  tiny_lfu.hpp       # TinyLfuCache（W-TinyLFU）
  gdsf.hpp           # GdsfCache（GDSF）
  arc.hpp            # ArcCache（ARC）
  clock.hpp          # ClockCache（CLOCK）
  factory.hpp        # Policy、MakeCache、MakeThreadSafeCache、ShardedCache
  workload.hpp       # 5 种可复现负载生成器（测试/基准共用）
  detail/hash.hpp    # SplitMix64、NextPowerOfTwo
  detail/sketch.hpp  # CountMinSketch4、BloomDoorkeeper
tests/cache/         # 单元/随机/场景/压测 + 基准
cmake/Go2CppCache.cmake
```

**统一接口。** `Cache<K, V>` 提供 `Get / Peek / Put / Erase / Clear / Size /
Capacity / Weight / Stats / Name / Describe`；`Put` 的第三个参数是权重
（默认 1）。`Peek` 不改变淘汰顺序、不计命中统计，用于诊断。

**线程安全。** 裸策略不加锁；`ThreadSafeCache<K, V, Lock>` 用装饰器
加锁，`Lock` 默认 `std::mutex`，可传 `go2cpp::sync::Mutex` 获得
Fiber-aware 语义。`ShardedCache` 按 key 哈希分片，每片独立锁。

### 6.2 策略映射

| 论文/算法 | 类 | 与论文的关键差异 |
|---|---|---|
| LRU | `LruCache` | 无 |
| LRU-K | `LruKCache` | K 固定 2；corral 与主缓存同容量；Get 命中 corral 即提升 |
| 2Q | `TwoQueueCache` | ghost 命中记入 `pending_promote`，由下一次 Put 提升到 Am（接口 Get 不负责加载） |
| ARC | `ArcCache` | ghost 命中记入 pending，由 Put 完成提升；ghost 总容量 = C |
| W-TinyLFU | `TinyLfuCache` | 4-bit 4 行素描；Bloom doorkeeper；读时计频、写不计频；1%/20%/80% |
| GDSF | `GdsfCache` | 堆 + 惰性删除 + version；`L` 用累加；`cost` 可配置 |
| CLOCK | `ClockCache` | 槽位数组 + free list；二次机会；不实现 CLOCK-Pro 的 LIR/HIR |

### 6.3 复杂度与内存（实现层）

| 类 | Get | Put | 额外结构 |
|---|---|---|---|
| LruCache | O(1) | O(1) | list + map |
| LruKCache | O(log n) | O(log n) | map(ref_time) + corral FIFO |
| TwoQueueCache | O(1) | O(1) | 3 list + 3 map + pending set |
| TinyLfuCache | O(1) | O(1) | 3 list + 3 map + sketch + bloom |
| GdsfCache | O(log n) | O(log n) | map + 堆（惰性删除） |
| ArcCache | O(1) | O(1) | 2 list + 2 ghost list + 4 map + pending |
| ClockCache | O(1) | O(1) 均摊 | vector 槽位 + map |

### 6.4 与论文的差异（诚实清单）

1. **LRU-K 的 corral 容量**：论文建议 corral 独立于主缓存、通常较小；
   本实现设为 `max(4, C)`，因为实测 `C/4` 在工作集较大时会让热 key 在
   第二次访问前被挤出（主缓存命中率跌到 0.25）。代价是内存约 2×。
2. **LRU-K 的 reference time**：严格按「倒数第二次访问时间」排序，
   Get 命中主表时用 `last_visit` 更新 `reference_time`；这与论文一致。
3. **2Q 的 ghost 提升**：接口的 Get 不负责从后端加载，所以 ghost 命中
   只记 pending，由紧随其后的 Put 完成 Am 插入；论文中这一步发生在
   「读磁盘后重新插入」路径上。
4. **ARC 的 ghost 命中**：同上，Get 只调整 `p` 并记 pending，Put 完成
   T2 插入。
5. **TinyLFU 的写路径**：Put 不计频（与 Caffeine 一致），只有读计频；
   这样扫描/回填不会抬高频率。素描的 4-bit 计数器、halving、Bloom
   doorkeeper 与 Caffeine 同构，哈希用 SplitMix64。
6. **GDSF 的 `L`**：用累加 `L += kv(victim)` 实现，等价于 `L = H(victim)`；
   未做 `L` 的周期性归一化（已知限制）。
7. **并发**：裸策略非线程安全；`ThreadSafeCache` 是粗粒度互斥；
   `ShardedCache` 分片后失去跨分片全局顺序。

### 6.5 测试覆盖

`tests/cache/` 共 12 个测试 TU + 1 个基准：

- **单元**：每个算法的 Get/Put/Erase/Peek/Clear/淘汰顺序/统计/权重/边界
  （容量 1、容量 0 异常、空指针 out、超大值）。
- **随机**：7 种策略 × 20,000 次随机操作，对拍「命中值 == 最后 Put 值」、
  容量不变量、`hits+misses` 守恒；小容量 1..7；字符串 key/value；
  `ThreadSafeCache` 包装。
- **场景**：5 个（热点、扫描、权重、CDN、混合），断言策略语义（谁该抗
  扫描、GDSF 该偏向小对象）。
- **压测**：LRU 100 万次读穿透（16.6 Mops/s）、LRU-K 20 万次写入
  （corral 有界）、GDSF 20 万次权重压力（权重 ≤ 预算）、ARC 30 万次
  （ghost 有界）、TinyLFU 8 线程 × 5 万（统计守恒）、CLOCK 50 轮 Clear。
- **头文件**：所有头自包含、7 个策略 + 工厂 + 分片模板实例化。

运行结果：`go2cpp_cache_test` 全部通过（0.64 s），`ctest -R cache`
2/2 通过。

---

## 7. 基准方法

### 7.1 负载

| 场景 | 生成方式 | 目的 |
|---|---|---|
| hotspot | 80% 请求落在 20% key（热点池 = C） | 基本命中率、recency |
| scan | 热点 + 每周期 25% 顺序扫描（唯一 key） | 防污染、恢复时间 |
| weighted | 80% 1 单位 + 20% 1000 单位 | 权重/大小感知 |
| cdn | Zipf(0.9) 视频 × 100 分片 × 64~4096 单位 | 长尾 + 大对象 |
| mixed | 热点 + 周期扫描 25% + 10% 概率短突发 | 综合压力 |

所有场景由固定 seed 生成，同一场景下所有策略跑**逐请求相同**的序列。
请求模型是读穿透：Get 未命中立即 Put（模拟 cache-aside）。

### 7.2 指标

- **命中率**：`hits / (hits + misses)`，包含扫描阶段的 miss，因此能反映
  污染对整体命中率的影响。
- **恢复窗口**：扫描结束后，按 2000 请求一个窗口统计命中率；首个达到
  「预热阶段稳态命中率 × 90%」的窗口位置（-1 = 未恢复）。
- **吞吐**：整段序列的 Mops/s（含 Put 与淘汰）。
- **内存**：替换全局 `operator new/delete`，记录缓存构造前到运行峰值的
  **净分配字节**；销毁后回到基线才算无泄漏。
- **大小/权重**：`Size()`、`Weight()`、`Stats()`。

### 7.3 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B .build-bento-wsl -DGO2CPP_BUILD_TESTS=ON
cmake --build .build-bento-wsl --target go2cpp_cache_test go2cpp_cache_bench -j 8
./.build-bento-wsl/go2cpp_cache_test
./.build-bento-wsl/go2cpp_cache_bench --quick   # ctest 默认
./.build-bento-wsl/go2cpp_cache_bench --full    # 完整数据
```

### 7.4 环境

- WSL2 Ubuntu 24.04，x86_64；
- GCC 13.3.0，`-O2 -Wall -Wextra -Wpedantic`；
- 单线程基准（并发测试单独在 `go2cpp_cache_test` 的压测段）；
- 内存统计包含 STL 节点分配，**不含** `requests` 向量（在基线前分配）。

---

## 8. 实测结果（`--full`）

### 8.1 命中率

capacity = 2000，ops = 300,000（scan 场景：预热 50k + 扫描 50k + 恢复 50k）。

| scenario | policy | hit_rate | recovery_ops | Mops/s | peak_KiB | size | weight |
|---|---|---|---|---|---|---|---|
| hotspot | LRU | 0.5580 | -1 | 28.39 | 143 | 2000 | 2000 |
| hotspot | LRU-K(2) | 0.7904 | -1 | 6.68 | 380 | 4000 | 4000 |
| hotspot | 2Q | 0.6484 | -1 | 17.99 | 226 | 2000 | 2000 |
| hotspot | W-TinyLFU | 0.7821 | -1 | 13.15 | 220 | 2000 | 2000 |
| hotspot | GDSF | 0.7693 | -1 | 18.41 | 415 | 2000 | 2000 |
| hotspot | ARC | 0.7817 | -1 | 23.90 | 280 | 2000 | 2000 |
| hotspot | CLOCK | 0.5308 | -1 | 30.20 | 174 | 2000 | 2000 |
| scan | LRU | 0.3674 | 4000 | 30.21 | 143 | 2000 | 2000 |
| scan | LRU-K(2) | 0.5197 | 2000 | 8.87 | 380 | 4000 | 4000 |
| scan | 2Q | 0.4248 | 2000 | 14.71 | 226 | 2000 | 2000 |
| scan | W-TinyLFU | 0.5061 | 4000 | 12.60 | 220 | 2000 | 2000 |
| scan | GDSF | 0.4847 | 2000 | 17.89 | 415 | 2000 | 2000 |
| scan | ARC | 0.5104 | 2000 | 16.86 | 277 | 2000 | 2000 |
| scan | CLOCK | 0.3507 | 4000 | 31.41 | 174 | 2000 | 2000 |
| weighted | LRU | 0.5579 | -1 | 29.97 | 143 | 2000 | 408593 |
| weighted | LRU-K(2) | 0.7914 | -1 | 7.00 | 380 | 4000 | 759244 |
| weighted | 2Q | 0.6493 | -1 | 18.39 | 226 | 2000 | 376625 |
| weighted | W-TinyLFU | 0.7835 | -1 | 13.79 | 220 | 2000 | 384617 |
| weighted | GDSF | 0.8158 | -1 | 12.35 | 12294 | 44618 | 401261 |
| weighted | ARC | 0.7828 | -1 | 25.68 | 280 | 2000 | 420581 |
| weighted | CLOCK | 0.5308 | -1 | 31.40 | 174 | 2000 | 415586 |
| cdn | LRU | 0.4771 | -1 | 21.37 | 143 | 2000 | 2272640 |
| cdn | LRU-K(2) | 0.6676 | -1 | 7.11 | 380 | 4000 | 4446976 |
| cdn | 2Q | 0.5352 | -1 | 13.81 | 226 | 2000 | 2243648 |
| cdn | W-TinyLFU | 0.5883 | -1 | 12.07 | 220 | 2000 | 2260608 |
| cdn | GDSF | 0.7483 | -1 | 16.94 | 1591 | 6570 | 2319808 |
| cdn | ARC | 0.5561 | -1 | 12.77 | 287 | 2000 | 2298944 |
| cdn | CLOCK | 0.4611 | -1 | 25.37 | 174 | 2000 | 2288320 |
| mixed | LRU | 0.5396 | -1 | 33.23 | 143 | 2000 | 2000 |
| mixed | LRU-K(2) | 0.6538 | -1 | 10.38 | 380 | 3999 | 3999 |
| mixed | 2Q | 0.6433 | -1 | 18.86 | 226 | 2000 | 2000 |
| mixed | W-TinyLFU | 0.4997 | -1 | 12.25 | 220 | 2000 | 2000 |
| mixed | GDSF | 0.4268 | -1 | 14.58 | 415 | 2000 | 2000 |
| mixed | ARC | 0.6399 | -1 | 16.00 | 287 | 2000 | 2000 |
| mixed | CLOCK | 0.5322 | -1 | 33.95 | 174 | 2000 | 2000 |

### 8.2 吞吐

按场景中位数（Mops/s）：

| 策略 | 热点 | 扫描 | 加权 | CDN | 混合 |
|---|---|---|---|---|---|
| CLOCK | 30.20 | 31.41 | 31.40 | 25.37 | 33.95 |
| LRU | 28.39 | 30.21 | 29.97 | 21.37 | 33.23 |
| ARC | 23.90 | 16.86 | 25.68 | 12.77 | 16.00 |
| 2Q | 17.99 | 14.71 | 18.39 | 13.81 | 18.86 |
| GDSF | 18.41 | 17.89 | 12.35 | 16.94 | 14.58 |
| TinyLFU | 13.15 | 12.60 | 13.79 | 12.07 | 12.25 |
| LRU-K | 6.68 | 8.87 | 7.00 | 7.11 | 10.38 |

**解读。** CLOCK/LRU 最快（无额外结构、缓存友好）；LRU-K 最慢（`std::map`
+ corral，O(log n)）；TinyLFU 的素描/准入有固定开销；GDSF 的堆在命中
路径上 push，成本高。吞吐与命中率需要按业务权衡：CDN 命中率提升 27 个
百分点通常比 5 Mops/s 的吞吐差异更值钱。

### 8.3 内存

| 策略 | 热点 | 加权 | CDN | 说明 |
|---|---|---|---|---|
| LRU | 143 KiB | 143 | 143 | 约 73 B/条目 |
| CLOCK | 174 | 174 | 174 | 槽位数组 + map |
| 2Q | 226 | 226 | 226 | 两个常驻 list + ghost |
| TinyLFU | 220 | 220 | 220 | 素描 + Bloom + 三个 list |
| ARC | 280 | 280 | 287 | 两个常驻 + 两个 ghost |
| LRU-K | 380 | 380 | 380 | 主表 + corral（2×） |
| GDSF | 415 | **12,294** | 1,591 | 堆 + 大量小条目 |

### 8.4 扫描恢复

- LRU / CLOCK：扫描阶段整体命中率 0.3674 / 0.3507，恢复需要 2 个窗口
  （4000 请求）；扫描后热 key 命中率 0（`test_scenarios`）。
- LRU-K：扫描阶段 0.5197，恢复 1 个窗口（2000）；热 key 命中率 1.000。
- TinyLFU：扫描阶段 0.5061，恢复 2 个窗口（4000）；热 key 命中率 0.99。
- ARC：扫描阶段 0.5104，恢复 1 个窗口（2000）；热 key 命中率 0.99。
- GDSF：0.4847，恢复 1 个窗口（2000）；频率+大小双重过滤。

### 8.5 解读与反直觉结果

1. **LRU-K 在热点场景最高（0.7904）**：因为 corral 与主缓存同容量，每个
   热 key 都有机会晋升；同时它的「二次访问门槛」过滤了 20% 的冷请求。
   这不是说 LRU-K 通用最优——它的吞吐只有 6.7 Mops/s，内存 2×。
2. **2Q 在热点场景只有 0.6484**：热工作集等于容量时，A1in 长期占满，
   Am 增长慢；2Q 的优势在「热工作集 < 容量」或「有明确淘汰-回归模式」，
   不在「工作集恰好等于容量」。
3. **TinyLFU 在混合场景只有 0.4997**：混合场景的突发是「新 key 连续 8 次
   后换新」，频率低、窗口只有 1%，准入会拒绝它们；这是 TinyLFU 用命中率
   换扫描抗性的代价。热点场景它仍有 0.7821。
4. **GDSF 在混合场景只有 0.4268**：按条目容量时，GDSF 的堆开销和频率偏差
   让它不如 ARC；GDSF 应该用在「大小差异大 + 字节预算」的场景，而不是
   纯热点。
5. **CLOCK 与 LRU 的命中率差 2–3 个百分点**，但吞吐高 5–10%，在 QPS
   极高的边缘场景是合理交换。

---

## 9. 限制与后续

### 9.1 未实现的算法

- **LIRS**：栈剪枝 + HIR/LIR 状态机，实现复杂度高；本专题优先交付可验证
  的 7 种策略。
- **CLOCK-Pro**：需要 LIR/HIR 状态和 ghost 引用位；本仓库的 `ClockCache`
  只实现基础 CLOCK。
- **S3-FIFO**：2023 年新算法，需要 S/M/G 三个 FIFO 和快速降级；实现
  不难，但本专题的 5 个场景不足以证明它的优势（它面向 SSD 写放大）。

后续可以按同一接口补这三个类，并复用 `workload.hpp` 与基准。

### 9.2 基准的局限

- 合成负载：热点/扫描/权重/CDN/混合是近似，真实业务的长尾、周期性、
  多租户隔离未覆盖。
- 单线程：吞吐数字不含锁竞争；并发只做了正确性压测，没做扩展性曲线。
- 内存统计是「净分配峰值」，不含 allocator 的碎片/arena 开销。
- 哈希用 `std::hash`，极端碰撞未测试。

### 9.3 实现层面的已知问题

- GDSF 的 `L` 单调增长，长期运行有浮点精度风险；未做归一化。
- TinyLFU 的 Bloom doorkeeper 会饱和（容量固定），饱和后退化为「所有
  key 都进素描」，防污染能力下降；生产环境应按容量扩容。
- ARC 的 pending 表上限 = 容量，极端 ghost 命中下可能挤掉待提升 key；
  影响有限。
- 所有策略的 `Stats()` 在裸实例上非线程安全；并发场景必须用
  `ThreadSafeCache` 或外部锁。

### 9.4 后续方向

1. 补 LIRS/CLOCK-Pro/S3-FIFO，做同一基准的横向对比。
2. 做多线程扩展性基准（分片数 vs 吞吐 vs 命中率）。
3. 引入真实 trace（如维基百科/CDN 公开 trace）做回放。
4. 给 GDSF 增加 `L` 归一化和条目上限的自动推导。
5. 研究「TTL + 淘汰」的组合（本专题未涉及过期语义）。

---

## 10. 参考文献

1. Mattson, R., Gecsei, J., Slutz, D., Traiger, I. *Evaluation Techniques
   for Storage Hierarchies.* IBM Systems Journal, 9(2):78–117, 1970.
2. Karedla, R., Love, J., Wherry, B. *Caching Strategies to Improve Disk
   System Performance.* IEEE Computer, 27(3):38–46, 1994.
3. O'Neil, E., O'Neil, P., Weikum, G. *The LRU-K Page Replacement Algorithm
   for Database Disk Buffering.* Proceedings of SIGMOD 1993, pp. 297–306.
4. Johnson, T., Shasha, D. *2Q: A Low Overhead High Performance Buffer
   Management Replacement Algorithm.* Proceedings of VLDB 1994, pp. 439–450.
5. Megiddo, N., Modha, D. *ARC: A Self-Tuning, Low Overhead Replacement
   Cache.* Proceedings of FAST 2003, pp. 115–130.
6. Jiang, S., Zhang, X. *LIRS: An Efficient Low Inter-reference Recency Set
   Replacement Policy to Improve Buffer Cache Performance.* Proceedings of
   SIGMETRICS 2002, pp. 31–42.
7. Jiang, S., Chen, F., Zhang, X. *CLOCK-Pro: An Effective Improvement of the
   CLOCK Replacement.* Proceedings of USENIX ATC 2005, pp. 323–336.
8. Einziger, G., Friedman, R., Manes, B. *TinyLFU: A Highly Efficient Cache
   Admission Policy.* ACM Transactions on Storage, 13(4), 2017（会议版
   PDP 2014）.
9. Cao, P., Irani, S. *Cost-Aware WWW Proxy Caching Algorithms.* Proceedings
   of USITS 1997, pp. 193–206.
10. Yang, J., Zhang, Y., Qiu, Z., Yue, Y., Vinayak, R. *FIFO Queues Are All
    You Need for Cache Eviction.* Proceedings of SOSP 2023.
11. Belady, L. *A Study of Replacement Algorithms for a Virtual-Storage
    Computer.* IBM Systems Journal, 5(2):78–101, 1966.
12. Cormode, G., Muthukrishnan, S. *An Improved Data Stream Summary: The
    Count-Min Sketch and its Applications.* Journal of Algorithms,
    55(1):58–75, 2005.
13. Caffeine 项目文档：*Efficiency — Window TinyLFU*.
    <https://github.com/ben-manes/caffeine/wiki/Efficiency>
14. Redis 文档：*Key eviction*（LRU/LFU 近似实现）.
    <https://redis.io/docs/latest/develop/reference/eviction/>
