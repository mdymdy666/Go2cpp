# 网络基础 P1 修复报告（Fiber recv 错误码 / HOOK=OFF 未解析符号）

- 日期：2026-10-30
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 Ubuntu 24.04）
- 审计来源：`example/WarningDocement/audit-base-network.md`
- 构建目录：
  - `.build-bento-wsl`：Release，`GO2CPP_BUILD_TESTS=OFF`，`GO2CPP_BUILD_HOOK=OFF`
  - `.build-p1-test-wsl`：Release，`GO2CPP_BUILD_TESTS=ON`，`GO2CPP_BUILD_HOOK=OFF`（本次新建，仅用于跑测试）
- 结论：**P1-1、P1-2 均已修复并验证**；未提交 commit。

---

## 1. P1-1 Fiber 内 Socket 超时/取消/关闭后错误码错误

### 1.1 现象与最小复现

修复前，Fiber 内 `Socket::recv(40ms)` 对无数据连接的错误码：

| 路径 | 修复前 | 期望 |
|---|---|---|
| 普通线程 recv(40ms) | `110 (ETIMEDOUT)` | 110 |
| Fiber 首次 recv(40ms) | **`0`** | 110 |
| Fiber 先 `wait_for(40ms)` 再 recv(40ms) | **`11 (EAGAIN)`** | 110 |

三轮复现结果一致（`0 / 11`），与审计记录的 `0/11/11` 吻合，且比审计时更稳定（审计为间歇）。

### 1.2 根因

`Socket::wait_or_retry` 用 `errno` 作为跨函数错误通道：

```cpp
const int error = wait_ready(fd, event, left, cancel_generation);
if (error != 0) { errno = error; return false; }
// recv_impl:
auto left = timeout;
if (!wait_or_retry(...)) return {-1, errno};   // 依赖 errno 传回
```

而 `wait_ready` 在 IOManager 分支里会 `manager->wait_for()` **park 当前 Fiber**。park 恢复后，Fiber 可能已经被调度到**另一个 OS 线程**；`errno` 是 OS 线程 TLS，此时 `errno = error` 写入的是新线程的副本。

更关键的是：`errno` 展开为 `(*__errno_location())`，GCC 依据 `__errno_location` 的 `const` 属性，把**指针缓存在寄存器里跨调用复用**。反汇编证据（修复前 `libgo2cpp_network.so` 的 `Socket::recv_impl`）：

```text
27ae8: call __errno_location@plt
27aef: mov  %rax,%r13            ; 缓存 park 前线程的 errno 指针
27b19: call Socket::wait_or_retry(...)   ; 内部可能 park 并迁移线程
27b1e: mov  0x0(%r13),%edx       ; 用旧指针读 errno —— 读到旧线程的 0/11
```

于是真实错误 `ETIMEDOUT(110)` 被写进新线程的 errno，调用方却从旧线程的 errno 副本里读到 `0` 或上一次 `recvmsg` 留下的 `EAGAIN(11)`。普通线程不 park、不迁移，所以路径正常。

`wait_ready` 的 IOManager 分支还有两处不足：非 timeout 错误分支 `return result.system_error ? ... : ECANCELED;` 不写 errno；关闭状态检查排在 timeout 判定之后。

### 1.3 修复

只改 `src/network/socket.cpp`：

1. **错误码全程走显式返回值**，不再用 errno 跨 park 传递：

```cpp
auto left = remaining(timeout, start);
if (left.count() == 0 && left != Duration::max()) return {-1, ETIMEDOUT};
const int wait_error = wait_ready(fd, io::IOEvent::kRead, left, cancel_generation);
if (wait_error != 0) return {-1, wait_error};
```

`send_impl` 同样处理；`recv_impl`/`send_impl` 不再调用 `wait_or_retry`（该私有函数保留，避免改动私有头文件）。

2. **park 返回后重新判定状态并映射 errno**，顺序改为：取消 → 就绪 → 关闭 → `kClosed`(EBADF) → 其他错误 → 超时；每个返回点都在当前线程重写 errno：

```cpp
if (cancelled()) { errno = ECANCELED; return ECANCELED; }
if (result.ready()) return 0;
if (is_closing()) { errno = ECANCELED; return ECANCELED; }
if (result.status == io::WaitStatus::kClosed) { errno = EBADF; return EBADF; }
if (result.status != io::WaitStatus::kTimeout) {
    const int error = result.system_error != 0 ? result.system_error : ECANCELED;
    errno = error;
    return error;
}
```

3. waiter 分支与普通线程 poll 分支补写 errno（`if (result != 0) errno = result;`）。

### 1.4 测试

`tests/network/test_network.cpp`：

- **新增 Fiber 超时/取消断言**（`run_network_tests` 内，`manager.Shutdown()` 之后）：
  - 默认多 worker 的 `IOManager`（强制覆盖 Fiber 迁移路径），连续 8 轮 `recv(40ms)`，每轮断言 `error == ETIMEDOUT`；
  - Fiber 内无限期 `recv` + 另一个 Fiber `cancel_read()`，断言 `error == ECANCELED`；
  - 全部带 watchdog（3s）防挂起。
- **收紧原 532 行过宽断言**：失败结果必须带明确 errno，`error == 0` 直接计为 unexpected：

```cpp
if (result.error == 0 ||
    (result.error != ECANCELED && result.error != EBADF &&
     result.error != ETIMEDOUT)) {
    unexpected_errors.fetch_add(1, std::memory_order_relaxed);
}
```

