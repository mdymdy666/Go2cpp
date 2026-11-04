# tunnel 数据面：吞吐基准 + 汇编取证 + 热路径日志

- 日期：2026-11-04
- 范围：`src/tunnel/mux.cpp`（转发路径）、`tests/tunnel/bench_tunnel.cpp`（新增基准）
- 结论一句话：**tunnel 数据面单连接 500 MiB/s、4 并发 640 MiB/s（环回，直连基线 3.3 / 8.7 GiB/s）**；瓶颈是「每 32 KiB 一次 write + 一次线程唤醒 × 4 层转发」，**不是**每 chunk 的堆分配（malloc 计数前后不变，吞吐也不变）。

## 1. 基准方法

拓扑（`bench_tunnel` 一个进程内起全套）：

```
bench 客户端 --HTTP--> tunnel-server 公网端口 --MuxSession--> tunnel-client
                                                             --HTTP--> 本地 SinkServer
bench 客户端 --HTTP--> 本地 SinkServer（直连基线）
```

- 数据面：8 MiB 的 HTTP POST body，经 RelayPipe（32 KiB chunk）→ MuxStream → DATA 帧（32 KiB/帧，`kMaxDataChunk = 65532`）→ 对端。
- 单连接吞吐：一次 POST 8 MiB，5 轮；**直连与隧道交替执行**（奇偶轮交换先后），抵消 CPU 升频/后台负载的顺序偏差。
- 4 并发吞吐：4 条连接各 POST 8 MiB，5 轮，交替执行，按 wall 算聚合 MiB/s。
- 延迟：单连接 96 次 64 KiB POST，按完成时间取 p50/p99（含每次建连，即「请求级 RTT」）。
- 完整性：SinkServer 校验收到的字节数，`requests/bytes/failures` 打印在末行；本文所有数据 `failures=0`。

复现（WSL，真树 `/UserData/CodexWorkSpace/Go2cpp`）：

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl --target go2cpp_tunnel go2cpp_tunnel_test -j12

# bench 尚未注册进 CMake（本次改动范围只含 src/tunnel 与 tests/tunnel）
cd .build-bento-wsl
g++ -O2 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic -I../include \
  ../tests/tunnel/bench_tunnel.cpp -o /tmp/bench_tunnel \
  -Wl,-rpath,/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl \
  libgo2cpp_tunnel.a libgo2cpp_frame.a libgo2cpp_relay_pipe.so.0.1.0 \
  libgo2cpp_context.so.0.1.0 libgo2cpp_scheduler.so.0.1.0 \
  libgo2cpp_fiber.so.0.1.0 libgo2cpp_hash.so.0.1.0 \
  libgo2cpp_error.so.0.1.0 libgo2cpp_log.so.0.1.0 -pthread
