# Go2Cpp 全模块理解力考试（出题规范 + 索引）

> 目的：逼接手者把项目真正读懂，而不是背 API。
> 判据：能答出「为什么这么设计」「不这么写会坏在哪」「数据从哪来」，并能指到代码行。
>
> **2026-11-03 铁律：不要相信项目里的任何文档数字。** 本项目迭代极快，一小时可能优化多轮，
> `docs/**` 里的 ns/吞吐都可能过时或从未有出处。本索引中的任何数字只作**历史对照**；
> 所有涉及性能的验收，一律要求答题者当场跑一次并报出测量条件（核数、`taskset`、轮数、min/median）。

## 一、题目总量

| 模块 | 文件 | 题数 | 状态 |
|---|---|---:|---|
| concurrency（Fiber / Scheduler / Channel / Mutex / WaitNode / Select / Context） | `01-concurrency.md` | 15 | 完成 |
| network | `02-network.md` | 20 | 已出（数字口径需按 §三.3 复核） |
| bento 应用框架 | `03-bento.md` | 20 | 已出（数字口径需按 §三.3 复核） |
| storage / cache | `04-storage.md` | 20 | 待出 |
| media | `05-media.md` | 20 | 待出 |
| p2p | `06-p2p.md` | 20 | 待出 |
| rpc | `07-rpc.md` | 20 | 待出 |
| distributed | `08-distributed.md` | 20 | 待出 |
| observability | `09-observability.md` | 20 | 待出 |
| tools | `10-tools.md` | 20 | 待出 |
| app | `11-app.md` | 20 | 待出 |
| relay / jinshan | `12-relay-jinshan.md` | 20 | 待出 |
| tunnel | `14-tunnel.md` | 20 | 已出（数字口径需按 §三.3 复核） |
| os | `15-os.md` | 20 | 待出 |
| go-api | `16-go-api.md` | 20 | 待出 |
| bufio | `17-bufio.md` | 20 | 待出 |
| 汇编/性能专题 | `18-asm.md` | 20 | 待出 |

 > 合计：concurrency 15 + 已出三卷（network/bento/tunnel 各 20）+ 其余 13 组 × 20 = **335 题**。
> 注：`02-network.md`、`03-bento.md`、`14-tunnel.md` 由其他出题 agent 产出，题目结构一致，
> 但它们的「数字口径」段落写在铁律之前（允许引用 docs 数字）；按 §三.3，这三卷的数字题需复核。
> 说明：concurrency 原计划 32 题，2026-11-03 按要求砍到 15 题，只保留 L3/L4（见 §七）。

## 二、每题必须包含的四个字段（缺一不合格）

- `【Ln 类型】`：L1 记忆 / L2 理解 / L3 陷阱 / L4 推演
- **痛点**：一句话说清「答不出说明哪里没读懂」
- **追查位置**：必须给 `文件:行号`，且能 `sed -n 'Np'` 看到相关内容
- **验收标准**：分条列出判据，要给可判定的技术事实，不许写「理解xxx」
- **常见错误答案**：至少一条，用于区分「背过」和「读过」

> 本卷追加第五段：**怎么自证**——必须给出可执行的测试/工具/最小复现命令，
> 以及要报出的指标与条件。没有自证入口的性能题不合格。

## 三、出题硬性规则

1. 行号必须真实，写完用 `sed -n 'N,Mp'` 复核；编造行号视为不合格。
2. 不许出「API 是什么」这种查文档就有的题。
3. 答案里出现数字必须来自**当场实测**；`docs/**` 的数字只能作历史对照，不能当验收判据。
   查不到/跑不出就写「未实测，答案定性」。
4. 每 20 题至少 L3 陷阱 ≥5、L4 跨模块 ≥3；本卷 15 题全部为 L3/L4。
5. 陷阱题优先选项目真实踩过的坑：`docs/audit/` 的 6 份基础审计、`AI_COLLAB.md`、
   commit 信息里「修复前/修复后」的记录（如 park→wake 10 ms 长尾、Mutex 双持、Select 丢值）。
6. 同一机制考不同侧面（语义/实现/性能/失效），不要重复。
7. **性能题必须能自己跑**：优先指向 `tests/concurrency/**`、`tools/startup_cost/**`、`ctest`；
   禁止用「报告里写 X ns」作为通过条件。

## 四、评分口径

L1 全对是基本要求；L2 ≥80%；L3 ≥60% 才算真读懂；L4 ≥40% 才算能独立改代码。
本卷无 L1/L2，因此及格线按 L3 ≥60%、L4 ≥40% 分别统计；两档都过才算通过。

## 五、本卷自证入口（2026-11-03 本机已验证）

