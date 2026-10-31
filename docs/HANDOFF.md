# 交接文档：Go2Cpp 项目认知 + 当前任务进度

> 用途：供新接手的 agent 在**零上下文**下快速建立项目认知并继续未完成任务。
> 生成时间：2026-10-15

---

## 第一部分：项目认知（先读这一部分）

### 1.1 这是什么

**Go2Cpp** 是一个用 C++17 实现的 Go 运行时语义库（Linux x86_64）。它提供
goroutine（Fiber/Task）、channel、context、sync、GMP 调度器、epoll IO 后端、
定时器、日志、配置等模块，目标是让 Go 代码能以接近 1:1 的方式转译为 C++。

### 1.2 核心概念映射

| Go | Go2Cpp | 说明 |
|---|---|---|
| G（goroutine） | **`Task`** + **`Fiber`** | `Task` 是调度器侧的句柄（id/状态/等待者），`Fiber` 是执行栈本身 |
| M（OS 线程） | **`Machine`**（`MId`） | worker 线程，惰性创建 |
| P（处理器） | **`Processor`**（`PId`） | 本地运行队列 |
| channel | `go2cpp::Channel<T>` | |
| context | `go2cpp::Context` | 取消树 + 值传递 |

**注意**：`PId` / `MId` **从 0 开始编号，0 是合法 ID**（不是"无效值"）。
`worker_count()` = 当前存活 M 数，**不等于 P 数**。

### 1.3 目录结构

```
include/go2cpp/
  <module>.hpp              转发头（4~8 行，仅 #include 真身）
  concurrency/              真身：channel/context/sync/future/timer/scheduler/fiber...
  tools/                    真身：error/control_flow/go/json
  core/                     真身：parking_condition/hybrid_mutex/timer
  network/                  真身：address/bytearray/fd_manager/sse/http/websocket
src/                        对应实现
example/                    可运行示例（demo=简单版 / full=全面版）
benchmarks/                 基准测试
tests/                      测试（tests/test_support.hpp + GO2CPP_TEST_FILTER）
third_party/                第三方（含 Go 参考源码、nlohmann/json）
docs/                       设计与审计文档
codex.md                    **AI 协作记录（含 24 轮历史，必读）**
```

### 1.4 构建与测试

```bash
# 质量构建（0 warning / 0 error 是硬性要求）
cmake -S . -B build-quality -DGO2CPP_BUILD_TESTS=ON
cmake --build build-quality --parallel
cd build-quality && ctest

# 性能构建（⚠️ benchmark 必须用 Release）
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_BUILD_BENCHMARKS=ON -DGO2CPP_BUILD_TESTS=OFF
cmake --build build-bench --parallel
```

**⚠️ 关键陷阱**：`build-quality` 的 `CMAKE_BUILD_TYPE` **为空 = 无优化**。
**用它测出来的计时数据毫无意义**，性能结论必须用 `build-bench`（Release）。

### 1.5 硬性约束（违反会导致返工）

- **禁止 PowerShell**。Linux/WSL 任务用 `wsl bash -lc "..."`；Windows 侧杂务写
  Python 脚本到 `.dsh_tmp/`，**开头必须** `sys.stdout.reconfigure(encoding="utf-8", errors="replace")`，
  用完自删（文件名以 `_` 开头或含 `keep` 的可保留）。
- **git 只 add 明确路径**。禁止 `add -A` / `add .` / `commit -a` / `push` / `reset` /
  `checkout` / `stash` / `clean`。只能 `git restore --staged <file>` 撤暂存。
- **提交前门禁**：`git status --short` 与"不碰清单"全量比对，出现未预期改动即中止。
- **验收四步**：登记 CMake → 构建 0 warning/0 error → **真实运行** → 逐条比对输出与注释里的"预期"。
- **`stdbuf -o0 -e0`** 是排查"输出丢失/静默挂起"的第一手段。
- 排查路径解析陷阱：`read`/`grep`/`edit` 工具可能把 `/UserData/...` 解析到 Windows 过期副本。
  **项目文件一律用 `wsl bash -lc 'cd /UserData/CodexWorkSpace/Go2Cpp && ...'` 操作。**

### 1.6 已固化的经验（血泪教训）

1. **Promise 不可复制** → 装进 `shared_ptr` 才能进 `std::function`。
2. **临时 `Timer` 立刻析构 = 取消任务** → 句柄必须存活。
3. **`Wrap` 的错误 `Message()` 是整条链**（`"outer: inner"`）→ 判断层级用**指针身份比较**，
   不能用文本比较。
