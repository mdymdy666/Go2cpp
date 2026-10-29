# error / reflect 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/error/{error,registry,mapping}.hpp` +
  `src/bento/error/*.cpp`、`include/go2cpp/bento/reflect/{reflect,caster,bind}.hpp`、
  `tests/bento/{test_error,test_reflect}.cpp`
- 审计工具：`tests/bento/audit_error_reflect.cpp`（独立程序，不注册 CTest，
  与既有 `audit_*_stress.cpp` 一致）
- 构建：`.build-bento-wsl`（Ninja，Release，`-Wall -Wextra -Wpedantic -Werror`）
- 环境：WSL2 Ubuntu 24.04，AMD Ryzen 9 8945HX（12 vCPU），GCC 13.3.0；
  宿主 load average ≈1.6（有其它进程，绝对值有 ±5–10% 噪声）
- 结论：**无 P0**。修复 5 个 P1（全部是分配/复制瓶颈）、1 个 P2（TLS 释放
  队列析构兜底）；记录 4 个 P2（接口契约/极端场景）与 5 个 P3。补 10 个单测
  用例（+168 行）。**1200 棵随机图、6 个真实场景、百万级压测、ASan+UBSan、
  TSan 全部通过**；`ctest -R bento` **48/48**（`bento_netem` 因环境缺 netem
  权限 Skipped，非本模块）。

## 1. 压测数据

条件：同一审计程序分别链接 **HEAD 源码** 与 **当前源码**，同环境交替各跑 3 轮
取中位数（`-O2 -DNDEBUG`，单线程；JSON/Bind 为 20 万次，其余 100 万次）。
未改路径（`Wrap`、caster、`MapHttpStatus`、`DefaultJson`）漂移 +0%～+8%，
可视作噪声基线；据此判断下述降幅均超出噪声。

### 1.1 查询与构造（8 节点错误图，单位 ns/op）

| 操作 | HEAD | 当前 | 变化 | alloc/op（HEAD→当前） |
|---|---:|---:|---:|---:|
| `Wrap(leaf)`（单叶） | 17.38 | 18.08 | +4.0%（噪声） | 1→1 |
| `Join({leaf,leaf_hit})` | 48.02 | 30.11 | **−37.3%** | 4→2 |
| `As<StringError>` 命中 | 157.00 | 109.20 | **−30.4%** | 9→5 |
| `As<TaggedError>` 命中 | 286.79 | 200.26 | **−30.2%** | 同上 |
| `As<AppSqlError>` 未命中 | 307.48 | 212.34 | **−30.9%** | 同上 |
| `Has<TaggedError>` | 286.52 | 198.83 | **−30.6%** | 9→5 |
| `Has(code)` 命中 | 172.56 | 106.90 | **−38.1%** | 13→6 |
| `Has(code)` 未命中 | 243.52 | 144.09 | **−40.8%** | 13→6 |
| `Is(root, leaf)` | 133.24 | 83.01 | **−37.7%** | 13→6 |
| `WalkErrors`（8 节点） | 298.48 | 201.13 | **−32.6%** | 17→10 |
| `UnwrapAll(0)`（8 节点） | 242.69 | 155.48 | **−35.9%** | — |
| `ErrorMessage`（8 节点） | 160.87 | 158.87 | −1.2% | — |
| `TypeRegistry.Create` | 35.19 | 27.15 | **−22.8%** | — |
| `TypeRegistry.Has` | 19.57 | 11.54 | **−41.0%** | 1→0（临时 string） |
| `TypeRegistry.Depth` | 10.95 | 11.74 | +7.2%（噪声） | 1→0 |
| `TypeRegistry.ParentName` | 19.47 | 11.45 | **−41.2%** | 1→0 |
| `Bind`（5 字段） | 145.70 | 105.35 | **−27.7%** | 每次 6→0（字段表） |
| `FieldList::Fields()` 重建（对照） | 34.31 | 33.97 | −1.0% | — |

延迟分位数（当前实现，3 轮中位数）：`As<StringError>` P50/P95/P99 =
108.5/115.0/122.4 ns（P99/P50=1.13×）；`WalkErrors` = 200.2/208.8/229.4
（1.15×）；`Bind` = 104.2/118.7/129.3（1.24×）；`Wrap` = 17.6/19.9/23.2
（1.32×，小对象分配主导）。全部 P99/P50 ≤1.32×。

### 1.2 深链（10 万层 Wrap）

