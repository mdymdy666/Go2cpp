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
| I-01 | 不一致 | fiber / 文档 | Fiber 上下文后端表述互相矛盾：`design.md` 与 `boundary_audit.md` 各自内部自相矛盾，`go_reference_audit.md` 与 `performance.md` 保留已撤回结论 | `docs/design.md:121-130` vs `:221`；`docs/boundary_audit.md:25` vs `:30`；`docs/go_reference_audit.md:135`；`docs/performance.md:190-193` vs `:204-208` | 高 | B-02 | **已完成** | 已按源码事实统一：Linux x86_64 默认自有上下文后端（`cmake/Go2CppModules.cmake:3-7` 平台判定、`:33-36` 编译 `context_x64.S` 并定义 `GO2CPP_USE_NATIVE_CONTEXT=1`、`:143-150` 仅在 OFF 时链接 `Boost::context`；本机实测存在 `context_x64.S.o` 且 `flags.make` 含该宏），其他平台回退 Boost.Context；`performance.md` 历史段加时效标注、原文保留不改写历史；commit `23677d6`；**次轮补漏**：`docs/performance.md:27`、`:63-65`、`:144` 三处残留表述已修正（首轮验证的 `grep -v` 按行过滤误伤了整个文件，见 codex.md 记录） |
| D-01 | 文档 | 跨模块 / README | `README.md` 三处示例路径失效（缺模块子目录前缀） | `README.md:54`、`:97`、`:48-52` | 高 | B-02 | **已完成** | 已改为 `example/logging/log_config_demo.cpp` 与 `example/concurrency/beginner_demo.cpp`，示例段整体按模块目录重写；24 个 markdown 全量链接扫描，改动文件零失效（全库 3 处命中均为 lambda `[](...)` 正则误报，已逐条核实）；commit `36e4728` |
| D-02 | 文档 | 跨模块 / README | `README.md:48-52` 只登记 6 个示例，实际有 15 个；9 个示例未在 README 登记 | `README.md:48-52` vs `ls example/*/*.cpp`（15 个） | 中 | B-02 | **已完成** | 已按 `example/{concurrency,network,logging,tools}` 分组列全 15 个示例，并指向 `example/README.md` 速查表；commit `36e4728` |
| D-03 | 文档 | 跨模块 / README | 模块 target 清单漏 `go2cpp::future` 与 `go2cpp::network` | `README.md:90-94` vs `cmake/Go2CppModules.cmake:38,43,89,91` | 中 | B-02 | **已完成** | 已补齐两个 target；核对 `ALIAS` 共 14 项（13 个模块 target + `go2cpp::runtime`）；commit `36e4728` |
| D-04 | 文档 | context | `docs/context_rollback.md` 的示例链接指向不存在的路径 | `docs/context_rollback.md:19`；实际文件在 `example/concurrency/context_rollback_demo.cpp` | 中 | B-02 | **已完成** | 链接已补 `concurrency/` 前缀，扫描确认 target 存在；commit `fa39566` |
| D-05 | 不一致 | 文档 | `docs/modules.md` 称 `go2cpp_network` 依赖三个 target，漏记 `go2cpp_sync` | `docs/modules.md:11` vs `cmake/Go2CppModules.cmake:155`（依赖 `go2cpp_io go2cpp_context go2cpp_scheduler go2cpp_sync`） | 低 | B-02 | **已完成** | modules.md 重写后列出完整依赖表，含 `go2cpp_sync`；commit `23eccd0` |
| D-06 | 文档 | 测试文档 | `docs/testing.md` 引用重构前的测试路径，并提到非本仓工作树 | `docs/testing.md:415` → `tests/high_load_stress.cpp`（实际 `tests/concurrency/`）；同处提到 `NewGo2Cpp` 工作树 | 低 | B-02 | **已完成** | 已修正 `:384` 的 `tests/test_sync.cpp` 路径；为 `:415` 段加工作树与迁移说明；文首新增「路径约定」统一覆盖历史扁平路径（含 `:490` 的 `example/io_wait_many_demo.cpp`，本轮新发现）；commit `6cb11fb` |
| D-07 | 文档 | 测试文档 | `docs/go_reference_audit.md` 引用重构前路径 | `docs/go_reference_audit.md:106` → `tests/test_control_flow.cpp`（实际 `tests/tools/`） | 低 | B-02 | **已完成** | 核实同类失效共 **6 处**（`:34`/`:57`/`:81`/`:105`/`:106`/`:122`），已全部改为分模块路径；该文件用的是「当前 C++ 实现位于」式陈述，故直接修正而非加历史说明；12 处路径引用逐一 `test -e` 通过（missing = 0）；commit `d263b90` |
| D-08 | 文档 | codex.md | 历史条目中 15 条路径引用已因目录重构失效 | `codex.md:60,103,126,145,231,232,633,797` 等（`example/log_config_demo.cpp`、`tests/test_io.cpp`、`src/io.cpp`、`src/error.cpp` 等扁平旧路径） | 低 | B-02 | **已完成** | 按「保留历史原貌」原则处置：在顶部快照引用块追加路径说明（早期 `src|tests|example/*.cpp` 扁平布局 → 当前分模块结构，旧路径按当前结构换算），**未改写任何历史条目**；commit `eb64a75` |
| D-09 | 文档 | context | 文档示例统一用小写下划线 API，而头文件同时提供 PascalCase，文档未说明二者并存 | `docs/context_rollback.md:9,12,34-49`；`include/go2cpp/concurrency/context.hpp:307-346` | 低 | B-03 | **已完成** | 已在 `docs/context_rollback.md` 首个示例后补「命名约定」：五种拼写并存、小写为内联转发、语义相同；并如实点明 `committed()`/`had_failure()` 只有小写形式；commit `b3220e9` |
| T-01 | 测试 | context | `test_context.cpp` 有 144 个 `GO2CPP_CHECK` 但 **0** 个 `REQUIRE`/`REQUIRE_EVENTUALLY`，断言失败只计数、异常路径无中止保护 | `tests/concurrency/test_context.cpp`（统计：CHECK=144, REQUIRE=0, REQUIRE_EVENTUALLY=0）；对比 `tests/test_support.hpp:140-150` 的宏语义 | 高 | B-03 | **已完成** | 22 处主流程等待断言（`Done().WaitFor` / `Task::wait_for`）改为 `GO2CPP_REQUIRE`：REQUIRE 0 → 22、CHECK 144 → 122；回调/线程内先存 atomic 再断言的写法按语义保留为 CHECK；commit `aa9d810` |
| T-02 | 测试 | tools | `test_control_flow.cpp`(39×CHECK) 与 `test_error.cpp`(27×CHECK) 无任何看门狗宏 | `tests/tools/test_control_flow.cpp`、`tests/tools/test_error.cpp`（REQUIRE=0, REQUIRE_EVENTUALLY=0, JOIN=0） | 中 | B-03 | **已完成** | 各补 1 处 `GO2CPP_REQUIRE`，理由是**真实越界防护**而非形式统一：`test_control_flow.cpp:28` 的 `order.size()==3` 后紧接 `order[0..2]` 索引；`test_error.cpp:104` 的 `formatted.size()` 后紧接 `size()-16U` 作 `compare` 起点（不足时下溢并抛 `out_of_range`）；commit `0a8095b` |
| T-03 | 测试 | hybrid_mutex | `HybridGate`/`HybridMutex` 全仓无测试引用 | 实现见 `include/go2cpp/concurrency/core/hybrid_mutex.hpp:23-113` | 高 | B-04 | **已完成** | 新增 `tests/concurrency/test_hybrid_gate.cpp`：默认策略归一化（0 → 1）、BasicLockable 与 `std::lock_guard` 兼容、8 线程 × 20,000 次读-改-写互斥校验、最小自旋实例互斥；commit `f8156e5` |
| T-04 | 测试 | fd_manager | `FdManager` 全仓无测试引用 | 实现见 `src/network/fd_manager.cpp`、`include/go2cpp/network/fd_manager.hpp:42-60` | 中 | B-05 | **已完成** | 新增 `tests/network/test_fd_manager.cpp`：auto_create 语义、非阻塞/超时元数据、erase 标记 closed、duplicate 复制用户状态；测试使用远离真实 fd 空间的编号避免干扰进程句柄；commit `f8156e5` |
| T-05 | 测试 | error（在途改动） | 新增的 `errors::*` 命名空间无测试、无示例 | 定义见 `include/go2cpp/tools/error.hpp:204-246`（用户在途改动） | 中 | 需用户决策 | **已完成（按 U-01 默认执行）** | 按台账默认建议**保留**该 API，新增 `tests/tools/test_errors_ns.cpp` 与 `example/tools/errors_demo.cpp`；**用户可推翻**——若决定撤回，应删除这两处并回退 `test_main.cpp`/`cmake` 中的对应登记；commit `f8156e5` |
| T-06 | 测试 | event / go | `event.hpp`（`EventBatch`/`SelectLoop`）与 `go.hpp`（默认调度器）无独立 `run_xxx_tests()` | `tests/test_main.cpp` 原无 event/go 条目；`SelectLoop` 仅在 `tests/concurrency/test_beginner_api.cpp:35-138` 覆盖 | 低 | B-03 | **已完成** | 新增 `tests/concurrency/test_event_go.cpp`（8 个场景）：EventBatch 簿记、handler 调用、超时自动停止、`max_rounds`、handler 异常隔离、stop/reset，以及 `go(scheduler,fn)`、`go(fn)`、小写 `fiber` 描述器 bind；commit `f8156e5` |
| T-07 | 测试 | parking_condition / thread_policy / fiber_local | 三个模块无独立测试类，仅间接覆盖 | 原先分别仅被 `test_future.cpp`、`test_scheduler.cpp`/`test_hook.cpp`、`test_fiber.cpp` 间接触及 | 低 | B-04 | **已完成** | 已拆为三个独立文件：`test_parking_condition.cpp`（notify_one/notify_all、超时、谓词立即满足、静态查询）、`test_thread_policy.cpp`（默认 TLS 状态、参与与 Hook 作用域恢复、worker 的 runtime_worker 标记）、`test_fiber_local.cpp`（TLS fallback、键唯一性、Fiber 间隔离、park 后保持、结束后回收）；commit `f8156e5` |
| E-01 | 示例 | network | `network_demo.cpp` 仅 20 行、只有成功路径，缺等待/取消/生命周期与错误路径；且未出现在 `example/README.md` 速查表 | `example/network/network_demo.cpp:6-19`（无 Scheduler、无超时、无错误分支）；`example/README.md:36-48` 速查表无 `network_demo`（对比 `cmake/Go2CppTests.cmake:65` 已构建） | 中 | B-01 | **已完成** | B-01 重写为 157 行、五场景（粘包 / 半包 / 超长帧 / 分片 / 非法请求）；速查表与批量列表均已登记；commit `8ff0e6f` |
| E-02 | 示例 | future / io_backend / fd_manager / address / byte_array / parking_condition / hybrid_mutex | 七个模块无任何示例 | 交叉检索 `example/` 无命中；对照 `example/README.md` 速查表 | 中 | B-04 / B-05 | **已完成** | 新增 6 个示例：`future_demo`、`hybrid_mutex_demo`、`fd_manager_demo`、`io_backend_demo`、`address_bytearray_demo`、`errors_demo`，均满足三路径标准并登记进 CMake 与速查表。**`parking_condition` 未单独出示例**：它是 `core/` 内部设施，按 `docs/extensibility.md` 的职责划分不构成对外使用面，改由 `test_parking_condition.cpp` 覆盖；commit `f8156e5` |
| E-03 | 示例 | 跨模块 | `example/README.md:1-15` 的推荐阅读顺序只列 5 个示例，与速查表 11 行不一致 | `example/README.md:5-10` vs `:38-48` | 低 | B-02 | **已完成** | 已在 B-01 处理：推荐顺序后新增「上面只是入门主线，完整示例清单与场景说明见下方『示例场景速查』表」；commit `79d7b71` |
| D-10 | 文档 | 跨模块 | 项目**无** `AGENTS.md`/`CLAUDE.md`/`.agent/` 目录，协作规范此前只存在于 `codex.md` | `ls -a`（仅 `codex.md`）；`.agent` 不存在 | 低 | 本轮已处理 | 已完成 | 本轮新建 `.agent/quality_task_rules.md`，并在 `codex.md` 记录 |
| E-04 | 示例 | 跨模块 / network | `sse_demo`、`websocket_demo`、`timer_task_demo` 仍未登记进 `example/README.md` 速查表；批量运行列表条目数不足 | `example/README.md` 速查表现 15 行 vs `ls example/*/*.cpp`（15 个）；`cmake/Go2CppTests.cmake:56-73` | 中 | B-02 | **已完成** | 速查表补 3 行（`timer_task_demo` 入 concurrency 组；`websocket_demo`、`sse_demo` 入 network 组），批量列表 12 → 15 项，与磁盘 15 个示例完全一致；commit `15c74fb` |
| D-11 | 文档 | 跨模块 | `docs/modules.md` 作为「模块索引」过于简短（13 行），未覆盖 `include/go2cpp/core`、`detail` 子目录与新模块 target 全貌 | `docs/modules.md:5-13` vs `ls -R include/go2cpp` | 低 | B-02 | **已完成** | 已扩为 59 行：目录↔target 对应表、头文件布局（含 `core`/`detail` 定位）、完整 `target_link_libraries` 依赖表、测试与示例登记约定；commit `23eccd0` |
| D-12 | 文档 | 测试文档 | `docs/testing.md:595` 称「当前仍未在现有 Boost.Context fcontext 后端实现……」，与当前 Linux x86_64 自有上下文后端不符 | `docs/testing.md:595` vs `cmake/Go2CppModules.cmake:3-7`；同类问题见 I-01 | 低 | B-02 | **已完成** | 已改为「现有 Fiber 上下文后端（Linux x86_64 为 Go2Cpp 自有 ABI，其他平台为 Boost.Context）」；commit `6cb11fb` |
| D-13 | 文档 | network / io_backend | `io_backend.hpp` 未记录「平台默认后端不可注销」这一约束；调用方按头文件语义调用 `UnregisterBackend("epoll")` 只会得到 false，容易被误认为已生效 | `include/go2cpp/io_backend.hpp:77`（仅写「注销后端；只影响后续 CreateBackend」）vs `src/network/io_backend.cpp:197`（`if (name == DefaultBackendName()) return false;`） | 低 | 待定 | 待办 | **本轮读源码发现**。修复：在头文件注释中补上该约束与理由（默认后端是 IOManager 的启动依赖） |
| D-14 | 文档 | tools / error | `Is(error, target)` 在 `target` 为空时有**特殊语义**——返回 `!error`（即退化为「error 是否为空」的判断），与常规「按对象身份匹配」完全不同，但头文件只有裸声明、无任何注释 | `include/go2cpp/tools/error.hpp:136`（裸声明）vs `src/tools/error.cpp:392-394`（`if (!target) return !error;`） | 低 | 待定 | 待办 | **本轮读源码发现**。修复：为 `Is` 补 Doxygen 注释，一并说明 `WalkErrors` 对空 visitor 返回 false 等边界 |
| D-15 | 文档 | concurrency / future | `BrokenPromiseError()` 返回**进程级共享单例**，因此不同 Future 因 Promise 被放弃而产生的错误**是同一个对象**，`Is(errA, errB)` 会返回 true。头文件与文档均未说明该共享语义 | `src/concurrency/future.cpp:168-173`（`static const ErrorPtr error = NewError(...)` 函数局部 static）vs `include/go2cpp/concurrency/future.hpp` 无相关说明 | 低 | 待定 | 待办 | **本轮读源码发现**。共享是为避免每次 Promise 析构重复分配（`:169` 行内注释已说明），属有意设计；建议在头文件补一句，否则按「错误身份唯一」写断言会踩坑。**同一模式还出现在 `src/concurrency/channel.cpp:39-91`**：`ChannelClosedError` 等 5 个错误工厂各自用函数局部 static 共享单例，因此两个不同 channel 的"already closed" 因此两个不同 channel 的"already closed"错误也是同一对象。**本轮复核 context.cpp 后修正判断**：`src/concurrency/context.cpp:1877-1893` 的 `CanceledError()`/`DeadlineExceededError()` 同样用函数局部 static 单例——这类**哨兵错误**（sentinel error）共享单例正是 Go 的惯用语义（`errors.Is(err, context.Canceled)`），因此本条的严重度按**信息性**看待：需要的是文档说明，而不是修改实现 |
| D-16 | 文档 | network / socket | `Socket::fd()` 返回的是**瞬时 fd 值**，不进入 `begin_operation` 的 in-flight 保护；调用方若长期持有该值，会与 `close()`/fd 复用竞争（ABA）。`docs/network.md` 只说明了读写、accept、connect、选项与地址查询会固定 fd，未提 `fd()` 本身 | `src/network/socket.cpp:151`（`fd()` 只加锁读 `m_fd`）vs `:87-103`（`begin_operation` 才做 in-flight 固定） | 低 | 待定 | 待办 | **本轮读源码发现**。建议在 `socket.hpp` 的 `fd()` 注释中说明它是瞬时值、不得跨操作持有 |
| D-17 | 文档 | scheduler | `Scheduler::wake_io_batch` 位于**公开区**（`scheduler.hpp:537`，public 起于 `:408`），但其实现 `scheduler.cpp:2972-2987` 明确跳过 `owned_by` 所有权校验（注释称「公开 `wake_io()` 仍保留完整校验，批量入口只接受内部已登记节点」）。头文件注释只描述批量优化效果，**未声明「仅接受本 Scheduler 已登记节点」这一前置条件**。外部调用方传入未注册或异属 task 时，不属于本调度器的 G 会被直接放进本调度器的 P 队列，破坏 owner 不变量 | `include/go2cpp/concurrency/scheduler/scheduler.hpp:534-537` vs `src/concurrency/scheduler.cpp:2972-2987`（跳过校验）与 `:2928-2931`（`wake_io` 保留校验） | 中 | 待定 | 待办 | **本轮读源码发现**。修复方向：在头文件注明前置条件，或把批量入口移入 private 只留给 IOManager |
| D-18 | 文档 | scheduler / configuration | `docs/compatibility.md` 的「动态 M」行明确写「affinity 只是有界偏好，不是缓存保证；**不做 CPU 绑核**」，但 `src/concurrency/scheduler.cpp:3505-3515` 在 `config.pin_workers_to_cpu == true` 时**确实调用 `pthread_setaffinity_np`** 把 worker 绑到 P 编号对应的 CPU；而 `docs/configuration.md` 又把 `pin_workers_to_cpu` 列为合法布尔配置项。三处描述不一致，`compatibility.md` 的绝对表述与实现冲突 | `docs/compatibility.md`（动态 M 行）vs `src/concurrency/scheduler.cpp:3505-3515` vs `docs/configuration.md`（布尔配置项段落） | 中 | 待定 | 待办 | **本轮读源码发现**。默认值为 `false`（见 `go2cpp.ini`），因此默认行为确实不绑核——但「文档称绝不绑核」与「存在开关且会绑核」必须统一表述 |
| T-08 | 测试 | 测试框架 | `GO2CPP_TEST_FILTER` 指定不存在的名字时，`tests/test_main.cpp` 不执行任何模块却仍返回 0，容易被误判为「测试通过」 | `tests/test_main.cpp` 的 `selected()` 对未知名字全部为假；本轮验证脚本曾因此把编译失败后的**旧二进制**误判为 7/7 通过 | 中 | 待定（建议 B-06） | 待办 | 建议：过滤器非空且未匹配任何模块时返回非零并在 stderr 明确提示；在修复前，验证脚本必须校验 `announce` 文本是否出现 |

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
| X-02 | 文档 | `docs/design.md` 同一文件内对 Fiber 后端的描述互相否定 | `docs/design.md:121-130` vs `:221` | 见 I-01；**B-02 已完成**（commit `23677d6`） |
| X-03 | 文档 | `docs/boundary_audit.md` 同一文件内自相矛盾 | `docs/boundary_audit.md:25`（native context 有溢出检查）vs `:30`（称当前使用 Boost.Context） | 见 I-01；**B-02 已完成**（commit `23677d6`） |
| X-04 | 文档 | `docs/performance.md` 内部性能结论数量级矛盾（混合锁「快 10~18 倍」vs「8.5~8.8 s」；IO「慢 3.0~4.3 倍」vs「0.59x 已超过」） | `docs/performance.md:140` vs `:42-46`/`:183`/`:214-216`；`:141` vs `:184`；另 `:5-7` 自称「本节优先」但顶部小节早于后文 | **B-02 已完成**（commit `64a0cdc`）：核实为**累积记录性质**而非错误数据——同参数在不同轮次的记录差异来自调度配置、编译选项与宿主抖动；已在文首补「阅读须知」说明不得跨节比较，当前结论以日期最新一节为准 |
| X-05 | 测试 | `tests/concurrency/test_context.cpp` 等以 CHECK 为主、缺超时保护，挂起式失败会拖到 CTest 30s TIMEOUT | 见 T-01/T-02；`cmake/Go2CppTests.cmake:31` TIMEOUT 30 | 属测试健壮性缺陷，非产品缺陷 |
| X-06 | 网络 / fd_manager | `FdContext` 的字段 getter 全部无锁读，而 `FdManager` 写侧持 `m_mutex`；头文件 `fd_manager.hpp:18` 却声称「字段访问由 FdManager 内部互斥保护」。跨线程「一边 `set_nonblock`/`set_timeout`、一边读 getter」构成 data race（UB） | `include/go2cpp/network/fd_manager.hpp:19-39`（getter 无锁）vs `src/network/fd_manager.cpp:57-59,66-68`（写在 `m_mutex` 下）；类注释见 `fd_manager.hpp:18` | 中 | 待定 | 待办 | **本轮读源码发现**（文档不可得）。建议修复：把 4 个 bool + 2 个 `milliseconds` 改为 `std::atomic`（注意 `duplicate` 的 `*result = *old->second` 需改为逐字段 load/store），或把 getter 收回 FdManager 并加锁。因本环境 TSan 受 WSL 限制无法运行、缺可复现失败现场，按修复门槛**暂不修改**，仅登记 |
| X-07 | 并发 / fiber_local | `FiberLocalCache::Reset()` 两条路径行为不对称：Fiber 路径走 `detail::Reset`，有 `for(;;)` 循环并把用户析构挪到锁外；线程路径只是 `ThreadValues().erase(m_key)`，**在 erase 内直接析构**。若值的析构函数重入并写回同一 key，线程路径会修改正在 erase 的容器，违反关联容器「元素析构函数不得修改容器」的约定（UB） | `include/go2cpp/concurrency/fiber_local.hpp:102-108`（Reset 的两条分支）vs `src/concurrency/fiber_local.cpp:124-150`（Fiber 路径的循环回收）；`src/concurrency/fiber_local.cpp:147` 的行内注释明确承认该约束 | 中 | 待定 | 待办 | **本轮读源码发现**。修复方向：让线程路径复用同一套「先摘出、锁外析构、循环复检」协议。同样因缺可复现失败现场而暂不修改 |
| X-08 | 并发 / control_flow | `recover::take()`/`operator()` 用**线程局部**的 `s_defer_depth` 判断「是否在 defer 回调内」。defer 回调本身禁止挂起/迁移，但若违反该约束让 Fiber 在回调中 Suspend 并迁移到另一线程，`InDeferCallback()` 在新线程上为 false，`take()` 会静默返回 `nullopt`，panic 状态无人消费 | `src/tools/control_flow.cpp:15,28-38`（thread_local 深度）与 `:257-260`、`:284-287`（两个入口都先查 `InDeferCallback()`）；异常捕获点在 `include/go2cpp/tools/control_flow.hpp:97-101` | 低 | 待定 | 待办 | **本轮读源码发现**。这是既有约束「defer 回调必须短小、不得挂起 Fiber」的**必然推论**，不是独立缺陷；登记以便调用方理解该约束为何必须遵守 |
| X-09 | 并发 / timer | 两处 TimerService 对「在自身定时器线程内停止服务」策略不一致：`src/concurrency/timer.cpp:120-123` 的 `Impl::~Impl` 检测到同线程时直接 `std::terminate()`；`src/concurrency/context.cpp:307-318` 的内部 TimerService `Shutdown()` 则**跳过 join 并继续返回**。同一代码库对同类危险操作给出两种后果 | `src/concurrency/timer.cpp:120-123` vs `src/concurrency/context.cpp:307-318` | 低 | 待定 | 待办 | **本轮读源码发现**。context.cpp 中的实例是内部实现、用户无法直接析构，实际影响有限；但策略差异值得统一或在注释中写明理由 |
| X-10 | 并发 / scheduler | 同一个 `WakeAction::kPending` 在不同唤醒入口的返回值语义相反：`wake_registered` 视为**失败**返回 false（`:2852-2854`），`wake_or_cancel` 同样返回 false（`:3089-3093`），而 `wake_io` 视为**成功**返回 true（`:2933-2935`）。三者都是 public API | `src/concurrency/scheduler.cpp:2847-2857` vs `:2928-2938` vs `:3084-3096` | 中 | 待定 | 待办 | **本轮读源码发现**。语义各有理由（IO 场景下 pending 表示运行中的 G 会自行消费令牌），但同名返回值在不同入口含义相反，调用方极易误判「是否已入队」；建议统一或在头文件逐入口注明 |
| X-11 | 并发 / scheduler | `enqueue` 中用于固定生产者条带的 `static thread_local std::size_t t_incoming_stripe`（`:2392-2399`）是**函数局部 static**，因此**跨 Scheduler 实例共享**：线程先向 Scheduler A 提交并缓存条带号后，再向 Scheduler B 提交会直接复用该号，不再从 B 的 `next_incoming_stripe` 分配。后果是 B 的分配器计数不反映实际使用，多 Scheduler 场景下条带分布可能不均 | `src/concurrency/scheduler.cpp:2392-2399` vs `:1797`（`next_incoming_stripe` 是 per-Impl 成员） | 低 | 待定 | 待办 | **本轮读源码发现**。不影响正确性（每个 Impl 有自己的 `incoming_queues`），仅影响分布均匀性；修复可把缓存下沉为 per-Impl，或接受现状并在注释说明 |

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
| **B-02**（已完成） | 文档重构后路径与索引同步 | D-01~D-09、D-11、I-01、X-02~X-04 | B-00 | `README.md`、`docs/modules.md`、`docs/context_rollback.md`、`docs/testing.md`、`docs/go_reference_audit.md`、`docs/design.md`、`docs/boundary_audit.md`、`docs/performance.md`、`codex.md`(仅顶部说明) |
| **B-03**（已完成） | tools + context：断言健壮性与新 API 覆盖 | T-01、T-02、T-05、T-06、D-09 | U-01 | `tests/concurrency/test_context.cpp`、`tests/tools/*.cpp`、`docs/context_rollback.md`、新增 `tests/concurrency/test_event_go.cpp`(建议) |
| **B-04**（已完成） | scheduler/fiber/并发核心缺口 | T-03、T-07、E-02(future/parking_condition/hybrid_mutex) | B-03 | 新增 `tests/concurrency/test_hybrid_gate.cpp`、可选 `example/concurrency/future_demo.cpp` |
| **B-05**（已完成） | network 缺口：fd_manager / io_backend / address / byte_array | T-04、E-02(io_backend/fd_manager/address/byte_array) | B-01 | 新增测试与示例各 1~2 个；`cmake/Go2CppTests.cmake` 同步登记 |
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