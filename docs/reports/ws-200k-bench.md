# WebSocket 长连接压测报告（Go2Cpp vs Go）

- 日期：2026-11-04
- 仓库：`/UserData/CodexWorkSpace/Go2cpp`，分支 `feat/isolated`，HEAD `fb8e2fa`
- 关联决策：`docs/reports/strategy-decision.md` 选「C++ 实时长连接网关」为首选方向，判据是
  **20 万 idle WebSocket 连接下 RSS 与心跳 p99 是否优于 Go**
- 被测代码（本轮新增，未 commit）：
  - 服务端 A：`app/ws_bench/server.cpp`（Go2Cpp，复用 `bento::WsServer`）
  - 客户端：`app/ws_bench/load.cpp`（C++17，单线程 epoll，自写 RFC 6455 握手与帧）
  - 服务端 B：`app/ws_bench/go_server/main.go`（Go 标准库，`net/http` + Hijack + 手写帧）
- 原始数据：运行机 `$HOME/wsbench-results/results.jsonl`（未入库）

---

## 0. 结论摘要

| 判据 | 5k | 10k | 20k | 结论 |
|---|---|---|---|---|
| RSS（Go2Cpp / Go） | 178.1 / 80.8 MB | 348.6 / 152.9 MB | 690.4 / 298.2 MB | Go2Cpp 是 Go 的 **2.20–2.32 倍** |
| 心跳 p99（Go2Cpp / Go） | 1.508 / 0.684 ms | 2.622 / 1.273 ms | 6.372 / 2.082 ms | Go2Cpp 是 Go 的 **2.06–3.06 倍** |
| 服务端 CPU 均值（Go2Cpp / Go） | 29.3 / 30.4 % | 49.9 / 44.5 % | 79.4 / 61.9 % | 5k 持平，10k +12%，20k +28% |

- **20k 连接全部成功**：两侧均 `created=20000 / connected=20000 / handshake_fail=0`，
  60 秒内 0 断连、0 心跳超时（Go2Cpp 心跳 1,199,343 次，全部收到 Pong）。
- **本轮结论：Go2Cpp 未优于 Go。** 在 idle 心跳场景下，RSS 约为 Go 的 2.3 倍，
  心跳 p99 为 2.1–3.1 倍，p999 在 20k 时差 10 倍（26.2 ms vs 2.6 ms）。
  按战略判据，本轮数据**不支持**「C++ 网关在内存与尾延迟上优于 Go」。
- **20 万未测**：本轮按任务步骤做到 2 万。单客户端（单一源 IP）的临时端口上限只有
  28,231 个（`ip_local_port_range=32768-60999`），20 万连接必须多客户端/多源 IP 才能建，
  超出 45 分钟时间盒，如实标注为「未测」。

> 口径提醒：被测的 `bento::WsServer` 头文件自述为「最小服务端骨架」（TLS、子协议、
> 路由、限流均未实现）。本报告结论针对**该实现 + 该压测方法**，不能直接外推为
> 「C++ 路线整体不可行」，但可以作为当前代码基线的真实数据。

---

## 1. 环境

| 项 | 值 |
|---|---|
| 内核 | `Linux 6.6.87.2-microsoft-standard-WSL2`（x86_64，Hyper-V） |
| CPU | AMD Ryzen 9 8945HX，12 vCPU（6 核 × 2 线程） |
| 内存 | 19.5 GiB（`free -m` total 20001 MB，available 18.2 GB） |
| `ulimit -Sn` / `-Hn` | 10240 / 1048576（压测前 `ulimit -n 200000`） |
| 临时端口 | `32768-60999`（28,231 个） |
| `net.core.somaxconn` | 4096 |
| 编译器 | g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| Go | go1.27.1 linux/amd64（`/usr/local/go/bin/go`） |
| 网络 | 服务端与客户端同机，走 `127.0.0.1` 回环 |

### 1.1 一个实测到的硬上限（默认 fd 限制）

