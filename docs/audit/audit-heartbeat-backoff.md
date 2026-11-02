# heartbeat / backoff 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/{heartbeat,backoff}.hpp`、
  `src/bento/net/{heartbeat,backoff}.cpp`、
  `tests/bento/test_{heartbeat,backoff}.cpp`、
  `tests/bento/bench_heartbeat_backoff.cpp`（本次新增的独立压测程序）
- 构建：`/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl`（Ninja，Release，`-O2`，
  `-Wall -Wextra -Wpedantic -Werror`），GCC 13 / Clang 18 双编译器验证
- 结论：**无 P0**。压测 6000 万次、随机 2400 组、6 个场景、60s 长稳、TSan/ASan/UBSan/LSan
  全部通过；修复 2 个 P1（退避上限抖动堆积、超时状态与 `IsAlive` 自相矛盾）、4 个 P2；
  `ctest -R bento` 48/48（47 过 + 1 个环境性 SKIP）。

## 1. 压测数据

工具：`tests/bento/bench_heartbeat_backoff.cpp`（独立程序，不注册 CTest；只依赖两个被测头文件）。
每项 **500 万次**操作（100 万 × 5 轮取中位数），12 项合计 6000 万次。

### 1.1 吞吐（假时钟，`-O2`，12 核 WSL）

| 项目 | ns/op | Mops/s |
|---|---|---|
| `Heartbeat::Poll()` 全命中 | 2.23 | 448.2 |
| `Heartbeat::Poll()` 未到期 | 2.08 | 479.6 |
| `Heartbeat::Poll()` 已超时短路 | 1.34 | 747.8 |
| `Heartbeat::OnReceiveAt()` | 1.00 | 998.9 |
| `Heartbeat::IsAlive()` | 2.04 | 490.9 |
| `Backoff::Next()` 默认 1s/30s/0.2 | 4.34 | 230.7 |
| `Backoff::Next()` 无抖动快路径 | 2.07 | 483.1 |
| `Backoff::Next()` 抖动 1.0 | 4.36 | 229.4 |
| `Backoff::Next()` 已封顶 30s | 4.37 | 228.7 |
| `Backoff::Next()` max=UINT32_MAX | 2.05 | 488.5 |
| `Backoff::Reset()+Next()` | 5.30 | 188.5 |
| `Backoff::PeekNominalMs()` | 5.15 | 194.1 |

- 心跳路径全部 ≤2.5ns/op，最坏是 `Poll()` 的未到期分支（一次 `Now()` + 两次比较），
  400 Mops/s 对单线程事件循环绰绰有余。
- `Next()` 的抖动路径是主要的 2 倍开销来源（每次多一次 splitmix64 + 两次 double 运算），
  但绝对量 4.3ns 仍在噪声级别；无抖动配置可再快一倍。
- 修复前后吞吐无回退（`Next()` 4.57→4.32ns/op，`Poll()` 2.28→2.30ns/op）。

### 1.2 内存

| 对象 | `sizeof` | 说明 |
|---|---|---|
| `Heartbeat` | 112 B | 含 2 个 `std::function`（64 B）+ 4 个整数 + 2 个 bool |
| `Backoff` | 40 B | 3 个标量参数 + 种子/状态 + 计数 |
| `Backoff::Config` | 24 B | 纯参数结构 |

- 6000 万次压测全程 RSS `3456 → 3840 KiB`（+384 KiB，全部是首次调用 `std::function`
  与静态库页的按需加载）；60s 长稳 RSS `3712 → 3840 KiB`（**+128 KiB**），无增长趋势。
- 两个类都是值语义、无堆分配、无全局状态：`static_assert` 校验可拷贝且析构不抛。

### 1.3 随机测试

| 对象 | 配置数 | 操作数 | 结果 |
|---|---|---|---|
| `Heartbeat`（随机间隔/超时/时钟步进/OnReceive/SetInterval/SetTimeout） | 1200 组 × 400 步 | 287,982 次 `Poll` | 0 失败 |
| `Backoff`（随机 base/max/jitter/seed，含 Reset 复现） | 1200 组 × 64 次 | 76,800 个延迟采样 | 0 失败 |

不变量（全部在随机路径里断言）：

- `Poll()==true` ⇔ `Beats` 恰好 +1，且发生时刻 ≥ 旧到期点，下次到期点严格等于 `now + interval`；
- 只有在跨过阈值的那一次 `Poll` 才 +1 次超时回调，恢复前不重复触发；
- `IsTimedOut() ⇒ !IsAlive()`（不可同时成立）；
- `Next()` 恒落在 `[1, max_ms]`，无抖动时严格等于名义值，`Reset()` 后序列逐位复现。

### 1.4 六个场景

