# channel 模块审计报告（只读）

- 审计日期：2026-10-30
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 Ubuntu 24.04，x86_64）
- 审计快照：`de203e4`；`channel.hpp` md5 `407492b…`，`channel.cpp` md5 `b286c72…`
- 构建：`.build-bento-wsl`（Release，`-O2 -DNDEBUG`，`GO2CPP_BUILD_TESTS=OFF`）
- 边界：**只读 + 跑测试 + 写报告**；未修改实现文件，未 commit

## 一、结论

| 维度 | 结论 | 证据 |
|---|---|---|
| 压测（10 万消息） | **通过**：有缓冲 307 万/s、无缓冲 65.9 万/s；P50 0.53~142.6 µs | §三 |
| 1000 并发 send/recv | **通过**：500 发送 + 500 接收，1 万条零丢失、零重复、零超时 | §四 |
| 随机（1000+） | **通过**：4 万步模型对拍 + 3.2 万步并发随机 + 1.2 万条角色分离随机；不崩、不挂、无泄漏 | §五 |
| 五场景 | **全部通过**（无缓冲配对、缓冲满/空、close 竞态、select 多路、Fiber+线程混合） | §六 |
| 解耦 | **头文件自包含通过；“只依赖 base（sync/parking）”不成立**：`channel.so` 直接依赖 scheduler/fiber，最小消费者需 6 个库 | §七 |
| 测试复查 | **现有测试全部通过，但存在 P1 级覆盖逃逸与 P2 级断言/文档缺口** | §八、§九 |

本次审计**未发现 channel 的功能性丢值、重复投递、死锁或内存泄漏**；发现的问题集中在**模块依赖超标**与**测试有效性/覆盖**。

## 二、检查对象与路径勘误

| 任务书 | 实际路径 | 说明 |
|---|---|---|
| `include/go2cpp/concurrency/channel.hpp` | 同左 | 3283 行，唯一实现头 |
| `include/go2cpp/channel.hpp` | 同左 | 4 行转发头（兼容旧路径） |
| `src/concurrency/channel*.cpp` | `src/concurrency/channel.cpp` | 544 行，仅放类型无关的 `Select` 与错误构造器 |
| 测试 | `tests/concurrency/test_channel.cpp`、`test_channel_concurrency.cpp`、`test_channel_thread_stress.cpp`、`test_fiber_select.cpp`、`tests/bench/channel_thread_mixed_retry.cpp`、`tests/scenarios/channel_deadlock/` | 见 §八 |

`.build-bento-wsl` 的缓存为 `GO2CPP_BUILD_TESTS=OFF`，`ctest -N` 只有 50 个 bento 用例，**没有任何 channel ctest**。本报告使用同一构建产出的 Release 共享库，手工编译并运行全部 channel 测试与审计装置。

## 三、压测：10 万消息吞吐与端到端延迟

拓扑：1 生产者 + 1 消费者，`Channel<Msg>`（16 B），消息内嵌发送时间戳；延迟 = 接收返回时刻 − 发送调用时刻（含排队与唤醒）。每档 10 万条。

| 配置 | 耗时 | 吞吐 | 平均 | P50 | P90 | P99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| 缓冲 1024 | 32.6 ms | **3,071,543/s** | 164.45 µs | 142.63 | 285.71 | 881.56 | 1095.10 |
| 缓冲 64 | 71.6 ms | **1,396,750/s** | 21.49 µs | 12.74 | 48.56 | 125.26 | 305.43 |
| 缓冲 1 | 90.7 ms | **1,102,265/s** | 0.95 µs | 0.53 | 1.35 | 10.45 | 176.94 |
| 无缓冲 | 151.7 ms | **659,216/s** | 1.62 µs | 0.90 | 2.40 | 11.11 | 355.49 |

**解读**：cap=1024 的吞吐最高，但延迟包含“生产快、消费慢”的排队时间，因此 P50 达 142 µs；这不是单次交接成本。cap=0/1 的 P50 < 1 µs，说明 rendezvous 快速路径（自旋 + 直接交接）有效。四档均 10 万条精确收齐，`errors=0`。

## 四、1000 并发 send/recv

- 500 个发送线程 × 20 条 = 10,000 条；500 个接收线程；cap=64。
- 发送完成后再 `Close`，接收者读到 `Closed` 退出。
- 实测：**348.8 ms**，`send_ok=10000`、`recv_ok=10000`、`missing=0`、`dup=0`、`timeouts=0`，1000 个线程全部 join。
- 无崩溃、无挂起；`DebugSnapshot` 的 send/recv waiters 均为 0。

