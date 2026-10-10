> 当前快照（2026-09-24）：在此前删除旧 panic_defer 模块后，新增
> control_flow 模块。它提供 RAII defer、显式 panic/recover 状态和
> LastDeferException()；这是可验证的 C++ 等价协议，不是 Go 编译器级自动
> 栈展开。Channel select 新增 SelectValue/SelectCaster/SetAny，IOManager
> 新增 Fiber-only WaitAny/WaitMany；普通线程多 fd 等待应使用原生 poll/select。
> 下方历史记录中的“未实现”结论以本说明和最新兼容性矩阵为准。
> 另注：历史条目中的源码/测试/示例路径反映**当时**的目录结构——早期为
> `src/*.cpp`、`tests/*.cpp`、`example/*.cpp` 的扁平布局，现已重构为
> `src/{concurrency,network,logging,config,tools}/`、
> `tests/{concurrency,network,logging,tools}/`、
> `example/{concurrency,network,logging,tools}/`。引用旧路径时请按当前结构
> 换算；这些条目不改写，以保留演进过程。

## 2026-10-10：B-02 收尾（参考审计路径、协作记录路径说明、示例登记）

- **本轮范围**：台账 B-02 第 3 批 3 条：**D-07**、**D-08**、**E-04**。至此 **B-02 批次全部完成**（D-01~D-08、D-11、D-12、E-04、I-01、X-02~X-04；D-09 台账原就归属 B-03）。
- **改动文件**（3 个）：
  - `docs/go_reference_audit.md`：修正 **6 处**扁平源码/测试路径——`:34` `src/scheduler.cpp`→`src/concurrency/scheduler.cpp`、`:57` context、`:81` channel、`:105` `src/control_flow.cpp`→`src/tools/`、`:106` `tests/test_control_flow.cpp`→`tests/tools/`、`:122` `src/error.cpp`→`src/tools/`。台账 D-07 只登记了 `:106` 一处，本轮按同类问题一并处置；该文件用的是「当前 C++ 实现位于」式陈述（非历史快照），故直接修正，不套用 D-08 的加注方式。
  - `codex.md`：在顶部快照引用块追加历史路径说明——早期为 `src/*.cpp`、`tests/*.cpp`、`example/*.cpp` 扁平布局，现已分模块，引用旧路径时按当前结构换算；**历史条目不改写**，以保留演进过程。
  - `example/README.md`：速查表补 `timer_task_demo`（concurrency 组末尾）、`websocket_demo` 与 `sse_demo`（network 组末尾）；批量运行列表 12 → 15 项。
- **验证结果**：`docs/go_reference_audit.md` 全部 12 处路径引用逐一 `test -e` 确认存在（missing = 0）；`example/README.md` 速查表 15 行、批量列表 15 项，与磁盘 15 个示例文件数一致；`cmake --build build-quality --parallel` 0 warning / 0 error；`ctest` **18/18 通过**；`log/` 已恢复到轮初 md5。
- **一处验证脚本失误（如实记录）**：本轮第一次验证在 `wsl bash -lc` 内拼了含反引号与嵌套单引号的 `sed`/`while` 管道，反引号被 bash 当作命令替换，输出退化为 `MISS` 空路径。**修改本身未受影响**（文件、构建、ctest 均正常）；随后改用 Python 侧读取解析重做验证，结论以上一段为准。教训：验证逻辑不要在 shell 字符串里嵌套反引号，改为 Python 读文件后逐项检查。
- **未通过/未覆盖**：未跑 sanitizer 与 Valgrind；`codex.md` 历史条目中的 15 处失效路径按 D-08 约定**不改写**，仅由顶部说明覆盖。
- **提交**：`d263b90`（参考审计路径）、`15c74fb`（示例登记）、`eb64a75`（codex.md 路径说明）。
- **下一步**：**B-03**（tools + context：断言健壮性与新 API 覆盖），覆盖 T-01、T-02、T-05、T-06、D-09。其中 T-05 依赖待决策项 **U-01**（在途改动新增的 `errors::` 命名空间是否保留），按台账默认建议「保留并补最小测试与示例」执行，用户可推翻。

## 2026-10-10：B-02 次轮（模块索引、测试文档路径、性能文档阅读须知）

- **本轮范围**：台账 B-02 第 2 批 5 条：**X-04**、**D-05**、**D-11**、**D-06**、**D-12**（按同文件聚合：`modules.md` 两条、`testing.md` 两条、`performance.md` 一条）。
- **改动文件**（3 个，全部为文档）：
  - `docs/modules.md`：13 行 → 59 行。补全 13 个模块 target 与 `go2cpp::runtime` 的对应关系、`include/go2cpp/core` 与 `detail` 子目录定位、完整的 `target_link_libraries` 依赖表；修正 `go2cpp_network` 漏记的 `go2cpp_sync`（`cmake/Go2CppModules.cmake:155`）。
  - `docs/testing.md`：`:384` 的 `tests/test_sync.cpp` → `tests/concurrency/test_sync.cpp`；为 `:415` 段补工作树与路径迁移说明；`:595` 的「现有 Boost.Context fcontext 后端」改为「现有 Fiber 上下文后端（Linux x86_64 为自有 ABI，其他平台为 Boost.Context）」；文首新增**路径约定**，统一说明历史条目中的扁平路径（覆盖本轮新发现的 `:490` `example/io_wait_many_demo.cpp`）。
  - `docs/performance.md`：文首新增**阅读须知**；就地修正 `:27`、`:63-65`、`:144` 三处残留的 Boost.Context 表述。
- **X-04 的处置判断**：该条目描述为「内部性能结论数量级矛盾」，核实后确认**不是错误数据，而是累积记录的性质**。同一负载参数（2,000 Fiber / 32 普通线程 / 206,400 次临界区）在不同轮次分别记录过 `99～107 ms`、`8.5～8.8 s`、`6.7～6.9 s`、`9/19/31/51 ms`，差异来自轮次间的调度配置、编译选项与 WSL 宿主抖动。因此按「不改写历史、补时效与阅读指引」处置，与 D-08 的处理原则一致；文首明确「不存在单一当前值」，并指定以日期最新一节为当前结论。
- **一处过程失误（如实记录）**：上一轮 I-01 的验证使用了 `grep -v '性能\|performance\|...'` 按行过滤，而 `grep -rn` 的输出行以文件名为前缀，`docs/performance.md` 的**每一行**都含 `performance`，导致整个文件的命中被过滤掉，漏检 `:27`、`:63-65`、`:144`。本轮已补齐。教训：按行排除必须匹配内容而非路径，或改用 `--include`/结构化扫描。
- **验证结果**：24 个 markdown 全量相对链接扫描，**本轮改动文件零真实失效**（唯一命中 `docs/quality_backlog.md:15` 是本项目自己写入的 `[](...)` 文本被正则误判，已核实）；`cmake --build build-quality --parallel` 0 warning / 0 error；`ctest` **18/18 通过**；`log/` 已恢复到轮初 md5。
- **未通过/未覆盖**：B-02 剩余 2 条（D-07、D-08）与 E-04 未处理；未跑 sanitizer 与 Valgrind；`docs/testing.md`、`codex.md` 记载的历史性能数字未复核。
- **提交**：`23eccd0`（modules.md）、`6cb11fb`（testing.md）、`64a0cdc`（performance.md）。
- **下一步**：B-02 收尾轮，处理 D-07（`docs/go_reference_audit.md` 重构前路径）、D-08（`codex.md` 历史路径加时效说明）、E-04（`sse`/`websocket`/`timer_task` 登记进速查表与批量列表）。

## 2026-10-10：B-02 首轮（README 路径与索引、Fiber 后端表述统一）

- **本轮范围**：台账 B-02 的第 1 批，按「单轮 ≤5 条」取严重度最高的 5 条：**I-01**（含重复登记的 **X-02**、**X-03**）、**D-01**、**D-02**、**D-03**、**D-04**。B-02 共 12 条，本轮完成 5 条，剩余 7 条留待后续轮次。
- **改动文件**（6 个，全部为文档；未触碰任何源码、测试或示例）：
  - `README.md`：示例段按 `example/{concurrency,network,logging,tools}` 重写并列全 15 个示例；修正 `example/log_config_demo.cpp` → `example/logging/log_config_demo.cpp`、`example/beginner_demo.cpp` → `example/concurrency/beginner_demo.cpp`；模块 target 清单补 `go2cpp::future` 与 `go2cpp::network`。
  - `docs/context_rollback.md`：示例链接补 `concurrency/` 前缀。
  - `docs/design.md:221`、`docs/boundary_audit.md:30`、`docs/go_reference_audit.md:135`：统一为「Linux x86_64 由 Go2Cpp 自有上下文后端提供，其他平台回退 Boost.Context」。
  - `docs/performance.md`：给「自研上下文已撤回」的历史段加**时效说明**引用块，指明其已被「2026-09-30 Context 与跨 M 唤醒专项」取代；**保留原文，不改写历史**，与台账 D-08 的处理原则一致。
- **源码事实依据**：`cmake/Go2CppModules.cmake:3-7` 先置 `GO2CPP_NATIVE_CONTEXT OFF`，仅在 64 位非 aarch64/arm64/riscv/ppc/s390 的 Linux 上置 ON；`:33-36` 编译 `src/concurrency/context_x64.S` 并定义 `GO2CPP_USE_NATIVE_CONTEXT=1`；`:143-150` 仅在 OFF 时链接 `Boost::context`。本机 `build-quality` 实测存在 `CMakeFiles/go2cpp_fiber.dir/src/concurrency/context_x64.S.o`，且 `flags.make` 含 `-DGO2CPP_USE_NATIVE_CONTEXT=1`。
- **验证结果**：24 个 markdown 文件全量相对链接扫描（按各文件所在目录解析相对路径），**本轮改动的 6 个文件零失效链接**；全库仅 3 处命中且均为 lambda 捕获 `[](...)` 被正则误判，已逐条核实为 `docs/configuration.md:60`、`docs/logging.md:53`、`docs/timer_usage.md:55`。README 引用的 8 个示例 target 在 `build-quality/CMakeFiles/` 下逐一确认存在。`cmake --build build-quality --parallel` 0 warning / 0 error；`ctest` **18/18 通过**（23.76 s）；`log/` 已恢复到轮初 md5。
- **未通过/未覆盖**：B-02 剩余 7 条（D-05、D-06、D-07、D-08、D-11、X-04、D-12）本轮未处理；未跑 sanitizer 与 Valgrind；`docs/testing.md` 与 `codex.md` 记载的历史性能数字未复核。
- **新登记**：**D-12**（`docs/testing.md:595` 仍称「现有 Boost.Context fcontext 后端」，与本轮统一后的表述冲突），归 B-02 下轮。
- **提交**：`36e4728`（README）、`23677d6`（后端表述）、`fa39566`（回滚文档链接）。
- **下一步**：B-02 第 2 批，取 D-05、D-06、D-07、D-08、D-11（D-12、E-04 视剩余预算可选）。

## 2026-10-10：B-01 示例质量（network_demo 三路径与登记）

- **本轮范围**：台账批次 **B-01**，覆盖 **E-01**、**E-03**。运行模式为定时任务单轮执行；轮次互斥锁 `.agent/guard.sh` 由本轮首次创建（lock 目录已加入 `.gitignore`）。
- **改动文件**：
  - `example/network/network_demo.cpp`：19 行 → 157 行。从「只有成功路径」扩为五个场景：场景一粘包，一次输入交付全部完整帧；场景二半包，分隔符跨两次输入，`take_frames()` 为空但 `buffered_size()==6`；场景三超长帧，`max_frame=4` 触发 `FeedError::FrameTooLarge`，缓冲被清空、失败态拒绝后续输入、`reset()` 后可复用；场景四 HTTP 逐字节喂入累积到 `Complete`；场景五非法请求行返回 `Error`，未 `reset()` 时坏缓冲继续报错。
  - `example/README.md`：速查表新增 `network_demo` 行；批量运行列表加入 `network`；推荐阅读顺序后补「完整示例清单见下方速查表」指引。
- **实现取舍**：`network_demo` 保持「纯解析、无 IO」定位，未引入 Scheduler/socket；epoll 上的等待、超时与取消由既有的 `io_wait_many_demo.cpp`、`sse_demo.cpp`、`websocket_demo.cpp` 覆盖，并在新示例的 Doxygen 注释中做了交叉指引。
- **一处如实说明**：HTTP 解析器**没有**粘性错误态。`Parser::feed_impl` 入口只检查 `m_finished`，不检查 `m_error`；返回 `Error` 后重复喂入仍报错的真正原因是出错时的缓冲区未被丢弃。示例注释与场景名按此事实书写，未表述为「失败态粘性」。
- **验证结果**：`build-quality`（Debug，TESTS+EXAMPLES ON）全量构建 0 warning / 0 error；`ctest` **18/18 通过**，总耗时 23.86 s，其中 `go2cpp_network_demo` Passed；示例单独运行退出码 0，五个场景全部报告「通过」。开发过程中示例曾因自身测试数据不合 `max_frame=4` 约束而 abort（`std::length_error: frame too large`），已改用合法短帧修正——属示例数据问题，非库缺陷。
- **未通过/未覆盖**：本轮未跑 sanitizer 与 Valgrind；未做多次重复运行（示例为确定性纯解析、无并发或超时路径，不适用「≥5 次重复」要求）；未验证根 `README.md` 的示例清单——那是台账 D-02，归 B-02。
- **新登记**：`E-04`（`sse_demo` / `websocket_demo` / `timer_task_demo` 仍未登记进速查表与批量列表），归 B-02。
- **提交**：`8ff0e6f`（示例）、`79d7b71`（索引登记）。
- **下一步**：B-02 文档重构后路径与索引同步（D-01~D-09、D-11、I-01、X-02~X-04），条目多于 5 条，须拆多轮。

## 2026-10-10：文档/测试/示例质量审计与建账

- **本轮范围**：只读审计 + 建账，**未修改任何既有源码/文档/测试/示例**。新增 3 个文件：
  `.agent/quality_task_rules.md`（执行规则）、`docs/quality_backlog.md`（问题台账）、本条记录。
  真身路径 `/UserData/CodexWorkSpace/Go2Cpp`（ext4，dev 2096）。
- **基线**：分支 `main`，HEAD `d0fdf70`；`git status --short` 为 4 项——用户 3 处在途改动
  （`go2cpp.ini`、`include/go2cpp/logging/log.hpp`、`include/go2cpp/tools/error.hpp`）
  加上一轮追加的 `codex.md`。`git diff` 指纹轮初为 `757a9920...`。项目无
  `AGENTS.md`/`CLAUDE.md`/`.agent/`，协作记录沿用 `codex.md`。
- **重要纠正（工具链导致的历史误判）**：内置 `read` 工具把项目路径解析到 E 盘旧副本，
  导致上一轮得出"`go2cpp_network` target 不存在、网络源码并入 `go2cpp_io`"的**错误结论**。
  本轮以 WSL 直读 + `git show HEAD:` 双重取证确认：`cmake/Go2CppModules.cmake` 真身为
  **177 行**，`go2cpp_network` 在 `:43-50` 定义、`:71` 导出 `EXPORT_NAME network`、
  `:91` 建别名、`:155` 依赖 `go2cpp_io go2cpp_context go2cpp_scheduler go2cpp_sync`，
  且该文件工作区与 HEAD **无差异**。故 `docs/modules.md:11` 关于 `go2cpp_network` 的
  说法本身是**正确**的（仅漏记 `go2cpp_sync` 依赖）。上一轮的相反结论已作废。
