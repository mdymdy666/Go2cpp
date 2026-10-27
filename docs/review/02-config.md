# 02 config 模块问题文档

> 工作目录/基线：`/UserData/CodexWorkSpace/Go2Cpp` @ `77b405f`（用户指定）。
> 关键文件：`include/go2cpp/config/config.hpp`（818 行）、`src/config/config.cpp`（1232 行）。
> 说明：本目录未包含 SO 热重载子模块（`so_manifest`/`so_reloader`），本轮只审 config 主体。
> 阅读状态：**暂停（2026-10-20）**，待专项审查（见下）
>
> 原因：用户决定 config 后续单独专项审查（问题较多，暂不逐点展开）。
> 已记录：§3 C-01/C-02/C-03；§6 监听器触发策略与 10 个场景；§7 回调 × 事务协作（含
> `m_pending` 二次加锁窗口）。专项审查时从 §7.3 清单开始。


## 1. 模块速览

分层（自底向上）：

```
IniFile（语法层）            config.cpp:346  Parse / :385 Get / :393 Has
  ↓
ConfigVar<T>（单变量事务）    config.hpp:273  ApplyString:380 / NotifyPending:421 / RollbackPending:460
  ↓
Config（注册表 + 整批事务）   config.cpp:527  LoadFromIni / :617 LoadFromFile / :1173 DumpIni / :1196 SaveToFile
  ↓
RuntimeConfig / 绑定          config.cpp:409  FromIni / config.hpp:672 RuntimeConfig
  ↓
Watcher（热加载）             config.cpp:688  StartWatcher / :157 ReadStableFileContents / :193 IsEditLocked
```

- 注册表：`Config::Instance()` 进程级单例；变量按 `name = "module.key"` 注册，`Groups()` 按 module 分组。
- 读取：`ConfigVar<T>::GetValue()` 用 `std::atomic_load` 读不可变 `shared_ptr<const T>`，**读路径不加锁**（config.hpp:285-288）。
- 写入：`SetValue`（单值立即生效）或 `ApplyString`（事务第一阶段，不通知）。

## 2. 问题记录

### Q-001 config 应该按什么顺序读？

- 结论：先 `IniFile::Parse`（只做语法，不碰全局状态）→ 再 `ConfigVar<T>` 的四个方法（单变量状态机）→
  再 `Config::LoadFromIni`（整批事务）→ 最后 `StartWatcher`（文件/锁协议）。不要从 Watcher 倒着读。
- 证据：
  - `IniFile::Parse`：`config.cpp:346-383`（临时 `parsed_sections`，全部成功才 swap）
  - `ConfigVar<T>` 三阶段：`config.hpp:380-471`
  - `Config::LoadFromIni`：`config.cpp:527-615`
  - `StartWatcher`：`config.cpp:688-760`
- 状态：已答

### Q-002 四阶段事务到底怎么保证「整批配置一次发布」？

- 结论：`LoadFromIni` 分四段，核心是**两把锁 + 两个阶段**：
  1. **预检**（持 `m_transaction_mutex`）：对每个已注册变量 `ValidateString`，并收集 `candidates` 快照；
  2. **跨字段校验**：所有 `TransactionValidator` 基于完整 candidates 判断，任一失败则**一个变量都没写**；
  3. **提交**：逐个 `ApplyString`（只写 `m_value`，不触发监听器）；失败则按 `applied` 逆序 `RollbackPending()`；
  4. **发布**：释放事务锁后统一 `NotifyPending()`，再跑 `CommitListener`（异常逐个隔离）。
- 证据：
  - 事务锁覆盖预检与提交：`config.cpp:531`、释放于 `:581`
  - 候选快照 + 预检：`:537-550`；跨字段校验：`:554-562`
  - 回滚栈：`:566-577`；失败逆序回滚：`:571-574`
  - 锁外通知：`:586`；提交回调隔离：`:588-607`
  - `ConfigVar::ApplyString` 先构造 `shared_ptr` 再持锁写 `m_pending`，避免 bad_alloc 后无法回滚：`config.hpp:393-412`
  - `RollbackPending` 只恢复值、不调监听器（事务未发布）：`config.hpp:460-471`
