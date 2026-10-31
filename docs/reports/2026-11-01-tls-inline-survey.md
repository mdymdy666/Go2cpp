# TLS 内联（initial-exec）可行性调研报告

- 日期：2026-10-31
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL，只读）
- 构建：`.build-bento-wsl`（Release，GCC 13.3.0，`BUILD_SHARED_LIBS=ON`，`GO2CPP_BUILD_HOOK=OFF`）
- 边界：**只读**。未修改任何源码/构建文件；本报告与中间产物全部在 `/tmp`。

---

## 1. 结论摘要

1. **现状确认**：`023eeb8` 引入的变量级 `tls_model("initial-exec")` 只覆盖了 `scheduler.cpp`、`fiber.cpp`（7 个热变量）、`channel.cpp`（Select 游标）等**已手写属性**的变量；同库其余 TU（`parking_condition.cpp`、`timer.cpp`、`thread_policy.cpp`）仍走 `__tls_get_addr`。
2. **78 处的真实构成**（`libgo2cpp_scheduler.so`，`objdump` 全文 `grep -c __tls_get_addr`）：**78 条 `call __tls_get_addr` 指令**，其中 30 条是 guard 检查（`cmpb $0x0,...(%rax)` 紧随调用），48 条是普通 TLS 数据访问（`local-dynamic`）。
3. **全仓口径**：25 个唯一共享库合计 **421 条** `__tls_get_addr` 调用，其中 guard 94 条、数据 327 条；7 个 `app_test_*` 可执行文件（bento 静态库链入）**全部 0 条**（可执行文件走 local-exec）。
4. **关键实验结论**：变量级 `tls_model` 属性**不能**消除非平凡 `thread_local` 的 guard 调用（`local_vec_attr` 仍 2 条）；只有**整个 TU/目标的编译选项 `-ftls-model=initial-exec`** 才能把 guard 一起内联（`local_vec_flag` 0 条）。
5. **可行性已验证**：对并发/网络 5 个库做影子编译 + 影子链接（不落工作树，产物在 `/tmp`），全部 **0 条 `__tls_get_addr`**，链接成功，无 `TPOFF32` 冲突：
   - scheduler 78 → 0；fiber 10 → 0；io 37 → 0；network 18 → 0；channel 22 → 0。
   - 叠加现网热选项 `-fno-semantic-interposition -fno-plt -fno-stack-protector -Wl,-Bsymbolic-functions` 后仍 0 条，`STATIC_TLS` 标志正常。
6. **主要风险**：initial-exec 会让 `.so` 带上 `DF_STATIC_TLS`，`dlopen` 时占用 glibc 静态 TLS 预算（实测约 1.6 KB）。当前 scheduler/fiber/channel 已带该标志；`so_reloader` 目前只是事务编排层（无真实 `dlopen` 实现，测试用假 loader），`mcp` 插件 0 TLS，因此**现存风险可控，但方案会把该约束扩散到更多库**。
7. **收益**：并发 6 库 + io + network/bufio/p2p 合计 **259 条 → 0**（全仓 421 → 162，降 61.5%）；其中 scheduler 78 → 0。若只覆盖现有 `GO2CPP_CONCURRENCY_HOT_FLAGS` 的 6 个库，则 179 → 0（421 → 242）。
8. **建议**：优先用「目标级 `-ftls-model=initial-exec`」而不是逐变量属性；把它加进现有 `GO2CPP_CONCURRENCY_HOT_FLAGS` 块，并新增 io/network/bufio 列表；对确有析构语义的非平凡 TLS 再单独评估平凡化。

---

## 2. 口径与方法

### 2.1 计数口径

- **调用点**：`objdump -d --no-show-raw-insn <elf>` 中同时含 `call` 与 `__tls_get_addr` 的行，排除 PLT 定义行（`... <__tls_get_addr@plt>:`）。
  - 注意 GCC 13 在部分调用点会生成带前缀序列 `data16 data16 rex.W call __tls_get_addr@plt`；若只匹配 `\tcall` 会漏计（本报告早期版本曾因此把 network 误报为 0，已修正）。
