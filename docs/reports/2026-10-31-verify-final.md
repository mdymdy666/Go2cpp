# 最终验证报告（正确性）

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 ext4，干净，未 commit）
- 构建：`.build-bento-wsl`（既有 Release 构建，本次只新增 `tests/concurrency/stress_final.cpp` 并独立编译）
- 日期：2026-10-31
- 环境：AMD Ryzen 9 8945HX（12 vCPU / SMT），19 GiB，内核 6.6.87.2-microsoft-standard-WSL2，g++ 13.3.0
- **重要**：本机为多 agent 共享，压测期间 loadavg 从 **2.9 飙到 555**（其他 agent 同时在跑压测/基准），
  因此**吞吐数字只用于横向定位，不作为性能结论**；性能基准由独立 agent 在低负载下测。
- 职责边界：本 agent **只验证、不修复**（除新增压测程序本身）。

---

## 0. 结论速览

| 项 | 结果 |
|---|---|
| 全量 ctest | **85 项：83 通过 / 1 SKIP（环境）/ 1 FAIL（既有文档滞后）** |
| `stress_final`（21 类混合，10^9 × 3 轮） | **3 轮全部通过**，总 3×10^9 操作，827s，RSS 6.2→6.8 MiB |
| 7 模型巨量（`stress_mixed_billion`） | f2f 10/10、t2t 10/10、mutex 10/10、cv 10/10、wg 10/10、f2t 3/10（预算裁剪）、t2f 7/10（watchdog 终止） |
| 失败/异常 | 2 起：① `stress_final` 首跑在第 1 轮 90% 处 180s 无进展（未复现）；② `t2f` 第 8 轮 120s 无进展（loadavg>200，疑似饥饿） |
| 代码缺陷 | 未发现数据一致性缺陷（顺序/计数/互斥/取消/关闭断言全绿）；仅性能问题（单锁争用）与上述两起挂起 |

---

## 1. 全量 ctest

命令（串行，`.build-bento-wsl`）：

```bash
cd /UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl
ctest --output-on-failure
```

结果：**85 项 / 83 Passed / 1 Skipped / 1 Failed**，总耗时 60.07s。

| 分类 | 测试 | 判定 | 依据 |
|---|---|---|---|
| **环境** | `bento_netem`（SKIP） | 非失败 | 需要 `CAP_NET_ADMIN`/`sch_netem`，当前容器无权限，测试自身跳过 |
| **既有（文档滞后）** | `docs_consistency`（FAIL） | 非代码缺陷 | 断言：`docs/testing.md` 登记 ctest=81，实际=85；测试地图缺 `bento_bench_protocols`、`bento_omni_server_m1`、`go2cpp_bufio_batch_test`、`reflect_codec`。工作树在我操作前即 `git status` 干净，故属既有问题 |
| **真失败（新引入）** | 无 | — | 其余 83 项全部通过 |

> 任务背景提到的「spin_mutex 3 + debug_dump 2 既有失败」在当前 commit 上**已不存在**
> （对应测试或修复已合入）；当前唯一 FAIL 是文档一致性。

完整日志：`/tmp/ctest_full.log`。

---

## 2. 最终巨量测试

### 2.1 `stress_final`：21 类结构混合（10^9 × 3 轮）

程序：`tests/concurrency/stress_final.cpp`（新增，独立编译，不改 CMake、不注册 ctest）。

拓扑：**8 个原生线程 + 8 个受管 Fiber**（Scheduler P=4），`taskset -c 2,3,4,5`；
每 worker 按加权随机执行：Channel(cap 0/1/64/1024)、FastChannel(SPSC/MPMC)、Mutex、
ConditionVariable、WaitGroup、RWMutex、SpinMutex、Select（recv/send/default/after/取消）、
Context（WithCancel/WithTimeout/中途取消）、Error（Fiber-local 跨 M）、socketpair 收发、
锁嵌套 / channel-of-channel / select 嵌套、关闭竞态。

