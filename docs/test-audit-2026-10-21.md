# Go2Cpp 测试审计报告（2026-10-21）

> **【历史】以下 0–8 节为 2026-10-21 早间审计快照，数字已过时，保留原文。**
>
> 审计对象：`/UserData/CodexWorkSpace/Go2Cpp-work`（git HEAD `03af552`）
> 审计方式：全量静态扫描（CMake、测试源码、脚本）+ 16 项实跑验证（含 2 项失败/阻断）
> 边界：只读代码，仅新增本文件与 `/tmp/test_audit.md`

## 2026-10-21 20:00 更新摘要（当前状态，优先阅读）

> 本节与第 9 节是本轮复核结果（工作树 HEAD `18dd4df`，2026-10-21 19:46 "channel 互等死锁 vs select 语义"）。
> **第 0–8 节是 2026-10-21 早间的历史快照（HEAD `03af552`，ctest 116 / 增强 170 口径），按要求保留原文、不回改**；
> 历史节中的数字与结论凡与本节冲突，以本节与第 9 节为准。

### 一句话结论

测试已从「能跑但进不了 ctest」推进到 **默认 ctest 130/130 全绿 + 增强构建 62/62 全绿 + 增强矩阵 183 用例 0 产品 FAIL**。
剩余问题收敛为四点：**1 条 flaky 用例（`events:包含事件行`）**、**2 个 hook 测试仍无法注册（有明确产品/测试原因）**、
**bench 未入 CI**、**新能力仍无 Sanitizer/Valgrind 记录**。

### 实测总表（2026-10-21 20:00）

| 测试面 | 入口 | 实测结果 | 进默认 ctest |
|---|---|---|---:|
| 默认构建 ctest | `ctest --test-dir build-work -j4` | **130/130 通过，26.16 s** | ✅ |
| 增强构建 ctest | `ctest --test-dir build-enhanced -j2` | **62/62 通过，77.13 s** | ✅（增强配置） |
| 五能力增强矩阵（含压测与父 park） | `bash tests/all_enhanced.sh` | **183 用例：PASS 182 / SKIP 1 / FAIL 0 / GAP 0** | ❌（脚本） |
| 　└ Debug | 同上 | 41（矩阵 36 + 压测 4 + 父 park 1 SKIP） | ❌ |
| 　└ 迁移 | 同上 | 36（矩阵 34 + 压测 2） | ❌ |
| 　└ 回放 | 同上 | 36（矩阵 34 + 压测 2） | ❌ |
| 　└ CLI | 同上 | 35（矩阵 31 + 压测 4） | ❌ |
| 　└ Doctor | 同上 | 35（矩阵 35） | ❌ |
| 场景（4 条，仅增强构建注册） | `ctest --test-dir build-enhanced -R '^scenario-'` | **4/4 通过**：deadlock-routes 2.07 s、full-trace 21.09 s、ctx-cancel 1.52 s、channel-deadlock 4.89 s | ✅ 仅增强 |
| tools 三套（ctest 包装） | `ctest --test-dir build-work -R '^tools-'` | **3/3 真跑通过**（非 SKIP）：migration 1.38 s、cli 3.70 s、replay 3.75 s | ✅ |
| 内存 bench（手工脚本） | `QUICK=1 bash tests/bench/run_memory_bench.sh` | fiber **60.6 KB/单位**（RSS）vs 线程 **65.8**（−8%）；虚拟 **68.5 KB** vs **6294 KB**（≈92×） | ❌ |
| 切换 bench（手工脚本） | `bash tests/bench/run_switch_bench.sh` | fiber **~500 ns** vs 线程 **~28.6 µs**（≈57×），3 轮 466/514/523 ns | ❌ |

### ctest 116 → 130 的构成（+14，逐条可对账）

| 增量 | 条数 | 内容 |
|---|---:|---|
| 审计孤儿 + 资源历史单测 | **+7** | `channel_concurrency`、`worker_lifecycle`、`future_audit`、`sync_cancel_stress`、`io_audit`、`parking_condition_stress`、`resource_history_test` |
| 并发原语测试 | **+4** | `primitives_stress`、`primitives_cancel`、`park_wake_pair`、`resource_history_stress` |
| tools 三套包装 | **+3** | `tools-migration`、`tools-cli`、`tools-replay`（缺构建目录时 exit 77 记 SKIP，不伪造 PASS） |

> 130 = 116 + 14，与 `ctest -N` 的 `Total Tests: 130` 一致（完整输出见 9.1 附录）。

### 本轮同时纠正的三处历史记录

1. **`hook_fifo` / `hook_behavior` 并未注册**：历史第 8 节与 `docs/testing.md` 早前版本称「7 个高风险孤儿已注册（含 hook_fifo、hook_behavior），ctest 123」——
   实际 `build-work`/`build-enhanced` 的 `CTestTestfile.cmake` 对二者 **0 命中**，`cmake/Go2CppTests.cmake:887-894` 明确记录它们**不注册**及原因
   （`hook_fifo` 测试自身死锁；`hook_behavior` 断言的是已记录的产品缺陷，不能改断言凑通过）。
   真正的 7 条是 6 个原孤儿 + 1 个新增资源历史单测。
2. **增强矩阵不是 170 条**：170 只是五能力主矩阵；加上 4 个压测脚本 12 条与父 park 1 条（SKIP），总数 **183**，由 `tests/enhanced/report.py` 汇总。
3. **场景从 3 条变 4 条**：新增 `channel_deadlock`（HEAD `18dd4df` 提交），四场景全部在增强构建注册并实跑通过。

---

## 0. 结论摘要

| 项 | 结果 |
|---|---|
| `ctest`（`build-work`） | **116/116 通过，60.83 s**（实测） |
| ctest 构成 | 41 条测试类 + **75 条示例冒烟**（example 被当测试注册） |
| `go2cpp_tests` 内部模块 | 46 个模块名（内含 10 个 `debug_*` 与 3 个回放/计时调试模块） |
| 独立测试二进制 | 38 个（41 条测试类减去 3 条同二进制 filter 变体） |
| 五能力增强主矩阵 | **170/170 通过**（debug 36 / migration 34 / replay 34 / cli 31 / doctor 35，实测） |
| `tools/migration` 场景 | **53/53 通过**（实测） |
| `tools/cli` 验收 | **213/213 通过**（实测） |
| `tools/replay` | 单测通过 + 6 个脚本场景通过（实测） |
| `ctx.cancel` 场景 | **65/65 通过**（实测） |
| 资源历史脚本 | **ALL PASS**（12 项 JSON 检查，实测） |
| 未注册测试源文件 | **12 个，约 3,900 行**（其中测试逻辑约 3,100 行，含 682 行 Channel 并发压测、599 行 M 生命周期测试） |
| 一键入口 `tests/all_enhanced.sh` | **实跑 exit 2**：硬编码 `/tmp/go2cpp-cli-build` 缺失 |
| CI | **无**（无 `.github`、`.gitlab-ci.yml`、`Jenkinsfile`） |

**核心判断**：新能力测试本身是"能跑且通过"的，但**没有进入 ctest、没有统一入口、依赖 `/tmp` 与多个特定构建目录**，且 ctest 全绿并不代表新能力被覆盖——`build-work` 的 `GO2CPP_DEBUG_DUMP`/`GO2CPP_DIAGNOSTIC`/`GO2CPP_REPLAY`/`GO2CPP_MIGRATION_API` **全部为 OFF**。

---

## 1. 审计范围与方法

### 1.1 范围

- `tests/` 全部文件（含 `bufio/compress/container/encoding/hash/math/path/os/network/logging/config/metrics/tools/concurrency/debug/replay/scenarios/enhanced`）；
- `tools/cli/tests`、`tools/migration/tests`、`tools/replay/tests` 与 `tools/replay/scripts`；
- `cmake/Go2CppTests.cmake`、`cmake/Go2CppOptions.cmake`、`cmake/Go2CppMigration.cmake`、`tools/*/CMakeLists.txt`；
- 各构建目录 `CMakeCache.txt` 的选项组合。

### 1.2 方法

1. 解析 CMake，建立「源文件 → target → `add_test`」映射，找出未被任何 target 引用的文件；
2. 统计每个测试源文件的断言宏调用数（`GO2CPP_CHECK`、`CHECK`、`*_CHECK`、`Check`、`REQUIRE`、`assert`），作为用例规模近似值；
3. 统计脚本中的 `pass/fail/skip/gap` 调用点；
4. 在真实构建上实跑 16 项测试（见第 6 节），记录退出码与用例计数；
5. 交叉核对 `docs/testing.md`、`docs/NEXT-STEPS.md`、`tests/enhanced/defects.md` 与实际行为。

> 口径说明：C++ 侧「用例数」并非框架级计数（项目用自定义断言宏 + 单一 main），本报告用**静态检查点数**近似；脚本侧用实际 PASS/FAIL 计数。

---

## 2. 测试清单

### 2.1 ctest 116 条（`build-work`）

配置：Release，`GO2CPP_BUILD_TESTS=ON`、`GO2CPP_BUILD_EXAMPLES=ON`，其余新能力开关全 OFF。

