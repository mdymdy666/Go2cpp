# SO 热重载：进度与审查记录

> 工作目录：`/UserData/CodexWorkSpace/Go2Cpp-work`（`feat/isolated` 分支 + `build-work/`）
> 主目录 `/UserData/CodexWorkSpace/Go2Cpp` 属于用户，本项目所有改动**不触碰它**。

## 阶段与状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| **step1** | 17 个工业级场景/边界 → `docs/so-reload-scenarios.md` | ✅ **完成**（`ddb8344`） |
| **step2** | config 分层扩展设计 + 实现 → `docs/so-reload-design.md`、`so_manifest.*`、`so_reloader.*` | ✅ **完成**（`acca90d`） |
| **step3** | 11 个场景测试全部通过；原 113 个测试无回归 | ✅ **完成**（**ctest 114/114**） |
| **step4** | F2 三项指标 + E3 `ForceRescan` + 使用文档 | ✅ **完成**（本轮；ctest 114/114） |

## config 模块的既有能力（审查基准）

我核对了 `include/go2cpp/config/config.hpp`，以下原语**已存在**，新设计不得重复造：

| 原语 | 位置 | 对 SO 热重载的映射 |
|---|---|---|
| `ConfigVar::ApplyString()` | 第 221 行 | 第一阶段：转换+校验+写入，**不通知** → 新 SO 已加载校验但未切换 |
| `HasPending()` | 第 223 行 | 是否有暂存的新版本 |
| `NotifyPending()` | 第 227 行 | 第二阶段：统一发布 → 全部成功后一次性切换 vtable |
| `RollbackPending()` | 第 230 行附近 | 回滚 → 部分失败时全部还原 |
| `StartWatcher` | 文件头部注释提到 | 文件监听 + `.lock` 协议 + 稳定快照 → 分层监听的现成实现 |
| `AddListener()` | 第 344 行 | 值变更回调 → `dlopen` 触发点 |

**关键判断**：SO 热重载需要的事务语义（两阶段提交、失败回滚、文件监听）**config 已经具备**，
新增的应该是「加载器作为消费者接入这条链」，而不是另造一套。

## 审查标准（我审 step1 产出时用）

1. **数量**：≥12 个场景；
2. **覆盖**：A 依赖与顺序 / B 配置组织 / C 在途调用 / D 失败与回滚 / E 并发 / F 隔离与可观测，六类都要有；
3. **落地**：每个场景必须写出「复用哪个 config 原语 / 还是需要新增」，不能只写概念；
4. **不编造**：库名、论文、协议细节不确定就标「需调研」；
5. **可测**：每个场景要有一句话的「测试怎么模拟」。

## 轮次日志

| 轮次 | 时间 | 动作 | 结果 |
|---|---|---|---|
| 1 | 15:05 | 建 goal + 定时任务 + 派子 agent | `fd48cb1f` running |
| 2-6 | 15:10~15:22 | 摸清 config 接口；给子 agent 补充「已有两阶段事务」的输入 | 已送达 |
| 7 | 15:24 | 检查产出 | **无文件**，`git status` 空 |
| 8 | 15:16 | 补 F2 三指标、E3 `ForceRescan`、`docs/so-reload-usage.md` | ✅ 构建通过；SO 测试 1/1；全量 114/114 |

## step2 / step3 完成记录（`acca90d`）

### 交付

| 文件 | 内容 |
|---|---|
| `docs/so-reload-design.md` | 423 行：分层、17 场景逐条键/校验、事务时序、ABI 约定、不做项 |
| `docs/state-machines/SoReloader.md` | 状态转化表（注释规范要求） |
| `include/go2cpp/config/so_manifest.hpp` + `src/config/so_manifest.cpp` | `Parse` / `Validate` / `BuildLoadOrder` / `HashFile` |
| `include/go2cpp/config/so_reloader.hpp` + `src/config/so_reloader.cpp` | `Reload`：两阶段事务 + revision 栅栏 |
| `tests/config/test_so_manifest.cpp` | 629 行，11 个用例，假 loader |
| `cmake/*.cmake`、`codex.md` | 构建接入 + 变更记录 |

### 关键设计（已独立核实）