第一次不带 `ulimit -n` 的 20k 试探得到：

```
created=20000 ok=10235 fail=9765 create_failed=9765
```

`10235 ≈ ulimit -Sn(10240) - 已用 fd`，即**默认限制下真实上限约 10,235 条连接**，
超出的 `socket()` 全部失败。后续所有档位都在 `ulimit -n 200000` 下运行。

---

## 2. 被测实现

### 2.1 服务端 A：Go2Cpp（`app/ws_bench/server.cpp`）

- 复用仓库现有 `go2cpp::bento::WsServer`（`include/go2cpp/bento/net/ws_server.hpp`）；
- 该实现内部用 `io::IOManager` 的 **fiber-per-connection**（与 Go 的
  goroutine-per-connection 同构），`worker_threads` 只决定 OS 线程数；
- HTTP Upgrade 由 `net::WebSocket::ValidateUpgradeRequest` + `AcceptWithPending` 处理，
  Ping 由协议层自动回 Pong，Text/Binary 由 `OnMessage` 原样回显；
- 启动参数：`--port`、`--workers`（默认 4）、`--max-conn`；
- 打印 `READY pid=<pid> port=<port>`；SIGTERM/SIGINT 后 `Stop()` 并打印 `STATS`。

### 2.2 服务端 B：Go 对照（`app/ws_bench/go_server/main.go`）

- 仅标准库：`net/http` 监听 + `Hijacker.Hijack()`；
- 手写 101 响应，`Sec-WebSocket-Accept = base64(sha1(key + GUID))`；
- 手写最小帧编解码：客户端掩码帧解析、服务端无掩码帧发送；
- Text/Binary 原样回显，Ping 回 Pong（payload 原样），Close 回 Close 后退出；
- 启动参数：`--port`、`--workers`（`runtime.GOMAXPROCS`，默认 4）、`--max-conn`；
- 与 A 打印同样的 `READY` / `STATS`，便于同一套编排脚本驱动。

### 2.3 客户端（`app/ws_bench/load.cpp`）

- 单线程 epoll 承载全部连接，非阻塞 socket，不占线程；
- 自写握手（随机 16 字节 `Sec-WebSocket-Key`、校验 `101` 与 `Sec-WebSocket-Accept` 头）
  与掩码帧收发；
- 建连按 16 条 / 2 ms 的节奏 ramp（上限每轮 4 批，避免饿死事件循环）；
- 握手后发一条 `hello-ws` 文本做回显校验；
- 进入 steady 后按 `--ping-ms`（默认 1000）给每条连接发 Ping，payload 为 8 字节序号，
  收到同序号 Pong 计算 RTT；Ping 用 `std::priority_queue` 时间堆均匀排程，避免
  2 万条连接同一毫秒爆发；
- 指标：建连成功率、握手耗时 p50/p99、心跳 RTT p50/p99/p999、断连数、Ping 超时数、
  客户端 CPU/峰值 RSS，最终输出单行 JSON。

### 2.4 公平性设置

- 两个服务端都跑 4 个 worker（Go 侧 `GOMAXPROCS=4`，Go2Cpp 侧 `worker_threads=4`）；
- 服务端 `taskset -c 2-9`，客户端 `taskset -c 0`，减少互相抢核；
- 同一份客户端二进制、同一套协议、同一档位顺序（先 Go2Cpp 后 Go）；
- 每档开始前检查端口是否释放，脚本按 pid 精确 `SIGTERM`（不用 `pkill -f`）。

---

## 3. 压测方法

1. 启动服务端，等待 `READY`，记录 pid；
2. 客户端 ramp 建连（16 条/2 ms），直到 2 万条全部有结果；
3. 进入 steady：已建立的连接按 1 秒窗口抖动排程，每秒每连接 1 次 Ping；
4. steady 持续 **60 秒**，期间每秒采样服务端 `VmRSS` 与 `utime+stime`；
5. 结束时客户端输出 JSON，脚本 `SIGTERM` 服务端并按 pid 回收；
6. 每档记录：RSS 峰值/均值、服务端 CPU 均值/峰值、握手 p50/p99、心跳 p50/p99/p999、
   断连/超时数、客户端 CPU。