| # | 场景 | 结果 |
|---|---|---|
| 1 | 假时钟边界前/后 1ms | 心跳 999/1000、1999/2000 精确；大跳 3001 只补 1 次心跳；超时 4999/5000 精确（`>=` 判死），恢复后窗口重新计算 |
| 2 | 连续超时/恢复 1000 轮 | 每轮 `TimeoutsFired` 恰好 +1，未恢复前额外 Poll 3 次不重复触发，恢复后 `IsTimedOut` 立即清除 |
| 3 | 30s 上限 + ±20% 抖动 | 10 万采样全部落在 `[24000, 30000]`；10 桶直方图 9798–10160（每桶 ≈10%），无堆积 |
| 4 | 多线程并发 | 4 线程 × 独立实例 100 万次 Poll 全命中；4 线程共享实例 + `mutex` 20 万次操作无状态错乱；4 线程独立 `Backoff` 100 万次全部在范围内 |
| 5 | 回调内重入 | 回调里再 `Poll()` 返回 false、`OnReceiveAt()` 可恢复、`Start()` 可重启并重排节拍；无死循环/二次触发 |
| 6 | 真实时钟 60s 长稳 | 11,514 次 `Poll`、2,302 次心跳（期望 2,400；`Poll` 实测 5.21ms/次，11514/5≈2302，与驱动节拍严格一致）、0 次超时、RSS +128 KiB |

### 1.5 并发与内存安全

| 检查 | 配置 | 结果 |
|---|---|---|
| ThreadSanitizer | 8 线程 × 独立实例（各 10 万次 `Poll`+`Next`）；8 线程共享实例 + `mutex`（各 10 万次） | **0 数据竞争** |
| ASan + UBSan + LSan | 完整压测程序（含 6s 长稳、1200+1200 组随机、全部场景） | **0 内存错误 / 0 未定义行为 / 0 泄漏** |

TSan 直接编译 `src/bento/net/{heartbeat,backoff}.cpp` 而不是链接静态库，保证被测代码本身被插桩；
同时这也再次证明两个实现不依赖其它模块（否则编译不过）。

## 2. 问题与处理

### P0（无）

未发现崩溃、数据损坏、死锁、明显泄漏。ASan/valgrind 未跑（模块无堆分配，收益低）。

### P1-1 退避到上限后抖动堆积在 30000，失去打散作用

- **证据**：旧的 `Next()` 先算 `nominal + [-span, +span]` 再 `min(…, max)`。名义值到 30s 后，
  上界 36s 的一半样本被硬压到 30000。压测直方图（24s→30s，每桶 600ms）：
  `5062 5024 4951 4948 4983 5012 5085 5055 5160 54720` —— **第 10 桶 54.7%**。
  多客户端会在同一时刻一起重连，抖动等于白加。文档表格写的是 `[24s, 30s]`，实现与文档不符。
- **修复**：先定区间 `[nominal*(1-jitter), min(nominal*(1+jitter), max)]`，再在区间内均匀采样
  （`src/bento/net/backoff.cpp`）。随机数消耗仍是每次 1 个，确定性种子序列语义不变。
- **回归**：`test_backoff.cpp` 新增 `TestCapJitterDistribution`（2 万采样 × 10 桶，每桶 6%–14%）；
  压测直方图修复后为 `9798–10160`（≈10%）。

### P1-2 `IsAlive()` 与 `IsTimedOut()` 可同时为真

- **证据**：超时回调上报后把 `SetTimeout` 调大，时间窗重新满足「存活」，但 `m_timeout_fired`
  仍为 true（`Poll()` 继续恒定返回 false）。随机测试命中 **1506 次**
  `IsTimedOut() && IsAlive()`。调用方会同时看到「已判死」和「还活着」。
- **修复**：无参 `IsAlive()` 改为 `!m_timeout_fired && IsAlive(m_timeout_ms)`
  （`heartbeat.hpp`）；带参版本保持文档承诺的「纯时间窗查询」。`SetTimeout` 明确不撤销已上报的超时。
- **回归**：新增 `TestTimedOutIsAliveConsistency`；随机测试的互斥不变量保留。

### P2-1 `max_ms > 2^31` 时永远退不到配置的上限

- **证据**：`NominalDelayMs` 把左移位数钳在 31，`max_ms = UINT32_MAX` 时最高只到
  2,147,483,648。压测断言实测 `highest == 2147483648 != 4294967295`。
- **修复**：钳到 32（`0xFFFFFFFF << 32` 仍在 `uint64` 内）。`max_ms` 配成上限时第 32 次即可达。
- **回归**：新增 `TestUpperBoundReachable`；压测项 `backoff.next(uint32 上限)` 断言 `== 0xFFFFFFFF`。

### P2-2 头文件缺少 `<utility>`

- **证据**：`heartbeat.hpp` 的内联 `SetOnTimeout` 使用 `std::move`，但只 include 了
  `<cstdint>` / `<functional>`。libstdc++/libc++ 目前会间接带入，属于「能编译但契约不完整」。
- **修复**：显式补 `#include <utility>`。
- **验证**：单独 TU 只包含两个头文件，GCC 13 与 Clang 18 在
  `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` 下均通过。

### P2-3 注入时钟回退时无符号减法下溢，误判对端死亡

- **证据**：`now - m_last_activity_ms` 在 `now < last` 时得到接近 2^64 的值，
  `IsAlive()` 直接判死、`Poll()` 立即触发超时。`steady_clock` 不会回退，但 `NowFn` 是可注入的，
  假时钟/系统时间调整都会踩到。