/tmp/bench_tunnel 5 4        # 参数：轮数、并发连接数
```

## 2. 基准数据（3 次独立运行，每次 5 轮取中位）

| 指标 | 直连 | 隧道 | 隧道开销 |
|---|---|---|---|
| 单连接吞吐（8 MiB/请求） | 3328 MiB/s | **494 MiB/s** | **6.7×** |
| 4 并发吞吐（4×8 MiB） | 8694 MiB/s | **629 MiB/s** | **13.8×** |
| 延迟 p50（64 KiB POST） | 0.273 ms | **1.453 ms** | 5.3× |
| 延迟 p99（64 KiB POST） | 0.528 ms | **1.980 ms** | 3.8× |
| 4 并发单请求完成时间 p50 | 3.54 ms | 48.80 ms | — |
| 4 并发单请求完成时间 p99 | 4.46 ms | 53.88 ms | — |

三次运行的隧道单连接中位：`508.05 / 493.75 / 493.84`（中位 493.84）；
隧道 4 并发：`642.54 / 629.16 / 628.00`（中位 629.16）。
直连基线三次：`3347.01 / 3328.10 / 2902.35`，并发 `7342.73 / 9016.74 / 8693.50`——
**直连受机器负载影响可达 ±15%，因此开销倍率（6.4×–6.9×）比绝对值更可信；隧道自身的 494/629 MiB/s 在 3 次运行里波动 <3%。**

延迟分布（最终版，96 样本）：
`tunnel mean 1.405–1.449 ms, p50 1.404–1.464 ms, p99 1.980–1.985 ms`；
`direct mean 0.279–0.285 ms, p50 0.266–0.283 ms, p99 0.483–0.608 ms`。

## 3. 汇编取证

对象：`objdump -dr --demangle .build-bento-wsl/CMakeFiles/go2cpp_tunnel.dir/src/tunnel/mux.cpp.o`
（Release，`-O3 -DNDEBUG`；重定位行用于统计 `memcpy`/`operator new`/`delete`）。

### 3.1 最终版 vs 优化前

| 热函数 | 指令数 | call 数 | memcpy | new/delete | 栈帧 |
|---|---|---|---|---|---|
| `MuxStream::Write` | 284 → **257** | 32 → 35 | 0 → 0 | 0 → 0 | 152 → **152 B** |
| `MuxStream::Read` | 221 → **163** | 23 → 12 | 2 → 2 | 0 → 0 | 72 → **40 B** |
| `MuxStream::PushData` | 133 → 133 | 14 → 14 | 1 → 1 | 0 → 0 | 64 → 64 B |
| `MuxSession::SendPayload` | 140 → **143** | 14 → 14 | 0 → 0 | 0 → 0 | 168 → **168 B** |

中间版本（日志直接内联进热函数）是反例证据：
`Write` 360 指令 / **744 B 栈帧**、`SendPayload` 340 指令 / **824 B 栈帧**。
把失败日志收敛到 `[[gnu::noinline]]` 冷段后，两者回到 257/152 B 与 143/168 B，
**低于改动前**，同时保留了日志。

### 3.2 每 chunk 的分配/拷贝点（源码级，32 KiB 一个 chunk）

| 位置 | 拷贝 | 分配 | 说明 |
|---|---|---|---|
| `MuxStream::Write` → `EncodeData` | 32 KiB | 1 次（**已消除**） | payload 循环外复用，`clear()` 保留容量 |
| `MuxSession::SendPayload` → `FrameCodec::Encode` | 32 KiB | 1 次 | wire = 12 B 帧头 + payload |
| `FrameCodec::Feed` → `ExtractFrame` | 32 KiB | 1 次 | 帧载荷从重组缓冲拷出 |
| `MuxStream::PushData` | 32 KiB | 1 次 | 接收队列每 chunk 一个 `vector` |
| `MuxStream::Read` | 32 KiB | 1 次释放 | 队列 front 拷给 RelayPipe 后 `pop_front` |

即每 32 KiB 业务数据约 **5 次 32 KiB 拷贝（5× 放大）+ 3 次堆分配**；
再加上 server/client 各一层 RelayPipe 自身的 read/write 缓冲，实际内存放大接近 7×。

### 3.3 为什么说瓶颈不是分配

用 `LD_PRELOAD` 计数 `malloc/free`（同一 bench 输入、同一进程拓扑）：

| 版本 | malloc 次数 | free 次数 |
|---|---|---|
| 优化前 | 24080 | 24071 |
| 最终版 | 24172 | 24158 |

`payload` 复用理论上每 8 MiB 省 256 次分配，但总分配计数在 ±0.4% 内**没有下降**——
说明单 chunk 的 payload 分配只占全部分配的小头（wire、帧载荷、队列 vector 才是大头），
而吞吐在优化前后同样是 ±2% 噪声。**分配不是这个数据面的瓶颈。**

真正的瓶颈（按每 chunk 62 µs 的实测反推）：32 KiB 数据要经过
「公网 socket 写 → RelayPipe 队列 → MuxStream::Write → 控制连接 write →
对端 ReceiveLoop 唤醒 → Dispatch → 队列 → RelayPipe 读线程 → 本地 socket 写」，
**每 32 KiB 至少 2 次 write syscall + 2 次线程唤醒 + 2 次 poll**。
8 MiB / 32 KiB = 256 个 chunk，单连接 16 ms 即 ~62 µs/chunk，
远高于 32 KiB memcpy 的 ~2 µs，差值就是 syscall + 线程调度的固定成本。

## 4. 改动清单（只动 `src/tunnel/mux.cpp`）

1. **payload 复用**（`MuxStream::Write`）：`std::vector<std::uint8_t> payload` 从循环内提到循环外；
   `EncodeData` 只 `clear()` 不释放容量，稳态每 chunk 少一次 32 KiB 分配。
   （总分配计数未变，属正确性无风险的清理，保留。）
2. **转发路径失败日志**（`go2cpp::logging`，`tunnel.mux`）：
   - `MuxStream::Write`：`DATA encode failed` / `DATA send failed`（stream id、offset、chunk、错误文本）；
   - `MuxSession::SendPayload`：`frame write failed`（帧类型、字节数、写错误）；
   - `MuxSession::Dispatch`：`recv queue overflow`（stream id、字节数）；
   - `MuxSession::ReceiveLoop`：`control read failed`（errno）、`frame decode failed`（错误文本）。
3. **冷段封装**：上述热路径日志收敛到 `[[gnu::noinline]] LogStreamWriteFailure / LogFrameWriteFailure`，
   避免日志流构造被内联进每 chunk 调用的 `Write` / `SendPayload`（栈帧 744 B → 152 B）。

改动前转发路径（`mux.cpp`）**0 条日志**；改动后 5 个日志点（2 个 `noinline` 冷段函数 + 3 处内联），覆盖 6 条失败分支，全部在失败路径上。

## 5. 正确性

```
$ cd .build-bento-wsl && ctest -R tunnel --output-on-failure
1/1 Test #261: tunnel ... Passed  4.32 sec
100% tests passed, 0 tests failed out of 1
```

- 既有 `test_tunnel.cpp` 全绿（协议、端到端、4 MiB 大文件、鉴权、重连、并发、直连 vs 隧道延迟）；
- `bench_tunnel` 末行 `integrity requests=... bytes=... failures=0`，8 MiB body 的字节数逐个请求核验；
- 3 轮重复基准结果稳定（隧道单连接 493–508 MiB/s，4 并发 616–642 MiB/s）；
- 编译零告警（`-Wall -Wextra -Wpedantic`，`make go2cpp_tunnel go2cpp_tunnel_test` 无 warning）。

## 6. 未做 / 后续

- **`writev` 合并写**、**零拷贝（`ByteView` 直通 RelayPipe 缓冲）**：需要改
  `include/go2cpp/tunnel/mux.hpp`（队列元素类型、`SendPayload` 接口）与帧层，
  超出本次「只改 `src/tunnel/` + `tests/tunnel/`」的边界。按当前数据，
  合并写预计只能省掉每 chunk 一次 syscall（~10–20 µs），收益上限约 20–30%，
  要做需单独立项。
- **单帧 64 KiB 上限**（`kMaxPayload = 65536`）是另一个几何约束：即使 RelayPipe
  的 `copy_buf` 提到 64 KiB，一个 chunk 仍要拆 2 帧，除非改协议常量。
- 当前吞吐（0.5–0.64 GiB/s）离「数据面 ≥10 GB/s」的标准很远，但该标准针对
  内存数据面；tunnel 是**网络转发**，每字节都要过 2 次内核 socket 缓冲，
  回环上限本身就在 GB/s 量级。若后续要以 10 GB/s 为目标，只能换
  共享内存/io_uring 路径，不是指令级优化能解决的。

## 合并写优化（2026-11-04）

目标：隧道开销 ≤2×（单连接 ≥1443 MiB/s、4 并发 ≥3640 MiB/s，按本机直连中位折算）。

### 1. 改动清单（7 个代码文件，全部在 `src/tunnel/` + `include/go2cpp/tunnel/`；另加本文档）

| 文件:行 | 改动 | 作用 |
|---|---|---|
| `include/go2cpp/tunnel/net.hpp:130` | 新增 `TcpConn::WriteV` 声明 | 批量写入口（`iovec` 最多 64 段） |
| `src/tunnel/net.cpp:399` | `TcpConn::WriteV` 实现 | 一次 `writev(2)` + 部分写推进 + `EAGAIN`→`poll(POLLOUT)`；与 `WriteAll` 共用 `m_write_mutex`，帧不交错 |
| `include/go2cpp/tunnel/protocol.hpp:90` | 新增 `kMaxSendBatchFrames=16`、`kFrameHeaderBytes=12` | 单批最多 16 帧（≈1 MiB）、帧头长度 |
| `include/go2cpp/tunnel/mux.hpp:262` | 新增 `MuxSession::SendData(stream_id, data, size)` | 数据面唯一出站提交点，整段切帧 + 一次 `writev` |
| `src/tunnel/mux.cpp:403` | `SendData` 实现 | 帧头落栈缓冲、body 直接进 iovec（零中间拷贝）；启用帧尾 CRC 时回退逐帧 `EncodeData`+`SendPayload` |
| `src/tunnel/mux.cpp:214` | `MuxStream::Write` 改为一次 `SendData` | 改前每 32 KiB chunk 一次 `EncodeData`（1 次 32 KiB 拷贝）+ 一次 `send` + 一次发送锁；改后每 256 KiB 一次 `writev`（4–5 帧） |
| `src/tunnel/mux.cpp:177` | `MuxStream::Read` 一次锁内取多元素 | 一次 `Read` 拼满调用方缓冲（最多 256 KiB），锁/唤醒从「每 64 KiB 一次」降到「每 256 KiB 一次」 |
| `src/tunnel/mux.cpp:253` | `PushData` 增加 `notify` 参数 | 批量投递时同一流的连续帧只唤醒一次 |
| `src/tunnel/mux.cpp:734` | `ReceiveLoop` 相邻同流 DATA 帧只对末帧置 `notify=true` | 一次 `recv`（512 KiB 缓冲）内 8 帧只发 1 次 futex 唤醒 |
| `src/tunnel/server.cpp:1161` / `client.cpp:519` | `copy_buf` 32 KiB→256 KiB、`queue_cap` 256 KiB→512 KiB | 每次线程接力搬运 8 倍数据，接力次数降 8 倍 |
| `src/tunnel/mux.cpp:703` | 接收缓冲 64 KiB→512 KiB | 一次 `recv` 拿 2 帧，减少半帧续接 |

为什么帧保持 64 KiB 而不是 256 KiB：实测把 `kMaxPayload` 提到 256 KiB 后，单连接 +19%（687→820），
但 **4 并发 −15%**（1065→902）——单帧 256 KiB 的重组缓冲 + 队列元素跨核迁移压垮 L2（每流 512 KiB 队列），
故回退 64 KiB，靠 `writev` 一次提交 4–5 帧来省 syscall。

### 2. 前后数据（`taskset -c 0-7`，每次 5 轮，3 次独立运行取中位，回环）

| 指标 | 改前 | 改后 | 变化 |
|---|---|---|---|
| 单连接吞吐（8 MiB POST） | 475.16 MiB/s | **754.05 MiB/s** | **+59%** |
| 单连接开销（直连/隧道） | 6.55×（直连 3029） | **3.40×**（直连 2886） | −48% |
| 4 并发吞吐（4×8 MiB） | 686.73 MiB/s | **858.77 MiB/s** | **+25%** |
| 4 并发开销 | 9.61×（直连 6502） | **7.93×**（直连 7280） | −17% |
| 隧道延迟 p50（64 KiB POST） | 1.438–1.471 ms | **1.416–1.466 ms** | 持平（+0.7%） |
| 隧道延迟 p99 | 1.980–2.233 ms | **2.036–2.116 ms** | 持平 |
| 4 并发单请求 p50 / p99 | 43.8–46.1 / 48.0–59.6 ms | **30.2–32.1 / 39.4–42.1 ms** | −30% / −20% |

三次运行的隧道绝对值：单连接 `643.19 / 858.67 / 754.05`；4 并发 `957.01 / 827.76 / 858.77`。
完整性：每次 `integrity requests=242 bytes=432013312 failures=0`（8 MiB body 逐请求核验）。
`ctest -R tunnel` 5/5 通过（含 `ratio < 4.0` 的延迟防回归检查，实测比值 3.55–3.94×）。

### 3. 为什么只到 3.40× / 7.93×，没到 ≤2×

单连接要 ≤2× 需 1443 MiB/s（直连 2886 的一半），当前 754，差 1.9 倍。按 256 KiB chunk 折算，
当前每 chunk 约 **330 µs**，拆成：

| 成本项 | 每 256 KiB 估算 | 依据 |
|---|---|---|
| memcpy ×5 | ~130 µs | relay 读缓冲→队列、`FrameCodec` 重组、`PushData` 入队、`MuxStream::Read` 出队、relay 写队列→socket；5 × 256 KiB @ ~10 GB/s |
| 线程接力 ×4 | ~160 µs | 「public 读线程 → relay 写线程 → mux 发送 → 对端 ReceiveLoop → PushData 唤醒 → relay 读线程 → 落地」中实际的 4 次跨线程唤醒（每次含 futex + 调度，40 µs 量级） |
| syscall ×3 | ~30 µs | `writev` 1 次 + `recv` 1 次 + 落地 `write` 1 次 |
| 锁/分支/统计 | ~10 µs | `m_send_mutex` + `streams_mutex` + 原子计数 |

**结论：瓶颈已从「每 32 KiB 一次 syscall」转为「每 256 KiB 仍有 4 次线程接力 + 5 次 memcpy」，
其中 4 次接力里 3 次由 `relay::RelayPipe` 的「读线程 + 有界队列 + 写线程」结构决定**——
`src/tunnel/` 只贡献中间 1 跳。要真正压到 ≤2× 必须把 RelayPipe 换成「同一线程读→写」的直通，
或两端共享内存 / `io_uring`；这需要改 `include/go2cpp/relay/relay_pipe.hpp`（本次可改范围之外）。

4 并发扩展性更差（7.93×）的两个串行点也在此：一条 `ReceiveLoop` 承担全部流的解帧与投递、
一把 `m_send_mutex` 串行化全部流的 `writev`。把发送改为 per-session 发送线程 + 无锁队列，
预计能再拿 20–30%，但仍不足以到 2×。

继续加大 `copy_buf`（1 MiB）单连接到 959 MiB/s（3.24×），但**延迟比值升到 4.04–4.15×**，
触发 `test_tunnel.cpp:911` 的 `ratio < 4.0`（每请求新建流要新分配 1 MiB 读缓冲 ×2 侧）。
最终取 256 KiB：吞吐 754 / 4 并发 859，延迟 6/6 通过阈值。

## RelayPipe 直通（2026-11-04）

目标：验证「读线程 → 有界队列 → 写线程」是否就是单连接开销 3.40× 的主因，并尝试压到 ≤2×。

### 1. 设计

开关：`PipeConfig::direct`（代码级）+ `GO2CPP_RELAY_DIRECT=1|true|on|yes`（进程级，默认关）。
只改 relay 层，不动 tunnel 协议与调用方语义：

| 模式 | 每方向线程 | 数据路径 |
|---|---|---|
| 默认 | 2（读 + 写） | `Read → 有界队列（512 KiB）→ Write` |
| 直通 | 1（读即写） | `Read → Write`，读缓冲直接作写缓冲 |

保留的语义（与默认模式逐项对齐）：

- **背压**：默认是「队列满则读线程阻塞」，直通是「写阻塞则读线程阻塞」——读端永不少于写端快超过一个 `copy_buf`，比 512 KiB 队列更严格；
- **慢 peer 隔离**：仍按 `write_timeout` 判定，看门狗只关闭**本方向的目的端**，另一方向继续；
- **超时/取消/EOF/统计**：复用同一套 `Abort/ForceStop/BeginWrite/EndWrite/AddDropped`，`dropped`/`timeouts` 口径不变；
- 直通不再有「队列停滞」事件（没有队列），慢 peer 只能表现为写 deadline——与默认模式的覆盖等价（队列停滞本质也是「写不出去」）。

改动（file:line）：

| 文件:行 | 改动 |
|---|---|
| `include/go2cpp/relay/relay_pipe.hpp:182` | `PipeConfig::direct`（默认 false）+ 字段文档 |
| `include/go2cpp/relay/relay_pipe.hpp:56` | 头注释补「直通模式」线程模型 |
| `src/relay/relay_pipe.cpp:78` | `EnvDirectMode()`：读环境变量 |
| `src/relay/relay_pipe.cpp:107` | `NormalizeConfig` 合并开关（字段优先） |
| `src/relay/relay_pipe.cpp:416` | `Start()`：直通只起一条泵线程 |
| `src/relay/relay_pipe.cpp:534` | `DirectLoop()`：读到就写 |
| `src/relay/relay_pipe.cpp:700` | `WriteChunk` 改收 `(const char*, size)`，直通零 chunk 分配/拷贝 |

调用方（`src/tunnel/server.cpp:1164`、`src/tunnel/client.cpp:522`）零改动：开关默认关，由环境变量打开。

### 2. 前后数据（环回，`taskset -c 0-7`，每次 3–5 轮）

| 指标 | 默认（队列） | 直通 | 变化 |
|---|---|---|---|
| 单连接吞吐（8 MiB POST）中位 | 815 MiB/s（665/877/945/755） | 747 MiB/s（707/803/778/717） | −8%（噪声 ±15%） |
| 单连接开销 | 3.23–4.38× | 2.86–4.31× | 噪声内 |
| 4 并发吞吐中位 | 866（655/1140/938/793） | 843（852/1114/1231/863） | 持平 |
| 延迟 p50（64 KiB POST） | 1.380 ms（1.380/1.346/1.528） | **1.068 ms**（1.066/1.100/1.068） | **−23%** |
| 延迟 p99 | 1.965 ms（1.965/1.923/2.447） | **1.538 ms**（1.538/2.658/1.525） | −22% |
| 4 并发单请求 p50 | 24.3–32.1 ms | 27.8–35.7 ms | 噪声内 |

完整性：每次 `integrity requests=222 bytes=264241152 failures=0`。
正确性：`ctest -R tunnel` 在默认与直通下均 5/5 通过；`go2cpp_relay_pipe_test` 8/8（1e7/1e8 双向、慢 peer、写 deadline、4 管道隔离、40 轮随机、取消、API 边界）。

**结论修正**：直通**不是吞吐优化**。省掉的 1 次线程接力被「读-写串行化」抵消——默认模式在双核上读/写两线程可以重叠（一边 socket 读、一边 socket 写），直通把两者放进同一线程后流水线消失；吞吐因此在噪声内。真实收益是**延迟**：少了 512 KiB 队列的排队时间，p50 稳定 −23%（4 次独立运行方向一致）。

### 3. 为什么仍到不了 ≤2×：段数与拷贝的物理上限

用 Python + `splice(2)` 搭「零拷贝转发链」，测「链路段数 → 吞吐」（8 MiB 单流，3 次取中位）：

| 段数 | 吞吐 | 相对 1 段 |
|---|---|---|
| 1 | 2517 MiB/s | 1.00× |
| 2 | 1851 | 1.36× |
| 3 | 1291 | 1.95× |
| 4 | 739 | 3.41× |

tunnel 的数据面恰好是 **3 段 TCP**（bench→server 公网口、server→client 控制连接、client→本地服务），但每段都做用户态拷贝并套一层 mux 帧化（每 64 KiB 帧：`FrameCodec` 重组 1 次 + `ExtractFrame` 1 次 + `PushData` 1 次 + `MuxStream::Read` 1 次，另加 2–3 次堆分配）。
实测单连接 755–945 MiB/s，与「4 段零拷贝链」的 739 同量级——即**当前多出来的成本 ≈ 多走一段零拷贝链**。

3 段零拷贝链的上限是 1291 MiB/s（1.95×），恰好卡在 2× 门槛；这说明：

1. 2× 不是靠删 relay 线程能达到的，需要**每段逼近零拷贝**（`splice`/`sendfile`/`io_uring`，或让 relay 直接转发 socket fd）；
2. 只优化 relay 最多回收「一段零拷贝链」的成本；本机噪声下这部分约为 0–10%。

### 4. 状态与后续

- 直通默认关，保留为低延迟选项；`GO2CPP_RELAY_DIRECT=1` 可用于对 p50 敏感、吞吐不敏感的转发场景。
- 若要以 ≤2× 为目标：优先做 `TcpConn` 的 `splice` 转发（数据面完全绕开用户态缓冲）或 `io_uring` 批量收发，其次才是 mux 帧层零拷贝（`ByteView` 直通）；删线程已被证伪。
- 注意：`PipeConfig` 新增字段是源码级 ABI 变更，所有使用方（含 `go2cpp_relay_pipe_test`）必须重编；本次验证中「旧测试二进制 + 新库」曾表现为慢 peer 用例失败（读到错位的 `direct`），重编后 8/8 通过。

> 测量前提说明：本机同时有其他任务在跑，直连基线在 2227–3463 MiB/s 间波动（±50%），因此上表用「隧道绝对值中位」而非「开销倍率」做主结论；开销倍率只作参考。