**回归有效性验证**：临时把 `recv_impl` 恢复为旧的 `wait_or_retry + errno` 实现后重跑，新测试出现 **9 次失败**（8 轮超时断言 + 1 次取消断言）；还原修复后全过。证明新断言确实覆盖该缺陷，不是空跑。

---

## 2. P1-2 HOOK=OFF 构建 network 库未解析符号

### 2.1 现象

`nm -uC .build-bento-wsl/libgo2cpp_network.so` 修复前包含：

```text
U go2cpp::hook::mark_system_nonblocking(int)
```

`GO2CPP_BUILD_HOOK=OFF` 时不存在 `go2cpp_hook` 目标，只链接 network/io 的程序链接失败。

### 2.2 根因

`src/network/socket.cpp` 的 `Socket::accept` 无条件调用 `go2cpp::hook::mark_system_nonblocking(accepted)`；`cmake/Go2CppModules.cmake` 的 `go2cpp_network` 链接列表不含 hook，且 `go2cpp_hook` 仅在 `GO2CPP_BUILD_HOOK=ON` 时创建。

### 2.3 修复

`cmake/Go2CppModules.cmake`（`go2cpp_network` 链接语句之后）：

```cmake
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_network PRIVATE go2cpp_hook)
    target_compile_definitions(go2cpp_network PRIVATE GO2CPP_NETWORK_HOOK_ENABLED=1)
else()
    target_compile_definitions(go2cpp_network PRIVATE GO2CPP_NETWORK_HOOK_ENABLED=0)
endif()
```

`src/network/socket.cpp`：

```cpp
#if defined(GO2CPP_NETWORK_HOOK_ENABLED) && GO2CPP_NETWORK_HOOK_ENABLED
            go2cpp::hook::mark_system_nonblocking(accepted);
#endif
```

- HOOK=OFF：调用完全消失，库内无 hook 符号；
- HOOK=ON：hook 作为 PRIVATE 依赖接入（共享库记录 `DT_NEEDED`），调用保留，行为不变。

---

## 3. 验证结果

| 项目 | 命令/方式 | 结果 |
|---|---|---|
| HOOK=OFF 构建 | `cmake --build .build-bento-wsl -j 12` | 通过 |
| P1-2 未解析符号 | `nm -uC libgo2cpp_network.so \| grep -i hook` | 无输出（NO_HOOK_SYMBOL） |
| 去 stub 链接 | 最小程序（不含 hook 空实现）只链 network/io/scheduler/... | 链接成功、运行正常 |
| HOOK=ON 回归 | 新建 `.build-p1-hookon-wsl`（HOOK=ON）构建 network | 通过；引用 hook 且 `DT_NEEDED` 含 `libgo2cpp_hook.so` |
| 最小复现（修复后） | `/tmp/network_p1_fix_repro` 3 轮 | Fiber 首次/第二次均 `110`，普通线程 `110` |
| 网络测试 | `GO2CPP_TEST_FILTER=network ./go2cpp_tests` | all checks passed（含新增 8 轮超时 + 取消 + 收紧断言） |
| IO 测试 | `GO2CPP_TEST_FILTER=io ./go2cpp_tests` | all checks passed |
| FdManager 测试 | `GO2CPP_TEST_FILTER=fd_manager ./go2cpp_tests` | all checks passed |
| SSE 测试 | `GO2CPP_TEST_FILTER=sse ./go2cpp_tests` | all checks passed |
| 回归有效性 | 临时恢复旧 `recv_impl` 后跑 network | 9 次断言失败（预期），还原后全过 |

说明：`go2cpp_tests` 全量运行（不传 filter）时 `test_spin_mutex.cpp` 存在与本任务无关的失败（其他 agent 并发改动/环境），本次只对 network/io/fd_manager/sse 相关模块做验收。

---

## 4. 改动文件（未 commit）

- `src/network/socket.cpp`
  - `Socket::accept` 的 hook 调用加编译期宏保护；
  - `wait_ready`：park 返回后重查取消/关闭/超时、补 errno、映射 `kClosed → EBADF`；
  - `send_impl` / `recv_impl`：等待错误改为显式返回值，不再读 `errno`。
- `cmake/Go2CppModules.cmake`
  - `go2cpp_network` 在 HOOK=ON 时 PRIVATE 链接 `go2cpp_hook`，并定义 `GO2CPP_NETWORK_HOOK_ENABLED`。
- `tests/network/test_network.cpp`
  - 新增 Fiber 超时（ETIMEDOUT）/取消（ECANCELED）断言；
  - 收紧 close/recv 竞态断言，禁止 `error == 0`。

---

## 5. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp
# P1-2: HOOK=OFF 库不应再有 hook 未解析符号
nm -uC .build-bento-wsl/libgo2cpp_network.so | grep -i hook   # 期望无输出
# 只链 network/io 的最小程序应能链接（不含 hook stub）
g++ -std=c++20 -O2 -pthread -Iinclude /tmp/network_p1_fix_repro.cpp \
  -o /tmp/network_p1_fix_repro -L.build-bento-wsl \
  -Wl,-rpath,$PWD/.build-bento-wsl \
  -lgo2cpp_network -lgo2cpp_io -lgo2cpp_context -lgo2cpp_scheduler \
  -lgo2cpp_fiber -lgo2cpp_log -lgo2cpp_error -lgo2cpp_sync
/tmp/network_p1_fix_repro      # 三条路径均应为 error=110
# P1-1: 网络测试（含新增断言）
cd .build-p1-test-wsl && GO2CPP_TEST_FILTER=network ./go2cpp_tests
```
