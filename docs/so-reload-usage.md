# SO 热重载使用指南

> 面向：接入 Go2Cpp 配置层、需要「不重启进程替换 `.so`」的开发者与运维。
> 设计依据：`docs/so-reload-design.md`（分层、17 场景、事务时序）。
> 场景清单：`docs/so-reload-scenarios.md`。
> 本文只讲**怎么用**；`SoReloader` 的内部状态机见 `docs/state-machines/SoReloader.md`。

`SoReloader` 负责的是**事务编排与可观测性**：revision 栅栏、manifest 静态校验、
两阶段 `ConfigVar` 提交、失败回滚、运行时指标。它**不包含** `dlopen`、不保存
vtable、不做流量灰度。真正加载代码需要你注入三个回调：

| 回调 | 时机 | 你负责什么 |
|---|---|---|
| `SoLoader` | 静态校验通过、提交之前 | `dlopen` + `dlsym` + 符号级 ABI 校验；失败时自己 `dlclose` |
| `SoUnloader` | 事务失败回滚 | 只卸载**本次刚加载**的句柄；旧版本句柄永远不会传进来 |
| `SoActivator` | 全部 `NotifyPending` 之后 | 一次原子指针替换；不阻塞、不失败 |

---

## 1. 最小可用示例

### 1.1 目录约定

```text
/opt/go2cpp/so/
  libedge-v2.so           # 版本化文件名：必须包含 version token
  libedge_codec-v2.so
/etc/go2cpp/
  so-manifest.ini         # 发布系统整体原子替换这一个文件
```

生产环境建议：SO 目录对运行进程**只读**；manifest 与 SO 在同一台机器同一文件系统；
不要用「覆盖写同一个 `.so` 路径」的方式发布（会破坏哈希校验与 B3 的前提）。

### 1.2 一份完整的 `so-manifest.ini`

两个 SO、一个依赖组，`libedge` 依赖 `libedge_codec`，因此它们必须同组、同一次事务升级：

```ini
# 全局段：一个文件一个 revision；发布系统用 rename 原子替换
[so]
revision=7
abi_version=go2cpp-abi-v1
keep_versions=3
groups=edge
sos=libedge,libedge_codec

# 组：members 必须与各 SO 的 group 字段双向一致
[so.group.edge]
members=libedge,libedge_codec

[so.libedge]
group=edge
version=2
path=/opt/go2cpp/so/libedge-v2.so
# 示例值；实际必须用 `sha256sum` 计算并替换
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
# 示例值；实际必须用 `sha256sum` 计算并替换
sha256=60303ae22b998861bce3b28f33eec1be758a213c86c93c076dbe9f558c11c752
abi_version=go2cpp-abi-v1
symbol_prefix=edge_codec_
dependencies=
```

要点：

- `path` 的文件名必须包含 `version`（`libedge-v2.so` 包含 `2`），且文件必须已存在；
- `sha256` 是文件**内容**的 64 位小写十六进制，不是文件名/版本的哈希；
- 每个 SO 必须导出 `go2cpp_so_abi_version()` 与 `go2cpp_so_name()`，返回静态字符串；
- `symbol_prefix` 全局唯一；加载器用 `RTLD_LOCAL` + 显式 `dlsym`，避免符号互相覆盖；
- 同一个组里只升级被依赖者、不升级依赖者会被 A1 检查拒绝（GOT/PLT 仍指向旧实现）。

### 1.3 先只做静态校验（不加载）

发布前可以单独跑校验，不接触 `dlopen`：

```cpp
#include "go2cpp/config/so_manifest.hpp"

go2cpp::config::SoManifest manifest;
std::string error;
if (!go2cpp::config::SoManifest::LoadFromFile(
        "/etc/go2cpp/so-manifest.ini", &manifest, &error)) {
    // INI 语法/读取失败；此时没有任何 SO 被加载
    return 1;
}
std::vector<go2cpp::config::SoDiagnostic> diagnostics;
if (!manifest.Validate(go2cpp::config::SoValidationOptions{},
                       &diagnostics, &error)) {
    // 文件不存在、哈希不符、ABI 不一致、依赖环、前缀冲突……
    for (const auto& item : diagnostics) {
        std::fprintf(stderr, "%s: %s\n", item.scope.c_str(),
                     item.message.c_str());
    }
    return 1;
}
```

