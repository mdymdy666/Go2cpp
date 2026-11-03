# 文档与源码漂移台账（doc-drift）

> 用途：记录「文档说的」与「源码实际」不一致的地方。**这份清单是数字台账（阶段 A）的输入**。
> 规则：每发现一例，记一行；修掉后把「状态」改为已修并注明提交。

| # | 位置 | 文档说 | 源码/事实 | 证据 | 状态 |
|---|---|---|---|---|---|
| 1 | `docs/engineering-standards.md` §六 | 调度器 `160ns vs Go 135（1.19×）` | 全仓无任何报告支撑该数字；实测基线是 wall 122.0（`3b4e590`）与 submit 326.7（`127a688`），两者不同口径 | `git log -- src/concurrency/scheduler.cpp` | 已修（`18e985b`） |
| 2 | 多处口头/文档 | HTTP pipeline `411k rps` | 全库查无此数；可复核的是 pipeline=16 单连接 **72.9k**、深度 8 → 53.7k、串行 8.9k | `docs/reports/asm-http-e2e.md:18,127,234` | 已更正 |
| 3 | `user/docs/14-tunnel.md:183` | `cmake --build --target tunnel-server tunnel-client` | CMake target 是 `tunnel_server`/`tunnel_client`，横线只是 `OUTPUT_NAME` | `cmake/Go2CppTunnel.cmake:77-83` | 已修（`a8eaced`） |
| 4 | `user/docs/14-tunnel.md:298` | `go2cpp_tunnel_test --filter protocol` | 测试程序不读 argv，无 `--filter` | `tests/tunnel/test_tunnel.cpp` grep 无命中 | 已修（`a8eaced`） |
| 5 | `docs/exam/14-tunnel.md` Q17 | 「回调里调 `Stop()` 会自 join 永久死锁」 | `client.cpp:83-98` 有 self-detach 分支；真实后果是 detached 线程 + 析构后可能 UAF | 已贴源码 | 已修（`a8eaced`） |
| 6 | 出题简报 | tunnel metrics「7 个」 | 实际 **9 个**（server 6 + client 3） | `src/tunnel/server.cpp` BuildStatusJson | 已更正 |
| 7 | `docs/exam/02-network.md` Q18 | 「HTTP/2 五个 .cpp 只在测试目标编译，导出库缺符号」 | **已进 `go2cpp_network`**；CMake 注释就写着「此前只在测试目标里编译」 | `cmake/Go2CppModules.cmake:217-224` | 本次修 |
| 8 | `docs/exam/02-network.md` Q18 | 「视频每帧一次性大分配」 | 视频帧已改**按需增长**（注释明确）；仍预分配的是 `StreamReassembler`：`assembly.data.assign(frag_count*kStreamFragmentBytes, 0)` | `include/go2cpp/bento/net/channel.hpp:815-820`、`src/bento/net/channel.cpp:202` | 本次修 |
| 9 | `docs/exam/02-network.md` Q16 / `docs/reports/asm-tls.md` | ChaCha20 `13.41×` | 那是**加 SIMD 前**的口径；`crypto.cpp` 367 行起已有 AVX2 8 路 / AVX-512 16 路后端，现状未实测 | `src/bento/net/crypto.cpp:366-382` | 本次修 |

## 为什么这份清单重要

项目迭代极快（一天几十个提交、一小时可能多轮优化），文档里的数字与结论**平均滞后数小时到一天**。
九例里有 4 例是「数字无出处」、3 例是「命令/接口名写错」、2 例是「已在代码里修好但文档没跟上」。
**结论：任何文档里的性能数字都不能直接用于决策，必须现场复跑或查本台账。**
