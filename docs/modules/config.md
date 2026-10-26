# 模块文档：配置（config）

> 适用基线：`HEAD 27e892b`（2026-10-26），WSL 工作树 `/UserData/CodexWorkSpace/Go2cpp`。
> 源码：`include/go2cpp/config/`（`config.hpp` 820 行、`so_manifest.hpp` 214 行、
> `so_reloader.hpp` 311 行）、`src/config/`（`config.cpp` 1,325 行、
> `so_manifest.cpp` 669 行、`so_reloader.cpp` 507 行）；CMake 目标 `go2cpp_config`。
> 引用约定：`config.hpp` / `so_manifest.hpp` / `so_reloader.hpp` 指
> `include/go2cpp/config/` 下同名文件；`config.cpp` / `so_manifest.cpp` /
> `so_reloader.cpp` 指 `src/config/` 下实现；`scheduler.hpp` 指
> `include/go2cpp/concurrency/scheduler/scheduler.hpp`。
> 状态：**现行**。本文合并 `docs/configuration.md`（旧文档顶部已标注合并去向，
> 正文保留为历史快照）。合并时修正了两处已漂移的默认值/范围：
> `fiber_stack_size` 默认 2048（旧文档写 0）、下限 2048（旧文档写 16384）；
> `fiber_bin_capacity` 默认 128（旧文档写 32）。以 `scheduler.hpp:206-268` 为准。

## 1. 概述

配置模块分四层（`config.hpp:5-38`）：

| 层 | 类型 | 职责 | 不负责 |
|---|---|---|---|
| 语法 | `IniFile` | `[section]`、`key=value`、`#`/`;` 注释、转义还原 | 类型转换、范围校验、同步 |
| 类型 | `ValueCodec<T>` / `ConfigVar<T>` | 文本↔值、校验器、监听器、事务暂存 | 文件监听 |
| 注册表 | `Config` | 名字表、事务提交、提交回调、跨字段校验器、watcher | 具体业务字段语义 |
| 快照 | `RuntimeConfig` | 调度器/日志/timer 的只读参数集合 + 统一校验 | 在线改队列拓扑 |

此外 `so_manifest.hpp` / `so_reloader.hpp` 是 **SO 热重载的配置层**：manifest 解析与
静态校验（不碰 `dlopen`）+ 两阶段事务编排（`so_manifest.hpp:3-27`、
`so_reloader.hpp:95-121`）。加载器本身由调用方注入。

核心保证是**整批事务**：一次 `LoadFromIni` 中所有变量要么全部写入并广播，要么全部
回滚；监听器在事务锁释放后执行，单个监听器异常不影响其它监听器，也不会回滚已提交
的值（`config.hpp:511-514`）。

### 1.1 最小示例

```cpp
// 启动路径：读文件 → 统一校验 → 应用日志 → 再创建 Scheduler
go2cpp::config::RuntimeConfig config;
std::string error;
if (!go2cpp::config::LoadRuntimeConfig("go2cpp.ini", &config, &error) ||
    !config.ApplyLogging(&error)) {
    // 配置错误时不要启动业务 Scheduler
}
```

```cpp
// 强类型变量 + 监听 + 热更新
auto& registry = go2cpp::config::Config::Instance();
auto workers = registry.Lookup<std::size_t>("scheduler.max_workers", 0,
                                            "最大 M 数量");
workers->AddListener([](const auto&, const auto& current) {
    // 在线刷新业务侧参数；注意不要在这里做长时间阻塞
});
registry.StartWatcher("go2cpp.ini", std::chrono::milliseconds(500));
```

## 2. INI 语法

支持的形式（`config.cpp:427-459`）：

```ini
# 整行注释；分号同样
; 另一种注释
[scheduler]
max_workers = 4        # 行内注释（第一个未转义的 # 或 ; 截断）
log.format = {time} [{level}] {message}\n
```

规则：

- 节标题必须闭合，空节名报错并带行号；键名缺 `=`、空键名同样带行号失败
  （`config.cpp:437-456`）；
- 只按**第一个** `=` 切分，值里可以再出现 `=`（`config.cpp:445-447`）；
- 值在第一个未转义的 `#`/`;` 处截断；字面量 `\n` 还原为换行，`\xHH` 还原为原始
  字节（`config.cpp:110-131`、`419-421`）；
- `Parse` 在临时表上构造，**完全成功才 `swap`**，语法错误不会留下半解析状态
  （`config.cpp:427-458`）；