| 分组 | 条数 | 说明 |
|---|---:|---|
| `go2cpp_tests` 及其 filter 变体 | 4 | `go2cpp_tests`、`unknown_filter_rejected`、`known_filter_runs`、`fiber_growable_stack` |
| 并发/调度/计时/状态机专项 | 7 | smoke、high_load、timer_stress、state_machine、queue_latency、state_matrix、fiber_select |
| 网络 / HTTP2 / hook / IO | 13 | hpack、frame、stream_state、connection、hook_ops、hook_raw、hook_correctness、hook_sleep、fiber_transport、http2_stress、http2_boundary、http2_state_matrix、desync_repro |
| OS / config / path / 基础库 / 日志 / metrics | 17 | os×3、config×2、path×2、compress、encoding、hash、container、bufio×2、math、rand、log_buffer、metrics |
| 示例冒烟 | **75** | `example/**` 全部注册为 ctest，仅验证进程退出 0 |

关键事实：

- `add_test` 在 CMake 中出现 43 次，其中 examples 循环展开为 75 条，合计 116；
- `go2cpp_tls_alpn_test` 只构建、不注册（避免依赖 `openssl s_server`）；
- `go2cpp_diagnostic_demo`、`go2cpp_debug_subscribe_demo`、`go2cpp_full_trace_scenario`、`go2cpp_deadlock_routes`、`go2cpp_ctx_cancel_rollback` 均**只构建不注册**。

### 2.2 `go2cpp_tests` 内部模块（46 个）

`tests/test_main.cpp` 通过 `GO2CPP_TEST_FILTER` 选择模块：

| 科目 | 模块名 | 主要文件 | 检查点（静态） |
|---|---|---|---:|
| 工具 | error / errors_ns / control_flow / strings / strconv / fmt / streams / time | `tests/tools/*.cpp` | 311 |
| 并发核心 | context / channel / scheduler / scheduler_stress / dynamic / fiber / fiber_growable / sync / locks / spin_mutex / future / event_go | `tests/concurrency/*.cpp` | 1,278 |
| 同步原语 | futex_mutex / parking_condition / thread_policy / fiber_local / beginner | 同上 | 106 |
| 网络 | io / network / sse / fd_manager / hook | `tests/network/*.cpp` | 564 |
| 日志 | log | `tests/logging/test_log_config.cpp` | 92 |
| 计时 | timer | `tests/concurrency/test_timer.cpp` | 55 |
| Debug | debug_fiber / debug_scheduler / debug_cli / debug_sync / debug_lock_selector / debug_events / debug_breakpoints / debug_signal_dump / debug_dump / debug_config | `tests/debug/*.cpp` | 635 |
| 回放 | resource_replay / timer_replay / timer_debug | `tests/replay/*.cpp`、`tests/debug/test_timer_debug.cpp` | 239 |
| 诊断（条件） | diagnostic（仅 `GO2CPP_DIAGNOSTIC=ON`） | `tests/debug/test_diagnostic.cpp` | 170 |

`tests/test_main.cpp` 还包含 T-08 回归：未知 filter 返回 2（不是 0），避免"空跑当通过"。

### 2.3 独立测试目标（38 个）

| 模块 | target | 源文件 | 检查点 | ctest |
|---|---|---|---:|---|
| 调度 | `go2cpp_scheduler_smoke` | `scheduler_smoke.cpp` | 54 | ✅ |
| 调度 | `go2cpp_high_load_stress` | `high_load_stress.cpp` | 33 | ✅ |
| 调度 | `go2cpp_scheduler_queue_latency` | 同名 | 6 | ✅ |
| 计时 | `go2cpp_timer_stress` | 同名 | 17 | ✅ |
| 状态机 | `go2cpp_state_machine` / `go2cpp_state_matrix` | `test_state_machine.cpp` / `test_state_machine_matrix.cpp` | 38 / 63 | ✅ |
| HTTP/2 | `hpack` / `frame` / `stream_state` / `connection` / `fiber_transport` / `http2_stress` / `http2_boundary` / `http2_state_matrix` / `http2_state_desync_repro` | `tests/network/*` | 86+72+39+89+15+27+1+57+33 | ✅ |
| hook | `hook_ops` / `hook_raw` / `hook_correctness` / `hook_sleep` | `tests/network/*` | 17+22+35+26 | ✅ |
| IO | `go2cpp_tests` 内 `test_io.cpp` | 973 行 | 166 | ✅ |
| OS | `os_test` / `os_dir_test` / `os_hook_conflict_test` | `tests/os/*` | 128+101+17 | ✅ |
| config | `config_registry_macro` / `so_manifest` | `tests/config/*` | 41+2 | ✅ |
| path | `path_test` / `path_filepath_test` | `tests/path/*` | 41+122 | ✅ |
| 容器 | `container_test` | 7 文件 | 230 | ✅ |
| 压缩 | `compress_test` | 9 文件 | 260 | ✅ |
| 编码 | `encoding_test` | 8 文件 | 444 | ✅ |
| 哈希 | `hash_test` | 5 文件 | 113 | ✅ |
| bufio | `bufio_test` / `bufio_stress` | 2 文件 | 431/0 | ✅ |
| math | `math_test` / `rand_test` | 2 文件 | 294 | ✅ |
| 日志 | `log_buffer_test` | 342 行 | 30 | ✅ |
| metrics | `metrics_test` | 1370 行 | 128 | ✅ |
| 并发 | `fiber_select_test` | 142 行 | 6 | ✅ |
| TLS | `tls_alpn_test` | 99 行 | 7 | ❌ 仅构建 |

> 注：`http2_boundary` 以「子进程 + 超时监控」为主，断言在 `ExpectNoCrash` 内部，静态宏计数仅 1，不代表实际覆盖强度；其余数字为宏调用近似值，实跑均通过。

### 2.4 脚本测试

#### A. 五能力增强矩阵（`tests/enhanced/`）

| 能力 | 脚本 | 用例数 | 依赖 | 进 ctest |
|---|---|---:|---|---|
| Debug | `debug/test_debug.sh` | **36**（实测） | `build-debug/go2cpp_debug_subscribe_demo` + CLI | ❌ |
| Debug 父 park | `debug/test_parent_park.sh` | 10 个判定点（7 PASS/2 GAP/1 FAIL） | `build-debug/` + g++ 直编 | ❌ |
| Debug 压测 | `debug/stress_debug.sh` | 4 | 同上 | ❌ |
| 迁移 | `migration/test_migration.sh` | **34**（实测） | `build-migration/migrate*` + `libmigration_demo_*.so` + CLI | ❌ |
| 迁移压测 | `migration/stress_migration.sh` | 3 | 同上 | ❌ |
| 回放 | `replay/test_replay.sh` | **34**（实测） | `build-replay/{replay,go2cpp_replay_*}` + CLI | ❌ |
| 回放压测 | `replay/stress_replay.sh` | 2 | 同上 | ❌ |
| CLI | `cli/test_cli.sh` | **31**（实测） | `build-cli/tools/cli/go2cpp-cli` | ❌ |
| CLI 压测 | `cli/stress_cli.sh` | 4（实测，1000 组随机） | 同上 | ❌ |
| Doctor | `doctor/test_doctor.sh` | **35**（实测） | `build-diag/go2cpp_diagnostic_demo` + `tests/debug/doctor_client.py` | ❌ |
| 一键 | `run_all.sh` | 10 个脚本 | 硬编码 `/tmp/go2cpp-cli-build` | ❌（实跑失败） |

公共库 `lib/common.sh` 默认值全部指向 `/tmp` 或特定构建目录：
`CLI=/tmp/go2cpp-cli-build/...`、`REPLAY_DIR=/tmp/go2cpp-replay-build`，但支持 `GO2CPP_CLI`/`GO2CPP_REPLAY_DIR` 等环境变量覆盖。

**190 口径**：实测主矩阵 170；加父 park（约 10）与压测（11）约 190。该数字无法由一键入口在当前工作树复现（见 P0-2）。

#### B. 场景测试（`tests/scenarios/`）

| 脚本 | 用例 | 断言数 | 依赖 | 进 ctest |
|---|---|---:|---|---|
| `ctx_cancel_rollback/run_ctx_cancel_rollback.sh` | cancel/force/external 三模式 | **65 PASS（实测）** | `build-doctor-full/go2cpp_ctx_cancel_rollback` + `doctor_client.py` | ❌ |
| `deadlock_routes/run_deadlock_scenario.sh` | 120 client → 死锁 → force-unlock | 19 个显式断言点（循环展开更多） | `build-doctor-full/{go2cpp_deadlock_routes,tools/cli/go2cpp-cli}` | ❌ |
| `full_trace/run_full_trace.sh` | 4+ Fiber 混合、6 视图 | 覆盖率脚本 `verify_coverage.py` | `build-debug-full/go2cpp_full_trace_scenario` + CLI | ❌ |

#### C. `tests/debug/` 与 `tests/replay/`

| 脚本 | 内容 | 依赖 |
|---|---|---|
| `tests/debug/run_subscribe_demo.sh` | SUBSCRIBE 推送 demo | `build-debug/go2cpp_debug_subscribe_demo` |
| `tests/debug/run_category_filter_demo.sh` | 分类过滤 demo | `build-debug-full/category_filter_demo` |
| `tests/debug/run_diagnostic_e2e.sh` | Doctor 端到端（14 个断言点） | `build-diag/go2cpp_diagnostic_demo` |
| `tests/debug/run_resource_history_demo.sh` | resources/locks/channels/history 四视图 + 栈/Fiber 链 | `build-debug-full/` + `build-doctor-full/tools/cli` + g++ |
| `tests/replay/run_resource_replay_demo.sh` | 资源事件录制/回放对比 | `build-debug-full/resource_replay_demo` + `/tmp/go2cpp-replay-work/replay`（默认） |
| `tests/replay/run_history_vs_replay_demo.sh` | history vs replay 对比 | `build-debug-full/` + `build-doctor-full/tools/cli` |