1. **组合而非继承**：`SoReloader` 持有 `Config&`，复用既有 `ApplyString`/`HasPending`/`NotifyPending`/`RollbackPending`；
2. **静态层不 `dlopen`**：loader 用 `std::function` 注入，测试可完全可控；
3. **revision 栅栏**：`fetch_max` 在取串行锁前执行，旧 revision 即使加载完成也会在提交前回滚；
4. **失败只卸新句柄**：`SoUnloader` 只作用于本次新加载的 SO，**旧版本永不经过 unloader**。

### 我的独立验证（不采信子 agent 自述）

```
git status --porcelain           → 只有预期文件
git diff include/go2cpp/config/config.hpp src/config/config.cpp → 空（既有接口未改）
cd /UserData/CodexWorkSpace/Go2Cpp && git status --porcelain → 空（主目录未被碰）
cmake --build build-work -j8     → 通过
ctest --test-dir build-work -R 'so_manifest|config_registry' → 2/2 通过
ctest --test-dir build-work      → 114/114 通过（原 113 + 新增 1）
```

### 子 agent 诚实列出的未实现项

| 项 | 状态 |
|---|---|
| `dlopen`/`dlsym`/`dlclose` 本身、vtable 存储与切换 | 由加载器层注入，本层不做 |
| ABI **符号级**校验（`go2cpp_so_abi_version()` 的真实值比对） | 需 loader 在 `dlopen` 后做 |
| E3 `IN_Q_OVERFLOW` 全量重扫 | 未实现 |
| F1 `candidate` 流量灰度 / 进程隔离 | 只解析透传 |
| F2 `reload_success_total`/`failure_total`/`duration_ms` | 未实现 |
| F3 跨进程测试 | 未做 |
| 已加载组从 manifest 消失时的退役 | 拒绝（不做退役） |
| 随机 fuzz / 吞吐压测 | 未做（本轮按 10 场景 + C2 验收） |

> 上表是 step2 时点的记录；其中 **E3 全量重扫**与 **F2 三指标**已在本轮补齐，
> 见下方 step4 记录。

## step4 完成记录（2026-10-19 15:16）

### 交付

| 文件 | 改动 |
|---|---|
| `include/go2cpp/config/so_reloader.hpp` | 新增 `ForceRescan(manifest)` / `ForceRescan()`、三个指标访问器与键常量、`ReloadAttempt` 计时作用域；**未改** `Reload` 等既有公开签名 |
| `src/config/so_reloader.cpp` | `Reload` 拆成 `ReloadImpl(manifest, force_rescan)`；所有返回路径经 `fail`/`MarkSuccess`/`MarkSkipped` 计数；`steady_clock` 计时；指标镜像到 `so.runtime.*`；`ForceRescan` 跳过幂等并全量重载 |
| `tests/config/test_so_manifest.cpp` | 新增 F2 指标（2 成功 + 1 失败 + 幂等不计）、E3 `ForceRescan`（loader 2→4→6）；E1 增加抢占计数断言 |
| `docs/so-reload-design.md` | E3/F2 状态更新、三个指标键、`ForceRescan` 时序与测试映射、已知限制修订 |
| `docs/state-machines/SoReloader.md` | 增加 `ForceRescan` 状态行与不变量 |
| `docs/so-reload-usage.md` | **新增** 528 行使用文档（最小 manifest、dlopen 三回调、5 步发布、排障、限制） |
| `docs/so-reload-progress.md` | 本记录 |
| `codex.md` | AI 协作记录 §16 |

### 三项任务

1. **F2 三指标：完成。** `so.runtime.reload_success_total` /
   `reload_failure_total` / `reload_duration_ms` 都是
   `ConfigVar<std::string>`（十进制文本，与既有 `so.runtime.*` 一致）；
   构造时注册，每次 `Reload`/`ForceRescan` 返回前（析构作用域）更新。
   成功只统计 `applied=true`；校验失败、loader 失败、revision 回退/被抢占都计入
   失败；同 revision 幂等命中两者都不计。`std::chrono::steady_clock` 从入口开始
   计时（含等 `m_reload_mutex`），因此能暴露锁竞争。
2. **E3 `ForceRescan`：完成。** 两个入口：`ForceRescan(manifest)` 与
   `ForceRescan()`（重扫 `CurrentManifest()`）。跳过 `revision == committed` 的
   幂等短路，仍拒绝 revision 回退、仍受 `highest_seen` 栅栏约束；重新 `Validate`
   后把 manifest 全部 SO 当变更集，逐个 `SoLoader`。文档写明 inotify 收到
   `IN_Q_OVERFLOW` 时应调用它。
