# Scheduler 示例同步（2026-11-05）

## 1. 扫描基线
- 模式：`Scheduler <name>(` / `Scheduler <name>{` / `new Scheduler`，范围 `user/`。
- 命中 **44 行**，其中 2 行是 `DebugScheduler proxy(...)` 误报 → **42 处真实构造**。
- 分布在 concurrency（最多）、network、debug、app 与 4 篇文档。

## 2. 关键事实（任务前提需修正）
多实例告警是**编译期宏**，不是环境变量：`src/concurrency/scheduler.cpp:3367` 用
`#ifndef GO2CPP_ALLOW_MULTI_SCHEDULER` 包裹，编译进库后无法在示例里用
`setenv("GO2CPP_ALLOW_MULTI_SCHEDULER","1",1)` 关闭。且告警只在 `active_instances>1`
时触发（`start()` 计数、`shutdown()` 归还），顺序创建/销毁的单实例示例本来就不告警。
因此本次对"故意多实例"的示例只保留构造 + 注释说明，未写入无效的 setenv。

## 3. 改造（22 处不再自建）

19 处代码改为 `default_scheduler(...)` / `default_scheduler_handle(...)` + `shutdown_default_scheduler()`，
并补 `#include "go2cpp/go.hpp"`：

| 文件 | 处数 |
|---|---|
| concurrency/runtime_demo、mixed_runtime_demo、dynamic_gmp_demo_new、context_full | 4 |
| concurrency/02-blocking-in-thread、03-context-cancel-leak | 2 |
| concurrency/sync_full（main + 场景5 改传参，消掉并存实例） | 2 |
| concurrency/runtime_full（main 改默认；场景4 移到关闭之后再跑，不并存） | 1 |
| concurrency/parking_condition_full（4 个场景各自 P 数） | 4 |
| concurrency/thread_policy_full（P=1 守卫场景 + P=2 worker 场景，末尾收尾） | 2 |
| concurrency/scheduler_demo（改用 handle，保留 shutdown 后 is_running 断言） | 1 |
| network/http2_h2c_server、http2_h2c_client | 2 |
| debug/debug_module_demo（先挂观察者再取默认实例） | 1 |

另 3 处文档：`user/docs/01-concurrency.md`、`02-network.md` 由并发的文档任务在
`dfc7319` 中同步为 `go()` 写法（已核对）；`user/modules/debug/README.md` 本 agent 同步。

## 4. 保留（20 处）

- **16 处：教学/多实例/多配置，故意保留**
  - `scheduler_full.cpp` 6 处：Scheduler 生命周期与多实例用例（本示例的教学对象）。
  - `dynamic_gmp_full.cpp` 7 处：每场景不同 `SchedulerConfig`（伸缩/亲和/终态），顺序单实例。
  - `dynamic_gmp_demo.cpp` 2 处：**真同时** P=4 与 P=1 两个实例，演示取消竞争。
  - `runtime_full.cpp:123` 1 处：INI 配置驱动 P=3 的独立实例，已挪到默认实例关闭之后。
- **3 处：bento 依赖，构建被跳过，未验证 → 不动**
  - `app/web_server_simple/main.cpp`、`app/web_server_full/app_server.h`、`chat_server_full/app_server.h`。
- **1 处：文档刻意保留**：`concurrency/README.md:91` 已被并发文档任务改写为「直接构造是高级用法」
  的显式重载示例，并注明第二实例会 WARN；尊重该裁定，未推翻。

## 5. 验证

- 编译：**17/17 通过，零警告零错误**。13 个 CMake 示例目标（`GO2CPP_BUILD_EXAMPLES`）
  + 4 个未登记示例（02/03、http2 server/client）用 `g++ -std=c++17 -Wall -Wextra -Wpedantic`
  手工链接仓库 `.so` 通过。
- 运行：15 个可运行示例 **rc=0**；14 个**零多实例告警**；
  `dynamic_gmp_demo` 出现 1 条预期内告警（故意多实例）。
- 复扫：`grep -rnE 'Scheduler +[A-Za-z_]+ *[({]' user/` → **22 行**，扣除 2 处 DebugScheduler
  误报为 **20 处**，即上表保留项；`grep -c "Scheduler s("` 已为 0。

## 6. 备注

- 未改 `src/`、未主动 commit；代码改动被并发提交 `dfc7319`、报告被 `13aaafe` 纳入。
  时间盒超时（约 75 分钟），bento 三个应用示例未纳入编译/运行验证。
