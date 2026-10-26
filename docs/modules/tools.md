# tools 模块（总览）

> 本文是 `tools/` 的总入口：CLI（graph/doctor/replay/migrate）、迁移工具、回放工具、
> 持久化原型。源：`tools/**`。

## 1. 定位

`tools/` 是运行时的**周边工程**（不进入 `go2cpp_runtime` 库）：统一命令行、迁移、
回放、持久化基准。构建开关：`GO2CPP_WITH_CLI`（默认 OFF）、独立工程
`tools/migration` / `tools/replay` / `tools/persistence` 各自 `build.sh` 或 CMake。

## 2. CLI（`tools/cli`）

| 子命令 | 说明 | 关键文件 |
|---|---|---|
| `graph` | 并发结构状态图（wait-for/resource/fiber-state/gmp/sequence × mermaid/dot/json） | `tools/cli/graph.{hpp,cpp}` |
| `dump` / `status` | Doctor/DebugServer 查询 | `tools/cli/dump*.{hpp,cpp}` |
| `doctor` | deadlock-report / force-wake / ctx.cancel / mutex-unlock / inject-fiber | `tools/cli/doctor*.{hpp,cpp}` |
| `replay` | G2CRPLY 录制/回放（step/next/until/gid） | `tools/cli/replay*.{hpp,cpp}` |
| `migrate` | G2CMIG03 导出/导入（含签名） | `tools/cli/migrate*.{hpp,cpp}` |
| `graph --replay` | 从回放文件生成图 | `tools/cli/graph.cpp` |

- 入口：`tools/cli/main.cpp`；帮助：`help.cpp`；退出码 0/1/2/3/4（3=降级）；
- 测试：`tools/cli/tests/run_all.sh`（213 断言）、`run_graph.sh`（91 断言）。

## 3. 迁移工具（`tools/migration`）

- `export` / `import` / `adopt`：符号化快照（G2CMIG03）、safepoint、签名；
- **只接受 version=3**（`src/format.cpp:943-944`）：CLI 能探测 v2，但**不可恢复**；
- 测试：`tools/migration/tests/run_tests.sh`（53 场景）。

## 4. 回放工具（`tools/replay`）

- G2CRPLY v5（13 种资源事件 56–68、72B payload、v1–v5 读兼容）；
- `replay`（单步/until/gid）、`record`、`verify`；虚拟时钟；
- 测试：`tools/replay/scripts/run_all.sh`（6 场景）、`go2cpp_replay_tests`；
- **依赖运行时动态库**（`libgo2cpp_debug_event.so` 等）：运行需
  `LD_LIBRARY_PATH=<build-replay>:<build-enhanced>:<build-work>`（见
  `tests/tools/run_replay.sh`）。

## 5. 持久化原型（`tools/persistence`）

- MPSC + 批量 + fdatasync；段 8MiB + manifest；112B 定长编码；
- 与 recorder 基线对比：1 线程 **2.96x**、4 线程 **13.83x**；段范围读 **9.9x**；
- 详见 [persistence.md](persistence.md)（本文档由 tools 任务补写）。

## 6. 边界

- CLI 依赖 Doctor/DebugServer 的 socket（G2CD/1、GO2CPP-DEBUG/1）；
- 迁移/回放的版本窗口：迁移仅 v3、回放 v1–v5；
- 独立工程需要先构建（`build-migration` / `build-replay` / `build-cli`），
  缺失时 ctest 以 77 SKIP。

## 7. 测试地图

| 入口 | 覆盖 |
|---|---|
| `tools/cli/tests/run_all.sh` | CLI 213 断言（dump/status/doctor/replay/migrate/graph） |
| `tools/cli/tests/run_graph.sh` | graph 91 断言（5 图 × 3 格式） |
| `tools/migration/tests/run_tests.sh` | 迁移 53 场景 |
| `tools/replay/scripts/run_all.sh` | 回放 6 场景 |
| `tests/tools/run_replay.sh`（ctest `tools-replay`） | 单测 + 6 场景（依赖 build-replay） |
