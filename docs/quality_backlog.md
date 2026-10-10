# Go2Cpp 质量台账（文档 / 测试 / 示例）

> 建立于第 0 轮质量审计（只读轮，未改动任何既有文件）。
> **证据通道**：全部经 `wsl bash -lc` 或 `\\wsl.localhost\Ubuntu\...` 从真身
> `/UserData/CodexWorkSpace/Go2Cpp` 读取——内置 `read`/`grep` 工具会解析到
> `E:\UserData\CodexWorkSpace\Go2Cpp` 旧副本，结论不可用。
> **配套**：执行规则见 `.agent/quality_task_rules.md`；覆盖矩阵见对话交付与
> 本文件第 4 节摘要。

## 1. 问题台账

| ID | 类别 | 模块 | 问题 | 证据 | 严重度 | 归属批次 | 状态 | 备注 |
|---|---|---|---|---|---|---|---|---|
| I-01 | 不一致 | fiber / 文档 | Fiber 上下文后端表述互相矛盾：一处称已用自有汇编、不再链接 Boost.Context，另一处称"当前使用 Boost.Context 受保护固定栈"，第三处称自研汇编"已撤回" | `docs/design.md:121-130`（自有 ABI）vs `docs/design.md:221`（Boost.Context）；`docs/boundary_audit.md:25`（承认 native context）vs `:30`（称 Boost.Context）；`docs/performance.md:190-193`（已撤回）vs `:204-208`（已上线）；`docs/go_reference_audit.md:135` | 高 | B-02 | 待办 | **源码事实已定**：真身构建实际编译 `src/concurrency/context_x64.S` 并定义 `GO2CPP_USE_NATIVE_CONTEXT=1`（`cmake/Go2CppModules.cmake:33-36`；第 0 轮实测产出 `context_x64.S.o`）。同文件内 `design.md:121-130` 与 `:221` 自相矛盾 |
| D-01 | 文档 | 跨模块 / README | `README.md` 三处示例路径失效（缺模块子目录前缀） | `README.md:54` → `example/log_config_demo.cpp`（实际 `example/logging/`）；`:97` → `example/beginner_demo.cpp`（实际 `example/concurrency/`）；`:48-52` 列出的示例名同样无前缀 | 高 | B-02 | 待办 | 自动扫描确认这 3 条 target 不存在 |
| D-02 | 文档 | 跨模块 / README | `README.md:48-52` 只登记 6 个示例，实际有 15 个；9 个示例未在 README 登记 | `README.md:48-52` vs `ls example/*/*.cpp`（15 个）；未登记：`context_rollback_demo`、`control_flow_demo`、`io_wait_many_demo`、`network_demo`、`sse_demo`、`timer_task_demo`、`websocket_demo`、`beginner_demo`(仅 :97 提及)、`log_config_demo`(仅 :54 提及) | 中 | B-02 | 待办 | 能力清单不完整，影响新用户发现示例 |
| D-03 | 文档 | 跨模块 / README | 模块 target 清单漏 `go2cpp::future` 与 `go2cpp::network` | `README.md:90-94` vs `cmake/Go2CppModules.cmake:38,43,89,91` | 中 | B-02 | 待办 | 实际共 14 个 target |
| D-04 | 文档 | context | `docs/context_rollback.md` 的示例链接指向不存在的路径 | `docs/context_rollback.md:19` 链接 `../example/context_rollback_demo.cpp`；实际文件在 `example/concurrency/context_rollback_demo.cpp` | 中 | B-02 | 待办 | 目录重构（2026-10-07）后未更新 |
| D-05 | 不一致 | 文档 | `docs/modules.md` 称 `go2cpp_network` 依赖三个 target，漏记 `go2cpp_sync` | `docs/modules.md:11` vs `cmake/Go2CppModules.cmake:155`（依赖 `go2cpp_io go2cpp_context go2cpp_scheduler go2cpp_sync`） | 低 | B-02 | 待办 | `go2cpp_network` 本身确实存在，此处只是依赖描述不全 |
| D-06 | 文档 | 测试文档 | `docs/testing.md` 引用重构前的测试路径，并提到非本仓工作树 | `docs/testing.md:415` → `tests/high_load_stress.cpp`（实际 `tests/concurrency/`）；同处提到 `NewGo2Cpp` 工作树 | 低 | B-02 | 待办 | 历史验证记录，改路径时需保留原始语义 |
| D-07 | 文档 | 测试文档 | `docs/go_reference_audit.md` 引用重构前路径 | `docs/go_reference_audit.md:106` → `tests/test_control_flow.cpp`（实际 `tests/tools/`） | 低 | B-02 | 待办 | — |
| D-08 | 文档 | codex.md | 历史条目中 15 条路径引用已因目录重构失效 | `codex.md:60,103,126,145,231,232,633,797` 等（`example/log_config_demo.cpp`、`tests/test_io.cpp`、`src/io.cpp`、`src/error.cpp` 等扁平旧路径） | 低 | B-02 | 待办 | **属历史记录**，宜加"路径为当时结构"的说明，而非改写历史 |
| D-09 | 文档 | context | 文档示例统一用小写下划线 API，而头文件同时提供 PascalCase，文档未说明二者并存 | `docs/context_rollback.md:9,12,34-49` 用 `record_undo`/`savepoint`/`rollback_to`/`rollback_done`/`had_failure`；`include/go2cpp/concurrency/context.hpp:308-346` 两套命名均有（如 `record_undo` :308 与 `RecordUndo`） | 低 | B-03 | 待办 | 易让读者以为只有一种拼写 |
| T-01 | 测试 | context | `test_context.cpp` 有 144 个 `GO2CPP_CHECK` 但 **0** 个 `REQUIRE`/`REQUIRE_EVENTUALLY`，断言失败只计数、异常路径无中止保护 | `tests/concurrency/test_context.cpp`（统计：CHECK=144, REQUIRE=0, REQUIRE_EVENTUALLY=0）；对比 `tests/test_support.hpp:140-150` 的宏语义 | 高 | B-03 | 待办 | 该模块含取消/deadline 等并发路径，缺少超时保护易退化为挂起 |
| T-02 | 测试 | tools | `test_control_flow.cpp`(39×CHECK) 与 `test_error.cpp`(27×CHECK) 无任何看门狗宏 | `tests/tools/test_control_flow.cpp`、`tests/tools/test_error.cpp`（REQUIRE=0, REQUIRE_EVENTUALLY=0, JOIN=0） | 中 | B-03 | 待办 | 纯同步逻辑，风险低于 T-01，但错误链深度用例可加超时 |
| T-03 | 测试 | hybrid_mutex | `HybridGate`/`HybridMutex` 全仓无测试引用 | `grep -rn 'HybridGate\|HybridMutex\|hybrid_mutex' tests/ example/` → 无结果；实现见 `include/go2cpp/concurrency/core/hybrid_mutex.hpp:23-113` | 高 | B-04 | 待办 | 该门保护 sync/io 的唤醒临界区，属并发核心设施 |
| T-04 | 测试 | fd_manager | `FdManager` 全仓无测试引用 | `grep -rn 'FdManager' tests/` → 无结果；实现见 `src/network/fd_manager.cpp`、`include/go2cpp/network/fd_manager.hpp:42-60` | 中 | B-05 | 待办 | `DescriptorGuard` 已被 `test_io.cpp:86,633,652,663` 间接覆盖，但 FdManager 本体未测 |
| T-05 | 测试 | error（在途改动） | 新增的 `errors::*` 命名空间无测试、无示例 | `grep -rn 'errors::New\|errors::Wrap\|errors::Is\|errors::Join' tests/ example/` → 无结果；定义见 `include/go2cpp/tools/error.hpp:204-246`（用户在途改动） | 中 | 需用户决策 | 需用户决策 | 先确认该 API 是否保留，再决定补测试或回退 |
| T-06 | 测试 | event / go | `event.hpp`（`EventBatch`/`SelectLoop`）与 `go.hpp`（默认调度器）无独立 `run_xxx_tests()` | `tests/test_main.cpp:7-22` 无 event/go 条目；`SelectLoop` 仅在 `tests/concurrency/test_beginner_api.cpp:35-138` 覆盖；`go2cpp::go(` 在 tests 中无命中 | 低 | B-03 | 待办 | 功能已覆盖，缺独立入口便于定向回归 |
| T-07 | 测试 | parking_condition / thread_policy / fiber_local | 三个模块无独立测试类，仅间接覆盖 | `parking_condition` 仅 `test_future.cpp` 提及；`thread_policy` 仅 `test_scheduler.cpp`/`test_hook.cpp`；`fiber_local` 仅 `test_fiber.cpp:120,283,386,407,426` | 低 | B-04 | 待办 | 可接受，视批次容量决定是否独立 |
| E-01 | 示例 | network | `network_demo.cpp` 仅 20 行、只有成功路径，缺等待/取消/生命周期与错误路径；且未出现在 `example/README.md` 速查表 | `example/network/network_demo.cpp:6-19`（无 Scheduler、无超时、无错误分支）；`example/README.md:36-48` 速查表无 `network_demo`（对比 `cmake/Go2CppTests.cmake:65` 已构建） | 中 | B-01 | **已完成** | B-01 重写为 157 行、五场景（粘包 / 半包 / 超长帧 / 分片 / 非法请求）；速查表与批量列表均已登记；commit `8ff0e6f` |
| E-02 | 示例 | future / io_backend / fd_manager / address / byte_array / parking_condition / hybrid_mutex | 七个模块无任何示例 | 交叉检索 `example/` 无命中；对照 `example/README.md` 速查表 11 行 | 中 | B-04 / B-05 | 待办 | 按批次拆分：future/parking_condition/hybrid_mutex 归 B-04；io_backend/fd_manager/address/byte_array 归 B-05 |
| E-03 | 示例 | 跨模块 | `example/README.md:1-15` 的推荐阅读顺序只列 5 个示例，与速查表 11 行不一致 | `example/README.md:5-10` vs `:38-48` | 低 | B-02 | **已完成** | 已在 B-01 处理：推荐顺序后新增「上面只是入门主线，完整示例清单与场景说明见下方『示例场景速查』表」；commit `79d7b71` |
| D-10 | 文档 | 跨模块 | 项目**无** `AGENTS.md`/`CLAUDE.md`/`.agent/` 目录，协作规范此前只存在于 `codex.md` | `ls -a`（仅 `codex.md`）；`.agent` 不存在 | 低 | 本轮已处理 | 已完成 | 本轮新建 `.agent/quality_task_rules.md`，并在 `codex.md` 记录 |
| E-04 | 示例 | 跨模块 / network | `sse_demo`、`websocket_demo`、`timer_task_demo` 仍未登记进 `example/README.md` 速查表；「批量运行全部示例」的 for 列表在 B-01 后仍只有 12 个（实际 15 个示例） | `example/README.md` 速查表现 12 行 vs `ls example/*/*.cpp`（15 个）；`cmake/Go2CppTests.cmake:56-73` | 中 | B-02 | 待办 | B-01 只登记了 `network_demo`，其余三个留待 B-02 统一处理 |
| D-11 | 文档 | 跨模块 | `docs/modules.md` 作为"模块索引"过于简短（13 行），未覆盖 `include/go2cpp/core`、`detail` 子目录与新模块 target 全貌 | `docs/modules.md:5-13` vs `ls -R include/go2cpp` | 低 | B-02 | 待办 | 与本轮交付的四方矩阵可互补 |