- **同步纠正第二项**：上一轮称"WebSocket 无测试覆盖"亦为旧副本数据所致。真身
  `tests/network/test_network.cpp` 中有 51 处 websocket 命中，含 RFC 6455 握手向量与
  socketpair 端到端闭环（`:560-598`）。
- **四方覆盖矩阵（文档/测试/示例）结论**：30 个模块中
  - **四维齐备**：error、control_flow、context、channel、scheduler、fiber、sync、timer、
    io、hook、socket、http、sse、websocket、logging、config、framing；
  - **缺示例**：future、io_backend、fd_manager、parking_condition、hybrid_mutex、
    address、byte_array（共 7 个）；
  - **缺独立测试入口**（功能已被间接覆盖）：event、go、fiber_local、parking_condition、
    thread_policy；
  - **完全无测试引用**：`HybridGate`/`HybridMutex`（`concurrency/core/hybrid_mutex.hpp`）、
    `FdManager`（`src/network/fd_manager.cpp`）。
- **测试框架事实**：自研框架，`tests/test_main.cpp:7-22` 声明 16 个 `run_xxx_tests()`，
  与 16 个实现一一对应，支持 `GO2CPP_TEST_FILTER`。宏用量实测：`GO2CPP_CHECK` 1301、
  `GO2CPP_REQUIRE` 86、`GO2CPP_REQUIRE_EVENTUALLY` 74、`GO2CPP_JOIN_WITH_WATCHDOG` 60。
  **`tests/concurrency/test_context.cpp` 有 144 个 CHECK 却 0 个 REQUIRE 系列**，
  `tests/tools/test_control_flow.cpp`(39) 与 `test_error.cpp`(27) 同样无看门狗保护。
- **关键不一致（台账 I-01）**：Fiber 上下文后端在文档间严重矛盾，且**同一文件内自相矛盾**——
  `docs/design.md:121-130`（自有汇编、不再链接 Boost.Context）vs `:221`（称 Boost.Context）；
  `docs/boundary_audit.md:25` vs `:30`；`docs/performance.md:190-193`（自研汇编已撤回）
  vs `:204-208`（已上线）。源码事实：构建实际编译 `src/concurrency/context_x64.S` 并定义
  `GO2CPP_USE_NATIVE_CONTEXT=1`（`cmake/Go2CppModules.cmake:33-36`）。
- **关键不一致（文档 D-01~D-09）**：`README.md:54/97` 与 `:48-52` 的示例路径因目录重构失效
  （缺 `concurrency/`、`logging/` 前缀）；`README.md:90-94` 模块清单漏 `go2cpp::future`、
  `go2cpp::network`；`docs/context_rollback.md:19` 链接指向不存在的
  `../example/context_rollback_demo.cpp`；`docs/testing.md:415`、`docs/go_reference_audit.md:106`
  引用重构前测试路径；`codex.md` 历史条目有 15 处失效路径（属历史记录，建议加时效说明而非改写）。
- **示例质量（E-01）**：`example/network/network_demo.cpp` 仅 20 行、只有成功路径，
  缺等待/取消与错误路径，且未出现在 `example/README.md:36-48` 速查表——违反项目自定的
  「每个示例至少含成功 + 等待/取消 + 错误/边界三条路径」标准（`example/README.md:30-34`）。
- **台账统计**：共 **22** 条。按类别：文档 11（D-01~D-11）、测试 7（T-01~T-07）、
  示例 3（E-01~E-03）、不一致 1（I-01）；另列疑似缺陷 5 条（X-01~X-05）与待用户决策 4 条
  （U-01~U-04）。按严重度：高 4、中 8、低 10。
- **批次划分与建议顺序**：B-01 示例三路径与登记 → B-02 文档路径/索引/后端表述同步 →
  B-03 tools+context 断言健壮性与新 API 覆盖 → B-04 并发核心缺口（HybridGate 等）→
  B-05 network 缺口（fd_manager/io_backend/address/byte_array）→ B-06 logging+config 与
  跨模块总入口。每批均可独立构建与 `ctest` 验证，无跨批依赖。
- **待用户决策要点**：U-01 在途新增的 `errors::` 命名空间是否保留（决定是否补测试/示例）；
  U-02 `docs/performance.md` 中 `/mnt/e/CodexWorkspace/...` 的 coost 基准路径是否视为有效；
  U-03 `codex.md` 历史失效路径是否改写；U-04 是否要求示例间歇失败修复后才能进 `ctest` 门禁。
- **未覆盖范围**：本轮未构建、未跑 `ctest`、未跑任何 sanitizer；台账中的间歇失败引用上一轮
  实测数据（本轮未复现）；未逐行通读 scheduler.cpp(4048)/io.cpp(2646)/hook.cpp(2161) 等大文件；
  未探查 `/mnt/e/...` 引用路径；未校验 `third_party` SHA-256；未触碰 `build/`、`log/`、`third_party/`。
- **工具陷阱（已固化进规则文件 §1.1）**：T1 内置 `read`/`grep` 解析到 E 盘旧副本
  （`E:\UserData\CodexWorkSpace\Go2Cpp`），必须走 `wsl bash -lc` 或 UNC 路径；
  T2 经 `pwsh` 下发 `wsl bash -lc` 时双引号内 `$` 被 PowerShell 插值吞掉（本轮再次触发，
  导致一次统计全空），含 shell 变量或 `$?` 必须改走 Python subprocess 通道；
  T3 工具会"成功返回错误内容"，关键结论需双通道交叉验证。

## 2026-10-10：项目认知梳理（只读，无源码改动）

- **本轮范围**：只做认知梳理与实测验证，未修改任何源码/配置/测试/文档；唯一写入是本条记录。工作路径为 WSL 真身 `/UserData/CodexWorkSpace/Go2Cpp`（ext4，`stat -c %d` = 2096）。
- **重要环境结论（工具层，非项目缺陷）**：DSH 的 `read`/`grep` 工具会把 `/UserData/CodexWorkSpace/Go2Cpp/...` 解析到 E 盘另一份陈旧副本 `E:\UserData\CodexWorkSpace\Go2Cpp`（9p，dev 72）。该副本缺 7 个文档（`docs/modules.md`、`docs/boundary_audit.md`、`docs/network.md`、`docs/sse.md`、`docs/websocket.md`、`docs/timer_usage.md`、`docs/timer_benchmark.md`），且 `cmake/Go2CppModules.cmake` 仍引用重构前的扁平源码路径（`src/error.cpp`、`src/io.cpp` 等）。本轮 31 个抽样文件中 17 个 md5 与真身不同，`codex.md` 真身 1116 行 vs 该副本 861 行。**后续在本工作区读取项目文件必须走 `wsl bash -lc` 通道，否则读到的不是真身。**
- **第二类工具陷阱**：经 `pwsh` 下发 `wsl bash -lc "..."` 时，双引号内的 `$` 会被 PowerShell 变量插值吞掉（`$f` 变空导致 `ambiguous redirect`，`$?` 无法取得退出码）。凡命令含 shell 变量或 `$?`，改走 Python→`subprocess(["wsl","bash","-lc",...])` 通道。
- **git 状态可信性核验**：`git show HEAD:include/go2cpp/runtime.hpp` 为 36 行、与工作区一致；`git ls-files -v | grep -v '^H '` 为空（无 skip-worktree / assume-unchanged）。故 `read` 的内容差异确认为工具解析问题，与索引无关。
- **公共 API 修正**：`include/go2cpp/runtime.hpp` 真身为 36 行、include 26 个模块头（含 `core/timer.hpp` 与 `network/*`）。`include/go2cpp/sync.hpp`（219 行）是根路径下**唯一非转发壳**的真实公共头，被 9 处引用；`include/go2cpp/concurrency/sync.hpp`（203 行）已与其发散且**全仓零引用**。
- **构建系统事实**：CMake 无 `-Werror`，也无任何开关可将其打开；警告策略仅 `-Wall -Wextra -Wpedantic`（`cmake/Go2CppModules.cmake:119`、`cmake/Go2CppTests.cmake:29,39,48,79`）。`GO2CPP_NATIVE_CONTEXT` 在 Linux x86_64 置 ON，编译 `src/concurrency/context_x64.S` 并定义 `GO2CPP_USE_NATIVE_CONTEXT=1`（同文件 `:33-35`）；网络源文件并入 `go2cpp_io`，`docs/modules.md:11` 所称 `go2cpp_network` target 并不存在。
- **Fiber 上下文后端判定（源码 + 实测）**：本轮构建实际产出 `context_x64.S.o`，且 `flags.make` 含 `-DGO2CPP_USE_NATIVE_CONTEXT=1`；`src/concurrency/fiber.cpp:6-9` 仅在未定义该宏时才引入 Boost.Context。即**当前默认后端是自研 x86_64 汇编，不链接 Boost.Context**。据此判定 `docs/design.md:221`、`docs/boundary_audit.md:30`、`docs/go_reference_audit.md:135`、`docs/performance.md:27/65/144/191` 为过期表述；其中 `docs/design.md:221` 与其自身 `:121-130` 直接冲突，`docs/boundary_audit.md:30` 与其自身 `:25` 冲突。
- **构建实测**：`cmake -S . -B build-orientation -DCMAKE_BUILD_TYPE=Debug` → `cmake --build build-orientation --parallel` 成功，**31 个 target，零警告零报错**；构建目录被 `.gitignore:2` 覆盖，未入库。
- **测试实测**：`ctest --test-dir build-orientation --output-on-failure` → **18 个用例，17 通过 / 1 失败**（25.21 s）。失败项 `go2cpp_dynamic_gmp_demo`，输出 `[gmp][场景3] cancel_requested=0 cancel_finished=1 body_ran=1 cancelled=0`。复现：直接运行 5 次 rc = 1,1,0,1,0；ctest 单跑 5 次 = 通过/通过/失败/失败/失败；全量 ctest 连续 2 轮均 18/18 通过 → **间歇性、与负载相关**。
- **失败根因（已定位到行）**：`example/concurrency/dynamic_gmp_demo.cpp:99` 的 `cancel()` 在任务已进入 Running 时返回 false，而 `:104-105` 把 `cancel_requested==true` 与 `state()==kCancelled` 同时当作硬条件，`:114-115` 又把 `cancel_ok` 计入返回码。属**过度断言 + 竞态窗口预期**，非运行时缺陷。另与 `example/README.md:42` 声称的"最终收敛为 `Cancelled`"矛盾。
- **技术栈确认**：cmake 3.28.3、gcc 13.3.0、12 核；源码 `.cpp/.S` 合计 21,938 行、头文件 9,771 行，最大文件 `src/concurrency/scheduler.cpp`(4048)。
- **发现的其他不一致**：`benchmarks/io_coost_fair_benchmark.cpp` 未被任何 CMake target 引用（孤儿文件）；`src/concurrency/context.cpp:255` 与 `src/concurrency/timer.cpp:84` 各有一个同名 `TimerService`；`docs/performance.md` 内部混合锁/IO 倍率存在两个数量级的自相矛盾（`:140` 快 10~18 倍 vs `:42-46` 8.5~8.8 s；`:141` 慢 3.0~4.3 倍 vs `:184` 0.59x）。
- **未覆盖范围**：未跑压测/随机测试/基准；未运行任何 sanitizer；未验证历史验证数字；未逐行通读 scheduler.cpp/io.cpp/hook.cpp/channel.hpp 等大文件；只测 Debug+共享+Hook-ON 一种构建矩阵；未校验 third_party 归档 SHA-256；未探查 `/mnt/e/...` 下文档引用的 coost 路径。以上均按"未确认"留白，不作结论。
- **清理**：临时脚本置于工作区 `.dsh_tmp/`（非项目内），构建产物在 `build-orientation/`（已被忽略）；项目根 `log/` 两个文件的 md5 与 mtime 均未变化。`git diff` 指纹前后同为 `355c390592702d857518763895db93287a98d1dbd8546d53a04282285fdcd9b4`，用户 3 处在途改动零丢失。

## 2026-10-02：全模块职责与后端边界复核

- 调度器增加 `SchedulerObserver` 事件协议。G/M/P 状态机仍由 Scheduler 独占，
  观察器只接收启动、worker、任务运行/挂起/完成/失败事件；观察器指针独立加锁，
  回调在调度器状态锁外执行并隔离异常，可接入日志、指标和追踪而不把业务代码
  嵌入 worker 热路径。
- IOManager 把 epoll/eventfd 抽成 `IOBackend`，新增线程安全的命名工厂和实例注入。
  默认 Linux epoll 保持原行为；后续 kqueue、IOCP、io_uring 只能实现 readiness、
  wait、wake、remove、shutdown 协议，不能直接恢复 Fiber 或修改 Context 状态。
- sync 和 Future 增加等待后端工厂，默认后端仍分别适配 Scheduler/ParkingCondition；
  已创建等待节点固定自己的后端，插件工厂在锁外执行，防止注册表重入和锁反转。
  HybridGate 的自旋次数改为可配置实例策略，仍只允许保护短临界区。
- error 增加 `WalkErrors` 环安全遍历扩展点，日志、指标和错误转换插件不再依赖
  WrappedError/JoinError 的具体类型；异常和环图均在错误边界内隔离。
- 新增 `docs/extensibility.md`，明确批次事务、Fiber/栈后端、跨平台 IO、插件
  生命周期和故障注入测试协议；README、design、scheduler 文档同步链接和说明。
- 验证：Release CTest 14/14 通过；`build-engineering-werror` CTest 3/3 通过；
  自定义 IO/Future 后端、调度器观察器、错误遍历和原有高负载测试均通过。

## 2026-10-02：日志 Item 插件化与配置事务解耦

- `LogFormatter` 新增 Sylar 风格的 `Item` 扩展边界。格式字符串在
  `PatternFormatter` 构造时预解析成 Item 链，日志热路径不再查表或解析文本；
  `AddFormat`/`addFormat`、`RemoveFormat`、`HasFormat` 和 `Formats` 由线程安全
  注册表提供，工厂在注册表锁外执行，允许插件在工厂中继续注册或查询格式项。
- 内置 `{time}`、`{function}`、`%d{...}`、`%p/%m/%N/%T/%n` 等格式均通过同一
  工厂协议创建；重复键拒绝覆盖，已构造 Formatter 持有自己的 Item 快照，注销
  不会破坏正在使用的 Formatter。新增无选项工厂和带 option 构造函数的模板接口，
  便于新手接入自定义字段。
- 配置中心增加 `ApplyString`/`NotifyPending`/`RollbackPending` 两阶段接口和
  `Config::AddCommitListener`。文件加载先整体校验，再整体发布值，失败恢复旧快照；
  监听器逐个隔离异常，变量监听器和提交级回调均在事务锁释放后执行，避免插件回调
  触发配置加载时发生锁反转。配置文件的编辑锁、临时文件和原子 rename 约束同步
  补充到配置文档；未来批次、Fiber 栈和多平台 IO 的替换协议补充到设计文档。
- 新增自定义 Item、重复注册、注销后快照、日期内置键保护、配置事务回滚和完整提交
  回调测试；更新 `example/log_config_demo.cpp` 和 `docs/logging.md` 新手示例。
- 验证：`build-final` 全量 CTest 14/14 通过；`build-engineering-werror` 全量
  CTest 3/3 通过；两套构建均使用现有项目警告策略，`git diff --check` 通过。