| 项目 | HEAD | 当前 | 变化 |
|---|---:|---:|---:|
| `As<StringError>` | 71.1 ns/node | 59.1 ns/node | **−16.9%** |
| `Has(code)` | 62.8 ns/node | 49.3 ns/node | **−21.5%** |
| `WalkErrors` | 71.9 ns/node | 54.9 ns/node | **−23.6%** |
| `Message` | 9.4 ns/layer | 8.8 ns/layer | **−6.4%** |
| `UnwrapAll(0)` ×5 | 34.49 ms | 25.97 ms | **−24.7%** |
| 构造 10 万层 | 57.6 ns/layer | 57.6 ns/layer | 持平 |

### 1.3 宽 Join（10 万元素）

| 项目 | HEAD | 当前 | 变化 |
|---|---:|---:|---:|
| `UnwrapAll(0)` ×5 | 27.33 ms | 19.70 ms | **−27.9%** |
| `WalkErrors` | 5.50 ms | 4.24 ms | **−22.9%** |
| `Message`（10 万行/689 KB） | 1.83 ms | 1.82 ms | 持平 |

### 1.4 内存与并发

- 30 万层 Wrap 链：RSS 增量 **11.1 MB（38.9 B/层**，`make_shared` 单次分配 +
  短字符串 SSO）；释放后 RSS 不回落（glibc 保留 arena，非泄漏）。
- 20 万宽 Join：RSS 增量 **11.1 MB（58.3 B/原因）**。
- 10 万层 `UnwrapAll(0)` 单次产生 10 万个 `shared_ptr`（1.6 MB vector），
  属接口契约成本（见 P2-3）。
- 并发：8 线程 × 25 万次混合（Wrap/Is/Has/Walk）= 200 万 ops，
  failures=0；HEAD 514–596 万 ops/s，当前 513–1072 万 ops/s（宿主负载波动，
  单次峰值 1072 万；无回归）。
- ASan+UBSan（`-O1 -g`，`detect_leaks=1`）：**219,871 checks / 0 failures**，
  0 条 sanitizer 报告；TSan 同规模：0 data race、0 failures。

### 1.5 确定性分配数（与 CPU 频率无关，最硬的证据）

| 操作 | HEAD alloc/op | 当前 alloc/op |
|---|---:|---:|
| `Wrap` | 1 | 1 |
| `Join` | **4** | **2** |
| `As<T>` | **9** | **5** |
| `Has(code)` | **13** | **6** |
| `WalkErrors` | **17** | **10** |

## 2. 随机测试（1000+）

| 项目 | 规模 | 结果 |
|---|---|---|
| 随机错误图 | **1200 棵**（深度 1–10，宽度 0–8，20% 共享子图，25% 重复原因，5% 自环） | 214,150 checks / 0 failures |
| 随机深链 | 12 棵，2,000–20,000 层 | `Is`/`Has`/`As`/`Walk` 数量与首尾节点全部正确 |
| 随机宽 Join | 12 棵，1,000–5,000 原因 | `UnwrapAll(0/1)` 数量、`Message` 行数、`Walk` 节点数全对 |
| 双节点环 | 200 个（weak 互指 + 尾节点） | `Is`/`Has`/`As`/`Walk`/`JSON` 均有界、不崩 |

每棵随机图与独立参考实现（递归前序 DFS + 地址去重）逐项比对：
`WalkErrors` 的**顺序与数量**、`As<T>` 的**首个匹配**、`Has(code)` 对所有出现码、
`Is` 对全部可达节点、`UnwrapAll(0)` 的**地址唯一性**、JSON/Message 的大小上界。
校验逻辑本身也验证了：环不会死循环、共享子图只访问一次、深链 2 万层不爆栈。

## 3. 真实场景（6 个，覆盖全部待验证功能）

