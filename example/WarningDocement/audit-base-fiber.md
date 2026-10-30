# fiber 基础检查报告（压测 / 随机 / 5 场景 / 解耦 / 测试复查）

- 日期：2026-10-30
- 审查对象：`include/go2cpp/concurrency/fiber.hpp`（510 行）+
  `src/concurrency/fiber.cpp`（3,428 行）+ `tests/concurrency/test_fiber*.cpp`
  （含 growable stack 专项）
- 构建：`.build-bento-wsl`（Ninja，Release，`-O3 -DNDEBUG`；
  `GO2CPP_BUILD_TESTS=OFF`）。审计另建 `.build-fiber-audit`（Release，
  `GO2CPP_BUILD_TESTS=ON`）只用于跑官方 `go2cpp_tests`。
- 审计程序：`/tmp/fiber_audit/audit_fiber.cpp`（独立程序，不注册 CTest）、
  `run_existing_tests.cpp`（复用现有测试模块）、`asan_random.cpp`（ASan 源码插桩）。
- 环境：WSL2 Ubuntu 24.04，内核 6.6.87.2-microsoft-standard-WSL2，
  AMD Ryzen 9 8945HX（12 vCPU / 19 GiB），GCC 13.3.0。
- 边界：**只读审计**；未修改实现、未修改测试、未 commit。
- 结论：**实现无 P0/P1 缺陷**；压测、随机、5 场景全部通过，解耦达标。
  **测试侧有 2 个 P1**：`test_fiber.cpp` 的 FIBER-03 用例存在
  heap-use-after-free（当前 HEAD 上 `GO2CPP_TEST_FILTER=fiber` 必崩），
  以及 `go2cpp_fiber_growable_stack` 的 CTest PASS 正则可吞掉断言失败。

---

## 1. 压测

条件：单线程 + 无调度器（直接 `Fiber::resume()`），WSL2；宿主有其它负载，
绝对值有约 ±5–10% 噪声。计时用 `steady_clock`，每次 `resume()` 前后取样。

### 1.1 10 万 Fiber 完整生命周期（创建 → 首次 resume → Completed → 析构）

| 指标 | 数值 | 说明 |
|---|---:|---|
| 总耗时 | **16.80 ms** | 10 万次，n=100000 |
| 每次操作 | **168.0 ns/op** | 创建 + 首切 + 完成 + 析构 |
| 失败数 | 0 | `resume()` 全为 true，状态全为 Completed |
| RSS 增量 | +128 KiB | 栈缓存复用，几乎不增长 |

条件：`Fiber(fn, 4096)`，默认 8 MiB 预留 + growable。该数字是**栈缓存命中**
路径（线程本地 32 条 + 全局 512 条的 `(usable, reserve)` 键缓存），不是
每次 `mmap`。

冷路径（40 种不同预留量轮换，使 32 条线程缓存持续失效，n=2000）：
**447.3 ns/op**，RSS +512 KiB，VmSize +278 MiB（40 种键的栈驻留缓存）。

对照官方文档 `docs/switch-benchmark.md` 的 ~500 ns（含调度器 park/wake）
与 ~100–200 ns 纯 resume/suspend 估算，本机纯 resume 路径同一量级。

### 1.2 切换延迟（2 个 Fiber ping-pong，2 万次 resume）

| 指标 | 数值 |
|---|---:|
| mean | **82.1 ns** |
| P50 | **76 ns** |
| P95 | 87 ns |
| P99 | 130 ns |
| min | 65 ns |
| max | 42,905 ns（单次离群，WSL2 调度中断） |
| 失败数 | 0 |

每样本是**一次 `resume()` 往返**（main → fiber → Suspend → main），
即一次完整上下文切换。P99/P50 = 1.71×，离群值来自宿主调度，不代表
切换成本。

### 1.3 10 万 Fiber 同时存活（8 MiB 预留，全部挂在 Suspend）

