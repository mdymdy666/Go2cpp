# 网络基础模块审查报告（Socket / UDP / IOManager-epoll）

- 日期：2026-10-30
- 审查对象：`include/go2cpp/network/{socket,io,io_backend,fd_manager,address}.hpp`、
  `src/network/{socket,io,io_backend,fd_manager}.cpp`
  （仓库中**不存在** `tcp_socket.hpp` / `udp_socket.hpp`；TCP/UDP 均由
  `socket.hpp` 的 `Socket` 统一承载，Unix 域同样。）
- 审计工具：`/tmp/audit_base_network/audit_base_network.cpp`（独立程序，未注册 CTest）
- 构建：`.build-bento-wsl`（Ninja，Release，`GO2CPP_BUILD_TESTS=OFF`、
  `GO2CPP_BUILD_HOOK=OFF`）
- 环境：WSL2 Ubuntu 24.04，12 vCPU / 19 GiB，GCC 13.3.0，内核 6.6.87.2
- 结论：**无 P0**；**2 个 P1**（Fiber 错误码契约、HOOK=OFF 链接缺陷）、5 个 P2。
  20 万次 IO 压测、1800 轮随机、5 个场景核心语义全部通过；无崩溃、无 fd 泄漏。
  发现 1 个测试过宽断言与 1 个覆盖缺口。

## 1. 压测数据

单次 Release 运行，未绑核。延迟含并发排队，不代表单连接极限。

| 项目 | 规模 | 成功 | 耗时 | 吞吐 | 延迟 | fd | RSS/HWM |
|---|---:|---:|---:|---:|---|---:|---:|
| TCP 短连接 | 100,000 次 connect/send/recv/close，200 并发 | 100,000 | 3.479s | 28,740 conn/s | p50 4.0ms / p95 5.5ms / p99 6.5ms / max 87ms | 无泄漏 | 14.1 / 14.8 MB |
| UDP 请求-响应 | 100,000 包 64B，200 并发 | 100,000 | 2.387s | 41,885 pkt/s | p50 2.2ms / p95 4.5ms / p99 6.0ms / max 10ms | 无泄漏 | 18.5 / 18.5 MB |
| epoll 多路复用 | 1,000 fd 同时等待，5 轮 | 5,000/5,000 | — | 全部同时唤醒 | p50 12.9ms / p95 16.5ms | 无泄漏 | — |
| wait_many | 1,000 fd × 5 轮（每轮注册+取消 1000 节点） | 5/5 | 0.034s | 147k 节点操作/s | — | 无泄漏 | — |

- epoll 等待中快照：`fd_slots=1000 epoll_registered=1000 waiters=1000/0 timers=1000`，
  说明 1000 fd 均真实注册进 epoll，不是用户态轮询。
- TCP 吞吐瓶颈在服务端：服务端为 8 线程 `Socket::accept` + echo（项目 Socket），
  28.7k conn/s 是服务端串行处理上限；客户端 100,000 次全部成功。
- epoll 1000 fd 的 12.9ms 含**外部线程 1000 次 write 系统调用**与 1000 个 Fiber
  的重新调度，不能直接当作 epoll 唤醒延迟。

## 2. 随机测试

| 模式 | 轮数 | 结果 |
|---|---:|---|
| 普通线程 | 1,200（固定种子 `0xC0FFEE`） | 15 类操作（create TCP/UDP/Unix、connect、send/recv、send_to/recv_from、accept、超时、cancel、shutdown、close、地址/错误查询）：**0 崩溃、0 断言失败**，fd 无泄漏 |
| Fiber | 600（6 变体，分批 50） | 核心断言全部通过（0 意外结果、fd 无泄漏），但 **44 次超时/取消/关闭后错误码不符合契约**（P1-1） |

- 普通线程随机中包含 285 次“预期失败”（对已关闭/未连接对象操作）、31 次对已关闭
  对象操作、9 次超时，均返回明确错误，进程稳定。
- 复现程序（`repro_recv.cpp`）连续 3 轮：普通线程 `error=110(ETIMEDOUT)`；
  Fiber 三轮分别 `0 / 11(EAGAIN) / 11`，期望均为 110。属**间歇性**缺陷，
  不是每次必现，因此单次场景 4 可能蒙混通过。

## 3. 五个真实场景