RSS 与 CPU 采样均来自 `/proc/<pid>/status` 与 `/proc/<pid>/stat`，1 秒一次；
CPU 百分比 = 1 秒内进程 tick 增量 / `CLK_TCK` × 100（即「单核百分比」，>100% 表示多核并行）。

---

## 4. 结果

### 4.1 资源

| N | 服务端 | RSS 峰值 | RSS 均值 | CPU 均值 | CPU 峰值 |
|---:|---|---:|---:|---:|---:|
| 5,000 | Go2Cpp | 178.1 MB | 174.6 MB | 29.3 % | 142 % |
| 5,000 | Go | 80.8 MB | 73.8 MB | 30.4 % | 85 % |
| 10,000 | Go2Cpp | 348.6 MB | 339.9 MB | 49.9 % | 194 % |
| 10,000 | Go | 152.9 MB | 138.1 MB | 44.5 % | 99 % |
| 20,000 | Go2Cpp | 690.4 MB | 658.3 MB | 79.4 % | 204 % |
| 20,000 | Go | 298.2 MB | 259.8 MB | 61.9 % | 99 % |

每连接边际内存（用 5k→20k 的峰值差计算）：

- Go2Cpp：`(690.4 - 178.1) MB / 15000 ≈ 35.0 KB/连接`
- Go：`(298.2 - 80.8) MB / 15000 ≈ 14.9 KB/连接`

### 4.2 延迟

| N | 服务端 | 握手 p50 | 握手 p99 | 心跳 p50 | 心跳 p99 | 心跳 p999 | 断连 | Ping 超时 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 5,000 | Go2Cpp | 17.82 ms | 79.75 ms | 0.456 ms | 1.508 ms | 1.977 ms | 0 | 0 |
| 5,000 | Go | 0.92 ms | 33.37 ms | 0.325 ms | 0.684 ms | 0.859 ms | 0 | 0 |
| 10,000 | Go2Cpp | 2.41 ms | 48.78 ms | 0.848 ms | 2.622 ms | 3.406 ms | 0 | 0 |
| 10,000 | Go | 1.00 ms | 44.51 ms | 0.436 ms | 1.273 ms | 2.714 ms | 0 | 0 |
| 20,000 | Go2Cpp | 19.94 ms | 220.09 ms | 1.373 ms | 6.372 ms | 26.245 ms | 0 | 0 |
| 20,000 | Go | 1.29 ms | 242.68 ms | 0.842 ms | 2.082 ms | 2.596 ms | 0 | 0 |

心跳样本量：5k 档 299,882 次；10k 档 599,699 次；20k 档 1,199,343 次（Go2Cpp 侧），
全部收到 Pong，无超时。

### 4.3 客户端负载（同一份客户端，用于判断测量偏差）

| N | 客户端 CPU（对 Go2Cpp） | 客户端 CPU（对 Go） | 客户端峰值 RSS |
|---:|---:|---:|---:|
| 5,000 | 18.6 % | 11.8 % | 7.3 MB |
| 10,000 | 28.2 % | 17.7 % | 10.5 MB |
| 20,000 | 57.3 % | 38.9 % | 17.4 MB |

客户端是单线程，20k 档最多吃掉约 0.57 核；心跳 RTT 里包含客户端自身的排队时间。
两个服务端用同一客户端、同一档位，因此**横向差值**（p99 的 2–3 倍）可信，
但**绝对值**偏保守（真实服务端处理时延只会更低）。

---

## 5. 分析

### 5.1 RSS：线性增长，斜率差 2.36 倍

两边 RSS 都随连接数线性增长，没有出现非线性爆炸，说明 20k 档内的数据结构是
O(N) 的。斜率 Go2Cpp 约 35.0 KB/连接、Go 约 14.9 KB/连接，差距稳定在 2.2–2.3 倍。