- `IniFile::Load` 先走稳定快照读取，再 `Parse`（`config.cpp:406-410`）；直接调用
  `Parse` 的调用方需自己保证文本完整（`config.cpp:423-425`）。

## 3. 强类型配置变量

### 3.1 ValueCodec

`ValueCodec<T>` 是文本编解码策略（`config.hpp:105-187`）：

| 类型 | 编码 | 解码 |
|---|---|---|
| 标量（主模板） | `operator<<` | `operator>>`，且必须消费**整个**文本（`stream >> ws` 后要求 eof），否则 `"1x"` 会被误读成 `1` |
| `std::string` | 原样 | 原样 |
| `bool` | `true`/`false` | `1/true/yes/on`、`0/false/no/off`，大小写不敏感 |
| `log::Level` | `log::ToString` | `log::ParseLevel` |
| `std::vector<T>` | 逗号分隔 | 逗号分隔逐项解码，任一项失败整体失败 |

`Decode` 失败不得修改输出参数；`Encode` 的结果必须能被同一 `Decode` 读回
（`config.hpp:112-116`）。

### 3.2 ConfigVar 的读写

- 读取：`GetValue()` 走 `atomic_load(shared_ptr<const T>)`，**不拿锁**、返回副本
  （`config.hpp:283-289`）；
- 写入：`SetValue()` 先在锁外跑校验器，再在锁内比较、原子发布新快照、快照监听器，
  最后在锁外按 ID 顺序回调（`config.hpp:290-327`）；
- 值未变化时直接返回 true，不触发监听器（`config.hpp:310-311`）；
- 监听器异常被吞掉，不阻断其余监听器，也不传播给配置调用方
  （`config.hpp:316-325`）；
- `SetValidator` 设置事务/常规校验器，`SetDirectValidator` 设置仅单值更新用的校验器；
  两者都不会在配置锁内执行（`config.hpp:328-343`）。

### 3.3 两阶段事务

一次热更新必须让模块看到**完整**的新配置，不能看到「新级别 + 旧格式」。因此
`ConfigVar` 暴露三个事务接口（`config.hpp:213-234`、`375-472`）：

| 阶段 | 接口 | 行为 |
|---|---|---|
| 预检 | `ValidateString` | 只转换 + 校验，不改值 |
| 写入 | `ApplyString` | 转换、校验、原子发布新值，把旧快照存进 `m_pending`，**不通知** |
| 发布 | `NotifyPending` | 取走 `m_pending`，在锁外按注册顺序回调监听器 |
| 回滚 | `RollbackPending` | 原子恢复旧快照，**不通知** |

`m_pending` 存在期间，`SetValue`/第二次 `ApplyString` 一律拒绝，避免两个事务交错
覆盖同一变量（`config.hpp:268-271`、`298-309`）。回滚保存的是旧值的 `shared_ptr`
快照，回滚路径无需再次分配 `T`，因此在 `bad_alloc` 下也能确定性恢复
（`config.hpp:474-480`）。

## 4. 配置注册表

`Config::Instance()` 是函数内静态对象，析构时 `StopWatcher()`（`config.hpp:501-505`、
`config.cpp:576-580`）。

| API | 语义 | 引用 |
|---|---|---|
| `Lookup<T>(name, default, desc)` | 查找或创建；名字形如 `module.key`；同名类型冲突返回空 | `config.hpp:540-549` |
| `LookupBase(name)` | 以基类指针查找 | `config.hpp:551-552` |
| `List()` | 全部变量快照（含默认段） | `config.hpp:561-562` |
| `Remove(name)` | **软注销**：不再出现在 List/DumpIni/加载范围；外部 `shared_ptr` 仍可用 | `config.hpp:563-571`、`config.cpp:1239-1242` |
| `Groups()` | 按第一个 `.` 分组，组内 key 字典序 | `config.hpp:573-578`、`config.cpp:1244-1263` |
| `DumpIni()` | 生成带 `#` 注释的 INI；值转义保证可被 `Parse` 逐字节读回 | `config.hpp:580-587`、`config.cpp:1265-1287` |
| `SaveToFile(path)` | 写 `path.tmp` 再 `rename`，读者不会看到半个文件 | `config.hpp:589-596`、`config.cpp:1289-1325` |
| `AddCommitListener` | 一次事务成功后的扩展回调，参数是变更变量列表 | `config.hpp:598-605`、`config.cpp:713-727` |
| `AddTransactionValidator` | 基于**完整候选快照**的跨字段校验器 | `config.hpp:606-609`、`config.cpp:732-745` |

