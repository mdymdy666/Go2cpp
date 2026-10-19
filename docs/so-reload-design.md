# SO 热重载：配置层设计

> 工作目录：`/UserData/CodexWorkSpace/Go2Cpp-work`
> 需求来源：`docs/so-reload-scenarios.md`（17 个场景）
> 审查基准：`docs/so-reload-progress.md`
> 约束：**不修改** `include/go2cpp/config/config.hpp` 与 `src/config/config.cpp`
> 的既有公开接口签名；新能力全部放在新文件里，通过组合接入 `Config`。

---

## 1. 目标与非目标

### 1.1 本层要解决的问题

把「一份 SO manifest 的变更」变成一次 **可预检、可回滚、可观测** 的配置事务：

1. 从 INI 解析「组 → SO 列表 → 路径 / SHA-256 / 版本 / 依赖 / 符号前缀」；
2. 在**不接触 `dlopen`** 的前提下完成静态校验（文件存在、哈希、声明 ABI 版本、
   依赖环、符号前缀冲突）；
3. 由调用方注入 loader 完成真正的 `dlopen` + `dlsym` + 符号级 ABI 校验；
4. 全部 SO 加载成功后才统一发布 `ConfigVar` 的运行时观测值，最后才允许
   加载器层切换 vtable；
5. 任一步失败就回滚本事务内写入的 `ConfigVar`，并卸载**本事务刚加载的**
   SO；**旧版本永不因失败事务被卸载**。

### 1.2 关键设计判断

`Config` 已经提供了 SO 热重载需要的两阶段事务原语，本层**不重复实现**：

| 既有原语 | 位置 | 本层用法 |
|---|---|---|
| `ConfigVar::ApplyString()` | `include/go2cpp/config/config.hpp:221` | 第一阶段：写运行时观测值，不通知监听器 |
| `ConfigVar::HasPending()` | `config.hpp:223` | 判断该变量是否真的产生了待发布变更 |
| `ConfigVar::NotifyPending()` | `config.hpp:227` | 第二阶段：全部 SO 加载成功、revision 栅栏通过后统一发布 |
| `ConfigVar::RollbackPending()` | `config.hpp:230` | 失败或更高 revision 抢占时恢复旧值 |
| `Config::LoadFromIni()` 的「预检 → 写入 → 广播」 | `src/config/config.cpp:527` | 本层复刻同样的三段顺序，但不改它的实现 |
| `Config::StartWatcher()` | `config.hpp:611` | B1/B3/E2/E3 的稳定快照 + `.lock` 协议，直接复用，不另写监听器 |
| `ConfigVar<T>::AddListener()` | `config.hpp:344` | 运行时观测值发布后的业务回调（如指标上报） |

### 1.3 本层明确**不做**的事

- **不实现 `dlopen`/`dlsym`/`dlclose`**：这是加载器层的职责。本层只定义
  `SoLoader` / `SoUnloader` / `SoActivator` 三个回调契约；
- **不实现 vtable**：不保存函数指针、不做原子指针替换。本层只保证
  `SoActivator` 在 `NotifyPending()` **之后**被调用；
- **不做流量灰度**：`candidate` 只被解析和透传，1% 分流属于业务/网关层；
- **不做进程隔离**：F1 的进程内崩溃本层无法兜底，独立进程插件模型不在范围内；
- **不实现 inotify**：B1/B3/E2/E3 复用 `Config::StartWatcher` 的轮询 + 稳定快照；
  `IN_Q_OVERFLOW` 的强制全量重扫不在本层；
- **不删除旧 SO**：C1 的 `retire_policy` 只解析、不执行；本层只维护
  「旧版本路径保留清单」，真正的 `dlclose` 由加载器在引用计数归零后决定；
- **不接管 `Config` 的私有事务锁**：`Config::m_transaction_mutex` 不可见，本层
  用 `SoReloader::m_reload_mutex` 串行化自己，并依赖每个 `ConfigVar` 的
  `m_pending` 与外部 `LoadFromIni` 互相排斥。

---

## 2. 分层结构