- 风险/建议：
  - 通知阶段在事务锁外，监听器可再次提交配置；因此「一次 LoadFromFile 的监听器看到的值」可能被后一个事务覆盖。
    这是有意的（避免插件回调与事务锁死锁），但调用方不能假设监听器里读到的是本批最终值。
  - `NotifyPending` 复制监听器表失败时会 `m_pending.reset()` 并跳过回调（`config.hpp:430-435`），配置值已提交但通知丢失。
- 状态：已答

### Q-003 热加载的锁文件与「半文件」防护是什么协议？

- 结论：写方按约定使用 `path + ".lock"`（非空 = 正在编辑），并用「临时文件 + rename」原子替换；
  读方 `ReadStableFileContents` 做**两次元数据 + 两次内容快照**，并在读前/读后各检查一次 `.lock`，
  只有全部一致才提交。Watcher 线程再叠加 mtime+size 与 `retry_pending` 兜底同尺寸原子替换。
- 证据：
  - 稳定读：`config.cpp:157-320`（两次 `FileStamp` + 双锁检查，失败重试）
  - `.lock` 只看大小、不读内容：`:186-204`
  - Watcher 循环：`:713-760`；锁非空置 `retry_pending`：`:727-735`；同尺寸替换兜底：`:738-742`
- 风险/建议：
  - `.lock` 是**协作协议**，不遵守的写方仍可能让读方读到半文件（稳定读只能降低概率）。
  - Watcher 用 `mtime + size` 判断变化；若编辑器保留 mtime 且尺寸相同，只能靠 `retry_pending` 或下一次周期。
  - `StartWatcher` 的线程退出/重启由 `m_watcher_lifecycle_mutex` 保护，析构调用 `StopWatcher`；不要在其他线程直接析构 `Config`。
- 状态：已答

### Q-004 已知缺陷/坑（本轮确认）

1. **`DumpIni` 往返注释与实现不一致（P2）**
   - `EscapeIniValue` 注释说「Parse 不做反向转义，读回是字面量 `\n`」（`config.cpp:1104-1107`），
     `DumpIni` 头注释也这么说（`:1178`）；但 `IniFile::Parse` 实际会把字面量 `\n` 还原成换行（`:372-376`）。
   - 实际非往返点是：`\t` / `\r` / `\0` 被转义但 Parse **只还原 `\n`**（`:373` 只 find `"\\n"`）；
     值里含 `#` / `;` 时 Dump 不转义，Parse 会在第一个 `#`/`;` 处截断（`:370-371`）→ **值被静默截断**。
   - 建议：要么 Parse 对称还原 `\t/\r/\0` 并转义 `#;`，要么在 `DumpIni` 文档里明确列出不可往返字符。
2. **同一事务内重复 `ApplyString` 会丢回滚点（P3，边界）**
   - `ApplyString` 发现新值等于当前值时执行 `m_pending.reset()`（`config.hpp:401-405`）。
     若同一变量在同一事务里先被 Apply 成 A（old=O），再 Apply 成 A（等于当前），`m_pending` 被清空；
     事务失败时该变量不在 `applied` 回滚列表里，值会永久停在 A，无法回到 O。
   - `LoadFromIni` 每个变量只 Apply 一次，正常路径不触发；直接调用 `ApplyString` 的代码需避免重复。
3. **事务 pending 期间 `SetValue`/`FromString` 会失败（P3，使用约束）**
   - `SetValue` 在锁内发现 `m_pending` 直接返回 false（`config.hpp:305-308`），错误文本「正在等待上一事务通知」。
     热加载事务窗口内用户直接改同一变量会失败；调用方必须重试或改用 `LoadFromIni`。
4. **`SaveToFile` 没有 fsync（P3，崩溃一致性）**
   - 写 `path + ".tmp"`、`flush`、`std::rename`（`config.cpp:1196-1230`），但没有 `fsync` 文件/目录；
     掉电时可能得到空文件或旧文件（rename 原子性只保证不出现半文件，不保证持久化）。