| # | 场景 | 关键证据 |
|---|---|---|
| 1 | 包裹链 + 映射：driver→repository→HTTP handler 三层 Wrap | `Message()=="GET /api/user/42 failed: query user:42: dial: connection refused"`；`MapHttpStatus(handler)=500`、叶子 `kUnavailable`→503、`kNotFound`→404；`MapLogLevel` 包装→ERROR、`kNotFound`→WARN；JSON 的 type/code_name/depth/wrapping/message 全匹配；`As<AppSqlError>` 取到 `42P01`；空错误→200/INFO/`null` |
| 2 | visitor 收集器：跳过、早停、深度分类、异常 | 跳过 WrappedError 子节点→6 节点且 2 个底层叶未出现；全量 8 节点（4 包装 + 4 叶）；命中 `kUnavailable` 早停于第 3 个节点；visitor 抛异常→`ok=false` 且保留已访问 1 个 |
| 3 | 注册表并发幂等 | 4 线程 × 250 个码全唯一、非 0；重复注册返回同码且 `Size` 不变；每个名字 `Lookup`/`Name` 往返；512 个批量注册往返；类型并发注册 + 重复注册 + `Create` 往返；同名不同元数据**保留首次**（父/深度/工厂）；depth=3 合法、depth=4 拒绝并写 `LastError` |
| 4 | 反射 caster 正反例 | 17 个 int 用例（含 ±边界、溢出、`+`/`-`、空白、`1e3`、`0x10`、前导零）、16 个 double 用例（`inf`/`nan`/`0x10`/`1,5`/`1e309`/`1e-400`/`1.2.3`）、bool 7 正 7 反；2 万随机 int 往返 0 失败；5,000 条随机垃圾不崩且成功项可复现；未特化 `long double`/`float` 退化为 `unknown` + `kString` |
| 5 | bind 配置绑定 | 全量/部分保留默认/失败强保证（对象不变、`kWrongType` 带字段名）、未知忽略 vs `reject_unknown`、`require_all`、两开关同时（未知优先）、`optional` 重载、`unordered_map`、大小写敏感、空 map；**重复 Bind 不再重建 `Fields()`**（`Fields` 累计调用仅 1 次 + bench 对照） |
| 6 | JSON 环/转义/截断 | 引号/反斜杠/换行/Tab/`\u0001`/中文透传；自环→`"causes":[null]`；200 层→`"truncated":true` 且 <100 KB；未注册类型工厂返回空→`"null"` |

## 4. 解耦检查

- 6 个头文件**逐个独立 TU 编译通过**（`-std=c++17 -Wall -Wextra -Wpedantic -Werror`）：
  error/{error,registry,mapping}.hpp、reflect/{reflect,caster,bind}.hpp。
- `grep` 全部 include：error/reflect 只包含彼此 + 标准库，**无 `bento/net`、
  `bento/app`、`bento/os` 等跨模块依赖**。
- `go2cpp_bento_error` 为静态库，仅链接 `Threads`；审计可执行文件 `ldd`
  无任何 `go2cpp` 动态库。
- 依赖清单：`error.hpp`→`<cstddef cstdint functional initializer_list memory
  string string_view type_traits unordered_set utility vector>`；
  `registry.hpp`→`error.hpp`+`<atomic cstddef cstdint deque functional map
  memory mutex string string_view type_traits unordered_map>`；
  `mapping.hpp`→`error.hpp`+`<cstdint functional string string_view>`；
  `reflect.hpp`→`caster.hpp`+`<cstddef cstdint string string_view type_traits>`；
  `caster.hpp`→`<charconv cmath cstddef limits string string_view system_error>`；
  `bind.hpp`→`reflect.hpp`+`<cstddef cstdint functional map optional string
  string_view type_traits utility vector>`。

## 5. 问题清单

### P0

无。依据：1200 棵随机图 + 200 个环 + 10 万层深链 + 自定义抛异常错误，
`Is/Has/As/WalkErrors/UnwrapAll(0)/Message/JSON` 全部不崩、不爆栈、不死循环；
ASan+UBSan 与 TSan 零报告；所有 `noexcept` 查询入口在自定义错误抛异常时均
转换为“未匹配/失败”。

### P1-1（已修复）`As/Has/Is/Walk/UnwrapAll(0)` 每查询构造 `unordered_set`

- 证据：8 节点图 `As` 9 alloc/op、`Has(code)` 13、`Walk` 17；其中约 2–9 次
  来自 `unordered_set` 的桶/节点分配，正常业务错误图只有个位数节点。
- 修复：`error.hpp:249` 新增 `detail::VisitSet`（前 8 个节点内联数组，溢出后
  才分配哈希表），`FindFirstError`、`ExpandDescendants`、`TraverseAny`、
  `WalkErrors` 全部改用（`error.cpp:124/157/387`）。分配数见 1.5，延迟
  −30%～−41%；深链 `As` −16.9%、`Walk` −23.6%。
- 注意：溢出分配失败时抛 `bad_alloc`，由 `Is/Has/As` 的 `noexcept` 包装
  统一兜底（不静默降级，避免环检测失效）。

### P1-2（已修复）`ErrorTypeRegistry` 每次查找构造临时 `std::string`

- 证据：`m_types` 原为 `unordered_map<string, TypeInfo>`，C++17 下
  `find(std::string(type_name))` 每次堆分配一次；`Create/Has/Depth/ParentName`
  全在 **持锁区内**分配，放大争用。