> `tests/debug/*_demo.cpp` 与 `tests/replay/resource_replay_demo.cpp` 不参与 CMake 构建，由脚本用 g++ 现场编译。

#### D. `tools/*`

| 工具 | 测试 | 规模 | 依赖 | 进顶层 ctest |
|---|---|---|---:|---|---|
| migration | `tools/migration/tests/run_tests.sh` | **53 场景（实测 53/53）** | `build-migration/`（独立 cmake） | ❌ |
| cli | `tools/cli/tests/run_all.sh` | **213 断言（实测 213/213）** | `BIN=` 环境变量 | ❌ |
| cli | `tools/cli/tests/integration_e2e.sh` | 四线联调（Debug/迁移/回放/CLI） | `build-integration/`、`build-migration/`、`build-replay/` | ❌ |
| cli | `tools/cli/tests/stress.py` + `fuzz_args.py` | 500 组随机 argv、200 次启动、200 次请求 | 假 DebugServer | ❌ |
| replay | `tools/replay/tests/test_replay_tools.cpp` | 格式 v1/v2、分片、auto_entry | 独立工程 | ⚠️ 仅独立工程 `ctest` |
| replay | `tools/replay/scripts/run_all.sh` | 6 个场景（stress/select/context/values/chunk/fault） | `build_dir` 参数（默认 `/tmp/go2cpp-replay-build`） | ❌ |

### 2.5 未注册 / 孤儿测试文件（12 个）

| 文件 | 行数 | 检查点 | 用途 |
|---|---:|---:|---|
| `tests/concurrency/test_channel_concurrency.cpp` | 682 | 49 | Channel 并发压测（1 万值精确计数、随机 close、FIFO） |
| `tests/concurrency/test_future_audit.cpp` | 462 | 57 | Future 单次终态/多等待者审计 |
| `tests/concurrency/test_worker_lifecycle.cpp` | 599 | 70 | 基础 M/补充 M 回收策略 |
| `tests/concurrency/test_parking_condition_stress.cpp` | 231 | 8 | ParkingCondition 并发压测 |
| `tests/concurrency/test_sync_cancel_stress.cpp` | 166 | 0 | **丢唤醒复现器**（docs 明确"尚未注册 ctest"） |
| `tests/network/test_io_audit.cpp` | 477 | 49 | IOManager HUP/shutdown/cancel 审计 |
| `tests/network/test_hook_behavior.cpp` | 246 | 9 | hook 开关行为（关闭后是否真阻塞） |
| `tests/network/test_hook_fifo.cpp` | 249 | 14 | FIFO/pipe park 行为（docs 声称"已注册"，与实际不符） |
| `tests/debug/resource_history_demo.cpp` | 218 | 0 | 资源历史 demo（仅脚本 g++ 编译） |
| `tests/debug/category_filter_demo.cpp` | 149 | 0 | 分类过滤 demo（仅脚本） |
| `tests/replay/resource_replay_demo.cpp` | 348 | 0 | 资源回放 demo（仅脚本） |
| `tests/enhanced/debug/fixture_parent_park.cpp` | 84 | 0 | 父 park 夹具（仅脚本 g++ 编译，实测失败） |

合计约 **3,900 行**（测试逻辑约 3,100 行，demo/夹具 800 行）。

---

## 3. 覆盖矩阵

状态定义：
- ✅ **已测**：进入 ctest 且断言有效；
- 🟠 **条件已测**：只有特定 `-D` 构建的 ctest 覆盖，默认构建不覆盖；
- 🟡 **仅脚本**：可跑但不在 ctest；
- ⚪ **依赖 /tmp 或手工**：需要手工构建或清理 `/tmp` 后不可复现；
- ❌ **未测**。

| 模块 | 状态 | 入口 / 证据 | 主要缺口 |
|---|---|---|---|
| 调度器 / Fiber / Context / Future / Event | ✅ | `go2cpp_tests` 46 模块 + smoke + high_load | — |
| park（ParkingCondition 基础） | ✅ | `test_parking_condition`（12 检查） | 压测文件未注册 |
| park（并发压测） | 🟡→❌ | `test_parking_condition_stress.cpp` 未编译 | 231 行从未运行 |
| channel（基础 / Select） | ✅ | `test_channel` 167 + `fiber_select_test` | — |
| channel（并发压力） | 🟡→❌ | `test_channel_concurrency.cpp` 未编译 | 682 行、10,000 值计数丢失 |
| PC（ConditionVariable/WaitGroup） | ✅ | `test_sync` 140 + `test_parking_condition` | — |
| mutex 10 类（Mutex/RWMutex/Timed/Recursive/Spin/Futex/PC…） | ✅ | `test_locks` 225 + `test_spin_mutex` 53 + `test_futex_mutex` 20 | — |
| 资源历史（resources/locks/channels/history） | 🟡 | `run_resource_history_demo.sh`，实测 ALL PASS | ctest **0 覆盖** |
| `ctx.cancel` | 🟡 | `run_ctx_cancel_rollback.sh`，实测 65 PASS | `test_diagnostic` 无此命令；无 ctest |
| 回放（G2CRPLY v4） | ✅ + 🟡 | `test_timer_replay`/`test_resource_replay`（ctest）；`tools/replay` 独立 + 6 脚本 | 增强矩阵未进 ctest |
| 迁移（G2CMIG03 CLI） | 🟡 | `tools/migration/run_tests.sh` 53/53 | 独立构建，不在顶层 ctest |
| 迁移（主工程 `GO2CPP_MIGRATION_API` + adopt demo） | ❌ | 仅 `docs/fiber-migration.md` 手工命令 | **无任何自动化** |
| CLI | 🟡 | `tools/cli/tests/run_all.sh` 213 + enhanced cli 31 | 无 ctest；`integration_e2e.sh` 需专用构建 |
| Doctor（status/dump/deadlock-report/force-wake/inject-fiber/mutex-unlock/stats） | 🟠 | `test_diagnostic`（170 检查，仅 `GO2CPP_DIAGNOSTIC=ON`）；实测 11 场景通过 | 默认 `build-work` 不含；`ctx.cancel` 缺失 |
| Debug dump / subscribe / 事件环 | 🟠 | `test_debug_*` 在 ctest（OFF 构建只测只读 dump）；脚本覆盖 socket 路径 | 真实 DebugServer 行为不进 ctest |
| Timer（基础/压测/Debug/回放） | ✅ | `test_timer`、`timer_stress`、`timer_debug`、`timer_replay` | — |
| 网络（HTTP/1、HTTP/2、WS、SSE、hook、IO） | ✅ | 13 个独立 ctest 目标 + `go2cpp_tests` 内 3 模块 | TLS/ALPN 未注册 |
| OS（文件/目录/hook 冲突） | ✅ | 3 个 ctest | — |
| 基础库（compress/encoding/hash/container/bufio/math/path/metrics/log） | ✅ | 12 个 ctest 目标 | — |

### 3.1 构建配置矩阵（关键）

| 构建目录 | DEBUG_DUMP | DIAGNOSTIC | REPLAY | MIGRATION_API | CLI | ctest 数 |
|---|---|---|---|---|---|---:|
| `build-work`（本次 116 验证） | OFF | OFF | OFF | OFF | OFF | 116 |
| `build-debug` | ON | OFF | — | OFF | OFF | 116 |
| `build-debug-full` | ON | OFF | ON | OFF | OFF | 116 |
| `build-diag` | ON | ON | — | OFF | OFF | 116 |
| `build-doctor-full` | ON | ON | OFF | OFF | ON | 116 |
| `build-cli` | OFF | OFF | — | OFF | ON | 0（`BUILD_TESTS=OFF`） |
| `build-replay`（tools/replay 独立） | — | — | ON | — | — | 1 |

**含义**：同一份源码，`build-work` 的 ctest 与 `build-doctor-full` 的 ctest 名称相同、数量相同（116），但实际执行路径不同（OFF 构建裁掉 `#if GO2CPP_DEBUG_DUMP` 段）。审计若只跑默认 ctest，会得到"新能力已测"的错误结论。

---

## 4. 缺口清单（按风险排序）

### P0-1　ctest 全绿 ≠ 新能力被覆盖（默认构建关闭全部新开关）

- **证据**：`build-work/CMakeCache.txt` 中 `GO2CPP_DEBUG_DUMP=OFF`、`GO2CPP_DIAGNOSTIC=OFF`、`GO2CPP_REPLAY=OFF`、`GO2CPP_MIGRATION_API=OFF`。
  `tests/debug/test_debug_dump.cpp:584/615/669/731` 的 socket 端到端段被 `#if GO2CPP_DEBUG_DUMP` 裁掉；`test_diagnostic.cpp` 在 `GO2CPP_DIAGNOSTIC=OFF` 时根本不编译。
  实测 `GO2CPP_TEST_FILTER=debug_dump ./build-work/go2cpp_tests` 只打印 1 条 announce（`debug dump: unified read-only interfaces`），socket 端到端场景不存在。
- **影响**：任何"跑过 ctest 116/116"的结论都不能证明 DebugServer、Doctor、资源历史、迁移 API 可用。
- **复现**：`grep GO2CPP_DEBUG_DUMP build-work/CMakeCache.txt`；`GO2CPP_TEST_FILTER=debug_dump ./build-work/go2cpp_tests`。

### P0-2　一键增强入口不可复现（硬编码 `/tmp`）

