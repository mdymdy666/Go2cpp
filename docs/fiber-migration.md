# Fiber 可迁移性研究：safepoint id、句柄表与签名（v3）

> 研究范围：Go2Cpp 的 Fiber / Channel，最小子集为「无外部资源、只等 channel
> 的挂起 Fiber」。配套可运行 demo 在 `tools/migration/`。
>
> v3 在 v2「地址符号化」之上解决三件 v2 明确留下的问题：
> **函数内指令布局敏感**（safepoint id）、**堆指针一律拒绝**（显式句柄表）、
> **快照无来源认证**（HMAC-SHA256 强制签名）；并补齐 lambda / 静态函数的
> 符号注册与符号表版本诊断。

## 0. 结论摘要

| 问题 | v1 | v2 | v3 | 证据 |
|------|----|----|----|------|
| 上下文保存区可序列化 | 能 | 能 | 能 | `context_x64.S:50-75` |
| 栈快照跨进程还原 | 需固定 VA | 按偏移平移 | 按偏移平移 | `relocations` 段 |
| 代码地址跨进程 | 需 `-no-pie` | 按符号解析 | 按符号解析 | `symbols` 段 |
| SO 换加载地址 | 不能 | 能 | 能 | demo v1→v2 |
| **函数内指令布局变化** | 失败 | **失败** | **能（safepoint id）** | §7.3 |
| lambda / 匿名静态函数 | 不支持 | 需手工注册 | **`MIGRATION_SYMBOL_FN` + 诊断** | §7.4 |
| **栈上堆指针** | 拒绝 | 拒绝 | **已登记对象 → 句柄** | §7.5 |
| **快照来源认证** | 无 | 无 | **HMAC-SHA256，强制** | §7.6 |
| 同名符号冲突 | 未检测 | 未检测 | **dump 拒绝并报告** | §7.7 |
| 现有 `go2cpp::Fiber` 直接迁移 | 不能 | 不能 | 仍不能（设计见 §11） | `m_context` 私有 |

**一句话**：v3 把「恢复点」从**指令偏移**提升为**稳定 id**，把「堆指针」提升为
**显式句柄**，把「快照文件」提升为**带认证的镜像**。剩下的硬边界是
**栈帧布局**（帧大小 / 局部槽位）与**进程级运行时状态**（锁、TLS、canary、
外部资源）—— 这两类需要核心 API 与调度器配合，单靠格式解决不了。

## 1. 目标与范围

目标：把暂停中的 Fiber（栈内容 + 寄存器上下文 + 等待状态 + 挂起点）序列化，
在另一个进程反序列化后继续执行。

最小子集：不持有 fd/socket/timer/取消 Context，只阻塞在 channel 收发或显式
park 上，单线程调度、无抢占。

不在范围：网络 IO、`fiber_local`、嵌套 Fiber 父链、跨机器、跨架构。

## 2. 机制剖析：一个挂起的 Fiber 保存了什么

### 2.1 栈：`FiberStack`

`fiber.cpp:759` 的 `FiberStack` 有两种布局：growable（大预留 + 顶部提交，
`fiber.cpp:464-511`）与固定栈（usable + guard page）。对迁移的含义：

- 需要序列化的是实际用到的 `[rsp, top)`，不是整个映射；
- growable 栈的 `committed` 只是分配状态，快照记录当前 `rsp` 即可；
- 生产版栈来自池化分配（`fiber.cpp:634-695`），**复用地址不可控**，迁移必须
  自己分配栈并按偏移重定位。

### 2.2 上下文保存区：`context_x64.S`

`go2cpp_context_jump`（`context_x64.S:50-75`）压入 6 个 callee-saved 后把
`rsp` 作为「上下文」返回；保存区布局：

| 偏移 | 内容 |
|------|------|
| +0 | r12 |
| +8 | r13 |
| +16 | r14 |
| +24 | r15 |
| +32 | rbx |
| +40 | rbp |
| +48 | rip（跳回点） |

`go2cpp_context_make`（`context_x64.S:19-32`）在同一布局上摆好
`rbx=entry / rbp=end / rip=entry trampoline`。**caller-saved 不需要保存**：
挂起一定发生在函数调用内部，SysV ABI 保证活跃值已落栈。

### 2.3 挂起 / 恢复路径

- `resume_locked`（`fiber.cpp:1285`）：置 Running、保存 errno、设
  `t_current_growable_stack`，然后 `JumpContext(m_context, this)`
  （`fiber.cpp:1369`），返回后用 `transfer.context` 更新 `m_context`；
- `suspend`（`fiber.cpp:1453`）：置 Suspended，`JumpContext(m_caller, this)`，
  恢复后 `m_caller = transfer.context`。

**关键推论**：`m_caller` 只在**下一次**挂起时使用；从快照恢复时，
`suspend()` 返回的第一件事就是刷新它。所以恢复方不需要预置旧 caller ——
demo 的 `FiberCtl::caller` 恢复时置 0 也安全。

### 2.4 等待状态：Channel + ParkingCondition

`Channel<T>` 用两个堆上队列（`channel.hpp:2188-2190`）：

```cpp
std::deque<std::shared_ptr<PendingSend>> m_senders;
std::deque<std::shared_ptr<PendingRecv>> m_receivers;
```

`PendingRecv`（`channel.hpp:1521`）含 `optional<T>`、`shared_ptr<SelectWaitState>`、
`core::ParkingCondition cv` 与两个原子位。**不能 memcpy**，且节点不在 Fiber
栈上。demo 的取舍：等待节点（`Waiter`）放 Fiber 栈上，通道只存队列头尾的
**栈内偏移**；恢复时用偏移重算指针。

## 3. v3 设计

### 3.1 地址分类

dump 时栈上每个 8 字节值按以下顺序判定：

| 判定 | 处理 | 审计计数 |
|------|------|----------|
| 是当前 safepoint 的返回地址槽 | `kRelocSafepoint`，恢复时写当前镜像锚点 | `symbol_ptrs` / `code_ptrs` |
| 落在旧栈 `[base, top)` | `kRelocStack`，addend = 旧栈内偏移 | `stack_ptrs` |
| 能被符号表解析 | `kRelocSymbol`，symbol_id + 符号内偏移 | `symbol_ptrs`（代码段另计） |
| 落在已登记对象区间 | `kRelocHandle`，句柄 id + 对象内偏移 | `handle_ptrs` |
| 落在其它映射 | 不重定位，记录风险 | `other_ptrs` |
| 落在未登记堆/匿名可写映射 | **拒绝**并指出槽位/地址 | `heap_ptrs` |
| 不在任何映射 | 视为普通数据（如 64 KiB payload） | 不计 |

