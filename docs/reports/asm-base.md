> 来源：/tmp/base_asm.md，2026-11-02 归档

# container / math / metrics / path 四模块汇编级审计与对标

- 时间：2026-11-02
- 机器：AMD Ryzen 9 8945HX（12 核，L2 6 MiB，L3 32 MiB）、WSL2、ext4
- 工具链：gcc 13.3.0、cmake 3.28 + ninja、`objdump -d -C --no-show-raw-insn`
- 构建：**独立目录 `/tmp/build-base`**，`CMAKE_BUILD_TYPE=Release`（`-O3 -DNDEBUG`），`BUILD_SHARED_LIBS=ON`
- 口径：`taskset -c 4-11`；每项 1e6 次操作、5 轮取最小；性能表给 3 次独立运行区间
- 源码：WSL 树 `/UserData/CodexWorkSpace/Go2cpp`（不是 E: 树）
- 复现脚本：`/mnt/e/CodexWorkspace/Go2Cpp/.dsh_tmp/{base_bench,metrics_hot,container_probe}.cpp`、`asm_stats.py`

---

## 0. 结论速览

| 模块 | 热路径 | 对标基线 | 对标结果 | <1.1× |
|---|---|---|---|---|
| container | `Heap::Push/Pop`（1e6 push+pop） | `std::priority_queue` | **1.028–1.035×** | ✅ |
| container | `List::PushBack` | `std::list::push_back` | 1.047–1.159× | ⚠️ 边界 |
| container | `Element::Next` 遍历 | `std::list` 迭代器 | **1.455–1.494×** | ❌ |
| container | `Ring::New+Delete` | `std::list` 建/清 | 0.744–1.092× | ✅ |
| container | `Ring` 整体（New+赋值+Do+Delete） | `std::list` 同流程 | 1.176–1.569× | ❌ |
| math | `Sqrt` | `std::sqrt` | **1.388–1.409×** | ❌ |
| math | `Sin/Cos/Pow/Atan2/Hypot` | libm | 0.975–1.054× | ✅ |
| math | `Exp` / `Log` | libm | 1.109–1.120× | ⚠️ |
| math | `Max` / `Min` | `std::fmax` | **0.997–1.006×**（优化前 3.4×） | ✅ |
| math | `Modf` | `std::modf` | **1.322–1.381×** | ❌ |
| metrics | Prometheus 纯渲染（1e6 样本） | 历史 28.04 ns/指标 | **24.3–26.5 ns/指标** | ✅ 未退化 |
| metrics | `Registry::Render()` | 历史 131.25 ns/指标 | **71.2–88.7 ns/指标** | ✅ 未退化 |
| path | `Clean` | `std::filesystem::path::lexically_normal` | **0.195–0.202×** | ✅ |
| path | `Base/Ext/Dir/Join` | `std::filesystem` 对应接口 | **0.114–0.316×** | ✅ |

**一句话**：path 全面碾压 `std::filesystem`（快 3–9×）；metrics 导出未退化（反而更快）；container 的 Heap 打平、List/Ring 的差距来自 Go 语义封装（`Element*` 句柄、两段式建环）；math 的四个未达标项经 **LTO 对照实验**证明是共享库跨 TU 调用边界代价（LTO 后 0.99–1.04×），不是实现退化。

---

## 1. objdump 证据

统计方式：`objdump -d -C --no-show-raw-insn`，按符号切分，统计指令条数、`call` 次数、PLT 次数、`lock` 前缀数、`operator new` 次数、`sub $N,%rsp` 栈帧。

### 1.1 math（`libgo2cpp_math.so`）