- **证据**：`tests/all_enhanced.sh` → `tests/enhanced/run_all.sh:41` 检查 `/tmp/go2cpp-cli-build/tools/cli/go2cpp-cli`；该路径不存在。实跑输出：
  `缺少 CLI 构建产物：/tmp/go2cpp-cli-build/tools/cli/go2cpp-cli`，退出码 2。
  `lib/common.sh:13/18` 的 `CLI`、`REPLAY_DIR` 默认值同样是 `/tmp`。
- **影响**：190 条增强用例无法在干净工作树复现；换机器或清理 `/tmp` 后入口直接失效。
- **缓解事实**：各脚本支持 `GO2CPP_CLI`、`GO2CPP_REPLAY_DIR`、`GO2CPP_DIAG_DEMO` 覆盖，实测 170/170 通过。

### P0-3　12 个测试文件未注册（约 3,100 行测试逻辑从未编译）

- **证据**：CMake 全量扫描（见 2.5）。其中：
  - `test_sync_cancel_stress.cpp`：`docs/AGENT-HANDOFF.md:293`、`codex.md:761` 明确记录"尚未注册 ctest"，是 Fiber cancel+cleanup 丢唤醒的独立复现器；
  - `test_hook_fifo.cpp`：`docs/NEXT-STEPS.md:96` 声称"**已注册**"，但 CMake 中无引用（文档与事实矛盾）；
  - `test_channel_concurrency.cpp`（682 行）、`test_worker_lifecycle.cpp`（599 行）、`test_io_audit.cpp`（477 行）、`test_future_audit.cpp`（462 行）均为高价值并发/IO 审计用例。
- **影响**：这些路径（丢唤醒、M 回收、IO 取消/关闭竞态、Channel 并发）在 ctest 报告中完全不可见。

### P0-4　主工程跨进程迁移 API 无自动化测试

- **证据**：`cmake/Go2CppMigration.cmake` 构建 `go2cpp_fiber_adopt_demo` / `go2cpp_fiber_adopt_demo_alt`，明确注释"不注册 ctest 用例"；全仓库仅有 `docs/fiber-migration.md:793` 的手工命令，无脚本引用（`grep adopt_demo` 只命中文档与构建产物）。
- **影响**：`GO2CPP_MIGRATION_API=ON` 的 dump/restore/selftest 与跨可执行文件拒绝路径没有回归保护；`tools/migration` 的 53 场景测的是独立 CLI（G2CMIG03），不覆盖主工程 API。

### P1-1　三个场景脚本没有 runner，且不在任何总入口

- **证据**：`tests/scenarios/{deadlock_routes,full_trace,ctx_cancel_rollback}` 均未出现在 `run_all.sh`；只能手工执行（`docs/debug-reliability.md:368/372` 给了命令）。
- **风险**：`ctx.cancel`（本轮 HEAD 的核心能力）只有该脚本覆盖，实测 65/65 通过——但它不在 CI/一键入口里。

### P1-2　资源历史仅靠 g++ 直编脚本，ctest 零覆盖

- **证据**：`tests/debug/test_debug_dump.cpp` 无 `history/resources/locks/channels` 关键字；`run_resource_history_demo.sh` 用 g++ 现场编译 `resource_history_demo.cpp` 并链接 `build-debug-full/libgo2cpp_*.so`。
- **实测**：`bash tests/debug/run_resource_history_demo.sh build-debug-full 10` → ALL PASS（12 个 JSON 检查 + 栈/Fiber 链计数）。
- **风险**：依赖两个特定构建目录；无编译失败保护。

### P1-3　`/tmp` 默认路径普遍存在

| 位置 | 默认值 |
|---|---|
| `tests/enhanced/lib/common.sh:13` | `/tmp/go2cpp-cli-build/tools/cli/go2cpp-cli` |
| `tests/enhanced/lib/common.sh:18` | `/tmp/go2cpp-replay-build` |
| `tests/enhanced/run_all.sh:41` | 同上（硬检查） |
| `tools/replay/build.sh:13` | `/tmp/go2cpp-replay-build` |
| `tools/replay/scripts/*.sh` | `/tmp/go2cpp-replay-build`、`/tmp/go2cpp-replay-stuck.g2cr` 等 |
| `tests/replay/run_resource_replay_demo.sh:21` | `/tmp/go2cpp-replay-work/replay` |

### P1-4　`test_parent_park.sh` 实测失败（构建目录过期，脚本不校验）

- **证据**：实跑 `bash tests/enhanced/debug/test_parent_park.sh` → `==== debug: PASS=0 FAIL=1 ==== FAILED: fixture:compile`。
  手工复现 g++ 链接错误：`undefined reference to go2cpp::debug::detail::g_debug_env_ready`、`PrimitiveHintScope`、`RecordResourceEvent` 等。
  根因：脚本写死链接 `build-debug/libgo2cpp_*.so`（该目录时间戳为 10-20 19:04，早于当前头文件所需符号），脚本不做构建新鲜度校验。
- **影响**：同一能力的两套测试，一套 36/36 通过、另一套 0/1 失败，外部无法判断是产品缺陷还是夹具问题。

### P2-1　没有 CI

- **证据**：无 `.github/`、`.gitlab-ci.yml`、`Jenkinsfile`。
- **机会**：ctest 全量 60.83 s，完全适合作为 PR 门禁。

### P2-2　文档过时 / 与事实冲突

| 文档 | 声明 | 实际 |
|---|---|---|
| `docs/testing.md` | 最新记录 2026-10-02（14/14），只覆盖旧 ctest | 未提及 116、新能力、enhanced 入口 |
| `tests/enhanced/defects.md` D1（高） | "CLI 仍只认 G2CMIG02/v2" | `tools/cli/commands.cpp:1320-1332` 已支持 v3；实测 migration 34/34（含 `cli-migrate-*`）通过 |
| `docs/NEXT-STEPS.md:96` | `test_hook_fifo.cpp` "已注册" | CMake 无引用 |

### P2-3　Doctor `ctx.cancel` 无单测

- `test_diagnostic.cpp` 覆盖 status/dump/deadlock-report/force-wake/inject-fiber/mutex-unlock/stats，但 `grep ctx.cancel` 无命中；仅场景脚本覆盖。

### P2-4　新能力无 Sanitizer/Valgrind 记录

- `docs/testing.md` 的 ASan/UBSan/TSan/Memcheck 记录截至 2026-09-29，未覆盖 debug/diagnostic/replay/migration 新代码。

---

## 5. 固化建议

### 阶段 1：让已有测试"可复现"（0.5–1 人日）

1. **改 `tests/enhanced/run_all.sh`**：删除硬编码 `/tmp` 检查，改为解析顺序：
   `GO2CPP_CLI` → `build-cli/tools/cli/go2cpp-cli` → `build-doctor-full/tools/cli/go2cpp-cli`；
   `GO2CPP_REPLAY_DIR` → `build-replay`；`GO2CPP_MIG_DIR` → `build-migration`。
   缺失时打印明确构建命令并以 77（skip）退出，而不是 2。
2. **新增 `tests/run_all_tests.sh`**：串行调用
   `ctest -j` → `enhanced/run_all.sh` → `scenarios/*` → `tools/migration` → `tools/cli` → `tools/replay`；
   任一产品 FAIL 汇总但不中断，harness 失败返回非 0；输出统一 TSV。
3. **注册 6 个高风险孤儿测试**（`worker_lifecycle`、`channel_concurrency`、`future_audit`、`io_audit`、`sync_cancel_stress`、`parking_condition_stress`）：
   每个加 `add_executable` + `add_test`（约 20 行/个），超时 120–300 s。
   预计 ctest 从 116 增至 122。
4. **修 `test_parent_park.sh`**：优先使用 `build-debug-full`（与资源历史一致），并在编译前检查 `.so` 时间戳/`nm` 符号；失败时归为 SKIP 而不是产品 FAIL。

### 阶段 2：把新能力纳入 ctest（1–2 人日）

5. **新增 `build-enhanced` 配置**（`DEBUG_DUMP=ON`、`DIAGNOSTIC=ON`、`REPLAY=ON`、`WITH_CLI=ON`），并注册：
   - `go2cpp_ctx_cancel_rollback` 场景（脚本 + `TIMEOUT 120` + `ENVIRONMENT` 指向构建内 CLI）；
   - `go2cpp_deadlock_routes`（默认 120 client，`--clients 20` 的快跑变体）；
   - `go2cpp_full_trace_scenario`（`DURATION=10`）；
   - 资源历史 demo（脚本包装，`PASS_REGULAR_EXPRESSION "ALL PASS"`）。
6. **补 3 个新测试文件**：
   - `tests/debug/test_resource_history.cpp`：直接调用 DumpResources/DumpLocks/DumpChannels/DumpHistory（不启 socket），断言字段与计数；
   - `tests/debug/test_diagnostic_ctx_cancel.cpp`：覆盖 `ctx.cancel` 的 gid/address 拒绝路径与撤销路径；
   - `tests/migration/test_fiber_adopt.cpp` 或 `tools/migration/tests/adopt_roundtrip.sh`：dump→restore→selftest + 跨二进制拒绝。
7. **把 `tools/replay` 单测挂到顶层 ctest**（用 `add_subdirectory` 或复制 4 个源文件），消除"独立工程 ctest 不算数"的问题。

### 阶段 3：CI 与文档（0.5–1 人日）