---

## 2. 如何接入：三个回调的真实实现

> 下面的代码是**加载器层示例**，不属于配置层的公开契约；复制到你的项目后按业务
> 的 vtable 结构替换。示例假设 Linux/ELF 与 `dlopen`。

### 2.1 先约定 SO 侧导出什么

配置层只强制两个符号：

```cpp
extern "C" const char* go2cpp_so_abi_version();  // 必须返回 "go2cpp-abi-v1"
extern "C" const char* go2cpp_so_name();         // 必须返回 manifest 的节名，如 "libedge"
```

vtable 由加载器层自行约定。示例增加一个：

```cpp
struct ModuleVTable {
    int (*handle)(const char* request, std::string* response);
};

extern "C" const ModuleVTable* go2cpp_so_vtable();  // 加载器层约定，不是配置层要求
```

### 2.2 加载器侧的注册表

`SoReloader` 保证 `SoLoader`/`SoUnloader`/`SoActivator` 在同一事务的串行区内调用，
因此 `pending`/`retired` 只被发布线程访问；业务线程只读原子 vtable 槽位。

```cpp
#include <dlfcn.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "go2cpp/config/so_manifest.hpp"
#include "go2cpp/config/so_reloader.hpp"

namespace {

struct ModuleVTable;  // 你的函数表，见 2.1

struct PendingSo {
    void* handle{nullptr};
    const ModuleVTable* vtable{nullptr};
};

struct ModuleSlot {
    // 业务线程用 acquire 读这个指针；激活时 release 写。
    std::atomic<const ModuleVTable*> vtable{nullptr};
    void* active_handle{nullptr};  // 只由发布线程读写
};

// 启动、任何 Reload 之前建好；之后只读，不再增删。
std::map<std::string, std::unique_ptr<ModuleSlot>> g_slots;
// 以下两个只由 SoReloader 的发布线程访问（事务内串行）。
std::map<std::string, PendingSo> g_pending;
std::vector<void*> g_retired_handles;

void EnsureSlots(const std::vector<std::string>& names) {
    for (const auto& name : names) {
        g_slots.emplace(name, std::make_unique<ModuleSlot>());
    }
}

const ModuleVTable* CurrentVTable(const std::string& name) {
    const auto it = g_slots.find(name);
    if (it == g_slots.end()) return nullptr;
    return it->second->vtable.load(std::memory_order_acquire);
}

}  // namespace
```

### 2.3 `SoLoader`：加载、校验、登记 pending

```cpp
bool LoadSo(const go2cpp::config::SoSpec& spec, std::string* error) {
    // RTLD_NOW：缺符号立刻失败；RTLD_LOCAL：避免 A3 的全局符号污染。
    void* handle = ::dlopen(spec.path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (error) *error = std::string("dlopen 失败: ") + ::dlerror();
        return false;
    }

    const auto abi = reinterpret_cast<const char* (*)()>(
        ::dlsym(handle, "go2cpp_so_abi_version"));
    const auto name = reinterpret_cast<const char* (*)()>(
        ::dlsym(handle, "go2cpp_so_name"));
    const auto vtable = reinterpret_cast<const ModuleVTable* (*)()>(
        ::dlsym(handle, "go2cpp_so_vtable"));

    if (!abi || !name || !vtable) {
        if (error) *error = "SO 缺少 go2cpp_so_abi_version/go2cpp_so_name/vtable";
        ::dlclose(handle);  // 本回调失败时必须自己清理，配置层不会收到这个句柄
        return false;
    }
    if (spec.abi_version != abi()) {
        if (error) *error = "SO 实际 ABI 与 manifest 不一致";
        ::dlclose(handle);
        return false;
    }
    if (spec.name != name()) {
        if (error) *error = "SO 实际名字与 manifest 节名不一致";
        ::dlclose(handle);
        return false;
    }
    if (g_slots.find(spec.name) == g_slots.end()) {
        if (error) *error = "加载器未为该 SO 预留 ModuleSlot";
        ::dlclose(handle);
        return false;
    }

    // 只登记 pending；此时配置值还没有 NotifyPending，业务仍在使用旧 vtable。
    g_pending[spec.name] = PendingSo{handle, vtable()};
    return true;
}
```