| 阶段 | 数值 |
|---|---:|
| 创建（含首切） | **13.56 µs/fiber**，100,000 个共 1,356 ms |
| 存活 RSS | **460.5 MiB**（≈4.6 KiB/fiber，仅初始 1 页栈） |
| 存活 VmSize | **819.3 GiB**（≈8.39 MiB/fiber，`MAP_NORESERVE` 预留） |
| 批量 resume 全部完成 | **11.58 µs/fiber**，共 1,158 ms |
| 批量 resume 失败 | 0 |
| 全部析构 | 16.4 ms |
| 释放后 RSS | 63.6 MiB |
| 释放后 VmSize | **4.52 GiB**（= 栈缓存 544 条 × 8 MiB 的驻留映射） |

结论：
- 10 万并发 Fiber 在本机可行（`vm.max_map_count=1,048,576`，
  `overcommit_memory=0`，无虚拟内存限制）；
- **VmSize 与并发数近似线性（8 MiB/fiber）**，这是 growable 栈的固定成本；
  8,000 连接场景的 62.8 GB 数据与此一致（`docs/modules/fiber.md` §11）；
- 全部退出后 VmSize 仍保留约 **4.5 GiB** 的栈缓存映射（全局 512 + 线程 32），
  这是可观测的驻留成本，不是泄漏；缓存满后 `munmap`。
- 峰值 RSS 由已提交栈页决定，10 万 fiber 只占 460 MiB（每 fiber 一页）。

### 1.4 栈增长

| 场景 | 结果 |
|---|---|
| 1 万次 fiber，每次递归 1024 帧（约 284 KiB 实际栈） | **31.19 µs/op**，总 311.9 ms，RSS +256 KiB |
| 2,000 个 fiber 同时存活，每个 128 帧（约 40 KiB） | 峰值 RSS 85.6 MiB；完成后 27.5 MiB；VmSize 16.4 GiB → 4.47 GiB（缓存）；批量 resume 2,000 个共 29.3 ms（14.7 µs/fiber，含栈回收） |
| 深栈 8 MiB（30,174 帧 × 278 B） | **8.37 ms**；`strace` 统计 **138 次 `mprotect`**（≈133 次 64 KiB 主增长 + 校准 1,000 帧的约 4 次 + 初始提交/回收复原各 1 次） |
| 增长后返回外层帧 | 局部变量地址与值不变（`address_stable=1`，`marker_ok=1`） |

单次深栈场景在 `strace` 下从 8.4 ms 变为 59.9 ms，说明增长成本主要来自
信号 + `mprotect`，不是上下文切换。

---

## 2. 随机测试

### 2.1 非插桩随机（5000 次操作，seed=12345）

操作分布：创建 28%、resume 34%、取消 10%、跨线程 resume 16%、销毁 12%。
每个 Fiber 的主体随机 1–6 步，每步可能深递归 0–2000 帧（固定栈按栈大小
限制深度）并 `Suspend(Yield)`。

```
[random] seed=12345 ops=5000 created=684 resumed=1667 cancelled=491
         destroyed=684 cross_thread=181 bodies_completed=217
         bodies_cancelled=190 failures=0 rss_kb=14196
```

- 0 失败，0 崩溃；181 次跨线程 resume 全部成功（父级均为 main 上下文）。
- `Fiber::DestroyTimeoutCount()` 恒为 0（没有触发隔离区路径）。
- 随机栈参数覆盖 `stack_size ∈ {1, 2048, 4096, 16384, 65536}`、
  `reserve ∈ {0, 1 MiB, 8 MiB}`、growable 开/关。

### 2.2 ASan + UBSan 源码插桩随机（4 个 seed，共 9,000 次操作）

`-fsanitize=address,undefined -fno-omit-frame-pointer`，`fiber.cpp` /
`fiber_local.cpp` / `context_backend.cpp` / `context_x64.S` 全部源码插桩
（`-DGO2CPP_USE_NATIVE_CONTEXT=1`）。ASan 下实现按设计强制固定栈。

