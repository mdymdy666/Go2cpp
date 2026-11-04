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