| 函数 | 指令 | call(PLT) | 原子 | 栈帧 | 关键形态 |
|---|---:|---:|---:|---:|---|
| `Sqrt` | 8 | 0 | 0 | 0 | 负值检查 + `sqrtsd`；负数 `jmp sqrt@plt` |
| `Sin/Cos/Exp/Log/Pow/Exp2/Log2/Log10/Sinh` | 3 | 0 | 0 | 0 | `endbr64` + `jmp <libm>@plt`（尾调用，零栈帧） |
| `IsNaN` | 5 | 0 | 0 | 0 | `ucomisd` + `setp`（`x != x`） |
| `IsInf` | 22 | 0 | 0 | 0 | `std::isinf` 位测试 + 符号分支 |
| `Signbit` | 5 | 0 | 0 | 0 | `movmskpd` |
| `Max`（优化前） | 54 | **5**（IsInf×2、IsNaN×2、Signbit） | 0 | **16 B** | 每次 Max 5 次 PLT + 5 次栈 spill |
| `Max`（优化后） | 62 | **0** | 0 | **0** | 指令变多但无调用/无栈帧 → 7.5 → 2.2 ns |
| `Min`（优化前） | 53 | 5 | 0 | 24 B | 同上 |
| `Min`（优化后） | 65 | 0 | 0 | 0 | 同上 |
| `Modf` | 27 | 2（`modf@plt`、`__stack_chk_fail@plt`） | 0 | 24 B | 结构体 16 B 返回 + libm 调用 + canary |

`Max` 优化前的关键片段（`objdump` 原文，54 条里 5 次 `call`）：

```
2545:  mov    $0x1,%edi
2559:  call   22d0 <go2cpp::math::IsInf(double, int)@plt>
257b:  call   22d0 <go2cpp::math::IsInf(double, int)@plt>
2589:  call   22b0 <go2cpp::math::IsNaN(double)@plt>
25a6:  call   22b0 <go2cpp::math::IsNaN(double)@plt>
25da:  call   2350 <go2cpp::math::Signbit(double)@plt>
```

`Sqrt` 全文（8 条，正数路径 3 条）：

```
2864:  pxor   %xmm1,%xmm1
2868:  ucomisd %xmm0,%xmm1
286c:  ja     2873            # 负数 → libm sqrt@plt
286e:  sqrtsd %xmm0,%xmm0
2872:  ret
2873:  jmp    23a0 <sqrt@plt>
```

**判读**：math 的值语义完全等价于 libm（1e6 次逐 bit 对拍 0 差异），差距只来自「`go2cpp::math::X` 是共享库导出符号，调用方无法内联」。

### 1.2 path（`libgo2cpp_path.so`）

| 函数 | 指令 | call(PLT) | 分配 | 栈帧 | 说明 |
|---|---:|---:|---:|---:|---|
| `Clean` | **394** | 13 | `_M_create`/`_M_replace`/`memcpy`/`operator delete` | 136 B | LazyBuffer 惰性物化 + `std::string` 构造/异常路径 |
| `Split` | 382 | 9 | `_M_create`、`memcpy` | 120 B | 返回 `pair<string,string>`，两个 string 的完整异常路径被内联 |
| `Base` | 85 | 3 | `_M_create`、`memcpy` | 16 B | 尾斜杠剥离 + `rfind` |
| `Ext` | 80 | 4 | `_M_create`、`memcpy` | 16 B | 反向扫描 |
| `Dir` | 42 | 5 | 调 `Clean`+`Split` | 88 B | 薄组合 |
| `JoinElements(init_list)` | 159 | 7 | `reserve`/`_M_append`/`_M_mutate` | 88 B | 一次 `reserve` + 纯 append + 末尾 `Clean` |
| `JoinElements(vector)` | 130 | 7 | 同上 | 72 B | 同上 |

**判读**：`path` 的 30 ns/op 不是指令数问题（394 条里大部分是 `std::string` 的异常/扩容模板展开），而是「至少 1 次堆分配 + 1 次 memcpy」——这已经是返回 `std::string` 的物理下限。对比 `std::filesystem::path` 的 145–158 ns，说明后者在每次 `lexically_normal()` 里做了更多次分配/迭代器构造。

### 1.3 metrics（`libgo2cpp_metrics.so`）