- **guard 判定**：调用后 1–3 条指令内出现 `cmpb $0x0,...(%rax)`，即 `__tls_guard` 检查；否则记为数据访问。
- **唯一库**：`.so`/`.so.0`/`.so.0.1.0` 为同一文件的符号链接/副本，按 `realpath` 去重（25 个唯一库）。
- **TU 级**：`objdump -dr <obj>` 统计 `R_X86_64_TLSGD`（general/local-dynamic，需 `__tls_get_addr`）、`R_X86_64_GOTTPOFF`（initial-exec，已内联）、`R_X86_64_DTPOFF32`（local-dynamic 偏移）、`R_X86_64_TPOFF32`（local-exec，不能进 `.so`）。

### 2.2 复现命令（只读）

```bash
cd /UserData/CodexWorkSpace/Go2cpp
objdump -d --no-show-raw-insn -C .build-bento-wsl/libgo2cpp_scheduler.so > /tmp/s.asm
grep -c __tls_get_addr /tmp/s.asm          # 78（含 PLT/GOT 注释行则更多）
readelf -sW .build-bento-wsl/libgo2cpp_scheduler.so | grep TLS
readelf -dW .build-bento-wsl/libgo2cpp_scheduler.so | grep FLAGS   # STATIC_TLS
```

本次调研的中间数据：`/tmp/tls_survey2/report.json`（逐调用点）、`/tmp/tls_shadow_targets/`、`/tmp/tls_combo/`、`/tmp/tls_attr_test/`、`/tmp/tls_dlopen/`。

---

## 3. `thread_local` 盘点与分类

`grep -rn thread_local src/ include/` 命中 **109 行**（含注释），实际定义约 **45 处**。按目录：

| 目录 | 定义数（约） | 代表 |
|---|---|---|
| `src/concurrency` | 20+ | fiber 栈池、scheduler 当前任务、parking 池、sync 等待节点缓存 |
| `include/go2cpp/concurrency` | 5 | `channel.hpp` 池/游标、`recursive_mutex` token、`fiber_local` |
| `src/network` | 4 | `io.cpp` 的 `t_inside_poll`、`s_node_cache`；`hook.cpp` 4 个（HOOK=OFF 未编译） |
| `src/bento` / `include/go2cpp/bento` | 3 | `ReleaseState`、`log_hook::t_emit_depth`、`random_device` |
| `src/logging` | 1 | `t_submit_depth` |
| `src/debug` | 15 | `ThreadRing`、计数器数组、`HintSlot`、`StagingSlot` |
| `src/os` | 1 | `SignalName` 的 `char[32]` |
| `src/tools` | 1 | `t_error_release_work`（指针） |
| `include/go2cpp/error` | 1 | `ThreadLocalValues`（unordered_map） |
| `tools/replay` | 3 | `t_current_gid`、`t_gid_cache`、`UcontextScheduler::s_current`（inline） |

### 3.1 平凡（POD / 指针 / 整数）——可直接 initial-exec

| 变量 | 位置 | 说明 |
|---|---|---|
| `t_scheduler / t_machine_id / t_processor_id / t_blocking_machine_id / t_in_idle_poll / t_incoming_lock_depth / t_task_block_cache` | scheduler.cpp | 已有 IE 属性；POD 或 `shared_ptr` 常量初始化 |
| `t_current_growable_stack / s_current_fiber / t_thread_hash / s_main_execution / t_errno_slot / t_growable_*` | fiber.cpp | 已有 IE 属性 |
| `NextSelectOffset::state` | channel.cpp | 已有 IE 属性 |
| `s_policy_*`（4 个） | thread_policy.cpp | 平凡但**未标属性** → local-dynamic |
| `MutableThreadTag()::tag` | timer.cpp | POD（8B），未标属性 |
| `t_incoming_stripe`（2 处） | scheduler.cpp | `size_t`，函数内 static，未标属性 |
| `t_inside_poll` | io.cpp | `bool`，全局 default visibility，未标属性 |
| `t_submit_depth` | log.cpp | 全局，未标属性 |
| `t_error_release_work` | tools/error.cpp | 指针，函数内 static |
| `SignalName()::unknown[32]` | os/proc.cpp | 字符数组 |
| `debug_event_ring.cpp` 的计数器/数组/HintSlot | src/debug | 全部 POD |
| `recursive_mutex::token`、`channel.hpp::budget/generation` | 头文件 | 平凡 |