**v2 的 bug 修正在这里**：符号兜底解析曾把「地址小于下一个注册符号」当作充分
条件，于是主程序数据段与 dlopen SO 之间的堆地址会被误判成前一个符号的偏移，
绕过堆检查。v3 要求「最近下界符号 + 1 MiB 跨度」同时成立（`symbol_table.cpp`
第 3 步），堆地址因此会落到句柄/拒绝分支。

### 3.2 符号表

`symbol_table.hpp` 提供进程级注册表：

- **主路径：编译期注册**。`MIGRATION_SYMBOL(name)` 展开出静态
  `SymbolRegistrar`；`MIGRATION_SYMBOL_FN(fn)` 额外支持函数指针 / 无捕获 lambda；
- **lambda 命名**：先 `dladdr`（动态符号表），失败再读 ELF `.symtab`
  （含局部符号），用 `abi::__cxa_demangle` 生成可读名；`.symtab` 只用于
  **命名**，不用于解析栈上任意地址（否则会绕过「显式注册」约束）；
- **operator() 注册**：`+lambda` 得到的是转换函数 `_FUN`，真正出现在栈帧上的
  是 `operator()`。宏通过成员函数指针（Itanium ABI 前 8 字节为函数地址）
  同时注册两者；
- **冲突检测**：同名不同址（inline 克隆、手写重复注册）记录为 conflict，
  dump 直接拒绝并打印两个地址；
- **版本哈希**：`Hash()` 覆盖整表符号名（排序后 FNV-1a）；快照同时记录
  「引用集哈希」（`symbol_hash`）与「整表哈希」（`symbol_table_hash`），
  并把整表**名字清单**写进文件，恢复侧据此列出「新增/移除」明细；
- **epoch**：`kSymbolEpoch` 变化即拒绝（符号命名/语义不兼容）。

### 3.3 栈帧遍历与返回地址重写

```
rbp = 保存区 [+40]
while rbp ∈ [base, top):
    ret  = *(rbp + 8)          # 返回地址
    next = *(rbp)              # 上一帧 rbp
    ret 必须能符号化，否则 dump 失败（给出地址 + 模块 + 注册建议）
    记录 frame{rbp_offset, ret_slot_offset, symbol_id, ret_offset}
    if next <= rbp: break
    rbp = next
```

要求 `-fno-omit-frame-pointer -fno-optimize-sibling-calls`，并对可迁移函数加
`noinline`（demo 的 `FiberEntry/ParkSelf/ChannelRecvAt/BuiltinDemoBody`）。

### 3.4 safepoint：从指令偏移到稳定 id

**问题**：v2 在 `ChannelRecvAt` 帧上保存的返回地址是
`用户函数 + 指令偏移`。若新版本在该调用之前插入了代码，同一偏移会落到错误
指令。safepoint id 把「恢复点」从地址变成**语义编号**。

**注册**（`safepoints.hpp`）：`MIGRATION_SAFEPOINT(id, out)` 在调用之后展开：

```cpp
extern char _migration_sp_address_##id[] __asm__("g2c_safepoint_" #id);
static const SafepointAnchor _migration_sp_anchor_##id
    __attribute__((section("migration_safepoints"), used, aligned(8))) = {
        id, 0, &_migration_sp_address_##id, __PRETTY_FUNCTION__, __FILE__,
        __LINE__, 0};
asm volatile(".local g2c_safepoint_" #id "\n g2c_safepoint_" #id ":");
if (SafepointResumePending(id)) { out = SafepointResumeValue(); }
```

- 锚点是**内联汇编定义的全局符号**，汇编器把它放在语句在指令流中的确切位置，
  链接器把段里的指针重定位到它。GCC 的 `&&label` 在静态初始化里会被折叠到
  函数入口（实测），不可用；
- 段记录需要 `aligned(8)`：否则编译器可能给每个 `static` 对象 32 字节对齐，
  段内条目不再是紧凑的 40 字节；
- 恢复进程**不执行用户代码**：`SafepointTable` 用 `dl_iterate_phdr` 枚举模块、
  读磁盘 ELF 段表找 `migration_safepoints` 段，运行时地址 = `加载基址 + sh_addr`；
  主程序与 dlopen 的 SO 走同一条路径。

**dump**：`ChannelRecvAt` 用自己的帧指针记录返回地址槽
（`__builtin_frame_address(0) + 8`，x86-64 帧布局），并把 safepoint id 写进
`FiberCtl`。快照的 Safepoints 段记录 `{id, symbol_id, slot_offset, old_offset}`，
重定位表里该槽位是 `kRelocSafepoint`。

**restore**：按 id 查当前镜像的锚点，校验「锚点所属函数 == 旧返回地址所属
函数」（防止 id 被复用），把锚点地址写回槽位。`ParkSelf` 恢复后逐帧返回，
`ChannelRecvAt` 的 `ret` 落到锚点；此时 `rsp`/callee-saved 与「调用正常返回」
完全一致，`%rax` 是通道投递的返回值。

**返回值语义**：编译器可能把「保存返回值到局部变量」放在锚点之前，恢复路径
会跳过它；因此宏在锚点后显式补写 `out = SafepointResumeValue()`，`ChannelSend`
在 resume 前用 `ArmSafepointResume(id, value)` 记录本次投递值。

**边界**：safepoint id 只解决**指令偏移**。栈帧布局（帧大小、局部槽位）在
版本间必须一致；帧布局变化意味着恢复后的栈与代码预期不符，需要整帧重映射
（未实现，见 §8）。

### 3.5 句柄表

**登记**（dump 侧）：

```cpp
auto* label = new std::string("跨进程标签");
auto* pod   = new DemoPayload{...};
MIGRATION_MIGRATE_OBJECT(label, "std::string");
MIGRATION_MIGRATE_OBJECT(pod, "DemoPayload");
```

`RegisterMigratableObject` 记录 `[address, address+size)` 与类型名；dump 扫描
栈槽时若值落在区间内，分配句柄 id、调用类型的序列化函数、写 Handles 段，并把
槽位改成 `{handle id, 对象内偏移}`。

**类型契约**：ADL 可找到的 `MigrationSerialize` / `MigrationDeserialize`，再
用 `MIGRATION_SERIALIZABLE("DemoPayload", DemoPayload)` 注册工厂；内建
`std::string`（长度 + 字节）。恢复侧按类型名查工厂，缺失时列出已注册类型并拒绝。

**所有权**：dump 侧对象仍归原进程；restore 侧重建对象由句柄表持有，
`ReleaseHandles()` 统一析构（Fiber 跑完后调用）。

**不支持**：对象内部指针、循环引用、非默认构造、虚表/`shared_ptr` 控制块。

