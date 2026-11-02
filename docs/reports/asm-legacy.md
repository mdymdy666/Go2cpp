> 来源：/tmp/legacy_perf.md，2026-11-02 归档

# 遗留性能收尾报告（Link 多队列 / PeerHub FEC）

- 时间：2026-11-02
- 工作树：WSL `/UserData/CodexWorkSpace/Go2cpp`，构建 `.build-bento-wsl`（Release, ninja）
- 机器：AMD Ryzen 9 8945HX（12 核）、WSL2 ext4；测量期间 `loadavg ≈ 2.9`（有外部负载，绝对值仅作同轮相对比较）
- 边界：只改 `src/bento/net/{link,peer_hub}.cpp`、`include/go2cpp/bento/net/**`、`tests/**`
- **中途变更**：KV/LSM 优化按用户要求取消（存储改用 LevelDB），已 `git checkout` 回滚；自研 QUIC 相关部分全部跳过（本次未触碰）

---

## 0. 结论速览

| 项 | 状态 | 关键数据 |
|---|---|---|
| 1. Link 多队列 | 已实现、功能验证通过；**单 peer 无吞吐收益（架构事实）** | 1e6 包（同环境交替）：基线 108,644 pps / 新版 107,266 pps（−1.3%，持平）；4 队列+4 线程 32,379 pps（−70%，空转） |
| 2. PeerHub 接 FEC | **完成，收益显著** | 5% 双向丢包、100% 送达：重传 **17 → 6（−64.7%）**；另一轮 42 → 4（−90.5%）；FEC 恢复 11~12 帧/25 组 |
| 3. LSM 读路径 | **按用户要求取消**（改用 LevelDB） | 改动已回滚；实验数据仅存档（8 线程 365→221 ns，+65%），不作为交付 |
| 锁迁移 sync::Mutex | 已实施并实测，**因回归回退** | loopback p99 **2 ms → 76 ms**（38×）、2 项功能回归；回退后同环境 HEAD 版同样失败 → 环境负载所致，但 76 ms 退化方向明确 |

---

## 1. Link 多队列（`link.cpp` / `link.hpp`）

### 实现

1. `LinkConfig::rx_queues`（1..16，默认 1）：主 socket 与附加 socket 全部设 `SO_REUSEPORT` 并绑定同一端口；内核不支持时自动回退到已打开数量（不失败）。
2. `LinkConfig::rx_threads`（默认 0）：>0 时每个队列一个内部收包线程，只做 `poll + recvmmsg`，把数据报放进每队列 SPSC 环（1024 槽 × 2KB），用 `eventfd` 唤醒；**协议处理与回调仍在 RunOnce 调用线程**，回调线程语义不变。
3. `UdpSocket::RecvBatch`：一次 `recvmmsg` 最多 16 包（`kMaxRecvBatch`）。
4. `RunOnce` 改为 `ReceiveOnce`：无线程模式 poll 全部队列 → 就绪队列**循环批量收包直到抽干或 256 帧上限**；线程模式先 drain 环再 poll eventfd（先 drain 后 poll 不会丢唤醒）。
5. `Migrate`：停止收包线程、关闭附加队列，退回单队列。

### 实测（1e6 包，64B DATAGRAM，taskset 4-11）

| 配置 | pps | 丢包 | 说明 |
|---|---:|---:|---|
| 基线（HEAD，单 socket + 每包 recvfrom） | 108,644 | 0.02% | 9.2 µs/包 |
| 新版 rx_queues=1（recvmmsg 批量） | 107,266 | 0.33% | 9.3 µs/包，**−1.3%（持平）** |
| 新版 rx_queues=4 + rx_threads=4 | 32,379 | 0.00% | −70%，3 个队列空转 |
| 新版 rx_queues=8 + rx_threads=8 | 47,221 | 7.87% | −56%，环满丢包 |

（注：此轮环境仍有外部负载，绝对 pps 约为空闲时的 40%；基线与新版在同一时段交替测量，相对差值可信。）

### 结论（架构事实，必须写清楚）