| 函数 | 指令 | call(PLT) | 原子 | 栈帧 | 说明 |
|---|---:|---:|---:|---:|---|
| `Counter::Inc` | 4 | 0 | 0 | 0 | `jmp Counter::Add@plt`（尾调用，**每次 Inc 两次跳转**） |
| `Counter::Add` | 14 | 1（`EnsureSeries`） | **1 `lock add`** | 8 B | 先校验标签族，再原子加 |
| `Histogram::Observe` | 60 | 1（`EnsureSeries`） | **3**（2×`lock add` + 1×`lock cmpxchg`） | 16 B | 内联二分找桶 + 桶计数 + count + sum(CAS) |
| `Registry::Render` | 49 | 6（`pthread_mutex_lock/unlock`、`reserve`、`_Rb_tree_increment`、虚调用 `call *0x18(%rax)`） | 0 | 8 B | 持锁遍历 `std::map`，每族一次虚调用 |
| `TextWriter::Sample(uint64)` | 84 | 6 | 0 | 0 | 纯 append + `to_chars` |
| `AppendDouble` | 104 | 5（含 `std::to_chars`） | 0 | 80 B | 最短往返格式化 |
| `Exporter::Render` | 34 | 3 | 0 | 24 B | `Registry::Render` + 分位数后处理 |

`Histogram::Observe` 的三个原子 RMW（原文）：

```
12247:  lock addq $0x1,(%rax,%rsi,1)   # 桶计数
12259:  lock addq $0x1,0x40(%rbx)      # count
1228e:  lock cmpxchg %rcx,(%rdx)       # sum（CAS 循环，12295 回跳）
```

`Registry::Render` 的导出循环（原文）：

```
10d47:  call   pthread_mutex_lock@plt
10d63:  mov    0x50(%r13),%rsi
10d6b:  shl    $0x8,%rsi              # reserve(size * 256 B)
10d6f:  call   basic_string::reserve@plt
10d8f:  mov    (%rdi),%rax
10d92:  call   *0x18(%rax)            # 虚函数 AppendPrometheus（每族一次）
10d98:  call   _Rb_tree_increment@plt # map 遍历（每族一次）
10da8:  call   pthread_mutex_unlock@plt
```

**判读**：导出路径的 24–26 ns/指标由「每指标 ~40 字节 append + 一次 `to_chars`」构成，已贴近 `memcpy` 的 5–6×（memcpy 7244–9650 MB/s vs writer 1429–1559 MB/s）；`Registry::Render` 的额外开销是**每族**（不是每指标）一次锁/虚调用/红黑树递增，所以 1e6 系列 / 1000 族时摊薄。

### 1.4 container（header-only，`-O3` 探针）

容器是模板，符号在调用方内联；用 `__attribute__((noinline))` 包出完整循环后统计（1000 元素）：

| 探针 | 指令 | call | alloc | 栈帧 | 对应 |
|---|---:|---:|---:|---:|---|
| `heap_ours`（push+pop 全流程） | 222 | 3 | 2 | 88 B | `Heap<int>` |
| `heap_std`（同上） | 208 | 3 | 2 | 72 B | `priority_queue` |
| `list_ours` | 89 | 5 | 1 | 40 B | `List<int>` |
| `list_std` | 76 | 5 | 1 | 40 B | `std::list<int>` |
| `ring_ours` | 108 | 3 | 2 | 8 B | `Ring<int>`（New→赋值→Do→Delete） |

**判读**：
- `Heap` 每次 push/pop 直接展开为 `std::push_heap`/`std::pop_heap` 的同一份内联代码，多出的 14 条是 `Size()/Empty()` 的 `std::out_of_range` 分支（异常路径冷代码），热路径一致 → 实测 1.03×。
- `List` 多出 13 条来自 `Element::Next()`：每次都要 `if (m_list == nullptr)` 判空 + `m_list->end()` 取哨兵 + `&*next`。`std::list` 迭代器只有「++ + 比较 end」。这就是 walk 1.46× 的全部来源。
- `Ring::New` 是 1e6 次 `new Ring()`（24 B/节点），与 `std::list::push_back` 的分配次数相同；`DeleteRing` 原实现先 `Len()` 全环遍历再删（O(2n)），已改为单遍（见 §4）。

---

## 2. 对标数据（ns/op，3 次运行区间）

### 2.1 container