| 入口 | 命令 | 本机结果 |
|---|---|---|
| 调度器 task churn | 编译 `tests/concurrency/bench_scheduler_task_opt.cpp`（命令见 `01-concurrency.md` Q01）→ `/tmp/bench_scheduler_task_opt 4 100000 3` | wall 323/253/155 ns、cpu 1526/1222/754 ns（12 vCPU、未绑核、3 轮） |
| park→wake 长尾 | `STARTUP_COST_OUT_DIR=/tmp/exam-build tools/startup_cost/build.sh` → `/tmp/exam-build/startup_cost_park_wake --mode runtime --iters 200 --calibrate` | p50 TOTAL 13.98 µs、p99 55.76 µs、`>1ms=0` |
| 功能回归 | `cd .build-bento-wsl && ctest -R 'go2cpp_scheduler_smoke\|go2cpp_select_lockorder_test' --output-on-failure` | 2/2 通过 |

> 这三条命令已实际执行；其余题里给的 ctest 用例名来自 `ctest -N`（共 290 条），
> 出题时未逐条重跑，答题者应自己跑并报告结果。

## 六、总索引（concurrency 15 题）

| 题号 | 难度 | 主题 | 考察机制 | 自证入口 |
|---|---|---|---|---|
| Q01 | L4 推演 | 调度器口径 | wall vs submit、历史数字不可复现、对外表述 | `bench_scheduler_task_opt 4 100000 5` |
| Q02 | L3 陷阱 | runnext 单槽 | 亲和性、挤出到环尾、yield 饿死 | `ctest -R scheduler_smoke` |
| Q03 | L3 陷阱 | Fiber 栈 | 2 KB→4 KB、8 MiB VA、10 万连接、SIGSEGV 边界 | `ctest -R fiber_growable_stack` |
| Q04 | L3 陷阱 | Channel vs Vyukov | 单锁语义、实验件定位、Cap=1 退化 | `ctest -R channel_concurrency` |
| Q05 | L4 推演 | park→wake 长尾 | runnext 消费方与 notify_one 错配、广播修复 | `startup_cost_park_wake` |
| Q06 | L3 陷阱 | findrunnable | 锁分级、incoming 轮转、netpoll 登记 | `ctest -R scheduler_queue_latency` |
| Q07 | L4 推演 | sysmon / BlockingRegion | detach 可逆、不能抢占、region 契约 | `ctest -R scheduler_smoke` |
| Q08 | L4 推演 | shutdown | 未启动/已启动、O(n²)、live_tasks | 10 万 parked G 关停扩展性 |
| Q09 | L3 陷阱 | 双 claim / 状态字 | execution_claim、run_claim、wake 令牌 | `ctest -R scheduler_smoke` |
| Q10 | L3 陷阱 | per-P 分片 | 三类分片、O(P) 聚合、drain 精确判零 | `bench_scheduler_task_opt` 1P vs 4P |
| Q11 | L3 陷阱 | Channel 等待队列 | 侵入式链表、per-node futex、单一赢家 | `ctest -R channel_thread_stress` |
| Q12 | L3 陷阱 | Mutex 状态字 | 两态、load+store 双持、UpdateStateAtomic | `ctest -R sync_cancel_stress` |
| Q13 | L3 陷阱 | WaitNode O(n²) | 僵尸节点、全队列重排、FutexMutex | 大量超时等待者压测 |
| Q14 | L3 陷阱 | Select 有值优先 | closed 延后、传输不可回滚、ABI | `ctest -R select_lockorder_test` |
| Q15 | L4 推演 | Context 传播 / 回滚 | 先标记后回调、逆序、rollback 冻结 | `ctest -R context_rollback_demo` |

## 七、难度与覆盖校验

- 难度：L3 = 10 题（Q02–Q04、Q06、Q09–Q14），L4 = 5 题（Q01、Q05、Q07、Q08、Q15）；无 L1/L2。
- 覆盖：Fiber 1、Scheduler 9（口径、runnext、长尾、findrunnable、sysmon、shutdown、双 claim、分片、栈边界与 Scheduler 共用）、
  Channel 2、Mutex 1、WaitNode 1、Select 1、Context 1。
- 机制不重复：Q02 只考 runnext 的结构取舍，Q05 只考 park→wake 的故障链；
  Q01 只考测量口径，Q10 只考分片与精确聚合。

## 八、维护约定

1. 代码改动后先跑 §五 的三条入口，再决定题目是否还成立；失效的行号立即修。
2. 禁止把「报告里的数字」写进验收；只能写「自己跑并报条件」。
3. 新增题必须同时给「怎么自证」；给不出自证入口的性能题不收。
4. 索引与题目文件必须同步改；`docs/exam/` 之外的文件不作为本考试的产出。