### 3.6 签名与完整性

- 快照尾部是 64 字节签名段：`{magic "G2CSIG01", alg, flags, key_id[16], mac[32]}`；
- **HMAC-SHA256**（FIPS 180-4 / RFC 2104），密钥来自
  `MIGRATION_SIGNING_KEY` 或 `MIGRATION_SIGNING_KEY_FILE`；
- 覆盖范围：整文件，其中 **CRC 字段与 MAC 字段都置 0**（两者互不依赖，写端
  先签名后补 CRC，读端同样清零）；每个段另有独立 CRC，签名段的段 CRC 按
  「MAC 置 0」计算；
- `key_id = SHA-256(key)` 的前 16 字节写进签名段。恢复侧先比 key_id，再比 MAC：
  「密钥不匹配」与「内容被篡改」可以区分；
- 比较使用恒定时间实现；
- 未签名 / 签名错误 / 缺密钥一律拒绝；`MIGRATION_INSECURE_NO_SIGNATURE=1` 是
  **仅测试**的逃生门。

### 3.7 恢复流程

1. 读快照：magic/version/header/长度/段 CRC/整文件 CRC/**签名**/build-id/epoch；
2. 解析符号目录：逐个 `Lookup`，缺失的集中列出并拒绝；
3. 校验引用集哈希、符号内偏移、挂起点 id 存在且唯一、句柄 id 唯一；
4. 重建句柄对象（类型工厂），安装到句柄表；
5. `mmap` 新栈（地址由内核决定），把栈快照拷到 `new_base + ctx_offset`；
6. 应用重定位：栈内平移 / 符号解析 / **safepoint 锚点** / **句柄对象地址**；
7. 重建 `FiberCtl`/`ChannelCtl`：`fn`、`channel` 来自符号解析，
   `ctx`/`waiter`/`head`/`tail` 来自新基址 + 偏移，`safepoint_id` 来自快照；
8. `ChannelSend` → `ArmSafepointResume` → `JumpContext(ctx)`；Fiber 从
   `ParkSelf` 返回后落到 safepoint 锚点，继续跑到下一次挂起或完成；
9. 完成后 `ReleaseHandles()`。

## 4. 序列化格式 v3

实现：`tools/migration/include/migration/format.hpp`、`src/format.cpp`。

### 4.1 布局

```
[0,224)              Header
[224,224+48*N)       Section table（每项 48 字节）
8 字节对齐后          各 Section 数据
末尾（最后一节）       Signature 段（64 字节）
```

全部小端、定宽、逐字段写字节，不 `memcpy(struct)`。

### 4.2 Header（224 字节）

| 偏移 | 类型 | 字段 | 说明 |
|------|------|------|------|
| 0 | char[8] | magic | `G2CMIG03` |
| 8 | u32 | version | 3 |
| 12 | u32 | header_size | 224 |
| 16 | u32 | flags | 见 §4.4 |
| 20 | u32 | section_count | |
| 24 | u16 | arch | 62 = EM_X86_64 |
| 26 | u8 | pointer_size | 8 |
| 27 | u8 | endianness | 0 |
| 28 | u64 | build_id_hash | 可执行文件 build-id 哈希 |
| 36 | u64 | file_size | |
| 44 | u64 | fiber_id | |
| 52 | u64 | stack_va | 旧栈顶（诊断） |
| 60 | u64 | stack_size | |
| 68 | u64 | context_va | 旧 ctx（诊断） |
| 76 | u64 | stack_bytes | |
| 84 | u64 | arena_va | 诊断 |
| 92 | u64 | arena_size | |
| 100 | u64 | channel_va | 旧通道地址（诊断） |
| 108 | u64 | waiter_va | 旧等待节点（诊断） |
| 116 | u32 | wait_kind | 0/1/2 |
| 120 | u32 | crc32 | 整文件 CRC；计算时本字段为 0 |
| 124 | u32 | symbol_epoch | 符号系统版本（=1） |
| 128 | u64 | stack_base_va | **旧栈基址（重定位原点）** |
| 136 | u64 | symbol_hash | 引用集哈希 |
| 144 | u32 | symbol_count | |
| 148 | u32 | reloc_count | |
| 152 | u32 | frame_count | |
| 156 | u32 | safepoint_count | |
| 160 | u64 | ctx_offset | ctx - stack_base |
| 168 | u64 | waiter_offset | waiter - stack_base |
| 176 | u64 | symbol_table_hash | 生成进程整表哈希 |
| 184 | u64 | signature_offset | 签名段偏移（0 = 未签名） |
| 192 | u32 | handle_count | |
| 196 | u32 | safepoint_schema | 挂起点段版本（=1） |
| 200 | u64 | signature_size | 64 |
| 208 | u64 | reserved5 | |
| 216 | u64 | reserved6 | |

### 4.3 段表

| type | 名称 | 内容 | 必需 |
|------|------|------|------|
| 1 | Stack | `[context_va, stack_va)` 原始字节 | 是 |
| 2 | Registers | 7×u64：r12 r13 r14 r15 rbx rbp rip（冗余校验） | 是 |
| 3 | Channel | capacity/count/closed + head/tail **偏移** + buffer | 有等待时 |
| 4 | PointerAudit | stack/symbol/code/heap/handle/other/slots 计数 | 否 |
| 5 | Summary | UTF-8 摘要 | 否 |
| 6 | FiberState | ctx/caller/stack/state/result/arg + 符号引用 + safepoint_id | 是 |
| 7 | Symbols | `{id, kind, size, name, display}`，**不含地址** | 是 |
| 8 | Relocations | `{kind, target_id, slot_offset, addend}` | 是 |
| 9 | Frames | `{rbp_offset, ret_slot_offset, symbol_id, ret_offset}` | 诊断 |
| 10 | Safepoints | `{id, symbol_id, slot_offset, old_offset}` | 有 safepoint 时 |
| 11 | Handles | `{id, old_address, object_size, type, payload}` | 有句柄时 |
| 12 | Signature | `{magic, alg, flags, key_id[16], mac[32]}` | 是（强制） |
| 13 | SymbolManifest | 生成进程整表名字清单 | 是 |

重定位种类：`1=kRelocStack`、`2=kRelocSymbol`、`3=kRelocHandle`、
`4=kRelocSafepoint`。

### 4.4 flags