### 3.2 非平凡（有构造/析构 → 需要 guard）——真正的 78 处来源

| 变量 | 位置 | 类型 | 析构语义 |
|---|---|---|---|
| `ParkingCondition::WaitNode::t_pool()::pool` | parking_condition.cpp | `LocalPool{vector<shared_ptr>}` | 线程退出移交进程级回收站 |
| `ParkingCondition::notify_all()::t_wake_list` | parking_condition.cpp | `vector<WaitNode*>` | 无（清空复用） |
| `sync::t_wait_node_cache` | sync.cpp | `vector<shared_ptr<WaitNode>>` | 无（元素析构自动） |
| `fiber::s_thread_stack_pool` | fiber.cpp | `StackCache{vector<CachedStack>}` | **munmap** 所有缓存栈 |
| `fiber::s_thread_fiber_bin` | fiber.cpp | `FiberBin{vector<unique_ptr<Fiber>>}` | 析构 Fiber |
| `fiber_local::s_thread_values` | fiber_local.cpp | `unordered_map<KeyId, shared_ptr<void>>` | 值析构 |
| `context::t_state_release_queue` | context.cpp | `vector<shared_ptr<State>>` | 波次排空（防深递归） |
| `context::t_draining_state_releases` | context.cpp | `bool` | 平凡，但与上者同 TU |
| `io::IOManager::State::s_node_cache` | io.cpp | `vector<pair<State*, NodePtr>>` | 元素析构 |
| `error::detail::ThreadValues()::values` | error_local.hpp | `unordered_map<slot, shared_ptr<void>>` | **波次析构**（防重入） |
| `bento::error::g_release_state` | bento/error.cpp | `ReleaseState{vector<ErrorPtr>}` | 排空错误链 |
| `debug::DebugEventCollector::StagingSlot` | debug_events.hpp | `vector<SchedulerEvent>` | 元素析构 |
| `Channel::NextWaiterGeneration` / `WaitNodePool::pool` | channel.hpp | `vector` / 整数 | 池复用 |
| `token_pairing::random_device` | bento/net | `std::random_device` | 不可平凡化 |
| `fiber::t_next_fiber_name / taken` | fiber.cpp | `std::string` | 字符串 |
| `replay::t_current_gid / t_gid_cache / s_current` | tools/replay | inline，default visibility | 见风险 7.3 |

---

## 4. 反汇编验证

### 4.1 逐库调用数（唯一 `.so`，421 条）

| 库 | calls | guard | data | TLS 字节 | 已有 STATIC_TLS |
|---|---:|---:|---:|---:|---|
| `libgo2cpp_scheduler.so` | **78** | 30 | 48 | 960 | 是 |
| `libgo2cpp_io.so` | 37 | 11 | 26 | 104 | 否 |
| `libgo2cpp_log.so` | 36 | 0 | 36 | 4 | 否 |
| `libgo2cpp_os_monitor.so` | 28 | 6 | 22 | 144 | 否 |
| `libgo2cpp_context.so` | 27 | 6 | 21 | 128 | 否 |
| `libgo2cpp_error.so` | 25 | 3 | 22 | 72 | 否 |
| `libgo2cpp_sync.so` | 24 | 5 | 19 | 104 | 否 |
| `libgo2cpp_channel.so` | 22 | 3 | 19 | 76 | 是 |
| `libgo2cpp_os.so` | 19 | 3 | 16 | 96 | 否 |
| `libgo2cpp_bufio.so` | 18 | 3 | 15 | 64 | 否 |
| `libgo2cpp_compress.so` | 18 | 3 | 15 | 64 | 否 |
| `libgo2cpp_control_flow.so` | 18 | 3 | 15 | 64 | 否 |
| `libgo2cpp_debug.so` | 18 | 3 | 15 | 64 | 否 |
| `libgo2cpp_future.so` | 18 | 3 | 15 | 64 | 否 |
| `libgo2cpp_network.so` | 18 | 3 | 15 | 64 | 否 |
| `libgo2cpp_fiber.so` | 10 | 9 | 1 | 208 | 是 |
| `libgo2cpp_p2p.so` | 7 | 0 | 7 | 0 | 否 |
| 其余 8 个库（config/container/encoding/hash/math/metrics/path/rand 等） | 0 | 0 | 0 | 0–8 | 否 |
| **合计** | **421** | **94** | **327** | 2224 | — |