`DumpIni` 的转义集合是：首尾空格、`\`、`#`、`;`、`\n`、`\r`、`\t`、`\0`
（`config.cpp:1186-1213`）；`IniFile::Parse` 对称还原，因此「导出再读回」逐字节一致
（`config.hpp:34-37`）。

## 5. 事务式热加载

`Config::LoadFromIni` 的八步（`config.cpp:583-691`）：

1. 取 `m_transaction_mutex`，预检+写入不被另一个 `LoadFromIni` 交错；
2. 快照全部变量，构造候选值表（未出现在本次 INI 的键用当前值）；
3. 逐变量 `ValidateString`，把通过的「变量 + 待应用文本」放进 pending；
4. 用完整候选快照跑所有 `TransactionValidator`（必须在任何写入之前）；
5. 逐变量 `ApplyString`（只写不通知）；任一失败则逆序 `RollbackPending`；
6. 释放事务锁——监听器可以再次读/提交配置；
7. 对已写入变量统一 `NotifyPending`，模块只能看到完整的新值集合；
8. 执行提交回调；单个回调异常不影响已提交的值，也不终止监听线程。

失败语义：预检/事务校验失败时**没有任何变量被写入**；`ApplyString` 失败时已写变量
全部回滚；通知阶段即使回调抛异常也**不回滚**（值已经对外可见）
（`config.cpp:599-601`）。

## 6. 文件监听与编辑锁

`StartWatcher(path, interval)`（`config.cpp:764-844`）：

1. 生命周期锁保护 `std::thread`，先停旧线程；
2. 启动前检查 `path + ".lock"`：锁文件非空表示仍在编辑，直接失败，不把半文件当
   初始配置（`config.cpp:772-780`）；
3. 初始 `LoadFromFile` 失败不启动线程；
4. 后台线程按 `interval` 轮询，`StopWatcher()` 用条件变量立即唤醒；
5. 锁文件非空时置 `retry_pending`，锁释放后强制重试一次——因为同尺寸的原子
   `rename` 可能不改变 mtime/size（`config.cpp:803-819`）；
6. 加载失败保留旧值并置 `retry_pending`，下一轮继续；监听器异常记录后继续循环，
   不退出线程（`config.cpp:821-841`）。

推荐的写入协议（`docs/configuration.md` 原文，现仍是唯一约定）：

```text
写入 go2cpp.ini.lock（任意非空内容）
写入 go2cpp.ini.tmp
关闭并校验 go2cpp.ini.tmp
rename(go2cpp.ini.tmp, go2cpp.ini)
截断 go2cpp.ini.lock 为空
```

监听方还做连续两次 `FileStamp` + 内容比对，尽量避开「正在被长时间 truncate/write」
的文件；但这不能替代锁文件协议（`config.cpp:340-402`、`config.hpp:29-30`）。

## 7. RuntimeConfig：默认值、范围与校验

`RuntimeConfig` 是启动路径与热更新共用的只读参数快照（`config.hpp:662-696`）。
`FromIni` 在副本上构造，失败不改动传入对象；`Validate` 是最后一道闸；`ApplyLogging`
只改全局 LoggerManager（`config.cpp:485-530`、`532-560`、`562-567`）。

### 7.1 调度器字段

默认值以 `SchedulerConfig` 为准（`scheduler.hpp:206-268`），范围以
`config.cpp:158-211` 与 `Validate` 为准：

| 键 | 默认 | 允许范围 | 说明 |
|---|---:|---|---|
| `processor_count` | 0 | 0 或 1..32 | 0 = 硬件并发数 |
| `min_workers` | 0 | 0 或 ≤32 | 0 = `processor_count`（受 max 封顶） |
| `max_workers` | 0 | 0 或 ≤32，且 ≥ `min_workers` | 0 = 按 P 与阻塞策略计算 |
| `local_queue_limit` | 256 | 1..1048576 | 每个 P 的本地队列 |
| `fiber_stack_size` | 2048 | 0 或 2048..67108864 | 0 = 后端默认；Linux 归一化到 1 页（4 KiB） |
| `task_affinity_budget` | 4 | 0..1048576 | 同类任务优先留在最近 P |
| `fiber_bin_capacity` | 128 | 0..4096 | 每 M 的 FiberBin 容量 |
| `idle_wait_ms` | 10 | 1..86400000（24 h） | 空闲 M 等待周期 |
| `idle_worker_timeout_ms` | 250 | 1..86400000 | 补充 M 的阻塞结束复查周期 |
| `sysmon_interval_ms` | 10 | 1..86400000 | sysmon 检查周期 |
| `long_syscall_threshold_ms` | 50 | 1..86400000 | 已声明阻塞区的长调用阈值 |
| `allow_worker_oversubscription` | true | bool | 阻塞区允许 M 超过 P |
| `enable_sysmon` | true | bool | 阻塞监控开关 |
| `pin_workers_to_cpu` | false | bool | M 绑核；默认关闭以免干扰宿主配额 |
| `collect_metrics` | true | bool | 只关累计指标，不改调度语义 |