| seed | ops | completed | cancelled | DestroyTimeout | ASan/UBSan 报告 |
|---|---:|---:|---:|---:|---|
| 999 | 3000 | 117 | 74 | 0 | 无 |
| 7 | 2000 | 78 | 55 | 0 | 无 |
| 42 | 2000 | 72 | 54 | 0 | 无 |
| 20261030 | 2000 | 72 | 70 | 0 | 无 |

结论：随机路径**无 UAF / 无双重释放 / 无未定义行为**；库侧生命周期
（创建、挂起、取消、析构、跨线程恢复）与 ASan 的栈切换钩子配合正常。

---

## 3. 真实场景（5 个，覆盖全部待验证功能）

| # | 模拟的真实用法 | 输入 | 期望 | 实际 |
|---|---|---|---|---|
| 1 | 深递归任务需要 8 MiB 栈（解析/遍历） | `Fiber(fn, 64 KiB, 32 MiB, growable)`；递归 30,174 帧（278 B/帧） | 不崩、结果正确、栈地址稳定 | **通过**：used=8,699,904 B ≥ 8 MiB；checksum 全对；`address_stable=1`；`has_stack=1`、`current_sp≠0`；8.37 ms |
| 2 | 变长栈反复增长/收缩，同一 Fiber 复用 | 同一 Fiber 内深度 200→2,000→100→6,000→300→12,000→4,000 帧 | committed 只向下扩展、外层地址不变、checksum 正确；随后新 Fiber 复用缓存栈从初始 32 KiB 开始 | **通过**：`monotonic=1`，`address_stable=1`，7 个深度 checksum 全对，峰值 3.51 MiB；复用栈 `initial_usable=32768`，resume 5.12 µs |
| 3 | Fiber 间 channel 通信（生产者/消费者，有缓冲 + 无缓冲） | 有缓冲 64：20,000 个 int；无缓冲：2,000 次交接 | 顺序、总和正确；无死锁 | **通过**：received=20000、sum 正确、`order_ok=1`；无缓冲 handoff=2000；4 个 worker，`threads` 回落到 1 |
| 4 | 协作式取消：父取消传播到子 Fiber；析构取消挂起 Fiber | 父 Fiber 内 `RequestCancellation()` 后子 Fiber 首次 resume；另建挂起 Fiber 后析构 | 子感知取消并正常退出（不崩、不泄漏）；析构在预算内完成；Completed 后 resume 返回 false | **通过**：`child_cancelled=1`、`child_rounds=0`、`child_completed=1`；析构 2.4 µs；`second_resume=0` |
| 5 | Scheduler 启动/停止（3 轮），混合普通任务与阻塞在 channel 的任务 | 4 worker；每轮 200 个普通任务 + 20 个阻塞任务 + 20 次 Send + Close | 全部任务收尾、线程回落、无残留 | **通过**：3 轮 `done=200 blocked_done=20 failures=0`；`threads` 6→1；RSS 稳定 |

补充：`example/concurrency/fiber_stack_limit_full`（官方示例，6 个场景：
探深、递归改迭代、换 Fiber 续跑、显式限深、JSON 深层嵌套边界/随机/攻击载荷）
实测**全部通过**（`STACK_LIMIT_EXIT=0`）。

---

## 4. 解耦检查（只依赖 base）

### 4.1 公共头自包含

```
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -I include -fsyntax-only
  #include "go2cpp/concurrency/fiber.hpp"  → 通过
  #include "go2cpp/fiber.hpp"（4 行转发头）→ 通过
```

`concurrency/fiber.hpp` 只 include 8 个标准库头：
`<cstddef> <cstdint> <exception> <functional> <memory> <string> <thread> <vector>`；
不含任何项目内头、无 `using namespace`。

### 4.2 动态依赖（实测）