5. **未注册 key 被静默忽略（P3，可诊断性）**
   - `LoadFromIni` 只遍历已注册变量（`config.cpp:539-550`），INI 里多出来的 key 不会报错；
     拼写错误会表现为「配置没生效」。可加 `strict` 选项或一次性 warning。

## 3. 已确认缺陷

| ID | 严重度 | 位置 | 触发 | 期望/实际 | 状态 |
|---|---|---|---|---|---|
| C-01 | P2 | `config.cpp:1108-1121` vs `:370-376` | 值含 `#`/`;` 或 `\t`，`DumpIni` 后 `LoadFromIni` | 期望往返；实际截断/变字面量 | 未修 |
| C-02 | P3 | `config.hpp:401-405` | 同一事务对同一变量重复 `ApplyString` | 期望回滚到事务前；实际丢 old_value | 未修（边界） |
| C-03 | P3 | `config.cpp:1196-1230` | 掉电/崩溃 | 期望持久化；实际无 fsync | 未修 |

## 4. 待读 / 待确认

- [ ] `RuntimeConfig::FromIni`（`config.cpp:409-446`）与 `ConfigVar` 绑定的关系：快照校验通过后由谁 SetValue？
- [ ] `Config::Bind`/`Lookup`/`Remove` 与 `AutoConfigRegistrar` 的静态注册顺序（跨 TU 初始化顺序）
- [ ] `StartWatcher` 失败重试与 `StopWatcher` 的线程 join 边界
- [ ] SO 子模块：当前目录未包含，暂不纳入本轮

## 5. 关联

- 导读：`docs/code-review-guide.md` §2.1、§5 路线 F
- 文档：`docs/configuration.md`
- 审计：`docs/module-audit-2026-10-20.md`（P2「配置 DumpIni 非往返」；P1 #13 SO 递归不在本目录）

## 6. 监听器触发策略（设计建议 + 配合场景）

> 背景：用户提出「`SetValue` 每次都回调」不合理，希望像定时器一样区分「一直触发 / 触发 N 次 /
> 条件触发 / N 次+条件」。结论：**默认行为不改**（每次值变化都通知）；把触发策略作为
> `ConfigVar::AddListener` 的可选参数，按场景选用。

### 6.1 现状与边界

- `AddListener(Listener)` 只接受 `void(const T& old, const T& now)`，返回 id；`DelListener(id)` 手动注销
  （`config.hpp:344-349`）。
- `SetValue` 值相等直接 `return true`，不回调（`config.hpp:309-310`）。
- `NotifyPending` 只对产生 `pending` 的变量回调（`config.hpp:401-405`、`:421-448`）。
- 因此「每次都回调」的准确含义是：**只要值真的变了，就通知所有监听器**。这是配置作为「状态」的
  正确性要求，不是缺陷。

### 6.2 建议 API（不破坏现有调用）

```cpp
struct ListenerOptions {
  std::size_t max_calls = 0;                          // 0 = 无限
  std::function<bool(const T&, const T&)> predicate;  // 可选，true 才调用
  bool immediate = false;                             // 注册时先按当前值调一次
  std::chrono::milliseconds min_interval{0};          // 可选节流窗口
};
std::uint64_t AddListener(Listener cb, ListenerOptions opt = {});

// 便利包装（可选）
std::uint64_t AddListenerOnce(Listener cb);
std::uint64_t AddListenerFor(std::size_t n, Listener cb);
std::uint64_t AddListenerIf(Predicate pred, Listener cb);
```

`AddListener(cb)` 保持等价于 `AddListener(cb, {})`，老代码零改动。

### 6.3 配合场景

#### S1 状态同步（一直触发，默认）
- 需求：`log.level`、`scheduler.*`、`hook.*` 变化后，模块必须追上最新值。
- 当前写法：`AddListener`，回调里重建 Sink / 更新参数。
- 建议：保持 `AddListener`；**不要**用 `Once`/`For(n)`，否则配置改了模块还停在旧值。
- 注意：回调在事务锁外执行；同一事务里其他变量可能还没轮到通知（值已全部提交，通知按变量顺序）。