| 轮 | 目标操作 | 完成 | 耗时 | 吞吐 | loadavg | RSS | 断言 |
|---|---:|---:|---:|---:|---:|---:|---|
| 1 | 10^9 | 10^9 | 272.00 s | 3.68 M ops/s | 5.96 | 6.2 MiB | **28/28 通过** |
| 2 | 10^9 | 10^9 | 267.67 s | 3.74 M ops/s | 5.71 | 6.5 MiB | **29/29 通过** |
| 3 | 10^9 | 10^9 | 287.80 s | 3.47 M ops/s | 8.27 | 6.8 MiB | **29/29 通过** |
| 合计 | 3×10^9 | 3×10^9 | 827.48 s | 3.63 M ops/s | 5.7–8.3 | 基线 4.5 → 6.8 | 全部通过 |

断言覆盖（每轮真实计数，非恒真）：
1. 操作总数 = 目标，21 类计数和 = 目标，21 类全部被真实执行；
2. 共享 Channel cap64/cap1024/cap0「无丢失、无重复」（全局唯一序号 + 计数 + 求和；
   丢一个少 1、重一个多 1、一丢一重必改和）；
3. 私有 Channel（cap 1/64/1024）与 SPSC 环严格 FIFO（自检过的严格递增验证器）；
4. FastChannel MPMC 无丢失/无重复；
5. Mutex / SpinMutex 临界区成对序号相等（互斥未被破坏）、结束未持有；
6. RWMutex 读者 8 槽全等（无撕裂）；
7. CV 生产 = 消费 + 残留、私有队列收发配对、waiters=0；
8. WaitGroup 计数归零、waiters=0、`WaitFor(0)` 未归零必须 false、归零必须 true；
9. Select 每次 index 合法、分支和守恒、五路（recv/send/default/after/取消）全覆盖、兜底超时 ≤0.1%；
10. Context 取消后 IsDone/Err 一致、子 Context 同步取消、创建 = 取消；
11. Error（Fiber-local）跨 M 迁移后指针/错误码/槽值三重一致；
12. socketpair 回环内容一致；
13. 嵌套（锁/channel/select）无违例；
14. 关闭竞态：Close 幂等、关闭后 Send 被拒、Recv 排空后才 Closed、Len=0、waiters=0；
15. 所有共享 Channel waiters=0、队列排空；
16. RSS 相对基线 ≤512 MiB、轮间 ≤128 MiB（实测 +2.3 MiB）；
17. 所有参与者正常结束（无死锁/无异常）。

### 2.2 7 模型巨量（`stress_mixed_billion`，按实测 ns/op 降级）

**降级原因（如实说明）**：跨线程模型每消息一次 futex park/wake，实测
f2t 为 **75–100 µs/op**、t2f 为 **20–44 µs/op**、wg 为 **65–83 µs/op**
（不是 20 µs 的理论值；负载放大 3–5 倍）。按 10^8 × 10 轮根本跑不完，
故按「每模型 ≤6 分钟」降级；同线程模型保持 10^7 × 10 轮。

| 模型 | 规模 | 轮次 | ns/op（min–max） | 总操作 | 断言 | 结果 |
|---|---|---|---:|---:|---|---|
| f2f | 10^7/轮 | 10/10 | 54.4 – 61.2 | 10^8 | 发送=接收=10^7、FIFO 0 违例 | **全通过** |
| t2t | 10^7/轮 | 10/10 | 195.2 – 236.6 | 10^8 | 同上 | **全通过** |
| mutex | 10^7/轮 | 10/10 | 172.2 – 3306.8 | 10^8 | 锁内计数精确、无交错、waiters=0 | **全通过** |
| cv | 10^6/轮 | 10/10 | 174.3 – 737.1 | 10^7 | 生产=消费、FIFO、队列排空、waiters=0 | **全通过** |
| f2t | 10^6/轮 | **3/10**（预算裁剪） | 74 846 – 100 075 | 3×10^6 | 同上 | **全通过** |
| t2f | 10^6/轮 | **7/10**（第 8 轮 watchdog） | 19 667 – 43 497 | 7×10^6 | 同上（前 7 轮） | **前 7 轮通过，第 8 轮挂起** |
| wg | 10^5/轮 | 10/10 | 65 283 – 82 510 | 999 680 | 计数归零、waiters=0 | **全通过** |