## 2026-09-29：调度器、混合锁与 Coost 对比

- 本地 P 队列成功取到 G 后跳过调度器全局 admission 锁；G/M/P 计数统一在
  两条取任务路径之后更新，避免本地快路径遗漏运行计数。
- `maybe_grow()` 改为有界维护周期，同时保留 runnable backlog 下的初始 worker
  补齐，修复并发 shutdown 测试中第二个 G 被取消而无法执行的问题。
- Scheduler Fiber 恢复增加首次父级绑定和后续快速恢复入口；同一 M 上不再
  每次重复获取 Fiber 父链元数据锁，迁移到新 M/P 时仍更新调试绑定。
- 混合 Mutex 增加有界 Fiber 协作重试，并在已有 FIFO 等待者时禁止无序快进；
  短临界区基准从约 13.9 秒降至约 0.10～0.20 秒。
- `src/sync.cpp` 的无 Context 竞争等待改用调用栈上的 `WaitNode`，队列保存裸
  指针；带 Context 的等待继续使用线程本地缓存的拥有节点。该路径参考了本地
  Coost 的嵌入式等待节点设计，但保留 Go2Cpp 的取消和 Fiber park 语义。
- 新增并更新 `docs/performance.md`，记录本机 Coost checkout（HEAD
  `c1cc11b`，工作区 dirty）的可复现对照：调度 13～52 ms 对 436～497 ms，
  混合 Mutex 1,097～1,766 ms 对 99～107 ms，IO 486～569 ms 对 1,666～2,098 ms。调度和
  IO 仍有差距，混合锁已反超；没有把 Coost README 的日志数据冒充调度器或锁
  基准。
- 验证：`go2cpp_high_load_stress` 完成全部检查；调度器定向测试连续 3 次
  通过；工程化构建 CTest 13/13、`-Werror` CTest 3/3、ASan+UBSan
  `go2cpp_tests` 全部通过；本轮 Valgrind 按 `sync/scheduler/dynamic/channel/io`
  分模块运行，均为 0 errors、0 definite/indirect/possible leak；完整测试在
  Valgrind 下被 IO/动态日志的内置 watchdog 超时终止，不能作为全量 Valgrind
  通过结论。

## 2026-09-26：Context 局部回滚

- 新增 `ContextRollback`/`RollbackScope`：临时子 Context、LIFO undo、savepoint、
  显式 rollback/commit 和独立 `RollbackDone()` 完成通知。
- Context 独立取消现在会从父节点弱登记表中移除 child；deadline timer 注册失败
  不再留下永远等待的 Context。`WithValue` 的异常路径会取消并清理半发布 child。
- undo 只表示局部补偿，不恢复父 Context 或外部 IO；回调在线程锁外执行，异常隔离
  并记录为 `kFailed`。取消先发布 child `Done`，再执行内部 undo，普通 Done 观察回调
  在 undo 后运行；undo 不得等待自身 `RollbackDone()` 或尚未发布的信号。仍禁止
  longjmp、跨 Fiber/线程跳转和在回调中阻塞。
- Context 取消新增前置 claim：在 child `Done` 可观察前冻结 action 日志，避免
  取消窗口追加/提交新动作；未取消的短命 child 会从 parent 的 weak 登记表及时移除。
- 新增 ContextRollback 单元覆盖 savepoint LIFO/ABA、析构/父取消、deadline、移动
  所有权、managed P=1、异常隔离、取消与显式 rollback 竞态；新增
  `example/context_rollback_demo.cpp`，详细契约见 `docs/context_rollback.md`。
- 验证：Debug 示例 CTest 11/11；Release 2/2；`-Werror` 2/2；ASan+UBSan 2/2；
  Context 定向重复 30/30；TSan 使用 `setarch x86_64 -R` 的 context 过滤通过，
  全量 TSan 在 WSL native waiter watchdog/地址映射限制下不作为全量通过结论；
  Valgrind Memcheck `ERROR SUMMARY=0`，definite/indirect/possible 均为 0，
  416B/4 blocks 为进程级 still reachable 缓存。
>
> 当前快照说明（2026-09-23）：以下早期记录是追加式审计历史，其中的
> panic/defer/recover 示例和模块名称不代表当前 API。当前源码已删除
> `panic_defer` 模块、`SendOrPanic`/`CloseOrPanic` 和对应测试；Fiber 只
> 捕获并记录普通 C++ 异常，使用 RAII/`try/catch`。当前交付还包含失败终态、
> 有界 `shutdown_for()`、常见 Hook/sysmon 边界、Fiber claim 析构栅栏以及
> 错误父级的 Failed 放弃路径。

> 本轮收尾还修复了手动 Fiber 在已取消 Context 上等待条件变量的 carrier 阻塞竞态，
> 将 `Channel<T>` 限制为不抛 move/析构并使 `Close()` 不抛，且修正 Fiber 快照中
> main_fiber 的 active 标记。最终 Debug/Release/ASan+UBSan/Valgrind 均通过；
> TSan 在本 WSL 的映射或 native waiter watchdog 约束下未作为通过结论。

## 2026-09-20：新手接口与混合等待边界

- 新增 `go2cpp/go.hpp`：显式/默认 `go()`、默认 Scheduler 生命周期、关闭后新实例重启、轻量小写 `fiber` 和 `Scheduler::add()` 接入。默认调度器使用进程生命周期状态锚点；`shutdown_default_scheduler()` 会移出旧句柄，避免向终态 Scheduler 排队。
- 新增 `go2cpp/event.hpp`：`EventBatch`/`SelectLoop` 薄封装已有 channel `Select`，用协程感知 `sync::Mutex` 串行 `work()`，native 线程和 Fiber 可并发调用（work 本身串行）；handler/Select 异常转为显式错误结果，不强制销毁挂起 Fiber；stop 通过内部 Context 唤醒等待。
- 新增 `tests/test_timer.cpp`，覆盖 P=1 下 TimerService 回调唤醒 Fiber、`Channel::WaitForChange` 不占 M、定时器取消；新增 `tests/test_beginner_api.cpp` 和 `example/beginner_demo.cpp`，覆盖 go/fiber/select、Fiber 内等待、native/Fiber 混用、handler 异常和默认调度器重启。
- `runtime.hpp` 导出新手头文件；CMake/CTest 注册新测试和示例。
- 本次验证：GCC Debug Hook-on 9/9，GCC Release 静态 Hook-off 8/8，`-Werror` Debug Hook-on 9/9，`GO2CPP_TEST_FILTER=beginner` 和 `timer` 均通过。
- 明确边界：Timer 回调在 native 定时器线程执行，只通知等待节点，不直接恢复 Fiber 栈；任意阻塞业务必须经 `go()`/`Scheduler::spawn()`，EventBatch 事件列表生命周期由调用方保证。
- 追加修复：`EventBatch::make_stop_config()` 现在把剩余总时长传递给当前 `Select`，空 channel 在总时长到期时不会无限等待；新增 20ms watchdog 回归。
- 追加修复：EventBatch 增加内部 stop Context；`stop()` 会取消 DoneSignal 并唤醒正在 Select 的 Fiber，按正常返回路径结束，不强制销毁挂起栈；新增 P=1 stop 唤醒回归。
- Release CTest 首次运行出现一次 `mixed_runtime_demo` native-owner watchdog 超时，随后独立运行 5 次并完整 CTest 重跑均通过；记录为宿主资源抖动，未观察到可复现代码回归。

# Change log

## 2026-09-15

- Initialized the standalone Go2Cpp C++17 runtime project in `/UserData/CodexWorkSpace/Go2Cpp`.
- Added the design and compatibility matrix describing GMP, context, channel, error and explicit panic/defer boundaries.
- Added the umbrella CMake runtime target, warning policy, install/export rules, and test/example options.
- Added `docs/testing.md` with reproducible Debug/Release/Clang, sanitizer,
  Valgrind, example, watchdog and stress-test commands.
- Linked the verification matrix from `README.md` for discoverability.
- Key documentation/test files for this verification unit: `docs/testing.md`,
  `README.md`, `codex.md`, and `tests/test_scheduler.cpp`.
- Verification recorded on 2026-09-15: GCC Debug and Release, Clang 18
  Debug, ASan, UBSan, CTest (2/2), the runtime example, and Valgrind Memcheck
  passed. The final Memcheck runs found zero errors and zero bytes in use at
  exit for both the unit suite and scheduler smoke test (661,087 and 846
  allocations/frees in the final sample; allocation counts can vary with the
  libc/thread implementation).
- TSan configured and compiled, but cannot start in this WSL image because of
  `ThreadSanitizer: unexpected memory mapping`; no TSan race conclusion is
  claimed.
- Synchronized the self-referential scheduler test task publication before
  callback access (assignment before enqueue plus a release/acquire gate);
  200 full-suite runs and 200 scheduler-smoke runs passed with a ten-second
  watchdog per process. The smoke test's always-evaluated checks are warning
  clean in Release as well as Debug.
- Added iterative context cancellation and iterative parent-state release so
  deep cancellation/destruction chains do not consume the native call stack;
  the 20,000-node test and an independent 100,000-node audit passed.
- Added explicit expired-deadline behavior, root-context cancellation
  protection, channel cancellation-priority coverage, panic-body exception
  translation, and the scheduler yield-then-unhandled-panic regression.
- Restricted scheduler `yield`/`park` to the current G and finalized unbound P
  states as dead during shutdown; sparse-P and never-started shutdown tests now
  cover those transitions.
- Made `error::Is`/`As` chain traversal iterative and added a 100,000-layer
  wrapping regression; built-in wrapper release is iterative, while custom
  error ownership/message graphs remain required to be acyclic.
- Split CMake into option/module/test/install files and verified umbrella,
  individual-module, and relocated-prefix package consumers; all configured,
  built, and exited zero. The final smoke and full-suite watchdog runs each
  completed 200/200 iterations.
- Status: implementation and stress verification are complete for this
  snapshot; native-Linux TSan remains a follow-up because the WSL image cannot
  start the instrumented process.

## Pause handoff (2026-09-15)

The requested pause is at the pre-commit handoff. The latest canonical WSL
source includes the fake-clock expiry fix, named/typed context-key isolation,
current-G-only scheduler yield/park admission, sparse-P shutdown finalization,
iterative built-in error-chain release, and panic-handler exception isolation.

Latest completed gates:

- `build-final-modular`: GCC Debug build and CTest 2/2 passed; the example
  prints the GMP/channel/error/panic demonstration output.
- `build-final-asan` and `build-final-ubsan`: rebuilt after the latest runtime
  changes; the full unit executable passed with no diagnostics.
- Valgrind Memcheck on the unit and scheduler-smoke executables: both exit 0,
  `0 bytes in 0 blocks`, `ERROR SUMMARY: 0`; the sampled allocation counts
  were 661,087/661,087 and 846/846.
- Watchdog stress: `go2cpp_tests` 200/200 and
  `go2cpp_scheduler_smoke` 200/200 passed, with a ten-second timeout per run.
- The existing package-consumer audit passed for the umbrella target,
  individual module targets, and a relocated install prefix; its temporary
  source/install directories were removed.

## Resume verification (2026-09-16)

- Reconfigured and rebuilt GCC Release after the final context/panic edits;
  CTest passed 2/2 and the runtime example exited zero with the expected
  GMP/channel/error/recover output.
- Reconfigured with `CC=clang CXX=clang++`, rebuilt with Clang 18, and passed
  CTest 2/2. Both final compiler configurations were warning-clean under the
  configured `-Wall -Wextra -Wpedantic` policy.
- Rechecked the Go `go1.23.0` source archive against
  `third_party/go-reference/SHA256SUMS`; SHA-256 verification passed.
- Remaining handoff action is repository staging/diff review and the requested
  Git commit. Native-Linux TSan remains an external environment follow-up.

Known boundaries to preserve in the final report: cooperative rather than
asynchronously preemptive scheduling, native blocking calls occupy their M,
polling/API-based select, explicit nil-channel status instead of an infinite
nil-channel block, copyable values for `SendCase`, no Go compiler/ABI/GC, and
the documented inability of a C++ throw during explicit `Frame` destruction
to be recovered by that frame. Custom error ownership/message graphs must be
acyclic. The default WSL TSan invocation remains loader-sensitive and can report
`ThreadSanitizer: unexpected memory mapping` before the test starts; the final
non-PIE/ASLR-disabled WSL run is recorded below, and a native Linux gate is
still recommended.

## Finalization (2026-09-16)

- Added a persistent per-Scheduler owner token to `Task`; a G is bound to the
  first scheduler that admits it, and foreign `enqueue`/`wake` calls are
  rejected without changing the task state. Added the cross-scheduler
  ownership regression in `tests/test_scheduler.cpp`.
- Added the full-buffer select-send regression in `tests/test_channel.cpp`.
  It verifies that draining a full capacity-1 channel refills the slot from an
  armed select sender and preserves FIFO order.
- Updated `docs/design.md`, `docs/scheduler.md`, and
  `docs/compatibility.md` with the ownership and bounded-wait contracts. Added
  `docs/dependencies.md` with the Go provenance and GC/ABI/stack/cgo
  replacement inventory; linked it from `README.md`.
- Rebuilt and tested the final source with GCC 13 Debug (`build-final-modular`),
  GCC 13 Release (`build-final-release`), Clang 18 (`build-final-clang-env`),
  and a strict `-Werror` build (`build-final-werror`). Every CTest run passed
  2/2; the example printed the GMP, select/directional-channel, error and
  recover output.
- Rebuilt ASan and UBSan configurations and ran both unit and scheduler-smoke
  executables with no diagnostics. Valgrind Memcheck reported zero errors and
  zero bytes in use at exit for both binaries (1,161,500 and 983 allocations/
  frees in the recorded sample). The Go archive checksum verified `OK` from
  `third_party/go-reference`.
- TSan compiled successfully. The default WSL layout can fail at process
  startup with `unexpected memory mapping`; running the non-PIE build through
  `setarch x86_64 -R` allowed both test executables to exit zero with no TSan
  report. A native Linux runner remains the recommended independent gate.
- Watchdog stress completed 200 `go2cpp_tests` runs and 300
  `go2cpp_scheduler_smoke` runs, each with a ten-second process timeout. The
  refreshed `build-final-install` prefix contains all module/umbrella targets,
  headers and package configuration files.
- Status: all requested compatibility-subset modules, examples, tests and
  documentation are complete for this snapshot. Remaining work is optional:
  run TSan on a native Linux kernel/loader and expand the explicit API if a
  future translator needs semantics outside the documented subset.

## Final audit continuation (2026-09-16)

- Closed the scheduler admission window identified during review: queue
  removal, runnable accounting, stop observation, and the Task execution claim
  now occur under one scheduler admission transaction. Added regressions for
  duplicate enqueue during a yielded callable and wake-after-park before the
  callable returns.
- Made internal requeue/wake failure handling idempotent with
  `cancel_if_runnable_unqueued()`. A concurrent successful public enqueue is no
  longer mistaken for a failure and cannot be cancelled by the worker.
- Added terminal task-registry pruning and moved shutdown queue-task
  destruction outside scheduler/P/join locks. The smoke suite now verifies a
  task capture whose destructor re-enters the Scheduler; it exits under the
  ten-second watchdog.
- Refreshed the example to print context value/cancellation and error identity
  checks. The final example output and exit status are successful.
