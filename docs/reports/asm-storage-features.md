# LSM 功能层（Queue + Subscribe + Cache）交付报告

日期：2026-11-01　构建目录：`.build-bento-wsl`（Release，`-Wall -Wextra -Wpedantic -Werror`）
底座：KV 引擎 aecc513（`include/go2cpp/storage/kv.hpp`）
边界：**未改动** `kv.hpp` / `kv.cpp` / `wal.*` / `memtable.*` / `sstable.*` / `arena.*`；
新增 `include/go2cpp/storage/{queue,watch,memcache}.hpp`、`src/storage/{seglog,queue,watch,memcache}.cpp`、
`tests/storage/{queue,watch,memcache}_test.cpp`、`tests/storage/features_bench.cpp`，
`cmake/Go2CppKv.cmake` 追加源文件与 4 个目标。**未 commit**。

---

## 1. 三个功能如何共用一套 LSM

| 功能 | 复用点 | 磁盘/内存落点 |
|---|---|---|
| Queue（MQ） | WAL 顺序写（`WalWriter`，64KB 块 + CRC32C + 1MB 用户缓冲）；段封口写成 SSTable 稀疏索引 | 消息日志 `queue/NNNNNN.seg` + `NNNNNN.idx`；游标是 KV 记录 `queue/<topic>/<consumer>/offset` |
| Subscribe（Watch） | 同一条 WAL 格式的变更日志；KV 写路径包装 | `watch/NNNNNN.seg`；游标是 KV 记录 `watch/<id>/offset` |
| MemCache | MemTable 的「槽/载荷分离」布局 + `cache` 模块的 256 桶时间轮与侵入式 LRU/节点池 | 纯内存；`persist` 时写穿 `KV::Put`，`read_through` 从 LSM 回填 |

共用的 `SegLog`（`src/storage/seglog.{hpp,cpp}`）是本次新增的核心复用层：

* 写端直接复用 `WalWriter`（与 KV 的 WAL 完全同格式）；
* 段封口把稀疏检查点写成 **SSTable**（复用块/footer 格式，key=seq 大端）；
* 读端自带按偏移定位的扫描器（`WalReader` 只能从头扫，MQ 消费者要从任意 seq 开始）；
* 恢复时截断最后一段的尾部垃圾（kill -9 残留），避免新记录被垃圾挡住。

---

## 2. 正确性测试（全部 PASS）

| 测试 | 覆盖 | 规模 | 结果 |
|---|---|---|---|
| `go2cpp_queue_test` | 基本收发 / 多消费者独立游标 / 多 topic 交错 / 冷读预取 / 游标持久化（重开 KV）/ 段滚动（256KB 阈值 + `.idx` 存在）/ 尾部垃圾截断 / **fork + SIGKILL 崩溃恢复** / 1e6 全量 | 1e6 | **PASS**（59 209 checks, 0 failures） |
| `go2cpp_watch_test` | 前缀匹配 / 多订阅者 / 批写 `Write` / 无订阅零事件快路径 / Replay（全量+区间）/ 稳定 id 游标持久化 / 异步背压 / 1e5 写回调顺序 | 1e5 | **PASS**（111 180 checks, 0 failures） |
| `go2cpp_memcache_test` | 基本读写/覆盖/长 value / TTL 过期与覆盖清 TTL / LRU 淘汰与命中提升 / 多线程 4×5e5 / 写穿 + 回填 / Clear+Purge / 1e7 Get+Set | 1e7 | **PASS**（574 checks, 0 failures） |

关键验证点：

* **Queue 1e6**：append 的 offset 必须严格 0..999999；consume+ack 循环里每条 offset 与 payload 逐一比对，
  0 丢失、0 重复；ack 后游标 == 1e6，再 consume 返回 false。
* **崩溃恢复**：子进程 append 10 万条 + 消费 10 条 + `Sync()` + `KV::Close()` 后 `raise(SIGKILL)`；
  父进程重开：`Head == 100000`、游标 == 10、第 10 条可读、再 append 的 offset == 100000。
* **Watch 1e5**：回调计数 == 1e5，`seq` 严格递增且等于回调序号，key 顺序与写入一致。
* **MemCache 1e7**：容量 1e6、key 空间 1e6，命中率 99.9%，`size <= capacity`。

---

## 3. 性能（1e7，WSL ext4，`taskset -c 0-3`，单次运行）