8. **CI 两条 job**：
   - `default`：`cmake -B build-work ...` + `ctest`（60 s）；
   - `enhanced`：`-DGO2CPP_DEBUG_DUMP=ON -DGO2CPP_DIAGNOSTIC=ON -DGO2CPP_REPLAY=ON -DGO2CPP_WITH_CLI=ON` + `ctest` + `tests/run_all_tests.sh`。
9. **文档**：在 `docs/testing.md` 增加「新能力测试入口」章节（构建矩阵、脚本清单、已知 `/tmp` 依赖）；修正 `defects.md` D1 与 `NEXT-STEPS.md:96` 的过时声明。

**总工作量估算**：2–3 人日（不含 CI 运行环境搭建）。

---

## 6. 实跑验证记录

环境：WSL2 Ubuntu（`docs/testing.md` 环境表记录 GCC 13.3 / CMake 3.28），Release 构建；时间 2026-10-21。

| # | 测试 | 命令（摘要） | 结果 | 耗时 |
|---|---|---|---|---:|
| 1 | ctest 全量 | `ctest --test-dir build-work --output-on-failure` | **116/116 通过** | 60.83 s |
| 2 | ctest 单过滤 | `ctest -R go2cpp_known_filter_runs` | 1/1 通过 | 0.08 s |
| 3 | enhanced Debug | `GO2CPP_CLI=build-cli/... test_debug.sh` | **36 PASS / 0 FAIL** | ~8 s |
| 4 | enhanced Migration | `... test_migration.sh` | **34 PASS / 0 FAIL** | ~6 s |
| 5 | enhanced Replay | `GO2CPP_REPLAY_DIR=build-replay ...` | **34 PASS / 0 FAIL** | ~10 s |
| 6 | enhanced CLI | `... test_cli.sh` | **31 PASS / 0 FAIL** | ~12 s |
| 7 | enhanced Doctor | `GO2CPP_DIAG_DEMO=build-diag/...` | **35 PASS / 0 FAIL** | ~15 s |
| 8 | tools/migration | `run_tests.sh build-migration` | **53/53 通过** | ~5 s |
| 9 | tools/cli | `BIN=build-cli/... run_all.sh` | **213 PASS / 0 FAIL** | ~25 s |
| 10 | tools/replay | `build-replay/go2cpp_replay_tests` + `scripts/run_all.sh build-replay` | 单测通过；**6 场景通过** | ~15 s |
| 11 | ctx.cancel 场景 | `run_ctx_cancel_rollback.sh` | **PASS=65 FAIL=0** | ~12 s |
| 12 | 资源历史 | `run_resource_history_demo.sh build-debug-full 10` | **ALL PASS**（12 检查） | ~13 s |
| 13 | Doctor 单测 | `GO2CPP_TEST_FILTER=diagnostic ./go2cpp_tests`（build-doctor-full） | **11 场景通过** | ~2 s |
| 14 | CLI 压测 | `stress_cli.sh` | **4 PASS / 0 FAIL**（1000 组随机） | ~5 s |
| 15 | 父 park | `test_parent_park.sh` | **FAIL：fixture:compile**（见 P1-4） | ~3 s |
| 16 | 一键入口 | `bash tests/all_enhanced.sh` | **exit 2**（缺 `/tmp` CLI） | <1 s |

### 6.1 关键输出摘录

```
# 1) ctest 全量
100% tests passed, 0 tests failed out of 116
Total Test time (real) =  60.83 sec

# 8) migration
通过 53 项，失败 0 项

# 9) CLI
==== 汇总 ====
PASS=213 FAIL=0

# 11) ctx.cancel
PASS=65 FAIL=0 logs=/tmp/go2cpp-ctx-cancel-...

# 12) 资源历史
history.mutex.events = 73
history.mutex.with_stack = 49 with_chain = 51
== 结果 ==
ALL PASS

# 15) 父 park
==== debug: PASS=0 FAIL=1 SKIP=0 GAP=0 ====
FAILED: fixture:compile
```

---

## 7. 附录：证据位置

| 结论 | 证据 |
|---|---|
| 116 条 ctest 组成 | `cmake/Go2CppTests.cmake`（43 个 `add_test` + examples 循环 75） |
| 默认构建关闭新能力 | `build-work/CMakeCache.txt` 四个 `GO2CPP_*=OFF` |
| Debug 测试条件编译 | `tests/debug/test_debug_dump.cpp:584/615/669/731` |
| 孤儿测试 | CMake 引用扫描；本报告 2.5 表 |
| `sync_cancel_stress` 未注册 | `docs/AGENT-HANDOFF.md:293`、`codex.md:761` |
| `hook_fifo` 声明冲突 | `docs/NEXT-STEPS.md:96` vs CMake 无引用 |
| CLI 已支持 v3 | `tools/cli/commands.cpp:1320-1332`；实测 migration 34/34 |
| 一键入口 `/tmp` 硬编码 | `tests/enhanced/run_all.sh:41`、`lib/common.sh:13/18` |
| 迁移 API 无测试 | `cmake/Go2CppMigration.cmake`（注释"不注册 ctest"）、`docs/fiber-migration.md:793` |
| 父 park 编译失败 | 第 6 节 #15；链接错误 `g_debug_env_ready` 等 |
| 无 CI | 仓库根目录无 `.github/`、`.gitlab-ci.yml`、`Jenkinsfile` |

---

*本报告只读审计，未修改任何代码、测试或既有文档。*

---

## 8. 2026-10-21 固化进展

> **【历史】本节记录 2026-10-21 早间的固化进展（ctest 123 / 增强 170 口径）。其中「7 个高风险孤儿含 hook_fifo、hook_behavior」的说法已被第 9 节纠正：这两个测试实际上并未注册。本节数字以第 9 节为准，正文保留。**

> 本节追加于 2026-10-21，记录审计之后同日落地的 CI 与文档修复。第 1–7 节保持
> 审计时（HEAD `03af552`）的快照，不回改。

### 8.1 已完成

| 项 | 状态 | 位置 / 证据 |
|---|---|---|
| CI 工作流 | **已新增**：`default`（cmake + ctest）与 `enhanced`（DEBUG_DUMP/DIAGNOSTIC/REPLAY/MIGRATION_API/CLI 全开 + 增强矩阵 + 3 个关键场景 + tools 三套） | `.github/workflows/ci.yml`；`python3` + `yaml.safe_load` 解析通过（2026-10-21） |
| `docs/testing.md` | **已更新**：新增「现状与 CI（2026-10-21）」，含 ctest 116 / enhanced 170 / 场景 / tools 结果、一键入口、default/enhanced 构建矩阵与 CI 说明 | `docs/testing.md` 文末 |
| defects.md D1 | **已修正**：标题与状态改为「已修复（2026-10-20）：CLI 已支持 G2CMIG03/v3」，保留修复前历史记录 | `tests/enhanced/defects.md` D1 |
| `NEXT-STEPS.md:96` | **已修正**：`test_hook_fifo.cpp` 由「已注册」改为「未注册（2026-10-21 审计确认）」 | `docs/NEXT-STEPS.md:96-98` |
| 孤儿测试注册（P0-3，部分） | **已注册 7 个**：`channel_concurrency`、`worker_lifecycle`、`future_audit`、`sync_cancel_stress`、`io_audit`、`hook_fifo`、`hook_behavior`（hook 模块条件注册） | `cmake/Go2CppTests.cmake`（2026-10-21）；重配置 `build-work` 后 `ctest -N` = **123**（实测） |
| 父 park 夹具（P1-4） | **已修复**：按 `build-enhanced → build-debug-full → build-debug` 选构建，缺符号/过期时记 SKIP 而非产品 FAIL | `tests/enhanced/debug/test_parent_park.sh`（2026-10-21 并行修复） |

### 8.2 待补 / 未在本轮范围内

- **孤儿测试注册（P0-3，部分完成）**：2026-10-21 并行修复已注册 7 个高风险文件
  （`channel_concurrency`、`worker_lifecycle`、`future_audit`、`sync_cancel_stress`、
  `io_audit`、`hook_fifo`、`hook_behavior`）；`build-work` 重配置后 `ctest -N`
  由 **116 增至 123**（实测数量，新增 7 条尚未全量回归）。第 2.5 节仍列 12 个文件，
  其中 5 个未注册：`test_parking_condition_stress.cpp`、`resource_history_demo.cpp`、
  `category_filter_demo.cpp`、`resource_replay_demo.cpp`、`fixture_parent_park.cpp`
  （后 4 个为 demo/夹具，由脚本 g++ 现场编译）。
- **`tests/run_all_tests.sh`（P0-2 建议的统一入口）**：当前工作树仍无此文件；
  落地后需同步 `docs/testing.md` 的「一键入口」表。
- **`tests/enhanced/run_all.sh` 的 `/tmp` 硬检查（P0-2）**：已由 2026-10-21
  并行修复落地——`lib/common.sh`/`run_all.sh` 改为「环境变量 → `build-enhanced` →
  各能力专项目录」探测，缺 CLI 时 exit 77；CI 仍逐脚本调用五主矩阵，以便把
  父 park（P1-4）排除在门禁之外。
- **父 park 夹具（P1-4）**：已由并行修复改为「挑构建目录 + 过期记 SKIP」，
  但仍未纳入 CI 门禁（需要 `GO2CPP_DEBUG_DUMP=ON` 的夹具编译）。
- **Sanitizer / Valgrind 新能力覆盖（P2-4）**：仍待补。

### 8.3 复现 CI 的本地命令