- Final GCC Debug and `-Werror` CTest runs passed 2/2 after the audit fixes;
  ASan, UBSan and non-PIE/ASLR-disabled TSan were rebuilt and passed both test
  executables. Valgrind Memcheck reported zero errors and zero bytes in use at
  exit: unit `1,161,500` allocs/frees and smoke `983` allocs/frees.
- The watchdog loop passed 200 unit runs and 300 scheduler-smoke runs after
  the audit fixes. The installed umbrella, individual-target and relocated
  package consumers all exited zero. `sha256sum -c` still reports `OK` for the
  pinned Go archive.
- No required implementation task remained for the compatibility subset
  documented at that snapshot. Its then-known future work was limited to a
  native-Linux TSan gate and translator/ABI features outside this standalone
  C++17 library. The following boundary audit supersedes that statement for
  the newly requested Fiber, socket-hook, coroutine-sync, and dynamic-M scope.

## Boundary audit and HOOK clarification (2026-09-17, superseded)

This historical snapshot predates the implementation below. Its "not
implemented" conclusions are retained for audit history; use the 2026-09-18
section for the current status.

- Rechecked the four newly raised boundary cases against the source, tests and
  public headers. A returning/panicking callable or a cancelled queued/waiting
  task reaches a terminal G state, and queue/wake/shutdown races are covered.
  This does not force-unwind a running C++ stack or destroy a `Task` retained
  by user ownership. The scheduler does not preserve a C++ stack across
  `yield`/`park`; a callable that never returns cannot be resumed by this
  backend, and a blocking syscall occupies its M.
- The socket requirement is **not implemented**. There is no `IOManager`, FD
  readiness backend, `addEvent`/`cancelEvent`, `wait_for_event`, or syscall
  hook registry. The requested hook semantics include original-syscall
  fallback, readiness-before-timer registration, idempotent timeout/cancel
  wake-up, nonblocking-mode handling, FD reuse generations, and
  FD-close/`EINTR`/`EBADF` rules. Required transparent interception covers
  connect/accept, read/recv, write/send, close, fcntl/ioctl, and socket timeout
  options; explicit wrapper-only I/O is not sufficient.
- Coroutine-level `Fiber`, mutex, condition-variable and waitgroup facilities
  are not implemented. Existing `std::condition_variable` instances are
  internal OS-thread coordination, not coroutine suspension. A stackful-fiber
  backend or explicit callback/state-machine API is required.
- M management is fixed at `Scheduler::start()`: one stable worker per
  configured slot, with local/global queues and stealing. There is no dynamic
  scale-up/scale-down, blocked-syscall handoff, idle-M reclamation, or
  same-task-type thread-cache contract.
- Status: the existing compatibility subset remains intact, but these four
  boundary requests are only partially satisfied. The gaps are now explicit
  in `docs/compatibility.md` and `docs/design.md`.

## Boundary implementation and final verification (2026-09-18)

- Replaced the earlier callback-only scheduler path with a stackful
  Boost.Context Fiber backend. `Task` now owns a resumable C++ continuation;
  G/M/P admission, local/global queues, bounded stealing, park/wake pending
  permits, owner binding, dynamic M growth/shrink and task-class affinity are
  implemented in `src/scheduler.cpp`. Shutdown requests cooperative
  cancellation, wakes parked Gs, waits for started Fibers to return through
  their trampoline, releases completed Fiber stacks immediately, and never
  force-discards a live C++ stack.
- Added scheduler-aware `sync::Mutex`, `ConditionVariable` and `WaitGroup`,
  shared `ParkingCondition` and process-wide timer service. Managed G waits
  suspend their Fiber and leave the M available; unmanaged contended mutex
  calls deliberately return false/throw rather than silently blocking an
  unknown OS thread.
- Added Linux `IOManager`/epoll/eventfd readiness and the shared default-on
  socket hook. The hook covers connect/accept/read/recv/write/send,
  close/dup/fcntl/ioctl, socket timeout options and sleep. It registers
  readiness before deadline timers, uses one-shot outcomes and generation
  tokens, and has original-call/native-poll fallbacks. A boundary audit fixed
  `dup2`/`dup3` pre-replacement close notification, `dup2(fd, fd)` identity,
  native fallback `ETIMEDOUT`, and extreme-deadline conversion overflow.
- Added runnable examples under `example/`: `fiber_sync_demo.cpp`,
  `managed_pipeline_demo.cpp`, `dynamic_gmp_demo.cpp` and `io_hook_demo.cpp`.
  Added saturating relative-deadline arithmetic for Context and channel/select
  APIs, with regression coverage for `ContextDuration::min()/max()`.
  Updated the compatibility/design/dependency/scheduler/testing documents to
  distinguish implemented semantics from unsupported Go/compiler/ABI behavior.
- Current verification from the canonical WSL path:
  GCC Debug shared CTest **7/7**, Clang 18 Debug **7/7**, Release static
  Hook-off CTest **6/6**, strict `-Werror` CTest **7/7**; ASan and UBSan full
  unit/smoke runs passed; Valgrind unit and smoke runs both exited zero with
  `0 bytes in 0 blocks` and `ERROR SUMMARY: 0` (final alloc/free totals
  `1,883,691` and `3,387`); `(cd third_party/go-reference && sha256sum -c
  SHA256SUMS)` returned `OK` for the Go 1.23.0 archive; install/export smoke
  completed; serial watchdog stress passed
  `go2cpp_tests` **40/40** and `go2cpp_scheduler_smoke` **80/80**.
- TSan was rebuilt with non-PIE/ASLR-disabled WSL invocation. Filtered
  context/channel/scheduler/dynamic/fiber/sync/io runs passed. The full suite and hook filter
  exceeded the WSL watchdog without a diagnostic, so no full-suite TSan pass
  is claimed; native Linux remains the appropriate final TSan gate.
- Remaining explicit boundaries: no Go parser/compiler/GC/ABI, no
  asynchronous preemption or blocking-region M>P handoff, cooperative rather
  than forced Fiber cancellation, no exact Go fairness, and no transparent
  interception of every Linux socket-adjacent API (`poll`/`select`/`ppoll`,
  `*mmsg`, `sendfile`, `splice`, and signal-restart policy remain outside the
  subset). Static consumers must disable the shared Hook target.

## Final boundary regression and handoff (2026-09-18)

- Fixed cancellation/shutdown lost-wake windows for unstarted queued Gs and
  for advanced raw `Fiber::Suspend(Park)` users. Failed queue claims now prune
  terminal tasks and release callable captures; scheduler shutdown is notified
  when an external cancellation changes the terminal predicate. Fiber creation,
  pointer publication, resume observation and terminal ownership transfer are
  serialized against cancellation by the Task transition mutex.
- Added regressions for cancelled-queue capture release, raw Fiber park
  cancellation, managed `MSG_OOB` (`ENOTSUP`), tracked `FIONBIO` invalid-pointer
  `EFAULT`, empty/inert select, singleton `errors.Join`, context callback
  subtree marking, and zero/negative timed mutex acquisition.
- Hook boundary fixes include applying the configured connect timeout to native
  fallback, kernel-first `FIONBIO` validation, split close lifecycle locking
  around potentially blocking `SO_LINGER`, and explicit documentation for
  raw syscall/`io_uring`/`close_range`/fork-exec gaps. Ordinary-thread
  `MSG_WAITALL` on a previously adopted runtime-nonblocking descriptor remains
  a known semantic limitation; managed `MSG_WAITALL` and `MSG_OOB` return
  `ENOTSUP`.
- Earlier current-tree verification baseline: GCC Debug shared **7/7**, Release static
  Hook-off **6/6**, Clang 18 Debug **7/7**, and strict `-Werror` **7/7**;
  ASan and UBSan unit/smoke runs passed. Valgrind Memcheck logs
  `build-check/valgrind-tests-final3.log` and
  `build-check/valgrind-smoke-final3.log` both exited zero with 0 bytes in
  use, ERROR SUMMARY 0, and allocation totals `1,908,203` and `3,388`.
  The final serial watchdog run passed 40 unit and 80 smoke executions.
- TSan filtered runs completed earlier for context/channel/scheduler/dynamic/
  fiber/sync/io without a diagnostic; the rebuilt final WSL run did not finish
  within the watchdog, so no full-suite or hook TSan pass is claimed. Native
  Linux TSan remains a release-gate follow-up. Channel destruction, throwing
  select element/callback rollback, static-destruction ordering for global
  contexts, and owner lifetime for raw `IOManager*` bindings remain explicit
  documented preconditions or follow-up work.
- After the final `IOManager::wait` park-failure guard and select-formatting
  cleanup, Memcheck was rerun on the current tree. `valgrind-tests-final4.log`
  and `valgrind-smoke-final4.log` both exited zero with `0 bytes in 0 blocks`
  and `ERROR SUMMARY: 0`; allocation totals were `1,815,429` and `3,388`.
- After the public pre-cancelled-`Task` admission cleanup and its regression,
  Memcheck was rerun again on the current tree. `valgrind-tests-final5.log`
  and `valgrind-smoke-final5.log` exited zero with `0 bytes in 0 blocks` and
  `ERROR SUMMARY: 0`; allocation totals were `1,809,665` and `3,390`.

## Boundary API follow-up (2026-09-20)

- Extended the public documentation and runnable examples for the mixed native
  thread and managed Fiber boundary. Added mixed_runtime_demo.cpp and registered
  it as a CMake example/test target.
- Documented scheduler-aware mixed Mutex, ConditionVariable and WaitGroup
  semantics, including native condition-variable wait nodes, non-thread-owned
  unlock, timeout/cancellation expectations, and the prohibition on holding a
  thread-owned std::mutex across Fiber migration.
- Documented FiberLocalCache/FiberLocal value lifetime and migration behavior,
  per-thread hook and GMP participation scopes, and the intentional boundary
  that external participation records policy only and does not attach an M or
  run a queue.
- Documented BlockingRegion replacement-M admission. Known interposed socket
  and sleep fallbacks account for native blocking automatically; arbitrary
  unhooked native calls still require the explicit RAII region and remain
  cooperative.
- The post-boundary Debug build in build-docs-check compiled 57 targets; CTest
  passed 8/8, and the mixed example passed 20/20 watchdog runs. The Hook-off
  Release static build in build-docs-static passed 7/7, including the mixed
  example. Final sanitizer, Valgrind and compiler reruns are recorded below.

## Final verification after boundary hardening (2026-09-20, superseded)

- Made BlockingRegion non-movable and bound its cleanup to the entering M id.
  The destructor repairs the captured M even if an invalid Fiber migration
  occurs; the public contract still forbids yielding or parking inside the
  region. Native close fallback now uses the same blocking accounting.
- Worker participation scopes now preserve the scheduler-owned worker metadata
  when nested inside a runtime M. External participation remains an explicit
  policy/eligibility scope only; it does not attach an M or run a queue.
- Managed hook admission without an IOManager now lazily adopts sockets and
  uses the bounded native poll/deadline path when hooks are enabled. Explicitly
  disabled hooks preserve native blocking semantics and use BlockingRegion
  accounting. The recursion guard remains active.
- Added the plain-Scheduler lazy-adoption timeout regression and the worker
  nested-policy regression. The ASan test race that asserted Task::state
  immediately after a completion counter was made watchdog-based.
- Made the context deadline TimerService an intentionally process-lifetime
  singleton, removing static Context/TimerService destruction-order UAF risk;
  an exit callback stops and joins the service thread while the intentionally retained service object remains valid.

Final verification from /UserData/CodexWorkSpace/Go2Cpp:
- GCC Debug Hook-on CTest 8/8; Release static Hook-off 7/7; strict Werror 8/8;
  Clang 18 8/8.
- ASan and UBSan full unit/smoke runs passed with the documented environment
  variables.
- Memcheck logs build-check/valgrind-boundary-tests-final4.log and
  build-check/valgrind-boundary-smoke-final4.log exited zero with ERROR SUMMARY
  0. Definite, indirect and possible leaks were all zero. Intentional process-lifetime FiberLocal registry and context-timer service storage remained reachable: 360 bytes in three blocks for the unit run and 96 bytes in one block for smoke. The native context waiter now has a 2-second `WaitFor` watchdog before join.
- Watchdog stress passed go2cpp_tests 40/40 and go2cpp_scheduler_smoke 80/80.
  Rebuilt TSan scheduler, dynamic, and fiber filters passed with `setarch x86_64 -R`; the full suite exceeded a 120-second Python watchdog without a diagnostic, while the normal WSL invocation reports `ThreadSanitizer: unexpected memory mapping`. Native Linux remains the TSan gate.
- Install/export smoke passed with the current `build-install-final` tree and a temporary package consumer.
- Known semantic boundaries remain: no exact Go P handoff or asynchronous
  preemption, no external-thread attach/run-one backend, no Fiber stack pool,
  and no guarantee of same-task-type cache reuse. Arbitrary unhooked native
  blocking still requires an explicit BlockingRegion; poll/select/mmsg,
  sendfile/splice, io_uring and similar socket-adjacent APIs remain outside
  the hook subset.


## Final boundary hardening and race-detector follow-up (2026-09-20)

- Added watchdog-safe test infrastructure in tests/test_support.hpp:
  bounded RequireEventually, aborting Require, and thread joins guarded by
  a separate watchdog thread. Long test Fiber loops use a 1 ms pause only when
  running under Valgrind, so Memcheck cannot starve the coordinating test
  thread while TSan keeps the original yield behavior.
- Hardened tests/test_scheduler.cpp and related module/smoke/example tests:
  cancellation and publication waits have absolute deadlines, critical waits
  fail fast instead of continuing into unsafe teardown, and the cancelled
  queue-capture case uses a strict one-worker/no-oversubscription scheduler.
- TSan found a real race during this pass: dynamic M growth relocated
  Scheduler::Impl::machines vector slots while a managed worker performed
  BlockingRegion lookup. The machine record container is now a deque, so
  published shared-pointer slots are not relocated during append; scheduler,
  dynamic and fiber TSan filters then passed repeatedly. This is reflected in
  src/scheduler.cpp and docs/scheduler.md.
- Final GCC Debug/Release, strict Werror, Clang, ASan and UBSan CTest matrices
  passed: 8/8 Debug, 7/7 Release static, 8/8 Werror, 8/8 Clang, 8/8 ASan,
  and 2/2 UBSan unit/smoke.
- Valgrind final8 logs passed with zero definite/indirect/possible leaks:
  unit 1,516,975 allocs / 1,516,972 frees and 360 bytes in three
  intentional reachable blocks; smoke 3,406 / 3,405 and 96 bytes in one
  intentional FiberLocal-registry block. Both report ERROR SUMMARY: 0.
- Final stress passed 10 unit and 10 scheduler-smoke executions with a
  45-second process watchdog. Install/export consumer smoke printed 7.
- Remaining boundaries are unchanged and intentional: no exact Go P handoff or
  asynchronous preemption, external participation is metadata-only, FiberLocal
  is a logical value cache rather than a Folly stack/object pool, arbitrary
  unhooked blocking needs explicit BlockingRegion, and the WSL full-suite
  TSan gate remains incomplete pending native Linux.

## Shutdown lock hardening (2026-09-20)

- External shutdown now moves each published `std::thread` out while holding
  `Impl::mutex`, releases the scheduler mutex, and joins the moved thread. The
  final processor/state scan is also lock-protected. This removes the temporary
  snapshot allocation from the OOM-sensitive shutdown path while preserving
  the `join_mutex -> Impl::mutex` ordering.