#### S2 初始化钩子（`immediate` / `Once`）
- 需求：模块构造时用当前配置初始化一次，之后继续跟随变化。
- 当前写法：先 `GetValue()` 初始化，再 `AddListener`。
- 建议：`AddListener(cb, {.immediate = true})`，注册时先调一次，省掉重复代码。
- 注意：`immediate` 必须在释放 `m_mutex` 后调用（回调里可能再读配置，锁内调用会死锁）；
  注册时可能还是默认值，尚未 `LoadFromFile`，模块要能接受「先默认、后覆盖」。

#### S3 一次性迁移（`Once` + 条件）
- 需求：旧键 `scheduler.threads` 第一次出现时迁移到 `scheduler.processor_count`，只做一次。
- 当前写法：回调里 `static bool migrated` + `DelListener`。
- 建议：`AddListenerOnce` + `predicate`（旧键非空）。
- 注意：迁移里写另一个 `ConfigVar` 会触发它自己的 listener；避免递归/顺序依赖，最好在
  `CommitListener` 里做整批迁移。

#### S4 临时诊断（`For(n)`）
- 需求：排查配置抖动，只观察前 5 次变化，之后自动停止。
- 当前写法：计数器 + `DelListener`。
- 建议：`AddListenerFor(5, cb)`。
- 注意：计数单位是「通知次数」；`LoadFromIni` 失败回滚不会通知、不计数；`SetValue` 与
  `NotifyPending` 必须共用同一套计数逻辑。

#### S5 阈值告警（条件触发）
- 需求：`fiber_stack_size < 16KiB` 时告警，其他变化忽略。
- 当前写法：回调里 `if`。
- 建议：`AddListenerIf([](const auto&, const auto& now) { return now < 16384; }, cb)`。
- 注意：条件只决定「是否调用」，不影响值提交；告警要幂等（配合 `min_interval` 防刷屏）。

#### S6 诊断采样（`N` + 条件）
- 需求：只记录「值增大且超过 1MiB」的变化，最多 3 次。
- 建议：`ListenerOptions{3, [](const auto& old, const auto& now) {
  return now > old && now > (1u << 20); }}`。
- 注意：谓词抛异常按「不通知」处理；计数归零后在锁外注销，本次回调仍会执行完。

#### S7 去抖/节流（条件 + 时间窗）
- 需求：动态限流参数每秒可能改多次，模块最多每 100ms 重配一次。
- 建议：`min_interval = 100ms`，窗口内合并，只保留最后一次值；或调用方自己做 debounce。
- 注意：合并会丢中间值；只适合「最终状态」语义，不适合「每次都要审计」的场景。

#### S8 跨变量联动（用 `CommitListener`，不是 per-var）
- 需求：`min_workers <= max_workers`，任一变化后统一重算。
- 当前写法：两个变量各挂 listener，可能看到「半套」状态。
- 建议：`Config::AddCommitListener`（`config.hpp:601`）在整批 `LoadFromIni` 成功后调用一次；
  非法组合用 `AddTransactionValidator` 在提交前拒绝。
- 注意：`CommitListener` 只在事务路径触发；直接 `SetValue` 不经过事务，需要 per-var listener
  或统一走 `LoadFromIni`。

#### S9 测试等待（`Once`）
- 需求：测试里等配置第一次变更，避免 `sleep` 轮询。
- 建议：`AddListenerOnce` 里 `promise.set_value()`，测试侧 `future.wait_for(timeout)`。
- 注意：超时/失败路径 Once 不会触发，测试必须有超时；测试结束前注销，避免捕获已销毁对象。

#### S10 生命周期 RAII（`ListenerHandle`）
- 需求：回调捕获对象，忘记 `DelListener` 会 UAF。
- 建议：`AddListener` 返回 `ListenerHandle`（RAII，析构自动 `DelListener`），或提供
  `ScopedListener`；id 版保留给需要手动管理的场景。
- 注意：`DelListener` 只从注册表删除，**已在执行的回调仍会跑完**；跨线程销毁对象时要用
  `shared_ptr/weak_ptr` 或外部同步。