| # | 场景 | 期望 | 实际 |
|---|---|---|---|
| 1 | TCP 回环收发 1 MiB | 写完整、服务端收完整、校验和一致 | 通过 |
| 2 | 连接拒绝 / accept 超时 / 对端 RST | `ECONNREFUSED`；`ETIMEDOUT`；RST 后 `recv` 返回 0 或 `ECONNRESET` | 通过 |
| 3 | 大包 / 缓冲 | 8 MiB TCP 校验和一致；UDP 65507B 完整回显；65508B 返回 `EMSGSIZE`；64B 缓冲把 1024B 数据报截断为 64B | 通过 |
| 4 | 超时 | 普通线程 `recv(50ms)`、`set_recv_timeout(30ms)`、`accept(30ms)`、Fiber `recv(40ms)` 均返回 `ETIMEDOUT` | 普通线程/accept 通过；**Fiber 错误码间歇异常**（本次 110，复现 0/11，见 P1-1） |
| 5 | 并发 | 100 线程 × 10 连接 = 1000 连接全部成功；1000 Fiber 并发连接全部成功；fd 无泄漏 | 通过（线程 61,923 conn/s；Fiber 13,759 conn/s） |

## 4. 解耦检查

- **头文件自包含**：`address/socket/io/io_backend/fd_manager` 五个头分别单独
  `g++ -std=c++20 -Iinclude -fsyntax-only` 全部通过。
- **依赖闭包**：`g++ -MM` 展开后 `bento` 头文件计数均为 0；五个头与四个实现文件
  `grep bento` 无匹配。
- **动态依赖**：`readelf -d libgo2cpp_network.so` 的 NEEDED 仅
  `go2cpp_io/sync/context/error/scheduler + ssl/crypto`；
  `libgo2cpp_io.so` 仅 `context/scheduler/error`，均无 bento。
- **安装**：`cmake/Go2CppInstall.cmake` 用 `install(DIRECTORY include/ ...)`
  整树安装，新增头文件不会漏装。
- **P1-2**：`GO2CPP_BUILD_HOOK=OFF` 时 `libgo2cpp_network.so` 含未解析符号
  `go2cpp::hook::mark_system_nonblocking(int)`，只链接 network/io 的程序链接失败。

## 5. 现有测试复查

- 运行 `tests/network/test_io.cpp + test_network.cpp + test_fd_manager.cpp`
  （独立 runner）：**failures=0**，三个测试全部通过。
- 覆盖缺口：Fiber 内 `Socket::recv` 的**显式超时错误码**没有断言；
  `test_network.cpp` 的 Fiber 用例只覆盖 close/EBADF/ECANCELED 竞态。
- 过宽断言：`tests/network/test_network.cpp:532`
  `if (result.error != 0 && result.error != ECANCELED && ...)` 把 `error==0`
  视为合法；失败结果为 `{-1, 0}` 时应立即失败，这条断言正好掩盖 P1-1。
- SKIP：Socket/IO 测试**没有** SKIP 宏或条件跳过；但
  `tests/network/test_io_audit.cpp` 头注释写“不注册 ctest”，实际已在
  `cmake/Go2CppTests.cmake:823` 注册 `go2cpp_io_audit`（文档不一致，P2）。

## 6. 问题清单

### P0
无（无崩溃、无 fd 泄漏、无数据损坏、无永久挂起）。

### P1

**P1-1 Fiber 内 Socket 超时/取消/关闭后错误码不符合契约（间歇）**
- 证据：`repro_recv.cpp` Fiber 三轮 `error=0 / 11 / 11`（期望 110）；
  `audit_base_network random` 600 轮 Fiber 中 44 次错误码异常；
  普通线程同一路径稳定返回 110。
- 影响：调用方按 `result.error == ETIMEDOUT / ECANCELED / EBADF` 分支会漏判；
  `{-1, 0}` 还破坏 `SocketResult` 自洽性（`ok()==false` 但 `error==0`）。
- 位置：`src/network/socket.cpp:503-515`（`recv_impl` 的 `return {-1, errno}`）与
  `src/network/socket.cpp:417-471`（`wait_ready` 的 IOManager 分片分支）。
  根因未深挖（按要求不重构）；怀疑与 `IOManager::wait_for` 返回后
  `remaining()`/errno 被后续调度路径改写有关，需单独定位。
- 建议：先补 Fiber 超时错误码测试（最小复现已给），再修 `wait_ready`/`recv_impl`
  的错误传播；修前不要依赖该错误码做业务分支。

**P1-2 HOOK=OFF 构建下 network 库无法单独链接**
- 命令：`g++ ... -lgo2cpp_network -lgo2cpp_io ...`（不带 stub）→
  `undefined reference to go2cpp::hook::mark_system_nonblocking(int)`。
- 原因：`src/network/socket.cpp:288` 无条件调用该 Hook 符号；
  `cmake/Go2CppModules.cmake:331` 的 `go2cpp_network` 链接列表没有 hook；
  `go2cpp_hook` 仅在 `GO2CPP_BUILD_HOOK=ON` 时创建（同文件约 150 行）。