## 2. 待用户决策项

| 编号 | 问题 | 为什么需要用户拍板 | 默认建议（若用户不回） |
|---|---|---|---|
| U-01 | 在途改动新增的 `errors::` 命名空间（`include/go2cpp/tools/error.hpp:204-246`）是否保留？ | 它决定了后续要不要为它补测试与示例（T-05）。未经确认不应替用户决定 API 取舍 | 保留，并在 B-03 批次补最小测试与示例 |
| U-02 | `docs/performance.md` 中引用的 coost 基准路径（`/mnt/e/CodexWorkspace/Go2Cpp/coost`、`/mnt/e/CodexWorkspace/NewGo2Cpp`）是否视为有效复现路径？ | 这些路径位于禁止访问的副本区，本轮未探查；若属于"另一份独立副本"，文档中的复现说明需要重写 | 视为**已失效**，在 B-02 中改写为仓内可复现方式或标注"需外部 checkout" |
| U-03 | `codex.md` 历史条目中的失效路径（D-08，15 处）如何处理？ | 涉及"是否改写历史记录"的取舍，规则上倾向保留历史原貌 | 保留原文，仅在文件顶部快照区加一句"历史条目中的路径反映当时目录结构" |
| U-04 | 是否要求 `ctest` 在提交前必须全绿（含示例）？ | 示例 `go2cpp_dynamic_gmp_demo` 存在间歇失败（见 §3 疑似缺陷 X-01），全绿要求会阻塞批次 | 要求测试可执行体全绿；示例间歇项先按 X-01 修复后再纳入门禁 |