```
┌──────────────────────────────────────────────────────────────┐
│ 业务层：流量灰度、指标上报、崩溃重启                           │
└───────────────────────────────┬──────────────────────────────┘
                                │ SoActivator（NotifyPending 之后）
┌───────────────────────────────▼──────────────────────────────┐
│ 配置层（本设计）                                               │
│  SoManifest：解析 + 静态校验                                   │
│  SoReloader：revision 栅栏 + 两阶段事务 + 保留清单              │
│  运行时观测值：so.<name>.runtime.* → ConfigVar<std::string>     │
└───────────────────────────────┬──────────────────────────────┘
                                │ SoLoader / SoUnloader（回调注入）
┌───────────────────────────────▼──────────────────────────────┐
│ 加载器层：dlopen(RTLD_LOCAL) + dlsym + 符号级 ABI 校验 + vtable │
└──────────────────────────────────────────────────────────────┘
```

### 2.1 新增文件

| 文件 | 职责 | 不依赖 |
|---|---|---|
| `include/go2cpp/config/so_manifest.hpp` | `SoSpec`/`SoManifest`/`SoValidationOptions`/`SoDiagnostic` 的公共契约；解析、静态校验、拓扑排序、文件 SHA-256 | `dlfcn.h`、网络、调度器实现 |
| `src/config/so_manifest.cpp` | 解析 `[so]` / `[so.group.*]` / `[so.<name>]`；文件存在性、SHA-256、声明 ABI、依赖环、符号前缀冲突 | 同上 |
| `include/go2cpp/config/so_reloader.hpp` | `SoLoader`/`SoUnloader`/`SoActivator` 回调类型；`SoReloader` 与 `SoReloadResult` | 加载器实现 |
| `src/config/so_reloader.cpp` | revision 幂等/栅栏、变更集与依赖一致性、两阶段 `ConfigVar` 事务、失败卸载、保留清单 | `dlopen` |
| `tests/config/test_so_manifest.cpp` | 用假 loader 覆盖 A1/A2/A3/B2/B3/D1/D2/D3/E1/F2（另含 C2 顺序验证） | 真实 `.so` |
| `docs/state-machines/SoReloader.md` | 事务状态转化表 | — |

### 2.2 修改的构建文件

| 文件 | 改动 |
|---|---|
| `cmake/Go2CppModules.cmake` | `go2cpp_config` 增加 `src/config/so_manifest.cpp`、`src/config/so_reloader.cpp`；链接 `go2cpp_hash`（复用既有 SHA-256，不重复实现） |
| `cmake/Go2CppTests.cmake` | 新增 `go2cpp_so_manifest_test`，使 `ctest -R so_manifest` 可命中 |

**没有修改** 的既有文件：`include/go2cpp/config/config.hpp`、`src/config/config.cpp`、
`include/go2cpp/config.hpp`。`SoReloader` 通过构造函数接收 `Config&` 并组合
`ConfigVarBase::ptr`，没有继承任何既有类型。

---

## 3. manifest 键与 INI 结构

### 3.1 示例

```ini
# 全局段：一个文件一个 revision，发布系统整体原子替换
[so]
revision=7
abi_version=go2cpp-abi-v1
keep_versions=3
groups=edge,storage
sos=libedge,libedge_codec,libstore

# 组：members 必须与各 SO 的 group 字段互相印证
[so.group.edge]
members=libedge,libedge_codec

[so.group.storage]
members=libstore

# 一个 SO 一段；节名必须同时出现在 [so] 的 sos 列表里
[so.libedge]
group=edge
version=2
path=/opt/go2cpp/so/libedge-v2.so
sha256=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
abi_version=go2cpp-abi-v1
symbol_prefix=edge_
dependencies=libedge_codec
keep_versions=3
retire_policy=never
candidate=false

[so.libedge_codec]
group=edge
version=2
path=/opt/go2cpp/so/libedge_codec-v2.so
sha256=60303ae22b998861bce3b28f33eec1be758a213c86c93c076dbe9f558c11c752
abi_version=go2cpp-abi-v1
symbol_prefix=edge_codec_
dependencies=

[so.libstore]
group=storage
version=1
path=/opt/go2cpp/so/libstore-v1.so
sha256=...
abi_version=go2cpp-abi-v1
symbol_prefix=store_
dependencies=
```