## 五、随机测试（1000+）

### 5.1 单线程模型对拍（4 万步）

3 个通道（cap=0/1/4），随机 `TrySend/TryRecv/Select(3×RecvCase+Default)` 共 40,000 步，与 `std::deque` 参考模型逐值对拍：**mismatches=0**。关闭后 `TrySend→kClosed`、`TryRecv` 先排空再 `kClosed`、重复 `Close→kAlreadyClosed` 均成立。

### 5.2 并发随机（8 线程 × 4000 操作 = 32,000）

4 个通道（cap=0/1/4/16），随机混合 `SendFor(2 ms)`、`TrySend`、`RecvFor(2 ms)`、`TryRecv`、`Select(4×RecvCase±Default, 0.5 ms 超时)`、`Close`、已取消 context 的 `Select`；唯一序号贯穿全程，结束后 close + 排空对账。

| 指标 | 实测 |
|---|---:|
| send_ok / recv_ok | 10,762 / 10,762 |
| missing / dup / phantom | **0 / 0 / 0** |
| 关闭相关操作 / 合法超时 | 4,937 / 722 |
| `ok_without_value`（Ok 却无值） | 0 |
| 结束态 waiters / select 注册数 / 残留缓冲 | 0 / 0 / 0 |
| 崩溃 / 挂起 | 无 |

> 说明：随机拓扑下 722 次 `SendFor/RecvFor` 超时是**合法的“当时无对手”**（8 个线程可能同时都在 send 或都在 recv），不是丢唤醒。为排除这一点，另做了角色分离的 liveness 测试（5.3）；放开超时时间后该数字随拓扑变化，但 missing/dup 始终为 0。

### 5.3 角色分离随机（保证有对手，12,000 条）

4 个固定生产者 + 4 个固定消费者 + 1 个延迟关闭线程；每线程随机 `SendFor(2 s)`/`RecvFor(2 s)`/`Try*`/`Select(SendCase|RecvCase+Default)`。

- **33.1 ms** 完成；`send_ok=recv_ok=2,069`，`timeouts=0`，`bad=0`，`missing/dup/phantom=0`。
- 关闭后剩余发送全部 `Closed`，无丢值。

### 5.4 泄漏与内存检查

| 检查 | 结果 |
|---|---|
| 分配计数（进程级 `operator new` 拦截），同一 fuzz 连跑两轮 | 首轮 +4（进程级缓存），**第二轮 delta=0**；`fuzz_repeat` 三轮为 +4/0/0 |
| ASan + LSan（`detect_leaks=1`）跑 fuzz | **无报告、无泄漏** |
| ASan + LSan 跑五场景（含 Scheduler/Fiber） | **无报告、无泄漏** |
| LSan 方法自检（故意 `new int[10]` 不释放） | 能正确报告 Direct leak，排除“工具没生效” |

## 六、五场景

| # | 场景 | 规模 | 结果 |
|---|---|---|---|
| 1 | 无缓冲配对 | 64 对生产/消费 × 1000 条 + 1 万次 select 双向配对 | 0 mismatch；238 ms |
| 2 | 有缓冲满/空 + 环形回绕 | cap=4 空/满边界；8P8C × 12,500 = 10 万条，cap=3 | missing=0、dup=0；2,575 ms |
| 3 | close 竞态 | 30 轮，4P4C，200~3200 µs 随机关闭 | bad_round=0；77 ms |
| 4 | select 多路 | 8 通道 × 8 发送者 × 2000 条，8 个 selector 各挂 8 个 RecvCase + AfterCase(20 ms) | 16,000 条精确收齐；43.8 ms |
| 5 | Fiber + 线程混合 | Scheduler 4 worker；2 原生 + 2 Fiber 生产、2 原生 + 2 Fiber 消费，共 6 万条；另 2000 次 Fiber Select ↔ 原生 Send | missing=0、dup=0；147.6 ms |

补充：`Select` 公平性 20 万次、4 个始终就绪的 case，份额 0.250/0.249/0.252/0.249，最大偏差 **0.002**，与“随机选一个”的承诺一致（仓库现有测试未对此断言，见 §八）。

## 七、解耦与头文件自包含

### 7.1 头文件自包含：通过

```text
g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Iinclude \
  -x c++ - <<< '#include "go2cpp/concurrency/channel.hpp"'
# OK（无缺头、无前置声明依赖）
```

`go2cpp/channel.hpp` 转发头同样可独立编译。`channel.hpp` 直接包含：`context.hpp`、`core/parking_condition.hpp`、`core/timer.hpp`、`debug/debug_event_ring.hpp`、`debug/debug_resources.hpp`、`error.hpp`（迁移 API 打开时才额外包含 `concurrency/fiber.hpp`）。不含 `sync.hpp`。