> 7 个 `app_test_*` 可执行文件全部 **0 条**：bento 静态库进入可执行文件后使用 local-exec，不产生 `__tls_get_addr`。

### 4.2 热点函数（scheduler / io）

```
scheduler.so 78 条：
  12 ParkingCondition::WaitOnce
  11 Scheduler::worker_loop
   7 ParkingCondition::notify_all
   3 ParkingCondition::WaitNode::Release
   3 ScopedThreadParticipation 构造
   2 Scheduler::yield_current / park_current / enter_blocking
   1 Task::wait / wait_for / Scheduler::enqueue / wake_registered / ...
io.so 37 条：
   9 error::detail::ThreadSet
   6 error::detail::ThreadReset
   5 IOManager::State::register_wait
   3 IOManager::State::poll_once / s_node_cache 的 TLS init
   3 error::detail::ThreadGet
```

### 4.3 TU 级分解（决定 78 处的来源）

| 目标文件 | TLSGD | GOTTPOFF | DTPOFF32 | 结论 |
|---|---:|---:|---:|---|
| `scheduler.cpp.o` | **0** | 127 | 0 | 手写 IE 属性生效 |
| `parking_condition.cpp.o` | **15** | 0 | 18 | `t_pool`（非平凡）guard+数据 |
| `timer.cpp.o` | 0 | 0 | 21 | 平凡变量，local-dynamic |
| `thread_policy.cpp.o` | 0 | 0 | 40 | 平凡变量，local-dynamic |
| `fiber.cpp.o` | 0 | 多 | 0 | 7 个热变量已 IE；非平凡 `StackCache/FiberBin` 仍有 guard |
| `io.cpp.o` | **36** | 0 | 0 | `s_node_cache` + `t_inside_poll` + ThreadValues |
| `sync.cpp.o` | **18** | 0 | 0 | `t_wait_node_cache` + ThreadValues |
| `network/socket.cpp.o` | **18** | 0 | 0 | 全部是 `ThreadValues`（error_local 内联实例化） |

**关键**：`network.so` 的 18 条不是 socket 数据面 TLS，而是 `error::detail::ThreadValues()`（`error_local.hpp` 的 `static thread_local unordered_map`）在每个 TU 的实例化。网络库自身几乎无 TLS。

### 4.4 调用点形态

```
12cdd:  lea    0x2ec4c(%rip),%rdi        # GOT 中的 tls_index（local-dynamic 共用）
12ce4:  call   *0x2f1de(%rip)            # __tls_get_addr
12cea:  cmpb   $0x0,0x10(%rax)           # __tls_guard（TLS 偏移 0x10）
12cf1:  je     ...                        # 未初始化则走 __tls_init
```

同一 `.so` 内所有调用共享一个 `tls_index`（local-dynamic 的模块基址），这就是 78 条调用参数相同的原因。

---

## 5. 根因：为什么 initial-exec 只省一半

### 5.1 变量属性 vs 编译选项（对照实验，产物在 `/tmp/tls_attr_test`）