注意：`fiber_stack_size` 的下限是 **2048**，不是旧文档写的 16384
（`config.cpp:161-164`、`187-191`）；`fiber_bin_capacity` 默认 **128**，不是旧文档
写的 32（`scheduler.hpp:258`）。

### 7.2 日志与 timer 字段

| 键 | 默认 | 校验 | 引用 |
|---|---|---|---|
| `log.level` | `warn` | `ParseLevel` 合法 | `config.hpp:676`、`config.cpp:506` |
| `log.stdout` | false | bool | `config.hpp:677`、`config.cpp:508` |
| `log.directory` | `log` | 非空 | `config.hpp:678`、`config.cpp:553` |
| `log.file` | `go2cpp.log` | 非空 | `config.hpp:679`、`config.cpp:553` |
| `log.format` | `{time} [{level}] {logger} ({file}:{line}) {message}\n` | 非空 | `config.hpp:680`、`config.cpp:552` |
| `timer.warn_after_days` | 30 | 0..36500，且 ≤ error | `config.hpp:682`、`config.cpp:554-557` |
| `timer.error_after_days` | 365 | 0..36500 | `config.hpp:684`、`config.cpp:554-557` |

`processor_count`/`min_workers`/`max_workers`/`local_queue_limit`/Fiber 栈参数属于
**启动期结构配置**，不是独立热更新参数；已启动的 Scheduler 不应在线改队列拓扑或
P/M 上限（`docs/configuration.md` 原文，当前实现未提供在线重建）。

## 8. 绑定函数

| 函数 | 作用 | 引用 |
|---|---|---|
| `BindLoggingConfig(config, error)` | 把 `log.*` 五项注册为 ConfigVar，并让任一项变化都重建 LoggerManager 配置（一次读全五项，避免中间态） | `config.hpp:645`；`config.cpp:868-900` |
| `BindRuntimeConfig(config, target, error)` | 把 `scheduler.*`/`timer.*`/`log.*` 绑定到 `RuntimeConfig` 目标；先校验目标当前值，类型冲突整体失败 | `config.hpp:649-650`；`config.cpp:919-923` |
| `LoadAndWatch(config, path, interval, error)` | 新手入口 = `BindLoggingConfig` + `StartWatcher` | `config.hpp:655-660` |

生命周期约束：`target` 必须比所有监听器活得久；监听器用裸指针捕获目标字段
（`config.cpp:916-918`）。`BindLoggingConfig` 用 `weak_ptr` 捕获配置变量，避免
`ConfigVar → Listener → ConfigVar` 引用环（`config.cpp:860-866`）。

## 9. 静态注册宏

```cpp
GO2CPP_CONFIG_REGISTER(scheduler, std::size_t, max_workers, 0, "最大 M 数量");
GO2CPP_CONFIG_CANCEL(scheduler, obsolete_key);
```

- 宏用 `__LINE__` 保证同一 `key` 在多处注册时不重名（`config.hpp:791-806`）；
- 注册在**静态初始化期**完成；`AutoConfigRegistrar` 构造期间绝不抛异常（静态期异常
  会直接 terminate），类型冲突时 `registered()==false`（`config.hpp:721-757`）；
- `type` 不能含未加括号的逗号（如 `std::map<int,int>`），否则宏参数被截断，需先
  `using` 别名（`config.hpp:799-801`）；
- `GO2CPP_CONFIG_CANCEL` 只能注销**本翻译单元里已注册过**的项；跨 TU 依赖静态
  初始化顺序，不可靠，跨模块请在运行时调用 `Config::Remove`
  （`config.hpp:808-817`）。

## 10. SO 热重载配置层

`so_manifest.hpp` 只做 manifest 的解析、静态校验与拓扑排序，**不依赖 `dlfcn`**
（`so_manifest.hpp:5-8`）：

- `SoSpec`：组、版本化路径、SHA-256、版本、依赖、符号前缀、保留策略、候选标记
  （`so_manifest.hpp:62-74`）；