- 修复：`registry.hpp:215` 改为 `std::map<std::string, TypeInfo, std::less<>>`，
  5 处查找直接传 `std::string_view`（`registry.cpp:145/158/191/196/205`）。
  查找分配 1→0，`Has/ParentName` −41%，`Create` −22.8%；`Depth` 在噪声内
  （map O(log n) 与哈希抵消，但分配消失）。

### P1-3（已修复）`Bind` 每次重建字段表（每字段一个 `std::function`）

- 证据：`BindMap` 原为 `const std::vector<FieldBinder<T>> fields =
  FieldList<T>::Fields();`，每次 `Bind` 构造 vector + 5 个 `std::function`。
- 修复：`bind.hpp:115` 新增 `detail::FieldsOf<T>()`（按 T 的 magic static），
  `bind.hpp:150` 引用缓存。`Bind` −27.7%（145.7→105.4 ns），与
  `Fields()` 重建成本 34 ns/次吻合；字段表构造从每 op 摊销为每类型一次
  （bench 中 `Fields` 累计调用 1 次）。
- 语义约束已写入 `bind.hpp` 文档：`Fields()` 必须是纯函数、字段集合稳定。

### P1-4（已修复）`WrappedError::Message()` 每层复制 `shared_ptr`

- 证据：原实现每层 `ErrorPtr cause = current->m_cause; owner = std::move(cause);`，
  10 万层链产生约 20 万次引用计数原子操作。
- 修复：`error.cpp:245` 改为沿 `const ErrorPtr&` 读取；安全性来自不可变链
  （`this` 由调用方保活，每层 `m_cause` 保活下一层）。深链 `Message`
  9.4→8.8 ns/layer（−6.4%），10 万层约省 0.06 ms/次。

### P1-5（已修复）`Join` 对大 vector 二次分配/逐元素搬移

- 证据：原 `Join` 先建 `filtered`，`JoinError` 构造再 `reserve + push_back`
  复制一遍；10 万原因时多一次 ~800 KB 分配。
- 修复：`error.cpp:354` 用 erase-remove 就地过滤后 move；`JoinError` 构造
  （`error.cpp:278`）先 `any_of` 检查无空指针则直接接管 vector。`Join`
  −37.3%、alloc 4→2。

### P2-1（已缓解）`thread_local` 释放队列的析构顺序

- 证据：`g_release_state` 是 `thread_local ReleaseState`；若用户
  `thread_local` 对象持有错误链且在 `g_release_state` **之后**才析构
  （即先构造），链式析构会访问已销毁的 `pending`（UB 风险）。常规路径
  （线程内正常释放）不触发。
- 处理：`error.cpp:51` 给 `ReleaseState` 加析构函数做最终 drain：析构函数体
  先于成员析构执行，`pending` 仍可用，链式入队由本循环统一释放。
  `test` 中 5 万层 thread_local 链在线程退出时正常释放（不爆栈）。
- 残余风险：若用户在 `g_release_state` 已完成析构后才析构其 thread_local
  错误对象，仍会访问已销毁对象。彻底方案是“永不析构的 TLS 指针”（每线程
  泄漏一个 vector）或自定义 `shared_ptr` deleter，代价更大，记录不改。

### P2-2（记录）`As<T>` 每节点 `dynamic_pointer_cast`

- 证据：`FindFirstError` 对每个节点做 RTTI 转换；深链 10 万层 `As` 一次
  6 ms（58 ns/node）。用 `TypeName()==T::BentoErrorTypeName()` 做快路径
  **不安全**：`TypeName` 由用户宏/手写提供，可能与动态类型不一致，会改变
  `As` 语义。按“正确性优先”记录不改。

### P2-3（记录）`UnwrapAll()` 返回 `std::vector<ErrorPtr>` 的固有分配

- 每个 wrapping 节点一次 vector 分配（`WrappedError`/`JoinError` 各一次），
  `As` 优化后仍 5 alloc/op（8 节点图）。改为 `span`/回调式遍历会变更公共
  接口（`Error::UnwrapAll` 是虚函数），影响全部调用方；记录，建议在
  2.0 接口里重设计。

### P2-4（记录）`JoinError::Message()` 对嵌套 Join 无 `visited`

- 证据：`error.cpp` 的 `JoinError::Message()` 用栈展开嵌套 Join，无去重；
  环安全依赖“错误图不可变、构造期无法自引用”的不变量。自定义 `Error`
  子类可构造 Join 环使 `Message()` 死循环；`error.hpp` 文档已声明
  `Message()` 假定无环，环安全入口是 `Is/As/Has/WalkErrors`。记录，未改。

