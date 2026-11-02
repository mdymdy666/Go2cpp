# MCP Socket 工具审计报告（socket_list / socket_stats / socket_events / socket_close）

- 日期：2026-10-30
- 审查对象：`mcp/include/mcp/tools/socket_{registry,tools}.hpp`、
  `mcp/src/tools/socket_{registry,tools}.cpp`、
  `mcp/tests/{test_socket_tools.cpp,test_socket_whitelist.cpp,test_socket_e2e.py,socket_test_util.hpp}`
- 构建：`.build-mcp-socket-wsl`（Release）、`.build-mcp-socket-asan`（Debug + ASan/LSan）
- 环境：WSL2 Ubuntu 24.04，内核 6.6.87.2，GCC 13.3.0，12 vCPU / 19 GiB
- 边界：未改 base（`src/`、`include/`），未 commit
- 结论：4 个工具完成；`mcp/` 编译 0 警告；35/35 ctest 通过；ASan/LSan 0 报告。
  **业务侧尚未自动登记 socket**（第 7.1 条），是唯一未接入项。

## 1. 工具与数据源

| 工具 | 输入 | 输出 |
|---|---|---|
| `socket_list` | `include_unobserved`（默认 true） | 本进程 socket：fd/协议/本地/远端/状态/registered/closable |
| `socket_stats` | `fd` | 应用层收发/错误/超时 + `tcp`（TCP_INFO）+ 事件计数 |
| `socket_events` | `fd`、`n`（1..256，默认 20） | 最近 n 条事件，时间升序 |
| `socket_close` | `fd`、`reason`（≤200 字符） | 关闭结果 + 审计 id/sink |

数据源选型（任务要求调研 `SocketDebug` 或适配现有 registry）：

- 仓库**不存在** `SocketDebug` 类型（`grep -rn SocketDebug` 0 命中）；
- 现有 `go2cpp::os::monitor::TcpMonitor` 是 eBPF tracepoint **全系统**采集：按四元组
  聚合重传/RTT、需 root，**没有 fd 维度、没有应用层收发字节、没有登记/关闭事件**
  （`include/go2cpp/os/monitor/tcp_monitor.hpp:1-40`），无法支撑 `stats(fd)` /
  `events(fd)`；
- 故新增 `SocketRegistry`（进程内轻量注册表）：`/proc/self/fd` + `SO_TYPE` +
  `getsockname/getpeername` + `TCP_INFO`，只覆盖本进程。

## 2. 白名单与审计

- **登记**：`/proc/self/fd/<fd>` 必须是 `socket:[inode]`，保存 inode；
  `socket_close` 只关闭「已登记 + inode 一致 + 未关闭」的 fd。
- **ABA**：fd 关闭后被 `dup2` 成文件或新 socket 时 inode 不一致 → 拒绝关闭。
- **fail-closed**：审计写入失败即拒绝关闭，目标 fd 保持打开。
- **审计**：JSON Lines，默认 `log/mcp-socket-audit.log`
  （`GO2CPP_MCP_SOCKET_AUDIT` 覆盖），`O_APPEND|O_CREAT` 0600；
  每条含 `ts/pid/action/fd/reason/result/audit_id`，拒绝路径写 `result=denied`。

## 3. 测试

```bash
cmake -S . -B .build-mcp-socket-wsl -DGO2CPP_WITH_MCP=ON -DMCP_BUILD_TESTS=ON
cmake --build .build-mcp-socket-wsl -j8
ctest --test-dir .build-mcp-socket-wsl -R socket --output-on-failure
```

| 套件 | 用例 | 断言 | 覆盖 |
|---|---:|---:|---|
| `test_socket_tools` | 8 | 1479 | 注册/schema、真实 TCP list、TCP_INFO 收发、事件顺序/截断、close+审计、UDP/Unix、随机 fuzz、压测 |
| `test_socket_whitelist` | 8 | 84 | 未登记/移出白名单/普通文件拒绝、ABA、fail-closed、文件审计 0600、环形溢出、外部关闭、8 线程并发 |
| `test_socket_e2e.py` | 6 组 | 34 | 真 `mcp_server`（stdio）：tools/list、非法 fd、fd=0 拒绝、协议 -32602、审计文件、正常退出 |