按此斜率外推 20 万连接（**未实测，仅估算**）：

- Go2Cpp：`35.0 KB × 200,000 ≈ 7.0 GB`
- Go：`14.9 KB × 200,000 ≈ 3.0 GB`

两者都能塞进 19 GB 内存，但 Go2Cpp 的余量明显更小；200k 档必须先解决
单机源端口上限（多客户端/多源 IP），再验证 RSS 是否仍保持线性。

Go2Cpp 每连接多出的约 20.1 KB，可能来自 fiber 栈、`Socket`/`WebSocket`/`SocketStream`
对象、读写缓冲与 `m_active` 表节点；本轮没有做 heap profile，**不下结论**。

### 5.2 心跳 p99：Go2Cpp 慢 2.1–3.1 倍，尾部更差

- 5k：1.508 vs 0.684 ms（2.20×）
- 10k：2.622 vs 1.273 ms（2.06×）
- 20k：6.372 vs 2.082 ms（3.06×）；p999 26.245 vs 2.596 ms（10.1×）

20k 档 Go2Cpp 的 p999 明显翘尾（26 ms），而 Go 的 p999 只有 2.6 ms，说明 Go2Cpp 在
2 万 fiber 同时被 2 万次/秒的 Ping 唤醒时，调度或 IO 路径出现了长尾（例如 fiber 唤醒
排队、每连接多次小 syscall、锁竞争）。这是本轮最值得优化的点。

### 5.3 CPU：规模越大差距越明显

5k 档两者持平（29.3% vs 30.4%），10k 档 Go2Cpp 高 12%，20k 档高 28%。
Go2Cpp 峰值到 204%（约 2 核），Go 峰值 99%（约 1 核）——同样 2 万次/秒的 Ping/Pong，
Go 用约 1 核完成，Go2Cpp 用了约 2 核，和 p99 的差距方向一致。

### 5.4 握手：Go2Cpp 的 p50 明显更慢，但 p99 在 20k 反而略好

5k 档 Go2Cpp 握手 p50 17.82 ms、Go 0.92 ms；20k 档 Go2Cpp p50 19.94 ms、Go 1.29 ms。
但 20k 的 p99 两者都在 220–243 ms，且建连总时长都约 11.7–12.5 秒——这一段主要受
**客户端单线程 + WSL 回环 connect() 成本**限制（实测 2 万次 `socket/connect/epoll_ctl`
连续执行约 12 秒），不能把 p99 归因于服务端。握手 p50 的差距则更可能是服务端
accept→fiber 启动路径的开销，建议后续单独做 accept 路径微基准。

### 5.5 稳定性

3 档 × 2 服务端共 6 次运行：0 次建连失败（fd 足够时）、0 次断连、0 次 Ping 超时、
0 次进程崩溃。Go2Cpp 侧 `STATS` 显示 20k 条 handshake、20k 条 message，与客户端
`echo_ok=20000` 对齐。

---

## 6. 失败与限制

1. **20 万未测**。原因：单客户端源端口只有 28,231 个，200k 需要至少 8 个源 IP/客户端
   进程；本轮时间盒 45 分钟，只完成 5k/10k/20k。
2. **20k 是单机可测的实际上限附近**。20,000 / 28,231 已占 71% 端口；再多会开始
   `EADDRNOTAVAIL`（`create_failed` 上升）。
3. **客户端单线程**。20k 档客户端 CPU 57.3%，RTT 绝对值被客户端排队抬高；
   但两个服务端用同一客户端，相对比较仍成立。
4. **同机干扰**。WSL 虚拟化、服务端与客户端同机、host 侧其他负载都会影响 p99；
   已用 taskset 分离核心，但无法完全隔离。
5. **被测服务端是骨架**。`WsServer` 未实现 TLS/子协议/路由/限流，本轮只测
   「Upgrade + 帧回显 + 自动 Pong」这条最小路径；生产网关的额外功能会进一步改变
   RSS 与 p99。