```
readelf -d .build-bento-wsl/libgo2cpp_fiber.so.0.1.0 | grep NEEDED
  libstdc++.so.6, libgcc_s.so.1, libc.so.6, ld-linux-x86-64.so.2
nm -D --undefined-only libgo2cpp_fiber.so.0.1.0 | grep go2cpp   → 无输出
```

**fiber 库不依赖任何其它 go2cpp 模块**（没有 `libgo2cpp_*.so` 的 DT_NEEDED，
也没有项目内未定义符号）。`libgo2cpp_fiber` 自身由 `fiber.cpp` +
`fiber_local.cpp` + `context_backend.cpp` + `context_x64.S` 组成。

### 4.3 最小使用验证

`minimal.cpp` 只 include `go2cpp/concurrency/fiber.hpp`，只链接
`-lgo2cpp_fiber`（不链 scheduler/channel/context/error），运行：

```
[minimal] ok stack_size=4096 destroy_timeout=0
```

### 4.4 源码依赖方向

- `fiber.cpp` 的项目内 include 只有：`go2cpp/fiber.hpp`（转发）、
  `go2cpp/debug/debug_resources.hpp`（`GO2CPP_DEBUG_DUMP=ON` 时才调用）、
  `go2cpp/detail/context_backend.hpp`、`go2cpp/fiber_local.hpp`；
  **没有** scheduler / channel / io / network 头。
- 反向依赖（上层依赖 fiber，方向正确）：`channel.hpp`、`runtime.hpp`、
  `debug/debug_fiber.hpp`、`debug/debug_dump.hpp` 均 include fiber 公共头。
- 编译期可选依赖：`valgrind/valgrind.h`（`__has_include` 探测，普通构建
  宏消除）；非原生后端才 `#include <boost/context/...>`，本机
  `GO2CPP_NATIVE_CONTEXT=ON` 不启用。

**结论：解耦达标。** 唯一可讨论点是 `fiber.hpp` 依赖 `<thread>`（用于
`std::thread::id`）与 `<functional>`（用于 `std::function`），属于标准库，
不构成模块耦合。

---

## 5. 测试复查

### 5.1 断言有效性

- `GO2CPP_CHECK`：失败累加 `g_failures` 并打印 `file:line`，不中断；
  `GO2CPP_REQUIRE`：失败直接 `abort`（`watchdog_abort`）；
  `GO2CPP_JOIN_WITH_WATCHDOG`：独立 watchdog 线程超时 abort。宏本身有效，
  不存在 NDEBUG 下变空操作的问题（`test_support.hpp` 无 `#ifdef NDEBUG`）。
- `test_main.cpp` 对未知 `GO2CPP_TEST_FILTER` 返回非零（T-08 已修），
  实测 `GO2CPP_TEST_FILTER=nonexistent_fiber` 打印「未匹配任何测试模块」
  并失败，不会把空跑伪装成通过。
- 断言密度：`test_fiber.cpp` 143 处、`test_fiber_growable_stack.cpp` 39 处、
  `test_fiber_local.cpp` 20 处；`test_fiber_select.cpp` 用自有 `Check`（5 项）。

### 5.2 P1-1：`test_fiber.cpp` FIBER-03 存在 heap-use-after-free（当前 HEAD 必崩）

**证据链：**

1. 手工 runner（`test_fiber.cpp` + 库）运行 `GO2CPP_TEST_FILTER=fiber`：
   **8/8 次失败**（7 次 SIGSEGV + 1 次 abort）。
2. 官方 `.build-fiber-audit/go2cpp_tests`（Release，`GO2CPP_BUILD_TESTS=ON`，
   同一 HEAD）运行 `GO2CPP_TEST_FILTER=fiber`：**SIGSEGV**；
   gdb 回溯（两次运行，崩溃点随 observer 执行到的 accessor 漂移）：
   ```
   #0 go2cpp::Fiber::debug_stack() const            ← 第一次
   #0 go2cpp::Fiber::execution_binding() const      ← 第二次
   #1 std::thread::_State_impl<... run_fiber_tests()::{lambda()#36}>
   ```
   两次都在 observer 线程（lambda#36）内，指向同一段裸指针观察代码。