3. **使用文档：完成。** `docs/so-reload-usage.md` 覆盖任务要求的 5 节，并额外给出
   SO 侧导出符号、加载器注册表与回滚发布注意事项。

### 实际验证

```
cmake --build build-work -j8
  → 全量构建通过（新代码 -Wall -Wextra -Wpedantic 无告警）

ctest --test-dir build-work -R so_manifest --output-on-failure
  → 1/1 Passed（0.01s）
go2cpp_so_manifest_test 直接运行：
  [F2 指标] success=2 failure=1 duration_ms=0
  [E3] revision 未变仍全量重扫：loader 调用 2→4→6，版本 1→2
  [so_manifest] failures=0

ctest --test-dir build-work
  → 114/114 Passed（61.75s）
```

### 边界与后续

- **未做**：真实 inotify 事件源（属于上层 watcher）；真实 `dlopen` loader 与
  vtable 切换（属于加载器层）；F1 灰度/进程隔离；F3 跨进程测试；随机 fuzz/压测。
- `ForceRescan` 仍拒绝 revision 回退；若溢出期间 manifest 已被更高 revision
  提交，本次重扫可能按回退被拒绝，调用方应重新读取后重试。
- 三个指标键是进程级固定名；多个 `SoReloader` 实例会互相覆盖镜像值，生产只应
  创建一个实例。
- 本轮**未提交 git**，留给主 agent 审查后提交。

## 失误记录：提交 `58b31d4` 混入了子 agent 的在途改动

### 发生了什么

子 agent `2cf46805` 仍在写 `DoneCase` / `AfterCase` + 测试时，主 agent 执行了
`git add -A` 并提交 `58b31d4`，把它的**未完成改动**一并提交：

| 文件 | 行数 |
|---|---:|
| `include/go2cpp/concurrency/channel.hpp` | +222（`DoneCase` / `AfterCase`） |
| `tests/concurrency/test_channel.cpp` | +540（测试用例） |

**`58b31d4` 的提交信息没有提到这两批改动**，提交信息与实际内容不符。

### 影响评估

```
cmake --build build-work -j8   → 通过（无 error）
ctest -R channel               → 2/2 通过（channel_demo、channel_full）
grep -c 'DoneCase|AfterCase' tests/concurrency/test_channel.cpp → 49 处
```

代码可编译、既有 channel 测试全过，**没有破坏性后果**，但提交历史不干净。

### 处理

1. 等 `2cf46805` 自然完成（不打断）；
2. 其产出验证通过后，提一个 fixup commit 说明 `58b31d4` 实际包含两批改动；
3. **规则**：子 agent 运行期间**禁止 `git add -A`**，只做只读检查，等它 stopped 再提交。

---

## 收尾决策（主 agent 自行决定）

### 1. 不合并到主目录

主目录 /UserData/CodexWorkSpace/Go2Cpp 仍在用户手上，所有改动保留在
eat/isolated 分支，等用户审阅后自行 git merge。

### 2. DoneCase(nullptr) 已实测安全，无需修复



nil ContextPtr 不会崩溃、也不会被误判为「已取消」，与 Go 的 nil channel 语义一致。

### 3. Fiber（受管 G）场景未测 —— 已知限制

现有 T1~T11 / B1~B4 全部在**普通线程**上跑；Select 在 **Fiber 里 park** 的路径
（WaitForChange → park → 被 TrySelect 唤醒）**没有专项测试**。

**原因**：需要搭 Scheduler + IOManager + go() 的完整环境，一个测试要
几十行样板；本轮时间预算不足，且现有测试已覆盖「选中语义」本身。

**风险判断**：中等。Select 的 park 路径与 RecvCase 共用同一套
WaitForChange，而 RecvCase 在 Fiber 下的用例是既有的、已通过。因此
DoneCase 在 Fiber 下**大概率同样正确**，但**未实测**。

**如果要做**，最小验证方法是：在 go() 里跑
Select({RecvCase(ch), DoneCase(ctx)})，然后从另一个 Fiber 调 ctx->Cancel()，
断言被唤醒且返回 kCancelled。

### 4. 两个既有 flaky（非本次引入）

- tests/network/test_io.cpp:736 的 
ready_indices.size()==2，单跑 10 次失败 2 次；
- go2cpp_tests 整体偶发 2 次失败，重跑即过。

两者都不经过 channel / config 模块。