6. **未做 heap profile / perf**。35.0 KB/连接与 p999 翘尾的根因未定位，只给方向。

---

## 7. 结论

在 5k / 10k / 20k idle WebSocket 连接、1 秒心跳、60 秒 steady 的条件下：

- **功能面**：Go2Cpp 的 `bento::WsServer` 能稳定承载 2 万连接，零失败、零断连、
  零心跳超时，说明 fiber-per-connection 模型在 2 万级是成立的。
- **战略判据**：RSS 是 Go 的 2.2–2.3 倍，心跳 p99 是 Go 的 2.1–3.1 倍，CPU 在 20k
  高 28%。**本轮数据不支持「Go2Cpp 在 20 万 idle 长连接上优于 Go」**。
- **下一步建议**（按收益排序）：
  1. 把每连接内存从 35.0 KB 压到 15 KB 以内（fiber 栈、缓冲、对象布局）；
  2. 定位 20k 档 p999=26 ms 的长尾（fiber 唤醒路径 / 每连接 syscall 数 / 锁）；
  3. 用多源 IP 客户端把档位推到 50k / 100k / 200k，验证 RSS 线性度与 p99 拐点。

---

## 8. 复现命令

```bash
# 0) 进入真实仓库（注意是小写 pp）
cd /UserData/CodexWorkSpace/Go2cpp

# 1) 构建三个二进制（在仓库外构建 go2cpp_bento_ws，不改仓库文件）
bash app/ws_bench/build.sh
# 产物：$HOME/wsbench-bin/{ws_bench_server,ws_bench_load,ws_bench_go}
# 依赖：cmake / g++ / go1.27；首次会 configure 并编译 go2cpp_bento_ws（约 2 分钟）

# 2) 单档：go2cpp 服务端 + 2 万连接 + 60 秒
bash app/ws_bench/run_bench.sh go2cpp 20000 60 19043 4

# 3) 单档：Go 对照
bash app/ws_bench/run_bench.sh go 20000 60 19053 4

# 4) 完整阶梯（5k/10k/20k × 两个服务端），约 7 分钟
for pair in "5000 19041" "10000 19042" "20000 19043"; do
  set -- $pair
  bash app/ws_bench/run_bench.sh go2cpp "$1" 60 "$2" 4
done
for pair in "5000 19051" "10000 19052" "20000 19053"; do
  set -- $pair
  bash app/ws_bench/run_bench.sh go "$1" 60 "$2" 4
done
```

脚本内部已做：`ulimit -n 200000`、端口占用检查、服务端 `taskset -c 2-9`、
客户端 `taskset -c 0`、每秒 RSS/CPU 采样、按 pid `SIGTERM` 回收、结果追加
`$HOME/wsbench-results/results.jsonl`。

> 注意：WSL 的 `/tmp` 会被系统清理，构建产物与结果默认放在 `$HOME` 下，不要放 `/tmp`。

---

## 附录 A：原始 JSON（6 次运行，逐字）