### 2.4 `SoUnloader`：只回滚本次新加载的句柄

```cpp
void UnloadSo(const go2cpp::config::SoSpec& spec) {
    const auto it = g_pending.find(spec.name);
    if (it == g_pending.end()) return;  // 不是本次加载的，绝不碰旧版本
    void* handle = it->second.handle;
    g_pending.erase(it);
    if (handle) ::dlclose(handle);
}
```

`SoUnloader` 只会收到本次 `SoLoader` 返回 true 的 spec，顺序与加载相反。**旧版本
句柄永远不会经过它**，所以这里不需要、也不允许按名字去 `dlclose` `g_slots` 里的
`active_handle`。

### 2.5 `SoActivator`：一次原子指针替换

```cpp
void ActivateSo(const go2cpp::config::SoSpec& spec) {
    const auto slot_it = g_slots.find(spec.name);
    const auto pending_it = g_pending.find(spec.name);
    if (slot_it == g_slots.end() || pending_it == g_pending.end()) return;

    ModuleSlot& slot = *slot_it->second;
    // 旧句柄先退役但不 dlclose：在途调用可能仍持有旧 vtable 的函数指针。
    if (slot.active_handle) g_retired_handles.push_back(slot.active_handle);
    slot.active_handle = pending_it->second.handle;
    slot.vtable.store(pending_it->second.vtable, std::memory_order_release);
    g_pending.erase(pending_it);
}
```

契约要求 `SoActivator` 只做原子指针替换、不阻塞、不失败。上面的实现没有锁、
没有 IO，也没有 `dlclose`；旧句柄进入 `g_retired_handles`，等加载器层确认没有
在途调用后再回收（`retire_policy=refcount` 属于加载器层，配置层不执行）。

### 2.6 接线

```cpp
int main() {
    auto& config = go2cpp::config::Config::Instance();
    EnsureSlots({"libedge", "libedge_codec"});

    go2cpp::config::SoReloader reloader(config, LoadSo, UnloadSo, ActivateSo);

    go2cpp::config::SoManifest manifest;
    std::string error;
    if (!go2cpp::config::SoManifest::LoadFromFile(
            "/etc/go2cpp/so-manifest.ini", &manifest, &error)) {
        std::fprintf(stderr, "读取 manifest 失败: %s\n", error.c_str());
        return 1;
    }
    const auto result = reloader.Reload(manifest);
    if (!result.ok) {
        // 旧版本仍在服务；result.error 是首条中文诊断
        std::fprintf(stderr, "首次加载失败: %s\n", result.error.c_str());
        return 1;
    }

    // 业务热路径：不加锁读原子 vtable
    const ModuleVTable* edge = CurrentVTable("libedge");
    if (edge && edge->handle) edge->handle("ping", nullptr);
    return 0;
}
```

接入检查清单：

- [ ] `g_slots` 在**任何** `Reload` 之前建好，之后不再增删（否则业务线程读到悬空指针）；
- [ ] `SoLoader` 失败时自己 `dlclose`，只对返回 true 的 spec 登记 pending；
- [ ] `SoUnloader` 只处理 `g_pending`，不碰 `active_handle`；
- [ ] `SoActivator` 只做原子 store，不 `dlclose`、不等待、不抛异常；
- [ ] 业务线程通过 `acquire` 读 vtable，发布线程通过 `release` 写；
- [ ] 进程内只创建一个 `SoReloader`（运行指标键是进程级固定名）。

---

## 3. 发布流程：从构建到原子替换

下面 5 步的顺序不能颠倒。核心原则：**先让新文件在最终路径可用，再原子替换
manifest；旧文件在 `keep_versions` 窗口内不要删。**