| 变体 | 编译方式 | `__tls_get_addr` |
|---|---|---:|
| `local_vec_plain`（匿名命名空间 `vector<int>`） | 默认 | 4 |
| `local_vec_attr`（变量加 `tls_model("initial-exec")`） | 属性 | **2（guard 仍调用）** |
| `local_vec_flag`（整个 TU 加 `-ftls-model=initial-exec`） | 编译选项 | **0** |
| `global_str_plain`（全局 `string`） | 默认 | 3 |
| `global_str_attr`（变量加属性） | 属性 | **1（guard 仍调用）** |
| `local_int_plain`（平凡 `int`） | 默认 | 1 |
| `local_int_attr`（平凡 `int` + 属性） | 属性 | **0** |

**结论**：
- 平凡变量 + 变量属性 → 0 条；
- 非平凡变量 + 变量属性 → guard 仍走 `__tls_get_addr`（编译器生成的 `__tls_guard` 不继承变量属性）；
- **整个 TU 加 `-ftls-model=initial-exec` → guard 也内联，0 条**。

这正是现状：「initial-exec 只省了一半」不是编译器出错，而是**作用域不够**——属性只作用到变量本体，guard 是编译器生成的独立 TLS 符号（`__tls_guard` / `_ZGV...`），按 TU 默认模型（general/local-dynamic）寻址。

### 5.2 平凡变量也会调用

`timer.cpp`/`thread_policy.cpp` 的 4 个线程策略变量与 `ThreadTimerTag` 都是平凡类型，但在 `-fPIC` 共享库中编译器默认用 **local-dynamic**（`R_X86_64_DTPOFF32` + 一次 `__tls_get_addr` 取模块基址），因此仍有调用。**平凡化只能去掉 guard，不能去掉这层调用**；要归零必须同时有 initial-exec。

---

## 6. initial-exec 可行性评估

### 6.1 现状

- 代码中只有 3 处手写属性：
  - `fiber.cpp:133` `GO2CPP_FIBER_TLS`（7 个变量）；
  - `scheduler.cpp:219` `GO2CPP_SCHED_HOT_TLS`（8 个变量）；
  - `channel.cpp:85` `NextSelectOffset` 的 `state`。
- CMake **没有任何** `-ftls-model` 全局或目标级设置（`grep -rn ftls-model cmake/ CMakeLists.txt` 为空）。
- CMake 已有并发热选项块（`cmake/Go2CppModules.cmake:44-95`）：
  ```cmake
  option(GO2CPP_CONCURRENCY_HOT_FLAGS ... ON)
  set(GO2CPP_HOT_LIB_TARGETS
      go2cpp_context go2cpp_channel go2cpp_scheduler
      go2cpp_fiber go2cpp_sync go2cpp_future)
  # -fno-semantic-interposition / -fno-plt / -fno-stack-protector
  # -Wl,-Bsymbolic-functions
  ```
  这是插入 `-ftls-model=initial-exec` 最自然的位置；但**不含** `go2cpp_io`、`go2cpp_network`、`go2cpp_bufio`、`go2cpp_p2p`。

### 6.2 影子编译 + 影子链接（不落工作树）

用 `compile_commands.json` 的真实命令，替换输出到 `/tmp`，追加 `-ftls-model=initial-exec`，再 `g++ -shared` 链接：

| 目标 | 影子 `.so` 调用数 | 链接 | `TPOFF32` |
|---|---:|---|---:|
| scheduler（4 TU） | **0** | OK | 0 |
| fiber（4 TU） | **0** | OK | 0 |
| io（2 TU） | **0** | OK | 0 |
| network（14 TU） | **0** | OK | 0 |
| channel（1 TU） | **0** | OK | 0 |
| scheduler + 热选项组合（`-fno-semantic-interposition -fno-plt -fno-stack-protector -Wl,-Bsymbolic-functions`） | **0** | OK | 0 |

影子产物 `readelf -d` 显示 `FLAGS: STATIC_TLS`，与现有 scheduler/fiber/channel 一致。

### 6.3 链接兼容性边界

`-ftls-model=initial-exec` 对 **default-visibility 的 inline `thread_local`** 可能生成 `R_X86_64_TPOFF32`，GNU ld 在 `-shared` 下会拒绝：

```
relocation R_X86_64_TPOFF32 against symbol `_ZN6go2cpp6replay6detail13t_current_gidE`
can not be used when making a shared object
```