3. ASan（测试代码插桩）报告：
   ```
   ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8 thread T15
     #2 go2cpp::Fiber::debug_stack() const
     #3 run_fiber_tests() test_fiber.cpp:546
   freed by thread T15:
     #1 go2cpp::Fiber::debug_info() const
   ```
   即 observer 线程在 `debug_info()`/`debug_stack()` 之间访问了已释放的
   `FiberRecord`（216 字节）。
4. valgrind 下不崩（串行化时序），但暴露另一个断言失败：
   `test_fiber.cpp:566: check failed: observer_reads.load(...) > 0`。

**根因（测试代码，非实现）：**
FIBER-03 块（`test_fiber.cpp` 约 530–565 行）用 `published` 裸 `Fiber*`
让 observer 线程并发读，主线程每轮：
`AcquireForScheduler` → `published.store(fiber.get())` → `resume` →
`RecycleForScheduler(std::move(fiber), 8U)`。

`RecycleForScheduler` 的契约（`fiber.hpp:307-310`、`fiber.cpp:1966-1992`）是
**条件不满足时静默丢弃，由 `unique_ptr` 正常析构**。丢弃条件包含
`m_record.use_count() != 1`：observer 正在 `debug_info()`/`debug_stack()`
里复制 record 的 `shared_ptr` 时，主线程的 Recycle 会看到 use_count>1 →
丢弃 → `Fiber` 对象与 `FiberRecord` 立即析构；observer 手里的裸指针随即
悬空 → UAF。测试没有对 `published` 做生命周期同步，也没有等待 observer
读完再 Recycle。

**影响：** `go2cpp_tests` 的全量 `go2cpp_tests` 用例（无 PASS 正则，退出码
生效）当前必然是红的；fiber 模块无法在 HEAD 上通过。

**建议（未改代码）：** observer 改为持有 `std::shared_ptr<Fiber>`（或
`weak_ptr` + 短临界区），并在 `RecycleForScheduler` 前与主线程用
mutex/atomic 握手；或把 `bin_capacity` 设为 0（不回收）并接受栈缓存开销；
或在该回归用例中显式保证 use_count==1。

### 5.3 P1-2：`go2cpp_fiber_growable_stack` 的 PASS 正则可以吞掉断言失败

`cmake/Go2CppTests.cmake:304-308`：

```cmake
add_test(NAME go2cpp_fiber_growable_stack COMMAND go2cpp_tests)
set_tests_properties(go2cpp_fiber_growable_stack PROPERTIES
    ENVIRONMENT "GO2CPP_TEST_FILTER=fiber_growable"
    PASS_REGULAR_EXPRESSION "fiber growable stack"
    TIMEOUT 120)
```

`run_fiber_growable_stack_tests()` 的第一行就是
`go2cpp_tests::announce("fiber growable stack: reserve + 2KiB initial commit + SIGSEGV growth")`，
该字符串**在任何断言之前**输出。CTest 的 `PASS_REGULAR_EXPRESSION` 语义是
「输出匹配即通过，忽略进程退出码」。已用最小 CMake 工程实测：

```
add_test(NAME fake_pass COMMAND sh -c "echo fiber growable stack announce; exit 1")
set_tests_properties(fake_pass PROPERTIES PASS_REGULAR_EXPRESSION "fiber growable stack")
→ 1/2 Test #1: fake_pass ... Passed（exit 1 被忽略）
```

因此 growable 测试里任何 `GO2CPP_CHECK` 失败（`g_failures>0`、main 返回非零）
都不会让这条 CTest 变红。`go2cpp_tests` 全量用例没有 PASS 正则，不受影响，
但它当前会被 P1-1 的崩溃打断。