### 3.2 字段语义

| 段/键 | 必填 | 语义 | 校验 |
|---|---:|---|---|
| `[so] revision` | 是 | manifest 单调递增版本号 | 必须 > 0；回退拒绝，相等幂等 |
| `[so] abi_version` | 否 | 本文件声明的 ABI 字符串，默认 `go2cpp-abi-v1` | 非空，且必须与宿主期望值一致 |
| `[so] keep_versions` | 否 | 每个 SO 保留的版本路径数，默认 3 | 必须 ≥ 1 |
| `[so] groups` | 是 | 组名列表（逗号分隔） | 非空、无重复、名字合法 |
| `[so] sos` | 是 | SO 名列表（逗号分隔） | 非空、无重复、名字合法 |
| `[so.group.<g>] members` | 是 | 组成员列表 | 非空；成员必须存在于 `sos`；成员只能属于一个组 |
| `[so.<name>] group` | 是 | 所属组 | 必须与 `[so.group.*]` 的 members 双向一致 |
| `[so.<name>] path` | 是 | **带版本号的文件名** | 文件存在、是普通文件、basename 含版本 token |
| `[so.<name>] version` | 是 | 版本标识（如 `2`、`v2`） | 非空；必须出现在 `path` 的文件名里 |
| `[so.<name>] sha256` | 是 | 文件内容的 SHA-256（64 位十六进制） | 格式合法，且与实际内容哈希一致 |
| `[so.<name>] abi_version` | 否 | 该 SO 声明的 ABI 字符串 | 非空，且必须等于 `[so] abi_version` |
| `[so.<name>] dependencies` | 否 | 同组依赖（逗号分隔） | 依赖存在、无自依赖、无环、必须同组 |
| `[so.<name>] symbol_prefix` | 是 | `RTLD_LOCAL` + 显式 `dlsym` 的符号前缀 | 非空；全局不允许两个 SO 使用同一前缀 |
| `[so.<name>] keep_versions` | 否 | 覆盖全局保留数 | 必须 ≥ 1 |
| `[so.<name>] retire_policy` | 否 | `never` / `refcount`，默认 `never` | 只解析，不在本层执行 |
| `[so.<name>] candidate` | 否 | 是否候选版本，默认 false | 只解析；流量灰度不在本层 |

### 3.3 运行时观测键（由 `SoReloader` 回写）

| 键 | 写入时机 | 用途 |
|---|---|---|
| `so.runtime.revision` | 每次成功提交 | 当前 manifest revision |
| `so.runtime.last_reload_at` | 每次成功提交 | Unix 秒 |
| `so.<name>.runtime.current_version` | 该 SO 成功提交 | F2：当前版本 |
| `so.<name>.runtime.loaded_path` | 该 SO 成功提交 | F3：本进程实际加载路径（含版本） |
| `so.<name>.runtime.loaded_sha256` | 该 SO 成功提交 | F3：跨进程比对内容 |
| `so.<name>.runtime.abi_version` | 该 SO 成功提交 | 排障 |
| `so.<name>.runtime.loaded_at` | 该 SO 成功提交 | 排障 |

这些键都是普通 `ConfigVar<std::string>`，因此 `DumpIni()`、`StartWatcher()`、
`AddListener()` 全部可直接复用。它们**只在事务提交阶段发布**；失败事务不会
留下新值。

---

## 4. 17 个场景逐条对照