本仓命中的头文件：
- `tools/replay/include/replay/replay_source.hpp:27` `inline thread_local std::uint32_t t_current_gid`；
- `tools/replay/include/replay/gid.hpp:30` `inline thread_local FiberGidCache t_gid_cache`；
- `tools/replay/include/replay/ucx.hpp:164` `inline thread_local UcontextScheduler* s_current`；
- `include/go2cpp/bento/net/log_hook.hpp:163` `inline thread_local std::uint32_t t_emit_depth`（bento 静态库，当前链进可执行文件，local-exec，不受影响）。

并发/网络 10 个目标都**不包含**上述头文件（影子链接全部通过），因此当前方案安全；若未来这些头文件进入共享库 TU，需要先加 `visibility("hidden")` 或把变量改为函数内 `static`。

---

## 7. 风险

### 7.1 已有 STATIC_TLS 与 `dlopen`

现有 3 个库已带 `DF_STATIC_TLS`（scheduler/fiber/channel），说明「initial-exec + 共享库」在本项目**已经存在**。当前它们由 `DT_NEEDED` 在进程启动时加载，安全。

`so_reloader`（`src/config/so_reloader.cpp`）设计上支持热重载 `.so`，但：
- 它**不含** `<dlfcn.h>`，`dlopen` 由调用方注入 `SoLoader`；
- 全仓未找到生产用 `SoLoader` 实现；`tests/config/test_so_manifest.cpp` 使用**假 loader**（注释明确「不编译、不 `dlopen`」）；
- `mcp/src/plugin/plugin_loader.cpp:270` 有真实 `dlopen(RTLD_NOW|RTLD_LOCAL)`，但加载的是独立 C ABI 插件（`fake_notify_plugin.so` 的 NEEDED 只有 libc/libstdc++/libgcc，TLS 调用 0），不链接 go2cpp 运行时。

### 7.2 静态 TLS 预算（实测）

`dlopen` 带 initial-exec TLS 的 `.so` 会从 glibc 静态 TLS surplus 分配。实验（`/tmp/tls_dlopen`，每个 `.so` 纯 TLS 大小固定）：

| 库 TLS 大小 | 结果 |
|---|---|
| 512 B × 4 | 前 3 个成功，第 4 个 `cannot allocate memory in static TLS block` |
| 1024 B × 4 | 第 2 个失败 |
| 2048 B × 4 | 第 1 个失败 |
| 同尺寸 general-dynamic | 4 个全部成功（动态分配，无此限制） |

→ surplus ≈ **1.6 KB**（glibc 默认 1664 B）。对照本项目：`scheduler.so` TLS **960 B**，单个就可能吃掉大半预算；全部 25 个库合计 **2224 B**，若全部改为 initial-exec 并 `dlopen`，必然失败。

### 7.3 其他风险

| 风险 | 说明 | 缓解 |
|---|---|---|
| 符号可见性/TPOFF32 | 见 6.3 | 不进共享库的头文件 inline TLS 加 hidden |
| `-ftls-model` 与 `-fno-plt` 叠加 | 已实测组合 0 条、链接 OK | — |
| 静态库（bento） | `-ftls-model` 对 `.a` 无意义；链进可执行文件时 local-exec 已最优 | 不需要改 |
| 跨平台 | MSVC 无 `-ftls-model`；项目当前强制 Linux x86_64 | CMake 用 `CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"` 守卫 |
| 调试符号/ABI | `-ftls-model` 不改 ABI，只改代码生成与 `DF_STATIC_TLS` | 可加 per-target 选项开关，随时回退 |

---

## 8. 平凡化可行性

**目标**：把非平凡 TLS 改成「TLS 只存平凡指针 + 堆/静态对象」，从而去掉 guard。但注意 5.2：**平凡化不等于零调用**，还要配合 initial-exec。

### 8.1 适合平凡化的（缓存类，无析构语义或可泄漏）

