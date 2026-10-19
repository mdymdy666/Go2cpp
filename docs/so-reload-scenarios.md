# SO 热重载：工业级场景与边界清单

> 工作目录：`/UserData/CodexWorkSpace/Go2Cpp-work`
> 关联：`docs/so-reload-progress.md`（审查基准）、`include/go2cpp/config/config.hpp`（既有原语）
> 约定：每个场景都写明「复用哪个 config 原语 / 还是需要新增」；不确定的标 **需调研**。

---

## A. 依赖与加载顺序

### A1. SO 依赖 SO，被依赖者先更新

- **触发**：`A.so` 依赖 `B.so`；运维只替换了 `B.so`，没动 `A.so`。
- **失败模式**：`A.so` 的 GOT/PLT 在**首次调用**时已绑定到旧 `B.so` 的地址，新 `B.so` 虽然被加载，`A` 仍走旧地址 → **两个版本同时在用**，行为不可预测；若旧 `B.so` 被 `dlclose` 则 `A` 跳进已卸载内存（段错误）。
- **工业界解法**：**同一依赖组必须整体切换**（Envoy 的 `xDS` 把 cluster+listener 作为同一快照提交；Kubernetes 的 ConfigMap+Deployment 一起滚动）。
- **config 需要**：新增「依赖组」概念——同一组内的 SO 在一个事务里整体生效（可复用 `NotifyPending` 的两阶段）。
- **测试模拟**：造 `A.so`→`B.so`，只替换 `B`，断言「事务被拒绝，`A` 仍用旧 `B`」。

### A2. 循环依赖

- **触发**：`A.so`↔`B.so` 互相 `dlsym`。
- **失败模式**：`dlopen` 返回成功但符号未就绪，首次调用段错误。
- **工业界解法**：**禁止循环**（构建期检查）；运行期用拓扑排序。
- **config 需要**：新增「依赖图校验」，环则拒绝整个事务。
- **测试模拟**：构造环，断言加载期就失败而非运行期崩溃。

### A3. `RTLD_GLOBAL` 符号污染

- **触发**：两个 SO 导出同名符号，后加载者用 `RTLD_GLOBAL` 覆盖前者。
- **失败模式**：**旧版本函数被新符号劫持**，且无任何报错。
- **工业界解法**：一律 **`RTLD_LOCAL` + 显式 `dlsym` 函数指针**，不依赖全局符号；符号加版本前缀（如 `foo_v2_`）。
- **config 需要**：新增「符号前缀/命名空间」配置项（`so.<name>.symbol_prefix`）。
- **测试模拟**：两个 SO 同名符号，断言只有显式 `dlsym` 的那个被调用。

---

## B. 配置组织

### B1. 分层监听 vs 统一生成配置

- **触发**：10 个 SO 各自一个 ini（多 `inotify`），还是单文件 `so-manifest.ini`（原子替换）。
- **失败模式**：分层监听 → **部分更新**（3 个 SO 换了、7 个没换的中间态）；统一文件 → **单点写入竞争**（多个运维同时改）。
- **工业界解法**：**统一生成配置 + 原子替换**（`rename()`），由发布系统生成，不给人工直接编辑（Envoy 的 `xDS` 快照、Nginx 的 `nginx.conf` 由配置管理生成）。
- **config 需要**：复用 `StartWatcher`（已支持稳定快照 + `.lock`）；新增「manifest 版本号」用于幂等判断。
- **测试模拟**：写一半文件再 `rename`，断言 watcher 只在 `IN_CLOSE_WRITE`/`IN_MOVED_TO` 后触发一次。

### B2. 配置与 SO 文件不一致

- **触发**：manifest 指向 `libfoo.so` 但文件不存在 / 内容是旧的 / SHA256 不匹配 / ABI 版本号不符。
- **失败模式**：`dlopen` 成功但加载了**错误版本**（最危险：静默错误）。
- **工业界解法**：**三重校验**——路径存在 + 内容哈希 + ABI 版本号（`extern "C" const char* go2cpp_abi_version()`）；三者任一不符即拒绝。
- **config 需要**：新增 `so.<name>.sha256`、`so.<name>.abi_version`、`so.<name>.path`。
- **测试模拟**：篡改文件内容但保留路径，断言哈希校验拒绝。

### B3. 配置先改还是文件先换