见 `docs/testing.md`「构建矩阵」与「CI」两节；核心是 `default` 构建
`build-work`、`enhanced` 构建 `build-enhanced`、两个独立工具工程
`build-migration`/`build-replay`，再用 `GO2CPP_*` 环境变量指向产物执行增强矩阵。

## 9. 2026-10-21 20:00 复核（当前状态）

> 本节是审计之后同日 20:00 的复核记录，数据全部为**实测**（工作树 `/UserData/CodexWorkSpace/Go2Cpp-work`，
> HEAD `18dd4df`，WSL2 Ubuntu / GCC 13.3 / CMake 3.28.3）。第 1–8 节保留为历史快照。

### 9.1 默认构建 ctest：130 条（实测 130/130 通过）

```text
$ ctest --test-dir build-work -N | tail -2
Total Tests: 130

$ ctest --test-dir build-work --output-on-failure --timeout 600 -j4 | tail -3
100% tests passed, 0 tests failed out of 130
Total Test time (real) =  26.16 sec
```

按性质分组（编号即 `ctest -N` 序号，可逐条对账）：

| 分组 | 条数 | 序号 | 说明 |
|---|---:|---|---|
| 并发原语（本轮新增） | 4 | #1–#4 | `primitives_stress`、`primitives_cancel`、`park_wake_pair`、`resource_history_stress` |
| `go2cpp_tests` 家族 | 4 | #5–#8 | 主二进制 + 未知/已知 filter 变体 + `fiber_growable_stack` |
| 调度 / 计时 / 状态机 | 5 | #9–#13 | smoke、high_load、timer_stress、state_machine、queue_latency |
| 网络 / HTTP2 基础 | 4 | #14–#17 | hpack、frame、stream_state、connection |
| OS | 3 | #18–#20 | file、dir、hook 冲突 |
| hook | 4 | #21–#24 | ops、raw、correctness、sleep |
| HTTP2 / fiber transport | 6 | #25–#30 | stress、boundary、state_matrix、desync_repro、fiber_transport、state_matrix |
| 基础库 | 13 | #31–#43 | log_buffer、math、rand、path×2、container、config×2、fiber_select、compress、encoding、hash、metrics |
| 审计孤儿测试（本轮注册） | 6 | #44–#49 | `channel_concurrency`、`worker_lifecycle`、`future_audit`、`sync_cancel_stress`、`io_audit`、`parking_condition_stress` |
| 资源历史单测（本轮新增） | 1 | #50 | `resource_history_test`（230 行，不启 socket） |
| tools 三套（本轮包装） | 3 | #51–#53 | migration / cli / replay，缺构建目录 exit 77 → SKIP |
| bufio | 2 | #54–#55 | test + stress |
| 示例冒烟 | 75 | #56–#130 | `example/**`，只验证退出码 0 |
| **合计** | **130** | | 116（历史）+ 7 + 4 + 3 = 130 |

<details>
<summary>完整 <code>ctest -N</code> 输出（130 条，可折叠）</summary>

```text
Test project /UserData/CodexWorkSpace/Go2Cpp-work/build-work
  Test   #1: go2cpp_primitives_stress
  Test   #2: go2cpp_primitives_cancel
  Test   #3: go2cpp_park_wake_pair
  Test   #4: go2cpp_resource_history_stress
  Test   #5: go2cpp_tests
  Test   #6: go2cpp_unknown_filter_rejected
  Test   #7: go2cpp_known_filter_runs
  Test   #8: go2cpp_fiber_growable_stack
  Test   #9: go2cpp_scheduler_smoke
  Test  #10: go2cpp_high_load_stress
  Test  #11: go2cpp_timer_stress
  Test  #12: go2cpp_state_machine
  Test  #13: go2cpp_scheduler_queue_latency
  Test  #14: go2cpp_hpack_test
  Test  #15: go2cpp_frame_test
  Test  #16: go2cpp_stream_state_test
  Test  #17: go2cpp_connection_test
  Test  #18: go2cpp_os_test
  Test  #19: go2cpp_os_dir_test
  Test  #20: go2cpp_os_hook_conflict_test
  Test  #21: go2cpp_hook_ops_test
  Test  #22: go2cpp_hook_raw_test
  Test  #23: go2cpp_hook_correctness_test
  Test  #24: go2cpp_hook_sleep_test
  Test  #25: go2cpp_fiber_transport_test
  Test  #26: go2cpp_http2_stress
  Test  #27: go2cpp_http2_boundary
  Test  #28: go2cpp_state_matrix
  Test  #29: go2cpp_http2_state_matrix
  Test  #30: go2cpp_http2_state_desync_repro
  Test  #31: go2cpp_log_buffer_test
  Test  #32: go2cpp_math_test
  Test  #33: go2cpp_rand_test
  Test  #34: go2cpp_path_test
  Test  #35: go2cpp_path_filepath_test
  Test  #36: go2cpp_container_test
  Test  #37: go2cpp_config_registry_macro_test
  Test  #38: go2cpp_so_manifest_test
  Test  #39: go2cpp_fiber_select_test
  Test  #40: go2cpp_compress_test
  Test  #41: go2cpp_encoding_test
  Test  #42: go2cpp_hash_test
  Test  #43: go2cpp_metrics_test
  Test  #44: go2cpp_channel_concurrency
  Test  #45: go2cpp_worker_lifecycle
  Test  #46: go2cpp_future_audit
  Test  #47: go2cpp_sync_cancel_stress
  Test  #48: go2cpp_io_audit
  Test  #49: go2cpp_parking_condition_stress
  Test  #50: go2cpp_resource_history_test
  Test  #51: tools-migration
  Test  #52: tools-cli
  Test  #53: tools-replay
  Test  #54: go2cpp_bufio_test
  Test  #55: go2cpp_bufio_stress
  Test  #56: go2cpp_debug_module_demo
  Test  #57: go2cpp_runtime_demo
  Test  #58: go2cpp_runtime_basic_demo
  Test  #59: go2cpp_runtime_full
  Test  #60: go2cpp_fiber_sync_demo
  Test  #61: go2cpp_managed_pipeline_demo
  Test  #62: go2cpp_dynamic_gmp_demo
  Test  #63: go2cpp_dynamic_gmp_demo_new
  Test  #64: go2cpp_dynamic_gmp_full
  Test  #65: go2cpp_mixed_runtime_demo
  Test  #66: go2cpp_beginner_demo
  Test  #67: go2cpp_channel_demo
  Test  #68: go2cpp_channel_full
  Test  #69: go2cpp_context_demo
  Test  #70: go2cpp_context_full
  Test  #71: go2cpp_sync_demo
  Test  #72: go2cpp_sync_full
  Test  #73: go2cpp_future_full
  Test  #74: go2cpp_config_demo
  Test  #75: go2cpp_config_full
  Test  #76: go2cpp_runtime_config_demo
  Test  #77: go2cpp_runtime_config_full
  Test  #78: go2cpp_config_center_demo
  Test  #79: go2cpp_config_center_full
  Test  #80: go2cpp_control_flow_demo
  Test  #81: go2cpp_control_flow_full
  Test  #82: go2cpp_io_wait_many_demo
  Test  #83: go2cpp_network_demo
  Test  #84: go2cpp_websocket_demo
  Test  #85: go2cpp_websocket_full
  Test  #86: go2cpp_sse_demo
  Test  #87: go2cpp_sse_basic_demo
  Test  #88: go2cpp_sse_full
  Test  #89: go2cpp_context_rollback_demo
  Test  #90: go2cpp_log_config_demo
  Test  #91: go2cpp_log_demo
  Test  #92: go2cpp_log_full
  Test  #93: go2cpp_timer_task_demo
  Test  #94: go2cpp_timer_full
  Test  #95: go2cpp_parking_condition_demo
  Test  #96: go2cpp_parking_condition_full
  Test  #97: go2cpp_scheduler_demo
  Test  #98: go2cpp_scheduler_full
  Test  #99: go2cpp_thread_policy_demo
  Test #100: go2cpp_thread_policy_full
  Test #101: go2cpp_fiber_local_demo
  Test #102: go2cpp_fiber_local_full
  Test #103: go2cpp_fiber_stack_limit_demo
  Test #104: go2cpp_fiber_stack_limit_full
  Test #105: go2cpp_future_demo
  Test #106: go2cpp_futex_mutex_demo
  Test #107: go2cpp_fd_manager_demo
  Test #108: go2cpp_http_demo
  Test #109: go2cpp_http_full
  Test #110: go2cpp_fd_manager_full
  Test #111: go2cpp_io_backend_demo
  Test #112: go2cpp_io_backend_basic_demo
  Test #113: go2cpp_io_backend_full
  Test #114: go2cpp_address_bytearray_demo
  Test #115: go2cpp_address_full
  Test #116: go2cpp_bytearray_full
  Test #117: go2cpp_errors_demo
  Test #118: go2cpp_error_full
  Test #119: go2cpp_strings_demo
  Test #120: go2cpp_strings_full
  Test #121: go2cpp_strconv_demo
  Test #122: go2cpp_strconv_full
  Test #123: go2cpp_fmt_demo
  Test #124: go2cpp_fmt_full
  Test #125: go2cpp_streams_demo
  Test #126: go2cpp_streams_full
  Test #127: go2cpp_time_demo
  Test #128: go2cpp_time_full
  Test #129: go2cpp_io_hook_demo
  Test #130: go2cpp_io_hook_full

Total Tests: 130
```

</details>

### 9.2 增强构建 ctest：62 条（实测 62/62 通过）