| 位 | 含义 |
|----|------|
| 0 `kFlagCrossProcessReady` | 恢复信息齐全 |
| 1 `kFlagHasHeapPointers` | 存在未句柄化堆指针（正常不会出现） |
| 2 `kFlagHasCodePointers` | 栈上有代码地址 |
| 3 `kFlagHasStackPointers` | 栈上有自引用 |
| 5 `kFlagSymbolized` | 快照已符号化 |
| 6 `kFlagHasHandles` | 有句柄表 |
| 7 `kFlagSigned` | 带 HMAC 签名（正常恒置位） |
| 8 `kFlagHasSafepoints` | 有 safepoint 槽位 |

### 4.5 校验

magic / version / header_size / arch / 指针宽度 / 字节序 / file_size /
symbol_epoch / 整文件 CRC / 每段 CRC / **签名（key_id + MAC）** / build-id /
段数量 / 段间自洽 / 符号与句柄 id 可解析 / 挂起点槽位越界 / `state == Parked(2)`。

## 5. 不可迁移项清单

| # | 类别 | 内容 | v3 的处理 |
|---|------|------|-----------|
| 1 | 未登记堆指针 | `Fiber::Impl*`、`PendingRecv*`、`std::function` 目标、`shared_ptr` 控制块 | 审计发现即拒绝，并提示登记/序列化 |
| 2 | **栈帧布局变化** | 帧大小、局部变量槽位改变 | 未解决；要求版本间帧布局一致，见 §8 |
| 3 | 指令偏移变化 | 挂起点前后的指令增删 | **safepoint id 解决** |
| 4 | 线程局部 | `s_current_fiber`、`t_current_growable_stack`、`errno` | 恢复后由代码重建 |
| 5 | 锁 / 原子 | `ParkingCondition` 内部互斥量、futex 代际、`m_resume_claim` | 等待节点无锁；单线程 |
| 6 | 栈保护 canary | `__stack_chk_guard` 是进程级随机值 | `-fno-stack-protector` |
| 7 | C++ 运行时 | `exception_ptr`、`fiber_local`、`Context` 取消回调 | 范围外 |
| 8 | 外部资源 | fd/socket/timer/epoll 注册 | 范围外 |
| 9 | 分配器状态 | glibc arena / tcache / mmap 布局 | 不依赖（按偏移/句柄） |
| 10 | 架构 / ABI | 不同 ISA、结构体布局、编译器版本 | version + epoch + build-id 拒绝 |

## 6. 跨进程阻塞点

### 6.1 已由 v3 消除

- **固定 VA / `-no-pie`**：栈按偏移、代码/数据按符号、堆按句柄；
- **SO 换加载地址**：符号名相同即可；
- **同函数内指令偏移变化**：safepoint id；
- **快照来源不可信**：强制 HMAC 签名。

### 6.2 仍存在（真实 `Fiber`/`Channel` 不可直接迁移的原因）

1. **无 dump/adopt API**：`m_context`/`m_caller`/`m_stack` 私有
   （`fiber.cpp:1527-1529`），只读 API 能 dump，restore 必须写；
2. **控制块堆分配**：`Impl` 由 `make_unique`、`FiberRecord`/`PendingRecv` 由
   `make_shared`（`channel.hpp:1521`），栈上的 `self`/`pending` 指针需要句柄化；
3. **等待节点不在栈上**：`PendingRecv` 在 `deque<shared_ptr>`（`channel.hpp:2188-2190`）
   且含锁/原子，需要 Channel 提供「按语义重挂」的内部 API；
4. **同进程 restore 也不容易**：新栈来自池化分配，地址不可控；旧 Fiber
   析构只能 `abandon_suspended`（`fiber.cpp:1008`），无法展开 RAII；
5. **canary/TLS/锁/外部资源**：见 §5；
6. **栈帧布局必须一致**：见 §8。

## 7. Demo 与实测

### 7.1 设计

位于 `tools/migration/`，不修改核心库、不并入主构建：

- 复用真实上下文 ABI（编译 `context_backend.cpp` + `context_x64.S`）；
- 控制块为全局对象（`g_fiber_ctl`/`g_channel_ctl`），注册为符号；
- 栈普通 `mmap`，等待节点在栈上，通道只存偏移；
- 栈上 64 KiB 可校验 payload，证明迁移的是完整活跃栈；
- 可选 `MIGRATION_DEMO_SO` 把主体放到 SO。

### 7.2 基础运行（PIE + ASLR）

```
[A] 栈=[0x7be510c00000,0x7be510e00000) 初始 ctx=... 入口=0x5ce85757d960
[A] fiber#1 启动，准备在通道 0x... 上接收（栈上 payload=65536 字节，校验和=8355840）
[A] Fiber 已挂起：ctx=... waiter=... safepoint=0x10001
[A] 栈快照 65816 字节，符号 8 个，重定位 22 条，帧 6 个，safepoint 1 个，句柄 0 个
[A] 指针审计 stack=6 symbol=16 code=9 heap=0 handle=0 other=0 slots=8227
...
[B] 旧栈 0x... → 新栈 0x...；入口 BuiltinDemoBody+0 → 0x...
[B] 挂起点 id=0x10001：旧 BuiltinDemoBody+0xaa → 新 BuiltinDemoBody+0xad（布局变化不影响）
[B] 发送第 1 条消息 4242
[B] fiber#1 继续执行，收到消息 #1 = 4242
[B] fiber#1 栈上 payload 校验通过（8355840）
[B] Fiber 已完成，result=7（期望 7）
```

要点：B 侧没有打印「启动」，说明是从 `ChannelRecvAt` 内部续行；注意旧/新偏移
相差 3 字节 —— 锚点在编译器「保存返回值」之后，恢复路径用宏里的显式赋值补上。

### 7.3 safepoint：布局变化后恢复

`demo_body_sp_v1.cpp` 与 v2 语义相同；v2 在第一个挂起点之前用不同长度的循环
写 `layout_pad`，之后又写一段。两版：

| 指标 | v1 | v2 |
|------|----|----|
| 挂起点槽位 `slot_off` | `0x1eff48` | `0x1eff48`（帧布局一致） |
| 记录的回地址偏移 `old_offset` | `0x19a` | `0x1de`（指令布局不同） |
| 恢复结果 | — | `result=7` |

```
[B] 挂起点 id=0x20001：旧 migration_demo_body+0x19a → 新 migration_demo_body+0x1e1
[B] fiber#1 [sp-v2] 栈 payload 校验通过（8355840），句柄校验通过：label="跨进程标签" payload=heap-payload
```

用旧 v1 SO（没有 safepoint 段）恢复同一快照会明确拒绝：

```
[B] restore 失败: 快照引用的挂起点 id=0x20001 在当前镜像中不存在：请用包含相同
    MIGRATION_SAFEPOINT(0x20001) 的版本恢复（函数布局可以变化，id 必须一致）
```

### 7.4 lambda