### 7.2 “只依赖 base（sync/parking）”：不成立

| 证据 | 实测 |
|---|---|
| `readelf -d libgo2cpp_channel.so` 的直接 NEEDED | `context`、`error`、**`scheduler`**、**`fiber`** |
| `ldd libgo2cpp_channel.so` 的 go2cpp 闭包 | channel → context → **scheduler** → **fiber** → **log**（共 5 个） |
| `nm -D --undefined-only` | `Scheduler::current_task_ptr()`、`Fiber::Current()`、`ParkingCondition::*` |
| `channel.cpp` 直接包含 | `fiber.hpp`、`scheduler.hpp`（用于 `CanSpinWait`） |
| 最小消费者仅链 `-lgo2cpp_channel -lgo2cpp_error` | **链接失败**：`DoneSignal::RemoveCallback`（context）DSO missing |
| 再加 `-lgo2cpp_context` | **仍失败**：`ParkingCondition::ParkingCondition`（位于 scheduler 目标）DSO missing |
| 完整闭包 | 必须链 `channel+context+scheduler+fiber+log+error` 共 **6 个库** 才能运行 |

根因有两层：

1. `ParkingCondition` 的实现被编进 `go2cpp_scheduler` 目标（`cmake/Go2CppModules.cmake:23-30`），而不是独立的 base/parking 目标；
2. `go2cpp_channel PUBLIC go2cpp_context go2cpp_error`，`go2cpp_context PUBLIC go2cpp_scheduler`，`go2cpp_scheduler PUBLIC go2cpp_fiber go2cpp_log`；同时 `channel.cpp` 直接引用 Scheduler/Fiber 符号，链接器必须显式带上它们。

因此**“只依赖 base（sync/parking）”在模块/链接层面不成立**；`channel.hpp` 还无条件包含 timer 与 debug 头（OFF 构建宏为空，但编译期仍解析），编译面也比“纯 base”更大。若目标是让 channel 成为可独立嵌入的 base 原语，需要把 `CanSpinWait` 的 Scheduler/Fiber 判定下沉，并把 `ParkingCondition` 从 scheduler 目标中拆出；本次不改实现。

## 八、测试复查

### 8.1 现有测试与运行结果

| 文件 | 行数 | 断言数 | 注册情况 | 本次结果 |
|---|---:|---:|---|---|
| `tests/concurrency/test_channel.cpp` | 1079 | 152 个 `GO2CPP_CHECK` | `go2cpp_tests`（filter=channel） | **通过**（含 T1-T11/B1-B4/R1） |
| `tests/concurrency/test_channel_concurrency.cpp` | 682 | 49 个 `CHECK` | `go2cpp_channel_concurrency` ctest | **通过**（8 组） |
| `tests/concurrency/test_fiber_select.cpp` | 142 | 6 个 `Check` | `go2cpp_fiber_select_test` ctest | **通过**（F1-F5） |
| `tests/concurrency/test_channel_thread_stress.cpp` | 817 | 0 个 `CHECK`（用 `Fail` 计数） | **未注册** | 手工 5 轮 × 7 场景 × 2 万条 **通过**（4.7 s） |
| `tests/bench/channel_thread_mixed_retry.cpp` | 611 | 无 | **未注册**（bench 复现器） | 未跑（长跑复现器，非验收测试） |
| `tests/scenarios/channel_deadlock/` | 727 | 脚本断言 | 仅 `GO2CPP_BUILD_ENHANCED=ON` 时注册 | 默认构建未注册 |

断言机制本身有效：`GO2CPP_CHECK` 累计 `g_failures`，`test_main.cpp` 在非零时返回 1；`GO2CPP_REQUIRE`/watchdog 超时直接 abort；未知 `GO2CPP_TEST_FILTER` 返回 2，避免“0 个用例 = 通过”。

### 8.2 覆盖缺口