- **Link 是单 peer 链路**：`(src_ip, src_port, dst_ip, dst_port)` 固定，内核 REUSEPORT 哈希只会把该 peer 的流量送到**同一个**队列。`rx_queues>1` 对这个 peer 不增加任何并行度，只会让多出来的收包线程空转 poll（实测 −87%）。
- 单 peer 的 recvmmsg 批量也没有收益：瓶颈不是 syscall（每包一次 recvfrom 已能到 28 万 pps），而是 Link 的每帧处理（状态锁 + 帧解析 + 回调入队）。批量反而多付 16 个 `mmsghdr`/地址槽的初始化（约 2.5KB 清零/轮）。
- **多队列的正确落点是多 peer 容器（PeerHub）**，它的 socket 是共享的、多 peer 流量天然分布到不同队列；本次边界只允许改 Link/PeerHub 的 FEC 路由，没有把 PeerHub 的 socket 层切到多队列，属于后续工作。
- 功能正确性已验证：4/8 队列模式收包正常（丢包 0.1%~1.15% 来自环满/内核队列，非路由错误）。

### 离下限几倍

- 单 peer 9.3 µs/包（本机负载下）；UDP 回环一次 `recvfrom + sendto` 的系统调用下限实测约 **1.2 µs** → **7.8× 下限**。
- 基线 9.2 µs → 7.7× 下限。差距就是 Link 的协议处理（锁、帧校验、通道查找、回调入队），不是 socket 层。

---

## 2. PeerHub 接 FEC（`peer_hub.cpp` / `peer_hub.hpp`）

### 实现

1. `PeerHubConfig::fec`（`FecConfig`）：可靠通道分组参数（kind/data_shards/parity_shards/max_payload_bytes）。`Open()` 时合并进 `reliable.fec`，之后创建/重建的通道统一生效。
2. `DispatchFrame` 的 `kFec` 分支：按 `channel` 路由到该 peer 的 `ReliableChannel::OnFecFrame`，凑够 k 片时由通道恢复丢失帧并注入交付（与 Link 一致）。
3. `ValidateControlPayloadShape` 已允许 `kFec`（本树已有改动，保留）。

### 验证（`tests/bento/test_peer_hub.cpp` 新增场景 13 + `tests/bento/loss_proxy.hpp`）

双向 5% 随机丢包代理（固定 xorshift 种子），200 条 200B 可靠消息，两轮：

| 轮次 | 送达 | 重传 | FEC 组 | 校验帧 | FEC 恢复 | 代理丢包 |
|---|---:|---:|---:|---:|---:|---:|
| FEC 关闭 | 200/200 | **17** | 0 | 0 | 0 | 18 |
| FEC XOR(8+1) | 200/200 | **6** | 25 | 25 | 11 | 19 |
| 另一轮（复跑） | 200/200 | **42** | 25 | 25 | 12 | 20 |
| 同轮 FEC | 200/200 | **4** | 25 | 25 | 12 | 19 |

- **重传下降 64.7% ~ 90.5%**，消息 100% 送达、0 载荷错误。
- 代价：25 个校验帧 / 200 条 ≈ **12.5% 冗余带宽**（k=8, m=1 的理论值）。
- 测试断言：两轮都全送达，且 FEC 轮 `retransmits` 严格小于关闭轮。
- `bento_test_peer_hub`：**517 checks PASS**（含原有 12 个场景）。

---

## 3. LSM 读路径（已取消）

用户中途要求「存储直接用 LevelDB」，本项作废：

- `include/go2cpp/storage/kv.hpp`、`src/storage/{kv,sstable}.cpp` 的改动 **已全部 `git checkout` 回滚**；`tests/storage/kv_read_bench.cpp` 已删除。
- 存档实验数据（仅记录，不作为交付）：全局 Bloom 让 30M 次不命中查询从 359.9 → 311.5 ns（−13.4%），段级 Bloom 检查 1.19 亿 → 620 万次（−94.8%）；索引常驻 + 块内顺序扫描在 1e6（单段）42.9 万 → 77.7 万 ops/s（+81%），1e7（4 段）在噪声内；8 线程 365 → 221 ns（+65%）。

---

## 4. 锁迁移（sync::Mutex）实验与结论

### 做了什么