```
sym[1] (anonymous namespace)::g_lambda_body::{lambda(FiberCtl*)#1}::operator()(...) kind=3
sym[7] (anonymous namespace)::g_lambda_body::{lambda(FiberCtl*)#1}::_FUN(...) kind=3
[B] 挂起点 id=0x30001：旧 ...::operator()+0x9c → 新 ...::operator()+0x9f
[B] Fiber 已完成，result=7（期望 7）
```

`_FUN` 与 `operator()` 同时注册：前者是转换函数，后者是栈上的实际函数。
本目标关闭了 IPA 克隆，否则 `.constprop/.isra` 会让实际函数与注册地址不一致。

### 7.5 句柄

dump 侧：

```
[A] 栈快照 65912 字节，符号 8 个，重定位 28 条，帧 6 个，safepoint 1 个，句柄 2 个
[A] 指针审计 stack=7 symbol=17 code=9 heap=0 handle=4 other=0 slots=8231
```

restore 侧：

```
[B] fiber#1 [sp-v2] 栈 payload 校验通过（8355840），句柄校验通过：label="跨进程标签" payload=heap-payload
```

未登记时：

```
[A] dump 失败: 栈上存在未登记的堆指针 0x...（槽位 0x...）：v3 只迁移显式登记
    的可序列化对象，请在分配处调用 MIGRATION_MIGRATE_OBJECT(ptr, "TypeName") ...
```

### 7.6 签名

```
[B] restore 失败: 签名密钥不匹配：快照 key_id=4773d12e2371bb93，当前密钥 key_id=5df404c22ba4e956
[B] restore 失败: 未配置签名密钥：无法校验快照签名。...
[A] dump 失败: 未配置签名密钥：请设置 MIGRATION_SIGNING_KEY 或 ...
[B] restore 失败: 快照未签名：生产环境拒绝恢复未签名快照。...
[B] restore 失败: 签名校验失败：快照内容被篡改，或密钥错误（key_id 相同但 MAC 不符）
```

最后一条由测试脚本构造：改一个 Summary 字节、同步段 CRC 与整文件 CRC（让 CRC
通过），HMAC 仍然拒绝 —— 证明签名独立于 CRC。

### 7.7 符号冲突 / 缺失 / 表差异

```
[A] dump 失败: 符号名冲突：同名符号注册到了不同地址（inline 克隆 / 重复注册），
    按名字恢复不再唯一，共 1 处：MigrationConflictDemo @0x... / 0x...
[A] dump 失败: rbp 链第 3 帧：地址 0x...（模块 ...unregistered.so）无法符号化：
    该返回地址属于未注册的静态函数、匿名命名空间函数或 lambda ... 请加
    MIGRATION_SYMBOL(name) 或 MIGRATION_SYMBOL_FN(fn) ...
[B] 符号表版本差异（不阻断恢复）：当前进程新增 1 个、移除 0 个；新增: SpExtraSymbol
```

### 7.8 测试矩阵

`tools/migration/tests/run_tests.sh <build>`，**53 项全部通过**：

- 基础：dump/restore/verify、64 KiB payload、result 校验、PIE/ASLR；
- safepoint：v1 布局 dump → v2 布局 restore、槽位相同/偏移不同、缺 id 拒绝；
- lambda：dump/restore、demangled 名 + kind=3；
- 句柄：2 个对象跨进程重建、未登记堆拒绝；
- 签名：自检向量、正确/错误/缺失密钥、未签名、CRC 通过但 HMAC 拒绝、密钥文件；
- 符号：冲突拒绝、未注册静态函数拒绝、符号表差异明细、缺失符号拒绝；
- 拒绝：CRC、截断、magic、build-id（`migrate_alt`）、文件不存在；
- 随机/压力：5 组随机消息、20 次随机位翻转全部拒绝、10 轮 dump+restore
  （约 25ms/轮）。

主工程回归：`ctest --test-dir build-work -j4` 仍为 **116/116**（demo 是独立
CMake 工程，不进入主构建）。

## 8. 限制与风险

1. **栈帧布局必须稳定**：safepoint id 只保证「指令偏移变化」可恢复；帧大小、
   局部变量槽位变化会让恢复后的栈与代码预期不符。跨版本迁移要求可迁移函数的
   帧布局（ABI、编译选项、局部变量布局）保持一致，或实现整帧重映射（未实现）。
2. **IPA 克隆**：`.constprop/.isra` 克隆会让栈上的实际函数与注册地址不同，
   可迁移 TU 应关闭 `-fno-ipa-cp/-sra/-icf`。
3. **堆对象必须显式登记**：未登记一律拒绝；对象内部指针需在序列化函数里展开。
4. **canary 未序列化**：用 `-fno-stack-protector` 规避。
5. **安全**：快照 = 可执行栈 + 代码入口，等价于代码注入载体；必须签名、
   校验来源、限制权限。签名只证明「持有密钥的人生成」，不证明内容语义安全；
   恢复前仍应校验 build-id/epoch/符号表。
6. **版本漂移**：结构体布局、优化级别、编译器版本变化会让旧栈解释错位；
   `version`/`epoch`/`build-id` 只能拒绝，不能兼容。
7. **锁与异常**：挂起时若持锁或处于异常展开中，恢复语义无定义（demo 限定
   在无锁等待点）。
8. **性能**：全栈拷贝 + 逐槽扫描为 O(栈使用量)；65 KiB 可接受，MiB 级栈
   增长时成本明显。
9. **密钥管理**：环境变量会出现在 `ps`/`/proc/<pid>/environ`；生产优先用
   权限 0400 的密钥文件（见 §12）。

## 9. 后续路线

### P0：核心最小 API（不改默认行为）

见 §11：`Fiber::ExportMigrationImage` / `ImportMigrationImage`、
`Channel::ExportWaitState` / `AdoptWaitState`。

### P1：符号化基础设施进核心

把 `MIGRATION_SYMBOL` / `MIGRATION_SYMBOL_FN`、符号表、relocation 表、
safepoint 段下沉到 `include/go2cpp/debug/`，让 Fiber/Channel 的 dump 直接产出
v3 格式；可迁移 TU 统一加 `noinline`、关 IPA 克隆。

### P2：safepoint id（已完成 demo）

见 §3.4；剩余工作是把它接进真实 `Fiber::suspend()` 的调用点。

### P3：句柄化外部资源

channel/timer/fd 改成全局句柄（id + 注册表），快照只存句柄；恢复侧按句柄
重建或拒绝。跨机器迁移的前提。

### P4：跨机器

同架构 + 同 build-id + 句柄化 + 签名快照；跨架构需改为「逻辑快照 + 重放」。

## 10. CLI 与库接口