**建议：** 去掉 `PASS_REGULAR_EXPRESSION`（让退出码生效），或改为
`FAIL_REGULAR_EXPRESSION "check failed|watchdog|terminate"`。

### 5.4 P2-3：`observer_reads > 0` 断言无同步，快机器上会假失败

FIBER-03 在 200 轮循环后直接 `observer_stop=true`，没有等待 observer
至少读到一次。valgrind 下实测 observer 线程 0 次读取 →
`check failed: observer_reads.load(...) > 0`。它不是实现问题，而是测试
对调度时序的隐含假设。

### 5.5 覆盖缺口（未测或弱覆盖）

| 缺口 | 说明 |
|---|---|
| growable 失败路径 | >256 KiB 大帧、越界写、handler 重入三种 fatal 路径无自动测试（都会 core，只能手工验证） |
| `debug_stack` 正向字段 | 现有断言只测「非当前 Fiber 时 has_stack=false / 地址为 0」；`has_stack=true`、`current_sp != 0`、`mapping_base/top` 非 0 没有单测（本次审计程序补测通过） |
| `resume_result().context_snapshot` | 只断言 `size() >= 3` 与 `failure != nullptr`，未校验父链 id/depth 内容 |
| 栈缓存上限 | 32/512 条上限、缓存满后的 `munmap`、`reset_for_scheduler` 抛 `logic_error` 路径无测试 |
| 析构超时隔离区 | FIBER-02 只测 `DestroyTimeoutCount` 递增与 runner 返回；隔离区 Impl 的生命周期/内存不回收属于已知设计取舍，未测泄漏 |
| 跨进程迁移 | 不在主测试套件；`tools/migration/tests/run_tests.sh` 单独跑（本次未执行） |
| `test_fiber_select.cpp` | 已注册为独立 ctest `go2cpp_fiber_select_test`（5 项），不在 `go2cpp_tests` 内；本次单独运行通过 |

### 5.6 SKIP / 逃逸检查

- `test_fiber_growable_stack.cpp:270-271`：非 Linux/x86_64 打印
  `[跳过] growable stack 需要 Linux/x86_64 原生上下文后端`，不增加失败计数。
  当前平台该分支不生效（编译期 `#if defined(__linux__) && defined(__x86_64__)`），
  属于合理降级，不是逃逸。
- 全仓 `tests/concurrency/test_fiber*.cpp` 无运行时 `return` 提前退出、
  无 `#ifdef` 包住断言、无 `GTEST_SKIP` 类机制。
- 唯一实际逃逸是 5.3 的 PASS 正则。

### 5.7 构建/注册事实

- `.build-bento-wsl`：`GO2CPP_BUILD_TESTS=OFF`，`ctest -N` 共 50 条，
  **fiber 相关 0 条**。在该构建下 fiber 测试既没编译也没运行；
  要跑 fiber 测试必须另建 `GO2CPP_BUILD_TESTS=ON` 的构建（本次
  `.build-fiber-audit`）。
- `.build-fiber-audit` 中 fiber 相关 ctest：`go2cpp_fiber_growable_stack`、
  `go2cpp_fiber_select_test`、`go2cpp_fiber_local_demo/full`、
  `go2cpp_fiber_stack_limit_demo/full`、`go2cpp_fiber_transport_test`（未构建）。
  `go2cpp_fiber_growable_stack` 实测 Passed（0.02 s）；
  `go2cpp_fiber_select_test` 5/5 通过；`go2cpp_fiber_stack_limit_full` 6/6 通过。
- `go2cpp_tests` 全量用例当前会被 P1-1 的 fiber 崩溃打断（本次未跑完整
  全量套件，只跑 `GO2CPP_TEST_FILTER=fiber` 复现）。

---

## 6. 结论与建议