### P2-5（记录）同名类型注册静默保留首次注册

- 证据：`ErrorTypeRegistry::Register` 对已存在名字直接返回 true，不校验
  parent/depth/工厂是否一致。已文档化，并在 `test_error.cpp:632`
  （`TestTypeRegistryConflictKeepsFirst`）固定行为。若需严格，可改为元数据
  不一致时返回 false 并写 `LastError`；属 API 语义变更，记录。

### P2-6（记录）`Bind` 的 `Caster::Cast` 抛异常会传播

- `StringToString` 的 `assign` 可能抛 `bad_alloc`；`BindMap` 未捕获，
  异常会穿出 `Bind`（接口未声明 `noexcept`）。调用方需自行处理；
  若要求“失败一律返回 false”，需在 `BindMap` 内捕获并映射
  `kWrongType`（会吞掉 OOM 语义），记录不改。

### P2-7（记录）`EnqueueRelease` 分配失败时退回递归释放

- `WrappedError::~WrappedError` 是 `noexcept`；`pending.push_back` 抛
  `bad_alloc` 时只能 `cause.reset()` 直接释放，深链在极端 OOM 下会递归
  爆栈。发生概率极低（内存耗尽），记录。

### P2-8（记录）`DefaultJson` 对深链的复杂度

- `AppendErrorJson` 每层调用 `error->Message()`，而 `WrappedError::Message()`
  会展开剩余链，故深链 JSON 为 O(64 层 × 链长)（深度上限 64 把绝对量限住；
  200 层实测 <100 KB）。记录，未改。

### P3

1. `test_error.cpp`/`test_reflect.cpp` 存在 `if (found) { ... }` 条件断言，
   均由前置非空/数量断言兜底；建议后续统一改为“先记录 bool 再断言”风格。
2. `bento_netem` 在无 netem 权限的 WSL 环境 **Skipped**（非本模块）。
3. 工作树中 error/reflect 文件 git mode 由 100644 变为 100755（**非本次
   改动**，进入 diff 噪声）；建议后续统一为 644。
4. `audit_error_reflect.cpp` 未注册 CTest（与既有 `audit_*` 一致，避免把
   压测拖进单测）；建议后续统一以 `label=bench` 注册。
5. `ErrorCodeRegistry::Name(code)` 依赖“deque 下标 = code−1”不变量；当前
   无注销接口，若未来支持 erase 需同时改 Name/Code 映射。

## 6. 测试复查

| 项目 | 结果 |
|---|---|
| 原有单测 | `bento_test_error` 281 checks、`bento_test_reflect` 16,376 checks，全部通过 |
| 补测后 | **1,326 checks / 16,396 checks**，全部通过 |
| 新增用例 | 10 个（test_error：深链 Walk、环 `UnwrapAll(0)` 去重、环中 `As`、512 批量码往返、类型冲突、depth=3/4 边界；test_reflect：未特化 `TypeTraits`、超长数字/`-0`、空 `apply`、`reject_unknown`+`require_all`、字段表缓存） |
| 断言有效性 | 深链 `Message().size()==depth*3+4`、`Walk` 首尾节点、分配数、`Fields` 调用次数均为强断言；修正了 `TestWalkSkipChildren` 原有的弱断言（只比 `UnwrapAll(1)[1]`，现由数量 3 兜底） |
| SKIP 逃逸 | 无 SKIP 宏、无跳过；`test_error`/`test_reflect` 无条件断言逃逸路径 |
| 全量 | `ctest -R bento` **48/48 通过**（1 Skipped：bento_netem） |

## 7. 复现命令

```bash
cd /UserData/CodexWorkSpace/Go2cpp
# 单测
cmake --build .build-bento-wsl --target bento_test_error bento_test_reflect
cd .build-bento-wsl && ctest -R 'bento_(error|reflect)$' --output-on-failure
# 审计（压测 + 随机 + 场景）
cd /UserData/CodexWorkSpace/Go2cpp
g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/bento \
  tests/bento/audit_error_reflect.cpp src/bento/error/error.cpp \
  src/bento/error/registry.cpp src/bento/error/mapping.cpp -pthread \
  -o /tmp/audit_error_reflect && /tmp/audit_error_reflect
# ASan+UBSan / TSan：追加 -O1 -g -fsanitize=address,undefined（或 thread） \
#   -DAUDIT_NO_NEW_OVERRIDE
```