```bash
migrate dump     <file>     # 进程 A（需要签名密钥）
migrate restore  <file>     # 进程 B（验证签名/符号/挂起点）
migrate verify   <file>     # 只读校验
migrate selftest            # SHA-256/HMAC 向量
```

环境变量：`MIGRATION_SIGNING_KEY[_FILE]`、`MIGRATION_INSECURE_NO_SIGNATURE`、
`MIGRATION_MODE`、`MIGRATION_FILE`、`MIGRATION_MSG1`、`MIGRATION_MSG2`、
`MIGRATION_DEMO_SO`、`MIGRATION_DEMO_UNREGISTERED_HEAP`、
`MIGRATION_DEMO_EXTRA_SYMBOL`。

或链接 `migration_core` 调 `DumpDemoToFile` / `RestoreDemoFromFile` /
`VerifySnapshotFile`；格式层用 `ReadSnapshotFile` + `DescribeSnapshot`，
符号层用 `SymbolTable::Instance()`，挂起点用 `SafepointTable::Instance()`。

## 11. 生产版 `Fiber`/`Channel` 的 adopt API（已实现最小子集）

> 本节原为设计稿，2026-10-20 已按此落地最小可用实现（见 §11.6–§11.8）。
> 实现只在 `-DGO2CPP_MIGRATION_API=ON` 时参与编译与链接；默认构建不包含
> 任何迁移代码、`Channel<T>` 没有额外基类、Fiber 布局与热路径不变。
> §11.1–§11.5 保留原始设计，供后续扩展（发送等待、select、调度器接入）参考。

### 11.1 目标 API

```cpp
namespace go2cpp {

struct FiberMigrationImage {
    void* context;              // m_context（保存区指针）
    void* stack_bottom;         // 栈低地址
    std::size_t stack_size;     // 映射大小
    std::size_t stack_used;     // top - rsp
    std::uint32_t state;        // Ready/Running/Suspended/Completed/Failed
    std::uint32_t suspend_reason;
    std::uint64_t fiber_id;
    std::uint32_t safepoint_id; // 挂起点（0 = 非 safepoint 挂起）
    std::uint64_t safepoint_slot;
};

class Fiber {
public:
    // dump 方向：只读导出，不改变 Fiber 状态
    bool ExportMigrationImage(FiberMigrationImage* image,
                              std::vector<std::byte>* stack) const;
    // restore 方向：构造一个 Suspended 的 Fiber，栈由调用方提供并接管
    static std::shared_ptr<Fiber> ImportMigrationImage(
        const FiberMigrationImage& image,
        std::unique_ptr<FiberStack> stack,
        std::string* error);
};

class Channel<T> {
public:
    // dump：把当前等待者导出成与栈位置解耦的 token
    bool ExportWaitState(ChannelWaitState* out) const;
    // restore：把 token 重新挂到队列上（不重建锁/原子）
    bool AdoptWaitState(const ChannelWaitState& state, std::string* error);
};

}  // namespace go2cpp
```

### 11.2 `Fiber` 需要的访问器 / 改动点

| 类别 | 需要的访问 | 位置 / 说明 |
|------|-----------|-------------|
| 上下文 | `detail::Context m_context`、`m_caller` 读写 | `fiber.cpp:1527-1529`；目前私有 |
| 栈 | `bottom()`、`size()`、`stack_pointer()`、`usable_size()` | `FiberStack`（`fiber.cpp:759` 起）已具备只读接口，导入时需要「接管外部 mmap 栈」的构造 |
| 状态 | `FiberState state()`、挂起原因、`id()`、父 Fiber | 已有部分只读 API；导入时要把状态置 `Suspended` 并跳过首进入 trampoline |
| 恢复点 | `suspend()` 前记录 `{safepoint_id, return slot}` | `fiber.cpp:1453` 附近；对接 demo 的 `ChannelRecvAt` 模式 |
| 恢复入口 | `resume_locked` 识别「迁移恢复」标志 | `fiber.cpp:1285`；避免走首次 resume 的状态机/TLS 初始化 |
| 栈所有权 | `FiberStack::Adopt(void* base, size_t size)` | 恢复侧必须接管 mmap 栈；析构时释放 |
| 旧对象 | `abandon_suspended()` | `fiber.cpp:1008`；迁移后旧 Fiber 只能放弃，不能展开栈上 RAII |
| TLS | `t_current_fiber`、`t_current_growable_stack`、`errno` | 恢复线程上重建；`resume_locked` 已在跳转前设置，导入路径要复用 |

**状态迁移点**（按调用顺序）：

1. **dump**：`Fiber::suspend()` 进入前 → 记录 safepoint id / 返回槽；
   `ResumeFiber` 返回后 → 确认状态 `Suspended`、`m_context` 指向保存区；
2. **导出**：只读快照 `m_context`/`m_caller`/栈区间/状态；`m_caller` 置 0
   （恢复后由 `suspend()` 返回路径自动刷新）；
3. **导入**：`Fiber::ImportMigrationImage` → 设置 `m_stack`（接管 mmap）、
   `m_context = image.context`、`m_caller = nullptr`、状态 `Suspended`、
   safepoint 表、`id`；
4. **恢复**：调度器把新 Fiber 放回 run queue → `resume_locked` 检测迁移标志 →
   `JumpContext(m_context, this)` → `ParkSelf` 逐帧返回 → safepoint 锚点；
5. **完成**：`ReleaseHandles()`；旧 Fiber 对象 `abandon_suspended()`。

### 11.3 `Channel` 需要的访问器 / 改动点

等待节点是最大障碍：`PendingRecv`（`channel.hpp:1521`）在堆上、含
`ParkingCondition`/原子/`shared_ptr`，不能 memcpy，也不能跨进程搬运。

| 方案 | 做法 | 代价 |
|------|------|------|
| A（推荐，demo 已用） | 等待节点放 **Fiber 栈**；Channel 只存头尾 **token + 栈内偏移** | 需要 Channel 支持「栈上等待节点」的新 API，改动大但迁移语义干净 |
| B | 节点仍在堆，但 dump 时把逻辑字段（值、方向、select 状态）导出，restore 时新建节点重新入队 | 需要 `PendingSend/PendingRecv` 的构造/入队内部 API；锁/原子代际要重置 |
| C | 只支持「无等待者」的 Channel 快照 | 适用范围最小，无法迁移挂起中的收发 |

需要的内部访问器（按方案 B）：

- `Channel::m_mutex`、`m_change_cv`（`channel.hpp:2182`）：dump 时必须已释放，
  恢复侧重建（不是序列化）；
- `m_senders` / `m_receivers`（`channel.hpp:2188-2190`）：导出队列长度与每个
  节点的逻辑状态；恢复时按顺序重建；