4. **`errors::Is` 按身份而非文本**。
5. **断言必须在输出里可见** —— 未打印的条件静默失败过两次。
6. **涉及另一执行流/后台线程/异步计数的断言，必须用「轮询+超时」，不能一次采样。**
   （栽过三次：控制帧交付、`\r` 换行、`metrics()` 窗口）
7. **`LogFormatter` 的格式项键是单字符**，`T`/`F` 等多数已被内置占用。
8. **Windows 控制台默认 GBK** → Python 脚本必须显式重配 UTF-8 输出。

---

## 第二部分：当前任务进度

### 2.1 任务背景

用户关注 Go2Cpp 的性能与**Fiber 栈溢出风险**。已完成大面积实测，正在做
example 演示与 nlohmann/json 薄封装。

### 2.2 已完成（勿重做）

| 项 | 状态 |
|---|---|
| `third_party/nlohmann/json.hpp` | **已部署**（919,975 字节，v3.11.3，**MIT**）★ 2026-10-15 实测修正：上游 `parse()` 实际是**迭代**实现（`sax_parse_internal` + 堆上 `states` 显式栈），"递归下降"的旧结论不成立；真正会爆栈的是 `dump()`/拷贝（见 §2.4 第 1 条的修正说明） |
| `benchmarks/fiber_stack_probe.cpp` | **已写并跑完**，45 组实测数据 |
| `benchmarks/gmp_compute_benchmark.cpp` | **已写并跑完** |
| `build-bench/` | **已配置**（Release + benchmarks ON） |
| SSE `\r` 换行缺陷 | 已定位并修正示例（commit `cb1bc88`） |
| scheduler metrics flaky | 已修（commit `351f756`） |
| config `ReadSize` 缺陷 | 已修（commit `b215579`） |

### 2.3 核心实测结论（可直接引用）

**栈深度公式（45 组实测，`stack_utilization` 全为 1.0000）**：

```
可承受 C++ 调用深度 ≈ 栈容量 / (每帧局部变量字节数 + 128)
```

那个 **128 字节**是与帧大小无关的固定开销（返回地址 + 保存寄存器 + 对齐）。

| 栈容量 | frame=0 | 16 | 64 | 256 | 1024 |
|---|---:|---:|---:|---:|---:|
| 8 KiB | 62 | 55 | 41 | 20 | 6 |
| **128 KiB（Fiber 默认）** | **1022** | 908 | 681 | **340** | **112** |
| 1 MiB | 8190 | 7280 | 5459 | 2729 | 909 |
| **8 MiB（pthread）** | **65497** | 58219 | 43664 | 21831 | 7276 |

**关键概念**：**Fiber 嵌套深度（`nesting_depth()`）与 C++ 调用深度是两个正交维度。**
`nesting_depth()` 数的是 Fiber 链（A resume B resume C），每层**自带独立栈**；
爆栈对应的是 C++ 函数调用深度。**因此"到一定深度就 resume 一个新 Fiber"是有效的
规避手段 —— 它是 Go 自动增长栈的手动等价物。**

**爆栈行为**：`mmap` 的栈最低地址处有 1 页 `PROT_NONE` guard page；栈向下增长，
越界即 SIGSEGV。**全项目没有 SIGSEGV handler、没有 sigaltstack** → 内核默认动作
= 进程立即终止（退出码 139）。**不可捕获、不可恢复。**

**性能实测（build-bench，12 逻辑核）**：

| 场景 | 结果 |
|---|---|
| CPU 满负载 12 路 | **Fiber 46.55ms vs Thread 45.55ms = 1.02x**（无协程税） |
| 扩展性 | 2→2.02x, 4→4.01x, 8→7.43x(92.9%), 12→**10.46x(87.2%)** |
| IO 密集（4096 fd / 25 轮） | **Fiber 690ms vs thread-pool 2208ms = 快 3.2x** |
| IO setup 成本 | **Fiber 170ms vs thread-pool 6.5ms（慢 26x）** |
| G 创建（20 万并发） | **25.2 µs/G vs 线程 85.5 µs/T** |

### 2.4 待办（按顺序执行）

1. **写薄封装** `include/go2cpp/tools/json.hpp`（真身）+ `include/go2cpp/json.hpp`（转发头）。
   设计要点：`using Json = ::nlohmann::json;` + `JsonScanDepth(text, max_depth)`
   （扫 `[`/`{` 深度，跳过字符串与转义）+ `ParseJson(text, max_depth = 64)`
   （**先扫深度、超限即拒**，再交给 `nlohmann::json::parse`）。
   **必须限深**，理由经 2026-10-15 实测修正：上游 `parse()` 本身是**迭代**的
   （`sax_parse_internal` 用堆上的 `states` 显式栈，20 万层可正常解析），
   但 `dump()` 与拷贝仍是**递归**的 —— 128 KiB 栈上的临界深度实测为
   `1162 / 816` 层（Release）与 `609 / 185` 层（无优化）。所以栅栏拦的是
   「深文档进入程序之后」的递归操作与 O(深度) 资源放大，而不是解析本身。