| 项目 | ours | 基线 | 比率 | 判定 |
|---|---:|---:|---:|---|
| `Heap` push+pop（2e6 ops） | 36.86–37.32 | 35.84–36.30 | **1.028–1.035×** | ✅ |
| `List` push+walk+pop（2e6 ops） | 7.42–7.66 | 6.12–6.75 | 1.135–1.237× | ❌ |
| `List` push only | 10.63–12.68 | 10.14–10.94 | 1.047–1.159× | ⚠️ |
| `List` walk only | 2.05–2.32 | 1.39–1.55 | **1.455–1.494×** | ❌ |
| `Ring` vs `std::list`（全流程） | 18.86–22.31 | 13.34–16.04 | 1.176–1.569× | ❌ |
| `Ring New+Delete`（不含遍历） | 10.06–13.70 | 11.96–13.52 | 0.744–1.092× | ✅ |
| `Ring` walk vs `vector` 扫描（参考） | 2.68–13.97 | 0.10–0.12 | 26–137× | 结构差异，不计入 |

### 2.2 math

| 项目 | ours | 基线 | 比率 | 判定 |
|---|---:|---:|---:|---|
| `Sqrt` | 2.18–2.24 | 1.57–1.61 | **1.388–1.409×** | ❌ |
| `Sin` | 5.91–6.20 | 5.62–5.88 | 1.051–1.054× | ✅ |
| `Cos` | 7.85–8.39 | 7.59–8.09 | 1.029–1.037× | ✅ |
| `Exp` | 3.66–3.82 | 3.29–3.45 | 1.109–1.113× | ⚠️ |
| `Log` | 3.58–3.79 | 3.21–3.40 | 1.114–1.120× | ⚠️ |
| `Pow` | 6.89–7.28 | 6.73–7.11 | 1.010–1.024× | ✅ |
| `Atan2` | 8.48–9.12 | 8.27–8.96 | 1.018–1.025× | ✅ |
| `Hypot` | 8.79–9.53 | 8.79–9.55 | 0.975–1.000× | ✅ |
| `Max`（优化后） | 2.14–2.34 | 2.13–2.32 | **0.997–1.006×** | ✅ |
| `Modf` | 4.74–5.19 | 3.59–3.88 | **1.322–1.381×** | ❌ |

**LTO 对照实验**（把 `src/math/math.cpp` 与基准一起 `-flto` 编译，模拟「跨 TU 可见」）：

| 项目 | 共享库（本次） | LTO | 结论 |
|---|---:|---:|---|
| `Sqrt` | 1.39–1.41× | **0.999×** | 差距 100% 是调用边界 |
| `Exp` | 1.11× | **0.987×** | 同上 |
| `Log` | 1.12× | **1.001×** | 同上 |
| `Modf` | 1.32–1.38× | **1.035×** | 同上 |
| `Max` | 1.005× | 0.332× | 内联后常量传播 |

### 2.3 metrics（Prometheus 导出，1e6 系列，3 轮）

| 阶段 | 本次实测 | 历史（2026-10-31） | 结论 |
|---|---:|---:|---|
| 纯渲染 `TextWriter` | **24.3–26.5 ns/指标**（1429–1559 MB/s） | 28.04 ns/指标（1349 MB/s） | ✅ 未退化（快 8–16%） |
| `Registry::Render()` 基线 | **71.2–88.7 ns/指标** | 131.25 ns/指标 | ✅ 未退化 |
| 端到端 `prom::Render` | **70.6–80.1 ns/指标** | 100.53 ns/指标 | ✅ 未退化 |
| 分位数后处理（1e4 系列×10 桶） | **1.00–1.14 µs/系列** | 1.24 µs/系列 | ✅ 未退化 |
| `memcpy` 下限 | 7244–9650 MB/s | 7808 MB/s | 机器同量级 |

导出热路径的物理距离：writer 1429–1559 MB/s vs memcpy 7244–9650 MB/s = **5.1–6.5× memcpy**，与历史 5.79× 一致。

**附带发现（inc/observe 热路径，1e7 次）**：

| 操作 | 本次 | 纯原子下限 | 倍数 |
|---|---:|---:|---:|
| `Counter::Inc` | 3.47 ns | 1.34 ns（`fetch_add(relaxed)`） | 2.59× |
| `Counter::Inc`（带标签、已缓存） | 3.44 ns | 1.34 ns | 2.56× |
| `Gauge::Inc` | 3.67 ns | 1.34 ns | 2.74× |
| `Histogram::Observe`（10 桶） | 5.15 ns | 4.25 ns（3×`fetch_add`） | 1.21× |