| 缺口 | 风险 | 说明 |
|---|---|---|
| `test_channel_thread_stress.cpp` 未进 CMake | **P1** | 817 行高频混合压测（mpmc64/1/0、rtt0、mixed）在 ctest 中完全不存在；只能手工编译 |
| 默认构建 `GO2CPP_BUILD_TESTS=OFF` | P1（环境） | `.build-bento-wsl` 的 ctest 无 channel；本次手工编译才跑到 |
| channel 死锁场景仅增强构建注册 | P2 | 默认构建永不运行；脚本缺二进制时 `exit 77`，ctest 记为 SKIP 而非失败 |
| Select 公平性无断言 | P2 | 仓库测试没有“多 case 同时就绪”的分布断言；本次 20 万次实测偏差 0.002，但无回归保护 |
| Fiber 与原生线程**同时**操作同一 channel | P2 | 已注册测试里 Fiber 用例只包含 Fiber/主线程；本次场景 5 才覆盖 2 原生 + 2 Fiber 同时收发 |
| `Send` 关闭后的状态 | P2 | 文档说 `kAlreadyClosed`，实现返回 `kClosed`；测试统一用 `.Closed()`（两者都真），矛盾被掩盖 |
| `Range` | P2 | 文件头两处写 `Range`，但 `Channel<T>` **没有 Range 方法**（grep 无定义） |
| `ForceWakeWaiters` | P2 | 无任何测试（仅 Doctor 路径使用） |
| `WaitForChange`/`Generation` 的 channel 集成 | P3 | 只在 `test_timer.cpp` 间接覆盖 |
| move-only T 的内建 select 拒绝路径 | P3 | `unsupported` 字段存在，但没有 `unique_ptr` 等用例 |

### 8.3 断言/跳过有效性问题

| 级别 | 问题 | 位置 | 影响 |
|---|---|---|---|
| P2 | `TestRandomCloseExactCounting` 在 1.5~8 ms 就 close；本次运行 6000 条里仅 **474 条**发送成功，其余直接 `Closed`。精确计数只覆盖约 8% 的值 | `test_channel_concurrency.cpp:234-238` | close 前“已发送值恰好一次”的验证量偏小；建议延迟 close 或循环多轮 |
| P2 | FIFO 用例用 `sleep_for(5ms)` 拉开线程启动顺序，再断言严格 FIFO | `test_channel_concurrency.cpp:437-465` | 高负载/慢 CI 下可能误判；建议用 barrier/握手确定入队顺序 |
| P2 | close 唤醒断言 `< 1000 ms` 是墙钟阈值 | `test_channel_concurrency.cpp:348,382,416` | 重载机器上可能假失败 |
| P3 | `test_channel.cpp` 多处 `sleep_for(2ms/5ms)` 后断言“已挂起/已开始” | 约 75、129、173、188、207、256、270 行 | 时序依赖，非确定性 |
| P3 | 随机 close 用未播种的 `std::rand()`，序列固定但不可控 | `test_channel_concurrency.cpp:236` | 复现性依赖 libc 实现；建议固定种子 |
| P2 | 头注释与 CMake 不一致：注释写“不注册 ctest”，CMake 实际注册 | `test_channel_concurrency.cpp:3` vs `cmake/Go2CppTests.cmake:780-789` | 维护者会误判覆盖情况 |
| P2 | 文档与实现不一致：`Send` 关闭返回 `kClosed`（实现）而头注释写 `kAlreadyClosed`；`Range` 只存在于文档 | `channel.hpp:51,2619`；`channel.hpp:8,52` | 用户按文档写 `== kAlreadyClosed` 会失败 |

**SKIP 逃逸**：channel 测试文件内部没有 `GTEST_SKIP`/`DISABLED`/`#if 0`；真正的逃逸是“未注册”（817 行压测、611 行复现器）和“默认构建不注册 + 缺件 exit 77”的死锁场景。二者都不会让 CI 变红。

## 九、问题清单