2. **改 `cmake/Go2CppModules.cmake`**：在 `target_include_directories(go2cpp_runtime INTERFACE ...)`
   （约第 134 行）里加 `${CMAKE_SOURCE_DIR}/third_party`。
   **用 `CMAKE_SOURCE_DIR` 而非 `CMAKE_CURRENT_SOURCE_DIR`**（该文件在 `cmake/` 子目录）。
3. **构建验证**（0 warning / 0 error）。
4. **部署 example**：`example/concurrency/fiber_stack_limit_demo.cpp`（简单版）+
   `fiber_stack_limit_full.cpp`（全面版 6 场景：① 探深 ② 递归改迭代 ③ **换 Fiber 续跑**
   ④ 显式限深 ⑤ 迭代式解析 ⑥ **JSON 深层嵌套**），注册进 `GO2CPP_EXAMPLES`。
5. **跑通并逐条比对输出**。
6. **更新 `codex.md`**（按项目规范记录本轮改动）。
7. **提交**（用户已授权；只 add 明确路径）。

> **★ 2026-10-15 完成情况**：以上 7 项已由接手的新 agent 全部完成并提交
> （`codex.md` 最新一轮「第 25 轮」）。其中 §2.2 的"递归下降"结论与第 1 项的
> 限深理由都经**实测修正**：nlohmann 3.11.3 的 `parse()` 是迭代实现，会爆栈的
> 是 `dump()`/拷贝。两个示例实际为 3 场景（demo）与 6 场景（full，含 fork 反证），
> 并额外修掉了 `benchmarks/` 的 4 处编译警告（此前 benchmarks 未纳入 0 warning 门禁）。

### 2.5 未提交的改动清单

```
third_party/nlohmann/json.hpp          新增
benchmarks/gmp_compute_benchmark.cpp   新增
benchmarks/fiber_stack_probe.cpp       新增
cmake/Go2CppTests.cmake                修改（注册两个 benchmark）
build-bench/                           新增构建目录（应加入 .gitignore）
```

**注意**：用户在查看的 6 个在途文件当时未提交，且**不在本次任务范围内**：
`example/concurrency/future_full.cpp`、`include/go2cpp/concurrency/context.hpp`、
`include/go2cpp/logging/log.hpp`、`include/go2cpp/tools/error.hpp`、
`src/concurrency/context.cpp`、`src/tools/error.cpp`。

### 2.6 example 场景 3 的实现要点（换 Fiber 规避爆栈）

```cpp
constexpr std::size_t kSwitchEvery = 600;      // 留足余量（128 KiB 上限 1022）
constexpr std::size_t kNestedStack = 128U * 1024U;

std::uint64_t recursive_with_new_fiber(std::size_t depth, std::size_t* switches) {
    if (depth == 0) return 0;
    if (depth % kSwitchEvery == 0) {
        ++(*switches);
        std::uint64_t inner_result = 0;
        go2cpp::Fiber inner(                                     // ★ 换栈
            [&] { inner_result = recursive_with_new_fiber(depth - 1, switches); },
            kNestedStack);
        (void)inner.resume();                                    // 父级在此阻塞
        return inner_result + ...;
    }
    // 正常递归层
}
```

**三个代价必须写进注释**：父 Fiber **阻塞**在 `resume`（同步，非并行）；
内存叠加（N 层 = N × 128 KiB）；每层有真实 Fiber 创建开销（约 25 µs）。

---

## 第三部分：新 agent 的启动步骤

1. 读本文件；
2. 读 `codex.md` **最新一轮**（了解最近改动与风格）；
3. 读 `include/go2cpp/concurrency/fiber.hpp`（228 行，公开接口）；
4. 按 §2.4 顺序执行待办；
5. 每完成一步做构建验证，不要攒到最后；
6. 全部完成后更新 `codex.md` 并提交。

---

## 第四部分：模块阅读清单（新 agent 先做这件事）

### 4.1 目录地图（实测）

```
include/go2cpp/
├── *.hpp                    27 个**转发头**（4~8 行，仅 #include 真身）
│                            ⚠️ 看到它们不要直接读，去对应真身目录
├── network/                 12 个真身头
├── concurrency/              8 个真身头
├── concurrency/core/         3 个
├── concurrency/scheduler/    1 个
├── concurrency/detail/       1 个
├── core/                     4 个
├── tools/                    4 个
├── scheduler/                1 个
├── logging/                  1 个
├── config/                   1 个
└── detail/                   1 个
```