全量：`ctest --test-dir .build-mcp-socket-wsl` → **35/35 通过**。

## 4. 压测与随机

| 项目 | 条件 | 结果 |
|---|---|---|
| 混合查询 | 2000 次 list/stats/events 轮转，4 个真实 TCP 连接 | **14,504 ops/s**；P50 11.5µs / P95 199.8µs / P99 234.0µs；0 失败 |
| 并发 | 8 线程 × 100 次查询，与 close 竞争 | 0 失败、0 崩溃 |
| 随机 fuzz | 1000 组（固定种子 20261030） | 接受 317 / 拒绝 683；0 崩溃，成功结果均可解析为 JSON |
| ASan/LSan | Debug + `-fsanitize=address` | 0 报告，exit=0 |

压测为单进程未绑核，绝对值受宿主负载影响约 ±10%。

## 5. 真实场景（覆盖全部 4 个工具与错误路径）

| # | 场景 | 期望 | 实际 |
|---|---|---|---|
| 1 | TCP 环回 4 KiB → `socket_stats` | `tcp.available=true`，`received_bytes≥4096`，state=established | 通过 |
| 2 | TCP 反向 1 KiB | `sent_bytes` 增长（内核已确认） | 通过 |
| 3 | UDP 未连接 socket | protocol=udp，state=unconnected，`tcp.available=false` | 通过 |
| 4 | AF_UNIX socketpair → close 一侧 | unix_stream/connected；对端 `recv` 返回 0（EOF） | 通过 |
| 5 | `socket_close` + 重复关闭 | 首次真关闭并写审计；二次 denied 且留痕 | 通过 |
| 6 | 未登记 / 已 Unregister / 普通文件 fd | 拒绝关闭，fd 仍可用，审计 result=denied | 通过 |
| 7 | fd 复用（dup2 成文件 / 新 socket） | inode 不一致 → 拒绝，新 fd 不被误关 | 通过 |
| 8 | 审计汇写失败（模拟磁盘满） | fail-closed：拒绝关闭，fd 保持打开 | 通过 |
| 9 | 外部 close 后查询 | list 不再展示；events 保留 closed；stats 明确报错 | 通过 |
| 10 | e2e 真服务（stdio） | 4 工具可见；fd=0 拒绝并落审计；服务存活 | 通过 |

## 6. 构建质量

- clean 重建 `mcp_core/mcp_server/test_socket_*`：exit 0，**`mcp/` 路径 0 警告**；
- base 既有 24 条警告（`src/debug`、`src/concurrency`、`src/network/http2`、
  `include/go2cpp/container`）非本次引入，未触碰。

## 7. 限制与已知问题

1. **不拦截 socket 创建**：未改 `src/network`，业务需显式
   `SocketRegistry::Register(fd, label, &err)` 才进入白名单；未登记 socket 只读展示。
   **待接入**。
2. **事件仅进程内**：重启丢失；每 fd 环形 256 条，溢出丢最旧并累加 `dropped_events`。
3. **UDP/Unix 无 TCP_INFO**：字节数只来自应用层埋点，`tcp.available=false`，不猜数值。
4. **统计不相加**：`sent_bytes/recv_bytes` 取内核与应用层的较大值，两者分列，避免重复计数。
5. **close 的 TOCTOU**：锁保护注册表但不保护内核 fd 表；业务线程若在探测与 `close()`
   之间对同一 fd 并发关闭，理论上仍可能关到被复用的 fd（未复现）。
6. **审计非事务**：崩溃可能丢最后一行；文件权限固定 0600。
7. **平台**：仅 Linux（`/proc` + `TCP_INFO`）。

## 8. 半成品评估（上次进度 vs 本次补齐）

- 上次已完成：`SocketRegistry`、4 个工具、`application.cpp` 注册、CMake 接线、
  README/architecture 的 33 工具计数、三套测试骨架。
- 本次验证：clean 重建 `mcp/` 0 警告；全量 35/35 ctest 通过；ASan/LSan 0 报告。
- 本次补齐：按项目测试规范缺**随机测试**与**可量化压测**，在 `test_socket_tools.cpp`
  新增 `TestRandomArguments`（1000 组 fuzz）与 `TestThroughput`（2000 次分位统计）。
- 未完成：业务侧自动登记（须改 `src/network`，超出本次边界）。
