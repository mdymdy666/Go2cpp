# SO 热重载：进度与审查记录

> 工作目录：`/UserData/CodexWorkSpace/Go2Cpp-work`（`feat/isolated` 分支 + `build-work/`）
> 主目录 `/UserData/CodexWorkSpace/Go2Cpp` 属于用户，本项目所有改动**不触碰它**。

## 阶段与状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| **step1** | 17 个工业级场景/边界 → `docs/so-reload-scenarios.md` | ✅ **完成**（`ddb8344`） |
| **step2** | config 分层扩展设计 + 实现 → `docs/so-reload-design.md`、`so_manifest.*`、`so_reloader.*` | ✅ **完成**（`acca90d`） |
| **step3** | 11 个场景测试全部通过；原 113 个测试无回归 | ✅ **完成**（**ctest 114/114**） |
| step4 | 使用文档与剩余限制的收口 | 🔄 进行中 |

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