**27 个公开模块入口**（转发头名即模块名）：

```
address  bytearray  channel  config  context  control_flow  error  event
fd_manager  fiber  fiber_local  future  go  hook  http  io  io_backend
log  myendian  runtime  scheduler  socket  socket_stream  sse  sync
thread_policy  websocket
```

**映射规律**（重要）：

| 公开头 | 真身位置 |
|---|---|
| `go2cpp/context.hpp` | `go2cpp/concurrency/context.hpp` |
| `go2cpp/fiber.hpp` | `go2cpp/concurrency/fiber.hpp` |
| `go2cpp/channel.hpp` | `go2cpp/concurrency/channel.hpp` |
| `go2cpp/core/parking_condition.hpp` | `go2cpp/concurrency/core/parking_condition.hpp` |
| `go2cpp/scheduler.hpp` | `go2cpp/concurrency/scheduler/scheduler.hpp` |
| `go2cpp/error.hpp` | `go2cpp/tools/error.hpp` |
| `go2cpp/log.hpp` | `go2cpp/logging/log.hpp` |
| `go2cpp/config.hpp` | `go2cpp/config/config.hpp` |
| `go2cpp/http.hpp` | `go2cpp/network/http.hpp` |

⚠️ **例外**：`go2cpp/sync.hpp`（219 行）**本身就是真身**，而
`go2cpp/concurrency/sync.hpp` 才是转发头 —— **与其余模块相反**。

### 4.2 自取规模（不要手数，用这条命令）

```bash
cd /UserData/CodexWorkSpace/Go2Cpp
for n in address bytearray channel config context control_flow error event \
         fd_manager fiber fiber_local future go hook http io io_backend log \
         myendian runtime scheduler socket socket_stream sse sync \
         thread_policy websocket; do
  s=$(find include src -name "$n.hpp" -o -name "$n.cpp" 2>/dev/null \
      | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')
  printf "%-16s %s\n" "$n" "${s:-0}"
done | sort -k2 -rn
```

### 4.3 建议阅读顺序（按依赖方向，从地基往上）

已实测的规模（供安排精力）：

| 顺序 | 模块 | 规模 | 为什么读它 |
|---:|---|---:|---|
| **1** | `detail/context_backend` | 小 | **Fiber 的唯一依赖**（协程后端抽象） |
| **2** | **`fiber`** | **228 + 1502** | **真正的地基**：G 的载体、park/unpark 的一端 |
| **3** | `core/timer` | 782 | 被 ParkingCondition 与 Scheduler 共同依赖 |
| **4** | `core/parking_condition` | 341 | **站在 Fiber × Scheduler 交叉点**（三身份分发） |
| **5** | `scheduler` | 很大 | 反过来依赖 ④；GMP 实现 |
| **6** | `context` | **453 + 2402** | 取消树；`DoneSignal`/`Savepoint`/`ContextRollback` |
| **7** | `error` | 482 | 全项目横切；`Wrap` 的链式 Message 是坑点 |
| **8** | `channel` / `future` / `sync` | 406 / 179 / 1543 | 并发原语，Context 的消费者 |
| **9** | `network/*` | 12 个头 | epoll 后端、http、websocket、sse |
| **10** | `log` / `config` / `tools/*` | 小 | 横切设施 |

**依赖方向是单箭头**：`context_backend → Fiber → Timer → ParkingCondition → Scheduler`。
**每一步只依赖前面读过的**，按此顺序不会卡住。

### 4.4 四份必读的文档（在读完模块后）

| 文档 | 作用 |
|---|---|
| `codex.md` | **AI 协作记录，含 24 轮历史** —— 改动风格、提交约定、踩过的坑 |
| `docs/modules.md` | 模块职责与边界 |
| `docs/design.md` | 整体设计取舍 |
| `docs/dependencies.md` | 模块依赖关系 |

### 4.5 ⚠️ 模块阅读的四个陷阱

1. **转发头陷阱**：`include/go2cpp/*.hpp` 全是 4~8 行转发头，**读它们等于什么都没读**。
2. **`sync` 是反例**：见 §4.1 的例外说明。
3. **行号会漂移**：`context.cpp` / `context.hpp` 被用户重排过，
   引用时**用符号名定位，行号仅作参考**。
4. **注释质量参差**：`context.cpp` 有 71 处模板化的敷衍注释
   （「函数功能：…执行流程：1. 校验传入参数…」），**与实际代码不符，不可信**；
   以**代码本身**为准。