## 3. 疑似代码缺陷（本轮不修，仅登记）

| 编号 | 模块 | 现象 | 证据 | 备注 |
|---|---|---|---|---|
| X-01 | 示例 / scheduler | `go2cpp_dynamic_gmp_demo` **间歇性失败**：`cancel()` 在任务已进入 Running 时返回 false，而示例把它当作硬失败条件 | `example/concurrency/dynamic_gmp_demo.cpp:99`（`cancel()` 返回值）、`:104-105`（`cancel_requested && state()==kCancelled` 同时成立才行）、`:114-115`（`cancel_ok` 计入返回码）。第 0 轮实测：直接运行 5 次 rc=1,1,0,1,0；`ctest` 单跑 5 次 3 败；全量 2 轮均 18/18 | 属**示例断言过严 + 竞态窗口**，不是调度器崩溃；与 `example/README.md:42` 声称"最终收敛为 Cancelled"矛盾 |
| X-02 | 文档 | `docs/design.md` 同一文件内对 Fiber 后端的描述互相否定 | `docs/design.md:121-130` vs `:221` | 见 I-01 |
| X-03 | 文档 | `docs/boundary_audit.md` 同一文件内自相矛盾 | `docs/boundary_audit.md:25`（native context 有溢出检查）vs `:30`（"当前使用 Boost.Context"） | 见 I-01 |
| X-04 | 文档 | `docs/performance.md` 内部性能结论数量级级矛盾（混合锁"快 10~18 倍" vs "8.5~8.8 s"；IO"慢 3.0~4.3 倍" vs "0.59x 已超过"） | `docs/performance.md:140` vs `:42-46`/`:183`/`:214-216`；`:141` vs `:184`；另 `:5-7` 自称"本节优先"但顶部小节早于后文 | 属历史记录累积冲突，需在 B-02 中加"数据时效与条件"标注 |
| X-05 | 测试 | `tests/concurrency/test_context.cpp` 等以 CHECK 为主、缺超时保护，挂起式失败会拖到 CTest 30s TIMEOUT | 见 T-01/T-02；`cmake/Go2CppTests.cmake:31` TIMEOUT 30 | 属测试健壮性缺陷，非产品缺陷 |