| 级别 | 编号 | 问题 | 位置 | 建议 |
|---|---|---|---|---|
| P1 | C1 | channel 的链接闭包含 scheduler/fiber/log，最小消费者需 6 个库；“只依赖 base”不成立 | `cmake/Go2CppModules.cmake:23,303`；`channel.cpp:33-34` | 拆出独立 parking 目标；把 `CanSpinWait` 的 Scheduler/Fiber 判定下沉或改为编译期钩子 |
| P1 | C2 | 817 行线程压测未注册 ctest；默认构建 tests 关闭，channel 在 ctest 中零覆盖 | `tests/concurrency/test_channel_thread_stress.cpp`；`.build-bento-wsl/CMakeCache.txt` | 注册为独立 ctest（小规模）+ 夜间长跑；审计构建用 `GO2CPP_BUILD_TESTS=ON` |
| P2 | C3 | `Send` 关闭后返回 `kClosed`，文档/成员注释写 `kAlreadyClosed`；测试只用 `.Closed()` 掩盖 | `channel.hpp:51,2078,2619`；`test_channel.cpp:89,98` | 统一文档与实现，测试显式断言具体状态 |
| P2 | C4 | 文件头承诺 `Range`，实现不存在 | `channel.hpp:8,52` | 删除文档或补 API + 测试 |
| P2 | C5 | 死锁场景仅增强构建注册，缺件 `exit 77` 记 SKIP | `cmake/Go2CppTests.cmake:899-905`；`run_channel_deadlock.sh:63-71` | 默认构建也注册（不需要 Doctor 的 timeout/multi/default）；缺件应 fail 或显式标注 |
| P2 | C6 | 随机 close 场景有效发送量过低（本次 474/6000），close 竞态覆盖偏弱 | `test_channel_concurrency.cpp:234-238` | 延迟到 50%~80% 进度再 close，或增加轮次 |
| P2 | C7 | Select 公平性、Fiber+原生线程同 channel 无回归断言 | `test_channel.cpp`、`test_fiber_select.cpp` | 把本次 fairness 200k 与混合场景纳入 ctest |
| P2 | C8 | `ForceWakeWaiters`、`WaitForChange`、move-only select 拒绝路径无测试 | `channel.hpp:1194,1242`；`test_channel.cpp` | 补 3~5 个定向用例 |
| P3 | C9 | FIFO/唤醒断言依赖 sleep 与 1 s 墙钟阈值 | `test_channel_concurrency.cpp` | 改握手/轮询，去掉绝对时间阈值 |
| P3 | C10 | `test_channel_concurrency.cpp` 头注释“不注册 ctest”已过期 | 文件第 3 行 | 更新注释 |
| P3 | C11 | 随机 close 使用未播种 `std::rand()` | `test_channel_concurrency.cpp:236` | 固定种子 |

## 十、未覆盖与限制

1. **未跑全量 ctest**：`.build-bento-wsl` 是 `GO2CPP_BUILD_TESTS=OFF`，channel 测试为独立编译运行；CI 默认构建的整条链路未复现。
2. **未做 TSan 全库构建**：共享库未插桩，TSan 只能检查测试装置本身；本周期的并发正确性依赖 ASan/LSan + 对账，而非 race detector。
3. **未验证 >10 万消息**：10 万已满足任务要求；cap=1024 的延迟含排队，未测更大队列的尾延迟。
4. **未做 ARM/其他平台**：仅 x86_64 WSL2。
5. **未深挖重构**：按任务要求只记录依赖结构，不提交拆分方案。
6. **审计期间工作树 HEAD 被其他 agent 推进**（`e7f3ec4→…→de203e4`），但 `channel.hpp`/`channel.cpp` 的 md5 未变；测试与库均按变更后的 HEAD 重建。

## 十一、复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl -j"$(nproc)"

# 审计装置（临时源码，已按任务清理；链接现有 Release 库）
g++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Ithird_party \
  channel_audit.cpp -L.build-bento-wsl \
  -Wl,-rpath,"$PWD/.build-bento-wsl" \
  -lgo2cpp_channel -lgo2cpp_context -lgo2cpp_scheduler \
  -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_error -pthread -o channel_audit

./channel_audit all        # 随机 + 5 场景 + 4 档压测 + 1000 并发 + 公平性
./channel_audit fuzz       # 两轮随机 + 泄漏趋势
./channel_audit fuzz_repeat# 三轮分配增长

# 仓库自带测试（同链接参数）
g++ -std=c++17 -O2 -Iinclude -Itests main_channel.cpp \
  tests/concurrency/test_channel.cpp <同样的 -l 列表> -o test_channel
./test_channel
g++ -std=c++17 -O2 -Iinclude tests/concurrency/test_channel_concurrency.cpp \
  <同样的 -l 列表> -o test_channel_concurrency && ./test_channel_concurrency
g++ -std=c++17 -O2 -Iinclude tests/concurrency/test_fiber_select.cpp \
  <同样的 -l 列表> -o test_fiber_select && ./test_fiber_select
g++ -std=c++17 -O2 -Iinclude tests/concurrency/test_channel_thread_stress.cpp \
  -lgo2cpp_sync <同样的 -l 列表> -o test_channel_thread_stress
./test_channel_thread_stress --rounds=5 --msgs=20000 --scenario=all --no-gdb

# 解耦证据
readelf -d .build-bento-wsl/libgo2cpp_channel.so | grep NEEDED
nm -D --undefined-only .build-bento-wsl/libgo2cpp_channel.so | c++filt | \
  grep -E 'Scheduler|Fiber|ParkingCondition'
```

> 审计过程中创建的临时源码、二进制与构建目录已清理；仓库实现文件未改动。`/tmp/audit_base_channel.md` 与本文件内容一致。