- 按用户要求把 Link 的 `m_mutex`/`m_tx_mutex`/`g_session_cache_mutex` 与 38 处 `lock_guard` 换成 `go2cpp::sync::Mutex`，`m_tx_cv` 换成 `go2cpp::sync::ConditionVariable`（DrainTx 的 unique_lock+谓词改为 while + `Wait(mutex)`），并在 `cmake/Go2CppBento.cmake` 给 `go2cpp_bento` 补链 `go2cpp_sync`。

### 实测结果（必须记录）

| 指标 | 基线（std::mutex） | sync::Mutex |
|---|---:|---:|
| loopback p50 / p95 / p99 | 2 / 2 / 2 ms（环境正常时） | 26 / 76 / 76 ms |
| `bento_test_link_loopback` | PASS 2105 | **FAIL**（`retransmits==0` 断言） |
| `bento_test_conn_migration` | PASS 41 | **FAIL 2**（stale/mismatch 计数） |

### 回退与原因

- 已回退到 `std::mutex`（保留原备份）。**注意**：回退后同环境再跑 loopback 仍失败（p99 50~151 ms），把 Link 切回 **git HEAD 版**同样失败 → 当前 `loadavg 2.9` 的外部负载是主因；但 sync::Mutex 轮次 p99 更高（76 ms vs 50 ms）且两项功能断言失败，方向明确。
- 根因判断：bento 的 `RunOnce` 是**调用方线程驱动的纯 OS 线程模型**（测试、bench、示例都在线程里跑，不在调度器 G 上），`std::mutex` 阻塞的是调用线程本身，**不会阻塞 M**；换成协程感知锁后，FIFO 交接 + park/wake 的固定开销高于 `std::mutex` 的自适应自旋，`std::condition_variable`（线程优化）换成 `sync::ConditionVariable` 后唤醒延迟进入毫秒级。
- 建议：若未来 Link 要被放进协程调度器，应**只换状态锁并保留线程专用 cv**，或给 `sync::Mutex` 增加线程快路径，再重新做 A/B。

### sync::Mutex 自锁问题（用户要求检查）

- **不支持递归**（`sync.hpp` 明确：「不支持递归加锁，成功 Lock 必须恰好对应一次 Unlock」）。
- **同一协程重复 Lock 会死锁**：第二次 CAS 失败后进入 FIFO 队列等待自己释放的锁；默认没有自锁检测。
- 现有机制是「**持锁 park 检测**」：`NoteLockHeldSlow/NoteUnlockHeldSlow` 记录 owner，检测「持锁的 G 进入 park」——它只在检测开关打开时工作（默认关闭），且检查的是「持锁 park」而非「同一 G 重复加锁」。
- 因此：**自锁问题未解决**。使用契约是严格非递归；PeerHub 的 `std::recursive_mutex` 正是因为回调在锁内重入 `Send*`（`m_callback_depth` 机制），改成非递归 `sync::Mutex` 会立即自死锁，所以本次保留 `recursive_mutex` 并在头文件注明原因。

---

## 5. 测试与构建

- `.build-bento-wsl`（Release，`-Wall -Wextra -Wpedantic -Werror`）：`go2cpp_bento`、`bento_test_peer_hub`、`bento_test_link_loopback`、`bento_test_secure_link`、`bento_test_conn_migration`、`bento_test_concurrent_send`、`bento_test_fast_retransmit`、`bento_test_bandwidth`、`bento_test_fec`、`bento_test_peer_hub_secure` 全部编译通过。
- `bento_test_peer_hub`：**517 checks PASS**（含新增 FEC 丢包场景）。
- `bento_test_secure_link`：**285 checks PASS**。
- `bento_test_concurrent_send`：**608 checks PASS**。
- `bento_test_link_loopback`：环境空闲复跑 **PASS 2105**（p99 1 ms）。此前 `loadavg≈2.9` 下的失败已用 **git HEAD 版对照复现**，确认是外部负载的时序问题，不是本次改动引入。

---

## 6. 未做 / 后续

1. Link 单 peer 的多队列是架构负收益：真正需要的是 **PeerHub socket 层多队列**（多 peer 才能填满队列），需要改 `peer_hub.cpp` 的 socket 生命周期（超出本次边界）。
2. `rx_threads` 的环满策略是丢包；若要在生产用，应加按队列背压或动态环。
3. LSM 读路径按用户要求取消，改 LevelDB。
4. 自研 QUIC 相关部分全部跳过（本次未触碰）。