- `PendingSend` / `PendingRecv` 的 `optional<T>`、`select` 关联、
  `signaled/parked` 原子位：提供只读/重建内部 API；
- 恢复时的 `parked` 标志与 `ParkingCondition` futex 代际：不能沿用旧值，
  必须置「未 park」并让下一次 `park` 正常登记；
- `SelectWaitState` 的 `shared_ptr` 不能跨进程：恢复时要么重建 select 状态，
  要么拒绝 select 等待者。

**状态迁移点**：

1. dump：`Send/Recv` 在把节点入队后、`park` 前记录 safepoint；
   `park` 返回后清除；
2. 导出：节点方向、值、队列位置；Channel 的 `closed`、缓冲内容；
3. 恢复：重建 Channel（或复用全局句柄）→ 按 token 重新入队 → 把 Fiber 的
   `waiter` 指针指向新节点 → 继续 `ChannelSend`/`ResumeFiber`；
4. 清理：旧进程的节点在 dump 后由旧进程自行回收（或进程退出）。

### 11.4 调度器 / 运行时配合

- **run queue**：导入的 Fiber 必须先进入「挂起但可恢复」集合，不能在
  `ImportMigrationImage` 内部自动 resume；由调度器显式 `Resume`；
- **M 绑定**：迁移后的 Fiber 不绑定原 M；`resume_locked` 的 TLS 设置按新 M 重建；
- **取消 / deadline**：`Context` 的取消回调、timer 注册不能跨进程，恢复时应
  拒绝或重新注册（最小版：拒绝带 deadline 的挂起）；
- **fiber_local**：跨进程无意义，恢复时应清空或拒绝非空；
- **异常**：挂起时若处于 `catch`/展开中，栈上有 `__cxa_exception` 指针，
  不能迁移（dump 侧应检测并拒绝）；
- **canary**：生产版要么序列化 `__stack_chk_guard` 并写回，要么对可迁移函数
  关闭栈保护（推荐前者，但需要写 TLS/guard 的权限）。

### 11.5 分阶段落地建议

1. **只读导出**（低风险）：`ExportMigrationImage` + v3 格式，先验证「能 dump
   出可校验快照」；
2. **同进程 adopt**：新栈 + 新 Fiber 对象，验证 `adopt → resume → complete`
   闭环（不跨进程）；
3. **跨进程**：加签名、build-id、符号表、safepoint id；先支持「只等 Channel」；
4. **Channel 集成**：按方案 A 改造等待节点，或按方案 B 提供内部重挂 API；
5. **调度器集成**：run queue、M 绑定、取消/timer 拒绝策略；
6. **安全加固**：密钥管理、权限、审计（key_id/build-id/epoch）。

### 11.6 落地实现（`GO2CPP_MIGRATION_API=ON`）

#### 构建面

- `cmake -DGO2CPP_MIGRATION_API=ON` 打开；默认 OFF。
- OFF：不编译迁移实现、不链接格式层，`Channel<T>` 无额外基类/虚表，Fiber 布局不变；
  默认构建的 ctest 116 项与热路径零变化。
- ON：`go2cpp_fiber` 私有链接 `go2cpp_migration_format`（复用 `tools/migration` 的
  format/sha256/signature/symbol_table，不编译它的 runtime/CLI）；所有 go2cpp 模块
  关闭 stack protector（挂起帧的 canary 无法跨进程）；新增
  `go2cpp_fiber_adopt_demo` 与反例 `go2cpp_fiber_adopt_demo_alt`，两者都不注册 ctest。

#### 迁移等待路径（关键设计）

`Channel<T>::RecvUntil` 在以下条件同时满足时走迁移路径，否则保持原行为：

- 当前 Fiber 显式调用 `EnableMigration()`；
- 无缓冲（capacity==0）、接收队列为空、未关闭、无 context/deadline、非 nonblocking。

迁移路径**不进入 `ParkingCondition`**：它创建 `PendingRecv`、挂到 `m_receivers`，把
`{channel, node, &pending, kind}` 登记到 Fiber 的迁移槽，然后直接
`Fiber::Suspend(Park)`。这样挂起栈帧里没有 `WaitNode`/Scheduler/Task 等跨进程无效
引用；唤醒由宿主在 `Send` 后调用 `Fiber::resume()` 完成（demo 顺序：导入 → Send →
resume）。`make_shared` 与 deque 入队放在 noinline 辅助函数里，把分配器临时留在
独立栈帧，避免污染导出时的指针审计。

#### 导出 `Fiber::ExportMigrationImage(path, options, error)`

1. 校验：`EnableMigration` + 迁移槽活跃 + 状态 Suspended + `kind==recv` +
   `Channel::ExportWaitState` 成功（无缓冲、恰好一个等待者、无 select、未关闭）；
2. 记录上下文保存区 `m_context`、栈 `[bottom, top)`、栈使用量、等待通道/节点/
   shared_ptr 槽；
3. 以 `{Fiber, Impl, FiberRecord, Channel, WaitNode, WaitControl, StaleCaller,
   WaitSlot}` 为「可重定位对象」，扫描 `[ctx, top)` 每个 8 字节槽：
   - 栈内 → `kRelocStack`（相对 ctx 的偏移）；
   - 已登记对象区间 → `kRelocHandle`（type + 区间内 addend）；
   - 模块内地址（任意 PT_LOAD 段，代码或数据）→ `kRelocSymbol`
     （`module:<realpath>` + 模块内偏移；build-id 哈希写在 `display`）；
   - 其他可写映射/堆 → 默认拒绝（指针审计）；`allow_dead_heap_slots=true` 时
     登记为 `ClearedHeapSlot`，恢复后该槽写 0；
4. 写 G2CMIG03 快照（CRC32 + HMAC-SHA256 + build-id）；`safepoint_id=0`，恢复点由
   上下文保存区的 rip 决定，不需要 safepoint 槽。

#### 导入 `Fiber::ImportMigrationImage(path, options, error)`

1. `ReadSnapshotFile` 校验 magic/version/CRC/HMAC/可执行文件 build-id；
2. 按模块真实路径 + build-id 重新解析代码/数据地址，按句柄重建对象；
3. 新建独立 Fiber（固定栈，容量至少覆盖快照栈字节），把栈拷贝到
   `new_ctx = new_top - stack_bytes`，逐槽重定位：
   `kRelocStack` 相对 ctx、`kRelocSymbol` 模块基址+偏移、`kRelocHandle` 新地址+addend；