### 6.4 策略 × 场景矩阵

| 策略 | 适合 | 不适合 |
|---|---|---|
| 一直触发 | 状态同步（S1）、审计 | 只关心首次/临时观察 |
| `Once` | 初始化（S2）、迁移（S3）、测试等待（S9） | 状态同步（会漂移） |
| `For(n)` | 临时诊断（S4）、采样 | 生产状态同步 |
| 条件 | 阈值告警（S5）、过滤 | 条件依赖其他变量且未走事务 |
| `N` + 条件 | 诊断采样（S6） | 需要完整历史的审计 |
| `immediate` | 晚于配置初始化的模块（S2） | 注册时配置尚未加载、且不能接受默认值 |
| `min_interval` | 抖动参数（S7） | 每次变更都要记录/审计 |

### 6.5 与现有机制的分工

| 机制 | 粒度 | 触发时机 |
|---|---|---|
| `ConfigVar::AddListener` | 单变量 | 该变量每次值变化（`SetValue` 或事务 `NotifyPending`） |
| `Config::AddCommitListener` | 整批事务 | `LoadFromIni` 全部提交成功后一次 |
| `Config::AddTransactionValidator` | 整批事务 | 提交前校验，不通知 |
| `Config::StartWatcher` | 文件 | 文件变化触发 `LoadFromFile` |

### 6.6 实现要点与风险

实现要点：
1. 抽一个 `InvokeListeners(var, old, new)`，`SetValue` 与 `NotifyPending` 共用，避免两套计数。
2. listener 条目存 `{cb, max_calls, remaining, predicate, last_called}`；计数在锁内更新或用原子量。
3. 自动注销在锁外：回调返回后若 `remaining == 0`，再取 `m_mutex` 从 `m_listeners` 删除。
4. `immediate` 在注册锁释放后调用；谓词/回调异常逐个 `try/catch(...)` 隔离。
5. 提供 `ListenerHandle` RAII；`AddListener(cb)` 兼容旧行为。

风险：
- `Once`/`For(n)` 会让模块错过后续配置，必须用命名和文档显式警告；只适合初始化/诊断/迁移。
- 计数只在成功通知时递减；事务失败回滚不通知、不计数。
- 条件依赖其他变量时，通知顺序不保证（值已全部提交，但通知按变量顺序）。
- 节流会丢中间值；需要审计的场景不要用。
- 回调内注册/注销依赖 `NotifyPending` 的快照语义：本次快照里的副本仍会执行。

## 7. 回调 × 事务协作（待专项审查）

> 用户纠正：不是死锁，而是 `NotifyPending` 里 `m_pending.reset()` 的「二次加锁窗口」——
> 中间可能有其他并发单位取走/改写 `m_pending`；回调与事务无关、可能是多次无关回调。
> 后续单独审查。本节只记录审查项，**不改代码**。

### 7.1 二次加锁窗口（用户指出的主风险）

问题不在「catch 再锁会自死锁」，而在 **`m_pending` 的「移走」和「清除」被拆到了两个临界区**：

```cpp
try {
    std::lock_guard lock(m_mutex);      // L1
    if (!m_pending) return;
    listeners = m_listeners;            // 可能抛（std::function 拷贝）
    change = std::move(m_pending);      // optional move 后源仍 engaged
    m_pending.reset();
} catch (...) {
    std::lock_guard lock(m_mutex);      // L2：L1 释放到 L2 之间有一个无锁窗口
    m_pending.reset();                  // 无条件清，可能清掉别人新写的 pending
    return;
}
```

**交错时序（真实风险）**：

1. 线程 A 在 `listeners = m_listeners` 抛 `bad_alloc`，L1 释放，进入 catch 前的窗口；
2. 线程 B 调 `NotifyPending`：拿到锁，把 `m_pending` move 走并 reset，释放锁，开始回调；
3. 线程 C 调 `ApplyString`：看到 `m_pending` 为空，写入**新的** pending，值也写进 `m_value`；
4. 线程 A 的 catch 拿到锁，`m_pending.reset()` 把 C 刚写的 pending 清掉；
5. 结果：C 的值已提交，但永远不会 `NotifyPending`，也没有回滚点；后续 `SetValue` 可能被拒/状态错乱。