- After this change, Debug CTest remained 8/8; dynamic, sync and scheduler-smoke
  watchdog runs passed. Memcheck final9 reported zero definite, indirect or
  possible leaks (unit 1,517,159 allocs / 1,517,156 frees; smoke 3,415 / 3,414)
  with only the documented 360B and 96B process-lifetime reachable blocks.

## Reaper lifetime hardening (2026-09-20)

- `reap_dead_workers()` now reserves its retired-thread vector before moving
  any joinable `std::thread`, and retains `join_mutex` through the actual joins.
  This makes the OOM path non-terminating and keeps external shutdown from
  observing a half-finished reaper maintenance pass.
- After the reaper change, Release Hook-off 7/7, strict Werror 8/8, Clang 8/8,
  ASan 8/8 and UBSan 2/2 passed; TSan scheduler and dynamic filters passed
  under `setarch x86_64 -R`. Final10 Memcheck again had zero definite,
  indirect or possible leaks: unit 1,560,975/1,560,972 allocations/frees and
  360B reachable; smoke 4,042/4,041 and 96B reachable; both `ERROR SUMMARY: 0`.

## 2026-09-21 恢复工作验证

- 重新确认 WSL 工作目录为 `/UserData/CodexWorkSpace/Go2Cpp`，Git 根目录、
  分支和工作区状态正确；恢复时未发现未提交改动。
- 使用已有 `build-check` 运行
  `ctest --test-dir build-check --output-on-failure --timeout 60`，当前 Debug
  Hook-on 测试 **9/9** 通过，包含单元测试、调度器、Fiber 同步、动态 M、
  混合 native/Fiber、入门 API 和 IO Hook 示例。
- 本次只做恢复审计和回归验证，没有引入新的实现缺口。未改变已记录的边界：
  WSL 环境仍不能把完整 TSan 套件作为最终通过依据；异步抢占、完整 Go
  编译器/ABI/GC 语义和未覆盖的 Linux socket API 仍属于明确限制。


## Sysmon 长系统调用交接（2026-09-23）

- 在 `src/scheduler.cpp` 与 `include/go2cpp/scheduler/scheduler.hpp` 增加可配置 sysmon 监控线程：`enable_sysmon`、`sysmon_interval`、`long_syscall_threshold`。
- `BlockingRegion` 和 Hook native fallback 发布 M 的阻塞起点；达到阈值后记录 detached/task/count，从 P 的 attached 计数逻辑解绑，并按 `min_workers + detached_count` 申请替代 M；原 M 返回时恢复 P 计数。
- detached M 存在期间提高空闲回收底线，避免替代 M 在原系统调用返回前被回收；worker epilogue 防止 attached 计数下溢；shutdown 在 drain 完成后停止并 join sysmon。
- 修复 `IOManager` 已存在但未跟踪 FD adopt 失败时直接进入 libc 的路径，使 managed G 仍进入 `BlockingRegion`；新增 pipe fallback Hook 回归。
- 更新 `docs/scheduler.md`、`docs/design.md`、`docs/compatibility.md`、`docs/go_reference_audit.md`、`docs/testing.md`，明确这是安全的逻辑解绑，不是 Go 精确 `_Psyscall` 或异步抢占；raw syscall/未 Hook 阻塞仍是边界。
- 验证：Debug Hook-off 2/2、Hook-on 9/9、dynamic 重复 10 次、ASan 2/2、UBSan 2/2、Valgrind dynamic 0 definite/indirect/possible lost；TSan 全量受 WSL + Boost.Context 映射限制，未宣称通过。


## Fiber 父链生命周期与 sysmon 边界加固（2026-09-23）

- `src/fiber.cpp` 引入共享 `FiberRecord` 父链元数据。父 Fiber 销毁后，子 Fiber 的诊断快照和取消遍历不再解引用裸父对象；共享记录保留 `alive=false` 墓碑。真正的挂起栈恢复仍要求固定父对象存活。
- Ready Fiber 析构可跳过尚未进入的用户栈；Suspended Fiber 在错误父级之外析构或传播失败会标记 Failed 并清理可安全清理的状态；挂起栈上的 RAII 只能由固定父级正常收尾。
- worker 任务边界检测未销毁的 `BlockingRegion` 并 fail-fast，防止跨 yield/park 的错误记账污染后续 G。
- 新增 Fiber 墓碑/唤醒后父链顺序回归和双长 syscall sysmon 回归。Debug、Hook-on、Release、Werror、Clang、ASan、UBSan、TSan（全量 3 次）与 Valgrind 均按 `docs/testing.md` 记录的命令通过。

## 2026-09-23 monitor 活性、嵌套 IO 与栈边界复核

本轮在既有 sysmon/Fiber 父链实现上继续复核并修正：

- src/scheduler.cpp：sysmon 的周期等待改用独立 sysmon_wait_mutex/sysmon_wait_condition，不再因 Scheduler 主锁被高负载路径占用而阻塞在等待入口；扫描阶段保留 try_to_lock，sysmon_pass_count 明确为心跳尝试计数。仍保持单 monitor 线程，M 扩容由已有有界策略负责。
- include/go2cpp/scheduler/scheduler.hpp、src/io.cpp：IO park 通过 park_io() 标记 SuspendReason::Io，方便诊断嵌套等待链。
- src/fiber.cpp：恢复路径在嵌套 scheduler park 后按返回状态恢复 TLS 当前 Fiber，防止父 Fiber 创建第二个子 Fiber 时错绑已完成的兄弟 Fiber。
- tests/test_hook.cpp：增加两层连续 timeout/ready、三层 Fiber 父链连续 IO、Fiber 快照深度和 send(MSG_NOSIGNAL) 系统错误回归；修正 phase=2 瞬时状态的测试观察方式，避免把合法快速推进误报为 watchdog。

结论保持谨慎：嵌套 Fiber 的单 FD IO、timeout、ready、cancel/close 唤醒和父级自然返回已验证；没有公共多 FD wait_any/Fiber select。sysmon 不能安全地跨线程终止或迁移 C++ Fiber，也不能注入系统级异常。Fiber 栈仍是带保护页的固定大小栈，没有 Go morestack/copystack 自动增长；需显式设置 fiber_stack_size。Hook 的 write/send 错误保持 libc errno，不隐式转 C++ 异常；SIGPIPE 仍遵循调用者的 POSIX 策略。

## 2026-09-23 最终复测修订

- Fiber 恢复路径新增栈对象 RAII TLS guard；任何嵌套 scheduler park、取消或早退都恢复进入 resume 前的直接调用者，避免 TLS 残留把已完成兄弟 Fiber 当作后续子 Fiber 的父级。
- 当前源码重编译后的 Hook/Fiber/dynamic 过滤器各 5/5，通过 Hook 9/9、Release 8/8、Werror 2/2、Clang 2/2、ASan 2/2、UBSan 2/2；TSan 在 setarch x86_64 -R 下当前全量单次通过，无 race 报告。
- WSL + TSan 重复全套仍可能在不同 native waiter watchdog 超时，不能据此宣称重复稳定；这属于测试环境风险，生产实现未通过放宽同步语义来掩盖。
- 最新 Memcheck 为 build-sysmon-hook/valgrind-final-current.log，definite/indirect/possible lost 均为 0，416B 为已有进程级 reachable 状态。

## 2026-09-24：显式控制流、Caster 与多 FD 等价层

- 新增 control_flow 模块：defer 是不可复制 RAII 对象，构造参数按值保存，析构或
  run_now() 按 C++ 作用域逆序执行一次；panic/recover 使用共享 PanicInfo 状态，
  recover 只在 defer 回调边界消费，回调异常通过 LastDeferException() 查询。
- 恢复旧 panic_defer 模块并不安全，因此没有使用 longjmp、信号异常或 C++ 内部
  异常伪造 Go 栈跳转。该模块是显式状态协议，调用者必须在 call() 后自行 return
  或分支；普通 C++ 异常、Fiber failure 和 error 保持独立。
- Channel select 增加 SelectValue、SelectCaster、MakeCaster、SetAny 和栈对象
  Cast 重载；std::any 仍保持兼容字段，不可复制源值可通过共享 holder 访问。
- IOManager 增加 WaitAny/WaitMany 及 timeout/cancel/close 组合；多个请求登记到
  epoll，普通线程明确返回 EPERM。修复一次多 FD readiness 竞态：Fiber 恢复前用
  零超时 poll 汇总仍就绪节点，再执行其余 waiter 清理。
- 本轮 Debug CTest 为 10/10；更多 Release、Werror、ASan、UBSan、Valgrind
  结果在 docs/testing.md 追加，未把 WSL 环境限制冒充 sanitizer 通过。

已知边界：recover 不提供 Go 编译器级 G/栈帧隔离，panic 不自动终止 Fiber 或展开
外层 defer；SelectCaster 的可变函数对象需要调用方自行保证并发安全；固定 Fiber 栈
仍未实现 Go 式动态扩容；poll/select Hook 保持 libc ABI，不自动改写为 WaitMany。


## 2026-09-24 最终边界复核

- defer/panic/recover 增加显式状态协议测试：参数注册时保存、LIFO、run_once/dismiss、普通异常作用域展开、回调异常记录、Caster 异常隔离和线程边界。
- SelectResult::Value<T>() 对不可复制 T 不再实例化 std::any_cast；新增 TakeValue<T>()，避免 move-only 类型的编译期断言。
- WaitMany 的补采样改为只完成 fd/方向队列头节点，并拒绝同一集合内重复 fd/方向；新增混合无效 fd 回归，保持外部 waiter FIFO。
- Debug 10/10、Werror 10/10、Release 9/9、ASan/UBSan 全组通过；Valgrind 完整套件 416B/4 blocks still reachable，definite/indirect/possible 均 0，ERROR SUMMARY 0。
- TSan 在 setarch 下四个过滤用例通过；完整套件在 WSL 120 秒 watchdog 超时，未宣称全量通过。
- 已知语义边界仍保留：panic 不自动展开 C++ 栈，recover 不绑定 G/Fiber owner，固定 Fiber 栈无 Go 式动态增长，WaitAny/WaitMany 仅 managed Fiber 可用。
## 2026-09-24 最终修复记录

- 修改内容：为 WaitMany 增加完整请求集合预校验，避免在部分 epoll 节点已发布后才发现无效 FD；修复 Memcheck 下混合无效 FD 的顺序竞态。
- 修改内容：SelectWaitState::Take 直接移动 typed holder；内建 Channel Select 对不可复制元素明确返回 kInvalid，普通 Send/Recv 与独立 SelectValue 保留 move-only 支持。
- 验证：Debug CTest 10/10、Werror CTest 10/10、Release CTest 9/9；ASan/UBSan 全量测试通过。
- 验证：Valgrind Memcheck 完整套件 ERROR SUMMARY 0，definite/indirect/possible lost 均为 0，416 bytes/4 blocks still reachable。
- 验证：TSan 在 setarch 下 control_flow、io、channel、scheduler_smoke 四个过滤用例通过；完整套件仍受 WSL watchdog 限制，未宣称全量通过。
- 后续状态：构建目录和测试日志已清理，本轮提交已完成。固定 Fiber 栈、显式 panic/recover 协议、managed Fiber 专用 WaitAny/WaitMany 和 Caster 并发责任仍属于兼容性边界。

- 示例补充：新增 example/io_wait_many_demo.cpp，展示两个 socket FD 的 WaitManyFor
  和 WaitAnyFor；Hook-on Debug/Werror 各 11/11，Hook-off Release 10/10，直接运行
  输出 wait-many=true。

## 2026-09-30：工程化日志与配置模块

- 新增 `include/go2cpp/log.hpp`、`src/log.cpp`：自有 C++17 日志实现，按
  `LogFilter -> LogItemWorker -> LogFormatter -> LogSink` 解耦；默认使用线程安全
  文件 Sink，发布等级为 warn，默认写入 `log/go2cpp.log`，stdout 必须配置开启。
- 提供 `PatternFormatter`、`CallbackSink` 和大小写兼容的 `GO2CPP::Logger`/宏接口，
  可在回调中桥接 spdlog/fmt，不把第三方 ABI 强行加入运行时。
- 新增 `include/go2cpp/config.hpp`、`src/config.cpp` 和根目录 `go2cpp.ini`：支持
  section、key=value、注释、日志和调度器参数统一校验；`max_workers` 上限 32、
  `local_queue_limit` 必须大于 0 等约束在启动前检查。
- CMake 增加 `go2cpp_log`、`go2cpp_config` 目标及安装导出；新增日志并发/过滤/配置
  边界测试与 `example/log_config_demo.cpp`。
- 验证：WSL GCC 13.3 + Boost.Context 1.83 的工程化构建成功；日志过滤并发测试通过，
  `go2cpp_log_config_demo` 通过。完整 CTest 中已有 scheduler-aware synchronization
  用例一次出现既有的时序失败，单独重跑日志测试和其余目标成功；该失败未被隐藏。

## 2026-09-30：工程化日志与配置增强复做

- 复核发现上一版日志只有单一文件/标准输出 Sink，配置只能一次性读取，未达到 Sylar
  级别。本轮在原模块上增量增强，没有替换现有 Logger API。
- 日志增加 Sylar 风格 `%d/%p/%N/%T/%f/%l/%m/%n` 格式解析、NOTICE/CRIT/ALERT
  等级、Logger 父级传播、多 Worker 默认/自定义分层、动态重建默认输出地、滚动文件
  Sink、stderr Sink、内存 Sink、Flush/Reopen 和第三方 Callback 边界。
- 配置增加强类型 `ConfigVar<T>`、`ValueCodec`（基础类型、字符串、vector）、变更
  监听器、事务式类型预校验、文件 mtime watcher、`BindLoggingConfig` 和
  `BindRuntimeConfig`。运行中修改 `go2cpp.ini` 会立即更新已存在 Logger；调度器结构
  参数仍明确要求重建 Scheduler，避免在线修改 P/M 队列拓扑。
- 修复 ASan 发现的监听器共享指针环：日志监听器改用 weak_ptr，完整日志测试不再泄漏。
- 验证：完整 `go2cpp_tests` 通过；Werror 构建通过；ASan/UBSan 全量 `go2cpp_tests`
  通过；Valgrind 日志测试通过，未报告 definite/indirect/possible leak；动态配置、
  Sylar 格式、层级传播和滚动文件均有回归测试。

## 2026-09-30：coost 对比性能复核与 IO/调度器优化

- `src/io.cpp` 采用持久 epoll ET 注册、读写方向 pending readiness 和 eventfd
  tickle 合并；同一 FD 的连续等待不再重复 ADD/DEL，WaitNode 唤醒走
  `Task::wake_io()` 的原子状态路径。Descriptor generation、关闭和超时仍保留
  原有线性化检查。
- `src/scheduler.cpp` 增加 worker 本地 yield 回队和 IO park/wake 快路径，维护
  扩容采样，保留 BlockingRegion/sysmon 的替代 M 语义；`src/fiber.cpp` 延迟首次
  栈分配、完成后立即回收并使用 M/TLS 栈缓存，避免短任务预先建立保护栈。
- Release 公平基准（50,000 G、每 G 4 次 yield，执行时间不含 shutdown）本轮
  Go2Cpp 42--71 ms，coost 18--30 ms；当前调度器仍约为 coost 的 1.4--2.0 倍，
  尚未达到用户要求的 1.25 倍。主要剩余成本是每个 Fiber 独立受保护栈和 shared_ptr/
  admission registry；直接替换为无保护栈会损失越界保护，未采用不安全伪优化。
