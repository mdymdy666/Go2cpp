# TODO（2026-11-08 更新）

> 按「谁 / 什么 / 为什么」记录，做完打勾。
> 优先级：P0 = 阻塞/正确性，P1 = 性能/体验，P2 = 完善。

---

## 一、JSON 库（✅ 已并入 Go2Cpp）

| # | 事项 | 状态 | 结果 |
|---|---|---|---|
| J1 | 深嵌套优化 | ✅ | **−25.8%**（1885→1400 ns），绝对值达标 |
| J2 | 数字密集优化 | ⚠️ | **−5.3%**（目标 −20%，未达标）——`from_chars` 慢 87%、`divsd→乘法` 破坏 round-trip，**需 AVX2** |
| J3 | Reflect 缓存 | ✅ | **−83.7%**（217→35.4 ms，6.1×） |
| J4 | Reflect 3 个缺失符号 | ✅ | `kDecimalType = 9` 已在 `json/core/conversion.hpp` 定义 |
| J5 | `WalkErrors` +47% | ⏳ | 不在热路径（可接受） |
| **J6** | **合并进 Go2Cpp** | ✅ | `include/go2cpp/json/` + `src/json/` + `tests/json/`（18 测试 / 115 万 checks） |
| J7 | `ReadMe.md` 编译命令 | ✅ | 已补 `src/go2cpp` 部分 |
| **J8** | **删 nlohmann** | ✅ | **24,765 行删除**，全项目零引用（`cee32c2`） |
| **J9** | **数值契约** | ✅ | 9 条契约 + 修 4 bug + **100 万位级往返**（`d14a9ce`） |
| **J10** | **J2 的 AVX2 版** | ⏳ P1 | `_mm256_maddubs_epi16` + 运行时检测（**需两套代码**） |

---

## 二、douyin（`app/douyin/`）

| # | 事项 | 优先级 | 状态 |
|---|---|---|---|
| D1 | **举报 `\uXXXX` 转义** | P0 | 🏃 **agent `99372d54` 在做** |
| D2 | 封面纯黑重抽 | P2 | ⏳ |
| D3 | 演示数据扩充 | P2 | ✅ 已有 2 房间 / 3 会话 / 4 举报 |
| D4 | `app/douyin\r/` 垃圾目录 | P2 | ✅ 已加 `.gitignore` |
| D5 | 待核实功能（WS Resume / 视频压缩 / 草稿箱） | P2 | ⏳ |

---

## 三、跨平台（Windows）

| # | 事项 | 状态 | 结果 |
|---|---|---|---|
| **W1** | `platform/` 三件 | ✅ | `a0aa3bf`（14 checks） |
| **W2** | hook 白名单 | ✅ | `f504b75`（Windows stub，`enabled()` 恒 false） |
| **W3** | IOBackend Windows | ✅ | `615bdba`（`WSAPoll` + 10ms 轮询） |
| **W4** | Windows fiber | ✅ | `17a43cc`（`CreateFiber`/`SwitchToFiber`） |
| **W5** | CMake Windows 放开 | ✅ | `0e547da`（`CreateFiber` 分支） |
| **W6** | `Process::Spawn` | ✅ | `b737d53`（`CreateProcessW`，**`proc.cpp` 单一实现**） |
| **W7** | `AtomicRename`/`DeleteFileIfExists` | ✅ | `ce5b26f`（**rename 不覆盖 / unlink 打开中文件**） |
| **W8** | `os::monitor` 降级 | ✅ | `ca2a932`（Windows 空 INTERFACE） |
| **W9** | `debug` 模块降级 | ✅ | `c29c0d7`（只编 `debug_dump.cpp`） |
| **W10** | `fiber.cpp` 的 `/proc` 降级 | ✅ | `0bd12d4`（`GetModuleFileNameW` + `VirtualQuery`） |
| **W11** | `SetReuseAddr`（`SO_REUSEADDR` 语义反转） | ✅ | `7870948`（Windows `SO_EXCLUSIVEADDRUSE`） |
| **W12** | `bento/net/` 的 2 处 `SO_REUSEADDR` | 🏃 | **agent `f7ad0587` 在做** |
| **W13** | `SOCKET` 64 位 vs `int` fd 契约 | ⏳ P0 | `SpawnedProcess.fd` 是 `int`，Windows socket 是 `UINT_PTR` |
| **W14** | `errno` vs `WSAGetLastError` | ⏳ P1 | `socket.cpp` **0 处 `WSAGetLastError`**，75 处 `errno` |
| **W15** | `diagnostic_agent` 的 `GO2CPP_DIAGNOSTIC=ON` | 🏃 | **agent `361c41d8` 在做** |
| **W16** | Windows 实机验收 | ⏳ | 需 Windows 环境（目前只有 `-U__linux__` 近似验证） |

---

## 四、Go2Cpp 基座

| # | 事项 | 优先级 | 状态 |
|---|---|---|---|
| G1 | `Context` 回调列表换 `KInlineVector` | P1 | ⏳（实测小容器快 5-9×） |
| G2 | 非流式 RPC Cancel | — | ✅ `0c6ac1a` |
| G3 | `bench_tunnel` 等注册 CMake | P2 | ⏳ |
| **G4** | **`monitor_capacity` 的 5% 阈值** | P2 | ⏳ **基线也失败**（hook 环境 fd 竞争，与 JSON 无关） |