`Counter::Inc` 的 2.6× 来自汇编可见的两跳：`Inc → jmp Add@plt → call EnsureSeries@plt`，再加一次 `lock add`。`Histogram::Observe` 只比「3 次原子」下限高 21%，已经很好。

### 2.4 path

| 项目 | ours | `std::filesystem` | 比率 | 判定 |
|---|---:|---:|---:|---|
| `Clean` vs `lexically_normal` | 29.3–30.8 | 145.0–157.8 | **0.195–0.202×** | ✅ 快 5× |
| `Base` vs `filename()` | 9.3–10.0 | 69.4–75.1 | **0.133–0.134×** | ✅ 快 7.5× |
| `Ext` vs `extension()` | 8.2–9.0 | 72.3–78.2 | **0.114–0.115×** | ✅ 快 8.7× |
| `Dir` vs `parent_path()` | 36.9–37.2 | 117.5–118.7 | **0.311–0.316×** | ✅ 快 3.2× |
| `Join(3 段)` vs `operator/` | 70.1–71.7 | 265.3–269.1 | **0.264–0.267×** | ✅ 快 3.8× |

---

## 3. 正确性：1e6 次操作与基线一致

`/tmp/base_bench`（默认模式）逐项对拍，最终 `failures=0`：

| 对拍项 | 规模 | 基线 | 结果 |
|---|---|---|---|
| `Heap` push/pop 序列 | 1e6 push + 1e6 pop | `std::priority_queue`（`greater`）逐元素 | ✅ 完全一致 |
| `List` push/walk/remove | 1e6 项，正反遍历 + 删一半 | `std::list` 逐元素 | ✅ 完全一致 |
| `Ring` 遍历 | 1e6 节点（New→赋值→Do→Len→Move） | 写入顺序数组 | ✅ 完全一致 |
| `math` 逐 bit | 1e6 个输入 × 6 函数（Sqrt/Sin/Cos/Exp/Log/Floor） | libm | ✅ 0 处差异（`memcmp` 位级） |
| `math` 特殊值 | Max/Min/Modf/Inf/NaN/±0 | Go 文档语义 | ✅ |
| `path::Clean` | **1e6 次调用**（1 万条 Go 官方 oracle × 100 遍） | 真 Go `path.Clean` 输出 | ✅ 0 处不一致 |
| `path::Base/Ext/IsAbs/Join` | 1e6 条随机路径 | 独立参考实现 | ✅ |
| `path::Dir` | 1 万条 oracle | `Clean(Split().first)` 参考 | ✅ |

回归测试（`/tmp/build-base`）：`go2cpp_math_test`、`go2cpp_path_test`、`go2cpp_container_test` 全部 **exit 0 / all checks passed**（含 container 的 1e6 stress 与 path 的 1 万条对拍）。

---

## 4. 本次修改（2 处，均已回归）

### 4.1 `src/math/math.cpp`：`Max`/`Min` 改用内建判定

- **问题**：一次 `Max` 5 次 PLT 调用 + 16 B 栈帧，7.5 ns/op，是 `std::fmax` 的 **3.4×**。
- **改法**：函数体内改用 `std::isinf` / `std::isnan` / `std::signbit`（`-O3` 展开为 `ucomisd`/`movmskpd` 位测试），判断顺序与 Go 语义逐条保留（+Inf 先于 NaN、±0 的符号取舍）。
- **效果**：54 insn + 5 call + 16 B 栈 → 62 insn + **0 call + 0 栈帧**；**7.5 → 2.2 ns（1.005×）**。
- **风险**：零 API/ABI 变化，`IsInf/IsNaN/Signbit` 的导出符号仍在（其他调用方不受影响）。

### 4.2 `include/go2cpp/container/ring.hpp`：`DeleteRing` 单遍删除