- Release IO 基准（4096 socketpair、25 轮）Go2Cpp 695--731 ms，coost
  1356--1582 ms；本机该场景 Go2Cpp 已超过 coost。不同负载和机器仍需重新测量，
  不能把单一基准外推为所有 IO 工作负载。
- 验证：Debug/Release 全量 CTest 各 14/14 通过；scheduler、dynamic、io 过滤器
  通过；`git diff --check` 通过。构建时仅有 WSL 文件时钟偏移警告。未改变用户的
  `LOCAL_CODE_REVIEW_ORDER.md` 未跟踪文件。

## 2026-09-30：当前收尾复核与并发热路径修复

- 调度器将外部生产者改为 `incoming_queue` 与独立互斥量，并把任务注册表拆成
  16 个分片；`Task::prepare_enqueue` 在一次状态转换中完成 owner、取消门和入队
  标记，减少重复加锁和重复入队窗口。
- IO 的 `WaitMany` 增加注册后的零超时 readiness 补采样，修复“状态为 ready 但
  ready 列表为空”的竞态；领取等待节点时清理 pending 标记，并只在同一注册的
  读写队列都为空时失效 epoll interest，避免高负载下丢唤醒或重复注册。
- Mutex 保留有等待者时的公平保护，只对无等待者竞争执行有限次协作让出；没有把
  普通线程锁伪装成可跨线程迁移的 Fiber 锁。
- 配置 watcher 增加生命周期互斥量和条件变量快速停止；配置读取使用不可变原子
  快照；日志 sink、级别和 worker 列表使用快照并避免 Info 级别逐条 flush。新增
  的生命周期约束已写入 `docs/configuration.md`。
- 当前构建验证：WaitMany demo 连续 20 次通过；高负载样例通过；最终 Debug CTest
  13/13、Release CTest 14/14 均通过。性能复测写入
  `docs/performance.md`：公平调度中位数约为 Coost 4 倍，IO 场景约为 Coost 0.59 倍，
  混合 Mutex 仍明显落后，未宣称达到全局 1.25 倍目标。
- 进一步修复：Machine 的任务亲和字段改为原子快照，避免 worker 更新与
  `Scheduler::machines()` 读取之间的数据竞争；配置 watcher 捕获监听器异常，避免
  用户回调异常导致 watcher 线程 `std::terminate`。修复后两套 CTest 再次全通过。
- IO 生命周期复核增加 WaitNode 的完成操作屏障：`complete`、epoll 取 ready 节点和
  deadline drain 在 claim 前登记 active operation，节点回收会等待这些操作完成；
  Context 回调仍有独立进入/退出屏障。pending readiness 保留高性能 epoll 快路径，
  但在下次消费前通过零超时原生 poll 复核 FD，避免读空后的 stale ready。
- 最终验证：Release CTest 14/14、Debug CTest 13/13、Werror CTest 3/3、ASan
  CTest 3/3 通过；核心 Release `go2cpp_tests` 连续 4 次通过。TSan 当前构建在本机
  WSL 报 `ThreadSanitizer: unexpected memory mapping`，属于运行环境限制，未宣称通过。
  Valgrind 当前测试的 ERROR SUMMARY 为 0，definite/indirect/possible leak 均为 0，
  但 Memcheck 放大时序导致 IO watchdog 超时，416 bytes still reachable 为进程级缓存。
- Fiber Context 复核结论不变：自研自动扩栈上下文曾在嵌套恢复中破坏父链，已撤回；
  当前生产实现继续使用 Boost.Context 受保护固定栈，自动扩栈属于未完成边界。

## 2026-10-01：混合 Mutex 关键路径优化

- `src/sync.cpp` 将锁状态合并为单一原子字节，并把 `kWaiter` 的发布放在队列锁
  内，修复等待者发布与 Unlock 交错造成的丢唤醒。WaitNode 增加 generation 和
  `TryCancelAndWake` 门，修复 Context 旧回调污染缓存节点及取消与 `Arm()` 交错
  永久挂起的问题。没有改变普通线程的阻塞语义，Fiber 等待仍按 FIFO handoff
  进入调度器。
- `src/scheduler.cpp` 和 `scheduler.hpp` 增加 `park_wait()` 原子挂起路径；同步
  原语不再为每次等待获取 admission 锁。跨 M 唤醒按 G 最近运行的 P 投递，P 本地
  回队仅在无运行 M 或队列积压时通知 worker，降低 futex 抖动。Task 最近 P 字段
  改为原子快照，避免 worker 写与外部唤醒读之间的数据竞争。
- 当前本机 Release 高负载混合 Mutex 样本 15～35 ms，完整样本 26/50 ms；线程
  `std::mutex` 为 40～45 ms。计算、调度、Channel、IO 的完整样本仍分别约为
  1.5 s、121～124 ms、24～26 ms、662～748 ms；IO 继续优于线程 poll。性能数字
  仅代表本机重复运行，不能当作跨机器 SLA。
- 验证结果：`build-native-context` CTest 14/14、`build-engineering-werror` CTest
  3/3、ASan 单测全部通过；Valgrind ERROR SUMMARY 0，definite/indirect/possible
  leak 均为 0（288 bytes still reachable 为进程级缓存）。TSan 仍因 WSL
  `unexpected memory mapping` 无法运行，未宣称通过。构建期间仅出现 WSL 文件时钟
  偏差警告，不影响目标生成。

## 2026-09-30：Context 后端与跨 M 唤醒门

- Linux x86_64 新增 Go2Cpp 自有 Context ABI，布局借鉴 coost/TBOX，但保留
  `mmap + PROT_NONE` guard page、FiberStack TLS/全局缓存、ASan/TSan 切换、
  `m_resume_claim` 串行恢复和跨 M caller transfer；该平台不再链接 Boost.Context。
  非 x86_64 Linux/其他平台回退 Boost.Context。
- 明确 coost 的 Buffer 只是固定共享栈的挂起快照，不是自动动态栈；本轮没有复制
  `_exit` 结束路径，也没有声称实现 Go `morestack`/`copystack`。
- IO WaitNode、ParkingCondition 和 sync WaitNode 的 wake/disarm/arm 使用
  `HybridGate`：原子 flag 先做 64 次短自旋，竞争持续再进入 `std::mutex`；节点
  内的 Scheduler/Task 所有权和 callback/operation 生命周期仍由门保护。
- native 后端高负载复测：调度 113～133 ms、计算 1327～1526 ms、混合 Mutex
  6680～6895 ms、IO 616～665 ms；线程对照约 40～46 ms、1283～1439 ms、39～42 ms、
  1614～1704 ms。混合锁仍受 park/wake/FIFO handoff 限制，未达到线程或 Coost
  1.25 倍目标。
- native Release CTest 14/14、native ASan/UBSan CTest 14/14 通过；高负载连续
  3 次通过。详细约束与数据见 `docs/performance.md`、`docs/design.md`。

## 2026-09-30：调度器与混合 Mutex 专项优化收尾

- 调度器新增 16 条 incoming 条带、非空位图、已注册 G 的唤醒快路径和 worker
  本地 P 回队路径；worker 仅在仍有 runnable G 时通知，避免无效 futex 唤醒。
- 混合 Mutex 的 Unlock 统一在队列锁下完成 FIFO handoff，修复丢唤醒和误清锁
  竞态；已有 waiter 时禁止新的自旋者越过队头；无 Context 等待节点继续使用
  调用栈对象，带 Context 节点使用线程本地缓存。
- 当前公平调度基准（50,000 G、4 次 yield、8 个生产线程）总时延 84～103 ms，
  coost 为 18～36 ms，约 3.0 倍；混合 Mutex（2,000 Fiber、32 线程）为 8.5～8.8 s，
  coost 为 1.6～5.7 s，线程 `std::mutex` 为 39～40 ms。未达到 1.25 倍目标，
  主要剩余成本是 shared_ptr/注册表、Boost 受保护栈和 park/wake 跨 M 链路，不能
  通过无锁改动安全消除。
- 明确使用规约：混合锁只用于短临界区；不得持锁等待 IO、定时器或 park；高争用
  优先使用原子或按 P 分片，普通线程独占热点使用 `std::mutex`。
- Coost 的 Buffer 只是固定执行栈槽的挂起快照，不是自动动态栈；Boost
  `segmented_stack` 依赖受限工具链，故未替换当前受保护固定栈。
- 验证：Release CTest 14/14、Debug CTest 13/13、Werror CTest 3/3、ASan CTest
  3/3；高负载样例连续 3 次全部通过；TSan 仍受 WSL `unexpected memory mapping`
  环境限制，未宣称通过。详细数据和调度环节见 `docs/performance.md`。

## 2026-10-01：FiberBin 复用与 M 本地队列复核

- 调度器新增每个 M 的线程本地 FiberBin。只有已经完成或失败、上下文和栈都已
  释放、且父记录没有被嵌套 Fiber 持有的内部 Fiber 才会进入缓存；复用时重新
  创建 FiberRecord/ID，并清除父链、失败状态、调度标志及 sanitizer 状态。普通
  用户 Fiber、挂起 Fiber 和跨 M 的活动 Fiber 不经过此路径。
- FiberBin 默认容量为 32，配置上限为 4096，可通过 `scheduler.fiber_bin_capacity`
  在 INI 和动态配置中修改。容量仅影响对象分配复用，不改变调度语义；容量过大
  会增加每个 M 的常驻内存。
- 曾尝试把任务从 P 队列进一步分为 MLocalTaskQueue、P 队列和全局队列，但在
  50,000 G 的高负载 yield 压力下出现完成超时。该实现已撤回，没有把未经验证的
  runnable 计数、shutdown 排空、sysmon 解绑和跨 M 唤醒状态机带入主线。当前仍以
  P 本地队列、incoming 条带和全局窃取为生产实现；后续若重新实现 MLocal，必须先
  补齐可证明的所有权、溢出和关闭协议。
- 验证：Release CTest 14/14 通过，`go2cpp_tests` 连续运行通过；ASan 直接运行
  和单测过滤重跑可以通过，但在重复压力运行中仍会间歇性触发既有
  `test_readiness_timeout_race` 的 timeout/stack-use-after-scope（临时禁用 FiberBin
  后仍可复现，确认不是 FiberBin 引入）。因此不能把 ASan 宣称为稳定全通过；TSan
  仍受本机运行时映射限制。高负载混合 Mutex 仍明显慢于线程基准，FiberBin 只降低
  创建/销毁分配开销，不宣称已经消除调度和混合锁的主要成本。

## 2026-10-02：逻辑链路审计与 IO 代际竞态修复

- 对 Task 的状态转换、Runnable 计数、execution claim、P/M 绑定、sysmon、shutdown、
  嵌套 Fiber 父链、混合同步原语、Context 取消、Channel/Select 以及 IO 等待分别记录
  线性化点和不变量，详见 `docs/correctness_audit.md`。审计结论限定在文档列出的
  API 使用契约内，未把“测试通过”表述成数学意义上的零 Bug。
- `src/io.cpp` 在 descriptor token 首次检查后、取得 State 锁并发布 waiter 前增加第二次
  generation/FD 检查；关闭或 dup2 与注册交错时不会把失效 fd 重新放入无后续事件的队列。
  当 fd 已无 waiter 时撤销 epoll registration，避免旧 ET 注册造成忙循环和代际残留；仍有
  waiter 时保留已发布的 interest 位，以吸收内核中已经排队的迟到 ET 事件。
- `src/fiber.cpp` 的 native context 为 Memcheck 登记/注销自定义 Fiber 栈，修复换栈后
  Valgrind 无法识别栈边界导致的误报。`tests/test_io.cpp` 新增嵌套 Fiber IO 唤醒回归，
  并仅在 Valgrind 下放宽超时和轮数，保持同一状态机而避免检测器开销造成误报。
- 重新验证：`build-context-test` CTest 13/13、`build-engineering-werror` CTest 3/3、
  `build-native-context` CTest 14/14 均通过；高负载测试连续三轮通过；ASan/UBSan 全量
  单测通过；Valgrind `ERROR SUMMARY: 0`，definite/indirect/possible leak 均为 0，
  仅有 416 bytes/4 blocks 的进程级 still reachable 缓存。TSan 仍受 WSL
`unexpected memory mapping` 启动限制，未宣称 native Linux 竞态验证通过。

## 2026-10-02：性能复测发现并修复混合 Mutex UAF

- Release 高负载复测曾在混合 Mutex 阶段出现 SIGSEGV，进一步定位到
  `src/sync.cpp`：`Mutex::Impl::m_waiters` 保存裸 `WaitNode*`，
  `Unlock()` 释放队列锁后才调用 `Wake()`，等待方可能已经销毁栈节点或
  重置线程本地缓存节点，形成悬空访问。
- 修复后等待队列改为 `std::deque<std::shared_ptr<WaitNode>>`，所有争用等待者
  通过 `AcquireWaitNode()` 获取共享节点；`Unlock()` 将 shared ownership
  保持到锁外 `Wake()` 完成，等待方按节点地址移除队列元素。
- 修复验证：Release CTest 14/14、Werror CTest 3/3 通过；Release 高负载连续
  5 轮无崩溃；ASan/UBSan mutex-only 连续 5 轮无报告；Valgrind 全量单测
  `ERROR SUMMARY: 0`，definite/indirect/possible leak 均为 0，仅 416 bytes
  still reachable。
- 同一组 CPU 上的三轮性能样本：计算 Fiber/线程为
  1595/1616、1614/1632、1560/1555 ms；调度为 93/55、150/55、107/50 ms；
  Channel 为 17/53、18/69、20/56 ms；IO 为 2896/2034、2614/2016、
  2725/2091 ms。混合 Mutex 为 18、1784、1562 ms，线程 `std::mutex`
  为 39、38、37 ms，仍有明显长尾，不能宣称性能稳定。
- Coost 本机对照（同 CPU 集合、历史 `-O2` 构建）调度中位数约 8 ms、
  IO 约 1686 ms、混合 Mutex 约 1721 ms。计时边界和编译参数不完全相同，
  这些数据只用于工程定位，不是严格同配置基准。

## 2026-10-02：收尾文档、中文注释与配置统一

- 新增 `docs/getting_started.md`，从构建、最短 `go()`、Scheduler/Fiber 生命周期，
  讲到 Fiber/线程混合同步、Channel/Select、Context、Linux IO/Hook、日志、配置
  热更新和故障排查；README 与 example 入口均已链接该指南。
- 为 Scheduler/Task、Fiber、IOManager、Context、Channel、Mutex、ConditionVariable、
  WaitGroup、Future、Error、FiberLocal、Hook、线程策略、定时器、配置中心等公共类
  补充中文的依赖、职责、生命周期、参数和返回值说明，并把公共头中的整段英文注释
  翻译为中文；示例注释也已统一为中文。
- `go2cpp.ini` 现在覆盖 SchedulerConfig 全部重要字段和日志字段；统一校验 P/M 数量、
  本地队列、Fiber 栈、FiberBin、亲和预算及毫秒参数范围。ConfigVar 支持强类型校验器，
  非法热更新会拒绝并保留旧值，min/max_workers 的单步修改也不会留下不可启动状态。