| 场景 | 对应键 | 校验规则 / 处理位置 | 本版状态 |
|---|---|---|---|
| **A1** 依赖组里只更新被依赖者 | `group`、`dependencies`、`version`、`path`、`sha256` | `SoReloader::Reload` 计算变更集后检查：若 B 变更，所有依赖 B 的 A 也必须在本次变更集里，否则拒绝整次事务且**不调用 loader** | ✅ 实现，测试覆盖 |
| **A2** 循环依赖 | `dependencies` | `SoManifest::Validate` 对依赖图做三色 DFS；发现回边即失败，错误信息带环路径 | ✅ 实现，测试覆盖 |
| **A3** 两个 SO 同名符号前缀 | `symbol_prefix` | `SoManifest::Validate` 收集所有非空前缀，重复即失败 | ✅ 实现，测试覆盖 |
| **B1** 统一生成配置 + 原子替换 | `revision`、`groups`、`sos` | manifest 必须带 `revision`；同一 revision 幂等。文件写入仍由发布系统 `rename`；监听复用 `Config::StartWatcher` | ⚠️ 解析/幂等已实现；watcher 为既有能力，本层未新增 |
| **B2** 配置与 SO 文件不一致 | `sha256`、`abi_version`、`path` | 静态阶段：路径存在 + 内容 SHA-256 + 声明 ABI 与宿主一致；符号级 ABI 由 loader 校验 | ✅ 静态部分实现，哈希测试覆盖 |
| **B3** 配置先改还是文件先换 | `path`、`version` | `path` 必须是带 `version` token 的文件名；`Validate` 同时要求文件已存在，因此「manifest 先指过去」会被拒绝 | ✅ 实现，测试覆盖 |
| **C1** 在途调用旧 SO | `retire_policy` | 只解析并透传；本层**从不卸载当前/旧版本**，失败事务也只卸载本次新加载的句柄 | ⚠️ 语义成立，策略执行在加载器层 |
| **C2** vtable 原子替换 | —（加载器层） | `SoReloader` 保证 `SoActivator` 在全部 `NotifyPending()` 之后调用；切换本身由加载器做单次原子指针替换 | ✅ 顺序契约实现；额外测试覆盖顺序 |
| **D1** 新 SO 加载失败 | —（事务） | loader 返回 false → 已加载的新 SO 逆序 `SoUnloader` → 已暂存 `ConfigVar` 逆序 `RollbackPending` → 旧版本保持服务 | ✅ 实现，测试覆盖 |
| **D2** 部分成功、部分失败 | —（事务） | 与 D1 同一事务；任一步失败全量回滚，**不是只回滚失败的那个** | ✅ 实现，测试覆盖 |
| **D3** 回滚到旧 SO | `keep_versions`、`path` | 每次成功提交把 `path` 追加到保留清单，按 `keep_versions` 裁剪；清单只记录，不卸载旧文件 | ✅ 实现，测试覆盖 |
| **E1** 多线程同时热更 | `revision` | `HighestSeenRevision` 在进入串行区**之前**原子抬高；提交前再次比较；旧 revision 即使已加载完成也会被回滚，只有最大 revision 能提交 | ✅ 实现，测试覆盖 |
| **E2** 文件写入中被 dlopen | `path` | 依赖既有 `Config::StartWatcher` 的稳定快照 + `.lock`；本层要求版本化文件名，进一步避免覆盖写 | ⚠️ 复用既有能力，未新增测试 |
| **E3** inotify 重复/丢失/溢出 | `revision` | 事件只当去抖信号，真实状态靠 revision/hash 对比；`IN_Q_OVERFLOW` 强制全量重扫在 watcher 层 | ❌ 溢出重扫未实现 |
| **F1** 新 SO 首调崩溃 | `candidate` | 只解析/透传；进程内隔离与灰度不在本层 | ❌ 仅保留配置位 |
| **F2** 可观测指标 | `so.<name>.runtime.*`、`so.runtime.*` | 事务提交时回写 `current_version` / `loaded_path` / `loaded_sha256` / `abi_version` / `loaded_at` / `revision` / `last_reload_at` | ⚠️ 部分实现；成功/失败计数与耗时未实现 |
| **F3** 多进程共享同一 SO | `so.<name>.runtime.loaded_path`、`loaded_sha256` | 每进程独立 `SoReloader`，各自回写自己的路径与哈希；不假设跨进程一致 | ⚠️ 观测键已实现；跨进程测试未做 |