| 变量 | 改法 | 生命周期 |
|---|---|---|
| `sync::t_wait_node_cache` | `thread_local WaitNode* t_free_head`（POD 自由链表） | 无需析构；节点归还时链回 |
| `parking_condition::t_pool` | 同上（审查报告 #15 已建议） | 线程退出时链表节点可移交进程级回收站（现有 `OrphanPool` 机制） |
| `parking_condition::t_wake_list` | `thread_local WaitNode**` + 固定容量或 `std::vector` 指针（懒分配） | 指针本身平凡；vector 对象可 `new` 后不析构 |
| `io::s_node_cache` | `thread_local std::vector<...>*`（`new`，不析构） | 与 `fiber_local::registry()` 同款「活到进程结束」策略 |
| `debug::StagingSlot` | 同上 | 元素析构丢失，可接受（诊断数据） |
| `channel.hpp::WaitNodePool::pool` | 改 POD 自由链表 | 池复用语义不变 |
| `fiber::t_next_fiber_name` | 改 `thread_local std::string*`（`new`） | 名字仅诊断用，泄漏可接受 |

### 8.2 不适合简单平凡化的（有真实析构语义）

| 变量 | 原因 | 建议 |
|---|---|---|
| `fiber::s_thread_stack_pool` | 析构要 `munmap` 缓存栈，泄漏会永久占虚拟内存 | 保留非平凡 + 目标级 initial-exec → guard 归零 |
| `fiber::s_thread_fiber_bin` | 析构要释放 `unique_ptr<Fiber>` | 同上 |
| `error::ThreadValues()::values` | 波次析构防重入/防泄漏 | 同上；或改 `pthread_key`（复杂） |
| `bento::error::g_release_state` | 线程退出要排空错误链 | 同上 |
| `token_pairing::random_device` | 不可平凡化（libstdc++ 状态） | initial-exec |

**结论**：平凡化是「减少 guard 数量」的补充手段，不是替代方案；对 78 处而言，最省事且零语义变化的是**目标级 initial-exec**。

---

## 9. 清单

### 9.1 可内联（加 `-ftls-model=initial-exec`，零语义变化）

| 目标 | 覆盖文件 | 当前调用 | 改后 |
|---|---|---:|---:|
| `go2cpp_scheduler` | scheduler/parking_condition/timer/thread_policy | 78 | 0 |
| `go2cpp_context` | context.cpp | 27 | 0 |
| `go2cpp_sync` | sync.cpp | 24 | 0 |
| `go2cpp_channel` | channel.cpp | 22 | 0 |
| `go2cpp_fiber` | fiber/fiber_local/context_backend | 10 | 0 |
| `go2cpp_future` | future.cpp | 18 | 0 |
| `go2cpp_io` | io.cpp/io_backend.cpp | 37 | 0 |
| `go2cpp_network` | 14 个 TU | 18 | 0 |
| `go2cpp_bufio` | bufio.cpp | 18 | 0 |
| `go2cpp_p2p`（如算网络层） | p2p | 7 | 0 |
| **合计** | | **259** | **0** |

> 影子链接已验证前 5 个（scheduler/fiber/io/network/channel）；其余为同构目标，风险相同（均不含 6.3 的 inline TLS 头文件）。

### 9.2 需改（非平凡 → 平凡）后才完全归零

| 变量 | 现状 | 改法 |
|---|---|---|
| `ParkingCondition::WaitNode::t_pool` / `notify_all::t_wake_list` | vector + guard | POD 自由链表 / 裸指针 |
| `sync::t_wait_node_cache` | vector + guard | POD 自由链表 |
| `io::State::s_node_cache` | vector + guard | 堆上 vector 指针（不析构） |
| `debug::StagingSlot` / `bento::ReleaseState` | vector + guard | 堆上对象（不析构）或保留 initial-exec |
| `error::ThreadValues` | unordered_map + guard | 保留 initial-exec（不建议平凡化） |
| `fiber::StackCache/FiberBin` | vector + 析构 | 保留 initial-exec |

### 9.3 保留（默认表查，不内联）

