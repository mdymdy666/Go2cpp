# 调试内省（debug）

> **一句话定位**：运行期内省：快照 / 事件环 / 断点 / dump CLI。
> 本文是「模块 → 能力 → 异常」手册；[可观测性总览（debug 事件环/回放）](../../deep/10-observability.md)


> 调试模块的示例与说明：不打断程序的前提下，看清「调度器在干什么、
> 谁持有锁、Fiber 叫什么名字、事件怎么流动」。

## 能力清单

`go2cpp::debug` 是一个**只读观察层**：它给运行中的调度器、Fiber、锁加上
元数据与快照能力，而**不改变框架的任何行为**。

设计上有两条硬约束（见各头文件头部）：

1. `debug` 依赖框架（`Fiber`/`Scheduler`/`sync`），但**框架对 `debug` 零依赖** ——
   关掉调试功能时，热路径没有任何分支；
2. `Debug*` 类通过**继承原类**扩展（`DebugFiber : Fiber`、`DebugMutex : sync::Mutex`…），
   不重复实现；因此**绝不能**通过基类指针销毁派生对象（基类析构非虚）。

能力分成六组：

| 组 | 头文件 | 能做什么 |
| --- | --- | --- |
| Fiber 元数据 | `debug_fiber.hpp` | 给 Fiber 挂任务名/描述/源码位置，按 id 查询 |
| 调度器代理 | `debug_scheduler.hpp` | `Snapshot()` + text/ini/json 三种快照格式 |
| 锁与等待组 | `debug_sync.hpp` | `DebugMutex`/`DebugConditionVariable`/`DebugWaitGroup` 的只读快照 |
| 事件采集 | `debug_events.hpp` | 实现 `SchedulerObserver`，把调度事件收集成可导出的流 |
| 协作式断点 | `debug_breakpoints.hpp` | `BreakpointSpec` + `Checkpoint`/`ScopedCheckpoint`，只登记命中不阻塞 |
| 事件环 / dump / 服务 | `debug_event_ring.hpp`、`debug_dump.hpp`、`debug_cli.hpp`、`debug_server.hpp` | 环形缓冲、CLI 解析、Unix socket 在线查询 |

## 底层架构

```
       业务代码                    debug 模块（只读）
          │                              │
   Scheduler ──set_observer──► DebugEventCollector ──► 事件流 / 导出
          │                              │
          ├── DebugFiber 元数据注册表 ◄───┤  按 G id 查「这是谁的任务」
          ├── DebugMutex / CV / WaitGroup │  锁持有者、等待队列快照
          └── DebugBreakpoints ◄──────────┘  Checkpoint 命中计数

   快照出口：DebugScheduler::DumpText() / DumpIni() / DumpJson()
   在线出口：DebugServer（Unix socket，需 GO2CPP_DEBUG_DUMP=ON）
```

**编译期开关决定开销**（都默认 OFF，宏展开后热路径零成本）：

| 开关 | 打开后 |
| --- | --- |
| `GO2CPP_DEBUG_DUMP` | 事件环记录 park/wake/runqueue + `DebugServer` 监听 |
| `GO2CPP_REPLAY` | 录制资源事件到 G2CRPLY 流（用于离线重放） |
| `GO2CPP_DIAGNOSTIC` | 在线诊断 agent |
| `GO2CPP_BUILD_ENHANCED` | 一键打开上面四个（含 `MIGRATION_API`） |

## 最小示例

```cpp
#include "go2cpp/debug.hpp"
#include "go2cpp/scheduler.hpp"

int main() {
    using namespace go2cpp;
    using namespace go2cpp::debug;

    SchedulerConfig config;
    config.processor_count = 2;
    // 默认契约：进程级唯一调度器；default_scheduler() 首次调用即启动，
    // 因此先取实例、再挂观察者（避免漏掉 start 事件）。
    Scheduler& scheduler = default_scheduler(config);

    // ① 挂事件采集器
    auto collector = std::make_shared<DebugEventCollector>(4096);
    scheduler.set_observer(collector);

    // ② 注册协作式断点（不阻塞，只计数）
    BreakpointSpec spec;
    spec.name = "demo";
    const auto id = DebugBreakpoints::Instance().Add(spec);

    for (int i = 0; i < 20; ++i) {
        scheduler.spawn([] { ScopedCheckpoint ck("demo"); (void)ck.hit(); })->wait();
    }

    // ③ 快照：text / ini / json 三种格式任选
    DebugScheduler proxy(scheduler);
    const auto snap = proxy.Snapshot();          // processor_count / worker_count / runnable_total
    const std::string text = proxy.DumpText();

    shutdown_default_scheduler();
    return DebugBreakpoints::Instance().Hits(id) == 20 ? 0 : 1;
}
```