### 第 1 步：构建带版本号的新 SO

```bash
# 产物名必须让 Validate 能在 basename 里找到 version token
cmake --build build --target edge_plugin -j8
cp build/libedge.so /tmp/release/libedge-v2.so

# 确认三个符号都在（前两个是配置层要求，第三个是本示例的加载器约定）
nm -D /tmp/release/libedge-v2.so | grep -E 'go2cpp_so_(abi_version|name|vtable)'
```

同一个 SO 的 `version`、文件名、manifest 的 `version` 三者必须一致；不要复用
`libedge-v2.so` 这个文件名去装不同内容——哈希会变，而旧进程可能仍映射着旧 inode。

### 第 2 步：计算内容哈希

```bash
sha256sum /tmp/release/libedge-v2.so
# 把输出的 64 位小写十六进制填进 [so.libedge] sha256
```

不要手抄、不要用 `md5sum`；`Validate` 会在每次 Reload 时重新计算并逐字节比较。

### 第 3 步：生成新 revision 的 manifest

```bash
REV=$(( $(grep -E '^revision=' /etc/go2cpp/so-manifest.ini | cut -d= -f2) + 1 ))
sed -e "s/^revision=.*/revision=${REV}/" \
    -e "s#^path=.*libedge-v2.so#path=/opt/go2cpp/so/libedge-v2.so#" \
    /etc/go2cpp/so-manifest.ini > /tmp/so-manifest.ini.new
```

revision **必须严格递增**；相等会被当成幂等（不重载），更小会被拒绝。只改
`keep_versions`/`retire_policy`/`candidate` 不会触发 `dlopen`，但会提交新的观测值。

### 第 4 步：原子替换（先文件、后 manifest）

```bash
# 1) 新 SO 先放到最终版本化路径；不覆盖旧版本文件
install -m 0644 /tmp/release/libedge-v2.so /opt/go2cpp/so/libedge-v2.so

# 2) 用最终路径做预检（Validate 会检查文件存在 + 哈希）
./so-manifest-check /tmp/so-manifest.ini.new

# 3) 同一文件系统内 rename，读者要么看到旧文件、要么看到新文件
mv /tmp/so-manifest.ini.new /etc/go2cpp/so-manifest.ini
```

如果使用 `Config::StartWatcher` 的 `.lock` 协议：写入期间把
`/etc/go2cpp/so-manifest.ini.lock` 写成**非空**，watcher 会跳过；写完后清空或删除
锁文件，watcher 会在下一轮强制重试一次。`mv` 本身是原子的，锁只是给「边写边读」
的编辑器场景兜底。

### 第 5 步：触发 Reload 并确认

```cpp
// 你自己的 manifest 监听回调（轮询或 inotify）
go2cpp::config::SoManifest next;
std::string error;
if (!go2cpp::config::SoManifest::LoadFromFile(path, &next, &error)) {
    // 语法错误：不要 Reload，旧版本仍在服务
    return;
}
const auto result = reloader.Reload(next);
if (!result.ok) {
    // 失败：旧版本仍在服务；记录 result.error，修复后发布更大的 revision
    std::fprintf(stderr, "热更失败: %s\n", result.error.c_str());
}
```

**inotify 溢出（`IN_Q_OVERFLOW`）**：事件队列溢出说明至少丢了一个变更通知，
此时不能相信 revision 相等就跳过。重新读取 manifest 后调用：

```cpp
const auto result = reloader.ForceRescan(next);
// 等价写法：reloader.ForceRescan() 直接重扫 CurrentManifest()
```

`ForceRescan` 会跳过「revision 相等即幂等」，重新 `Validate` 整份 manifest，并把
**所有** SO 重新加载一遍；成功时 `applied=true`，即使 revision 没变。

回滚发布：不要试图把 manifest 改回小 revision（会被拒绝）。正确做法是构造一个
**更大的 revision**，把 `path`/`sha256` 指回旧版本文件；旧文件仍在保留窗口内。

---

## 4. 排障指南：按 `so.runtime.*` 看当前状态