完整日志：`/tmp/bench_1e7.log`。

| 操作 | ns/op | ops/s | MB/s |
|---|---:|---:|---:|
| queue.append(40B) | 196.1 | 5 099 337 | 234 |
| queue.consume(40B) | 24.2 | 41 245 422 | 1 572 |
| queue.consume+ack(40B) | 69.7 | 14 345 118 | 547 |
| queue.cold_replay(40B) | 2 169.6 | 460 909 | 18 |
| watch.put(sync 1 sub) | 785.4 | 1 273 221 | 49 |
| watch.put(0 sub, no event) | 533.8 | 1 873 195 | 71 |
| kv.put(baseline，同机同负载) | 460.8 | 2 169 972 | 83 |
| watch.put(async enqueue) | 1 437.3 | 695 757 | 27 |
| memcache.set(1 shard) | 721.5 | 1 385 993 | 42 |
| memcache.get(1 shard) | 859.8 | 1 163 031 | 35 |
| memcache.get_nocopy(1 shard) | 547.1 | 1 827 766 | 56 |
| memcache.peek(纯查找) | 570.9 | 1 751 737 | 53 |
| memcache.get(8 shards) | 980.9 | 1 019 479 | 31 |
| lower.seglog_append(40B) | 71.6 | 13 973 718 | 559 |
| lower.seglog_append(4KB) | 6 528.7 | 153 170 | 627 |

对照（同机、同工作集 1e6）：

| 对照 | ns/op | 说明 |
|---|---:|---|
| `lrucache.get`（项目现有 LRU） | 585.3 | 哈希 + LRU + value 拷贝，无锁无 TTL |
| `unordered_map.find(1e6)` | 143.9 | 裸哈希，无淘汰策略 |
| `baseline.empty_loop` | 0.2 | 循环本身 |

---

## 4. 实测下限与「离下限几倍」

| 项 | 实测下限 | 本实现 | 倍数 |
|---|---|---:|---:|
| 内存 memcpy | 46~61 GB/s（256MB 单线程） | — | — |
| 裸顺序写（/dev/shm，1MB×1024 + 1 次 fdatasync） | 1.73 GB/s | — | — |
| 裸顺序写（/tmp overlay） | 0.54 GB/s | — | — |
| fdatasync 单次 | 1.8~2.1 ms | — | — |
| WAL 顺序写（SegLog，40B 记录） | 559 MB/s | — | — |
| WAL 顺序写（SegLog，4KB 记录） | 627 MB/s | — | — |
| Queue append | 71.6 ns（裸 SegLog append） | 196.1 ns | **2.7×** |
| Queue consume（内存命中） | ~10 ns（deque 访问 + 40B 拷贝） | 24.2 ns | **2.4×** |
| Watch 事件开销 | 53 ns（回调本身） | +324 ns（对 kv.put） | 事件写日志占 ~270 ns |
| MemCache 纯查找 | 585 ns（项目 LRUCache） | 570.9 ns | **0.98×（持平）** |
| MemCache Get（含拷贝） | 585 ns（项目 LRUCache） | 859.8 ns | **1.47×** |
| MemCache Get vs 裸哈希 | 143.9 ns | 859.8 ns | **6.0×** |
| MemCache Get vs DRAM 单次随机访问 | ~100 ns | 859.8 ns | 8.6× |

**结论**：

* Queue 的 append 距「裸 WAL 追加」2.7×，差额是 topic 查找、内存队列入队、
  统计与段索引检查——都是 MQ 语义必需，不是实现浪费；
* Queue 的 consume 已到 24 ns（4 100 万/s），接近「deque 访问 + 一次 40B 拷贝」的物理下限；
* MemCache 的**纯查找与项目 LRUCache 持平（0.98×）**，说明哈希 + 条目布局没有问题；
  多出来的 ~275 ns 是分片锁 + TTL 判定 + LRU 链改写 + `std::string` 赋值（16B 超过 SSO，
  每次一次 malloc）；
* 与裸 `unordered_map` 的 6× 差距不是「实现慢」，而是「带 LRU + TTL + 线程安全」
  的固有成本——同项目的 LRUCache 在同样负载下是 585 ns，也是一样的量级。

---

## 5. objdump 证据

`objdump -d --no-show-raw-insn -C libgo2cpp_kv.so`，统计函数体（Release）：