完整版（5 个部分）见 `debug_module_demo.cpp`。

## 3 个常见异常

### 1. `DebugServer::Start()` 返回 false
- **现象**：在线查询端口连不上，Start 返回 false。
- **原因**：未开 `GO2CPP_DEBUG_DUMP`，事件环与 Server 被宏完全消除——这是**预期行为**，不是故障。
- **处理**：用 `-DGO2CPP_DEBUG_DUMP=ON`（或 `GO2CPP_BUILD_ENHANCED`）重新编译后重试。

### 2. 通过基类指针 delete `DebugFiber`
- **现象**：析构时崩溃或内存破坏。
- **原因**：`Fiber`/`sync::Mutex` 的析构**不是虚函数**，`Debug*` 只是继承扩展。
- **处理**：始终按具体类型析构；`DebugFiber` 不得放进按 `Fiber*` 删除的位置（如 `Task::m_fiber`）。

### 3. 用快照数字做调度决策
- **现象**：按 runnable 数扩缩容，行为抖动。
- **原因**：快照是**瞬时视图**，读到即过期，采集本身也有开销。
- **处理**：只用于观测/导出；控制回路用框架自身的自适应扩缩，不要自建反馈。

## 示例清单

| 示例 | 演示什么 |
| --- | --- |
| `debug_module_demo` | ① `DebugScheduler` 代理 + text/ini/json 快照；② `DebugEventCollector` 采集与导出；③ `DebugMutex`/`DebugConditionVariable`/`DebugWaitGroup` 只读快照；④ 协作式断点（`DebugBreakpoints` + `Checkpoint`/`ScopedCheckpoint`）；⑤ dump CLI（`ParseDumpCommand` + `RunDumpCli`）写文件 |

```sh
cmake --build /tmp/build-user --target go2cpp_debug_module_demo -j
/tmp/build-user/go2cpp_debug_module_demo
```

该示例已注册为 ctest 用例（必须快速结束、失败返回非零）。

## 源码位置

| 能力 | 头文件 | 实现 |
| --- | --- | --- |
| Fiber 元数据 | `include/go2cpp/debug/debug_fiber.hpp` | `src/debug/` |
| 调度器快照 | `include/go2cpp/debug/debug_scheduler.hpp` | `src/debug/` |
| 锁/等待组快照 | `include/go2cpp/debug/debug_sync.hpp` | `src/debug/` |
| 事件采集 | `include/go2cpp/debug/debug_events.hpp` | `src/debug/debug_event_ring.cpp` |
| 事件环 | `include/go2cpp/debug/debug_event_ring.hpp` | `src/debug/debug_event_ring.cpp` |
| 断点 | `include/go2cpp/debug/debug_breakpoints.hpp` | header-only |
| dump CLI | `include/go2cpp/debug/debug_cli.hpp` | `src/debug/debug_dump.cpp` |
| 在线服务 | `include/go2cpp/debug/debug_server.hpp` | `src/debug/debug_server.cpp` |
| 信号 dump | `include/go2cpp/debug/debug_signal_dump.hpp` | header-only |
| 一次性入口 | `include/go2cpp/debug.hpp` | 转发头 |

设计与类约定见 `DebugTooling/design/09-debug-class-conventions.md`；
整体架构见 `ArchitectureDiagram/diagrams/G10-debug.md`。

## 注意事项

- **不要通过基类指针销毁 `Debug*` 对象**：`Fiber`/`sync::Mutex` 的析构不是虚函数；
  `DebugFiber` 也**不得**放进 `Task::m_fiber`（那里按 `Fiber*` 删除）。
- **默认什么都不做**：不开 `GO2CPP_DEBUG_DUMP` 时事件环被宏完全消除，
  `DebugServer::Start()` 返回 false 是**预期行为**，不是故障。
- **断点是协作式的**：`Checkpoint` 只记录命中，不会暂停执行；
  要实现「停在某处」得自己在命中后等待外部 Resume。
- **快照是瞬时视图**：读到的 worker/runnable 数会立刻过期，别拿它做判断依据。
- **dump 文件会落在工作目录**：CLI 的路径参数是相对当前工作目录的。

## 深入阅读

- [可观测性总览（debug 事件环/回放）](../../deep/10-observability.md)
- [交接摘要（docs）](../../docs/10-observability.md)
- 源码位置：`include/go2cpp/debug/`、`src/debug/`、一次性入口 `include/go2cpp/debug.hpp`
- 示例目标名 = `go2cpp_<文件名>`；新增示例需同时登记 `cmake/Go2CppTests.cmake` 与本文清单。