### 4.1 变更集规则（A1/D2/E1 的共同基础）

1. **组的成员集合变化**（新增/删除成员）→ 该组所有成员都进入变更集；
2. **单个 SO 的 artifact 身份变化**（`group` / `path` / `sha256` / `version` /
   `abi_version` / `symbol_prefix` / 排序后的 `dependencies`）→ 该 SO 进入变更集；
3. 仅 `revision`、`keep_versions`、`retire_policy`、`candidate` 变化**不触发**
   `dlopen`，只提交新的观测值；
4. 已加载的组或 SO **从 manifest 中消失** → 拒绝（本层不负责退役，避免静默
   卸载旧版本）；
5. 变更集内若 B 被 A 依赖，A 也必须在变更集里，否则拒绝（A1）。

### 4.2 依赖顺序

`SoManifest::BuildLoadOrder` 只对**变更集内**的依赖边做拓扑排序；未变更的
依赖视为「已就绪的旧版本」。环在 `Validate` 已经拒绝，这里再防御一次。
同一层内按名字排序，保证测试和日志可重复。

---

## 5. 事务时序（文字版）

```
调用方                SoReloader                     ConfigVar             Loader 层
  │                      │                              │                     │
  │ Reload(manifest R)   │                              │                     │
  ├─────────────────────>│                              │                     │
  │                      │ 1. highest_seen = max(...,R) │                     │
  │                      │ 2. 取 m_reload_mutex（串行）  │                     │
  │                      │ 3. R < committed  → 拒绝      │                     │
  │                      │    R == committed → 幂等返回   │                     │
  │                      │    R < highest_seen → 丢弃旧事务│                    │
  │                      │ 4. SoManifest::Validate       │                     │
  │                      │    文件存在 / SHA-256 / 声明ABI │                     │
  │                      │    / 依赖环 / 前缀冲突          │                     │
  │                      │ 5. 计算变更集 + A1 依赖一致性   │                     │
  │                      │ 6. 拓扑排序                    │                     │
  │                      │ 7. 逐个加载 ───────────────────┼────────────────────>│
  │                      │                              │      dlopen+dlsym     │
  │                      │<─────────────────────────────┼──── true/false         │
  │                      │ 8. 写 runtime ConfigVar       │                     │
  │                      │    ApplyString（不通知）──────>│ 值已替换，pending=1   │
  │                      │ 9. 再次比较 highest_seen       │                     │
  │                      │    有更高 R → 回滚 + 卸载新SO  │                     │
  │                      │ 10. 全部成功 → NotifyPending ─>│ 监听器看到完整一代    │
  │                      │ 11. SoActivator（vtable 原子切换）──────────────────>│
  │                      │ 12. 提交 current_ / 保留清单   │                     │
  │<─────────────────────┤ ok=true, applied=true         │                     │
```

任一步失败：

```
loader false / ApplyString false / 更高 revision 抢占
        │
        ├─> 对本次已成功加载的 SO 逆序调用 SoUnloader（只卸新句柄）
        ├─> 对本次已 ApplyString 的 ConfigVar 逆序 RollbackPending
        ├─> 不调用 SoActivator
        ├─> current_ 与 retention 不变
        └─> 返回 ok=false，旧版本继续服务
```

### 5.1 并发与失败语义

- **串行化**：同一 `SoReloader` 的 `Reload` 由 `m_reload_mutex` 串行；
- **revision 栅栏**：`m_highest_seen` 是 `std::atomic<std::uint64_t>`，在取
  `m_reload_mutex` **之前** `fetch_max`。因此一个正在加载的旧 revision 会在
  提交前看到更大的值并主动回滚；后到的旧 revision 会在进入串行区后立即被
  丢弃；
- **幂等**：`R == committed_revision` 直接返回 `ok=true, applied=false`，
  不重复 `dlopen`；
- **旧版本安全**：无论哪条失败路径，都**不会**对 `current_` 里的旧句柄调用
  `SoUnloader`；`SoUnloader` 只接受本次 `loaded` 列表里的句柄；