- `SoManifest::Parse/LoadFromFile/Validate/BuildLoadOrder`：解析 → 文件/哈希/ABI/
  依赖环/前缀唯一性 → 只对变更集排序（`so_manifest.hpp:138-184`）；
- `SameSoArtifact` 判断同一个 SO 是否需要重新加载（`so_manifest.hpp:212`）；
- `Validate` 只校验 manifest **声明**的 ABI；真实 `go2cpp_so_abi_version()` 必须由
  loader 在 `dlopen` 后比较（`so_manifest.hpp:21-23`）。

`so_reloader.hpp` 是两阶段事务编排器（`so_reloader.hpp:95-121`）：

- 注入 `SoLoader`（`dlopen` + ABI/符号校验）、`SoUnloader`（只回滚本次新加载）、
  `SoActivator`（`NotifyPending` 之后切 vtable，不可回滚）
  （`so_reloader.hpp:46-73`）；
- `Reload(manifest)` 九步：抬 revision 栅栏 → 串行锁 → 静态校验 → 变更集与 A1 依赖
  一致性 → 拓扑加载 → 暂存变量 → 再比 revision → 统一通知 → 激活并提交
  （`so_reloader.hpp:139-157`）；
- `ForceRescan` 用于 inotify `IN_Q_OVERFLOW` 后跳过 revision 幂等、全量重扫
  （`so_reloader.hpp:159-187`）；
- 失败语义：卸载新句柄 → 回滚暂存变量，旧版本完全不参与；`SoActivator` 阶段失败
  不可回滚（`so_reloader.hpp:153-155`）。

接入步骤与运维注意事项见 `docs/so-reload-usage.md`，设计细节见
`docs/so-reload-design.md`；本模块文档只描述配置层边界。

## 11. API 一览

| 层 | API | 位置 |
|---|---|---|
| INI | `IniFile::Load/Parse/Get/Has` | `config.hpp:83-103` |
| 编解码 | `ValueCodec<T>::Encode/Decode`（标量/string/bool/Level/vector 特化） | `config.hpp:117-187` |
| 基类 | `ConfigVarBase::name/description/ToString/ValidateString/FromString/ApplyString/HasPending/NotifyPending/RollbackPending` | `config.hpp:203-239` |
| 变量 | `ConfigVar<T>::GetValue/SetValue/SetValidator/SetDirectValidator/AddListener/DelListener` | `config.hpp:274-373` |
| 注册表 | `Config::Instance/Lookup/LookupBase/LoadFromIni/LoadFromFile/List/Remove/Groups/DumpIni/SaveToFile` | `config.hpp:516-596` |
| 事务扩展 | `AddCommitListener/DelCommitListener/AddTransactionValidator/DelTransactionValidator` | `config.hpp:598-609` |
| 监听 | `StartWatcher/StopWatcher/Watching` | `config.hpp:610-618` |
| 绑定 | `BindLoggingConfig/BindRuntimeConfig/LoadAndWatch` | `config.hpp:641-660` |
| 快照 | `RuntimeConfig::FromIni/Validate/ApplyLogging`、`LoadRuntimeConfig` | `config.hpp:686-701` |
| 名字 | `SplitConfigName/JoinConfigName` | `config.hpp:702-710`；`config.cpp:1160-1175` |
| 注册 | `AutoConfigRegistrar/AutoConfigCanceller`、`GO2CPP_CONFIG_REGISTER/CANCEL` | `config.hpp:716-817` |
| SO | `SoSpec/SoManifest/SoValidationOptions/SameSoArtifact` | `so_manifest.hpp:62-212` |
| SO | `SoReloader/SoReloadResult/SoLoader/SoUnloader/SoActivator` | `so_reloader.hpp:46-157` |

## 12. 设计说明

1. **为什么用两阶段提交**：热更新最危险的中间态是「新级别 + 旧格式」「新 P 数 +
   旧队列上限」。先写全部变量（不通知）再统一通知，模块回调只能看到完整新值集合
   （`config.cpp:639-662`）。
2. **为什么监听器在锁外执行**：回调可能再次读/提交配置；如果持事务锁执行，会与
   下一次 `LoadFromIni` 形成锁反转（`config.cpp:654-657`、`config.hpp:511-514`）。
3. **为什么读取不加锁**：`GetValue()` 拿的是不可变 `shared_ptr` 快照；发布是
   `atomic_store`，读方一次 acquire load 即可，高频读取不阻塞刷新
   （`config.hpp:283-289`、`310-315`）。