- **触发**：先写 manifest（指向新文件）再替换 SO，或反过来。
- **失败模式**：先改 manifest → 窗口期内指向不存在的文件；先换文件 → 窗口期内 manifest 还指向旧路径（若旧文件被覆盖则读到新内容但版本号是旧的）。
- **工业界解法**：**版本化文件名 + 先落地后切换**：`libfoo-v2.so` 先 `rename` 到最终位置，再原子替换 manifest（两者都在同一文件系统，`rename` 是原子的）。
- **config 需要**：`so.<name>.path` 必须是**带版本的文件名**，而不是固定 `libfoo.so`。
- **测试模拟**：按「先文件后配置」顺序，在中间态采样，断言不会 `dlopen` 到半个文件。

---

## C. 在途调用

### C1. 正在执行旧 SO 函数的线程

- **触发**：线程 A 正在 `foo()` 里，主线程热更。
- **失败模式**：若 `dlclose` 旧 SO → A 下一秒跳进已卸载代码段（SIGSEGV）。
- **工业界解法**：**永不 `dlclose`**（或 `RTLD_NODELETE`）；旧 SO 常驻，靠引用计数/租约在「最后一个在途调用返回」后标记可回收（glibc 的 `dlclose` 只在引用计数归零时卸载，但 Go/C++ 运行时无法保证没有残留）。
- **config 需要**：新增 `so.<name>.retire_policy = {never, refcount}`（默认 `never`）。
- **测试模拟**：在旧 `foo()` 里 sleep，热更后断言旧 SO 仍可调用，且新调用走新版本。

### C2. 新旧共存期的 vtable 原子替换

- **触发**：一组函数指针（`init`/`handle`/`fini`）必须一起切换。
- **失败模式**：读者看到「新 `init` + 旧 `handle`」的半更新状态。
- **工业界解法**：**不可变 vtable + `std::atomic<const VTable*>` 指针替换**（RCU 风格）；读者 `load(acquire)` 一次拿到完整版本。
- **config 需要**：无需新增（vtable 属于加载器层），但**切换点必须由 `NotifyPending` 之后触发**。
- **测试模拟**：多线程持续调用，热更时断言每次调用看到的 vtable 都是完整的一代（版本号一致）。

---

## D. 失败与回滚

### D1. 新 SO 加载失败

- **触发**：`dlopen` 返回 null（缺依赖）、缺必需符号、ABI 版本不符。
- **失败模式**：若先卸旧再装新 → **服务中断**。
- **工业界解法**：**先装新、验证通过、再切指针、最后才考虑退役旧**；任何一步失败都保持旧版本服务。
- **config 需要**：复用 `ApplyString`（预检不生效）+ `RollbackPending`。
- **测试模拟**：造一个缺符号的 SO，断言旧版本仍在服务且错误被上报。

### D2. 部分 SO 成功、部分失败

- **触发**：manifest 里 5 个 SO，3 个成功、2 个失败。
- **失败模式**：**状态不一致**（组合了互不兼容的新旧版本）。
- **工业界解法**：**全或全无**（事务）；已加载的新版本也要回退到旧（不能只卸一半）。
- **config 需要**：复用 `NotifyPending` 的「全部成功后统一发布」，失败则 `RollbackPending`。
- **测试模拟**：5 个里第 4 个故意失败，断言前 3 个也没生效。

### D3. 回滚到旧 SO

- **触发**：新版本运行后出问题，要回到上一版。
- **失败模式**：旧文件已被覆盖 → **无版本可回**。
- **工业界解法**：**版本化文件 + 保留 N 份**（如 `libfoo-v1.so`/`v2.so`/`v3.so` 并存，manifest 指回 `v2`）。
- **config 需要**：新增 `so.<name>.keep_versions`（默认 3）。
- **测试模拟**：热更到 v2 后回滚到 v1，断言 manifest 与运行版本都回到 v1。

---

## E. 并发

### E1. 多线程同时触发热更

- **触发**：两个运维/两个 watcher 同时改 manifest。
- **失败模式**：重复 `dlopen`（同一 SO 加载两次，浪费内存且可能符号冲突）；vtable 竞争。
- **工业界解法**：**单写者 + 版本号幂等**（manifest 带单调递增 `revision`，旧 revision 直接丢弃）。
- **config 需要**：复用 `ConfigVar` 的 `m_pending` 互斥（同一时刻只允许一个事务）。
- **测试模拟**：并发写 10 次不同 revision，断言只有最大 revision 生效、只 `dlopen` 一次。

### E2. 文件正在被写入时被 `dlopen`