- **激活不可失败**：`SoActivator` 在 `NotifyPending` 之后执行，此时配置值
  已经对外可见，本层无法回滚。契约要求它只做一次原子指针替换，异常被捕获
  并忽略；加载器层的实现必须保证不抛异常；
- **与外部 `LoadFromIni` 的竞争**：本层不持有 `Config::m_transaction_mutex`，
  若另一个线程恰好在同一批变量上做配置事务，`ApplyString` 会因 `m_pending`
  返回 false；本层按失败事务处理。这是有意的「宁可失败，不交叉覆盖」。

---

## 6. ABI 版本校验的接口约定

### 6.1 SO 必须导出的两个符号

```cpp
extern "C" const char* go2cpp_so_abi_version();
extern "C" const char* go2cpp_so_name();
```

| 符号 | 返回 | 校验方 | 缺失/不匹配时 |
|---|---|---|---|
| `go2cpp_so_abi_version` | 形如 `go2cpp-abi-v1` 的静态字符串 | **loader 层**（`dlsym` 后比较） | loader 返回 `false` + 中文诊断；本层回滚整个事务 |
| `go2cpp_so_name` | 与 manifest `[so.<name>]` 节名一致的静态字符串 | **loader 层** | 同上；防止路径/哈希正确但文件被换错 |
| `sha256` | manifest 声明值 | **配置层静态校验** | `Validate` 失败，不进入 loader |
| `abi_version` | manifest 声明值 | **配置层静态校验** | 与 `[so] abi_version` / 宿主期望值不一致即失败 |

### 6.2 为什么静态层只能校验「声明的 ABI 字符串」

`go2cpp_so_abi_version()` 在 `.so` 内部，只有 `dlopen` 之后才能读；配置层
不引入 `<dlfcn.h>`，否则就破坏了「配置层不接触加载」的分层。因此：

- `SoManifest::Validate()` 校验 **manifest 里写的** `abi_version`：
  - 非空；
  - 等于 `[so] abi_version`；
  - 等于 `SoValidationOptions::host_abi_version`（默认 `kSoAbiVersion`）。
- `SoLoader` 的实现**必须**在 `dlopen` 成功后：
  1. `dlsym(handle, "go2cpp_so_abi_version")`；
  2. 若为空 → `false`，诊断写「缺少 go2cpp_so_abi_version」；
  3. 与 `spec.abi_version` 逐字节比较 → 不等则 `false`；
  4. 同样校验 `go2cpp_so_name` 与 `spec.name`；
  5. 任一步失败时，loader **自己负责** `dlclose` 刚拿到的句柄，再返回 false
     （本层只会对「返回 true 的 spec」调用 `SoUnloader`）。

### 6.3 建议的 loader 实现骨架（不属于本层）

```cpp
bool RealLoader(const go2cpp::config::SoSpec& spec, std::string* error) {
    void* handle = ::dlopen(spec.path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) { /* error */ return false; }
    auto abi = reinterpret_cast<const char* (*)()>(
        ::dlsym(handle, "go2cpp_so_abi_version"));
    auto name = reinterpret_cast<const char* (*)()>(
        ::dlsym(handle, "go2cpp_so_name"));
    if (!abi || !name || spec.abi_version != abi() || spec.name != name()) {
        ::dlclose(handle);
        return false;
    }
    // 句柄交给加载器层登记；vtable 在 SoActivator 里原子替换
    return true;
}
```

`RTLD_NOW | RTLD_LOCAL` 是硬约定：缺符号在 `dlopen` 就暴露；局部符号避免
A3 的全局污染。

---

## 7. 接入点与调用顺序

