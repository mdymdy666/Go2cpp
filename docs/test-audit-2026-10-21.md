# Go2Cpp 测试审计报告（2026-10-21）

> 审计对象：`/UserData/CodexWorkSpace/Go2Cpp-work`（git HEAD `03af552`）
> 审计方式：全量静态扫描（CMake、测试源码、脚本）+ 16 项实跑验证（含 2 项失败/阻断）
> 边界：只读代码，仅新增本文件与 `/tmp/test_audit.md`

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
