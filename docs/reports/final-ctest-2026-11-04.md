# 全量 ctest 最终验收（2026-11-04）

- 真树：`/UserData/CodexWorkSpace/Go2cpp`（WSL），HEAD `547c621`，工作区有未提交改动
- 构建目录：`.build-bento-wsl`，构建快照时间 **18:06:54**
- 命令：`ctest --test-dir .build-bento-wsl --output-on-failure`（默认串行）
- 全量耗时：716.23 s（18:12:51 – 18:25:0x）

## 一、二进制新鲜度（测试开始前）

| 产物 | mtime | 对比 `include/go2cpp/network/http.hpp`（18:03:45） |
|---|---|---|
| `.build-bento-wsl/go2cpp_tests` | 18:06:54 | 新鲜，晚于头文件 |
| `.build-bento-wsl/libgo2cpp_network.so.0.1.0` | 18:04:10 | 新鲜，晚于头文件 |

结论：测试开始时无需重建。**注意**：测试期间（18:19–18:26）并行任务修改了
`tests/tunnel/test_tunnel.cpp`、`tests/network/test_network.cpp`、`include/go2cpp/network/http.hpp`、
`src/tunnel/mux.cpp`、`mcp/src/server/server.cpp`；本报告只对 18:06:54 二进制快照有效，
不代表当前工作树。

## 二、结果总表

| 指标 | 数量 |
|---|---|
| 总数 | 284 |
| 通过 | 276（另 4 个 Skipped 被 CTest 计入 280/284，报 99%） |
| 失败 | 4 |
| 跳过（Not Run/Skipped） | 4（#71 tools-migration、#72 tools-cli、#73 tools-replay、#218 bento_netem） |

## 三、真实失败清单（已排除 Not Run / 二进制缺失 / 端口冲突）

### 1. #13 `go2cpp_tests` — 真失败（硬阻塞）

- 全量：Failed 28.81 s，1 项断言：`tests/network/test_hook.cpp:518 check failed: scheduler.worker_count() >= 2`
- 单独串行复跑 2 次（独占，39.82 s / 36.07 s），**两次均稳定 5 项失败**：
  - `tests/concurrency/test_locks.cpp:1408: torn_reads.load() == 0`
  - `tests/network/test_io.cpp:864: manager.NotifyClose(first[0])`
  - `tests/network/test_io.cpp:868: closed_result.status == IOWaitStatus::kClosed`
  - `tests/debug/test_timer_debug.cpp:527: dumps.load() > 0`
  - `tests/network/test_hook.cpp:518: scheduler.worker_count() >= 2`
- 复现：`ctest --test-dir .build-bento-wsl -R '^go2cpp_tests$' --output-on-failure`
- 根因初判：
  - `worker_count`：`test_native_fallback_grows_replacement_m`（min_workers=1/max_workers=0）下
    native 阻塞线程没有触发 replacement M 扩容。
  - `torn_reads`：配置热加载 30 s 压测中出现 RWMutex 读快照撕裂，指向今日 RWMutex 修复路径仍有缺口。
  - `NotifyClose`/`kClosed`：IO manager 的 close 通知与等待者唤醒链路丢事件。
  - `dumps`：并发 `DebugSnapshot` 2000 轮一次未成功（dump 路径被饿死或异常短路）。
- 补充：直接运行 `./go2cpp_tests`（绕过 ctest）复现 3 项（locks/timer/hook），退出码 0 但 `[test] failures=3`
  ——3 项跨运行稳定，test_io 两项为 ctest 环境附加；退出码未传播 failures，属测试框架缺陷。
- 说明：全量/单独 ctest/直接运行失败集合分别为 1/5/3 项，属负载相关的竞态放大，核心 3 项可独立复现。

### 2. #263 `tunnel` — 真失败（性能断言，非功能失败）

- 全量：Failed 7.28 s，`tests/tunnel/test_tunnel.cpp:911(现 913): ratio < 4.0`
  - 直连 p50=3.798 ms，隧道 p50=18.798 ms，比值 4.95x
- 单独串行复跑：仍失败，直连 3.388 ms / 隧道 20.141 ms，比值 5.94x
- 复现：`ctest --test-dir .build-bento-wsl -R '^tunnel$' --output-on-failure`
- 根因初判：本机直连回环基线偏低（3.4–3.8 ms），隧道固定开销（两端 RelayPipe 线程 + 2 次 connect）
  被放大；153 项检查仅此 1 项失败，功能链路正常，阈值对本机环境不成立或需预热/取更稳基线。

### 3. #284 `docs_consistency` — 真失败（文档未同步）

- 全量 0.17 s；单独复跑 0.23 s，25 项断言失败 6：
  - `docs/testing.md` 登记 `bento+mcp+runtime:290`，实际 ctest=284
  - README 缺 ctest 总数声明、缺 `N/N passed` 声明、缺工具总数与分解声明
  - docs 测试地图缺 16 个用例（app_gateway、app_realtime_resume、distributed_*、go2cpp_hook_*、tunnel 等）
- 复现：`ctest --test-dir .build-bento-wsl -R '^docs_consistency$' --output-on-failure`
- 根因初判：今日新增/更名用例后 `docs/testing.md`、`README.md` 未同步，纯文档一致性失败。

## 四、排除项

### #164 `bento_omni_server_m1` — 全量超时，非真失败

- 全量：***Timeout 180.10 s，无任何输出
- 单独串行复跑：**Passed 0.38 s**（`ctest -R '^bento_omni_server_m1$'`）
- 判定：与全量前序测试（bento_ws_server/bento_omni_server 等）的残留状态或端口重绑时序冲突，
  单独复跑即好；测试用 `bind_port = 0`，不是固定端口占用。不计入真失败，但应在完整串行链中复验。

## 五、结论

- **基座当前不可发布**，有阻塞：
  1. `go2cpp_tests` 5 项并发/IO/调度断言单人稳定复现（P0，硬阻塞）；
  2. `docs_consistency` 6 项文档断言失败（发布门禁类，可快速修）；
  3. `tunnel` 性能比值 4.95–5.94x 超 4.0 阈值（功能正常，阈值/环境问题，需定调）。
- 全量通过率 276/284（97.2%，执行口径），失败 4 中 3 为真失败、1 为环境性超时。
- 二进制与源码一致性：本报告基于 18:06:54 构建快照；并行改动已污染工作树，
  上述结论需在**新的干净构建**上复验，尤其 `go2cpp_tests`（RWMutex/IO/调度）与 `tunnel`。