> 说明：本节只登记现象与证据，**本轮不做任何修复**。

## 4. 已核实无问题项（避免后续重复排查）

| 项 | 结论 | 证据 |
|---|---|---|
| `go2cpp_network` CMake target | **存在**且依赖完整 | `cmake/Go2CppModules.cmake:43-50,71,91,108,155`；`git show HEAD:` 同为 177 行且含该 target，工作区无 diff |
| `tests/test_main.cpp` 注册完整性 | 16 个声明 ↔ 16 个实现，一一对应 | `tests/test_main.cpp:7-22` vs 各文件 `void run_*_tests()` |
| `cmake/Go2CppTests.cmake` 测试源列表 | 与磁盘一致（含 `tools/`、`concurrency/`、`network/`、`logging/` 分层） | `cmake/Go2CppTests.cmake:4-21` vs `find tests -name '*.cpp'` |
| `GO2CPP_EXAMPLES` vs 磁盘示例 | 14 + `io_hook_demo`（hook ON）= 15，完全一致 | `cmake/Go2CppTests.cmake:56-73` vs `ls example/*/*.cpp` |
| WebSocket 是否有测试覆盖 | **有**，含 RFC 6455 握手向量与 socketpair 端到端 | `tests/network/test_network.cpp:560-598`（websocket 关键字命中 51 处） |
| `network_demo.cpp` 所用 API | 全部存在（`Framer::delimiter` `framing.hpp:26`、`take_frames` `:104`、`HttpParser` `http.hpp:238`、`HttpRequest` `:235`、`method_name` `:33`、`Request::path()` `http.hpp:67`/`http.cpp:251`） | 逐符号核实 |
| `docs/context_rollback.md` 所用 API | 全部存在（`context.hpp:308,311,317,339,346`） | 逐符号核实 |
| `README.md` 的 markdown 链接 | 0 失效 | 全量正则扫描 |
| `docs/modules.md` 的目录划分 | 与 `src/` 实际一致 | `docs/modules.md:5-9` vs `ls src/` |
| `docs/sse.md:92` 引用的测试路径 | 正确 | `docs/sse.md:92` vs `tests/network/test_sse.cpp` |
| 项目根 `log/` 与 `build/` | 本轮未触碰、未清理 | 轮末 md5 与基线一致 |