- 最终验证：Release 独立构建 `build-final` CTest 14/14；`build-engineering-werror`
  CTest 3/3；ASan+UBSan 构建 CTest 14/14；Valgrind 全量单测通过，`ERROR SUMMARY: 0`，
  definite/indirect/possible leak 均为 0，仅 416 bytes/4 blocks 进程级 still reachable。
  TSan 仍受 WSL 的 `unexpected memory mapping` 启动限制，未宣称通过。

## 2026-10-02：配置热加载编辑锁与半文件保护

- 修复配置监听可能读到编辑器截断文件、并在失败后错误确认文件版本的问题。
  `IniFile::Load` 现在连续读取文件元数据和内容，只有快照稳定且 INI 解析成功才
  进入配置中心；失败不会更新监听器基准，后续轮询会继续重试。
- 配置监听增加固定的 `path + ".lock"` 编辑锁协议。锁文件非空时，启动监听会拒绝
  读取，运行中的监听会跳过刷新；锁文件为空或不存在才允许加载。生产写入流程应为：
  写入非空锁、写同目录临时文件、关闭并校验、`rename` 原子替换目标、最后清空锁文件。
  文档和 `go2cpp.ini` 模板已写明该约束。稳定快照是兜底保护，不能判断语法上完整但仍
  在继续写入的部分前缀，因此不支持绕过编辑锁直接长时间覆盖正式文件。
- 测试新增启动时锁拒绝、锁定期间保持旧值、清空锁后更新，以及截断半文件恢复；覆盖
  Release、`-Wall -Wextra -Wpedantic -Werror`、ASan/UBSan 和 Valgrind。
- 验证结果：`build-final` CTest 14/14；`build-engineering-werror` CTest 3/3；
  `build-native-asan` CTest 14/14；Valgrind `ERROR SUMMARY: 0`，definite/indirect/
  possible leak 均为 0，仅 416 bytes/4 blocks still reachable。构建时 WSL 与 Windows
  文件时间有轻微偏差，出现 gmake clock skew 警告，不影响测试结果。

## 2026-10-02：配置独立性规范

- 在 `docs/configuration.md`、`docs/getting_started.md` 和 `go2cpp.ini` 中明确规范：
  允许热更新的配置键必须独立解析、独立校验、独立生效，不能依赖同一文件中其他键
  的非原子中间值。`min_workers`/`max_workers` 等相互约束的结构参数被限定为启动期
  成组配置，必须通过完整快照一次替换并重新创建 Scheduler，禁止分别热更新。

## 2026-10-02：统一中文 Doxygen 注释规范

- 按 Sylar 风格补充 public header：类/结构体职责、依赖、线程安全和生命周期，
  公开函数的参数、返回值和失败边界，以及枚举和配置字段的含义；涉及文件包括
  Context、Channel、Fiber、Future、IO、Scheduler、同步原语、日志、Hook、配置和
  运行时入口。
- 为 `src/*.cpp` 的实现函数补充中文 `///` 执行流程注释，覆盖参数校验、锁/状态机、
  唤醒和资源释放等关键步骤；example 与 tests 的顶层入口也补充用途、流程和返回值。
  注释修改不改变逻辑，已清理误插入函数体语句前的模板注释。
- 验证：`git diff --check` 通过；Release CTest 14/14、Werror CTest 3/3 全部通过，
  全部目标成功编译。

## 2026-10-03：模块边界复核与可验证缺陷修复

- 对 GMP、Fiber/Context、Timer、混合同步、Future、Channel/Select、IOBackend/Hook、
  Error、Config 和 Logger 逐模块复核状态机、生命周期、锁顺序、扩展点异常和等待
  超时边界，结果记录在 `docs/boundary_audit.md`。文档把已修复问题、必须遵守的使用
  契约和仍无法安全等价实现的 C++/Go 语义差异分开，避免把测试通过误写成完全无条件
  正确。
- 修复 Error 自定义 `Unwrap/Is/As` 抛异常导致终止的问题；默认 IOBackend 禁止注销；
  IOBackend 实例按 IOManager 单 owner 检查；epoll 改为 level-triggered，避免同一 fd
  多等待者部分消费后丢唤醒；`wait_for(0)`、`WaitAnyFor(0)`、`WaitManyFor(0)` 增加
  原生非阻塞 readiness 探测。
- Fiber native 栈检查 sysconf、页对齐、guard page 映射尺寸溢出；Scheduler 观察事件
  增加 cancelled；Future/Sync 后端注册表改用锁外不可变快照，Future Context 回调
  增加异常展开清理守卫；IniFile 解析失败保持旧快照；Logger parent 拒绝自环和间接环。
- 新增同 fd 多等待者、零超时 readiness、共享 backend、极端 Fiber 栈尺寸、Future
  后端异常清理、Logger 环检测和 IniFile 失败快照测试。
- 验证：`cmake --build build-final --parallel 1` 成功；`ctest --test-dir build-final
  --output-on-failure` 串行 14/14 通过（13.36 秒）；IO、Error 过滤测试通过。
  NTFS/WSL 构建目录禁止在多个构建或 CTest 进程并发读写，避免共享库文件被并发链接
  时出现短文件；本轮未把并行加载错误误判为代码测试失败。
- 未宣称完全无条件正确：固定 Fiber 栈、显式 panic/recover、未 Hook 阻塞调用、
  单线程 TimerService、配置跨变量无锁快照和 watcher/插件生命周期约束仍按文档限制。
- 追加修复 Channel Select 的注册失败边界：内建 case 使用带结果的
  `arm_checked`，复制/注册失败会取消已注册节点并返回 `kInvalid`，不会在零节点
  上永久 park；新增回归测试并通过 `build-final` 与 `build-engineering-werror`。
- 当前 Memcheck：`build-final/valgrind-boundary-audit.log`，1,322,815 次分配、
  1,322,811 次释放；definite/indirect/possible 均为 0，416 bytes/4 blocks
  为进程级 still reachable，`ERROR SUMMARY: 0`。ASan `build-native-asan`
  的 `go2cpp_tests` 也通过；TSan 仍按既有记录受 WSL 映射限制，未伪造通过结论。

## 2026-10-06：定时任务模块文档补充

- 补充 `docs/timer_usage.md`，说明 TimerService 的一次性、周期、动态决策、弱引用、忙等待和 Scheduler/Fiber 投递边界。
- 明确取消与 Running 竞态、readiness 与 timeout 单次 claim、回调锁约束及 TimerManager 与 Scheduler 的解耦。

## 2026-10-06：定时任务模块实现与验证

- 重构 include/go2cpp/core/timer.hpp、src/timer.cpp：增加 TimerState、固定延迟/固定速率、
  TimerDecision、最大次数、忙等待窗口、Reset/Refresh、弱引用任务、异常观察器、执行器
  插件和 PendingCount/NextDeadline；队列使用 multimap 加状态索引，取消和重排为对数复杂度。
- Scheduler 增加 schedule_at/schedule_after/schedule_every 及大写别名，通过
  TimerExecutorAdapter 只把到期回调提交为 Fiber；TimerService 不继承 Scheduler，避免跨 M
  恢复 Fiber 栈。go2cpp::after/every 提供默认调度器的傻瓜式入口。
- 修复关闭竞态：TimerService 停止时正在返回的周期回调不会重新插入已关闭队列；异常回调
  在锁外隔离并将任务标记为 kFailed。明确记录慢回调会阻塞同一服务、服务析构等待回调、
  不能从自身计时回调销毁服务的边界。
- 新增 tests/test_timer.cpp 的 FIFO、非法周期、Reset、固定/动态周期、BusyWait、弱引用、
  异常、并发注册、Scheduler/Fiber 投递和 shutdown 竞态测试；新增
  example/timer_task_demo.cpp 六个线程/Fiber 场景。
- 新增 benchmarks/timer_task_benchmark.cpp、GO2CPP_BUILD_BENCHMARKS 和
  docs/timer_benchmark.md。200 任务实测：普通线程 27.91 ms、TimerService 5.50 ms、
  Scheduler+Fiber 6.41 ms；同机 coost 独立对照 7.11 ms（接口语义不同，仅作量级参考）。
- 验证命令：build-timer-final RelWithDebInfo 构建；CTest 14/14 通过（11.47 s）；
  timer 过滤测试连续 3 次通过；ASan+UBSan timer 过滤通过；Valgrind timer 过滤通过
  且返回码为 0，但当前 Fiber/上下文后端没有产生 Memcheck 退出摘要，未将其误报为完整
  leak 统计。git diff --check 通过。

- 收尾复核：runtime.hpp 显式聚合 core/timer.hpp；示例新增“业务事件提前完成取消超时”场景；
  测试新增可插拔执行器拒绝、同截止时间先进先出和正在执行周期任务的关闭竞态。

## 2026-10-07：网络基础设施与模块目录整理

- 目录按 logging、config、concurrency、tools、network 拆分；旧 include/go2cpp 根路径保留中文兼容转发头，命名空间和原有 target 名不变。测试与示例按模块目录维护，CMake 使用显式源文件列表。
- 新增 network 模块：Address/IPv4Address/IPv6Address/UnixAddress、endian、ByteArray、FdManager、Socket/SocketStream、Framer 和 HTTP/1.x message/parser/client/server。Socket 统一 RAII fd 生命周期，Fiber 中优先 IOManager 等待，普通线程回退 poll；fd 元数据与 close 通知保持代际安全。
- HTTP parser 支持半包、粘包、Content-Length、chunked、重复头、多值 Cookie/Set-Cookie 基础能力和流水线剩余字节，默认头体上限；冲突的 CL/TE、非法头和超限返回错误。HTTP/2/3、WebSocket upgrade、无限流式 Body 暂不在核心实现。
- 验证：`build-network` RelWithDebInfo、静态库、`-Wall -Wextra -Wpedantic` 构建通过；网络过滤测试、network demo 和全量 CTest 15/15 通过（11.05 秒）；`git diff --check` 需在最终提交前再次执行。
- 后续：补充真实 loopback TCP/UDP、Fiber IOManager、多请求 keep-alive、chunked trailer、fd close/reuse、ASan/UBSan/Valgrind 网络专项；HTTP 大文件/SSE 应使用流式 ResponseWriter 扩展。

## 2026-10-07：网络模块第二轮边界验证

- 增加 Framer 分隔符/固定长度/大端长度前缀，补充 Go 风格 ResolveTCPAddr、ResolveUDPAddr、DialTCP、DialUDP、ListenTCP、ListenUDP，以及 Socket 半关闭接口；根路径和 Sylar 风格 `.h` 入口均保留。
- HTTP 增加 CanonicalHeaderKey、ParseHTTPVersion、ParseTime、Get/Head/Post/NewRequest 便捷入口；客户端使用 write_all 处理短写，服务端循环读取分片请求并保留流水线剩余，URL 端口解析失败返回 InvalidUrl。
- 第二轮测试增加 chunked、重复 Content-Length、查询参数重复键、TCP 回环 accept、UDP 无连接 send_to/recv_from、Framer 长度帧和非法 URL。
- 实际验证：RelWithDebInfo 静态构建 `build-network`、Debug 共享+Hook 构建 `build-network-hook`、ASan+UBSan 构建 `build-network-asan` 均成功；网络过滤测试和 Hook 网络测试通过；全量 CTest `build-network` 15/15 通过（约 11 秒）；`-Wall -Wextra -Wpedantic` 无警告；Valgrind 网络过滤返回 0 且无错误退出，但 Fiber 上下文路径未产生 Memcheck 汇总，因此没有宣称完整泄漏统计。
- 保留限制：核心 HTTP 只实现受限 HTTP/1.x 消息缓冲，不包含 HTTP/2/3、WebSocket upgrade、Expect:100、无限 SSE/大文件 ResponseWriter；DNS 为同步 getaddrinfo；ByteArray 单所有者，Socket 同一连接的协议读写需由调用方串行化。

- 网络第二轮还增加了受管 Fiber 读写验证：Fiber 中 Socket 遇 EAGAIN 通过当前 IOManager park，普通发送线程写入后恢复，测试有 2 秒有限 watchdog；Hook、ASan/UBSan 和全量测试均再次通过。

## 2026-10-07：P0 Socket 生命周期与 HTTP 流式基础加固

- `include/go2cpp/network/socket.hpp`、`src/network/socket.cpp` 为读写、accept、connect、
  地址查询、选项查询和半关闭统一加入 in-flight 生命周期。每次操作固定 fd 快照；
  `close/release` 先设置 closing、递增代际、通知 IOManager，等待旧操作退出后再关闭或转移
  fd，并处理重复 close/release。受管 Fiber 和普通线程都覆盖关闭期间的取消路径。
- 修复普通线程无限期 poll 无法被 close 唤醒、Fiber readiness 注册与 close 竞态、关闭后
  旧操作继续使用 fd 等边界；关闭中的新操作返回 `ECANCELED/EBADF`。普通线程和 IOManager
  等待以非忙等待的小片检查关闭状态，保留 deadline 语义。
- `include/go2cpp/network/http.hpp`、`src/network/http.cpp` 增加严格 Transfer-Encoding
  校验、十六进制 chunk 溢出检查、chunk 扩展和 trailer 保存、body 长度溢出保护，以及
  1xx/204/304/HEAD 无 body 规则；Parser append 分配失败转为 ParseState::Error。
- 增加 `http::ResponseWriter`：以 `Idle -> HeadersCommitted -> Closed` 状态输出 chunked
  流，适用于 SSE/大响应的后续封装；每次 write 使用 SocketStream::write_all 处理短写，
  不允许未 start 或 close 后写入。Server 停止标志原子化，handler 异常隔离为 500 响应。
- 新增普通线程和 Fiber 并发 close、HTTP 严格 chunk/trailer、无体响应、HEAD、流式 writer
  状态机回归测试。验证：`build-network` 全量 CTest 15/15；`build-network-hook` 网络
  测试 2/2；`build-network-asan` 网络总测试通过；TSan 构建成功但网络过滤启动时受当前
  WSL 运行时限制报 `unexpected memory mapping`，因此未将 TSan 计为通过；Valgrind 网络
  过滤返回 0，definite/indirect/possible leak 均为 0，still reachable 96 bytes/1 block，
  错误汇总为 0。
- P0 仍不宣称完整 WebSocket/SSE/NetService：ResponseWriter 只是 HTTP/1.1 chunked 基础，
  WebSocket 握手/帧状态机、SSE 心跳/重连、路由治理和分布式组件留在后续阶段；DNS 仍为同步
  getaddrinfo，Fiber 栈和 C++ 异常边界保持现有文档限制。

## 2026-10-07：P1 WebSocket 协议层

- 新增 `include/go2cpp/network/websocket.hpp/.h`、根路径兼容头和
  `src/network/websocket.cpp`。协议对象独立于 Scheduler/IOManager，复用 Socket 的 Fiber/线程
  等待；读方向串行化，写方向由内部锁串行化，避免并发调用交错帧。
- 实现 RFC 6455 HTTP/1.1 Upgrade 校验：GET、HTTP/1.1、Upgrade/Connection token、版本 13、
  16 字节 Base64 key、SHA-1 Accept；提供 `Accept`、`AcceptWithPending` 和 `Connect`。后者不预读
  WebSocket 帧，服务端可用 `AcceptWithPending` 接管 HTTP Parser 的剩余字节，避免升级边界丢数据。
- 实现掩码方向、7/16/64 位长度、高位溢出、RSV/opcode、控制帧、分片重组、最大帧/消息大小、
  Ping 自动 Pong、Pong 忽略、Close 双向确认、Close code/reason 和 UTF-8 校验。