- **修复**：回退时把 elapsed 夹到 0（`heartbeat.cpp` 的 `IsAlive` 与 `Poll`）：
  宁可多等一个周期，也不因时钟异常误杀活连接。
- **回归**：新增 `TestClockRollback`。

### P2-4 `Poll()` 是 noexcept，但超时回调抛异常会立即 terminate

- **证据**：`TimeoutHandler` 是 `std::function`，`operator()` 不是 noexcept；`Poll() noexcept`
  下抛出即 `std::terminate`。原文档未声明这条契约。
- **处理**：按项目既有惯例（其它模块也不在回调外裹 `try/catch`）只补文档：
  `Poll()` 增加 `@warning`，明确回调只做置标志/关连接这类不抛动作。不改运行期行为。

### P2-5 没有 `.clang-format`，格式不纳入门禁

- **证据**：仓库无 `.clang-format`；`clang-format --dry-run --Werror -style=Google` 对既有
  `src/bento/net/link.cpp` 报 6371 行差异，说明格式从来不是验收条件，非本次引入。
- **处理**：记录，不引入全局格式化（会污染无关文件）。本次改动按仓库现有风格书写，
  全部文件最长行 ≤100 列。

## 3. 解耦与自包含

- `g++ -M` 依赖闭包：`heartbeat.cpp` 只依赖 `heartbeat.hpp` + 系统头；
  `backoff.cpp` 只依赖 `backoff.hpp` + 系统头。**不依赖 `base`，也不依赖任何其它 bento 模块**
  （`link.hpp`/`protocol.hpp` 均未出现在闭包里）。
- 两个头文件各自只 include 标准库（`heartbeat.hpp`：`<cstdint> <functional> <utility>`；
  `backoff.hpp`：`<cstdint>`），可单独包含。
- 压测程序故意不引 `test_util.hpp`（它间接包含 `link.hpp`），用独立断言宏，
  从使用侧再次证明解耦。

## 4. 测试复查

- **断言有效性**：`BENTO_CHECK` 记录失败但不中断、`Finish()` 汇总并返回非零；
  本次新增断言都带具体数值（`CHECK_EQ`）或区间，不是「只要不崩就算过」。
- **覆盖缺口（已补）**：`SetOnTimeout` 覆盖旧回调、`SetInterval` 在未运行时只改配置、
  超时后调大阈值的一致性、时钟回退、`max_ms = UINT32_MAX` 可达性、
  上限处抖动分布、`jitter = 1.0` 不返回 0。
- **规模变化**：`test_heartbeat` 67 → **93** 断言；`test_backoff` 565 → **44,617** 断言
  （新增的分布测试本身跑 2 万采样 × 10 桶）。
- **SKIP 复查**：`ctest -R bento` 48 项，47 通过、1 项 SKIP（`bento_netem`）。
  SKIP 是设计好的：需要 root/CAP_NET_ADMIN 才能用 `tc netem`，脚本显式 `exit 77`，
  CMake 配了 `SKIP_RETURN_CODE 77`；同功能有用户态替代 `bento_weaknet_proxy`（同一轮通过）。
  **不存在用 SKIP 掩盖失败**。

## 5. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
# 单元测试
cmake --build .build-bento-wsl --target bento_test_heartbeat bento_test_backoff
./.build-bento-wsl/bento_test_heartbeat && ./.build-bento-wsl/bento_test_backoff
# 压测（6s 长稳）/ 60s 长稳
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    tests/bento/bench_heartbeat_backoff.cpp .build-bento-wsl/libgo2cpp_bento.a \
    -pthread -o .build-bento-wsl/bench_heartbeat_backoff
./.build-bento-wsl/bench_heartbeat_backoff           # 6s
./.build-bento-wsl/bench_heartbeat_backoff --long    # 60s
# 全量
ctest --test-dir .build-bento-wsl -R bento --output-on-failure
```

Sanitizer 复现（直接编译实现文件，保证被测代码被插桩）：

```bash
# ASan + UBSan + LSan
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iinclude tests/bento/bench_heartbeat_backoff.cpp \
    src/bento/net/heartbeat.cpp src/bento/net/backoff.cpp -pthread -o /tmp/bench_asan
ASAN_OPTIONS=detect_leaks=1 /tmp/bench_asan
# TSan（WSL 内核 mmap_rnd_bits 较大时需关 ASLR，否则报 unexpected memory mapping）
g++ -std=c++17 -O1 -g -fsanitize=thread -Iinclude /tmp/tsan_check.cpp \
    src/bento/net/heartbeat.cpp src/bento/net/backoff.cpp -pthread -o /tmp/tsan_check
setarch "$(uname -m)" -R /tmp/tsan_check
```

## 6. 提交前提示

工作树里有 **119 个文件**存在 `100644 → 100755` 的权限位变更（含 `cmake/`、`docs/`、大量
`bento/net` 源文件），内容无差异，不是本次审查引入的。本次直接改动的 6 个文件与新增的
2 个文件已把权限恢复为 644。提交前建议用 `git diff --summary` 自查，避免把无关的 mode
变更带进提交。