另一条更隐蔽：如果异常发生在 `change = std::move(m_pending)`（`T` 移动构造抛），`m_pending` 仍是
engaged 但 `old_value` 已被 move 空、`new_value` 半移动；窗口内另一个 `RollbackPending` 读到空
`old_value`，会 `atomic_store(m_value, nullptr)`，把配置值清空。

**根因**：

- `std::optional` 的 move 不 reset 源，`m_pending` 在 `change = move` 后仍 engaged，其他线程能看到
  「半移动」状态；
- `change` 的取走与 `m_pending.reset()` 不在同一个不可分割操作里；
- catch 无条件 `reset()`，不区分「pending 已被别人取走 / 已被别人新写入」。

**建议（不改代码，审查时定）**：

- `m_pending` 改为 `shared_ptr<PendingChange>`，锁内 `auto change = std::exchange(m_pending, nullptr);`
  （nothrow，一步完成「取走 + 清空」）；
- listener 表改为 `shared_ptr<const map>` 快照，`AddListener/DelListener` 复制-替换（与 Logger 的
  worker 快照同款）；
- `listeners` 复制移到锁外（或放在 pending 清空之后），失败只丢通知；
- catch 不再加锁、不再碰 `m_pending`。

### 7.1.1 次风险：锁内执行用户代码

`std::mutex` 非递归，以下三处在持有 `m_mutex` 时碰用户代码，若用户代码访问同一 `ConfigVar`
会自死锁：

1. `listeners = m_listeners`（`config.hpp:425`）：复制 `std::function`；
2. `change = std::move(m_pending)`（`:428`）：移动 `T`；
3. `catch` 内 `m_pending.reset()`（`:434`）：析构 `T`。

修法与 7.1 同向：锁内只做 `shared_ptr` 交换，把移动/析构/`std::function` 拷贝全部移出锁。

### 7.2 回调与事务的语义缺口

`NotifyPending` 是**变量级**，`LoadFromIni` 是**整批事务**；当前只保证「回调运行时所有变量的
新值已经写入」，**不保证**：

1. 其他变量已经收到通知——通知按变量顺序串行，且通知期间可能有新事务插入；
2. 回调只被本事务触发——回调里调用 `SetValue` 会再触发一轮通知，形成「多次无关回调」；
3. 回调不重入 `LoadFromIni`——`LoadFromIni` 在通知前已释放 `m_transaction_mutex`（`:581`），
   所以回调可以启动新事务，两个事务的通知可能交错。

后果：监听器可能看到「值是新批、其他变量的通知是旧批/还没到」的混合状态；跨变量联动
（如 `min_workers <= max_workers`）如果写成 per-var 回调，很容易读到半套状态。

一个具体的悬挂场景：回调里对**同一变量**调用 `ApplyString`，会写入新的 `m_pending`；
但本事务的 `applied` 已经遍历过该变量，不会再对它 `NotifyPending`。这个 pending 会
一直悬挂，后续 `SetValue` 被拒绝（`config.hpp:305-308`），直到下一次事务碰巧通知它。

### 7.3 审查清单（后续专项）

- [ ] 是否把所有用户代码移出 `m_mutex`：`listeners` 改为 `shared_ptr<const map>` 原子快照，
      `PendingChange` 改为 `shared_ptr<const T>`（避免锁内移动/析构 `T`）
- [ ] `catch` 路径的 `m_pending.reset()` 是否也应移到锁外；`T` 析构抛异常的处理
- [ ] 回调内对同一变量/其他变量调用 `SetValue/ApplyString/NotifyPending` 的重入规则
- [ ] 事务通知期间插入新事务的顺序保证（是否需要 `BatchView`/批次序号）
- [ ] 是否需要「通知队列」串行化（单线程 dispatcher），把回调与事务线程解耦
- [ ] listener 快照复制失败时「丢通知 + 清 pending」是否可接受（与「配置已提交」的一致性）
- [ ] catch 路径不得无条件 `m_pending.reset()`；`m_pending` 是否改为 `shared_ptr` + `exchange`
- [ ] listener 快照是否改为 `shared_ptr<const map>`，避免锁内复制 `std::function`