```text
$ ctest --test-dir build-enhanced -j2 | tail -3
100% tests passed, 0 tests failed out of 62
Total Test time (real) =  77.13 sec
```

62 = 130 − 75（示例）+ 7（仅增强注册）：

| 仅增强构建新增 | 条数 | 说明 |
|---|---:|---|
| `go2cpp_ctx_cancel_test` | 1 | Doctor `ctx.cancel` 单测（`tests/scenarios/ctx_cancel_rollback/` 的对偶单测） |
| 四个场景 | 4 | `scenario-deadlock-routes`、`scenario-full-trace`、`scenario-ctx-cancel`、`scenario-channel-deadlock` |
| `go2cpp_scenario_resource_history` | 1 | 资源历史脚本入 ctest，`PASS_REGULAR_EXPRESSION "ALL PASS"` |
| `go2cpp_migration_adopt_roundtrip` | 1 | 主工程 `GO2CPP_MIGRATION_API` dump→restore→selftest + 跨二进制拒绝 |

> 关键点：`build-enhanced` 关闭了 `GO2CPP_BUILD_EXAMPLES`，所以没有 75 条示例冒烟；
> 场景与 adopt 用例的注册条件见 `cmake/Go2CppTests.cmake:797-861`，默认构建
> （`GO2CPP_BUILD_ENHANCED=OFF`）**不会**改变 130 这个数字。

### 9.3 五能力增强矩阵：183 用例（PASS 182 / SKIP 1 / FAIL 0）

```text
$ bash tests/all_enhanced.sh | tail -5
    TOTAL=183 PASS=182 FAIL=0 SKIP=1 GAP=0
    DEFECTS=4 P2=2
```

| 能力 | 用例 | PASS | SKIP | 组成 |
|---|---:|---:|---:|---|
| Debug | 41 | 40 | 1 | 矩阵 36 + 压测 4 + 父 park 1（SKIP：夹具构建过期） |
| 迁移 | 36 | 36 | 0 | 矩阵 34 + 压测 2 |
| 回放 | 36 | 36 | 0 | 矩阵 34 + 压测 2 |
| CLI | 35 | 35 | 0 | 矩阵 31 + 压测 4 |
| Doctor | 35 | 35 | 0 | 矩阵 35 |
| **合计** | **183** | **182** | **1** | 主矩阵 170 + 压测 12 + 父 park 1 |

> 口径说明：`tests/enhanced/report.py` 按 `MATRIX_FILE` 的行数统计 `total`，
> 所以 183 = 五能力主矩阵 170 + 四个 `stress_*.sh` 的 12 条 PASS + 父 park 的 1 条 SKIP。
> **主矩阵 170 本身不等于 183**，历史文档里「enhanced 170」的表述需要按此更新。

**父 park 的 SKIP 原因**（不是产品 FAIL）：`build-enhanced/libgo2cpp_debug.so` 缺
`UnregisterResource` 符号（构建产物早于当前源码），脚本按新鲜度检查记为 SKIP 并打印重建命令。
重建 `build-enhanced` 后该项会转为 PASS。

### 9.4 本轮新增测试清单（文件级，含规模）

#### A. 审计孤儿转正（7 条，全部进 ctest）

| # | 文件 | 行数 | ctest 名 | 内容 |
|---:|---|---:|---|---|
| 1 | `tests/concurrency/test_channel_concurrency.cpp` | 682 | `go2cpp_channel_concurrency` | 1 万值精确计数、随机 close、FIFO |
| 2 | `tests/concurrency/test_worker_lifecycle.cpp` | 599 | `go2cpp_worker_lifecycle` | 基础 M / 补充 M 回收策略 |
| 3 | `tests/concurrency/test_future_audit.cpp` | 462 | `go2cpp_future_audit` | Future 单次终态 / 多等待者 |
| 4 | `tests/concurrency/test_sync_cancel_stress.cpp` | 166 | `go2cpp_sync_cancel_stress` | Fiber cancel + cleanup 丢唤醒复现器 |
| 5 | `tests/network/test_io_audit.cpp` | 477 | `go2cpp_io_audit` | IOManager HUP/shutdown/cancel |
| 6 | `tests/concurrency/test_parking_condition_stress.cpp` | 231 | `go2cpp_parking_condition_stress` | ParkingCondition 代际字丢唤醒 |
| 7 | `tests/debug/test_resource_history.cpp` | 230 | `go2cpp_resource_history_test` | **新增**：直接调用 resources/locks/channels/history 四视图，不启 socket |

> `test_parking_condition_stress.cpp` 原来只有 `run_*` 没有 `main`，本轮补 `main`（断言逻辑未改）后注册。

#### B. 并发原语测试（4 条，全部进 ctest）

| 文件 | 行数 | ctest 名 | 内容 |
|---|---:|---|---|
| `tests/concurrency/test_primitives_stress.cpp` | 748 | `go2cpp_primitives_stress` | 10 类同步原语并发压测（Mutex/RWMutex/Timed/Recursive/Spin/Futex/PC/WaitGroup/Once/Chan） |
| `tests/concurrency/test_primitives_cancel.cpp` | 477 | `go2cpp_primitives_cancel` | 取消传播 / 中断等待 / 资源回收 |
| `tests/concurrency/test_park_wake_pair.cpp` | 438 | `go2cpp_park_wake_pair` | park/wake 配对与丢唤醒护栏 |
| `tests/debug/test_resource_history_stress.cpp` | 632 | `go2cpp_resource_history_stress` | 资源历史事件环压测 |

#### C. 回放 v5

| 文件 | 行数 | 入口 | 说明 |
|---|---:|---|---|
| `tests/replay/test_resources_v5.cpp` | 665 | `GO2CPP_TEST_FILTER=resources_v5 ./go2cpp_tests` | v5 记录格式单测，已编入 `go2cpp_tests`（`tests/test_main.cpp:125`） |
| `tests/replay/resource_v5_demo.cpp` + `run_resources_v5_demo.sh` | 266 + 31 | `bash tests/replay/run_resources_v5_demo.sh` | v5 端到端 demo（手工脚本，未入 ctest） |

#### D. 场景（4 条，仅增强构建）

| 场景 | 入口 | 断言/校验 | ctest |
|---|---|---|---:|
| `ctx_cancel_rollback` | `run_ctx_cancel_rollback.sh` | 65 PASS（cancel / force / external） | ✅ `scenario-ctx-cancel` |
| `deadlock_routes` | `run_deadlock_scenario.sh` | 120 client → 死锁 → force-unlock，19 个显式断言点 | ✅ `scenario-deadlock-routes` |
| `full_trace` | `run_full_trace.sh` + `verify_coverage.py` | 6 视图覆盖率校验 | ✅ `scenario-full-trace` |
| `channel_deadlock` | `channel_deadlock_demo.cpp`(727) + `run_channel_deadlock.sh`(329) | 互等死锁 + select timeout/multi/default，32 个断言点 | ✅ `scenario-channel-deadlock` |

#### E. bench（手工脚本，未入 ctest）

| 文件 | 行数 | 内容 | 文档 |
|---|---:|---|---|
| `tests/bench/fiber_vs_thread_memory.cpp` + `run_memory_bench.sh` + `summarize_memory_bench.py` | 615 + 210 | RSS/VmSize 三阶段采样（base/created/touched），独立进程 | `docs/memory-benchmark.md` |
| `tests/bench/fiber_vs_thread_switch.cpp` + `run_switch_bench.sh` | 82 + 33 | 无缓冲 channel ping-pong vs mutex+cv ping-pong | `docs/switch-benchmark.md` |

**结论数字**：内存 fiber 60.6 KB/单位（RSS）vs 线程 65.8（−8%）；虚拟 68.5 KB vs 6294 KB（≈92×）；
切换 fiber ~500 ns vs 线程 ~28.6 µs（≈57×，3 轮 466/514/523 ns）。

#### F. tools 三套包装（3 条，进 ctest）

| 文件 | 包装的测试 | ctest 名 | 缺依赖时 |
|---|---|---|---|
| `tests/tools/run_migration.sh` | `tools/migration/tests/run_tests.sh`（53 场景） | `tools-migration` | exit 77 → SKIP |
| `tests/tools/run_cli.sh` | `tools/cli/tests/run_all.sh`（213 断言） | `tools-cli` | exit 77 → SKIP |
| `tests/tools/run_replay.sh` | `tools/replay/scripts/run_all.sh`（6 场景） | `tools-replay` | exit 77 → SKIP |

> 实测三者**都是真跑通过**（1.38 s / 3.70 s / 3.75 s），不是 SKIP。

### 9.5 覆盖矩阵（按模块：状态 / 方式 / 用例数）

状态：✅ 已测且进默认 ctest；🟠 已测但仅增强构建；🟡 仅脚本；⚪ 手工/bench；❌ 未测。