| 库 | 调用数 | 原因 |
|---|---:|---|
| `log` | 36 | `std::__once_call`/`__once_callable`（libstdc++）+ `t_submit_depth`；非并发热路径 |
| `error` | 25 | 错误路径；`ThreadValues` 析构语义强 |
| `os_monitor` | 28 | 监控线程，非数据面 |
| `os` | 19 | 系统调用包装 |
| `compress` / `control_flow` / `debug` | 54 | 非热路径（主要是 error_local 内联实例化） |
| `p2p` | 7 | `random_device` 安全随机，保留 general-dynamic 亦可 |

---

## 10. 预期收益

| 方案 | 全仓调用数 | 降幅 |
|---|---:|---:|
| 现状 | 421 | — |
| 仅扩展现有 6 个并发库 | 421 − 179 = **242** | 42.5% |
| 并发 6 库 + io + network + bufio | 421 − 252 = **169** | 59.9% |
| 并发 6 库 + io + network + bufio + p2p | 421 − 259 = **162** | **61.5%** |
| scheduler 单库 | 78 → **0** | 100% |

- 每次访问省 1 次 `__tls_get_addr`（快路径约 10–20 cycle）+ guard 比较。既有审查估算：park/wake 往返触达 6–10 次，收益 10–20 ns/往返；`WaitOnce` 单点 10–15 ns/park。
- 可执行文件（bento 静态库）已是 0，无额外收益。

---

## 11. 实施建议（只读结论，未改代码）

1. **首选方案**：在 `cmake/Go2CppModules.cmake` 现有 `GO2CPP_CONCURRENCY_HOT_FLAGS` 块中增加：
   ```cmake
   check_cxx_compiler_flag(-ftls-model=initial-exec GO2CPP_HOT_HAVE_TLS_IE)
   ...
   if (GO2CPP_HOT_HAVE_TLS_IE)
       list(APPEND hot_target_flags -ftls-model=initial-exec)
   endif()
   ```
   并把 `go2cpp_io`、`go2cpp_network`、`go2cpp_bufio`（可选 `go2cpp_p2p`）加入目标列表，或新建 `GO2CPP_NETWORK_HOT_FLAGS` 块。
2. **保留回退开关**：仿照 `GO2CPP_CONCURRENCY_HOT_FLAGS` 加 `GO2CPP_TLS_INITIAL_EXEC=ON/OFF`，便于在需要 `dlopen` 的场景整体回退 general-dynamic。
3. **不要**继续用逐变量属性扩展——实验证明它清不掉 guard；已手写的属性可保留（无害），新目标统一用编译选项。
4. **验证清单**（落地后）：
   - `objdump -d --no-show-raw-insn .build-*/libgo2cpp_scheduler.so | grep -c __tls_get_addr` → 0；
   - `readelf -d ... | grep FLAGS` → 含 `STATIC_TLS`；
   - `ctest -R 'fiber|channel|scheduler|sync|io|network'` 全绿；
   - ASan/TSan 各跑一轮（TLS 模型不改变语义，但 guard 移除属代码生成变化，需回归）；
   - `dlopen` 专项：若启用 so_reloader 真实 loader，先确认目标 `.so` TLS 总量 < 1.6 KB 且不与其他 initial-exec 库叠加。
5. **文档**：落地后更新 `docs/reports/2026-11-01-asm-concurrency-review.md` #3/#15 的结论（「属性无法清 guard，编译选项可以」），并在此报告基础上补实测数字。

---

## 12. 附录：证据索引

| 内容 | 位置 |
|---|---|
| 逐调用点 JSON（421 条） | `/tmp/tls_survey2/report.json` |
| 影子 `.so`（scheduler/fiber/io/network/channel） | `/tmp/tls_shadow_targets/` |
| 影子链接（scheduler 4 TU） | `/tmp/tls_shadow_link/` |
| 热选项组合验证 | `/tmp/tls_combo/libsched_combo.so` |
| 属性 vs 编译选项实验 | `/tmp/tls_attr_test/` |
| `dlopen` 预算实验 | `/tmp/tls_dlopen/` |
| 全仓 thread_local 清单 | `grep -rn thread_local src/ include/ tools/`（109 行） |