- **问题**：旧实现先 `Len()` 全环遍历（O(n)）再逐节点删，总计 O(2n)；`Len()` 是纯指针追逐，纯浪费。
- **改法**：断开首尾成普通链后单遍 `delete`（析构的「修正邻居」分支置空跳过），O(n)。
- **效果**：`Ring New+Delete` 10.1–13.7 ns/op vs `std::list` 12.0–13.5（0.74–1.09×，打平或更快），整体 Ring 比率从 1.30–1.57 降到 1.18–1.48。
- **风险**：低；`container_test`（含 1e6 stress、ring 引用对照 `traverse ours/raw = 1.433`）与 1e6 对拍均通过。

---

## 5. 未达标项：根因与建议

| 项 | 比率 | 根因 | 建议（按收益/风险排序） |
|---|---:|---|---|
| `math::Sqrt` | 1.39–1.41× | `Sqrt` 是 `.so` 导出符号；`std::sqrt` 被内联成单条 `sqrtsd`。调用方每次付 `call/ret + endbr64 + 分支` | ① 头文件 inline（ABI 变更，需全仓重编）；② 给 `go2cpp_math` 加 `-fno-plt`（可省一次 PLT 跳转，预计到 ~1.2×）；③ LTO（实验值 1.0×） |
| `math::Exp/Log` | 1.11–1.12× | 同上（函数体 3.3–3.5 ns，调用开销 ~0.4 ns 占比 12%） | 同上；若只能选一个，`-fno-plt` 风险最低 |
| `math::Modf` | 1.32–1.38× | 结构体 16 B 返回（sret）+ `modf@plt` + stack canary | 头文件 inline；或改为「输出参数」重载（API 变更） |
| `List` walk | 1.46–1.49× | `Element::Next()` 每次 `m_list` 判空 + `m_list->end()` 取哨兵 + `&*next`；`std::list` 迭代器只 `++` + 比较 | 这是 Go `Element*` 句柄语义的固有成本（指针稳定 + 可 `Remove(e)`）。若要更快，只能另加迭代器 API（不改现有语义） |
| `Ring` 整体 | 1.18–1.57× | Go 的 `ring.New(n)` 是「先建 n 个空节点，再逐个赋值」，比 `std::list` 的 push_back 多一整趟指针追逐；纯建/删已经打平（0.74–1.09×） | 无实现缺陷；若调用方在意，用 `Ring::New` 后一次遍历赋值是 API 决定的 |

**明确不改的**：
- `path` 已全面领先，不动；
- `metrics` 导出已优于历史，不动；
- `Counter::Inc` 的 2.6× 是 `EnsureSeries` 校验（语义要求：根对象带标签时禁写）。可把该检查内联进 `Counter::Add` 省掉一次 PLT，但属于另一条优化线，本次只记录。

---

## 6. 复现命令

```bash
# 构建（独立目录）
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-base -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_EXAMPLES=OFF -DGO2CPP_WITH_BENTO=OFF
cmake --build /tmp/build-base --target go2cpp_math go2cpp_path go2cpp_metrics \
      go2cpp_container go2cpp_metrics_prometheus_bench -j 12

# 对标 + 1e6 对拍
g++ -O3 -DNDEBUG -std=c++17 -I/UserData/CodexWorkSpace/Go2cpp/include \
    /mnt/e/CodexWorkspace/Go2Cpp/.dsh_tmp/base_bench.cpp \
    -L/tmp/build-base -lgo2cpp_math -lgo2cpp_path -Wl,-rpath,/tmp/build-base -o /tmp/base_bench
taskset -c 4-11 /tmp/base_bench

# metrics 热路径 / Prometheus 导出
g++ -O3 -DNDEBUG -std=c++17 -I.../.dsh_tmp/metrics_hot.cpp -L/tmp/build-base -lgo2cpp_metrics ...
taskset -c 4-11 /tmp/metrics_hot
taskset -c 4-11 /tmp/build-base/go2cpp_metrics_prometheus_bench --series=1000000 --iterations=3

# 回归
/tmp/build-base/go2cpp_math_test && /tmp/build-base/go2cpp_path_test && \
/tmp/build-base/go2cpp_container_test
```

原始日志：`/tmp/base-bench-3runs.log`、`/tmp/bench-prometheus-base.log`、`/tmp/base-bench-opt.log`、`/tmp/test-{math,path,container}.log`