- 新增 `example/network/websocket_demo.cpp`、`docs/websocket.md`；测试覆盖 RFC 握手向量、
  socketpair 客户端/服务端、126 字节扩展长度、文本/二进制、分片、HTTP+首帧同批到达和关闭。
- 验证：WebSocket/网络过滤测试、Hook 构建 `go2cpp_tests` 与 WebSocket 示例 2/2、ASan/UBSan
  网络测试均通过；Valgrind 网络过滤返回 0，definite/indirect/possible leak 均为 0，still
  reachable 96 bytes/1 block，错误汇总为 0。高负载测试单独运行通过。一次整套运行中已有
  `test_io.cpp:439/449` readiness 压力断言出现时间敏感失败，随后等待系统负载稳定后重跑，
  `build-network` 全量 CTest 16/16（11.26 秒）通过；`GO2CPP_TEST_FILTER=io` 也连续 3 次
  通过。TSan 仍受当前 WSL `unexpected memory mapping` 启动限制，未宣称通过。
- P1 明确限制：暂不支持 TLS、HTTP/2 CONNECT、permessage-deflate、扩展/子协议协商和发送端
  主动分片；SSE、鉴权、限流、审计和 NetService 治理继续分阶段实现。

## 2026-10-07：P2 SSE 流式事件

- 新增 `include/go2cpp/network/sse.hpp/.h`、根路径兼容头和 `src/network/sse.cpp`，并加入
  `go2cpp_network`。`sse::Event` 负责字段校验和多行 data 编码；`sse::Parser` 支持任意
  分片、LF/CRLF/CR、UTF-8 BOM、注释、retry、Last-Event-ID 以及行/事件/缓冲区上限；EOF
  不会把未以空行结束的半事件误报为完整消息。
- 新增 `sse::Stream`：复用 `http::ResponseWriter` 输出 200、`text/event-stream`、缓存控制、
  keep-alive 和 chunked 响应；事件/注释写入串行化，心跳通过 `tick()` 显式触发，失败后锁定
  为不可复用状态。Context 取消回调使用 weak 状态，不捕获裸 this，并调用 Socket::cancel_write
  唤醒受阻 Fiber/线程；Socket 的读写取消代际不依赖取消线程当前的 IOManager。不建立无界
  事件队列，慢消费者由写超时或 IO 错误反馈。
- 新增 `tests/network/test_sse.cpp`、`example/network/sse_demo.cpp`、`docs/sse.md`，覆盖
  编码注入、空事件、多行 data、半包解析、空 id 清除 Last-Event-ID、超长限制、chunked
  生命周期、心跳、重复 close、Context 取消、并发生产者以及普通线程跨线程取消 Socket
  等待。
- 验证：`cmake --build build-network -j2`、Hook 和 ASan/UBSan 的 `GO2CPP_TEST_FILTER=sse`
  均通过；普通线程、并发生产者和 Context 取消路径都已实际运行。最终 `ctest --test-dir
  build-network --output-on-failure` 为 17/17 通过（11.80 秒）；Valgrind 全量测试返回 0，
  definite/indirect/possible leak 均为 0，still reachable 416 bytes/4 blocks，错误汇总为 0。
  TSan 仍受当前 WSL `unexpected memory mapping` 启动限制，不能宣称通过。
- 已知边界：SSE 不包含 TLS、HTTP/2/3、自动重连、跨连接事件存储或有界异步队列；这些能力
  由上层 NetService/事件日志注入。`ResponseWriter::flush()` 在 TCP 上只是状态检查，每次
  `send/comment` 已经立即写入 chunk；调用方不得用零长度 chunk 冒充 flush。

## 2026-10-07：P0/P1/P2 第二轮审查与增强

- 根据 Socket/HTTP、WebSocket、SSE 三个只读审查结果修正了实际缺陷：`ResponseWriter` 的
  start/write/close 任一部分写失败后进入 sticky `Failed` 状态，禁止继续污染半个 header 或
  chunk；204/1xx/304 不再生成实体 Content-Length；出站 HTTP 头执行 token、控制字符和 CRLF
  校验；Parser 严格校验请求起始行、三位状态码、响应无体 framing，并拒绝端口溢出。
- `io.cpp` 的 `update_interest_locked` 改为按当前读写等待队列精确更新 epoll interest，修复
  READ|WRITE 收缩时遗留 READ 注册造成的水平触发忙循环；`descriptor_is_open` 只接受 F_GETFD
  成功，并对 EINTR 重试。
- WebSocket 写锁和 SSE 流锁统一改为 `go2cpp::sync::Mutex`，使阻塞写在受管 Fiber 中 park
  而不会卡住同一 M 的其他 Fiber。WebSocket 关闭、析构和写入失败会发布 Socket 读写取消代际；
  握手写入继承 `io_timeout`；要求 Host、拒绝 body/Transfer-Encoding/控制字符，拒绝非最小长度
  编码，并对协议错误、非法 UTF-8、超限消息发送 1002/1007/1009 Close。
- SSE 统一 `max_event_bytes=0` 为不限长，Parser 在解析 `id:` 行时立即发布 Last-Event-ID，
  并明确 Parser 为单消费者状态机；示例新增元数据游标、注释心跳、Context 取消和 WebSocket
  Ping/Pong 场景，网络示例增加 204 无实体演示。测试补充出站头注入、204 body、端口边界和
  WebSocket Host 检查。
- 最终验证：`cmake --build build-network -j2` 通过；`GO2CPP_TEST_FILTER=network`、`sse` 均通过；
  全量 CTest 17/17 通过（11.47 秒）；network/websocket/sse 三个示例均返回 0；Hook 构建的
  network+sse 过滤测试通过；ASan+UBSan 构建的 network+sse 过滤测试通过；Valgrind network
  和 sse 专项返回 0，definite/indirect/possible 均为 0，错误汇总为 0。TSan 仍受当前 WSL 的
  `unexpected memory mapping` 启动限制，未宣称通过。
- 本轮仍保留的边界：DNS 解析同步、Server 路由只允许初始化阶段修改、Socket 自定义 waiter
  没有强制取消协议、Fiber 中主动 close 与 Scheduler shutdown 的极端生命周期需要调用方遵循
  文档约束；这些不应被误报为已完全等价 Go runtime。
- 追加修正 HTTP Client：发送方向继承 Client timeout，ETIMEDOUT/ECANCELED 映射为公开的
  Timeout/Cancelled，并在同一批字节中正确跳过 100 Continue 等 informational 响应；全量网络
  回归再次通过。
- IO 专项新增单 P/单 worker 的读写双 interest 收缩回归：先同时等待同一 fd 的 READ/WRITE，
  消费立即就绪的 WRITE 后再验证 READ，`GO2CPP_TEST_FILTER=io` 通过。
- SSE 查询状态接口增加异常隔离：混合锁在 Scheduler 停止阶段无法 park 时，`noexcept` 查询
  返回 Failed/false/max，不会让异常穿过 `noexcept` 触发进程 terminate。
# 2026-10-08 混合 Mutex 专项优化

- `src/concurrency/sync.cpp` 保留无竞争原子快路径、FIFO 等待队列、`shared_ptr<WaitNode>` 生命周期锚点和跨 M `park_wait/wake_registered` 交接；没有采用会跳过 Scheduler 状态机的裸指针或未经验证的独立 Fiber 队列。
- 默认无状态 `SchedulerWaitBackend` 改为进程内共享实例，减少每次等待的控制块分配；受管 Fiber 的协作重试限定为有界 256 次，随后一次性进入等待路径，避免旧版反复调度造成的长尾。
- 失败的实验方案已撤回：提前把普通线程竞争计入等待者、Fiber/native 双队列优先级交接、专用轻量 waiter 都在压力或竞态测试中出现长尾或唤醒风险，没有保留在最终实现中。
- Release 专项 `go2cpp_high_load_stress --mutex-only` 30 轮：耗时最小/中位数/P95/最大值为 `9/19/31/51 ms`，无超时；同一完整高负载线程池 `std::mutex` 约 `36～40 ms`。按吞吐换算满足线程基线 75% 门槛。
- 验证：`build-network` 全量 CTest `17/17`；同步过滤测试连续 5 次通过；ASan/UBSan 同步过滤通过；Valgrind 混合锁专项 `ERROR SUMMARY: 0`，definite/indirect/possible leak 均为 0，96 bytes still reachable 为进程级缓存。
- 已知边界：带 Context、长临界区、ConditionVariable/WaitGroup 和手动 Fiber 仍走完整等待节点路径；锁内禁止阻塞 IO、日志回调和插件回调。TSan 受当前 WSL `unexpected memory mapping` 环境限制，未宣称通过。


## IO 性能优化记录（2026-10-08 12:16:07 UTC）

- 变更：Epoll 后端的 `EPOLLONESHOT` 能力声明与事件位保持一致；默认 Linux 后端启用 ONESHOT，由 `IOManager` 显式重新布防；事件批处理保持 256；注册表记录 `FdSlot*`，仅在 `m_mutex` 下解引用并在删除注册项前失效。
- 文件：`src/network/io.cpp`、`src/network/io_backend.cpp`、`include/go2cpp/network/io_backend.hpp`、`benchmarks/io_epoll_fair_benchmark.cpp`、`cmake/Go2CppTests.cmake`。
- 正确性：`ctest --test-dir build-network --output-on-failure` 最新一次曾在实验性调度快路径下暴露 high-load channel 失败，随后已撤销该快路径；撤销后 `go2cpp_high_load_stress` 通过，IO 专项通过；ASan/UBSan IO 专项通过。
- 公平基准：4096 fd、25 waves、8 workers、1 poller，线程 epoll 稳态中位数约 2172 ms，IOManager+Fiber 约 3626 ms，Fiber/线程约 1.67；事件完整 102400/102400，无超时。目标 1.25 倍尚未达到，不能宣称完成。
- 后续：需要针对 Fiber 调度/任务状态转换和跨 M 入队继续做受控 profiling；当前已知低风险优化未能消除调度开销。

## IO 性能优化记录（2026-10-10）

- 正确性修复：`src/network/io.cpp` 的 `drain_queue_locked`、`take_ready_locked` 和
  `complete_if_head` 统一采用“先取得队头、登记 operation、完成 claim、再移出队列”的
  线性化顺序，避免取消/超时与 epoll 就绪并发时把未完成节点弹出后永久丢失。新增
  `test_wait_many_immediate_ready_race`，双 fd 已就绪场景循环 1000 次，验证 `WaitMany`
  不少返回 ready 项。
- 热路径优化：WaitNode 的 operation 生命周期改为 0/1/2/3 原子状态机；无 Context 等待
  跳过回调互斥量清理；FdSlot 复用 DescriptorToken，去掉重复 F_GETFD/descriptor registry
  校验；IO poller 一次批量唤醒按最近 P 分桶，减少 P 队列锁次数；已 claim 的 WaitNode
  采用无锁任务快照；批量入队复用原子 queued 门，跳过每事件的 transition_mutex；epoll
  注册 token 改为 `fd + 32 位序号`编码，移除每次 ONESHOT rearm 的 `m_registrations`
  哈希表节点分配。
- 构建与回归：`build-network` 全量 CTest 17/17 通过；IO 过滤测试通过；ASan/UBSan IO
  过滤测试通过；`go2cpp_high_load_stress --io-only` 通过，Fiber 1239 ms、thread-poll
  1992 ms；没有发现错误、超时或泄漏报告。
- 公平基准（Release，4096 fd、25 waves、8 workers、1 poller、102400 事件）：Go2Cpp
  thread-pool+epoll 中位数约 2013 ms，IOManager+Fiber 约 703 ms，Fiber/thread 约 0.349，
  所有事件完整且无超时。独立 coost 适配器同口径实测 thread-epoll 约 143 ms、coost 约
  1155 ms；因此本轮数据在该 WSL 运行环境下 Fiber 小于独立 coost 适配器，但仍不能把
  Go2Cpp 结论外推到所有线程池实现。单 worker/低并发时 Fiber 仍明显慢于线程基线，说明
  上下文切换和调度固定成本尚未消除。
- 追加压力口径（4096 fd、10 waves、16 workers、40960 事件）：thread-pool+epoll 中位数
  约 723 ms，IOManager+Fiber 中位数约 296 ms，Fiber/thread 约 0.410；对应 coost 适配器
  thread-epoll 约 66 ms、coost 约 525 ms。高并发 Fiber 仍明显优于本项目线程基线和该
  适配器的 coost 协程，但相对最快三方线程 epoll 仍有固定调度成本。
- 约束与后续：WSL CPU/调度噪声会使同一基准出现较大波动；所有模型均串行运行并记录完整
  事件数后才纳入比较。用户要求的“对任意线程 epoll 与 coost 均不超过 1.25 倍”尚未被
  全场景证明，不能宣称达标；下一步应继续做上下文切换、P 队列入队和 ONESHOT rearm 的
  受控 profiling，而不能通过削弱正确性校验来降低数字。

## 2026-10-10：全模块边界回归与生命周期修复

- 修复 `src/network/io.cpp` 的 `WaitMany` 零超时路径：所有未完成节点都执行零超时
  `poll` 探测，不再因已有终态节点而跳过其他 fd，避免多 fd 同时就绪时丢失结果。
- 修复 `src/concurrency/scheduler.cpp` 的等待交接：`Task::wake_for_wait()` 与 park
  使用同一 `m_transition_mutex`，防止唤醒先到时被后续 park 覆盖；同步测试扩大为固定种子
  256 轮并加入 yield 抖动。
- 修复 Socket 延迟 close/release 的生命周期：进行中的操作持有 `shared_ptr` 自保持，关闭
  请求只发布取消代际，最后一个操作退出后再由唯一 owner 关闭 fd、移除 FdManager 并通知
  等待者；新增阻塞等待下 close/release 回归。
- 修复动态定时器配置的引用环：`ConfigVar` 监听器改为弱引用自身变量，避免
  `ConfigVar -> Listener -> ConfigVar` 泄漏；测试绑定全局 `Config::Instance()` 时使用进程
  生命周期的 `static RuntimeConfig`，遵守 `BindRuntimeConfig` 的目标生命周期契约。
- 变更文件：`src/network/io.cpp`、`src/network/socket.cpp`、`include/go2cpp/network/socket.hpp`、
  `src/concurrency/scheduler.cpp`、`tests/concurrency/test_sync.cpp`、`tests/network/test_network.cpp`、
  `src/config/config.cpp`、`tests/logging/test_log_config.cpp`。
- 验证结果：`cmake --build build-review -j2` 成功；Release 全量 CTest 随机顺序重复 2 轮，
  18/18、共 36 次通过；`go2cpp_tests` 全量通过；高负载场景 50000 yielding tasks、
  8000×100000 计算、2000 Fiber+32 线程混合锁、32000 channel 消息、4096×25 IO 均通过；
  新 ASan/UBSan 全量运行通过；新二进制 Valgrind Memcheck 返回 0，definite/indirect/possible
  leak 均为 0，still reachable 416 bytes/4 blocks，错误汇总为 0。
- 未通过/未覆盖：TSan 构建成功但运行被当前 WSL 环境的
  `ThreadSanitizer: unexpected memory mapping` 启动限制拒绝，不能据此宣称 TSan 通过；
  `BindRuntimeConfig` 仍要求调用方保证裸指针目标覆盖整个监听生命周期，接口文档已明确，
  后续可增加 RAII 绑定句柄以降低误用风险。