```json
{"server":"go2cpp","rss_peak_kb":182400,"rss_avg_kb":178792,"cpu_avg_pct":29.3,"cpu_max_pct":142,"label":"go2cpp","target":5000,"created":5000,"connected":5000,"handshake_ok":5000,"handshake_fail":0,"create_failed":0,"handshake_p50_ms":17.819,"handshake_p99_ms":79.745,"ping_sent":299882,"pong_recv":299882,"ping_timeout":0,"rtt_p50_ms":0.456,"rtt_p99_ms":1.508,"rtt_p999_ms":1.977,"disconnects":0,"echo_ok":5000,"duration_s":60,"steady_s":60.00,"elapsed_s":60.63,"client_cpu_pct":18.6,"client_maxrss_kb":7424}
{"server":"go","rss_peak_kb":82688,"rss_avg_kb":75620,"cpu_avg_pct":30.4,"cpu_max_pct":85,"label":"go","target":5000,"created":5000,"connected":5000,"handshake_ok":5000,"handshake_fail":0,"create_failed":0,"handshake_p50_ms":0.921,"handshake_p99_ms":33.371,"ping_sent":299861,"pong_recv":299854,"ping_timeout":0,"rtt_p50_ms":0.325,"rtt_p99_ms":0.684,"rtt_p999_ms":0.859,"disconnects":0,"echo_ok":5000,"duration_s":60,"steady_s":60.00,"elapsed_s":60.63,"client_cpu_pct":11.8,"client_maxrss_kb":7424}
{"server":"go2cpp","rss_peak_kb":356992,"rss_avg_kb":348011,"cpu_avg_pct":49.9,"cpu_max_pct":194,"label":"go2cpp","target":10000,"created":10000,"connected":10000,"handshake_ok":10000,"handshake_fail":0,"create_failed":0,"handshake_p50_ms":2.408,"handshake_p99_ms":48.779,"ping_sent":599699,"pong_recv":599699,"ping_timeout":0,"rtt_p50_ms":0.848,"rtt_p99_ms":2.622,"rtt_p999_ms":3.406,"disconnects":0,"echo_ok":10000,"duration_s":60,"steady_s":60.00,"elapsed_s":61.25,"client_cpu_pct":28.2,"client_maxrss_kb":10752}
{"server":"go","rss_peak_kb":156544,"rss_avg_kb":141442,"cpu_avg_pct":44.5,"cpu_max_pct":99,"label":"go","target":10000,"created":10000,"connected":10000,"handshake_ok":10000,"handshake_fail":0,"create_failed":0,"handshake_p50_ms":1.004,"handshake_p99_ms":44.514,"ping_sent":599713,"pong_recv":599713,"ping_timeout":0,"rtt_p50_ms":0.436,"rtt_p99_ms":1.273,"rtt_p999_ms":2.714,"disconnects":0,"echo_ok":10000,"duration_s":60,"steady_s":60.00,"elapsed_s":61.25,"client_cpu_pct":17.7,"client_maxrss_kb":10752}
{"server":"go2cpp","rss_peak_kb":706944,"rss_avg_kb":674104,"cpu_avg_pct":79.4,"cpu_max_pct":204,"label":"go2cpp","target":20000,"created":20000,"connected":20000,"handshake_ok":20000,"handshake_fail":0,"create_failed":0,"handshake_p50_ms":19.939,"handshake_p99_ms":220.086,"ping_sent":1199343,"pong_recv":1199343,"ping_timeout":0,"rtt_p50_ms":1.373,"rtt_p99_ms":6.372,"rtt_p999_ms":26.245,"disconnects":0,"echo_ok":20000,"duration_s":60,"steady_s":60.00,"elapsed_s":71.71,"client_cpu_pct":57.3,"client_maxrss_kb":17808}
{"server":"go","rss_peak_kb":305388,"rss_avg_kb":265990,"cpu_avg_pct":61.9,"cpu_max_pct":99,"label":"go","target":20000,"created":20000,"connected":20000,"handshake_ok":20000,"handshake_fail":0,"create_failed":0,"handshake_p50_ms":1.285,"handshake_p99_ms":242.682,"ping_sent":1199414,"pong_recv":1199336,"ping_timeout":0,"rtt_p50_ms":0.842,"rtt_p99_ms":2.082,"rtt_p999_ms":2.596,"disconnects":0,"echo_ok":20000,"duration_s":60,"steady_s":60.00,"elapsed_s":72.51,"client_cpu_pct":38.9,"client_maxrss_kb":17812}
```

字段说明：`rss_*` 单位 KB（1 秒采样）；`cpu_*_pct` 为单核百分比；`handshake_*` 从
`connect()` 起算到收到 101；`rtt_*` 为 steady 阶段 Ping→Pong；`elapsed_s` 含 ramp。