**实现（fiber.hpp / fiber.cpp）：**
- 压测：10 万生命周期 168 ns/op、切换 P50 76 ns / P99 130 ns、
  10 万并发 460 MiB RSS / 819 GiB VmSize、8 MiB 深栈 8.4 ms；
- 随机：9000 次 ASan/UBSan 插桩 + 5000 次非插桩，零失败、零 UAF；
- 5 场景全部通过，覆盖深栈、变长增长、Fiber 间 channel、取消、shutdown；
- 解耦：头文件自包含，库只依赖 libc/libstdc++，最小程序只链
  `-lgo2cpp_fiber` 可运行。
- **未发现需要修改实现的缺陷。**

**测试侧（需要在后续任务处理，本次按边界未改）：**
- P1-1：`test_fiber.cpp` FIBER-03 的 observer 裸指针与 Recycle 丢弃竞争，
  ASan 实测 heap-use-after-free；官方 `go2cpp_tests` 在
  `GO2CPP_TEST_FILTER=fiber` 下 100% 段错误。
- P1-2：`go2cpp_fiber_growable_stack` 的 PASS 正则在断言前输出，CTest
  会忽略退出码，断言失败无法让用例变红。
- P2-3：`observer_reads > 0` 无同步，快机器/valgrind 下假失败。
- 覆盖缺口见 5.5（growable fatal 路径、debug_stack 正向字段、栈缓存上限等）。

**已知限制（非缺陷）：**
- 栈缓存保留 544 × 8 MiB 虚拟映射（释放后 VmSize 4.5 GiB），是设计取舍；
- growable 与 ASan/TSan 互斥（sanitizer 下强制固定栈）；
- 进程级 SIGSEGV handler 与用户 handler/JIT 冲突，只能
  `GO2CPP_GROWABLE_STACK=0` 规避。

---

## 7. 复现命令

```bash
# 构建（只读复用现有构建）
cd /UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl && ninja go2cpp_fiber

# 审计程序（/tmp/fiber_audit）
g++ -std=c++17 -O2 -DNDEBUG -I include -o /tmp/fiber_audit/audit_fiber \
  /tmp/fiber_audit/audit_fiber.cpp \
  -L .build-bento-wsl -lgo2cpp_channel -lgo2cpp_scheduler -lgo2cpp_context \
  -lgo2cpp_error -lgo2cpp_log -lgo2cpp_fiber -lpthread \
  -Wl,-rpath,$PWD/.build-bento-wsl
/tmp/fiber_audit/audit_fiber stress 100000
/tmp/fiber_audit/audit_fiber churn 2000
/tmp/fiber_audit/audit_fiber scale 100000
/tmp/fiber_audit/audit_fiber deep
/tmp/fiber_audit/audit_fiber random 12345 5000
/tmp/fiber_audit/audit_fiber scenario

# 现有测试（手工 runner）
g++ -std=c++17 -O2 -DNDEBUG -I include -I tests \
  -o /tmp/fiber_audit/run_existing_tests /tmp/fiber_audit/run_existing_tests.cpp \
  tests/concurrency/test_fiber.cpp tests/concurrency/test_fiber_growable_stack.cpp \
  -L .build-bento-wsl -lgo2cpp_channel -lgo2cpp_scheduler -lgo2cpp_context \
  -lgo2cpp_error -lgo2cpp_log -lgo2cpp_fiber -lpthread \
  -Wl,-rpath,$PWD/.build-bento-wsl
/tmp/fiber_audit/run_existing_tests growable   # 通过
/tmp/fiber_audit/run_existing_tests fiber      # 段错误（P1-1）

# 官方测试（需 GO2CPP_BUILD_TESTS=ON）
cmake -S . -B .build-fiber-audit -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_BENCHMARKS=OFF
cd .build-fiber-audit && ninja go2cpp_tests
GO2CPP_TEST_FILTER=fiber ./go2cpp_tests           # 段错误（P1-1）
GO2CPP_TEST_FILTER=fiber_growable ./go2cpp_tests  # 通过
ctest -R go2cpp_fiber_growable_stack             # Passed（正则吞失败，P1-2）

# ASan 随机（源码插桩）
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DGO2CPP_USE_NATIVE_CONTEXT=1 -I include -o /tmp/fiber_audit/asan_random \
  /tmp/fiber_audit/asan_random.cpp src/concurrency/fiber.cpp \
  src/concurrency/fiber_local.cpp src/concurrency/context_backend.cpp \
  src/concurrency/context_x64.S -lpthread
/tmp/fiber_audit/asan_random 999 3000
```