```cpp
go2cpp::config::Config& cfg = go2cpp::config::Config::Instance();

go2cpp::config::SoReloader reloader(
    cfg,
    RealLoader,                       // SoLoader：dlopen + dlsym + ABI 校验
    RealUnloader,                     // SoUnloader：失败回滚时卸新句柄
    SwitchVTable);                    // SoActivator：NotifyPending 之后原子切换

go2cpp::config::SoManifest manifest;
std::string error;
if (!go2cpp::config::SoManifest::LoadFromFile("/etc/go2cpp/so-manifest.ini",
                                              &manifest, &error)) {
    // 语法/读取失败，未触碰任何 SO
}
const auto result = reloader.Reload(manifest);
if (!result.ok) {
    // 旧版本仍在服务；result.error 是首条诊断
}
// F2：读取当前版本
auto version = cfg.Lookup<std::string>(
    reloader.RuntimeVersionKey("libedge"), "", "SO 当前版本")->GetValue();
```

`StartWatcher` 仍由上层调用（`Config::StartWatcher` 只认识 `ConfigVar`，不认识
manifest）。推荐流程：

1. 启动时 `SoManifest::LoadFromFile` + `reloader.Reload`；
2. 用 `Config::StartWatcher` 监听 manifest 文件（复用稳定快照 + `.lock`）；
3. 在 manifest 相关 `ConfigVar` 的监听器里读取完整 manifest 并调用 `Reload`；
4. 发布系统先 `rename` 新 `.so`，再原子替换 manifest（B3）。

---

## 8. 测试映射

| 用例 | 断言 |
|---|---|
| A1 | 只把依赖者 B 从 v1 换到 v2，A 不变 → `Reload` 失败，loader 调用次数不增加，A/B 运行时版本仍是 1 |
| A2 | A↔B 循环依赖 → `Validate` 返回 false，错误含环 |
| A3 | 两个 SO 都声明 `dup_` → `Validate` 返回 false |
| B2 | manifest 哈希为全 0，文件内容非空 → `Validate` 返回 false |
| B3 | 版本化路径指向不存在的 `libb3-v2.so` → `Validate` 返回 false |
| D1 | 第二个 SO 的 loader 返回 false → 第一个 SO 被 `SoUnloader` 回滚，运行时版本不变，activator 0 次 |
| D2 | 5 个 SO 第 4 个 loader 失败 → 3 个已加载的全部卸载，5 个运行时版本全部保持旧值 |
| D3 | `keep_versions=3`，依次 v1..v4 → v1 在 v2 时仍在清单；v4 后清单为 `[v2,v3,v4]` |
| E1 | 旧 revision 正在 loader 中阻塞，新 revision 到达 → 旧事务回滚并卸载自己加载的 SO，只有新 revision 提交 |
| F2 | 热更后 `so.f2so.runtime.current_version` 可读且等于 manifest 的 `version`；`so.runtime.revision` 等于 manifest revision |

所有加载器都是假回调，不编译/不加载真实 `.so`。

---

## 9. 已知限制（诚实清单）

1. **E3**：`IN_Q_OVERFLOW` 强制全量重扫未实现，仍依赖 `Config::StartWatcher`
   的轮询与时间戳/大小比较；
2. **F1**：`candidate` 只是配置位，没有灰度、没有崩溃捕获；
3. **F2**：`reload_success_total`/`reload_failure_total`/`reload_duration_ms`
   未实现；当前只有 `revision`、`last_reload_at`、每 SO 版本/路径/哈希/时间；
4. **F3**：跨进程版本比对没有测试；`loaded_path`/`loaded_sha256` 已可供外部
   比对，但本层不假设多进程一致；
5. **C1**：`retire_policy` 不执行；旧 `.so` 文件与旧句柄都由加载器层管理；
6. **移除语义**：已加载的组/SO 一旦从 manifest 消失，本层直接拒绝；真正的
   下线需要加载器层先确认无在途调用，再调用 `SoUnloader`（不在本事务内）；
7. **跨组事务**：一次 `Reload` 对 manifest 内所有变更组做全或全无；如果需要
   按组独立提交，应使用不同的 `SoReloader` 实例或拆成多个 revision；
8. **路径**：不强制绝对路径；生产环境建议绝对路径，避免工作目录变化导致
   哈希/加载到不同文件。
9. **INI 值字符**：`IniFile::Parse` 在第一个 `#`/`;` 处截断值，因此路径中若含这两个字符会被截断；生产路径应避免。
