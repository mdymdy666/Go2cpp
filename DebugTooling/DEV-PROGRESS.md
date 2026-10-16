# 调试模块开发 · 进度表（DEV）

> 分支：`feature/gmp-debug-module`；窗口：2026-10-16 13:00 – 18:00（每 15 分钟一轮）。
> 设计依据：本目录 `design/` 与 `scenarios/`；本项目规范：`PROJECT_RULES.md`。
> **使用者定案**：① 元数据用 `DebugFiber` 继承 `Fiber`（调试信息放 `DebugFiber`，不影响原框架）；
> ② 多机时钟用双时间戳 + 明确声明不做绝对对齐；③ 运行期可改参数只开放观测/节律类。

## 总览

| 阶段 | 目标 | 状态 |
|---|---|---|
| P0 | 只读快照接口 + 元数据 + `emit_event` 微基准 | 🚧 进行中 |
| P1 | 序列化（text/ini/json）+ stdio/文件 sink → `dump scheduler` 可用 | ⏳ |
| P2 | 事件采集 + 环形缓冲 + `--follow` | ⏳ |
| P3 | 锁观测（D1/D2）+ 锁争用选择钩子（D3） | ⏳ |
| P4 | 控制面（迁移/插队/延迟替换），默认关闭 | ⏳ |
| P5 | 协作式断点（5 个安全点） | ⏳ |

## 逐轮记录

| 轮 | 内容 | 证据 |
|---:|---|---|
| 1 | 建分支 `feature/gmp-debug-module`；删除旧定时任务；**编译探针验证 `DebugFiber` 继承可行性** | `git branch --show-current`；探针输出 `Fiber_virtual_dtor=0 / is_final=0 / default_ctor=0 / copyable=0 / movable=0 / DebugFiber_ok=1` |
| 2 | 核实**原类可继承性**并写成约定：仅 `Fiber`/`Task`/`Scheduler`/`SchedulerObserver` 可继承；`Mutex`/`ConditionVariable`/`Channel`/`Context`/`Timer`/`HybridGate`/`ParkingCondition` 等**被标记 `final`**，不可继承也不改原框架 | `grep -rn "class [A-Za-z_]* final" include/`；新增 `design/09-debug-class-conventions.md` |
| 3 | 实现 **`DebugFiber`（继承 `Fiber`）+ `DebugRegistry`**（独立注册表，原框架零改动）；补测试（5 场景 + 压测 + 随机）；登记 CMake 与 `test_main`；构建通过 | `include/go2cpp/debug/debug_fiber.hpp`（183 行）、`tests/debug/test_debug_fiber.cpp`（232 行）、`cmake/Go2CppTests.cmake`、`tests/test_main.cpp` |
| 4 | 生成 **final 类清单**（81 个，含各自是否含虚函数）：**自身含 `virtual` 的只有 2 个**（`SelectValue`、`tools::defer`），其余 79 个为非虚 → 去掉 `final` 对它们零影响（已有实测支持）；核心相关类 16 个；确认**代理方案**并写入 `design/09` §2.4 | `design/10-final-class-inventory.md`（132 行）；`design/09` §2.4；脚本按花括号切类体逐个判定 |
| 5 | 观察者开销三档对照基准：新建 benchmarks/debug_observer_overhead_benchmark.cpp（222 行）并登记 CMake；**发现并修正一处测量错误**（缺热身轮导致先测档偏慢约 20 倍）；实测 1/2/4 worker 三档；**关键结论**：单 worker 下观察者净开销仅 +3~6%（约 27 ns/事件），而 **4 worker 基线本身异常**（1,602 → 19,914 ns，约 12 倍）属调度器自身扩展性问题、与观察者无关；建议插件用线程局部缓冲规避 observer_mutex 争用 | `DebugTooling/perf-observer-overhead.md`；`docs/performance.md` 新节；`./build-bench/go2cpp_debug_observer_benchmark 3 20000 {1,2,4}` |

## 下一步（第 4 轮）

1. **跑通全量验证**：`GO2CPP_TEST_FILTER=debug_fiber ./build-quality/go2cpp_tests` 与 `ctest`；
2. **P0 继续**：`emit_event` 微基准（三档：不调用 / observer 为空 / 空回调），
   这是设计文档中"零开销证明"的第一步，成本最低；
3. 之后开始 R1/R2 只读快照接口的**可行性设计**（注意：`Task`/`Scheduler` 无虚析构，
   按 `design/09` 的档 A/档 C 策略处理）。

## 约束提醒（每轮自查）

- 不改动原框架文件（`Fiber`/`Task`/`Scheduler`/`sync` 等）：本模块只**新增**文件；
- 测试必须含压测 + 随机 + ≥5 真实场景，新增测试必须登记 CMake；
- 性能结论只用 `build-bench`（Release）；每个逻辑单元跑 `build-quality` + `ctest`；
- git 只 add 明确路径；不提交使用者此前在途的 4 个文件。