- 影响：`.build-bento-wsl`（HOOK=OFF）下任何“只链 network 模块”的用法失败；
  默认 HOOK=ON 并整链 `go2cpp_runtime` 时被掩盖。
- 建议：socket.cpp 对该调用加 `#if GO2CPP_HOOK`，或在 CMake 中为
  `go2cpp_network` 声明对 `go2cpp_hook` 的依赖（仅 HOOK=ON）；HOOK=OFF 时提供
  内联空实现。审计程序目前用空夹具绕过（不改变被测语义）。

### P2

1. **FdManager 读侧数据竞争**：`fd_manager.hpp:20-24` 自述 getter 无同步；
   `Socket::set_recv_timeout` 写元数据与 Hook 侧并发读构成 UB。建议读侧也持锁
   或改为原子字段。
2. **wait_many 去重 O(n²)**：`io.cpp:2434` 双重循环，1000 请求约 50 万次比较；
   另在注册前/park 前/park 后各做一次全量 `poll` 探测
   （`io.cpp:2459/2636/2717`），1000 fd 每轮最多 3000 次 fd 检查。建议用
   `unordered_set` 去重并按需探测。
3. **100ms 分片等待**：`socket.cpp:445-460` 每 100ms 重新 `wait_for` 一次；
   长等待连接会产生周期性 epoll 注册/取消（源码注释也承认每请求约 10 次
   epoll_ctl 的历史问题）。建议用单个 deadline 注册替代分片。
4. **测试注释与注册不一致**：`tests/network/test_io_audit.cpp:3` 称“不注册 ctest”，
   实际 `cmake/Go2CppTests.cmake:823` 已注册。建议改注释。
5. **TCP 压测服务端瓶颈**：服务端 8 线程 echo 的 28.7k conn/s 限制了客户端读数；
   客户端 Socket 的极限吞吐尚未单独测量。建议后续用原始 epoll echo 服务端复测。

## 7. 复现方式

```bash
# WSL，工作树 /UserData/CodexWorkSpace/Go2cpp，构建目录 .build-bento-wsl
g++ -std=c++20 -O2 -DNDEBUG -pthread -Iinclude \
  /tmp/audit_base_network/audit_base_network.cpp -o /tmp/audit_base_network/audit \
  -L.build-bento-wsl -Wl,-rpath,$PWD/.build-bento-wsl \
  -lgo2cpp_network -lgo2cpp_io -lgo2cpp_context -lgo2cpp_scheduler \
  -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_error -lgo2cpp_sync
/tmp/audit_base_network/audit all      # 全量：压测+随机+5 场景
/tmp/audit_base_network/audit random   # 随机（含 Fiber 错误码统计）
```

审计源码、日志与最小复现保存在 WSL `/tmp/audit_base_network/`。

---

## 8. P1 修复记录（2026-10-30）

- **P1-1 已修复**：根因是 `wait_or_retry` 用 `errno` 跨 park 传递错误码。Fiber 在
  `manager->wait_for()` park 后可能迁移到另一个 OS 线程，而 `errno` 是 OS 线程
  TLS；叠加 GCC 依据 `__errno_location` 的 const 属性把指针缓存在寄存器里跨调用
  复用，恢复后读到的是 park 前线程的 errno 副本（`0` / `EAGAIN`），真实
  `ETIMEDOUT` 被吞掉。修复：`recv_impl`/`send_impl` 改为显式接收 `wait_ready`
  的错误码；`wait_ready` 在 park 返回后重新检查取消/关闭/超时，并把
  `kClosed` 映射为 `EBADF`、取消映射为 `ECANCELED`、到期映射为 `ETIMEDOUT`。
  验证：最小复现 3 轮三条路径全部 `error=110`；新增 Fiber 超时（8 轮）与取消
  断言；收紧 `tests/network/test_network.cpp` 的 `error == 0` 过宽断言；临时
  恢复旧实现时新测试出现 9 次失败，证明回归断言有效。
- **P1-2 已修复**：`go2cpp_network` 在 HOOK=ON 时 PRIVATE 链接 `go2cpp_hook` 并定义
  `GO2CPP_NETWORK_HOOK_ENABLED=1`；HOOK=OFF 时该宏为 0，`Socket::accept` 对
  `hook::mark_system_nonblocking` 的调用不参与编译。验证：`nm -uC` 无 hook 未解析
  符号；不含 hook stub 的最小程序链接/运行成功；HOOK=ON 构建仍引用该符号且
  `DT_NEEDED` 含 `libgo2cpp_hook.so`。
- 详细修复报告见 `example/WarningDocement/network-p1-fix.md`。