---

## 附录：原始输出摘录

```
[stress.lifecycle] n=100000 ns_per_op=168.029 total_ms=16.8029 failures=0
                   rss_delta_kb=128 checksum=4999950000
[stress.switch] n=20000 mean_ns=82.1153 p50_ns=76 p95_ns=87 p99_ns=130
                min_ns=65 max_ns=42905
[stress.growth] n=10000 ns_per_op=31186.9 total_ms=311.869 failures=0
                rss_delta_kb=256
[stress.live] count=2000 rss_before_kb=4588 rss_live_kb=85612
              rss_after_kb=27512 vm_before_kb=32016 vm_live_kb=16409012
              vm_after_kb=4465076 resume_all_ms=29.3386
[stress.churn] n=2000 ns_per_op=447.272 total_ms=0.894543 failures=0
               rss_delta_kb=512 vm_delta_kb=278016
[stress.scale] attempted=100000 created=100000 ns_per_fiber=13561.6
               create_ms=1356.16 rss_live_kb=460504 vm_live_kb=819264460
               error=none
[stress.scale.release] ms=16.3701 resume_all_ms=1158.4
                       resume_ns_per_fiber=11584 resume_failures=0
                       rss_after_kb=63576 vm_after_kb=4520908
[scenario.deep8m] frames=30174 used_bytes=8699904 per_frame_bytes=278
                  initial=65536 reserve=33554432 address_stable=1 marker_ok=1
                  has_stack=1 current_sp_nonzero=1
[scenario.deep8m.result] ok=1 state=3 checksum=3690982 expected=3690982
                         checksum_ok=1 used_ok=1 total_ms=8.36609 rss_kb=12752
[scenario.variable_growth] ok=1 completed=1 monotonic=1 address_stable=1
                           checksums_ok=1 peak_used_bytes=3514368 marker_ok=1
                           snapshot_has_stack=0 total_checksum=2321256
[scenario.variable_growth.reuse] ok=1 initial_usable=32768
                                  mapping_base=0x772cb5e00000 resume_ms=0.00512
[scenario.channel] producer_ok=1 consumer_ok=1 received=20000
                   sum_ok=1 order_ok=1 handoff=2000 handoff_expected=2000
                   sender_ok=1 receiver_ok=1 threads=1
[scenario.cancel.parent] ok=1 state=3 child_cancelled=1 child_rounds=0
                         child_completed=1
[scenario.cancel.destroy] observed=1 destroy_us=2.404 destroy_timeout_count=0
[scenario.cancel.reuse] second_resume=0 state=3
[scenario.shutdown] failures=0 threads_before=1 threads_after=1 rss_kb=17152
[random] seed=12345 ops=5000 created=684 resumed=1667 cancelled=491
         destroyed=684 cross_thread=181 bodies_completed=217
         bodies_cancelled=190 failures=0 rss_kb=14196
[asan.random] seed=999 ops=3000 completed=117 cancelled=74
              destroy_timeouts=0 failures=0 sink=2952314
```

`strace -f -e trace=mprotect,mmap,munmap -c ./audit_fiber deep`：

```
% time     seconds  usecs/call     calls    errors syscall
 51.26    0.003626          26       138           mprotect
 27.50    0.001945          38        50           mmap
 21.25    0.001503         751         2           munmap
```