## 8. 裸 `std::mutex` 与 park 的交互（高危，专项优先）

> 用户补充：config 默认用 `std::mutex`，**如果回调产生 park，很危险**。这一条优先级高于 §7.3。

### 8.1 事实

- `ConfigVar<T>::m_mutex` 是裸 `std::mutex`（`config.hpp:480`）；
  `Config::m_mutex`（`:622`）与 `m_transaction_mutex`（`:625`）也是 `std::mutex`；
  典型回调消费者 `LoggerManager::m_mutex` 同样是 `std::mutex`（`log.hpp:786`）。
- runtime 的「持锁 park」检测只覆盖 `sync::Mutex`：`sync::Mutex::LockImpl` 调
  `Task::note_mutex_locked()`（`sync.cpp:1025`）维护 `m_held_mutexes`，
  `park_for_scheduler()` 在 `scheduler.cpp:584` 检查它并告警。**裸 `std::mutex` 完全不在这套计数里**，
  持锁 park 既不会 abort，也不会 `mutex_held_park_total()` 计数。

### 8.2 危险路径

1. **锁内用户代码**（与 §7.1.1 同源）：`listeners = m_listeners` 复制 `std::function`（`:425`）、
   `change = std::move(m_pending)` 移动 `T`（`:428`）、`catch` 内 `m_pending.reset()` 析构 `T`（`:434`）。
   只要 `T` 或 listener 的拷贝/移动/析构里发生 park，就是**持 `ConfigVar::m_mutex` park**。
2. **回调链上的模块锁**：`NotifyPending`/`SetValue` 的回调本身在锁外（`config.hpp:437-447`、
   `:315-324`），但回调经常调用 `LoggerManager::Configure`（`config.cpp:814` / `:488`）。
   `Configure` 持 `LoggerManager::m_mutex`（`log.cpp:836-848`）做 `create_directories` + `ofstream`
   打开文件；普通文件会**阻塞 M**，如果路径落在网络 FS、FIFO、或被 Hook 的 fd 上，就可能 park，
   于是变成**持 `LoggerManager::m_mutex` park**。
3. **回调里的配置查询**：回调中调用 `Config::Lookup/List/Groups/Add*`（持 `Config::m_mutex`，`:622`）
   之后再执行任何阻塞/可 park 操作，就是持注册表锁 park。

### 8.3 后果

协作式调度下 `std::mutex::lock` 阻塞的是 worker 线程（M），不是 Fiber：

- 持锁 G park 后，其他 G 在同一 `std::mutex` 上会卡住 M；若持锁 G 的唤醒又依赖这个 M，
  就是真正的死锁；即使不死，也表现为 worker 被占死、吞吐骤降。
- 因为不在 `m_held_mutexes` 里，**没有告警、没有计数、没有 debug abort**；排查只能靠 gdb 看
  `std::mutex` 的 owner 和 `pthread_mutex` 状态。

### 8.4 建议（不改代码，专项时定）

- config/log 内部锁改用 `sync::Mutex`（允许 park，且进入检测）或 `SpinMutex`（禁止 park，
  debug 直接 abort）；至少不要继续用「裸 std::mutex + 可能 park 的回调」组合。
- `LoggerManager::Configure` 把 `FileSink` 构造移到锁外：先在锁外 `make_defaults()`，再锁内
  替换 `m_workers`（可参考 `LogItemWorker` 的复制-替换快照）。
- 明确契约并写进文档：`T` 的拷贝/移动/析构、listener 回调**不得 park/阻塞**；debug 下加断言。
- listener 回调投递到专用 dispatcher 线程/队列，与配置线程、worker 线程解耦。
- 项目级：给裸 `std::mutex` 加持锁 park 检测（instrumentation 包装），否则同类问题会反复出现。