合计约 **2.28×10^8** 操作、60 轮，除 t2f 第 8 轮外全部通过。

完整日志：`/tmp/mixed_7models.log`，分报告 `/tmp/mixed_7models/{batch1,batch2,batch3,cv}.md`。

---

## 3. 失败与异常记录（如实，未修复）

### 3.1 `stress_final` 首跑：第 1 轮 90% 处 180s 无进展（**未复现**）

- **场景**：`--ops 1000000000 --rounds 3`，8 线程 + 8 Fiber，P=4，`taskset -c 2,3,4,5`；
- **现象**：watchdog（180s 无进展）在第 1 轮全局进度 `900 324 032 / 10^9` 处触发并 `_Exit(3)`；
- **现场**（watchdog 打印）：
  - worker 0（thread）：`ops=14 413 056`，快照类别 `mutex`；
  - worker 8（fiber）：`ops=10 906 880`，快照类别 `fast-mpmc`；
  - 其余 14 个 worker 均已跑满 `62 500 000`（quota）；
  - 两者同属 **shard 0**（`id % 8 == 0`），即共享 `mutex[0]` / `spin[0]` / `rw[0]`。
- **后续**：同一二进制、同一参数**第二次、第三次均正常完成**（1e9 单轮 + 3×1e9 三轮全通过），
  因此**未复现**，第一版日志已被后续运行覆盖（现场以本条记录为准）。
- **初步判断**：概率性挂起，两个 worker 同时停在同一分片的同步原语上；
  需在低负载下用 `--hang-sec 60` 多轮复跑 + 打开 `GO2CPP_DETECT_HELD_PARK=1` 才能定位到具体原语。
- **复现命令**：
  ```bash
  cd /UserData/CodexWorkSpace/Go2cpp
  LD_LIBRARY_PATH=$PWD/.build-bento-wsl taskset -c 2,3,4,5 \
      .build-bento-wsl/stress_final --ops 1000000000 --rounds 3 --hang-sec 180
  ```

### 3.2 `t2f` 模型第 8 轮：120s 无进展（loadavg > 200，疑似饥饿而非死锁）

- **现象**：`[致命] 看门狗检测到超过 120s 无进展（疑似死锁）；当前模型=t2f 轮次=8 已发送=55819 已接收=55754`；
- **现场**：差值 65 = cap 64 缓冲 + 1 个在途，符合「发送方停住、接收方等最后一个」的形态；
- **环境**：同一时刻 `loadavg = 206.56`（1 分钟），59 秒后升到 527；
- **判断**：**无法区分真死锁与极端负载饥饿**——进程被 `taskset` 绑在 4 核，
  但系统有 600+ runnable 进程，单个线程被饿死 120s 完全可能；
- **复现命令**（建议低负载下跑，`--hang-sec 60`）：
  ```bash
  cd /UserData/CodexWorkSpace/Go2cpp
  LD_LIBRARY_PATH=$PWD/.build-bento-wsl taskset -c 2,3,4,5 \
      .build-bento-wsl/stress_mixed_billion --ops 1000000 --rounds 10 --models t2f --hang-sec 60
  ```

### 3.3 性能问题（非正确性，供性能 agent 参考）

- **单锁极度争用**：16 参与者 / 4 核共享**一把** `sync::Mutex` 时实测 **294 µs/op**（500k 操作 147 s），
  比无争用（~25 ns）慢 4 个数量级；同一程序把锁按 8 分片后回到 **24.5 M ops/s（~40 ns/op）**。
  说明争用时大量 park/wake（每次唤醒 ~100 µs 量级）。
- **RWMutex 写 + CV 共享队列 / 双锁嵌套**同样受此影响（分片前分别 22.8 µs、7.7 µs、2.8 µs/op）。
- **Channel cap0 带 20 µs 超时**：5.6 µs/op（一半超时），属预期。

---

## 4. `stress_final` 程序结构（供分结构 agent 复用）

