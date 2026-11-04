# 告警清理报告（2026-11-04）

## 结果

| 类型 | 修前 | 修后 |
|---|---:|---:|
| `-Wunused-function` | 23 | 0 |
| `-Wunused-result` | 15 | 0 |
| `-Wcomment` | 14 | 0 |
| `-Wswitch` | 2 | 0 |
| **合计** | **54** | **0** |

54 条编译输出对应 36 个唯一源位置（头文件告警按包含它的 TU 重复计数）。未用 `-w`、未关闭任何告警。

## 修法

- **`-Wcomment`（3 处 / 14 条）**：`include/go2cpp/container/{container,list,ring}.hpp` 的 Doxygen 注释含 `tests/container/*`，其中 `/*` 被当作嵌套注释起始；改为「`tests/container/` 下的用例」。
- **`-Wswitch`（2 条）**：`ChannelStatus` 新增 `kNoScheduler` 后两处状态名映射表未覆盖，补 `case` 而不加 `default`，保留枚举穷尽检查。
- **`-Wunused-function`（16 处 / 23 条）**：
  - 删除真死代码 5 处（匿名 namespace、全仓无调用）：`debug_dump.cpp` 的 `IsChannelResourceCode` / `IsLockEventCode`、`hpack.cpp` 的 `DecodeName`、`test_tunnel.cpp` 的 `ResponseHeader`、`test_http2_boundary.cpp` 的 `Check`。
  - `[[maybe_unused]]` 标注 11 处：仅在诊断宏开启时才被调用的助手——`debug_event_ring.cpp` 5 个（`GO2CPP_DEBUG_DUMP` / `GO2CPP_DEBUG_RESOURCES` 分支内调用）、`test_debug_dump.cpp` 4 个（`GO2CPP_DEBUG_DUMP` 段内调用）、`test_resource_history_stress.cpp` 2 个（`GO2CPP_DEBUG_RESOURCES` 段内调用）。这些函数在宏开启构建中仍有调用点，删除会破坏诊断构建，故标注保留。
- **`-Wunused-result`（15 条）**：测试中的 `(void)::read/write/pipe` 无法抑制 `warn_unused_result`；改为显式接收（`ConsumeIoResult`，原样返回，注释说明「返回值仅作调用已发生凭证，契约由缓冲区内容断言覆盖」），共 14 处，另 1 处裸 `::write` 同样纳入。

## 验证

- 修改前干净构建：`grep -c 'warning:'` = 54（`/tmp/build-full.log`）。
- 修改后全量构建：`grep -c 'warning:'` = 0、`error:` = 0。
- 改动涉及的 11 个 `.cpp` 用 `compile_commands.json` 逐文件复编译：0 warning。
- 三个容器头文件用 `-Wall -Wextra -Wpedantic -fsyntax-only` 复验：0 warning。
- ctest：286 项中 284 通过；`tunnel` 仅并行调度下抖动（单跑通过）；`docs_consistency` 属既有文档漂移（登记 290 / 实际 286），非本次改动引入。

## 改动文件（14）

`include/go2cpp/container/{container,list,ring}.hpp`、`src/debug/{debug_dump,debug_event_ring}.cpp`、`src/network/http2/hpack.cpp`、`tests/concurrency/test_state_machine_matrix.cpp`、`tests/debug/{test_debug_dump,test_resource_history_stress}.cpp`、`tests/network/{test_hook_correctness,test_http2_boundary}.cpp`、`tests/os/test_os_hook_conflict.cpp`、`tests/scenarios/channel_deadlock/channel_deadlock_demo.cpp`、`tests/tunnel/test_tunnel.cpp`。