### 4.1 怎么读

最快的办法是把配置注册表整体打印出来：

```cpp
std::cout << go2cpp::config::Config::Instance().DumpIni();
```

程序内按需读：

```cpp
auto& cfg = go2cpp::config::Config::Instance();

auto base = cfg.LookupBase("so.runtime.reload_failure_total");
if (base) std::cout << base->ToString() << "\n";

// per-SO
auto version = cfg.LookupBase(
    go2cpp::config::SoReloader::RuntimeVersionKey("libedge"));
if (version) std::cout << version->ToString() << "\n";
```

三个指标与其余观测键一样是 `ConfigVar<std::string>`，值是十进制文本：
`reload_success_total`/`reload_failure_total` 用 `std::stoull`，
`reload_duration_ms` 用 `std::stoll` 读取。指标镜像在外部 `LoadFromIni`
正持有同名变量 pending 时可能跳过本次写入；事务结果不受影响，排障时以
`Reload` 返回值与实例访问器为准。

### 4.2 键的含义

| 键 | 含义 | 更新时机 |
|---|---|---|
| `so.runtime.revision` | 当前已提交 manifest revision | 仅成功提交 |
| `so.runtime.last_reload_at` | 最近一次**成功**热更的 Unix 秒 | 仅成功提交 |
| `so.runtime.reload_success_total` | 累计成功提交次数（只统计 `applied=true`） | 每次 `Reload`/`ForceRescan` 返回前 |
| `so.runtime.reload_failure_total` | 累计失败次数（校验/loader/被 revision 抢占） | 每次 `Reload`/`ForceRescan` 返回前 |
| `so.runtime.reload_duration_ms` | 最近一次 `Reload`/`ForceRescan` 入口到返回的耗时（含等锁） | 每次 `Reload`/`ForceRescan` 返回前 |
| `so.<name>.runtime.current_version` | 该 SO 已提交版本 | 该 SO 成功提交 |
| `so.<name>.runtime.loaded_path` | 该进程实际加载的路径（含版本） | 该 SO 成功提交 |
| `so.<name>.runtime.loaded_sha256` | 实际加载内容哈希 | 该 SO 成功提交 |
| `so.<name>.runtime.abi_version` | 该 SO 的 ABI 字符串 | 该 SO 成功提交 |
| `so.<name>.runtime.loaded_at` | 该 SO 最近成功加载的 Unix 秒 | 该 SO 成功提交 |

注意：`last_reload_at` 与 per-SO 的 `loaded_at` **只反映成功**；失败时它们不变，
要看 `reload_failure_total` 和调用返回的 `result.error`。同 revision 的幂等命中
既不算成功也不算失败（`success_total` 不会涨）。

### 4.3 常见症状 → 先看哪里

**「发布了新 manifest，但 `current_version` 没变」**

1. 看 `failure_total` 是否增长；看应用日志里 `result.error`；
2. 如果 revision 没变：`Reload` 会按幂等直接返回（`applied=false`），这是正常的；
   确实需要重扫时用 `ForceRescan`（例如 inotify 溢出）；
3. 常见拒绝原因：哈希不匹配、`path` 不存在或文件名不含 version、声明 ABI 与宿主
   不一致、A1 依赖组不自洽、已加载组/SO 从 manifest 消失。

**「`failure_total` 一直涨」**

每次失败都对应一条 `result.error`；`Validate` 的 `diagnostics` 能一次列出多条
（预检程序传 `&diagnostics`）。并发发布时，较小 revision 被更大 revision 抢占
也会计入失败——这不是配置错误，但说明有两个发布者在竞争，应让发布端串行。

**「`success_total` 不涨，但 `current_version` 是对的」**

同 revision 幂等命中；不会重复 `dlopen`，也不会增加成功计数。检查发布端是否
真的抬高了 revision。

**「版本对，但业务仍在跑旧逻辑」**

- 看 `loaded_path` 是否指向新文件、`loaded_sha256` 是否等于新文件哈希；
- 检查 `SoActivator` 是否真的 `store` 了新 vtable，且业务线程用 `acquire` 读；
- 检查业务是否在启动时缓存了 vtable 指针（应每次调用时读取，或在切换后统一刷新）。