```
Config/参数      --ops/--rounds/--threads/--fibers/--sched-p/--hang-sec/--only/--quick
Participant      WorkerCtx {id, is_fiber, shard, quota, rand_state, 私有 FIFO×3, SPSC, 私有 CV, 本地 WG, socketpair, ErrorLocal 槽}
启动             StartGate（所有参与者就位后同时放行，计时不含启动偏斜）
调度             n 线程 std::thread + m Fiber Scheduler::spawn；quota 均分，余数给前若干 worker
主循环           每步 splitmix64 → PickKind(权重表) → Dispatch(kind) → kind_count[kind]++；
                 每 256 步 g_progress += 256（watchdog 进度源）
真实断言         见 §2.1 的 17 条；共享通道用「唯一序号 + count + sum」，私有通道用严格递增验证器
校验             轮末排空共享通道并计入接收侧统计 → 逐条断言 → 记 RSS/loadavg
watchdog         独立线程 1s 采样；hang_sec 无进展 → 打印每 worker（thread/fiber, ops, 当前类别）
                 + 每 shard 的 Mutex state_bits(locked/waiter/starving/woken)/waiters/native/fiber
                 + CV waiters + WG count/waiters + 各 Channel waiters/Len + FastChannel Len → _Exit(3)
```

**复用建议**：把 `Shared` 换成单结构对象、`Dispatch` 换成单场景操作，即可得到
「单结构 × 单场景」的压测；断言与 watchdog 可直接沿用。

---

## 5. 复现命令汇总

```bash
cd /UserData/CodexWorkSpace/Go2cpp
export LD_LIBRARY_PATH=$PWD/.build-bento-wsl

# ctest
(cd .build-bento-wsl && ctest --output-on-failure)

# 重新编译 stress_final（独立程序，不改 CMake）
g++ -std=c++17 -O3 -DNDEBUG -pthread -Iinclude tests/concurrency/stress_final.cpp \
    -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
    -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber \
    -lgo2cpp_context -lgo2cpp_error -lgo2cpp_log -o .build-bento-wsl/stress_final

# 21 类混合巨量（10^9 × 3 轮）
taskset -c 2,3,4,5 .build-bento-wsl/stress_final --ops 1000000000 --rounds 3 --hang-sec 180

# 单类定位（每类独立跑）
taskset -c 2,3,4,5 .build-bento-wsl/stress_final --ops 500000 --rounds 1 --only mutex

# 7 模型（降级规模）
g++ -std=c++17 -O3 -DNDEBUG -pthread -Iinclude tests/concurrency/stress_mixed_billion.cpp \
    -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
    -lgo2cpp_channel -lgo2cpp_sync -lgo2cpp_scheduler -lgo2cpp_fiber \
    -lgo2cpp_context -lgo2cpp_error -lgo2cpp_log -o .build-bento-wsl/stress_mixed_billion
taskset -c 2,3,4,5 .build-bento-wsl/stress_mixed_billion --ops 10000000 --rounds 10 --models f2f,t2t,mutex
taskset -c 2,3,4,5 .build-bento-wsl/stress_mixed_billion --ops 1000000 --rounds 10 --models cv,f2t,t2f --budget-sec 360
taskset -c 2,3,4,5 .build-bento-wsl/stress_mixed_billion --ops 100000 --rounds 10 --models wg
```

---

## 6. 结论

1. **正确性**：在 3×10^9 次混合操作（21 类）+ 2.28×10^8 次分模型操作中，
   顺序、计数、互斥、取消、关闭竞态、Fiber-local 状态、RSS 稳定等**全部真实断言通过**，
   没有发现丢失/重复/乱序/数据竞争类缺陷。
2. **两起挂起**（`stress_final` 首跑、`t2f` 第 8 轮）都在高负载下出现且未能稳定复现，
   已记录现场与复现命令，建议低负载下复跑确认。
3. **性能**：并发原语的**单锁争用**是明确短板（294 µs/op），分片后恢复；
   跨线程 futex 唤醒 20–100 µs/op 是 f2t/t2f/wg 规模必须降级的根因（非算力问题）。
4. **ctest**：唯一 FAIL 是文档计数滞后（81 vs 85）与测试地图缺 4 项，属既有问题，建议文档维护者修正。