4. **为什么 `Remove` 是软注销**：外部可能持有 `shared_ptr`；硬删除会让指针悬空，
   软注销让已持有者继续读写，只是不再参与文件同步（`config.hpp:565-568`）。
5. **为什么导出用 `\xHH`**：INI 没有标准转义；要保证「导出再读回逐字节一致」，
   只能自定义编码并让 `Parse` 对称还原（`config.cpp:1186-1213`、`config.hpp:36-37`）。
6. **为什么监听要 `.lock` + 连续快照**：编辑中的文件可能语法完整但只是前缀；
   锁文件把「写入方还在写」这一事实显式化，连续 `FileStamp` 再兜底
   （`config.cpp:329-402`、`764-819`）。

## 13. 边界与陷阱

- **配置文件是外部输入**：范围上限集中在 `config.cpp:158-167` 的 `kMax*`，各模块
  不要单独放宽（`config.hpp:34-35`）；
- **不要在线改结构参数**：`processor_count`/`min_workers`/`max_workers`/
  `local_queue_limit`/Fiber 栈参数只在创建 Scheduler 时一起校验；已启动后改这些
  字段不会重建队列拓扑；
- **监听器可能并发执行**：不同变量的监听器在不同线程/事务中执行，共享状态要自己
  同步；不要在监听器里长时间阻塞或等待会反向取业务锁的操作
  （`config.hpp:269-272`）；
- **`BindRuntimeConfig` 的 target 生命周期**：目标对象必须活过所有监听器；业务线程
  若同时直接读目标字段，需自备互斥量或在停用 watcher 后读
  （`config.cpp:916-918`）；
- **不要在配置监听回调里再 StartWatcher/StopWatcher**：需要重启时由其它管理线程
  执行（旧文档 §“生命周期约束”，当前实现未加递归保护）；
- **`Lookup<T>` 类型冲突返回空指针**，不是抛异常；调用方必须检查
  （`config.hpp:537-549`）；
- **静态注册的跨 TU 注销不可靠**：`GO2CPP_CONFIG_CANCEL` 只保证本 TU
  （`config.hpp:814-816`）；
- **`DumpIni` 的注释不参与读回**：`RenderComment` 只写 `#` 注释，`Parse` 会丢弃；
  往返一致的是**键值**而不是注释（`config.cpp:1215-1237`）；
- **`Validate` 覆盖不到的约束**：`RuntimeConfig` 只校验单字段/少量跨字段；业务私有
  约束应注册 `AddTransactionValidator`（`config.hpp:526-533`）。

## 14. 测试（对应 ctest）

| ctest | 覆盖 | 源码 |
|---|---|---|
| `go2cpp_config_registry_macro_test` | `GO2CPP_CONFIG_REGISTER/CANCEL`、类型冲突、静态注册期不抛 | `tests/config/test_config_registry_macro.cpp`；`cmake/Go2CppTests.cmake:647-654` |
| `go2cpp_so_manifest_test` | manifest 解析、哈希/ABI/依赖环/前缀、拓扑排序 | `tests/config/test_so_manifest.cpp`；`cmake/Go2CppTests.cmake:663` |
| `go2cpp_tests`（filter `log`） | INI 解析失败不覆盖旧快照、`RuntimeConfig::FromIni/Validate`、日志热加载 | `tests/logging/test_log_config.cpp`；`tests/test_main.cpp:99` |

```bash
ctest --test-dir /tmp/build-ci-sim -R 'config_registry|so_manifest' --output-on-failure
GO2CPP_TEST_FILTER=log /tmp/build-ci-sim/go2cpp_tests
# 2026-10-26 实跑：config_registry_macro_test 0.01 s、so_manifest_test 0.01 s，
# go2cpp_tests（含 log/config 模块）39.48 s，均通过。
```

## 15. 合并说明与变更记录

- 合并 `docs/configuration.md`：INI 语法、调度器参数表与超配实测、日志默认值、
  强类型变量示例、`LoadAndWatch` 新手入口、编辑锁协议、监听器生命周期约束；
  正文保留为历史快照。
- 2026-10-26：按 `HEAD 27e892b` 逐项核对默认值与范围，修正
  `fiber_stack_size`（默认 2048、下限 2048）与 `fiber_bin_capacity`（默认 128）
  两处漂移；补 `ConfigVar` 两阶段事务、`LoadFromIni` 八步、`ReadStableFileContents`
  的连续快照、SO manifest/reloader 配置层边界。