4. `Channel::AdoptWaitState` 新建 `PendingRecv` 挂回 `m_receivers`，并在新栈的
   shared_ptr 槽上 placement-new 接管一份真实引用（旧槽字节直接覆盖，不调用旧进程
   控制块的析构）；随后 `ReleaseMigrationHold()`；
5. 设置 `m_context=new_ctx`、`m_stack`、状态 Suspended、`m_migration_enabled=true`；
   不自动 resume。

#### 恢复语义

`resume()` → `resume_locked` 看到 Suspended + 非空 `m_context` → `JumpContext(new_ctx,
this)` 跳回 `Impl::suspend` 的 `JumpContext` 返回点；`suspend()` 收尾把 `m_caller`
刷新为本进程调用者上下文；channel 等待路径重新加锁、读取已就绪节点并返回，用户
Fiber 继续执行到结束，`Impl::entry` 跳回本进程 `resume_locked`。

### 11.7 限制与安全边界

- **只支持接收等待**（`<-ch`）：无缓冲、单生产者单消费者、无 select；发送等待/
  有缓冲/多等待者/已关闭/带 deadline 或 context 一律明确拒绝；
- **必须显式 `EnableMigration()`**；未开启的 Fiber 仍走原 `ParkingCondition` 路径；
- **唤醒由宿主负责**：迁移等待路径不用 Scheduler park 协议，`Send` 只完成节点交接，
  不 resume Fiber；导入方必须显式 `resume()`；
- **旧进程**：导出不改变原 Fiber；旧进程应把该 Fiber 视为已放弃（demo 直接
  `_Exit`），不能继续 resume；
- **canary**：迁移构建必须关闭 stack protector；默认构建不受影响；
- **`allow_dead_heap_slots`**：真实等待路径会留下容器内部死值（deque block、临时
  shared_ptr），默认拒绝导出；受控 demo 显式打开后清零。生产应消除残留或扩展句柄化；
- **模块 build-id**：可执行文件与引用模块必须同 build-id，跨版本明确拒绝；
- **签名**：默认强制 HMAC-SHA256，密钥走 `MIGRATION_SIGNING_KEY[_FILE]`；未签名
  快照拒绝生成/拒绝恢复；
- **未验证**：跨机器、跨架构、调度器自动接入、异常展开中的栈、持有锁的挂起点、
  select/发送等待。

### 11.8 验证

- 跨进程 demo：`./go2cpp_fiber_adopt_demo dump /tmp/x.mig` →
  `restore /tmp/x.mig`；输出「fiber 继续执行，收到消息 = 42」、状态 Completed；
- `selftest` 8 个场景：正常闭环、100 次随机值重复恢复（打印平均耗时）、篡改快照
  拒绝、无密钥导出拒绝、多等待者拒绝、未登记堆指针拒绝、已关闭通道拒绝、其他
  build-id 快照拒绝；
- 默认构建 `ctest` 116/116；`tools/migration` 独立构建场景保持 53/53。

## 12. 签名密钥管理

### 12.1 密钥来源与格式

| 来源 | 格式 | 说明 |
|------|------|------|
| `MIGRATION_SIGNING_KEY` | 原始字节，或 `hex:<hex>` | 适合 CI/临时环境；会出现在 `ps`/`environ` |
| `MIGRATION_SIGNING_KEY_FILE` | 文件内容（去行尾换行）；恰好 64 个十六进制字符按 hex 解 | **生产推荐**：权限 0400，由部署系统注入 |

- 最短 16 字节，建议 32 字节（256 bit）以上随机值；
- key_id = SHA-256(key) 前 16 字节，写进快照；诊断信息只打印 key_id，不打印密钥；
- 生成示例：

```bash
head -c 32 /dev/urandom | xxd -p -c 64 > /etc/go2cpp/migration.key
chmod 0400 /etc/go2cpp/migration.key
export MIGRATION_SIGNING_KEY_FILE=/etc/go2cpp/migration.key
```

### 12.2 校验与错误分类

| 情况 | 结果 | 诊断 |
|------|------|------|
| 正常 | 通过 | `signature=HMAC-SHA256 key_id=... verified=yes` |
| 未签名 | 拒绝 | "快照未签名：生产环境拒绝恢复未签名快照" |
| 缺密钥 | 拒绝 | "未配置签名密钥：无法校验快照签名" |
| key_id 不同 | 拒绝 | "签名密钥不匹配：快照 key_id=...，当前密钥 key_id=..." |
| MAC 不同 | 拒绝 | "签名校验失败：快照内容被篡改，或密钥错误" |
| 配置了非法密钥 | 拒绝（dump/restore 都） | "签名密钥过短"/"十六进制格式非法" |

### 12.3 轮换与运维

1. **轮换**：新旧密钥并存期，恢复侧按 key_id 选择密钥（当前实现只支持单密钥，
   多密钥需要扩展为 keyring）；旧快照要么用旧密钥恢复，要么重新 dump；
2. **审计**：每次 dump/restore 记录 `key_id`、`build-id`、`epoch`、符号表哈希，
   便于追查「用错密钥/用错版本」；
3. **存储**：快照文件本身不含密钥；密钥文件不要与快照放在同一目录/同一权限域；
4. **测试开关**：`MIGRATION_INSECURE_NO_SIGNATURE=1` 只能用于负例测试，
   生产构建应通过编译期开关彻底移除该分支；
5. **威胁模型**：HMAC 密钥是**对称**的，能生成签名的人就能伪造快照；如果
   生成方与恢复方不在同一信任域，应改用 ed25519 非对称签名（格式已预留
   `algorithm` 字段）。

## 13. 结论

- **safepoint id 有效**：v1 布局 dump → v2 布局 restore 成功，指令偏移
  `0x19a` vs `0x1de` 不同，而挂起点槽位相同；
- **lambda 可用**：`MIGRATION_SYMBOL_FN` 生成 demangled 名并注册 `operator()`，
  dump/restore 闭环；
- **句柄表可用**：`std::string` + 自定义 POD 跨进程重建并逐字段校验；
  未登记堆指针仍明确拒绝；
- **签名可用**：HMAC-SHA256 强制校验，错误/缺失/未签名/篡改全部拒绝；
- **真实对象已可迁移（最小子集）**：`GO2CPP_MIGRATION_API=ON` 下
  `Fiber::ExportMigrationImage/ImportMigrationImage` 与 `Channel<T>` 的
  等待状态导出/接纳已落地，跨进程 demo 与 8 场景 selftest 通过；
- **剩余边界**：只支持无缓冲单生产者单消费者的接收等待；唤醒由宿主 resume，
  尚未接入调度器自动唤醒；栈帧布局必须稳定、IPA 克隆必须关闭；
  `allow_dead_heap_slots` 是受控降级，不是安全边界。