---

## 五、流程 / 规范

| # | 事项 | 状态 |
|---|---|---|
| P1 | `READ-ME-FIRST.md` 已放旧副本 | ✅ |
| P2 | 删除旧副本 `E:\CodexWorkspace\Go2Cpp` | ⏳ P2 |
| P3 | `docs/plans/next-day-plan.md` 与本 TODO 合并 | ⏳ P2 |
| **P4** | **多 agent 协作规则**（写进 `~/.dsh/AGENTS.md`） | ✅ 7 条 |
| **P5** | **路径必须写死**（防旧副本陷阱） | ✅ 写进规则 |

---

## 关键教训（本日）

| # | 教训 |
|---|---|
| 1 | **`git add <目录>` = 事故**（把另一个 agent 的改动扫进提交）——**只 add 具体文件** |
| 2 | **并行 agent 同树提交会损坏 git 对象**（`17a43cc` 等 3 个对象变空）——**必须用独立 worktree** |
| 3 | **路径必须写死真树**（`E:\CodexWorkspace\Go2Cpp` 是旧副本，**没有 `src/`**） |
| 4 | **「重启了」不等于「生效了」**——`run_all.sh stop` 曾杀不掉旧进程，导致改代码不生效 |
| 5 | **`grep` 关键词要覆盖间接引用**（`nlohmann` 藏在 `go2cpp/json.hpp` 转发后面） |

---

## 六、可裁剪性（用户核心诉求：每个模块能单独使用，不引入其他模块）

> **目标**：任意模块（如 `json` / `url` / `log` / `metrics`）可以**单独作为一个库使用**——
> 只链接它 + 它**真正必需**的依赖，**不拖入** fiber / hook / scheduler / network 等重组件。

### 6.1 现状盘点（待做）

| # | 事项 | 说明 |
|---|---|---|
| C1 | **画模块依赖图** | 用 `cmake --graphviz` 或手写（`docs/modules/dependency-graph.md`），列出「A -> B」的**真实**依赖 |
| C2 | **找出「隐性依赖」** | 例：`go2cpp_network` 是否真的需要 `fiber`？`go2cpp_log` 是否真的需要 `hook`？——**逐个验证**，不是看 CMake 写了什么 |
| C3 | **`go2cpp_hook` 的强制链接** | 现在很多 target 无条件 `target_link_libraries(... go2cpp_hook)`——**改为可选**（`GO2CPP_BUILD_HOOK` 已存在，但调用方没尊重它） |
| C4 | **`go2cpp_runtime` 的 INTERFACE 聚合** | 它把一堆模块捆在一起——**要能拆**（只取需要的） |
| C5 | **`GO2CPP_MODULE_TARGETS` 的粒度** | 现在是一个整体列表——**每个模块能否单独 `-DGO2CPP_MODULE_JSON=ON` 之类** |

### 6.2 方案（待定稿）

| # | 方案 | 说明 |
|---|---|---|
| P1 | **每个模块一个最小示例** | `examples/minimal/<module>/`：`CMakeLists.txt` **只 `find_package`/`target_link_libraries` 该模块**，编译一个 10 行程序 |
| P2 | **依赖方向守卫** | CMake 里禁止「低层依赖高层」（如 `json` 依赖 `network`）——用 `if(TARGET ...)` + `message(FATAL_ERROR)` |
| P3 | **`go2cpp_hook` 可选** | 默认 OFF；只有需要「阻塞调用自动 fiber 化」时才开——**json/log/metrics 这些纯计算模块不该被 hook 拖累** |
| P4 | **`platform/` 保持零依赖** | 它只能依赖标准库（**已经是**），供所有模块用 |
| P5 | **每个模块的 `target_include_directories` 用 `$<BUILD_INTERFACE>`** | 已经做了（`go2cpp_platform`），其他模块要跟进 |

### 6.3 验收标准（硬性）

| # | 标准 |
|---|---|
| V1 | **每个模块**都有 `examples/minimal/<module>/`，`cmake --build` 通过 |
| V2 | 该示例的**链接库清单里没有「无关模块」**（如 `minimal/json` 不含 `go2cpp_network`/`go2cpp_fiber`） |
| V3 | 用 `cmake --graphviz` 导出的依赖图**无环、无越层** |
| V4 | **关掉 `GO2CPP_BUILD_HOOK=OFF`** 后，`json`/`log`/`metrics`/`url` 等纯计算模块**仍能构建并使用** |
| V5 | 文档 `docs/modules/cuttable.md`：**每个模块的「必需依赖」清单 + 最小示例路径** |

### 6.4 我的初步观察（待验证）

| 模块 | 现在链接了什么 | 疑似可以去掉的 |
|---|---|---|
| `go2cpp_json` | `go2cpp_reflect`（+ error） | **合理**（reflect 是它的字段反射依赖） |
| `go2cpp_log` | 待查 | **可能不需要 hook** |
| `go2cpp_network` | `hook`/`scheduler`/`fiber` | **可疑**：纯 socket 封装应该不需要 fiber（**待验证**） |
| `go2cpp_os_monitor` | `network`/`scheduler`/`channel` | **可疑**：采集器为什么要 scheduler？ |
| `go2cpp_http` | 待查 | 待查 |