## 5. 批次划分与执行顺序

| 批次 | 主题 | 覆盖台账项 | 依赖 | 变更面 |
|---|---|---|---|---|
| **B-00** | 建账与规则（本轮） | D-10 | — | 新增 `.agent/quality_task_rules.md`、`docs/quality_backlog.md`；追加 `codex.md` |
| **B-01** | 示例质量：补齐三路径与登记 | E-01、E-03(network 部分) | B-00 | `example/network/network_demo.cpp`、`example/README.md`、`README.md`(示例段) |
| **B-02** | 文档重构后路径与索引同步 | D-01~D-09、D-11、I-01、X-02~X-04 | B-00 | `README.md`、`docs/modules.md`、`docs/context_rollback.md`、`docs/testing.md`、`docs/go_reference_audit.md`、`docs/design.md`、`docs/boundary_audit.md`、`docs/performance.md`、`codex.md`(仅顶部说明) |
| **B-03** | tools + context：断言健壮性与新 API 覆盖 | T-01、T-02、T-05、T-06、D-09 | U-01 | `tests/concurrency/test_context.cpp`、`tests/tools/*.cpp`、`docs/context_rollback.md`、新增 `tests/concurrency/test_event_go.cpp`(建议) |
| **B-04** | scheduler/fiber/并发核心缺口 | T-03、T-07、E-02(future/parking_condition/hybrid_mutex) | B-03 | 新增 `tests/concurrency/test_hybrid_gate.cpp`、可选 `example/concurrency/future_demo.cpp` |
| **B-05** | network 缺口：fd_manager / io_backend / address / byte_array | T-04、E-02(io_backend/fd_manager/address/byte_array) | B-01 | 新增测试与示例各 1~2 个；`cmake/Go2CppTests.cmake` 同步登记 |
| **B-06** | logging + config 收口与跨模块总入口 | 残余项（`runtime.hpp` 聚合可编译性、`docs/modules.md` 完整索引） | B-02 | `tests/logging/`、`docs/modules.md`、可选新增 `tests/test_runtime_umbrella.cpp` |

**建议执行顺序**：B-01 → B-02 → B-03 → B-04 → B-05 → B-06。

理由：
- **B-01 最先**：改动面最小（1 个示例 + 2 处索引），能立刻消除"示例不达标 + 未登记"的新用户可见问题，且为 B-05 的示例补充建立模板。
- **B-02 紧随**：纯文档、零代码风险，先把失效链接与后端表述冲突消除，
  否则后续所有文档改动都会踩在错误路径上。
- **B-03 → B-05**：按"测试基础设施 → 并发核心 → 网络"递增风险；
  每批都能独立跑 `ctest` 验证，不跨批依赖。
- **B-06 收尾**：跨模块索引与 umbrella 编译性，需前面各批的新文件都已登记。

**批次独立性检查**：B-01/B-02 只碰文档与 1 个示例；B-03~B-05 各自拥有独立测试文件与
CMake 条目；B-06 只做索引汇总。任意一批回滚不影响其他批次的构建与测试结果。

## 6. 本轮（第 0 轮）未覆盖范围

- 未运行构建与 `ctest`（本轮为只读建账轮，仅做静态取证）；
  台账中的"间歇失败"结论引用的是**上一轮**的实测数据，本轮未复现。
- 未运行任何 sanitizer（ASan/UBSan/TSan/Valgrind）。
- 未验证 `docs/testing.md`、`codex.md` 中记录的历史验证数字。
- 未逐行通读大文件：`src/concurrency/scheduler.cpp`(4048)、`io.cpp`(2646)、
  `hook.cpp`(2161)、`context.cpp`(1895)、`concurrency/channel.hpp`(2007) 等。
- 未探查 `/mnt/e/...` 下文档引用的 coost / `NewGo2Cpp` 路径（属禁止区，且见 U-02）。
- 未审计 `third_party/` 内容与 SHA-256；未触碰 `build/`（属主为 root）。
- 未评估 HTTP/WebSocket/SSE 对 RFC 的逐条合规性。
- 未确认项：`WebSocketError::InvalidState` 是否有实际校验分支；
  `include/go2cpp/core/status.hpp` 内容；`tools/event.hpp` 的完整语义。