| 模块 | 状态 | 方式 | 用例数（实测/估算） |
|---|---|---|---:|
| 调度器 / Fiber / Context / Future / Event | ✅ | `go2cpp_tests` 49 个 filter 模块 + smoke/high_load | ~1,500 检查点 |
| 并发原语 10 类（Mutex/RWMutex/Timed/Recursive/Spin/Futex/PC/WaitGroup/Once/Chan） | ✅ | `test_locks`、`test_spin_mutex`、`test_futex_mutex` + 4 条新原语测试 | 225 + 53 + 20 + 748 行 |
| Channel（基础 / Select / 并发） | ✅ | `test_channel`、`fiber_select_test`、`channel_concurrency`、`scenario-channel-deadlock` | 167 + 6 + 682 行 |
| park / wake | ✅ | `test_parking_condition`、`parking_condition_stress`、`park_wake_pair` | 12 + 231 + 438 行 |
| 资源历史（resources/locks/channels/history） | ✅ | `resource_history_test`（默认构建）+ `go2cpp_scenario_resource_history`（增强） | 230 行 + 12 项 JSON 检查 |
| `ctx.cancel` | 🟠 | `go2cpp_ctx_cancel_test` + 场景 65 PASS | 仅增强构建 |
| 回放（v4 / v5） | ✅ | `test_timer_replay`、`test_resource_replay`、`resources_v5` filter | 3 模块 + 665 行 |
| 迁移（G2CMIG03 CLI 53 场景） | ✅ | `tools-migration`（ctest 包装） | 53 |
| 迁移（主工程 `GO2CPP_MIGRATION_API`） | 🟠 | `go2cpp_migration_adopt_roundtrip` | 仅增强构建，1 条 |
| CLI | ✅ | `tools-cli` + 31 矩阵 + 4 压测 | 213 + 31 + 4 |
| Doctor（status/dump/deadlock-report/force-wake/inject-fiber/mutex-unlock/stats/ctx.cancel） | 🟠 | `test_diagnostic`（170 检查）+ 35 矩阵 + `ctx_cancel_test` | 仅增强构建 |
| Debug dump / subscribe / 事件环 | 🟠 | `debug_*` 10 模块 + 36 矩阵 + 4 压测 | 仅增强构建生效 |
| Timer（基础 / 压测 / Debug / 回放） | ✅ | `test_timer`、`timer_stress`、`timer_debug`、`timer_replay` | 55 + 17 |
| 网络（HTTP/1、HTTP/2、WS、SSE、hook、IO） | ✅ | 13 个独立 ctest 目标 + `go2cpp_tests` 内模块 | 17 条 ctest |
| OS / 基础库（compress/encoding/hash/container/bufio/math/path/metrics/log/config） | ✅ | 16 条 ctest | 16 |
| 示例 | ✅ | 75 条冒烟（只验退出码） | 75 |
| bench（内存 / 切换） | ⚪ | `tests/bench/run_*.sh` | 2 个基准，未入 CI |
| Sanitizer / Valgrind（新能力） | ❌ | — | 0，记录仍停在 2026-09-29 |
| `hook_fifo` / `hook_behavior` | ❌ | 未注册（见 9.6） | 0 |

### 9.6 缺口（更新，按优先级）

#### P1　`events:包含事件行` 是 flaky 用例（本轮新发现）

- **现象**：单独跑 `bash tests/enhanced/debug/test_debug.sh` **稳定 35 PASS / 1 FAIL**，
  失败项 `FAILED: events:包含事件行`（断言 `events` 视图含 `kRunQueue` 行）；
  但在 `tests/all_enhanced.sh` 全量运行中同一用例 **PASS**（矩阵总计 FAIL=0）。
- **复现**：干净环境下 `bash tests/enhanced/debug/test_debug.sh`（20:00 复跑 2/2 失败）。
- **判断**：该断言依赖「事件环里恰好已有 runqueue 事件」，与调度时序/系统负载相关，
  属**测试自身不稳定**，不是产品回归。建议改为「先制造一次调度事件再断言」，或对
  `kRunQueue` 缺失记 GAP 而非 FAIL。

#### P1　两个 hook 测试仍无法注册（有明确原因，不应强行注册）

- `tests/network/test_hook_fifo.cpp`：场景 3 用 raw write 在主线程写满 `F_SETPIPE_SZ=4096`
  的管道且无读者，`write_f` 永久阻塞、测试自身死锁（实测 timeout 300 s）。
  需改成**非阻塞填充**后才能注册。
- `tests/network/test_hook_behavior.cpp`：场景 2 断言「kSleep 关闭后心跳 < 5」，实测心跳 39——
  与文件头注释记录的已知行为一致（`SetOpEnabled` 只改标志位，实际 sleep 仍走 park 路径）。
  这是**被测试记录的产品缺陷**，不能改断言凑通过，故不注册。
- 证据：`cmake/Go2CppTests.cmake:887-894`；`build-work`/`build-enhanced` 的
  `CTestTestfile.cmake` 对二者 0 命中。

#### P1　父 park 夹具 SKIP：`build-enhanced/libgo2cpp_debug.so` 构建过期

- 缺 `UnregisterResource` 符号，`test_parent_park.sh` 按新鲜度检查记 SKIP（不再像历史 P1-4 那样误报产品 FAIL）。
- **待做**：重建 `build-enhanced` 后应转 PASS；CI 里应把「夹具 SKIP」当 warning 而非通过。

#### P2　回放 v5 缺独立随机/边界矩阵

- 现状：`resources_v5` 作为 `go2cpp_tests` 的一个 filter，断言集中在格式与恢复语义；
  没有像 v4 那样的多 seed 随机交错（v4 侧有 `fuzz:500 轮随机事件交错`）。
- **待做**：补 `GO2CPP_TEST_FILTER=resources_v5` 的随机种子矩阵，至少 3 个 seed × 500 轮。
> 另注（20:03 观察）：工作树出现未跟踪的 v5 扩展文件 （`test_cv_replay.cpp`、`test_io_replay.cpp`、`test_rwmutex_replay.cpp`、`test_waitgroup_replay.cpp`、`test_mixed_v5_replay.cpp`、`replay_v5_stress_main.cpp`），尚未被 `cmake/Go2CppTests.cmake` 引用，因此不影响本报告的 `ctest -N` = 130；若后续注册，基线需同步上调。

#### P2　bench 未入 CI、无回归阈值

- `tests/bench` 两个基准都是手工脚本（`grep tests/bench cmake/` 无命中），结果写
  `tests/bench/results/`，没有阈值判定。
- **待做**：CI 加 nightly job，跑 `QUICK=1` 矩阵并与基线比较（如 RSS 退化 >20% 告警）。

#### P2　新能力仍无 Sanitizer / Valgrind 记录

- `docs/testing.md` 的 ASan/UBSan/TSan/Memcheck 记录停在 2026-09-29，未覆盖
  debug / diagnostic / replay / migration / bench 新代码。

#### P2　CI 尚未实跑

- `.github/workflows/ci.yml` 已存在且 YAML 可解析，但本轮只有本地实跑，
  没有远端 CI 运行记录；`default`（130）与 `enhanced`（62 + 矩阵 + 场景 + tools）两条 job 待首次跑通。

### 9.7 固化建议：已完成 / 待做

| 项 | 状态 | 证据 |
|---|---|---|
| ctest 116 → 130（+7 审计 +4 原语 +3 tools） | ✅ 已完成 | 9.1；`ctest -N` 130 |
| 4 个场景全部入 ctest（仅增强构建） | ✅ 已完成 | 9.2；`scenario-*` 4/4 通过 |
| tools 三套挂进 ctest（缺依赖 SKIP 而非 FAIL） | ✅ 已完成 | 9.4F；3/3 真跑通过 |
| 7 条审计孤儿/新增单测注册 | ✅ 已完成 | 9.4A |
| 资源历史从「仅 g++ 脚本」变为「ctest 单测 + 场景」 | ✅ 已完成 | `resource_history_test` + `go2cpp_scenario_resource_history` |
| 父 park 从 FAIL 改为 SKIP（不误报） | ✅ 已完成 | 9.3；`PASS=0 FAIL=0 SKIP=1` |
| `tests/run_all_tests.sh` 统一入口 | ✅ 已完成 | `docs/testing.md`「一键入口」 |
| 修 `events:包含事件行` flaky | ⏳ 待做 | 9.6 P1 |
| `hook_fifo` 改非阻塞填充后注册 | ⏳ 待做 | 9.6 P1 |
| v5 随机种子矩阵 | ⏳ 待做 | 9.6 P2 |
| bench 入 CI + 阈值 | ⏳ 待做 | 9.6 P2 |
| 新能力 ASan/UBSan/TSan/Valgrind | ⏳ 待做 | 9.6 P2 |
| CI 首次实跑 | ⏳ 待做 | 9.6 P2 |
| 重建 `build-enhanced` 让父 park 转 PASS | ⏳ 待做 | 9.6 P1 |

### 9.8 复现命令（当前状态）

```sh
cd /UserData/CodexWorkSpace/Go2Cpp-work

# 默认：130/130
ctest --test-dir build-work --output-on-failure --timeout 600 -j4

# 增强：62/62（含 4 场景 + adopt + ctx_cancel）
ctest --test-dir build-enhanced --output-on-failure --timeout 900 -j2
ctest --test-dir build-enhanced -R '^scenario-' -V

# 增强矩阵：TOTAL=183 PASS=182 SKIP=1
bash tests/all_enhanced.sh

# tools 三套（单独看是否 SKIP）
ctest --test-dir build-work -R '^tools-' -V

# bench（手工，约 1 分钟）
QUICK=1 bash tests/bench/run_memory_bench.sh
bash tests/bench/run_switch_bench.sh

# 回放 v5 单测
GO2CPP_TEST_FILTER=resources_v5 ./build-work/go2cpp_tests

# 已知 flaky：单独跑 debug 矩阵（预期 35 PASS / 1 FAIL）
bash tests/enhanced/debug/test_debug.sh
```

---

*第 9 节为本轮复核追加；第 0–8 节保持 2026-10-21 早间原样，作为历史对照。*