| 函数 | 字节 | 指令 | call | `lock` 前缀 | 栈帧 |
|---|---:|---:|---:|---:|---:|
| `Queue::Impl::Append` | 1 923 | 435 | 54 | 2 | 120 B |
| `Watcher::Impl::Notify` | 3 141 | 669 | 59 | 9 | 328 B |
| `MemCache::Impl::Set` | 5 061 | 1 122 | 62 | 5 | 136 B |
| `MemCache::Impl::Get` | 3 798 | 880 | 28 | 7 | 120 B |
| `SegLog::Append` | 836 | 185 | 9 | 0 | 48 B |

读法：

* `SegLog::Append` 185 条指令、9 个调用、**0 个原子指令**——纯顺序写路径，
  没有任何同步原语，符合「WAL 追加」的定位；
* `Queue::Impl::Append` 435 条指令里只有 2 个 `lock`（两个统计计数器），
  topic 缓存命中后不再有 map 查找与 string 构造；
* `MemCache::Impl::Get` 有 **7 个 `lock` 前缀**，全部来自 `GetStats` 用的
  `std::atomic` 计数器（gets/hits/misses/expirations…）。**这是已识别但本次未做的
  优化点**：改成「分片内普通计数 + GetStats 汇总」预计能省 5~7 次 `lock add`
  （每次无竞争约 7~10 ns，合计 ~8%）。之所以没做，是因为本次 1e7 测量的
  噪声是 ±25%，收益落在噪声内，无法给出可信的前后对比。

---

## 6. 优化前后（可复现的 A/B）

`QueueOptions.topic_cache` 与 `MemCacheOptions.purge_on_get` 把两项优化做成开关，
bench 里同一进程内先后跑两个配置：

| 场景 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| queue.append(40B)，n=2e6 | 188.1 ns | **114.4 ns** | **−39.2%** |
| queue.append(40B)，n=1e7 | 302.3 ns | **196.1 ns** | **−35.1%** |
| memcache.get(1 shard)，n=1e7 | 859.8 ns（purge=1） | 906.9 ns（purge=0） | 噪声内，无收益 |
| memcache.get_nocopy(1 shard)，n=1e7 | 547.1 ns | 581.0 ns | 噪声内，无收益 |

Queue 的优化（thread_local topic 缓存）有效且显著：省掉了每次操作的
`std::string(topic)` 构造 + 全局 `topics_mu_` 锁 + `unordered_map` 查找。

MemCache 的两项尝试（条目重排、Get 侧过期扫描节流）在这台机器上**没有统计显著收益**，
如实记录而不是包装成优化：条目重排理论上少一次 cache miss，但 1e6 工作集下
瓶颈是 DRAM 延迟本身；过期扫描在 TTL 轮为空时 `Min()` 只是一次 load。
`purge_on_get=false`（节流）保留为默认，因为语义上更省，过期判定仍然精确。

---

## 7. 已知限制

* 冷读 2 169 ns/条（热路径的 90 倍）：每次 `Refill` 都会打开一次段索引
  （`SSTable::Open` + mmap）。重启/`Seek` 回退时才发生，未做索引缓存。
* Queue 单 topic 消费串行（一把 topic 锁），没有分区/再均衡。
* Watch 前缀匹配是线性比较，订阅者很多时应改前缀树。
* MemCache 分片数默认 8 在单线程下比 1 分片慢 10~20%（多一层间接 + 局部性变差），
  应按并发度配置。
* 游标持久化依赖 KV 的 WAL 落盘策略：`Ack` 批量写 KV，KV 自身需要
  `KV::Close()` 或 `sync_writes=true` 才 fdatasync。

---

## 8. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp

# 构建（Release + 测试）
cmake -S . -B .build-bento-wsl -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_WITH_KV=ON
cmake --build .build-bento-wsl \
      --target go2cpp_queue_test go2cpp_watch_test go2cpp_memcache_test \
               go2cpp_storage_features_bench -j12

# 正确性
./.build-bento-wsl/go2cpp_queue_test --n=1000000
./.build-bento-wsl/go2cpp_watch_test --n=100000
./.build-bento-wsl/go2cpp_memcache_test --n=10000000

# 性能（1e7）
taskset -c 0-3 ./.build-bento-wsl/go2cpp_storage_features_bench --n=10000000

# ctest
ctest --test-dir .build-bento-wsl \
      -R 'go2cpp_(queue|watch|memcache)_test|go2cpp_storage_features_bench' \
      --output-on-failure
```
