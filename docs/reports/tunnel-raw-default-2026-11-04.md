# tunnel raw 按需启用 + soak 重跑（2026-11-04）

## 结论先行
- 大 body（8 MiB）走 raw：**2385.3 MiB/s、开销 1.27×**，≥1475 MiB/s 门禁 **达标（超 62%）**；
  同机 mux 对照 770.5 MiB/s / 3.59×；旧默认（mux 830 / 3.56×）吞吐 +187%、开销 −64%。
- 小请求（1 KiB）仍走 mux：p50 1.282 / p99 1.846 ms，与强制 raw（p50 1.111 / p99 1.584 ms）
  同量级、未变差。
- `ctest -R '^tunnel$'` 通过（170 checks、5.5 s，含 raw 并发上限用例）；4 组 soak 共 63,048 请求
  **0 错误**。测试 9 的性能断言（p50 比值 <4.0）在 load 4.58 峰值下出现过 1 次 4.04x 临界失败，
  负载回落后复跑 4 次 3.88–3.95x 全过；该请求 64 KiB 走 mux，与本次选路改动无关。

## 改法（未 commit）
| 文件 | 改动 |
|---|---|
| `include/go2cpp/tunnel/protocol.hpp` | 声明 `RawStreamThresholdBytesFromEnv()` / `ShouldUseRawStream()`；更新默认值注释 |
| `src/tunnel/protocol.cpp` | `GO2CPP_TUNNEL_RAW_STREAM` 默认 false→true（显式 0/false/off/no 仍整体回退 mux）；新增阈值解析与选路判断 |
| `src/tunnel/server.cpp` | 新增 `ParseContentLengthValue()`；`HandlePublic` 按请求 `Content-Length` 选路（替换原「全 raw / 全 mux」二选一） |
| `tests/tunnel/test_tunnel.cpp` | `TestRawStreamLimit` 显式设 `GO2CPP_TUNNEL_RAW_THRESHOLD=0`（该用例发 GET 无 Content-Length），结束时 unsetenv |

规则：`Content-Length > GO2CPP_TUNNEL_RAW_THRESHOLD`（默认 1048576 = 1 MiB）→ raw；其余
（小请求、未知长度）→ mux；`0` = 全走 raw（调试/压测），`-1` = 全走 mux。raw 协议/数据面未改。
注：HEAD `b176c7f` 的 `src/tunnel/mux.cpp` 丢唤醒修复由他人提交；本 agent 只改上表 4 个文件。

## soak 数据（WSL 环回，端口 0 自动分配 36xxx–45xxx，未用 28080/29xxx）
| 场景 | 样本 | 隧道 MiB/s | 直连 MiB/s | 开销 | 隧道 p50/p99 ms |
|---|---|---|---|---|---|
| raw 8 MiB（默认阈值） | 190 s / 31,522 轮 / 246 GiB | **2385.3**（分段 2341–2423） | 3039.2 | **1.27×** | 3.331 / **4.410** |
| mux 8 MiB（阈值 −1） | 25 s / 1,876 轮 / 14.7 GiB | 770.5（758–779） | 2766.3 | 3.59× | 10.328 / 13.409 |
| 小请求 1 KiB（默认→mux） | 15 s / 9,132 轮 | 0.7 | 3.2 | 4.22× | 1.282 / 1.846 |
| 小请求 1 KiB（阈值 0→raw） | 15 s / 10,443 轮 | 0.9 | 3.5 | 4.02× | 1.111 / 1.584 |

资源（soak 单进程内起 server/client/sink，每秒采样 `/proc/<pid>`）：
| 场景 | RSS 稳态（KB） | fd 稳态 min/中位/max | 线程 | 错误 |
|---|---|---|---|---|
| raw 8 MiB | 14,960 / 15,456 / 15,828，末 15,460（无增长） | 10 / 16 / 23（末 23） | 11–17 | 0 |
| mux 8 MiB | 17,740 / 20,972 / 23,420 | 11 / 13 / 13 | 16–23 | 0 |
| 小请求 mux / raw | 8,696–9,092 / 5,652–7,088 | 8–13 / 8–21 | 3–23 | 0 |

## raw 路径功能完整性（代码核对 + 测试）
- OPEN_RAW：`ServeRawStream` 分配 stream_id 后 `SendOpenRaw`；客户端 `on_raw_open` 起线程回拨。
- RAW_BIND：客户端发 4B id；服务端校验帧类型与 payload 长度（4B），失败断连。
- 绑定超时：`raw_bind_timeout_ms=10000`，超时回 **504 Gateway Timeout**；另有 502（无控制会话 /
  发不出 OPEN_RAW）、503（raw 额度满）路径。
- DetachConn：数据连接先从 MuxSession 摘出再 Stop，避免 Stop 关闭已移交的 fd；两端一致。
- EOF：`SplicePipe` 仅 EOF 时 `shutdown(目的端, SHUT_WR)`，失败路径 shutdown 两端隔离慢 peer。
- 并发闸门：`TryAcquireRawSlot` CAS + RAII 租约归还，ctest 上限用例（上限 2 / 并发 3 → 第 3 条 503、
  释放后恢复 200）通过。

## 阈值选择理由
1. raw 是 per-stream TCP：小请求多 1 RTT + 4 帧握手，且每流占 2 个 fd、留 TIME_WAIT；mux 复用控制连接。
2. 大 body 下 raw 走 splice 零拷贝，8 MiB 实测 3.59×→1.27×，收益随 body 增大单调上升。
3. 1 MiB 是保守拐点：1 KiB 两路延迟同量级（本地环回 raw 甚至略低，因 mux 帧化/队列开销），
   阈值不会让小请求变差；保留 mux 默认主要为 fd/TIME_WAIT 与旧客户端兼容。

## 遗留
- 老客户端不认 OPEN_RAW：大 body 会触发 raw 并断控制连接。升级顺序：**先客户端后服务端**；
  不能同时升级时服务端设 `GO2CPP_TUNNEL_RAW_THRESHOLD=-1` 回退。
- 本轮只测单连接；4 并发与 >30 min 长跑未做（时间盒）。`bench_tunnel.cpp` 仍未注册 CMake，
  soak 驱动为临时工具（`.build-bento-wsl/soak_raw*`，不入库）。