- **触发**：`cp libfoo.so libfoo.so`（直接写，不是 `rename`）。
- **失败模式**：`dlopen` 读到**半个文件** → `dlopen` 失败或更糟：加载成功但代码段被截断 → 运行时段错误。
- **工业界解法**：**发布流程只允许 `rename`**（同文件系统内原子）；`inotify` 监听 `IN_CLOSE_WRITE` 而不是 `IN_MODIFY`。
- **config 需要**：复用 `StartWatcher` 的「稳定快照」（已有：等待文件大小/mtime 稳定）。
- **测试模拟**：一边慢写一边触发热更，断言要么读到完整旧版、要么读到完整新版。

### E3. `inotify` 事件重复/丢失/队列溢出

- **触发**：`IN_MODIFY` 连续触发；或 `/proc/sys/fs/inotify/max_queued_events` 溢出得到 `IN_Q_OVERFLOW`。
- **失败模式**：重复热更（幂等性不足）；或**完全丢失一次更新**（最危险：以为已更新，实际没有）。
- **工业界解法**：**事件只作为「去抖信号」，真实状态靠轮询/对比**（manifest 的 revision/hash）；收到 `IN_Q_OVERFLOW` 时强制全量重扫。
- **config 需要**：复用 `StartWatcher`；新增「溢出后全量重扫」路径。
- **测试模拟**：注入 `IN_Q_OVERFLOW`，断言触发全量重扫而不是静默跳过。

---

## F. 隔离与可观测

### F1. 新 SO 首次调用就崩溃

- **触发**：新版本有野指针/未定义行为，第一次 `handle()` 就 SIGSEGV。
- **失败模式**：**整个进程崩**（新旧一起死）。
- **工业界解法**：**没有进程内隔离手段**——只能靠「灰度/金丝雀 + 快速回滚」；或把插件放进**独立进程**（gRPC 插件模型）。
- **config 需要**：新增 `so.<name>.candidate`（先只让 1% 流量走新版本）。
- **测试模拟**：模拟一段会崩的代码路径，断言（在测试中）能被捕获并回滚，且记录崩溃标记。

### F2. 热更缺少可观测指标

- **触发**：出问题后无法回答「现在跑的是哪个版本、上次热更何时、成功没」。
- **失败模式**：排障靠猜。
- **工业界解法**：暴露 `current_version`、`last_reload_at`、`reload_success_total`、`reload_failure_total`、`reload_duration_ms`。
- **config 需要**：新增只读 section `so.<name>.runtime.*`（由加载器回写，`DumpIni` 可导出）。
- **测试模拟**：热更后读这些键，断言与实际版本一致。

### F3. 多进程共享同一 SO 文件

- **触发**：`fork` 前加载 SO，或两个进程用同一路径。
- **失败模式**：一个进程热更（`rename` 新文件）后，另一个进程的映射还是**旧 inode**；两者版本不同却以为一致。
- **工业界解法**：**每进程独立加载 + 每进程上报自己的版本**（不要假设跨进程一致）。
- **config 需要**：`so.<name>.runtime.loaded_path`（含 inode 或 hash），可用于跨进程比对。
- **测试模拟**：两进程分别加载，`rename` 替换后断言各自记录自己的版本。

---

## 统计

| 类别 | 场景数 |
|---|---:|
| A 依赖与顺序 | 3 |
| B 配置组织 | 3 |
| C 在途调用 | 2 |
| D 失败与回滚 | 3 |
| E 并发 | 3 |
| F 隔离与可观测 | 3 |
| **合计** | **17** |

**标「需调研」的**：无（未使用不确定的库名或论文；`RTLD_NODELETE`、`rename` 原子性、`IN_Q_OVERFLOW`、`SO_REUSEPORT` 均为 POSIX/Linux 既有语义）。

## config 原语复用对照

| 场景 | 复用 | 需新增 |
|---|---|---|
| A1/A2 | `NotifyPending`/`RollbackPending` | `so.group`（依赖组） |
| A3 | — | `so.<name>.symbol_prefix` |
| B1 | `StartWatcher` | manifest `revision` |
| B2 | `ApplyString` 的校验阶段 | `so.<name>.sha256` / `abi_version` |
| B3 | `StartWatcher` | 版本化 `path` 约定 |
| C1/C2 | `NotifyPending` 之后切换 | vtable 层（不在 config） |
| D1/D2 | 两阶段事务全套 | — |
| D3 | — | `so.<name>.keep_versions` |
| E1 | `m_pending` 互斥 | — |
| E2 | `StartWatcher` 稳定快照 | — |
| E3 | `StartWatcher` | 溢出后全量重扫 |
| F1 | — | `so.<name>.candidate` |
| F2 | — | `so.<name>.runtime.*` |
| F3 | — | `so.<name>.runtime.loaded_path` |