**「`reload_duration_ms` 很大」**

时间包含等待 `m_reload_mutex` 的时间。如果 loader 里有 IO/网络/长时间初始化，
会直接体现在这里；把重活挪到 `SoLoader` 之外，或接受一次可观测的抖动。

**「多进程版本不一致」**

每个进程独立 `SoReloader`、独立写自己的 `loaded_path`/`loaded_sha256`。用这两个键
跨进程比对（F3）；本层不保证多个进程同时切换。

---

## 5. 限制与不适用场景

### 5.1 本层明确不做的事（引自 `docs/so-reload-design.md` §1.3）

- **不实现 `dlopen`/`dlsym`/`dlclose`**：这是加载器层的职责。本层只定义
  `SoLoader` / `SoUnloader` / `SoActivator` 三个回调契约；
- **不实现 vtable**：不保存函数指针、不做原子指针替换。本层只保证
  `SoActivator` 在 `NotifyPending()` **之后**被调用；
- **不做流量灰度**：`candidate` 只被解析和透传，1% 分流属于业务/网关层；
- **不做进程隔离**：F1 的进程内崩溃本层无法兜底，独立进程插件模型不在范围内；
- **不实现 inotify**：B1/B3/E2/E3 复用 `Config::StartWatcher` 的轮询 + 稳定快照；
  inotify 事件源由上层提供，但溢出后的强制全量重扫入口在本层
  （`SoReloader::ForceRescan()`）；
- **不删除旧 SO**：C1 的 `retire_policy` 只解析、不执行；本层只维护
  「旧版本路径保留清单」，真正的 `dlclose` 由加载器在引用计数归零后决定；
- **不接管 `Config` 的私有事务锁**：本层用 `SoReloader::m_reload_mutex` 串行化自己，
  与外部 `LoadFromIni` 的竞争按失败处理（宁可失败，不交叉覆盖）。

### 5.2 什么样的业务不该用它

1. **需要崩溃隔离或安全沙箱的第三方插件**：SO 在宿主进程内，段错误/死循环会直接
   带崩进程；应改用独立进程插件（RPC/共享内存）。
2. **硬实时、要求热更零抖动**：`dlopen`、哈希、页映射会产生毫秒级抖动，且
   `reload_duration_ms` 正是为了暴露它；不要把它放进 P99 敏感路径。
3. **有状态、长生命周期回调且没有引用计数**：本层不回收旧句柄；在途调用持有旧
   vtable 时旧句柄必须继续有效。没有 refcount/retire 方案就不要上线，否则会泄漏
   或 use-after-free。
4. **需要按百分比灰度/AB测试**：`candidate` 只是配置位，没有路由、没有流量切分。
5. **需要任意历史版本一键回滚**：revision 只增不减；回滚等于发布一个更大的
   revision 指回旧文件，且旧文件必须还在保留窗口内。
6. **跨进程强一致切换**：每个进程各自 Reload；没有分布式协调、没有屏障。
7. **ABI 不稳定、无法保证同编译器/同 STL**：`SoManifest` 只能校验**声明的**字符串，
   真实符号与内存布局必须由加载器保证；跨编译器边界请改用 C ABI/稳定接口层。
8. **需要自动清理旧 `.so` 文件**：本层只给保留清单，不删文件、不 `dlclose` 旧句柄。
9. **一次发布要求部分组生效**：一次 `Reload` 对 manifest 内所有变更组是全或全无；
   要按组独立提交，应拆成多个 revision 或使用多个实例（但注意指标键是进程级）。
10. **非 Linux/ELF 环境**：配置层本身平台中立，但本文的 `dlopen`/`RTLD_LOCAL`
    示例不适用于 Windows；需要自行替换为 `LoadLibrary`/`GetProcAddress` 等。

如果只是配置项热更、不需要替换代码，直接用既有 `Config`/`ConfigVar` 更简单；
`SoReloader` 只解决「代码文件也要换」这一类问题。
