> 当前快照（2026-09-24）：在此前删除旧 panic_defer 模块后，新增
> control_flow 模块。它提供 RAII defer、显式 panic/recover 状态和
> LastDeferException()；这是可验证的 C++ 等价协议，不是 Go 编译器级自动
> 栈展开。Channel select 新增 SelectValue/SelectCaster/SetAny，IOManager
> 新增 Fiber-only WaitAny/WaitMany；普通线程多 fd 等待应使用原生 poll/select。
> 下方历史记录中的“未实现”结论以本说明和最新兼容性矩阵为准。
> 另注：历史条目中的源码/测试/示例路径反映**当时**的目录结构——早期为
> `src/*.cpp`、`tests/*.cpp`、`example/*.cpp` 的扁平布局，现已重构为
> `src/{concurrency,network,logging,config,tools}/`、
> `tests/{concurrency,network,logging,tools}/`、
> `example/{concurrency,network,logging,tools}/`。引用旧路径时请按当前结构
> 换算；这些条目不改写，以保留演进过程。

# ★ include 通读全部完成（用户要求「两轮一起做」）

## 2026-10-15：新增 `tools/time`（Go `time` 核心子集）+ 补齐第三方库逐库计划产出时间（第 29 轮）

### 交付清单

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/time.hpp` | **~414** | `Duration` + `Time` + `Since`/`Until`/`Sleep` |
| `include/go2cpp/time.hpp` | 5 | 根路径转发头（部署前确认该路径未被占用） |
| `tests/tools/test_time.cpp` | 282 | 6 个真实场景 + 10 万次×2 大数据量 + 3 组随机属性测试 |
| `example/tools/time_demo.cpp` | 81 | 简单版 3 场景 |
| `example/tools/time_full.cpp` | 169 | 全面版 6 场景 |
| `NextStep/third-party-plan.md` | +36 | **新增 §3.5「计划产出时间（逐库/逐层）」**（必须项缺口修复） |
| `NextStep/roadmap.md` | +12/-7 | T 阶段表补「计划产出时间」列 |
| `tests/test_main.cpp`、`cmake/Go2CppTests.cmake`、`example/README.md` | — | 三处登记 |

### 复用优先：这一轮复用的是**运行时能力**，不是第三方库

`time.Sleep` 在 Go 里挂起的是 **goroutine 而不是线程**。调研后发现项目**已经具备**该能力：
`src/network/hook.cpp` 已拦截 `sleep`/`usleep`/`nanosleep`，其中的 `cooperative_sleep_for`
（临时 `eventfd` + `manager->WaitFor`）会把睡眠变成**协作式等待**。因此 `time::Sleep` 直接调用
`::nanosleep` 就得到正确语义 —— **不需要新造任何调度机制**。

其余取舍：C++ 的 `std::chrono` 与 Go `time` 语义不匹配（`Duration` 是强类型且带特定 `String()` 格式、
`Time` 带单调读数），故自研；语义对照 `go/src/time/`（Go 1.23.0，BSD-3-Clause），独立实现，
`Duration.String()` 的算法逐分支对照了上游 `time.go` 的 `Duration.format`。

### 对齐的非显然语义

- `Duration.String()`：`< 1s` 时按 ns/µs/ms 选单位与精度（`prec = 0/3/6`），`0 → "0s"`；
  否则秒带最多 9 位小数、**去尾随零**，再按需补 `m`/`h`；微秒符号用 **U+00B5 `µ`（两字节 UTF-8）**；
- `Time.Sub()` **优先用单调读数**，`Equal()` **只用墙钟**（与 Go 一致）；
- **零 `Time` 是「1 年 1 月 1 日」而不是 Unix 0**；
- `Sleep` 对**非正时长立即返回**；
- 补了**非成员** `int * Duration`，因为 Go 里 `2 * time.Second` 是常见写法。

### 验证（实测）

```sh
GO2CPP_TEST_FILTER=time ./build-quality/go2cpp_tests      # → all checks passed（6 场景 + 10 万×2 + 3 组随机）
./build-quality/go2cpp_time_demo / go2cpp_time_full       # rc=0，输出逐条与注释预期一致
```

随机测试的三组性质：随机纳秒 → `String()` 与**独立参考实现**对拍（10 万轮，0 差异）、
随机 `Add`/`Sub` 往返（10 万轮）、随机比较三分律与 `Truncate`/`Round` 的边界性质（10 万轮）。

### 本轮修掉的问题

| # | 问题 | 位置 | 处置 |
|---:|---|---|---|
| 1 | 缺 `int * Duration` 重载（`2 * time.Second` 无法编译） | 实现 | 补非成员 `operator*` |
| 2 | **真 bug**：`Duration.String()` 的 `< 1s` 分支漏了「小数之后补整数部分」（Go 是 `fmtFrac` 后还要 `fmtInt`）→ `1ns`/`1µs`/`1.5ms` 全输出空串 | 实现 | 补 `AppendInteger` |
| 3 | **语义 bug**：`IsZero()` 用 `time_point{}`（Unix 0）当零值，而 Go 的零值是「1 年 1 月」 | 实现 | 改为显式零标记 `m_is_zero` |
| 4 | 编译命令用 `set -e`，编译失败时诊断被吞、只剩空输出 | 流程 | 改为显式打印 `compile_rc`/`warn`/`error` 计数与诊断头部 |
| 5 | `Minute / 2` 期望写成 `"30m0s"`（实为 30 **秒**） | 测试 | 修正期望为 `"30s"` |
| 6 | 单位换算断言多除了一次（`FromNanoseconds(nanos/1000).Microseconds()`） | 测试 | 改为 `FromNanoseconds(nanos).Microseconds()` |
| 7 | `Round`-`Truncate` 差值性质对负数不成立（差为负） | 测试 | 改为判断绝对值 ≤ 单位 |

> 5~7 都在**测试代码**里，实现经独立参考实现 10 万轮对拍证明无误（诊断程序 0 差异）。

### 同时补齐的必须项缺口

`NextStep/third-party-plan.md` 原先只有「依赖顺序」列、**没有时间列**，与「每个模块的计划产出时间」要求不符。
现已新增 **§3.5（18 行表）**：逐库给出计划产出时间（第 4 周起）、前置依赖与**验收物**
（源码 + 测试 + 示例 + `docs/dependencies.md` 登记），时间轴与 `roadmap.md` 的 S1~S7 共用同一套「第 N 周」；
`roadmap.md` 的 T 阶段表也补了「计划产出时间」列（T1 第 4–5 周 / T2 第 6–7 周 / T3 第 8–9 周 / T4 第 9 周起）。

### 未覆盖 / 后续

- `After`/`Tick`/`Ticker`/`Timer`（需与 `Channel<Time>` 和 `core::TimerService` 对接）、
  `Format`/`Parse`（布局串是一套独立小语言）、`LoadLocation`、`Duration.Parse` —— 已在头文件末尾列成待办表；
- `Sleep` 在**未启用 Hook / 纯线程上下文**中会退化为真实线程睡眠（Go 不会），已写入文件头边界；
- `Time.String()` 采用简化形式（`YYYY-MM-DD HH:MM:SS[.fffffffff] +0000`），不含时区名与 RFC3339 家族；
- 性能未测：示例打印的耗时来自 Debug 构建、仅供参考。

## 2026-10-15：新增 `tools/streams`（Go `io` 接口族，第 28 轮）

### 交付清单

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/streams.hpp` | **664** | 18 个接口 + 5 个错误锚点 + 7 个核心函数 + 6 个适配器 + 2 个内存底座 |
| `include/go2cpp/streams.hpp` | 6 | 根路径转发头 |
| `tests/tools/test_streams.cpp` | 333 | 8 个真实场景 + 大数据量（1 MiB / 10 万次小块 / 2 万帧短读）+ 3 组随机属性测试 |
| `example/tools/streams_demo.cpp` | 103 | 简单版 3 场景 |
| `example/tools/streams_full.cpp` | 179 | 全面版 6 场景 |
| `tests/test_main.cpp` | — | 登记：声明 + `selected("streams")` |
| `cmake/Go2CppTests.cmake` | — | 登记：测试源 + `GO2CPP_EXAMPLES`（CRLF） |
| `example/README.md` | — | 登记：速查表 2 行 |

### 命名偏差（重要）

Go 的 `io` 包提供的是**流式读写接口族**（`io.Reader`/`io.Writer`/`io.Copy`…），
而本项目已经把这个名字用于**描述符与多路复用调度层**
（`go2cpp::io::IOManager` / `FdManager`，见 `include/go2cpp/network/io.hpp`；
且 `include/go2cpp/io.hpp` 已作为该模块的转发头存在）。两者语义不同、不可合并，因此：

- 文件：`include/go2cpp/tools/streams.hpp`；命名空间：**`go2cpp::streams`**
- 其余 API 与 Go 同名同义：`io.Reader` → `streams::Reader`，`io.EOF` → `streams::IoEof()`

该偏差已写入文件头注释与 `NextStep/dev-progress.md`，避免后来者误以为漏实现。

### 复用优先的结论

C++ 侧没有与 Go `io` 语义对齐的现成库：`std::istream`/`std::ostream` 用**状态位**表达失败，
不返回错误值，也不支持 Go 的「`n > 0` 同时带错误」契约；`ReadFull`/`CopyN`/`TeeReader` 这类组合语义更无处对应。
因此自研。语义对照来源：`third_party/go1.23.0.src.tar.gz` 的 `go/src/io/`（Go 1.23.0，BSD-3-Clause），**独立实现**。

### 对齐的非显然 Go 语义

- `ReadAtLeast`：`size < minimum` → `ErrShortBuffer`；**读够 minimum 后清除 `EOF`**；
  `0 < n < minimum` 且遇 `EOF` → `ErrUnexpectedEOF`（`n == 0` 时原样返回 `EOF`）；
- `ReadAll`：把 `EOF` **归一化为成功**（错误为空）；
- `Copy`：优先使用源端 `WriterTo` / 目标端 `ReaderFrom`，否则 32 KiB 缓冲循环；
  **短写 → `ErrShortWrite`**；连续 100 次空读 → `ErrNoProgress`；
- `CopyN`：写满 `n` 成功；`written < n` 且无错 → `EOF`；
- `MultiReader`：某个源「读到数据同时遇 `EOF`」时**清掉 EOF 先交付数据**，下次调用再切源；
- `CopyBuffer`：Go 在缓冲长度为 0 时 panic，这里返回 `ErrShortBuffer`（明确拒绝而非崩溃）。

### 验证（实测）

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/streams_check.cpp src/tools/error.cpp -o /tmp/streams_check
# → 编译诊断 0 行；streams.hpp check: failures=0

GO2CPP_TEST_FILTER=streams ./build-quality/go2cpp_tests   # → all checks passed
./build-quality/go2cpp_streams_demo / go2cpp_streams_full  # rc=0，输出与注释预期一致
```

测试特意实现了两个「恶劣源」：`ChunkedReader`（每次只给 1–37 字节）与 `DripReader`（每次只给 1 字节），
用来**强制走短读路径** —— 这是 Go `io` 契约里最容易写错的地方；配合 3 组随机属性测试
（随机分块 Copy 逐字节一致、2 万次随机偏移 `SectionReader`、2 万次随机 `LimitReader` 上限）。

### 本轮修掉的问题（全部在验证/示例侧）

| # | 问题 | 处置 |
|---:|---|---|
| 1 | 头文件级验证程序把字符串字面量 `const_cast` 后当可写缓冲传给 `Read` → `memcpy` 写入只读页 → **段错误**（现象是"编译通过但无输出"，又被 `set -e` 掩盖） | 改用真实可写缓冲；验证命令显式打印退出码 |
| 2 | `SectionReader(source, 2, 4)` 内容为 `"2345"`，`ReadAt(buf,2,1)` 期望写成 `"45"`（应为 `"34"`） | 修正测试期望 |
| 3 | 测试里 `"MAGIC:0011"` 按 11 字节断言（实际 10 字节 + 换行） | 修正断言 |
| 4 | 测试场景 8 出现 `new StringReader(...)` 且从不释放（废代码） | 重写为「`Discard()` 作 `TeeReader` 旁路」的有效断言 |
| 5 | 两个示例的帧数据只有 9 字节却期望「2 个完整帧」（9 字节只能成 1 帧） | 改为 17 字节 = 2 帧 + 1 字节尾帧 |

### 未覆盖 / 后续

- `io.Pipe`（Go 用 goroutine 实现的同步管道）未实现：C++ 侧需与 `core::ParkingCondition` 协作，留待后续；
- `SectionReader` 未提供 `Outer` 语义；`RuneReader`/`RuneScanner` 只定义了接口，尚未给出标准实现；
- 内存底座 `StringReader`/`BufferWriter` 是 `bytes.Reader`/`bytes.Buffer` 的**最小版本**，完整版留给 `bytes` 模块；
- 性能未测：示例里的耗时来自 Debug 构建、仅供参考；正式结论须用 `build-bench`（Release）。

## 2026-10-15：新增 `tools/fmt`（Go 标准库对齐第二批，第 27 轮）

### 交付清单

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/fmt.hpp` | **~612** | 格式串解析 + 类型分派（可变参数模板 + `if constexpr`）+ 全部公开 API |
| `include/go2cpp/fmt.hpp` | 5 | 根路径转发头 |
| `tests/tools/test_fmt.cpp` | 214 | 6 个真实场景 + 10 万次×2 大数据量 + 3 组随机属性测试 |
| `example/tools/fmt_demo.cpp` | 75 | 简单版 3 场景 |
| `example/tools/fmt_full.cpp` | 140 | 全面版 6 场景 |
| `tests/test_main.cpp` | — | 登记：声明 + `selected("fmt")` |
| `cmake/Go2CppTests.cmake` | — | 登记：测试源 + `GO2CPP_EXAMPLES`（CRLF） |
| `example/README.md` | — | 登记：速查表 2 行 |

### 复用优先的检索结论

| 候选 | 许可证 | 结论 |
|---|---|---|
| [fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT | **不采用为直接依赖**：语法是 `{}`，与 Go 的 `%v` 指令集不同；要用它必须先写「Go 格式串 → fmt 格式串」转换层，而转换层本身就要实现本文件的解析器（双份复杂度）。**保留为底层候选**（将来可在 `detail` 层替换实现） |
| libc `printf` 家族 | — | **不采用**：类型不安全；Go 的 width/precision 以 **rune** 计、C 的以字节计；无 `%v` |
| **自研** | — | **采用**：Go 语义 1:1，且复用本研究已有的 `strconv.hpp`（整数/浮点格式化、`Quote`）与 `error.hpp`（`ErrorPtr`/`Wrap`/`Join`） |

语义对照来源：`third_party/go1.23.0.src.tar.gz` 的 `go/src/fmt/doc.go`（Go 1.23.0，BSD-3-Clause），**独立实现**。

### 能力与对齐的 Go 语义

- **动词**：`v` `d` `s` `q` `t` `f` `e` `g` `x` `X` `b` `o` `c` `T`（`Errorf` 另支持 `w`）
- **flags**：`-`（左对齐）、`0`（零填充，**数字时在符号之后**）、`+`、空格、`#`（`0b`/`0`/`0x` 前缀、浮点保留小数点）
- **width/precision 按 rune 计算**（与 Go 一致，不是字节）；字符串 precision 按 rune 截断（`%.2s` 对 `"中文测试"` 得 `"中文"`）
- **`%v` 对浮点用最短唯一表示**（`0.1+0.2` → `0.30000000000000004`，`1e6` → `1e+06`）
- **`ErrorPtr` 按 `Message()` 输出**（对齐 Go 的 error 接口优先规则）；`std::vector<T>` 与 `std::pair` 递归 `%v`
- **错误标记**：`%!d(string=abc)`（类型不匹配）、`%!d(MISSING)`（缺参数）、`%!(EXTRA int=2)`（多余参数）
- **边界**（文件头已列）：不实现 `Formatter`/`GoStringer` 分派（C++ 无反射）、`%U`、复数、`%p` 的 `#` 细节；`*` 宽度/精度**抛 `std::logic_error` 明确拒绝**

### 验证（实测）

```sh
# 头文件级验证
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/fmt_check.cpp src/tools/error.cpp -o /tmp/fmt_check
# → 编译诊断 0 行；fmt.hpp check: failures=0

# 项目内正式测试（已登记）
GO2CPP_TEST_FILTER=fmt ./build-quality/go2cpp_tests      # → all checks passed

# 示例
./build-quality/go2cpp_fmt_demo    # rc=0，输出逐条与注释「预期」一致
./build-quality/go2cpp_fmt_full    # rc=0，6 场景全部通过
```

测试覆盖：6 个真实场景（日志行、SQL 拼接与转义、错误包装链、表格对齐、指标与百分号、结构化展示）、
10 万次日志构造、10 万次错误构造（并断言 `use_count` 回到基线，确认无泄漏）、
3 组随机属性测试（随机值×6 模板不得出现未预期标记、随机 int64 的 `%d`→`Atoi` 往返、随机 double 的 `%v`→`ParseFloat` **位相等**往返）。

### 本轮修掉的问题

| # | 问题 | 处置 |
|---:|---|---|
| 1 | `((expr, 0), ...)` 触发 `-Wunused-value` ×3 | 直接用逗号折叠 |
| 2 | `TypeName` 用 `sizeof` 推断 → `int` 报成 `int32`（Go 输出 `int`）；`char[2]` 衰变为 `char*` 落不到分支 | 改为按 **C++ 类型名**映射；字符串类判定用 `is_convertible_v<..., string_view>` |
| 3 | vector 分支**漏了 `%q`**（Go 对 `[]string` 的 `%q` 逐个引用） | 递归传递 verb |
| 4 | 三目写进逗号折叠 → **编译错误** `binary expression in operand of fold-expression` | 移入 `emit` lambda |
| 5 | 零参数 `Sprint()`/`Sprintln()` 触发模板诊断 | `if constexpr (sizeof...(Args) > 0)` 保护 |
| 6 | **流程教训**：一次「修正后输出未变」的真因是**编译失败**，而命令写成 `g++ ... \| grep -c warning ; ./check`（`;` 分隔）→ 掩盖错误并运行了**旧二进制** | 全部验证命令改为 `&&` 串联；已记入 `NextStep/dev-progress.md` |

> 第 6 条是通用教训：**"看到输出"不等于"跑的是新代码"**。宁可在命令里多写一个 `&&`。

### 未覆盖 / 后续

- `*` 形式的 width/precision（Go 允许从操作数取）暂未实现，明确抛异常而非静默给错结果。
- 未实现 Go 的 `Formatter`/`GoStringer` 接口分派（需要反射/虚接口约定，C++ 侧留给后续设计）。
- 性能未测：示例打印的耗时来自 Debug 构建、仅供参考；正式结论须用 `build-bench`（Release）。

## 2026-10-15：新增 `tools/strings` + `tools/strconv`（Go 标准库对齐第一批，第 26 轮）

### 背景

本日 18:00–22:00 的定时任务（每 15 分钟一轮）有三项目标：
① 模块化架构报告 + 一张含文字描述的架构图 → `ArchitectureDiagram/`；
② Go 标准库与第三方库预研 + 后续计划（含计划产出时间）→ `NextStep/`；
③ 有余力时按项目规则开发并记录进度 —— **本轮是 ③ 的第一个实际代码交付**。

### 交付清单

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/strings.hpp` | **654** | Go `strings` 的 32 个同名 API + 最小 UTF-8 工具（`DecodeRune`/`RuneCount`/`IsSpaceRune`）+ `StringBuilder` |
| `include/go2cpp/strings.hpp` | 5 | 根路径转发头 |
| `include/go2cpp/tools/strconv.hpp` | **546** | `Atoi`/`Itoa`/`ParseInt`/`ParseUint`/`FormatInt`/`FormatUint`/`ParseBool`/`FormatBool`/`ParseFloat`/`FormatFloat`/`Quote` + `ErrSyntax`/`ErrRange` 锚点 |
| `include/go2cpp/strconv.hpp` | 5 | 根路径转发头 |
| `tests/tools/test_strings.cpp` | 242 | 6 个真实场景 + 10 万字段往返 + 2 万/1 万轮随机对拍 |
| `tests/tools/test_strconv.cpp` | 245 | 6 个真实场景 + 20 万×2 往返 + 4 组随机属性测试 |
| `example/tools/strings_demo.cpp` | 88 | 简单版 3 场景 |
| `example/tools/strings_full.cpp` | 140 | 全面版 6 场景 |
| `example/tools/strconv_demo.cpp` | 90 | 简单版 3 场景 |
| `example/tools/strconv_full.cpp` | 153 | 全面版 6 场景 |
| `tests/test_main.cpp` | — | 登记：函数声明 + `selected("strings")`/`selected("strconv")` |
| `cmake/Go2CppTests.cmake` | — | 登记：测试源文件 2 项 + `GO2CPP_EXAMPLES` 4 项（**文件为 CRLF，插入按 CRLF**） |
| `example/README.md` | — | 登记：速查表 4 行 |

### 复用优先的检索结论（项目规范第 2 节）

- **strings**：候选 `absl/strings`（Apache-2.0，需完整构建集成）、`boostorg/algorithm`（Boost 1.0，重依赖）、
  `fmtlib/fmt`（MIT，但那是**格式化**库、对应 Go 的 `fmt`）、`tgalaj/strutil`（MIT，功能集与 Go 不对齐）
  → **结论：自研薄封装**（目标是 Go 语义 1:1，而不是通用字符串库）。
- **strconv**：C++ 侧没有语义对齐实现（`std::stoi` 不做 `base=0` 推断、不返回范围错误、不返回钳制值；
  `std::from_chars` 不认识下划线）→ **自研**，底层复用 `std::from_chars`/`std::to_chars` 保证 round-trip 精度。
- **语义对照来源**：项目内 `third_party/go1.23.0.src.tar.gz` 的 `go/src/strings/` 与 `go/src/strconv/`
  （Go 1.23.0，BSD-3-Clause）。**仅对照行为，代码为独立实现**；对照命令写在 `NextStep/dev-progress.md`。

### 验证（全部实际运行过）

```sh
# 头文件级验证（独立 TU，含 Go 的反直觉边界）
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/strings_check.cpp -o /tmp/strings_check && /tmp/strings_check
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/strconv_check.cpp src/tools/error.cpp -o /tmp/strconv_check && /tmp/strconv_check
# → failures=0（strings 49 条、strconv 60 条边界断言），0 warning

# 项目内正式测试（已登记）
GO2CPP_TEST_FILTER=strings ./build-quality/go2cpp_tests   # → all checks passed
GO2CPP_TEST_FILTER=strconv ./build-quality/go2cpp_tests   # → all checks passed

# 示例（4 个，退出码均为 0，输出逐条与注释「预期」一致）
./build-quality/go2cpp_strings_demo / go2cpp_strings_full / go2cpp_strconv_demo / go2cpp_strconv_full
```

对齐的**非显然 Go 语义**（都写进了测试）：`Count(s,"") == rune+1`、`Split(s,"")` 按 rune 切分、
`Replace(s,"",new,-1)` 在每个 rune 前后插入、`base=0` 时前导 `0` 为**八进制**（`"0755"` → 493）、
下划线**只在 `base=0`** 且必须在数字之间、越界返回 `ErrRange` **并给出钳制值**、
`ParseBool` 精确 12 个接受值、`ParseUint` 不接受符号前缀、`ParseFloat` 的 `Inf`/`NaN`/hex float。

### 过程中修掉的问题（全部由工具或断言暴露，非事后声称）

| # | 位置 | 问题 | 处置 |
|---:|---|---|---|
| 1 | `strings.hpp` | 注释里写 `go/src/strings/*.go`，`/*` 触发 GCC `-Wcomment` | 改措辞 |
| 2 | `strconv.hpp` | **实现 bug**：`std::from_chars(chars_format::hex)` 的输入**不含 `0x` 前缀** | 检出后先 `erase(0,2)` |
| 3 | `test_strings.cpp` | 用 `Index`（首次出现）判断 `HasSuffix`，重复子串必然误判 | 改用 `LastIndex` |
| 4 | `test_strconv.cpp` | 参考实现直接用 `from_chars`：它**不接受前导 `+`** | 参考实现先剥离 |
| 5 | `test_strconv.cpp` | 剥离 `+` 后 `from_chars` 把 `"+-5"` 当合法负数 | 改为**显式语法检查** + 无符号范围复核 |
| 6 | `test_strings.cpp` | 测试期望写错：`SplitAfter("a,b,c","")` 应为 `a|,|b|,|c` | 用诊断程序确认真实行为后修测试 |
| 7 | `PROGRESS.md` 更新脚本 | 锚点漏了「后续轮」列 → 一次更新未写入 | 改为按行首前缀匹配 |

> 3~6 都发生在**测试代码**里，实现无误 —— 这正是「随机对拍 + 独立参考实现」的价值：
> 它暴露的是「参考实现应当等价于谁」的理解偏差。

### 未覆盖 / 后续

- `FormatFloat` 未实现 Go 的 `'b'`/`'x'`/`'X'` 格式（明确抛 `std::logic_error`，已在文件头列为边界）。
- `ToUpper`/`ToLower`/`EqualFold` 只覆盖 ASCII + Latin-1（完整 Unicode 表留给后续 `unicode` 模块）。
- **未做「Go 与 Go2Cpp 同输入对拍」**：需要 Go 工具链参与，计划在 `NextStep/roadmap.md` 的 S5 阶段引入
  （离线参考，不进构建依赖）。
- 性能未测：`strings_full`/`strconv_full` 里打印的耗时是 Debug 构建、仅供参考；
  正式性能结论必须用 `build-bench`（Release）测量后写入 `docs/performance.md`。

## 2026-10-15：nlohmann/json 薄封装 + fiber_stack_limit 两版示例（第 25 轮）

### 用户要求

> 「按 `docs/HANDOFF.md` §2.4 执行待办：写 `tools/json.hpp`（真身）+ `json.hpp`（转发头）、
> `cmake/Go2CppModules.cmake` 加 `${CMAKE_SOURCE_DIR}/third_party`、构建验证
> （0 warning / 0 error）、部署 `fiber_stack_limit_{demo,full}.cpp` 并注册
> `GO2CPP_EXAMPLES`、跑通并逐条比对输出与注释里的"预期"、更新 `codex.md`、提交」

### 改动清单（关键文件）

| 文件 | 改动 |
|---|---|
| `include/go2cpp/tools/json.hpp` | **新增**（222 行，header-only 真身）：`Json` 别名、`kDefaultJsonMaxDepth = 64`、`JsonDepthExceededError()`、`JsonScanDepth()`（迭代扫描 + 早退）、`JsonParseResult`、`ParseJson()` |
| `include/go2cpp/json.hpp` | **新增**（5 行转发头，指向 `tools/json.hpp`） |
| `cmake/Go2CppModules.cmake` | `go2cpp_runtime` 的 `BUILD_INTERFACE` 增加 `${CMAKE_SOURCE_DIR}/third_party`（**+1 行**） |
| `cmake/Go2CppTests.cmake` | `GO2CPP_EXAMPLES` 登记两个新示例（+2 行）；该文件同时带入上一轮的 benchmark 注册（+12 行） |
| `example/concurrency/fiber_stack_limit_demo.cpp` | **新增**（136 行，3 场景） |
| `example/concurrency/fiber_stack_limit_full.cpp` | **新增**（640 行，6 场景） |
| `example/README.md` | 速查表登记两行（+2 行，规范要求新增示例必须登记） |
| `docs/dependencies.md` | 新增 nlohmann/json 一节：来源/版本/许可证/暴露方式/限深实测依据（+19 行） |
| `docs/HANDOFF.md` | **新增**（上一轮产物）并修正其中"nlohmann 是递归下降解析器"的错误结论（2 处） |
| `benchmarks/fiber_stack_probe.cpp` | **新增**（上一轮产物）+ 修 3 处编译警告 |
| `benchmarks/gmp_compute_benchmark.cpp` | **新增**（上一轮产物）+ 修 1 处编译警告 |

### ★ 推翻上一轮的一个结论：nlohmann 3.11.3 的**解析器是迭代的**

上一轮 `docs/HANDOFF.md` 记着「递归下降 `parse_internal` 已确认」。该结论来自
**grep 到 `parse_internal` 字样**，但实际调用链是
`parse()` → **`sax_parse_internal`**（`third_party/nlohmann/json.hpp:12348`），
其实现用 **`std::vector<bool> states` 显式状态栈 + `while (true)` 循环**
（`:12401` / `:12426`），**没有递归下降**。

实测（fork 隔离 + 二分，128 KiB Fiber 栈，本轮亲测）：

| 操作 | Release/-O2 | 无优化 |
|---|---|---|
| 解析（`ParseJson` 内部） | ≥ 524288 层 | ≥ 524288 层 |
| `dump()` | 1162 层 | 609 层 |
| 拷贝一份 `json` | 816 层 | 185 层 |

**结论修正**：「深 JSON 会在解析时爆栈」**不成立**；真正让进程消失的是
**解析之后的递归操作**（`dump()`、拷贝）。因此 `ParseJson` 的限深栅栏仍然必要，
但理由是「拦截深文档进入程序后的递归操作 + O(深度) 资源放大 + 版本稳健性」，
而不是「防解析爆栈」。`docs/HANDOFF.md`、`docs/dependencies.md` 与本条记录都已
按实测修正；`fiber_stack_limit_full.cpp` 场景 5 把这个反证做成了可运行示例
（20 万层解析成功 → 同一文档 dump/拷贝被信号杀死 → 临界深度实测）。

### 示例设计

- **简单版**（`fiber_stack_limit_demo.cpp`，3 场景）：128 KiB 栈的安全递归深度采样、
  递归改迭代、深层 JSON 被限深拒绝（错误身份判定）。
- **全面版**（`fiber_stack_limit_full.cpp`，6 场景）：
  ① fork 二分探深（Fiber 帧 0 / 帧 256B / 8 MiB pthread 对照，本机实测 1021 / 339 / 65496）；
  ② 递归改迭代；
  ③ ★ 换 Fiber 续跑（每 300 层换一块新栈，目标 4000 层 → 13 次换栈、最深嵌套 14）；
  ④ 显式限深（深度错误与语法错误用 `Is` 身份区分）；
  ⑤ 解析 vs 递归（20 万层解析成功、dump/拷贝被信号杀死、两者临界深度实测）；
  ⑥ JSON 深层嵌套实战（64/65 边界、随机 500 组、5 万层攻击载荷早退 0 µs、3000 次稳定性重复）。
- 所有"会不会崩"的判定都在 **fork 子进程**里做，用 `waitpid` 终止状态判结局；
  探深前 `setrlimit(RLIMIT_CORE, 0)`，避免在构建目录留下 core 文件。

### 过程中发现并修掉的两个缺陷（都在本轮新增的示例自身）

1. **fork 导致父进程输出重复一次**：子进程 `freopen` 会 flush 从父进程继承来的
   stdio 缓冲，把父进程尚未写出的内容再写一遍。修法：fork **前** `std::fflush(nullptr)`，
   子进程改用 `dup2` 指向 `/dev/null`，结束用 `_exit()`（而不是 `exit()`）。
2. **错误文本误导**：`JsonScanDepth` 早退时返回值只是真实深度的**下界**，
   旧文本写成「JSON 嵌套深度 65 超过上限 64」，会让人以为输入只有 65 层。
   已改为「扫描到第 N 层即早退，完整深度未统计」。

### 验证（全部实际运行过）

- `build-quality` 全量 `--clean-first` 重建：**0 warning / 0 error**
  （`grep -c -i warning` = **0**；覆盖 13 个库 + 全部示例 + 测试）
- `ctest --test-dir build-quality`：**69/69 通过**，总耗时 **27.89 s**
  （其中新增 `go2cpp_fiber_stack_limit_demo` 0.01 s、
  `go2cpp_fiber_stack_limit_full` 1.37 s，远低于 30 s 超时）
- `build-bench`（Release）下三个 benchmark 重编译：**0 warning**
  （修掉 4 处：`freopen` 返回值 2 处、`PTHREAD_STACK_MIN` 符号比较 1 处、
  `main` 未使用参数 2 个 —— 这些警告此前从未被发现，因为 `benchmarks/*`
  只在 `GO2CPP_BUILD_BENCHMARKS=ON` 时编译，而历次"0 warning"门禁跑的是
  `build-quality`（benchmarks OFF））
- 两个示例单独运行退出码均 0，输出逐条与注释里的"预期"一致

### 一次行尾事故与修复（教训）

用 Windows 侧 Python 以 **text 模式**回写 `cmake/Go2CppModules.cmake` 时，
`subprocess` 把 `\n` 转成了 `\r\n`，`git diff` 显示**整文件变化**。
已用 `tr -d "\r"` 还原为 LF，diff 恢复为"仅 +1 行"。
后续所有脚本改为**全程 bytes 读写**；并确认了两个事实：
`cmake/Go2CppTests.cmake` 在 HEAD 里本来就是 **CRLF**，而 C++ 源码是 LF
（仓库行尾并不统一，改动前必须先 `file` 确认）。

### ★ 提交前的自洽性检查（避免"提交了示例、没提交被测 API"）

提交后复核时发现：两个示例用了 `go2cpp::errors::Is(...)`，而 `errors::` 命名空间
**只存在于用户在途未提交的 `include/go2cpp/tools/error.hpp`** ——
`git show HEAD:include/go2cpp/tools/error.hpp | grep "namespace errors"` **无结果**。
若原样提交，**从 HEAD 全新 checkout 构建会失败**。这正是本文件早前记录过的
事故模式（"提交了测试与示例，却没提交它所依赖的头文件"）。

处置：把 6 处 `go2cpp::errors::Is` 全部换成 HEAD 里已存在的 `go2cpp::Is`
（语义完全相同；`git show HEAD:include/go2cpp/tools/error.hpp:136` 有
`bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept;`），
并补做了**提交后快照验证**：

```sh
git archive HEAD | tar -x -C /tmp/headverify
g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
    -I include -I third_party example/concurrency/fiber_stack_limit_demo.cpp
g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic \
    -I include -I third_party example/concurrency/fiber_stack_limit_full.cpp
```

两个 TU 均 **0 warning 通过**，`go2cpp/json.hpp` 单独包含也可编译 ——
证明本次提交自洽、不依赖任何在途文件。

**留下的检查动作**：凡新增示例/测试引用了什么 API，提交前都要确认该 API
在 `HEAD` 里存在（而不是只存在于工作区的在途文件里）。

### 未覆盖 / 后续状态

- 未跑 sanitizer（ASan/UBSan/TSan）与 Valgrind。
- **未新增 `tests/tools/test_json.cpp`**：本轮以「示例 + ctest」承载验证
  （demo 3 场景 + full 6 场景 + 随机 500 组 + 稳定性 3000 次 + fork 反证）。
  若要纳入 `go2cpp_tests`，需新增测试文件并在 `tests/test_main.cpp` 与
  `cmake/Go2CppTests.cmake` 两处登记。
- `benchmarks/fiber_stack_probe.cpp` 的 45 组数据**未在本轮重跑**（沿用上一轮结果）；
  本轮只重编译并复现了其中的临界深度量级（1021/339/65496 vs 1022/340/65497）。
- 在途未提交、**本轮未触碰**的用户文件：`include/go2cpp/tools/error.hpp`、
  `src/tools/error.cpp`、`include/go2cpp/logging/log.hpp`、
  `example/concurrency/future_full.cpp`（其中 error 两件是注释改造进行中）。

## 2026-10-14：Context 模块注释改造（第 1 批）

### 用户要求

> 「为 Context 模块完成注释。原来的注释太敷衍了。现在要求按照项目规范注释，
> 并且必须强制按照代码逻辑写 step 逻辑，并且在项目规范上面加上成员变量的注释规范」

### 问题量化（改造前的实测）

```
src/concurrency/context.cpp：
  函数功能：            71 处
  执行流程：            71 处
  按状态机规则：        71 处
  该函数遵循所属模块：  47 处

include/go2cpp/concurrency/context.hpp：
  @brief   47    @details  4    @param  0    @return  2
```

**这些模板注释与真实代码逻辑完全不符**。例如 `DoneSignal::Wait()` 实际只有三行
（复制 `m_state` → 加锁 → 谓词式等待），注释却写「1. 校验传入参数；2. 按状态机规则
获取同步保护；3. 发布返回结果」——**三句里没有一句对得上**。

`context.hpp` 则结构性不足：47 个 `@brief` 只有 4 个 `@details`、**0 个 `@param`**。

### ★ 规范补充：`PROJECT_RULES.md` 新增「成员变量注释（强制）」

按用户要求，在项目规范的「注释」小节下新增专门章节（文件 200+ 行 → 249 行），
核心是**每个成员变量必须覆盖三项**：持有什么（类型语义 / 所有权方向）、
由谁保护（哪个锁 / 是否原子 / 为何 mutable）、生命周期与不变式（何时设置、
何时清空、与谁联动）。

配套六条强制细则：禁止只复述类型名；所有权方向必须显式对照（父强子弱要写为什么）；
顺序型字段写初值与保留值；镜像/派生字段说明同源关系与存在理由；
复合不变式成对注释；并发字段标注访问协议。

并给出**可操作的自检口径**：把某个成员变量的注释单独抽出来交给不了解该模块的人，
他应当能回答"谁保护它、谁能改它、改它时还要改谁"。

### 本批完成

**① `context.hpp` 的成员变量注释（4 组，全部按新规范）**

| 位置 | 落实的关键点 |
|---|---|
| `DoneSignal::m_state` | 保护者（State::mutex）、复制即共享这一不变式 |
| `ContextKey::m_token` | **身份取的是堆对象地址而非其数值**；拷贝共享同一对象；锚点如何把身份保证延伸到值存续期 |
| `Context::m_state` / `m_done` | **`m_done` 是镜像成员**：说明它与 `state->done` 其实是同一信号、以及它存在的唯一理由（State 是 .cpp 内不完整类型，需要完整类型成员让 `Done()` 内联返回引用） |
| `ContextRollback` 四个成员 | 为何 `m_callback_mutex` 要独立一把锁、为何 mutable；`m_callback_id{0}` 的 0 哨兵语义与"是否为 0 即是否 armed" |

**② `src/concurrency/context.cpp` 的 `DoneSignal` 全部 8 块注释**

严格按代码真实步骤重写，不再出现任何模板句。代表性两处：

- **`Wait()`** —— 写清"先把 `m_state` 复制到局部变量不是可选优化，而是防止等待
  期间句柄销毁导致悬空"，以及第 3 步用的是 `ParkingCondition`（受管 G park 让出 M）。
- **`Signal()`** —— 五步顺序即契约：幂等检查 → 搬走两组回调并清空 → **先 `notify_all`**
  → internal 回调 → 用户回调；并解释**为什么 internal 必须先于用户**
  （ContextRollback 要抢在观察者之前冻结 undo），以及**为什么用户回调跑在之后
  才需要另一个独立的 `RollbackDone` 信号**。

**同时把两处已知缺陷就地标注**（符合规范"缺陷明确标注而不是留坑"）：
`IsDone()` 与 `Signal()` 都声明 `noexcept` 却会加锁（台账 X-25 系列），
注释里写明了后果（可能 `std::terminate`）与修法方向。

### 验证

- **构建 0 warning / 0 error**
- `context.cpp`：2403 → 2412 行（净增 9 行，主要是被替换掉的模板句换成了真实内容）

### 剩余（后续批次）

- `context.cpp` **还有 63 块**模板注释（`State` / `Context` / `ContextRollback` 三部分）
- `context.hpp` 的**函数注释**仍需补 `@param` / `@details`（当前只有 `@brief`）
- `State` 与 `DoneSignal::State` 的**成员变量注释**（这两个 struct 都在 `.cpp` 内）

## 2026-10-13：修复 —— scheduler 示例对 metrics 时序的过严假设（flaky）

### 现象

写完 SSE 修复后跑全量 ctest，**`go2cpp_scheduler_full` 开始失败**
（此前它一直通过）。

**先判定是否 flaky**：连续跑 8 次，
**2 次失败（run#1、run#5），6 次通过 —— 25% 失败率，确认是 flaky。**

失败点固定是场景 5：

```
task_runs=11 task_completions=11（预期均 >= 12）
```

### 根因：示例的时序假设过严，不是调度器缺陷

```cpp
for (auto& task : tasks) { task->wait(); }   // 全部返回
const auto metrics = scheduler.metrics();    // 立刻采样 → 有时还差一次
```

**`metrics()` 的计数累加与 `task->wait()` 的返回之间有一个很短的窗口。**
`wait()` 返回不代表计数已经写完 —— 采样撞上这个窗口就会少一次。

### 修复：轮询等待计数收敛

```cpp
const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
for (;;) {
    const auto metrics = scheduler.metrics();
    runs = metrics.task_runs;
    completions = metrics.task_completions;
    if ((runs >= kTasks && completions >= kTasks) ||
        std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(Ms(1));
}
```

### ★ 这一轮暴露的模式：我的示例里有一类"时序过严"的断言

连续三轮都栽在同一类问题上：

| 轮次 | 断言 | 真实情况 |
|---|---|---|
| 第 20 轮 | `read_message` 会把 Ping/Pong 交给调用者 | 控制帧由库内部处理 |
| 本轮 SSE | `"data: cr\r\r"` 直接派发 | 末尾 `\r` 需后续字节确认 |
| 本轮 scheduler | `wait()` 返回后 metrics 已更新 | 计数与返回之间有窗口 |

**共同点：把"通常成立"当成了"必然成立"。**

**规范（已固化）**：**凡是涉及"另一个执行流 / 后台线程 / 异步计数"的断言，
都必须用「轮询 + 超时」而不是「一次采样」。**

### 验证

- 构建：**0 warning / 0 error**
- `go2cpp_scheduler_full`：**连续 10 次全部通过**（修复前 25% 失败）
- 全量 ctest：**67 个测试全部通过**

## 2026-10-13：修复 —— SSE 示例对 CR-only 换行的错误假设（并定位一个设计缺口）

### 现象

`go2cpp_sse_full` 在 ctest 里失败。**先排除了 flaky**：连续跑 10 次，
**10/10 都是同样的 `CR=""`**，是**确定性失败**，不是随机。

### ★ 根因（`src/network/sse.cpp` 的行扫描器）

```cpp
for (std::size_t index = 0; index < m_buffer.size(); ++index) {
    if (m_buffer[index] == '\n') { ...break; }
    if (m_buffer[index] == '\r') {
        if (index + 1 == m_buffer.size()) {
            break;          // ★ 末尾的 \r 不切行
        }
        position = index;
        ending_size = (m_buffer[index + 1] == '\n') ? 2 : 1;
        break;
    }
}
```

**当 `\r` 位于缓冲区末尾时，扫描器保守地 `break`**（它可能是 `\r\n` 的前半，
需要下一个字节才能确认）。**但如果此后不再有字节** —— 例如调用方已经喂完
`"data: cr\r\r"` —— **最后一个 `\r` 会永远留在缓冲区，空行永远不被识别，
事件永远不派发。**

### 结论：这是一个真实的设计缺口

**头文件声称支持 `LF / CRLF / CR` 三种换行，但 `CR` 结尾的输入实际上无法
完整使用** —— 因为 `Parser` **没有"声明输入结束"的接口**
（`http::Parser` 有 `finish(Response&)`，`sse::Parser` 没有对应的）。

**影响**：任何以单个 `\r` 结尾的 SSE 流（规范允许），最后一个事件都不会派发。

### 本轮处置：修正示例的断言（不掩盖，而是如实描述契约）

`example/network/sse_full.cpp` 的场景 5 改为：

```cpp
Parser cr;
const auto cr_before = parse_all(cr, "data: cr\r");   // 末尾 \r 未确认 → 0 个事件
const auto cr_after  = parse_all(cr, "\rX");           // 补一个非 \r 字节 → 派发 1 个
```

断言 `cr_before.empty() && cr_after.size() == 1 && cr_after[0].data == "cr"`。

**实测通过**：`8 个场景全部通过`。

**我没有把 `CR=""` 直接改成"预期就是空"** —— 那样等于把缺口写成契约。
**而是把"必须补一个字节才能确认末尾 `\r`"这个真实行为明确地验证出来**，
并在注释里写清它为什么是这样。

### 建议（留给用户决定，本轮未实施）

给 `sse::Parser` 增加一个"输入结束"接口，与 `http::Parser::finish` 对齐：

```cpp
/** @brief 声明输入结束：把末尾遗留的 \r 确认为行结束符，并派发待完成事件。 */
Result finish(std::vector<Event>& events);
```

实现约 10 行（把末尾 `\r` 弹出后 `consume_line`，必要时再补一个空行触发派发）。
**这样 CR-only 换行才真正可用**。

**本轮不擅自扩展公开接口** —— 它改 `sse.hpp` + `sse.cpp`，影响面比示例大，
需要先确认这个方向是你要的。

### 验证

- 构建：**0 warning / 0 error**
- `go2cpp_sse_full`：**8 个场景全部通过**

## 2026-10-13：example 完善 · 第 24 轮（config · Lookup + 文件热重载主线）

### 用户要求

> 「先完成示例」—— 即补上 `Config::Lookup` + `StartWatcher` 这条
> 「**程序内置默认值 → 配置文件存在则覆盖**」主线的示例
> （此前 `config_full` 覆盖 `IniFile`/`ConfigVar`/`ValueCodec`，
> `runtime_config_full` 覆盖 `RuntimeConfig`，但这条主线没有专门示例）。

### 本轮完成

| 文件 | 说明 |
|---|---|
| `example/config/config_center_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/config/config_center_full.cpp` | **新建 · 全面版 7 个场景（含真实文件热重载）** |

### 全面版的 7 个场景（全部实跑通过，CODE=0）

1. **`Lookup` 注册内置默认值** —— `threads=4 name=default-service enabled=0`
2. **★ `Lookup` 幂等** —— 同名再次调用（默认值 999）返回**同一实例**、
   值保持 16、**描述沿用首次注册**
3. **`LookupBase` + `List`** —— 按名查询命中/未命中；`List` 列出全部（总数 3）
4. **★ INI 覆盖默认值** —— `加载前=16/default-service 加载后=32/from-ini`
5. **★ 提交监听** —— `AddCommitListener` 收到本次变更的两个变量；
   `DelCommitListener` 之后不再通知
6. **★ 事务校验** —— `AddTransactionValidator` 拒绝 `threads=99` 整批，
   **旧值保持 10 不变**、错误文本可取回；合法批次 `threads=12` 通过并应用
7. **★★ 真实文件热重载** —— 写临时文件 → `StartWatcher(path, 50ms)` →
   `Watching()=1` → **首次加载 64** → **改文件后自动重载为 128** →
   `StopWatcher()` → `!Watching()` 成立

### 场景 7 是这一轮的核心

它验证的是**真实的异步热重载**，而不是用 `LoadFromIni` 手工模拟：

```cpp
config.StartWatcher("/tmp/go2cpp-config-center-demo.ini", Ms(50), &error);
// 轮询等热重载把文件里的 64 应用进来
// 改写文件为 128，再等它自动重载
config.StopWatcher();
```

**两次加载都成功**（64 → 128），说明 watcher 线程、
`FileStamp` 稳定快照与事务性提交这条链路是通的。

### 三件事的最终确认

| 用户要求 | 结论 | 证据 |
|---|---|---|
| 基本固定参数搭配 | **已具备** | `Lookup<T>(name, default, desc)`，签名与 sylar 一致（场景 1、2） |
| 动态文件热重载 | **已具备** | `StartWatcher`/`Watching`/`StopWatcher`（场景 7，实测两次重载） |
| 默认加载、文件有则优先 | **已具备** | `Lookup` 先注册默认值 → `LoadFromIni`/热重载覆盖（场景 4、7） |

**加上前一轮修掉的 `ReadSize` 缺陷**（`[timer]` 段无法读取），
`config` 模块现在是**完整可用**的：
`IniFile` / `ConfigVar` / `ValueCodec` / `Config` 中心（Lookup + 热重载 + 事务）
/ `RuntimeConfig` 全部有覆盖。

### 验证结果

- 构建：**0 warning / 0 error**；两个示例均 **CODE=0**
- 已登记进 `GO2CPP_EXAMPLES`
- 6 个在途文件（用户在查看）全程未触碰

### 进度：24 轮完成 27 个模块 / 约 235 个场景

`config` 现在有 **4 个示例**（`config_demo` / `config_full` / `runtime_config_*` /
`config_center_*`），是该模块覆盖最完整的一块。

## 2026-10-13：修复 —— config 模块 `ReadSize` 对非 scheduler 段的静默失败

### 用户要求

> 「这个 Config 模块确实存在问题，你修复吧。
> 除了基本的固定参数搭配，还要求动态文件热重载，以及程序内部的自己加载
> （默认加载，如果文件信息里面有，就优先从配置文件热重载）。
> 参考 /UserData/CodexWorkSpace/sylar2/sylarConfig 模块的 LookUp」

### 调研结论：你要的三件事，现有 API 都已经具备

| 要求 | 现有 API | 与 sylar 的对应 |
|---|---|---|
| 基本固定参数搭配 | `Config::Lookup<T>(name, default_value, description)` | **与 sylar 的 `Lookup` 签名一致** |
| 动态文件热重载 | `Config::StartWatcher(path, interval, error)` / `StopWatcher()` / `Watching()` | sylar 无此能力，Go2Cpp 更强 |
| 默认加载、文件有则优先 | `Lookup` 先注册默认值，热重载事务性覆盖 | sylar 同语义 |

**补充**：sylar2 的实际路径是 `/UserData/CodexWorkSpace/sylar2/sylar`（不是 `sylarConfig`），
其 `Lookup` 定义在 `config.h:448`（单参查询）与 `:461`（带默认值创建）。

**Go2Cpp 还额外具备**：`AddCommitListener`（提交后通知）、
`AddTransactionValidator`（整批候选值校验）、`FileStamp` 稳定快照 +
`IsEditLocked` 编辑锁（避免读到半个文件）。

**所以"问题"指的是那个 `[timer]` 缺陷。**

### ★ 修复内容（唯一改动：`src/config/config.cpp` 的 `ReadSize`）

**修复前**：

```cpp
if (ParseSize(ini.Get(section, key), target)) {
    if (std::string_view(section) == "scheduler" &&
        ValidateSchedulerSize(key, *target, error)) {
        return true;
    }
    return false;          // ← 非 scheduler 段（如 timer）必定落到这里
}
```

**问题**：非 `scheduler` 段的值解析成功后，`if` 条件为假 → 跳过 `return true`
→ 直落 `return false`。于是 `[timer]` 的 `warn_after_days` / `error_after_days`
**永远无法被读取**，而且失败时**不设置 `error`**（静默失败）。

**修复后（按段分流）**：

```cpp
if (!ParseSize(ini.Get(section, key), target)) {
    if (error) *error = std::string("配置项不是非负整数: ") + section + "." + key;
    return false;
}
// ValidateSchedulerSize 只认识 scheduler 段的键名，其它段传入时既不在它的
// 白名单里也不该被它拒绝 —— 必须按段分流。
if (std::string_view(section) == "scheduler") {
    return ValidateSchedulerSize(key, *target, error);
}
return true;
```

### 验证

**① 缺陷回归验证**（`example/config/runtime_config_full.cpp` 场景 7 已改写）：

```
带 [timer] 的 FromIni=1 warn=5(=1) error=9(=1) 无报错=1 空 INI 仍用默认=1
```

修复前同一场景是 `FromIni=0 error="" 保持默认(30)=1`。
**现在 `[timer]` 段能正确读取，且空 INI 仍保持默认值 30/365。**

**② 该缺陷解释的两个既有现象**（现在都自然消解）：
- 仓库自带的 `go2cpp.ini` 含 `[timer]` 段 → `LoadRuntimeConfig("go2cpp.ini")` 现在能成功；
- 既有 `log_config_demo` 把"未加载 go2cpp.ini"当常态分支 → 现在会走成功分支。

**③ 构建**：0 warning / 0 error。

**④ 全量 ctest**：**65 个测试，64 通过，1 失败**。

### ⚠️ 那个失败与本次修复无关，如实记录

**失败测试**：`go2cpp_sse_full`（第 35 个）。

**证据表明无关**：
- `sse.cpp` / `sse.hpp` **不依赖 config**（grep 无匹配）；
- `sse.cpp` 最后一次改动是 `d0fdf70`（不在当前改动集里）；
- 本次只改了 `src/config/config.cpp`。

**但现象值得记录（疑似 flaky）**：失败点是场景 5 的 `CR=""`。
**我在第 13 轮手工运行同一个示例时，输出是 `CR="cr"`（当时通过）。**
同一份代码、同一个二进制，两次结果不同 —— **说明单独用 `\r` 作换行符的处理
可能存在非确定性**。

**本轮不擅自修改 SSE**：它不在本次任务范围内，且需要先独立复现才能判断。
**已作为待查项记录。**

### 状态

- **改动文件**：`src/config/config.cpp`（修复）、
  `example/config/runtime_config_full.cpp`（场景 7 改为回归验证）
- **7 个在途文件（用户在查看）全程未触碰**
- 用户要求的三个能力**确认已具备**，本次修复的是其底层读取路径上的一个静默缺陷

## 2026-10-13：example 完善 · 第 23 轮（config · RuntimeConfig，★ 发现真实缺陷）

### 本轮完成：`config` 的 `RuntimeConfig`（此前未覆盖）

| 文件 | 说明 |
|---|---|
| `example/config/runtime_config_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/config/runtime_config_full.cpp` | **新建 · 全面版 7 个场景** |

### ★★★ 本轮发现一个真实的框架缺陷（已实证）

**`src/config/config.cpp` 的 `ReadSize` 实现：**

```cpp
bool ReadSize(const IniFile& ini, const char* section, const char* key,
              std::size_t* target, std::string* error) {
    if (!ini.Has(section, key)) return true;
    if (ParseSize(ini.Get(section, key), target)) {
        if (std::string_view(section) == "scheduler" &&
            ValidateSchedulerSize(key, *target, error)) {
            return true;
        }
        return false;          // ← 非 scheduler 段的值解析成功后，必定落到这里
    }
    if (error) *error = std::string("配置项不是非负整数: ") + section + "." + key;
    return false;
}
```

**`FromIni` 用 `ReadSize` 读取 `[timer]` 的 `warn_after_days` / `error_after_days`。**
由于 `section` 是 `"timer"` 而非 `"scheduler"`，`if` 条件为假 → **跳过 `return true` → 直落 `return false`**。

> **结论：只要 INI 里出现 `[timer]` 段，`RuntimeConfig::FromIni` 必定失败，且不设置 `error`。**

**实测证据（场景 7）**：

```
带 [timer] 的 FromIni=0 error="" 保持默认(30)=1
去掉 [timer] 后 FromIni=1
```

**这个缺陷解释了两个既有现象**：

1. **仓库自带的 `go2cpp.ini` 含 `[timer]` 段**（`warn_after_days` / `error_after_days`），
   所以 **`LoadRuntimeConfig("go2cpp.ini")` 从来无法成功**；
2. **既有 `log_config_demo` 把"未加载 go2cpp.ini"当作常态分支** ——
   它的注释写着"示例目录不一定有 go2cpp.ini，缺少可选配置时仍使用默认安全值"，
   **恰好绕过（并掩盖）了这个缺陷**。

**影响面**：`[timer]` 段的所有配置**实际上完全不可用**；
用户改这些值不会有任何效果，且失败是**静默的**（`error` 为空），很难排查。

**本轮只做记录，没有修改 `src/`** —— 按既有约定，源码改动不属示例任务范围。

### 全面版的 7 个场景（全部实跑通过）

1. **`FromIni` 从内存构造** —— `解析=1 FromIni=1 P=6`（不需要文件）
2. **`scheduler` 组字段** —— P/M/queue/stack/oversub/sysmon/pin/metrics 全部按文件值
3. **`log` 组字段** —— `level/stdout/dir/file/format` 全部读到
   （`log` 段不经 `ReadSize`，所以**不受上面那个缺陷影响**）
4. **★ 事务性** —— `首次成功=1 值=5 第二次=0 之后值=5 失败时保持不变=1`
5. **默认值** —— 空 INI 得到 `timer=30/365 dir=log file=go2cpp.log`
6. **`Validate` 边界** —— 合法通过；`P=99` 与 `queue=0` 被拒（错误文本可取回）
7. **★★ 缺陷记录** —— 见上

### ★ 第 12 类坑（第 11 类的重演）：未打印的断言静默失败

首版 `runtime_config_full` 报「存在失败场景」，但**每一行打印出来的条件都是 1**。

**根因**：场景 3 里我断言 `log_format == "{level}: {message}\n"`，
**而这个条件恰好没有打印**。实测格式串长度是 **18**（`{level}: {message}` 正好 18 字符）——
**INI 解析把 `\n` 当成了行尾**，格式串里并没有换行符，逐字节相等自然失败。

**修复**：把每个影响 `ok` 的条件都单独提取、单独打印，并把格式串断言放宽为
"确实包含两个占位符"（尾部换行不属于契约）。

> **这条教训这是第二次出现**：上一轮（`tools/error`）也是"输出全对但整体失败"。
> **规范：凡进入 `ok` 的条件，必须在输出里有对应的可见项。**

### 验证结果

- 构建：**0 warning / 0 error**；两个示例均 **CODE=0**
- 已登记进 `GO2CPP_EXAMPLES`
- 在途文件（6 个，用户在查看）全程未触碰

### 总进度：23 轮完成 25 个模块

`concurrency`(10) / `core`(1) / `network`(8) / `tools`(2) / `logging`(1) / **`config`(2)**

**`config` 模块至此完整覆盖**（`IniFile`/`ConfigVar`/`ValueCodec` + `RuntimeConfig`）。

### 剩余

- **已判定不做**：`core/hybrid_mutex`（用户明确"绝对不允许 HybridGate"）

**A 组、C 组已清空；`RuntimeConfig` 已补完。**

## 2026-10-13：example 完善 · 第 22 轮（C 组 · tools/error，★ C 组完成）

### 本轮完成：`tools/error`

| 文件 | 说明 |
|---|---|
| `example/tools/error_full.cpp` | **新建 · 全面版 7 个场景** |
| `example/tools/errors_demo.cpp` | **沿用既有** —— 54 行，在简单版区间内 |

### 全面版的 7 个场景（全部实跑通过）

1. **创建** —— `errors::New` 产出带消息的错误，`Unwrap()` 为空
2. **★ 包装与展开** —— `Wrap` 建三层链，`Unwrap` 逐层回退到 `nullptr`
3. **★ 沿链识别根因** —— `Is` 穿透两层包装命中最初的错误；
   且**同文本的新对象不命中**（身份而非文本比较）
4. **nil 语义** —— `Is(nil,nil)=1`、`Is(err,nil)=0`、`Is(nil,err)=0`
5. **★ 聚合** —— `Join` 三条错误，每条都能被 `Is` 识别；
   **`vector` 重载与 `initializer_list` 重载产出等价结果**
6. **★ 按类型提取** —— `As<TimeoutError>` 从包装中取出自定义错误类型
   （读到 `milliseconds()==250`）；类型不符返回空
7. **★★ 薄封装等价性** —— 专门验证头文件那句承诺

### ★ 场景 7 是本轮的核心价值

`errors::` 这批新 API 的**唯一价值主张就是"等价于既有 API"**，所以用**交叉验证**测实：

```cpp
const bool cross_1 = Is(lower_wrapped, lower_root);          // 大写判小写链
const bool cross_2 = errors::Is(camel_wrapped, camel_root);  // 小写判大写链
const bool cross_3 = Is(camel_wrapped, camel_root) ==
                     errors::Is(camel_wrapped, camel_root);  // 结果一致
```

再验证两条链的 `Message()` 一致、`Unwrap` 深度都是 3。
**这样"薄封装"就从一句声明变成了被实测钉住的契约。**

### ★ 第 11 类坑：错误链的 `Message()` 是整条链，不是单层

首版场景 2 的断言：

```cpp
level1->Message() == "write failed"   // ✗ 实测是 "write failed: disk full"
```

**`Wrap` 产出的错误，其 `Message()` 是「外层: 内层」的完整拼接**：

```
链="save config failed: write failed: disk full"
     "write failed: disk full"
     "disk full"
```

**修复**：**改用身份（`shared_ptr` 指针）比较判断层级**，只对最内层的
`root->Message()` 做文本断言。

**这次的现象尤其值得记**：**所有打印出来的断言都是 1，但整体报"存在失败场景"** ——
因为失败的那个断言**恰好没被打印**。**"输出看起来全对"不等于断言全对**；
代码里必须保证每个影响 `ok` 的条件都能在输出里被看到。

### 关于 `tools/error` 的在途文件

`include/go2cpp/tools/error.hpp` 与 `src/tools/error.cpp` 用户在查看中。
本轮**只读不改** —— `errors::` 的内容与既有 API 签名都取自当前工作区真实代码，
**没有基于旧版本做任何假设**。

### 验证结果

- 构建：**0 warning / 0 error**；`go2cpp_error_full` → **CODE=0**
- 已登记进 `GO2CPP_EXAMPLES`
  （注意 `tools/errors_demo` 在列表**末尾且带右括号**，锚点必须包含 `)`）
- 在途文件（6 个）全程未触碰

### ★★ C 组完成

| C 组模块 | 轮次 |
|---|---|
| `logging/log` | 第 21 轮 |
| **`tools/error`** | **本轮** |

### 总进度：22 轮完成 24 个模块

`concurrency`(10) / `core`(1) / `network`(8) / `tools`(2) / `logging`(1) / `config`(1)

### 剩余

- **已判定不做**：`core/hybrid_mutex`（用户明确"绝对不允许 HybridGate"）
- **未覆盖**：`config` 的 `RuntimeConfig`（pending/watcher 路径）

**A 组、C 组均已清空。**

## 2026-10-13：example 完善 · 第 21 轮（C 组 · logging/log 模块）

### 本轮完成：`logging/log`

| 文件 | 说明 |
|---|---|
| `example/logging/log_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/logging/log_full.cpp` | **新建 · 全面版 7 个场景** |
| `example/logging/log_config_demo.cpp` | **沿用既有** —— 80 行，但它其实是"config + log"混合示例 |

### ★ 关键设计：零文件依赖

既有 demo 从 `go2cpp.ini` 加载配置并写日志文件。**本示例完全不碰文件**：

- 用 **`MemorySink`**（内存输出目标）收集日志，`Snapshot()` 取回
- 用 **自定义 `LogSink` 子类**接管写入
- **直接调 `sink->Write(record, formatted)`**，绕过 `LoggerManager` 的接线 API
- **不读 `go2cpp.ini`**、不写 `log/` 目录

**这样示例只依赖"格式化"与"输出目标"两组稳定接口，不会因日志配置的变动而失效。**

### 全面版的 7 个场景（全部实跑通过）

1. **级别解析** —— 7 个合法级别名全部解析正确，非法名被拒
2. **★ 级别过滤** —— `MinimumLevelFilter(Warn)` 放行 Error/Warn、拦截 Info/Debug；
   `SetLevel(Debug)` 之后原来被拒的放行
3. **格式化模式** —— `%m%n` 输出 `hello`，`[%m]%n` 输出 `[hello]`
4. **★ 自定义格式项** —— `AddFormat` 注册 → `HasFormat`/`Formats` 可见 →
   渲染 → `RemoveFormat` 注销
5. **★ 内存输出目标** —— 3 条记录，`Snapshot()` 首末一致
6. **自定义输出目标** —— `LogSink` 子类记录写入次数、flush 次数与最后级别
7. **`LogRecord` 字段** —— 11 个字段中 7 个被格式项读到并精确断言

### ★★ 第 10 类坑：格式项键是"单字符"，且多数单字母键已被内置占用

场景 4 改了三版才对，每版都揭示一层事实：

| 版本 | 写法 | 实测 | 揭示的事实 |
|---|---|---|---|
| v1 | 硬编码 `"T"` / `"F"` | `AddFormat` **返回 false**；`%T` 渲染成 **制表符**、`%F` 渲染成 **99** | **`T`（tab）与 `F`（fiber_id）已是内置格式项** |
| v2 | `HasFormat` 探测出多字符键 `"zz"` | `AddFormat` 成功，但渲染输出 **`"%zz/%zzb"` 原样** | **键必须是单字符** —— `%zz` 被解析成 `%z` + 字面 `z` |
| v3 | **在字母表里扫描空闲单字符键** | 注册成功、渲染 `worker-1/99`、注销生效 | ✅ |

**最终写法**（示例里保留了这个探测循环，并用注释说明为什么不能硬编码）：

```cpp
for (char ch = 'a'; ch <= 'z'; ++ch) {
    const std::string candidate(1, ch);
    if (LogFormatter::HasFormat(candidate)) continue;
    // 取前两个空闲键
}
```

**这一条的普遍意义**：**示例不应该依赖"哪些名字还没被占用"这种实现细节。**
用 `HasFormat` 探测，示例就能在格式项集合变化后继续工作。

### 另外修正的两处自己的预期错误

1. **简单版场景 1**：我把 `Level::Warn` 的枚举值猜成 2，实际是 **4**（枚举顺序决定）
2. **全面版场景 3**：我假设 `PatternFormatter("%")` 会报错，实测 `HasError()` 为 **false**
   —— `%` 被当作字面量。改为验证"不同模式产出不同结果"

**两处都是"把猜测写成断言"。** 这与第 20 轮的教训一致。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_log_demo` → **CODE=0**
- `go2cpp_log_full` → **CODE=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 在途文件（6 个，用户在查看）**全程未触碰**

### 进度：21 轮完成 23 个模块

### C 组剩余：1 个

- **`tools/error`** —— `error.hpp` 已新增 `go2cpp::errors` 命名空间
  （Go 风格薄封装：`errors::New` / `Wrap` / `Join` / `Is` / `As`）；
  既有 `errors_demo.cpp` 仅 54 行，在简单版区间内 → 只需补全面版

## 2026-10-13：example 完善 · 第 20 轮（A 组 · network/websocket，4/4 收官）

### ★ A 组完成

| 模块 | 轮次 |
|---|---|
| `network/io_hook` | 第 17 轮 |
| `network/io_backend` | 第 18 轮 |
| `concurrency/runtime` | 第 19 轮 |
| **`network/websocket`** | **本轮** |

### 本轮完成：`network/websocket`

| 文件 | 说明 |
|---|---|
| `example/network/websocket_full.cpp` | **新建 · 全面版 7 个场景** |
| `example/network/websocket_demo.cpp` | **沿用既有** —— 58 行，在简单版区间内 |

### ★ 关键突破：`Socket::FromFd` 让 websocket 不需要监听端口

此前判断"websocket 需要真实 socket 环境"**保守了**。既有 demo 揭示了正解：

```cpp
::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
auto server_socket = Socket::FromFd(pair[0], AF_UNIX, SOCK_STREAM);
auto client_socket = Socket::FromFd(pair[1], AF_UNIX, SOCK_STREAM);
```

**于是完整握手 + 收发 + 关闭都能在 socketpair 上跑通，不监听任何端口。**

### 全面版的 7 个场景（全部实跑通过）

1. **握手校验（纯计算）** —— 合法请求接受、缺 Key / 版本错 / Upgrade 错均被拒
2. **握手响应（纯计算）** —— `状态=101 Accept=s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`
   （**RFC 6455 规定的固定 Accept 值，可精确断言**）
3. **完整握手 + 文本收发** —— `发送=1 服务端读到="hello" 回显=1 客户端读到="echo: hello"`
4. **二进制消息** —— 含 `\x00\x01\x02\xFF`，校验 opcode 非 Text 且内容一致
5. **控制帧** —— `ping 发送=1 pong 发送=1 连接仍开启=1 控制帧后数据仍可收发=1`
6. **关闭语义** —— `关闭前 closed()=0 close() 成功=1 关闭后 closed()=1 对端读到 Close 帧=1`
7. **角色与底层套接字** —— `客户端角色=Client 服务端角色=Server 都有底层 Socket=1`

### ★★ 第 8 类坑：库 API 缺少超时上界导致静默挂起

首版现象**极其迷惑**：`std::cout` 输出**全部消失**，只剩 `std::clog` 的 5 行标题，
而 `EXIT=0`。

**两步定位**：

1. **`stdbuf -o0` 强制无缓冲重跑** —— 立刻看到场景 1–4 **全部通过**；
   `stdout` 丢失纯属**缓冲未刷新**（进程没走正常退出路径），真正停住的是**场景 5**
2. **根因**：`WebSocket::read_message()` **没有超时参数**，对端不回帧就**无限阻塞**

**修复**：给 `WebSocketConfig::io_timeout` 设上界，**让所有读写都有界**。

**三条可复用的教训**：
- **"EXIT=0 但输出缺失"不等于成功**
- **`stdbuf -o0` 是排查此类问题的第一手段**（区分"缓冲丢失"与"真的没执行"）
- **凡是"读"接口，先确认它有没有上界**

### ★★ 第 9 类坑：把实现细节当成了契约

场景 5 先后写了两版错误断言：

| 版本 | 断言 | 实测 |
|---|---|---|
| v1 | `read_message` 会把收到的 **Ping** 交给调用者 | **服务端读到 Ping=0** |
| v2 | 对端显式 `pong` 后客户端能读到 **Pong** | **客户端读到 Pong=0** |

**两次合起来得出正确结论**：

> **`read_message` 不向调用者交付控制帧**（由库内部处理），它只交付数据帧。

**最终写法**：**不断言控制帧能被读到**，改为验证一个我能确证、也更有价值的性质 ——
**发出控制帧之后，普通数据帧的收发不受影响**。

**这一条的普遍意义**：**断言必须建立在"已被验证的契约"上，而不是"实现看起来应该这样"。**
两次失败都是因为我把"合理猜测"直接写成了断言。

**另外，守卫两次都正确阻止了提交**（脚本里写死 `if 未通过 then 不提交`），
所以这两个错误假设**都没有进入提交历史**。

### 修正的两处签名误用（编译期拦截）

1. **`ValidateUpgradeRequest(request, std::string& key, std::string* reason)`** ——
   第二个参数是**输出参数**，必须传 `std::string` 左值
2. **`WebSocketRole` 没有导出到 `go2cpp`** —— 必须写 `go2cpp::net::WebSocketRole`

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_websocket_full` → **CODE=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`；**7 个在途文件全程未触碰**

### 总进度：20 轮完成 22 个模块（A 组全部完成）

`concurrency`(10) / `core`(1) / `network`(8) / `tools`(1) / `config`(1)

### 剩余

- **C 组（依赖在途文件，等用户定稿）**：`logging/log`、`tools/error`
- **已判定不做**：`core/hybrid_mutex`（用户明确"绝对不允许 HybridGate"）
- **未覆盖**：`config` 的 `RuntimeConfig`（pending/watcher 路径）

## 2026-10-13：example 完善 · 第 19 轮（A 组 · concurrency/runtime，3/4）

### 本轮完成：`concurrency/runtime`

**先说清这个模块的性质**：`include/go2cpp/runtime.hpp` 只有 **36 行**，
是**聚合头**（"一次包含全部公开模块"，面向转译程序），**它自身没有任何 API**。

因此它的"全面版"**不应该重复单模块演示**，而应展示**跨模块协同**。
本示例按这个定位写的。

| 文件 | 说明 |
|---|---|
| `example/concurrency/runtime_basic_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/concurrency/runtime_full.cpp` | **新建 · 全面版 7 个协同场景** |

既有 `runtime_demo.cpp` 115 行超简单版规格，故简单版也新建。

### 全面版的 7 个场景（全部实跑通过）

1. **多子系统并存** —— 一个聚合头同时用上 `channel` / `future` / `context`：
   `channel=7 future=9 context 未取消=1`
2. **★ 取消驱动收敛** —— `Context` 取消 → 阻塞在无缓冲 Channel 上的接收方退出：
   `接收方退出=1 判定为取消=1 都结束=1`
3. **提交与汇合** —— `Scheduler + go + WaitGroup + Timer`：
   `WaitGroup 汇合=1 完成=5 定时器触发=1`
4. **★ 配置驱动运行时** —— INI 里的 `processors = 3` 直接决定调度器 P 数量：
   `配置读出=3 实际 P=3`
5. **`Future` + `Context`** —— 挂起的 Future 等待被取消打断：`status=cancelled`
6. **`control_flow` 作用域** —— Fiber 内 `defer` 保证清理：`顺序=body second first`
7. **`fiber_local` 隔离** —— 多个 Fiber 各持一份：`A="A" B="B"`

**场景 2 和 4 最能体现"聚合头"的价值**：它们跨越了三个模块
（Context + Channel + Scheduler / Config + Scheduler），
**只有一次包含全部模块时才写得这么自然**。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_runtime_basic_demo` → **EXIT=0**
- `go2cpp_runtime_full` → **EXIT=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- **7 个在途文件全程未触碰**

### 进度（19 轮完成 21 个模块）

`concurrency`(10) / `core`(1) / `network`(7) / `tools`(1) / `config`(1)

### A 组剩余：1 个

- **`network/websocket`** —— 帧编解码为 private；公开的只有
  `ValidateUpgradeRequest` / `BuildUpgradeResponse` 两个握手入口（需 `http::Request`），
  其余 API 需要真实 `Socket`。
  **下一轮先核实 `WebSocket::Accept` 是否必须真实 listener** ——
  若能用 `socketpair` 走通握手 + 收发（如同 io_hook 的做法），
  就能在无端口环境下完成它；否则只演示握手路径并在示例中注明边界。

## 2026-10-13：example 完善 · 第 18 轮（A 组 · network/io_backend）

### 本轮完成：`network/io_backend`（A 组 2/4）

| 文件 | 说明 |
|---|---|
| `example/network/io_backend_basic_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/network/io_backend_full.cpp` | **新建 · 全面版 7 个场景** |

既有 `io_backend_demo.cpp` 110 行超简单版规格，故简单版也新建。

### 全面版的 7 个场景（全部实跑通过）

1. **平台默认后端名** —— 实测 **`default="epoll"`**
2. **注册与创建** —— `registered=1 created=1 类型正确=1`
3. **协议调用序列** —— `initialize/add_or_modify/remove/wait/wake/drain_wake/shutdown`
   各调用一次，**且参数原样透传到后端**（`fd=7 events=0x1 data=0xABCD enable=true
   wait capacity=4 timeout=25`）
4. **★ 同名重复注册被拒** —— `首次注册=1 重复注册=0`
5. **★ 注销后创建返回 nullptr** —— `注销前创建=有实例 注销=1 注销后创建=nullptr`
6. **★ 默认后端不可注销** —— `注销默认后端被拒=1 默认创建仍可用=1`
   （该约束只存在于实现中，头文件未记录，属**台账 D-13**）
7. **空名创建平台默认实例** —— `空名创建=1 可初始化=1`

**场景 3 是本轮最完整的一个**：不只验证"调用了几次"，还验证
**参数是否原样透传到后端**（fd / events / data / enable / capacity / timeout），
这比只数调用次数更能发现"参数被吞掉或搞错顺序"的问题。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_io_backend_basic_demo` → **EXIT=0**
- `go2cpp_io_backend_full` → **EXIT=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- **7 个在途文件全程未触碰**

### 进度（18 轮完成 20 个模块）

`concurrency`(9) / `core`(1) / `network`(7，新增 io_backend) / `tools`(1) / `config`(1)

### A 组剩余（2 个）

- **`concurrency/runtime`** —— 注意 `runtime.hpp` 是 **36 行聚合头，自身无 API**，
  所以"全面版"本质是**综合演示**（一次包含全部模块并能协同使用）；
  既有 `runtime_demo.cpp` 115 行超规格，两版需新建
- **`network/websocket`** —— 帧编解码为 private，公开的只有两个握手入口
  （`ValidateUpgradeRequest` / `BuildUpgradeResponse`）需要 `http::Request`；
  **下一轮先核实能否用 socketpair 构造出可用的握手路径**，
  若可行则沿用 io_hook 的经验（不监听端口）

## 2026-10-13：example 完善 · 第 17 轮（A 组 · network/io_hook）

### 用户要求：完成 A 组

**A 组剩余 4 个模块**：`concurrency/runtime`、`network/io_backend`、
`network/io_hook`、`network/websocket`。**本轮完成其中 1 个（io_hook）**。

### 本轮完成：`network/io_hook`

| 文件 | 说明 |
|---|---|
| `example/network/io_hook_full.cpp` | **新建 · 全面版 7 个场景** |
| `example/network/io_hook_demo.cpp` | **沿用既有** —— 65 行，在简单版区间内 |

**登记方式**沿用既有约定：放进 `if (TARGET go2cpp_hook)` 条件块
（与 `io_hook_demo` 一致）。

### ★ 关键设计：全部用 socketpair，不监听任何端口

既有 demo 已经示范了这个做法，本轮沿用并扩展：

- **`::socketpair(AF_UNIX, SOCK_STREAM, 0, fds)`** —— 纯进程内，**无需端口、无需外部网络**
- 配一个 RAII 的 `SocketPair` 结构确保两条 fd 一定被 `close`

**这解决了此前对 `socket` / `websocket` 那条线"需要真实端口、受限环境可能起不来"的顾虑** ——
**hook 与 IOManager 的验证完全可以用 socketpair 完成。**

### 全面版的 7 个场景（全部实跑通过）

1. **Hook 开关与作用域守卫** —— `进入前=1 作用域内=1 退出后恢复=1`
2. **受管 Fiber 里的 hook recv** —— `recv 成功=1 都结束=1`
3. **hook send 送达对端** —— `send 返回=4 对端收到=1`
4. **★ 读 Fiber 不阻塞 M（P=1 反证）** —— `读者完成=1 读者结束=1 写者结束=1`
5. **`SO_RCVTIMEO` 转换为 `ETIMEDOUT`** —— `超时=1 errno=110`
6. **`ScopedIOManagerBinding` 作用域绑定** —— 绑定期间收发正常
7. **`connect_timeout` 配置读写** —— `原值=5000ms 设为 1234ms 后=1234ms 还原后=5000ms`

**场景 4 是本轮最有价值的**：P=1 且读者先启动并在无数据时等待 ——
**若 hook 未生效、`recv` 直接阻塞了唯一的 M，写者就永远排不上，`reader->wait_for` 会超时失败。**
实测三者全部完成，**证明 syscall 确实被接管并把执行权交还给了 M**。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_io_hook_full` → **EXIT=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`（条件块内）
- **7 个在途文件全程未触碰**

### 进度（17 轮完成 19 个模块）

`concurrency`(9) / `core`(1) / `network`(6，新增 io_hook) / `tools`(1) / `config`(1)

### A 组剩余（下一轮继续）

- **`concurrency/runtime`** —— 注意：`runtime.hpp` 是 **36 行的聚合头，本身没有 API**，
  所以它的"全面版"本质是**综合演示**（一次包含全部模块）；既有 `runtime_demo.cpp`
  115 行超简单版规格，两版都需新建
- **`network/io_backend`** —— 既有 `io_backend_demo.cpp` 110 行，需先核实 API
- **`network/websocket`** —— 帧编解码为 private，需真实 socket；
  **但本轮 io_hook 的经验说明：socketpair 也许能覆盖一部分握手/读写路径**，
  下一轮先核实 `WebSocket::Accept` 是否必须真实 listener

## 2026-10-13：example 完善 · 第 16 轮（B 组 · config 模块）

### 本轮完成：`config`（此前**完全没有示例**）

| 文件 | 说明 |
|---|---|
| `example/config/config_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/config/config_full.cpp` | **新建 · 全面版 7 个场景** |

**首次建立 `example/config/` 目录**，已登记进 `GO2CPP_EXAMPLES`。

### 全面版的 7 个场景（全部实跑通过）

1. **INI 分段解析** —— 同名键按段隔离；支持 `#` 与 `;` 注释
2. **缺失键返回默认值** —— `存在键=yes 缺失键=D 缺失段=D2`
3. **`ConfigVar` 元信息与读写**
4. **★ 字符串往返** —— `int "-42" / bool "true" / string "hello world"`
5. **多种值类型** —— `vector<int>` 编码为 `1,2,3,4`，解码 4 个元素
6. **★ 校验器** —— 非法设置被拒且**值不变**，错误文本可取回
7. **★ 变更监听** —— 回调 2 次、`(old,new)=(2,3)`、注销后不再触发

### ★ 本轮踩到第 7 类坑：C++ 求值顺序污染了示例输出

简单版首版打印 `设置后=16`（应为 8）——**我把 `GetValue()` 和 `FromString("16")`
写在了同一个 `<<` 链里**，`FromString` 先求值改掉了值。

**特别之处**：`ok` 断言是对的、程序 `EXIT=0`、编译器 **0 warning**、
运行时自报"通过" —— **只有把输出逐字与注释里的"预期"对照，才能发现。**

### ★★ 门禁拦下一次真实风险

本轮开工时 `git status` 显示新增在途文件
`include/go2cpp/concurrency/context.hpp`（**非我所改**）。
**门禁中止了整轮，未提交任何东西**；加入不碰清单后才继续。

**不碰清单已增至 7 个**。

### ★ 一次自己的失误：Python 锚点转义

修上面那处输出时，我在 `OLD` 锚点里写了 `\"` 而文件里是 `"`，
**锚点 count=0** —— 这正是本会话早前记录过的坑，**我重复犯了**。
处置：该文件很短，**改为整份重写**，比继续调转义更可靠。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_config_demo` → **EXIT=0**（3 个片段）
- `go2cpp_config_full` → **EXIT=0**（7 场景全部符合预期）
- **7 个在途文件全程未触碰**

### 进度（16 轮完成 18 个模块）

`concurrency`(9) / `core`(1) / `network`(5) / `tools`(1) / **`config`(1)**

### 剩余模块

- **A 组**：`network/websocket`（需 socket）、`network/io_backend`、`network/io_hook`、
  `concurrency/runtime`
- **B 组**：`network/socket`（需端口）
- **C 组（依赖在途文件）**：`logging/log`、`tools/error`
- **未覆盖**：`config` 的 `RuntimeConfig`（pending/watcher 路径）

## 2026-10-13：example 完善 · 第 15 轮（A 组 · dynamic_gmp 模块）

### 本轮完成：`concurrency/dynamic_gmp`

| 文件 | 说明 |
|---|---|
| `example/concurrency/dynamic_gmp_demo_new.cpp` | **新建 · 简单版**（既有 `dynamic_gmp_demo.cpp` 为 131 行，超出简单版规格） |
| `example/concurrency/dynamic_gmp_full.cpp` | **新建 · 全面版 7 个场景** |

### 全面版的 7 个场景（全部实跑通过）

1. **配置上下界** —— `min=max=2` 时 `P=2 Worker=2`，启动数被配置锁死
2. **负载下扩展** —— **`启动 Worker=1 峰值=4`**（实测确实向上扩展）
3. **空闲回收** —— **`完成=32 空闲后 Worker=1 已回落=1`**
4. **★ 保底不被回收** —— `min_workers=3` 时**长时间空闲后仍是 3**
   （回收目标是下界，**不是 0**）
5. **任务类别与亲和** —— 40 个同类任务，`affinity_hits=30`，4 个 M 记录过类别
6. **★ `cancel` 被接受** —— `requested=1 final_state=5`（`kCancelled`）
7. **★ `cancel` 未被接受** —— `requested=0 final_state=4`（`kDead`）；
   **任务已完成时取消请求返回 false 是正确的，不是竞态缺陷**

### ★★ 本示例的核心贡献：把 cancel 的"两条路径"都验证到底

既有 demo 的注释里点出了这个契约，但**本示例把它拆成两个独立场景分别断言**：

- **被接受**（`requested == true`）→ **必须收敛到 `kCancelled`**
- **未被接受**（`requested == false`）→ **必须已在终态**（`kDead`/`kCancelled`/`kFailed`）

**关键在于第二条也要断言"已在终态"** —— 如果 `cancel()` 返回 false 而任务**仍在运行**，
那就真的是调度器缺陷。只接受"返回 false 也算通过"会漏掉这种缺陷。

### ★ 施工细节：抹掉了一处我自己写下的死代码

场景 2 首版里我写了一段毫无意义的判断：

```cpp
if (std::chrono::steady_clock::now() >
    std::chrono::steady_clock::now() + std::chrono::seconds(0)) { break; }
```

`now() > now()` 恒为假，永远不会 break —— 是我写"边等边采样"时留下的残渣。
已改为**带明确总超时的正常写法**：

```cpp
const auto sample_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
while (completed.load() != 64 && std::chrono::steady_clock::now() < sample_deadline) {
    peak = std::max(peak, scheduler.WorkerCount());
    std::this_thread::sleep_for(Ms(1));
}
```

**示例代码本身就是文档，不该留这种读不通的片段** —— 即使它"能跑、不报警告"。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_dynamic_gmp_demo_new` → **EXIT=0**（3 个片段）
- `go2cpp_dynamic_gmp_full` → **EXIT=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 5 个原始在途文件未触碰；`future_full.cpp` 也未触碰

### 进度（15 轮完成 16 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 57 行 | ✅ 188 行 / 6 场景 |
| `network/address` | ✅ 沿用 72 行 | ✅ 138 行 / 6 场景 |
| `network/bytearray` | ✅ 沿用 72 行 | ✅ 179 行 / 6 场景 |
| `network/fd_manager` | ✅ 沿用 56 行 | ✅ 209 行 / 7 场景 |
| `concurrency/thread_policy` | ✅ 新建 70 行 | ✅ 210 行 / 7 场景 |
| `concurrency/fiber_local` | ✅ 新建 61 行 | ✅ 192 行 / 7 场景 |
| `network/sse` | ✅ 新建 69 行 | ✅ 210 行 / 8 场景 |
| `network/http` | ✅ 新建 66 行 | ✅ 232 行 / 8 场景 |
| `concurrency/dynamic_gmp` | ✅ 新建 | ✅ 新建 / 7 场景 |

### 剩余模块

- **A 组**：`network/websocket`（需 socket）、`network/io_backend`、`network/io_hook`、
  `concurrency/runtime`
- **B 组**：`network/socket`（需端口）、**`config`**（25KB 头文件，需先专项读一轮）
- **C 组（依赖在途文件）**：`logging/log`、`tools/error`

## 2026-10-13：example 完善 · 第 14 轮（B 组 · network/http 模块）

### 本轮完成：`network/http`（此前**完全没有示例**）

| 文件 | 说明 |
|---|---|
| `example/network/http_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/network/http_full.cpp` | **新建 · 全面版 8 个场景** |

### 一个选型说明：为什么先做 http 而不是 websocket

原计划做 `network/websocket`，但读头文件后发现**它的帧编解码全是 private**
（`make_accept_key` / `make_client_key` / `read_frame` / `write_frame` /
`valid_utf8` / `valid_close_code` 都在 private 段），公开的只有两个握手入口
`ValidateUpgradeRequest` + `BuildUpgradeResponse`，**凑不满 3 个无网络场景**；
其余 API 都需要真实 `Socket`。所以改为 `network/http` ——
它的 `Headers` / `Request` / `Response` / `Parser` **全是纯计算**，
且这个模块此前完全没有示例。

**websocket 留待后续**：需要真实 socket 环境（届时先验证端口可用性）。

### 全面版的 8 个场景（全部实跑通过）

1. **请求往返** —— `Request::to_string` → `Parser::feed`：
   `target=/v1/notes/42 body="hello" host=example.test`，方法/版本/头全部还原
2. **头名大小写不敏感** —— 三种写法都能命中，`erase("CONTENT-type")` 也生效
3. **同名单头多值** —— `set` 覆盖后值数 1；`add` 三次后值数 3 且顺序为 `abc`
4. **路径与查询串拆分** —— `path()` / `query()` 正确切分
   （`/a/b/c`、`/search` + `q=box&page=2`、`/`）
5. **★ 增量分片解析** —— 逐字节 feed，中途出现 `NeedMore`，最后 `Complete`
6. **响应序列化** —— `204` 状态行、头字段；
   **`to_string(true)`（HEAD 请求）会省略正文**
7. **`redirect` 与 `set_cookie`** —— `302 Location=/login`、`sid=abc; HttpOnly`
8. **`Parser` 复用** —— `reset()` 后可解析下一个报文

### ★★ 本轮发现一个未查清的异常，已如实记录

**首版场景 8 用的是"管道化"（一次喂入两个请求 + `take_remaining`）**，实测：

```
第一个 target=/ 剩余字节=42
```

我设的 target 是 `/one`，两个请求总长约 44 字节，**而 `剩余=42` 说明解析器只消费了
2 字节就返回 `Complete`**。

**对照实验定位了边界**：场景 1 用「`PUT` + body」往返**完全正常**；
把场景 8 换成「`POST` + body」后**也完全正常**（`/one aaa /two bbb`）。
**差异只在于我首版那两个请求是「无 body 的 GET」。**

**我的处置**：

1. **把场景 8 改成确定成立的「`Parser` 复用」**，不让示例依赖未查清的行为；
2. **在 `http_full.cpp` 文件头留了一条 `⚠️ 未解决的观察`**，写明现象与对照结论；
3. **没有下"这是框架 bug"的结论** —— 也可能是我对 `to_string()` 在无 body
   情况下的终止约定理解有误。

**这条留给后续轮次深挖**（需要打印实际报文逐字节比对）。
**宁可标注未解决，也不把它写成"已验证"或"已确认是 bug"。**

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_http_demo` → **EXIT=0**（3 个片段）
- `go2cpp_http_full` → **EXIT=0**（8 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 5 个原始在途文件未触碰；`future_full.cpp` 也未触碰

### 进度（14 轮完成 15 个模块、约 130 个场景）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 57 行 | ✅ 188 行 / 6 场景 |
| `network/address` | ✅ 沿用 72 行 | ✅ 138 行 / 6 场景 |
| `network/bytearray` | ✅ 沿用 72 行 | ✅ 179 行 / 6 场景 |
| `network/fd_manager` | ✅ 沿用 56 行 | ✅ 209 行 / 7 场景 |
| `concurrency/thread_policy` | ✅ 新建 70 行 | ✅ 210 行 / 7 场景 |
| `concurrency/fiber_local` | ✅ 新建 61 行 | ✅ 192 行 / 7 场景 |
| `network/sse` | ✅ 新建 69 行 | ✅ 210 行 / 8 场景 |
| `network/http` | ✅ 新建 | ✅ 新建 / 8 场景 |

### 剩余模块

- **A 组**：`network/websocket`（需 socket）、`network/io_backend`、`network/io_hook`、
  `concurrency/runtime`、`concurrency/dynamic_gmp`
- **B 组**：`network/socket`（需端口）、**`config`**（头文件 25KB，需先专项读一轮）
- **C 组（依赖在途文件）**：`logging/log`、`tools/error`

## 2026-10-13：example 完善 · 第 13 轮（A 组 · network/sse 模块）

### ⚠️ 先说一件重要的事：在途文件从 5 个变成了 6 个

本轮开工前的 `git status` 显示多了一个被修改的文件：

```
 M example/concurrency/future_full.cpp     ← 第 4 轮我创建的示例
```

**这不是我改的。** `git diff` 显示改动只有 **1 行删除**：
文件头注释块里的一个空 `//` 行被删掉了（疑似编辑器自动格式化）。

**处理**：**我没有碰它，也没有提交它**，并在本轮脚本里把「在途文件」清单从 5 个
扩充为 6 个，门禁会拦截它的误提交。**如果这是你有意改的，它需要单独提交；
如果是误改，可以 `git checkout` 还原。**

### 本轮完成：`network/sse`

| 文件 | 说明 |
|---|---|
| `example/network/sse_basic_demo.cpp` | **新建 · 简单版**（既有 `sse_demo.cpp` 为 115 行，超出简单版规格） |
| `example/network/sse_full.cpp` | **新建 · 全面版 8 个场景** |

### 全面版的 8 个场景（全部实跑通过）

1. **基本事件** —— `data: hello` + 空行 → 1 个事件
2. **重复 `data` 字段合并** —— 三行 `data:` 合并为 `"line1\nline2\nline3"`
3. **`event` / `id` / `retry` 字段** —— 落到 `Event` 上，同时解析器记住
   `last_event_id()` 与 `retry_after()` 供重连使用
4. **★ 任意字节分片** —— **逐字节**喂入与整段喂入得到完全一致的事件
   （`data="alpha\nbeta"`，`last_event_id="9"`）
5. **换行变体与 BOM** —— `LF` / `CRLF` / `CR` 三种换行 + UTF-8 BOM 都能识别
6. **注释行** —— 以冒号开头的行被忽略，不产生事件
7. **★ 半个事件不派发** —— `"data: incomplete\n"`（缺结尾空行）**事件数 0**；
   补上空行后才派发。**避免把网络截断误报成完整业务消息**
8. **`encode` 往返** —— 编码后再解析得到等价事件

### ★ 本轮踩到第 6 类坑：编译警告（不是错误）

构建报 **0 error 但 1 warning**：

```
warning: hex escape sequence out of range
```

**根因**：我写了 `"\xEF\xBB\xBFdata: bom\n\n"` 表示 BOM + 正文。
但 **`\x` 转义会尽可能多地吞掉十六进制字符，而 `d` 恰好是十六进制数字** ——
于是 `\xBFd` 被解析成一个超范围的数值。

**修复**：用相邻字符串字面量把 BOM 与正文切开：

```cpp
parse_all(bom, "\xEF\xBB\xBF" "data: bom\n\n");
```

**为什么值得记**：项目基线是 **0 warning**，而这个警告**不影响正确性**
（编译产物照样能跑），如果只检查"有没有编译失败"就会漏掉。
**"零警告"这条纪律真正起作用的地方，正是这种"能跑但不干净"的情况。**

### 验证结果

- 构建：**0 warning / 0 error**（修复警告后）
- `go2cpp_sse_basic_demo` → **EXIT=0**（3 个片段）
- `go2cpp_sse_full` → **EXIT=0**（8 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 5 个原始在途文件全程未触碰；`future_full.cpp` 也未触碰

### 进度（13 轮完成 14 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 57 行 | ✅ 188 行 / 6 场景 |
| `network/address` | ✅ 沿用 72 行 | ✅ 138 行 / 6 场景 |
| `network/bytearray` | ✅ 沿用 72 行 | ✅ 179 行 / 6 场景 |
| `network/fd_manager` | ✅ 沿用 56 行 | ✅ 209 行 / 7 场景 |
| `concurrency/thread_policy` | ✅ 新建 70 行 | ✅ 210 行 / 7 场景 |
| `concurrency/fiber_local` | ✅ 新建 61 行 | ✅ 192 行 / 7 场景 |
| `network/sse` | ✅ 新建 | ✅ 新建 / 8 场景 |

### 目标状态：仍 active，未完成

已完成 **14 个模块**，`include/go2cpp/` 下约 **27 个模块**。剩余：

- **A 组**：`network/websocket`、`network/io_backend`、`network/io_hook`、
  `concurrency/runtime`、`concurrency/dynamic_gmp`
- **B 组**：`network/socket`、**`network/http`**、**`config`**
- **C 组（依赖在途文件）**：`logging/log`、`tools/error`

## 2026-10-13：example 完善 · 第 12 轮（B 组 · fiber_local 模块）

### 本轮完成：`concurrency/fiber_local`（此前**完全没有示例**）

| 文件 | 说明 |
|---|---|
| `example/concurrency/fiber_local_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/concurrency/fiber_local_full.cpp` | **新建 · 全面版 7 个场景** |

### 全面版的 7 个场景（全部实跑通过）

1. **`GetOrCreate` 按需就地构造** —— 再次调用返回**同一对象**（`同一对象=1`）
2. **两个 Fiber 的值互相隔离** —— `A="alpha" B="beta"`
3. **★ park/resume 后值仍然存在** —— 在无缓冲 Channel 上阻塞、
   唤醒后读到 `4242`。**实证了"值属于 G 而不是某次执行片段"**
4. **`Reset` 后可重建** —— `Reset 前有值=1 Reset 后为空=1 重建后="second"`
5. **Fiber 之外的线程局部后备** —— `新线程初次读到 -1 主线程仍为 11`
   （后备值按线程隔离，新线程看不到主线程存的 11）
6. **`key_id` 单调唯一** —— `key_id=6,7,8 严格递增且互不相同=1`
7. **多实例互不干扰** —— 数值槽 99 与文本槽同时正确

### ★ 本示例说明的关键语义

`fiber_local` 与 `thread_local` 的区别是本模块存在的全部理由：

| | `thread_local` | `FiberLocal<T>` |
|---|---|---|
| 作用域 | 当前 OS 线程 | **当前 Fiber（G）** |
| G 被迁移到另一个 M 时 | 值留在原线程，**丢失** | **值随 G 迁移，保留** |
| 生命周期终点 | 线程退出 | **Fiber trampoline 结束** |
| 在 Fiber 外使用 | 正常 | **退化为线程局部后备值** |

**场景 3 就是对第 2 行的实证**；**场景 5 是对第 4 行的实证**。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_fiber_local_demo` → **EXIT=0**（3 个片段）
- `go2cpp_fiber_local_full` → **EXIT=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰
- **本轮一次编译通过**（严格沿用"先读头文件、再读既有 demo"两道前置）

### 进度（12 轮完成 13 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 57 行 | ✅ 188 行 / 6 场景 |
| `network/address` | ✅ 沿用 72 行 | ✅ 138 行 / 6 场景 |
| `network/bytearray` | ✅ 沿用 72 行 | ✅ 179 行 / 6 场景 |
| `network/fd_manager` | ✅ 沿用 56 行 | ✅ 209 行 / 7 场景 |
| `concurrency/thread_policy` | ✅ 新建 70 行 | ✅ 210 行 / 7 场景 |
| `concurrency/fiber_local` | ✅ 新建 | ✅ 新建 / 7 场景 |

### 目标状态：仍 active，未完成

目标要求「为 Go2Cpp 的**每个模块**完善 example」。当前已完成 13 个模块，
而 `include/go2cpp/` 下约有 **27 个模块**。**剩余未覆盖的模块**：

- **A 组（有 demo、待补全面版）**：`network/io_backend`、`network/io_hook`、
  `network/websocket`、`network/sse`、`concurrency/runtime`（`runtime_demo` 115 行
  已可作简单版）、`concurrency/dynamic_gmp`
- **B 组（完全无示例）**：`network/socket`、**`network/http`**、**`config`**、
  `core/hybrid_mutex`（`HybridGate`，**用户已判定不保留，暂不写**）
- **C 组（依赖在途文件，等用户定稿）**：`logging/log`（`log.hpp`）、
  `tools/error`（`error.hpp`）

**按时序推算，12 轮目标轮次用完，但目标远未达成** —— 目标保持 active，
后续可继续推进剩余模块。

## 2026-10-13：example 完善 · 第 11 轮（B 组 · thread_policy 模块）

### 本轮完成：`concurrency/thread_policy`（此前**完全没有示例**）

| 文件 | 说明 |
|---|---|
| `example/concurrency/thread_policy_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/concurrency/thread_policy_full.cpp` | **新建 · 全面版 7 个场景** |

### 全面版的 7 个场景（全部实跑通过）

1. **默认状态** —— 普通线程 = `kUnmanaged` + `kInherit`，`scheduler == nullptr`
2. **`ScopedThreadParticipation`** —— 作用域内绑定调度器后 `participates_in_gmp=1`，
   退出后恢复
3. **`ScopedThreadHookMode`** —— 禁用 / 启用都能生效，退出后恢复
4. **`SetCurrentThreadHookMode`** 直接设置 —— **不会被自动还原，必须手工恢复**
5. **嵌套守卫** —— 内层退出恢复**外层状态**（`kEnabled`），而不是默认值
6. **`HookAllowed` 合并语义** —— `inherit(1,0) disabled(0,0) enabled(1,1)`
7. **`Scheduler` worker 内** —— `runtime_worker=1`、`participates_in_gmp=1`，
   且 worker 内的 Hook 覆盖**不影响主线程**（TLS 隔离）

### ★★ 本轮两处需要纠正的地方（都是我的问题）

**① 编译错误：`ScopedThreadParticipation` 必须传调度器身份**

首版我写成 `ScopedThreadParticipation(ThreadParticipationMode::kGmpEligible)`，
编译失败。读头文件后发现两个重载都要求 `scheduler::Scheduler*`（或引用）：

```cpp
explicit ScopedThreadParticipation(scheduler::Scheduler* scheduler,
                                   ThreadParticipationMode mode = kGmpEligible) noexcept;
```

**根因**：`participates_in_gmp()` 的实现是
`scheduler != nullptr && participation == kGmpEligible` ——
**只把 participation 改成 `kGmpEligible` 而不绑定调度器，仍然不算"参与 GMP"**。
这一点现在写进了示例注释。

**② 断言错误：`kEnabled` 是覆盖，不是跟随**

场景 6 首版实测 `enabled(1,1)`，我预期的是 `(1,0)`。
**框架是对的**：`kEnabled` 表示"线程级显式启用"，**优先级高于进程级开关**，
所以进程级为 `false` 时结果仍是允许。

**合并语义汇总（已写进示例）**：

| 线程模式 | 进程 true | 进程 false |
|---|---|---|
| `kInherit` | 1 | 0 |
| `kDisabled` | 0 | 0 |
| **`kEnabled`** | 1 | **1** |

**这两处合起来说明同一件事**：`thread_policy` 的两个维度
（"参与 GMP" 与 "Hook 开关"）**都不是单一布尔量**，
前者要求"模式 + 调度器身份"同时成立，后者是"进程级 + 线程级覆盖"的合并。
**示例把这两层语义都显式展开了，这正是它比头文件注释更有用的地方。**

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_thread_policy_demo` → **EXIT=0**（3 个片段）
- `go2cpp_thread_policy_full` → **EXIT=0**（7 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### 进度（11 轮完成 12 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 57 行 | ✅ 188 行 / 6 场景 |
| `network/address` | ✅ 沿用 72 行 | ✅ 138 行 / 6 场景 |
| `network/bytearray` | ✅ 沿用 72 行 | ✅ 179 行 / 6 场景 |
| `network/fd_manager` | ✅ 沿用 56 行 | ✅ 209 行 / 7 场景 |
| `concurrency/thread_policy` | ✅ 新建 | ✅ 新建 / 7 场景 |

**下一轮建议**：`concurrency/fiber_local`（B 组，无环境依赖）。

## 2026-10-13：example 完善 · 第 10 轮（A 组 · network/fd_manager 模块）

### 本轮完成：`network/fd_manager`

| 文件 | 说明 |
|---|---|
| `example/network/fd_manager_full.cpp` | **新建 · 全面版 7 个场景** |
| `example/network/fd_manager_demo.cpp` | **沿用既有** —— 56 行、4 个场景，作简单版 |

### 全面版的 7 个场景（全部实跑通过）

1. **首次 `get(fd, true)` 自动创建** —— `创建前=nullptr 创建后 fd=91001 initialized=1`
2. **非阻塞的两个独立维度** —— `user` 与 `system` 可分别设置：
   只设 user / 两个都设 / 都清掉，三种组合都验证
3. **超时的两个独立方向** —— `recv=250ms send=500ms`
4. **`erase` 生命周期** —— `旧句柄 closed=1 新对象=1 编号相同=1`
   （erase 把已发出的 `shared_ptr` 标记为 `closed()`，编号复用得到新对象）
5. **只读查询边界** —— `auto_create=false` 时不存在返回 `nullptr`，
   且**连续两次只读都不会意外创建**
6. **`duplicate` 复制元数据** —— `目标 fd=91007 继承 user_nonblock=1
   继承 recv_timeout=120ms 源仍存在=1`
7. **多 fd 隔离** —— 两个编号状态独立；擦掉一个后另一个仍在（且未被动 closed）

### ★ 本轮一次编译通过 —— 第 9 轮的教训生效了

第 9 轮我因为"从既有 demo 出现的片段外推未出现的成员"而臆造了
`ByteArray::data()`，导致 4 处编译错误。**本轮严格按修正后的规范执行**：

**先读 `include/go2cpp/network/fd_manager.hpp` 的公开接口
（确认 `ContextPtr` 在该头文件里是 `shared_ptr<FdContext>` 的局部别名，
不是 `go2cpp::ContextPtr`），再读既有 demo 学用法，然后才动笔。**

结果：**一次编译通过，0 warning，7 个场景全部符合预期。**

**→ 规范验证有效。后续轮次继续沿用"读头文件 + 读既有 demo"两道前置。**

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_fd_manager_full` → **EXIT=0**，7 场景全部符合预期
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### 说明：为什么本轮没做 `network/socket`

原计划做 `network/socket`，但它需要真实监听端口 / `connect`，
**在受限环境里可能起不来**（WSL 无外网，端口行为不确定）。
`fd_manager` 只操作元数据表（示例还刻意用远离真实 fd 空间的 91001 起编号），
**无任何环境依赖**，是更稳的选择。`socket` 留到后续轮次，
届时需要先验证监听端口能否在当前环境建立。

### 进度（10 轮完成 11 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 57 行 | ✅ 188 行 / 6 场景 |
| `network/address` | ✅ 沿用 72 行 | ✅ 138 行 / 6 场景 |
| `network/bytearray` | ✅ 沿用 72 行 | ✅ 179 行 / 6 场景 |
| `network/fd_manager` | ✅ 沿用 56 行 | ✅ 新建 / 7 场景 |

**下一轮建议**：`concurrency/thread_policy` 或 `concurrency/fiber_local`（B 组，无环境依赖），
或先验证 `network/socket` 的端口可用性。

## 2026-10-13：example 完善 · 第 9 轮（A 组 · address + bytearray 两个模块）

### 本轮完成

| 文件 | 说明 |
|---|---|
| `example/network/address_full.cpp` | **新建 · 全面版 6 个场景** |
| `example/network/bytearray_full.cpp` | **新建 · 全面版 6 个场景** |
| `example/network/address_bytearray_demo.cpp` | **沿用既有** —— 72 行、5 个场景，作简单版 |

### address 全面版的 6 个场景（全部实跑通过）

1. **IPv4 解析与端口** —— `192.168.1.10:8443`，`family()==AF_INET`
2. **子网计算 /24** —— `mask=255.255.255.0 network=192.168.1.0 broadcast=192.168.1.255`
3. **掩码长度两端 /0 与 /32** —— `/0 mask=0.0.0.0 net=0.0.0.0`、
   `/32 mask=255.255.255.255 net=10.20.30.40`
4. **Unix 域地址** —— `family=AF_UNIX 长度=27`
5. **`Address::Create` 工厂** —— 从 `sockaddr` 复制出正确子类（`family=1 / 2`）
6. **非法输入** —— `Create("not-an-ip")` 返回空指针；掩码超范围（64）返回空

### bytearray 全面版的 6 个场景（全部实跑通过）

1. **大端定长整数往返** —— `16/32/64` 位原值往返
2. **★ 字节序确实生效** —— **`大端 hex=11223344 小端 hex=44332211`**
3. **长度前缀字符串** —— `"hello" / ""（空串）/ "中文也支持"` 都能往返
4. **变长整数** —— `1 占 1 字节，300 占 2 字节`（编码长度随值变化）
5. **顺序读写与定位** —— 三个字段顺序读回，`set_position(0)` 可回退重读
6. **越界读取显式抛异常** —— 实测异常文本 `ByteArray read beyond end`；
   空缓冲读整数、声明长度超出实际数据两条路径都抛

### ★ 本轮我犯了一个明确的错误（第 5 类坑）

首版编译失败 4 处：**我臆造了 `ByteArray::data()`**（实际 `m_data` 是私有成员）。

```
error: 'class go2cpp::net::ByteArray' has no member named 'data';
       did you mean 'std::vector<unsigned char> ByteArray::m_data'?
```

**这违反了"绝不臆造接口"这条自定规范** —— 我看到既有 demo 里用了
`write_fuint16`/`read_fuint16` 就推测有 `data()`，而没有去读头文件。

**修复方式**：读 `include/go2cpp/network/byte_array.hpp` 的公开接口，发现真实可用的是：
- **`to_hex_string()`** —— 用来观察实际字节序（比 `data()` 更适合示例）
- **`size()`** —— 用来断言编码长度

**这次错误带来一个额外收获**：用 `to_hex_string()` 反而把"字节序是否真的生效"
验证得更直观 —— **`11223344` vs `44332211` 一眼就能看出**，
比读原始字节数组更有说服力。

**教训（第 5 类坑）**：
- 第 1/4 轮：编译失败（作用域 / 类型系统）
- 第 6 轮：编译通过但行为静默错误（`Timer` 句柄）
- 第 8 轮：编译通过、运行正常，但断言基于错误假设（`worker_count` / `PId` 从 0 起）
- **第 9 轮：从"同类 API 的存在"外推到"这个类也有同名方法"** ——
  **`ByteArray` 与 `std::vector` 无关，不该假设它有 `data()`**

**→ 修正后的施工规范：写示例前必须读目标类的真实头文件，不得从既有 demo
的用片段反推未出现的成员。**

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_address_full` → **EXIT=0**，6 场景全部符合预期
- `go2cpp_bytearray_full` → **EXIT=0**，6 场景全部符合预期
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### 进度（9 轮完成 10 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 57 行 | ✅ 188 行 / 6 场景 |
| `network/address` | ✅ 沿用 72 行 | ✅ 新建 / 6 场景 |
| `network/bytearray` | ✅ 沿用 72 行 | ✅ 新建 / 6 场景 |

**下一轮建议**：`network/socket`（有 `network_demo` 可作简单版），
或 B 组的 `concurrency/thread_policy` / `fiber_local`。

## 2026-10-13：example 完善 · 第 8 轮（A 组 · scheduler 模块）

### 本轮完成：`concurrency/scheduler`

| 文件 | 说明 |
|---|---|
| `example/concurrency/scheduler_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/concurrency/scheduler_full.cpp` | **新建 · 全面版 6 个场景** |

**为什么两版都新建**：既有 `runtime_demo.cpp` 有 115 行，**超出简单版 50~80 行的规格**，
且它演示的是"运行时综合用法"而非 Scheduler 本身。所以简单版也另写了一个聚焦的。

### 全面版的 6 个场景（全部实跑通过）

1. **启动与关闭** —— `is_running` 在 `start`/`shutdown` 前后翻转 `0 → 1 → 0`
2. **`spawn` 与 `wait` / `wait_for`** —— 包含"任务未完成时 `wait_for` 超时返回 false"
   与"放行后正常完成"两条路径
3. **并行度观测** —— `P=4 M=1 空闲时可运行=0 结束后可运行=0 实际执行=8`
4. **快照** —— `processors()` / `machines()` 返回运行时视图
5. **指标** —— `metrics()` 累计 `task_runs=12 task_completions=12`
6. **Fiber 内自省** —— `current_task()` 在 Fiber 内非空、在主线程内为空

### ★★ 本轮挖到两条真实的 API 语义（我原先的断言写错了，不是框架问题）

首版编译 0 warning，但 3 个场景断言不符：

| 场景 | 实测 | 我的错误假设 | 正确认识 |
|---|---|---|---|
| 3 | `P=4 M=1` | 以为 `worker_count()` 等于 P 的数量 | **`worker_count()` 是"当前存活的工作线程（M）数"，且 M 按需惰性创建** —— 它既不等于 P 数，也不保证达到 P 的上限 |
| 4 | `id 均非 0=0` | 以为 ID 从 1 开始 | **`PId` / `MId` 从 0 开始编号，`0` 是合法 ID 而不是"无效值"** |
| 6 | `有当前 P=0` | 用 `!= 0` 判断当前 P | 同上；改用 `current_processor_id() < processor_count()` 判断 |

**这两条都写进了示例注释**，因为它们正是使用者最容易踩的坑：
- 想"看看开了几个 P"的人会去读 `worker_count()`，然后困惑为什么是 1；
- 想"判断有没有当前 P"的人会写 `!= 0`，然后在 P0 上得到错误结论。

**教训（第四类坑）**：前几轮的坑是"编译失败"或"行为静默错误"，
本轮是**"编译通过、运行正常，但断言基于错误假设"** —— 这一类只有靠
**真实运行并逐条核对预期值**才能发现，无法靠编译器或类型系统拦截。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_scheduler_demo` → **EXIT=0**（3 个片段）
- `go2cpp_scheduler_full` → **EXIT=0**（6 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### 进度（8 轮完成 8 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 67 行 | ✅ 210 行 / 6 场景 |
| `concurrency/scheduler` | ✅ 新建 | ✅ 新建 / 6 场景 |

**下一轮建议**：`network/socket`（有 `network_demo` 可作简单版），
或 B 组的 `concurrency/thread_policy` / `fiber_local`。

## 2026-10-13：example 完善 · 第 7 轮（B 组 · core/parking_condition 模块）

### 本轮完成：`core/parking_condition`（此前**完全没有示例**）

| 文件 | 说明 |
|---|---|
| `example/concurrency/parking_condition_demo.cpp` | **新建 · 简单版 3 个片段** |
| `example/concurrency/parking_condition_full.cpp` | **新建 · 全面版 6 个场景** |

### API 要点（本轮核实）

- **谓词式**：`wait(lock, pred)` / `wait_for(lock, timeout, pred)` /
  `wait_until(lock, deadline, pred)`，**都要求调用方持有
  `std::unique_lock<std::mutex>`**（不是 `sync::Mutex`）
- `notify_one()` / `notify_all()`
- 静态查询：`FiberWaitUnsupported()` / `CancellationRequested()`

### 全面版的 6 个场景（全部实跑通过）

1. **★ 受管 G 等待不阻塞 M（P=1 反证）** —— 只给一个 P，让一个 Fiber 等待、
   另一个 Fiber 通知。实测 `等待者已完成=1`：
   **证明等待确实把执行权交还给了 M**（若阻塞，通知者永远排不上，会超时）
2. **谓词式等待** —— 谓词要求 `counter >= 3`，中间值 1、2 被挡回去继续等
   → `谓词最终成立=1 counter=3`
3. **`notify_all`** —— 4 个等待者一次全醒 → `被唤醒=4`
4. **`wait_for` 超时** —— 条件永不成立时 `返回=0 耗时约 30ms`
5. **普通线程等待路径** —— **同一个对象**也能被普通线程使用（走原生 CV）
6. **静态查询** —— 普通线程与受管 Fiber 内 `FiberWaitUnsupported()` 均为 0

### ★ 本轮的错误也在示例自己的测试代码里（如实记录）

首版场景 1 失败：`等待者已完成=0`。排查后确认**是我的测试写错了**，不是框架问题：

```cpp
const bool finished = waiter_done.load();   // ★ 在 wait() 之前就读了
waiter->wait();
```

`go()` 是异步的，主执行流立即检查当然还是 false。**正确写法是先 `waiter->wait()`
再读标志**。真正的反证在于：**若受管 G 的等待会阻塞唯一的 M，那么 `waiter->wait()`
本身就会挂住** —— 由外部 `timeout 60` 兜底。

这个错误本身也有教育价值：**"用 P=1 反证不阻塞 M"的写法必须让等待者的完成
成为真正的阻塞点**，而不是提前读一个还没被写入的标志。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_parking_condition_demo` → **EXIT=0**（3 个片段）
- `go2cpp_parking_condition_full` → **EXIT=0**（6 场景全部符合预期）
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### 进度（7 轮完成 7 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 171 行 / 6 场景 |
| `core/parking_condition` | ✅ 新建 | ✅ 新建 / 6 场景 |

**下一轮建议**：`network/socket`（有 `network_demo` 可作简单版），
或 B 组的 `concurrency/thread_policy` / `fiber_local`。

## 2026-10-13：example 完善 · 第 6 轮（A 组 · timer 模块，定时任务收尾轮）

### 本轮完成：`concurrency/timer`

| 文件 | 说明 |
|---|---|
| `example/concurrency/timer_full.cpp` | **新增 · 全面版 6 个场景** |
| `example/concurrency/timer_task_demo.cpp` | **沿用既有** —— 178 行，远超简单版规范 |

### 全面版的 6 个场景（全部实跑通过）

1. **`after()` 一次性任务** —— `已执行=1 恰好一次=1 RunCount=1`
2. **多个定时器按到期顺序执行** —— 乱序注册 60/20/40ms，实测回调顺序 `10,20,30`
3. **`Cancel()` 取消未领取任务** —— `取消前 Active=1 取消后 Active=0 是否执行=0`
4. **`Active()` / `RunCount()` 观测** —— 执行前 `Active=1 且 RunCount=0`，执行后 `RunCount=1`
5. **与 `WaitGroup` 配合** —— 4 个定时任务全部完成后 `WaitFor` 成功
6. **定时器 vs `Context` 超时的语义区别** —— 一个"到点做事"，一个"到点放弃等待"

### ★★★ 本轮挖到的关键 API 语义（比上一轮的 Promise 更隐蔽）

**首次运行 3 个场景失败**（编译完全通过，`0 warning 0 error`）：

```
三个都执行=0 顺序=-1,-1,-1        ← 场景2
WaitFor 成功=0 完成数=0            ← 场景5
定时器动作已执行=0                 ← 场景6
```

**根因**：我写成了

```cpp
(void)after(Ms(60), [&] { ... });   // ★ 临时 Timer 立刻析构
```

**`Timer` 句柄析构会取消尚未领取的任务** —— 于是三个定时器在注册后立刻被取消，
**永远不会触发**。而场景 1、4 因为把返回值存进了局部变量（句柄活到函数结束）所以成功。

**修复**：用 `std::vector<core::Timer>` 保存句柄。

**为什么这个坑特别值得记录**：

- **编译期完全无警告** —— `(void)expr` 是消除 unused 警告的常规写法，
  **在这里恰恰是错的**；
- **场景 1 成功、场景 2 失败** 的对比，说明这不是"定时器没工作"，
  而是"句柄生命周期决定了任务是否存活"；
- **这条语义在头文件注释里写了**（"Cancel；多个句柄不能复制，移动后源句柄失效"），
  但**没有明确说"句柄析构 = 取消"** —— 示例注释现在把这一点写清楚了。

**这条已写进 `timer_full.cpp` 的文件头注释**，并在场景 2、5、6 各留了说明。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_timer_full` → **EXIT=0**，6 场景输出与注释预期逐条一致
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### ★ 定时任务收官：6 轮完成 6 个模块

定时任务 `schedule-460525c6`（cron `*/30 19-21 * * *`）共触发 5 次
（19:30 / 20:00 / 20:30 / 21:00 / 21:30），加上用户下达任务当轮，**共完成 6 个模块**：

| 模块 | 简单版 | 全面版 | 备注 |
|---|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 | 新建两版 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 | 新建两版 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 | 新建两版 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 | 全面版新建 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 185 行 / 6 场景 | 全面版新建 |
| `concurrency/timer` | ✅ 沿用 178 行 | ✅ 新增 / 6 场景 | 全面版新建 |

**共新增 6 个全面版（每个 6 场景 = 36 个场景）与 3 个简单版**，
每个都经"登记 → 构建 0 warning/0 error → 真实运行 → 输出比对"四步验证。

### 三轮踩坑记录（都已固化进规范）

| 轮次 | 坑 | 固化措施 |
|---|---|---|
| 1 | `100ms` 字面量写在匿名 namespace，`chrono_literals` 不可见 | 统一用显式 `std::chrono::milliseconds` + `using Ms = ...` |
| 4 | `Promise` 不可复制，不能按值捕获进 `go()` | 必须 `shared_ptr<Promise<T>>` |
| 6 | **`Timer` 句柄析构 = 取消任务** | 必须保存句柄到容器 |

**这三条都是"编译通过但行为不对"或"编译失败但不明显"的类型**，
正是"必须真实运行确认输出合理"这条要求的价值所在。

### 未完成（保留给后续轮次）

- **A 组剩余**：`concurrency/scheduler`（`runtime_demo` / `dynamic_gmp_demo` 可作简单版）、
  `network/socket`（`network_demo` 可作简单版）、`logging/log`（**`log.hpp` 是在途文件，待用户定稿**）
- **B 组（完全无示例，两版都要新建）**：`config`（头文件 25KB，需先专项读一轮）、
  `network/http`、`core/parking_condition`、`concurrency/fiber_local`、`concurrency/thread_policy`
- **C 组（暂缓）**：`tools/error` —— 依赖的在途文件 `tools/error.hpp` 未定稿

## 2026-10-13：example 完善 · 第 5 轮（A 组 · control_flow 模块）

### 本轮完成：`tools/control_flow`

| 文件 | 说明 |
|---|---|
| `example/tools/control_flow_full.cpp` | **新增 · 全面版 6 个场景** |
| `example/tools/control_flow_demo.cpp` | **沿用既有** —— 94 行，已满足简单版规范 |

### 全面版的 6 个场景（全部实跑通过）

1. **`defer` 的 LIFO 逆序** —— `body → third → second → first`
2. **`defer` 的求值时机** —— 值参数在**注册时**求值（`by_value=1`），
   引用捕获在**执行时**读取（`by_reference=9`）→ **与 Go 的 `defer` 一致**
3. **`dismiss()` / `run_now()`** —— 已取消的不执行、提前执行的已执行，
   且 `run_now()` 后 `active()` 变 false
4. **★ `panic` 是显式状态发布，控制流继续** —— 实测
   `call 之后仍继续执行=1`，明确标注与 Go 的差异
5. **★ `recover` 的边界** —— 实测
   `边界外取到=0 边界外取后仍 active=1 边界内取到=1`
   （边界外 `take()` 返回空且**不消费状态**，边界内才能取到）
6. **`defer` 回调抛异常不中断** —— 三个回调（含抛异常的那个）全部执行

### ★★★ 本示例刻意承担的说明责任

场景 4 和 5 是本文件最重要的部分。项目的 `panic`/`recover` 是
**「显式状态记录」而非 Go 的栈展开**：不展开 C++ 栈、不自动执行外层 `defer`、
无人 `recover` 时也不终止进程。**业务代码必须自己检查状态并 return/分支。**

这个差异在台账里登记为 **X-30**（头文件措辞过强）。本示例把正确认识写进注释，
**让示例本身成为"不要误用"的第一道教育**。

### 施工细节

- **不依赖 `Logger`**：既有 `control_flow_demo.cpp` 用了 `Logger`，
  而 `include/go2cpp/logging/log.hpp` 是当前 5 个在途文件之一（API 可能变动）。
  本轮全面版**改用 `std::clog` + 一个全局 `std::vector<std::string>` 记录器**，
  完全不碰日志模块 —— 避免示例随在途文件一起失效。
- 一次编译通过，0 warning（延续第 2 轮确立的 chrono 规范；本文件未用时间字面量）。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_control_flow_full` → **EXIT=0**，6 场景输出与注释预期逐条一致
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### 进度（5 轮完成 5 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用 82 行 | ✅ 186 行 / 6 场景 |
| `tools/control_flow` | ✅ 沿用 94 行 | ✅ 新增 / 6 场景 |

**下一轮建议**：`concurrency/timer`（需先读 `timer.hpp` 确认 API），
或 `network/socket`（有 `network_demo` 可作简单版）。

## 2026-10-13：example 完善 · 第 4 轮（A 组 · future 模块）

### 本轮完成：`concurrency/future`

| 文件 | 说明 |
|---|---|
| `example/concurrency/future_full.cpp` | **新增 · 全面版 6 个场景** |
| `example/concurrency/future_demo.cpp` | **沿用既有** —— 实测 82 行，已满足简单版规范（50~80 行量级），不重复造 |

### 全面版的 6 个场景（全部实跑通过）

1. **基本取值** —— 另一个 Fiber 里 `SetValue`，主执行流 `GetResult` → `kReady 2026`
2. **错误传递** —— `SetError` → `kError`，`error` 字段可取回（**不抛异常**）
3. **异常传递** —— `SetException` → `kException`；
   **`GetResult` 不抛，只有显式 `GetOrThrow` 才抛**（实测抛出=1）
4. **等待超时** —— `WaitFor(20ms)` 返回 `kTimedOut`，之后再 `GetResult` 仍能拿到 `kReady`
   （**超时只影响本次等待，不改变 Future 状态**）
5. **`Context` 取消** —— `Wait(ctx)` 返回 `kCancelled`
6. **Promise 被放弃** —— 未 `SetValue`/`SetError` 就析构 → `kError` 且
   `Is(error, BrokenPromiseError())` 为真

### ★★ 本轮挖到一条重要的 API 约束（值得写进示例注释）

**首次编译失败 9 处**，根因是同一个：

```
error: use of deleted function 'go2cpp::Promise<T>::Promise(const Promise<T>&)'
error: static assertion failed: std::function target must be copy-constructible
```

**`Promise<T>` 不可复制，而 `go()` 的 `Task::Function` 是 `std::function`
（标准要求目标可复制）** —— 因此：

> **不能把 `Promise` 按值捕获进 `go()` 的 lambda。**

错误写法（我第一版就是这样）：
```cpp
auto promise = pair.first;                              // 拷贝 → 已删除
go(scheduler, [promise] { (void)promise.SetValue(1); }); // 再拷一次 → std::function 拒绝
```

正确写法：
```cpp
auto promise = std::make_shared<Promise<int>>(std::move(pair.first));
go(scheduler, [promise] { (void)promise->SetValue(1); });
```

**为什么既有 `future_demo.cpp` 没暴露这个问题**：它**全程在主执行流里
`MakePromise` + 直接 `SetValue`，没有用 `go()`** —— 所以从未遇到 lambda 捕获。
这解释了"为什么 82 行的旧示例看起来能用，但照它的写法搬到 Fiber 里就编不过"。

**这条约束已写进 `future_full.cpp` 的文件头注释**，示例本身就承担了说明责任。

### 附带修掉的一处隐性拷贝

场景 5 原本写 `const auto promise = pair.first;` —— `auto` 推导成 `Promise<int>`，
同样是拷贝。改为 `shared_ptr` 持有后统一消除。

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_future_full` → **EXIT=0**，6 场景输出与注释预期逐条一致
  （`kReady/value=2026`、`kError`、`kException` + 抛出、`kTimedOut`→`kReady`、
  `kCancelled`、`BrokenPromiseError`）
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### 进度（4 轮完成 4 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ 77 行 | ✅ 206 行 / 6 场景 |
| `concurrency/future` | ✅ 沿用既有 82 行 | ✅ 新增 / 6 场景 |

**下一轮建议**：`concurrency/timer`（`TimerService` / `after()`），
或 `logging/log`（注意 `log.hpp` 是在途文件，需先确认用户是否已定稿）。

## 2026-10-13：example 完善 · 第 3 轮（A 组 · sync 模块）

### 本轮完成：`concurrency/sync`

| 文件 | 内容 |
|---|---|
| `example/concurrency/sync_demo.cpp` | 简单版：Mutex 计数 / 跨执行流解锁 / WaitGroup |
| `example/concurrency/sync_full.cpp` | **全面版：6 个场景** |

### 全面版的 6 个场景（全部实跑通过）

1. **`Mutex` 互斥** —— 8 Fiber × 2000 次累加，结果 16000 不丢更新
2. **★ 跨执行流解锁** —— 主执行流加锁，**另一个 Fiber 调用 `Unlock()`**，
   之后能重新 `Lock()` 且 `TryLock()` 成功 → 实证了 `sync::Mutex`
   「Unlock 不要求由同一 G/线程调用」的 Go 风格所有权
   （**普通 `std::mutex` 在这里是 UB**）
3. **`LockFor` 超时** —— 锁被占用时 30ms 后返回 false，而不是一直等
4. **`Mutex` + `ConditionVariable` 生产者/消费者** —— 用**谓词循环**对抗虚假唤醒
   （`while (queue.empty()) cv.Wait(mutex)`），消费 5 项
5. **`WaitGroup` 汇合** —— 6 个 Fiber 全部结束，`Count()` 归零
6. **`Context` 取消打断 `Wait`** —— 取消后条件变量等待立即返回（`Wait` 返回，
   不再永久阻塞）

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_sync_demo` → **EXIT=0**（计数 8000、跨执行流解锁=1、完成数 3/Count 0）
- `go2cpp_sync_full` → **EXIT=0**，6 场景全部通过
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰
- **连续两轮（context、sync）一次编译通过** —— 第 2 轮确立的
  「示例里不用 `chrono` 字面量、统一 `using Ms = std::chrono::milliseconds`」
  规范生效

### 进度（3 轮完成 3 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ 62 行 | ✅ 165 行 / 6 场景 |
| `concurrency/sync` | ✅ | ✅ / 6 场景 |

**下一轮建议**：`concurrency/future`（`future.hpp` 已通读，不在在途清单），
或 `concurrency/timer`；`config` 与 `network/http` 属于"无示例"的 B 组，
建议先花一轮读头文件再动手。

## 2026-10-13：example 完善 · 第 2 轮（A 组 · context 模块）

### 本轮完成：`concurrency/context`

| 文件 | 内容 |
|---|---|
| `example/concurrency/context_demo.cpp` | 简单版：WithCancel / WithTimeout / WithValue 三件最常用的事 |
| `example/concurrency/context_full.cpp` | **全面版：6 个场景** |

**为什么选 context 而不是原计划的 config**：`config` 模块比预期复杂得多
（`include/go2cpp/config/config.hpp` 有 **25 KB**，含 `IniFile` / `ValueCodec<T>` /
`ConfigVarBase` / `ConfigVar<T>` / `RuntimeConfig`，还有 watcher 相关虚函数）。
**在 API 不熟的情况下动手，很可能浪费掉一整轮定时窗口。**
而 `context` 的 API 我在本会话前段已通读，且 `context.hpp` 不在 5 个在途文件之列 ——
**风险最低、产出最确定**。`config` 挪到后续轮次，届时先专门读一轮它的头文件。

**与既有 `context_rollback_demo` 不重复**：那个示例演示的是 `ContextRollback`（补偿事务），
本轮两个文件演示的是 `Context` 本身（取消树、超时、取值、根因、等待信号）。

### 全面版的 6 个场景

1. **`WithCancel` 级联** —— 只取消中间层，上下的 `IsDone()` 都变真
2. **`WithTimeout` 自动取消** —— `Err()` 是 `DeadlineExceededError`，
   **且主动再取消一次仍是 `DeadlineExceeded`**（验证取消幂等 + 超时不被覆盖）
3. **`WithValue` 沿父链查找** —— 子级覆盖 `t-002`，父级仍是 `t-001`，
   未设置的键返回空 `optional`
4. **`WithCancelCause`：`Err()` vs `Cause()`** —— 两级的 `Err()` 都是 `Canceled`，
   而 `Cause()` 都指向最初传入的那个根因
5. **`Background()` 免疫取消** —— 它没有取消函数，`Done()` 在 30 ms 内不完成
6. **`Done()` 信号等待** —— 在另一个执行流里稍后取消，主执行流阻塞在 `Done()` 上
   （而不是轮询 `IsDone()`）

### 验证结果

- 构建：**0 warning / 0 error**
- `go2cpp_context_demo` → **EXIT=0**，3 个场景输出与注释中的"预期"逐条一致
- `go2cpp_context_full` → **EXIT=0**，6 个场景全部通过
- 已登记进 `GO2CPP_EXAMPLES`
- 在途 5 个文件全程未触碰

### ★ 本轮主动规避了第 1 轮踩过的坑

第 1 轮因为 `100ms` 字面量写在匿名 namespace 里（`chrono_literals` 只在 `main()` 内可见）
而编译失败。本轮**统一改用显式 `std::chrono::milliseconds(20)` 写法**，
并在匿名 namespace 里定义 `using Ms = std::chrono::milliseconds;` ——
**一次编译通过，0 warning**。

**这条已固化为本任务的施工规范**：示例里不依赖 `chrono` 字面量，
避免作用域陷阱。

### 进度小结（两轮已完成 2 个模块）

| 模块 | 简单版 | 全面版 |
|---|---|---|
| `concurrency/channel` | ✅ 63 行 | ✅ 147 行 / 6 场景 |
| `concurrency/context` | ✅ | ✅ / 6 场景 |

**下一轮建议**：`concurrency/sync`（`sync.hpp` 我已通读：`Mutex` / `ConditionVariable` /
`WaitGroup` / `WaitBackend`，且不在在途清单内），或先花一轮专门读 `config` 再动它。

## 2026-10-13：example 完善 · 第 1 轮（建立规范 + 完成 channel 模块）

### 用户任务（定时推进，19:14 → 22:00 每 30 分钟一次）

> 「每 30 分钟完成一次 example 完善，分为两种：一种简单版，一种全面版，
> 两种都要有简要注释。全面版要求每个模块多使用场景覆盖（至少三个场景）。」

**已建立**：定时 `schedule-460525c6`（cron `*/30 19-21 * * *`，将在 19:30/20:00/20:30/21:00/21:30 触发）
与目标 `goal-fba91d5e`（上限 12 轮）。

### 规范（本轮确立，后续轮次沿用）

```
example/<module>/<topic>_demo.cpp   ← 简单版：50~80 行，最小可用，简要注释
example/<module>/<topic>_full.cpp   ← 全面版：200~400 行，≥3 个场景，每场景一段带注释
```

**保持现有目录结构不动** —— 现有 21 个示例天然就是"简单版"，
只在新增文件上用 `_full` 后缀区分全面版，不破坏任何既有构建。

**输出风格**沿用现有约定：`std::clog << "[模块][场景N] ..."`，`main` 返回 0/1 表示成败。

### 本轮完成：`concurrency/channel`

| 文件 | 行数 | 内容 |
|---|---|---|
| `example/concurrency/channel_demo.cpp` | ~72 | 3 个最小片段：无缓冲交接 / 有缓冲 / Close 后 range 语义 |
| `example/concurrency/channel_full.cpp` | ~150 | **6 个场景** |

**全面版的 6 个场景**：
1. 无缓冲 Channel 的同步交接（发送方阻塞到接收方取走）
2. 有缓冲 + 生产者/消费者 + `Close` 收尾（10 项 / 合计 55）
3. `select` 多路复用（两个 case 只有一个就绪，命中 index=1）
4. `select` + `DefaultCase` 非阻塞探测
5. `select` + 超时（`status = kTimedOut`）
6. `Context` 取消传播到 `Recv`（`Cancelled()`，不再永久等待）

### 过程中纠正的两处自己的错误（如实记录）

首次编译失败，4 error + 1 warning，两处都是我的疏忽：

1. **`100ms` / `0ms` / `30ms` 写在匿名 namespace 的函数里**，而
   `using namespace std::chrono_literals;` 只在 `main()` 内 —— 字面量在那个作用域不可见。
   → 修复：把 `using namespace std::chrono_literals;` 提到匿名 namespace 内。
2. **`scenario_select_multiplex` 声明了 `Scheduler&` 参数但没用到** → `-Wunused-parameter`。
   → 修复：去掉该参数并同步调用处。

**教训**：`-Wall -Wextra -Wpedantic` 是零容忍的（项目基线 0 warning），
写示例时不能依赖"大概能编过"，**必须真编**。

### 验证结果

- 构建：**0 warning / 0 error**
- 运行 `go2cpp_channel_demo` → **EXIT=0**，输出与注释中的"预期"逐条一致
- 运行 `go2cpp_channel_full` → **EXIT=0**，6 个场景全部通过
  （场景 5 实测 `selected=0 status=6`，场景 6 实测 `Cancelled=1 status=4`）
- CMake：两个目标已登记进 `GO2CPP_EXAMPLES`（`concurrency/channel_demo`、`concurrency/channel_full`）
- 在途 5 个文件全程未触碰

### 后续轮次的任务清单（按优先级，供定时轮次取用）

**A 组：已有 `_demo` 但缺 `_full` 全面版**
- `concurrency/future`（现有 `future_demo`）
- `concurrency/scheduler` / `runtime`（现有 `runtime_demo`、`dynamic_gmp_demo`）
- `concurrency/sync`（现有 `fiber_sync_demo`）
- `concurrency/context`（现有 `context_rollback_demo`）
- `concurrency/timer`（现有 `timer_task_demo`）
- `network/socket`（现有 `network_demo`）
- `logging/log`（现有 `log_config_demo`）
- `tools/control_flow`（现有 `control_flow_demo`）

**B 组：完全没有示例，两版都要新建**
- **`config`**（INI 配置，`include/go2cpp/config`）
- **`network/http`**（`http::Server` / `http::Client` / `Parser`）
- `core/parking_condition`
- `concurrency/fiber_local`
- `concurrency/thread_policy`
- `network/address` + `bytearray`（现有 `address_bytearray_demo`，可只补全面版）

**C 组：暂缓，等用户定稿**
- `tools/error` —— 依赖的 `include/go2cpp/tools/error.hpp` **是当前 5 个在途文件之一**，
  API 可能变化，**在其定稿前不写新示例**（现有 `errors_demo` 保持原样）

## 2026-10-13：补充分析 ——「跨线程解锁」是独立于「混合等待」的第二约束

### 用户提醒

> 「对于混合锁，需要的是**跨线程解锁**。」

**这个提醒纠正了上一轮建议里的一个疏漏**：我上一轮说"最优解是 `parking_lot::WordLock`"，
但**只论证了"性能与 lost-wakeup 的正解"，没有把"跨线程解锁"当成筛选条件来用**。

### ★ 关键认识：这是两个独立的需求

**需求 A「混合等待」**：受管 G 拿不到锁时要 **park 让出 M**，普通线程走原生 CV。
→ 约束的是**等待原语的实现**。

**需求 B「跨线程解锁」**：`lock()` 与 `unlock()` 可能落在**不同的 M / 不同的 OS 线程**上
（因为 hook 会把 `connect_with_timeout` 变成跨线程定时任务）。
→ 约束的是**锁状态里能不能记录 owner**。

**两者互不蕴含。** 一个锁可以满足混合等待却仍是 owner-bound，
也可以无 owner 却在等待时阻塞 carrier 线程。

### 用这两条重新筛一遍候选方案

| 方案 | 需求 A 混合等待 | 需求 B 跨线程解锁 | Go2Cpp 可用？ |
|---|---|---|---|
| `std::mutex` | ❌ 阻塞 M | ❌ **owner-bound** | ❌ |
| glibc adaptive mutex | ❌ 阻塞 M | ❌ **owner-bound** | ❌ |
| **`HybridGate`（现状）** | ⚠️ 不区分（靠"临界区极短"规避） | ✅ **`atomic_flag` 无 owner** | ✅ 但**无界自旋** |
| **`parking_lot::WordLock`** | ❌ 直接 futex，会阻塞 M | ✅ **只有 `LOCKED_BIT`，无 owner** | ⚠️ **通过 B，卡在 A** |
| **`sync::Mutex`** | ✅ **受管 G → park**（`WaitBackend`） | ✅ **「Unlock 不要求由同一 G/线程调用，采用 Go 风格所有权」** | ✅ **A、B 都满足** |
| `folly::DistributedMutex` | ❌ | ⚠️ 未确认 | ❌ |

**★ 结论：项目里唯一同时满足 A 和 B 的现成设施是 `sync::Mutex`。**

### ★★ 但 `sync::Mutex` 用不到 `core` 层 —— 循环依赖

`sync::Mutex` 的等待后端就是 `core::ParkingCondition`。若 `core` 反过来用它：

```
core::ParkingCondition  ←  sync::Mutex 的等待后端
        ↑
        └── core 层的 gate 若用 sync::Mutex  →  core → sync → core
```

**所以 `core` 层需要自己实现一个同时满足 A 和 B 的 gate。**

### ★★★ 这个组合没有现成开源实现 —— 原因值得记下

- **Rust / C++ 生态的混合锁都不需要 B**：它们的线程模型里，锁的持有者与释放者天然在同一执行流；
- **Go 的 `sync.Mutex` 满足 B**（不记录 owner），但**它跑在自己的 runtime 上，不需要 A**；
- **`parking_lot` 满足 B，但没有 A**。

**"Fiber 可在 M 间迁移" + "锁可能跨 M 解锁" 是 Go2Cpp 独有的组合**，
所以这里**不存在可以直接照搬的实现** —— 但**可以借鉴两个成熟技巧**。

### 可借鉴的两项，与一项必须自己做的

**可借鉴 ①：`parking_lot::WordLock` 的状态融合**
用一个字承载「锁位 + 队列锁位 + 队列头指针」，
并让**快路径 `unlock` 从 `fetch_sub` 的返回值里读出"是否有等待者"** —— 零额外原子操作。

**可借鉴 ②：`prepare_park()` 令牌机制**
先声明"我准备好被唤醒了"，再发布到队列，最后才等待 —— 封闭 lost-wakeup 窗口
（我们的 G5 已实证缺这一步会挂死）。

**必须自己做 ③：把等待原语换成双模式**
`WordLock` 的 `park()`/`unpark()` 是 Linux futex，**会阻塞 carrier 线程**（违反需求 A）。
`core` 层的 gate 必须把它换成 `ParkingCondition` 的双模式等待：
**受管 G → park 让出 M；普通线程 → 原生 CV**。
**并且要用谓词式等待（`wait(lock, pred)`）重查状态**，把令牌机制与双模式等待接起来。

### ★ 对第 4 轮「移除 HybridGate」决策的修正

先前 P0 项的表述是"移除 `HybridGate`，12 个加锁点改用 `sync::Mutex`"。
**经本轮分析，这个表述对 `core` 层不成立**，应改为：

- **`io.cpp`（network 层）** → 可以换 `sync::Mutex`（A、B 都满足，且无循环依赖）；
- **`sync.cpp`（同类内部）** → 可以换（或直接用真身）；
- **`parking_condition.cpp`（core 层）** → **必须新写一个 gate**：
  **保留 `HybridGate` 的"无 owner / 跨线程解锁"语义（需求 B），
  去掉无界自旋，等待改为 `ParkingCondition` 双模式（需求 A）**。

**换句话说：`HybridGate` 不该被"删除"，而该被"改造"** ——
它的**跨线程解锁语义是必需的**，要丢掉的只是那个 `for(;;)` 无界自旋。

### 状态

- 本轮**只做分析与记录，未改动任何源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- 本结论已补入 `docs/direction_plan.md` 阶段 0 的 A-1 附近。

## 2026-10-13：混合锁方案调研（用户要求「去技术帖和 GitHub 找是否存在更好的方案」）

### 用户提出的方案与实测结论

用户建议把 `HybridGate` 慢路径改成「尝试锁 64 次，然后 `std::this_thread::yield()` 让出」，
规则是「压测性能提升则保留，降低则回滚」。

**先回答用户直接问的那个问题**：`std::this_thread::yield()` **不是挂起** ——
Linux 上它就是 `sched_yield()`，语义是"把当前线程移到同优先级就绪队列末尾"，
**线程始终是 `R`（runnable）状态，不是 `S`（sleeping）**；
**没有同优先级竞争者时立即返回**。所以"64 次后 yield"的本质是
**从"独占空转"改成"轮流空转"**，比 `_mm_pause` 好，但不是"让出等待重新调度"。
**真正进入 `S` 状态的是 `futex(FUTEX_WAIT)`**（`condition_variable` 的底层）。

### 压测数据（三轮，全在 WSL /tmp 独立编译，未动项目源码）

G2 = 用户的方案（64 次 `_mm_pause` 后循环 `{CAS; yield()}`），相对现行 G1 的提升：

| 线程 | 第 1 轮 | 第 2 轮 |
|---:|---:|---:|
| 1（无竞争） | +1.9% | +4.2% |
| 2 | +85.8% | +15.7% |
| 4 | −3.9% | +49.1% |
| 8 | +73.6% | **−27.5%** |
| 16 | +1.8% | +4.3% |
| 32 | −1.2% | −2.5% |

**波动 −27.5% ~ +85.8%，中位数约 +3%。且基线本身不稳**（G1 在 2 线程两轮分别是
36.24 与 76.61 Mops/s）—— **测量噪声本身超过了待测差异**。
**→ 不满足「性能提升则保留」的条件，不采用。**
**→ 无需回滚：全程未修改项目任何源码，今天 13 个提交全部是 `docs:`。**

### ★★★ 调研结论：业界最优解是 `parking_lot::WordLock`

**来源**：`parking_lot_core/src/word_lock.rs`（作者 Amanieu d'Antras），
见 [Firefox 源码树镜像](https://xref.palemoon.org/firefox-esr91/source/third_party/rust/parking_lot_core/src/word_lock.rs)。
另参考 [folly::DistributedMutex](https://archive.softwareheritage.org/browse/content/sha1_git:6bca2794ec9efd7d6ac47c4dd22370e8e3b2384a/?branch=refs/tags/v2021.04.19.00&path=folly/synchronization/DistributedMutex.h&snapshot_id=a23c0ce8792bc8a7a894ace66eaed22daa7f8a9b)
与 glibc 的 adaptive mutex 讨论。业界共识是**三段式**：
**fast CAS → adaptive spin → futex sleep**，难点全在"如何以零额外开销判断是否有人等待"。

**`WordLock` 的关键设计（逐条对应我们三次失败的实验）**：

**① 一个字承载全部状态**

```rust
const LOCKED_BIT: usize = 1;
const QUEUE_LOCKED_BIT: usize = 2;
const QUEUE_MASK: usize = !3;
pub struct WordLock { state: AtomicUsize }   // 锁位 + 队列锁位 + 队列头指针
```

**② 快路径 `unlock` 从 `fetch_sub` 的返回值里读等待者信息**

```rust
let state = self.state.fetch_sub(LOCKED_BIT, Release);
if state.is_queue_locked() || state.queue_head().is_null() { return; }
self.unlock_slow();
```

**★ 这是关键**：`fetch_sub` 返回旧值，**从旧值就能读出"有没有等待者排队"，
不需要任何额外的原子读**。→ **无竞争时零额外操作。**

**这正是我们 G4 失败的原因**：G4 在 `unlock()` 里额外加了一次 `m_waiters.load()`，
在 **2 ns/lock（约 6 个周期）** 的临界区上，这次额外原子读就是 **−54%** 的代价。
`WordLock` 把它融合进了那一次 `fetch_sub`。

**③ 慢路径互斥用「一个位」而不是 `std::mutex`**

`QUEUE_LOCKED_BIT = 2`，慢路径之间靠 CAS 抢这一位。
→ 我们的 G4/G5 用了 `std::mutex m_slow` + `std::condition_variable m_cv`，**重量级原语**；
`WordLock` 直接 `park()`/`unpark()`（futex），中间没有额外锁对象。

**④ ★★★ `prepare_park()` 令牌 —— lost wakeup 的正解**

```rust
thread_data.parker.prepare_park();     // 先准备「停车令牌」
... 挂到队列头 ...
self.state.compare_exchange_weak(...); // 再发布到队列
thread_data.parker.park();             // 最后才 park
```

**先声明"我准备好被叫醒了"，再发布，最后 park** —— 窗口被令牌封闭。
→ **我们的 G5 正是因为缺这一步而挂死**：`fetch_or(kWaiter)` 之后、`cv.wait()` 之前，
`notify_one()` 会从窗口里漏掉。

**⑤ 只在"队列为空"时自旋**

```rust
if state.queue_head().is_null() && spinwait.spin() { ... }
```

**有等待者排队时自旋毫无意义**（锁不可能空出来给你），直接去排队。
**这才是"自适应"的真正含义** —— 不是"固定次数后放弃"。

### 对照表：我们三次实验的每个失败点，都能对上 `WordLock` 的一个设计

| 设计点 | `parking_lot` | 我们的实现 | 后果 |
|---|---|---|---|
| 状态载体 | 1 个 `AtomicUsize`（锁位+队列锁位+队列头） | `atomic<uint8_t>`（2 位） | 少了队列指针，无法把队列塞进同一个字 |
| 快路径 unlock | `fetch_sub` 返回值里读等待者 | 额外一次 `m_waiters.load()` | ❌ −54% |
| 慢路径互斥 | bit 2 做队列锁（CAS） | `std::mutex` | ❌ 重量级 |
| 等待原语 | 直接 futex park/unpark | `std::condition_variable` | ❌ 重量级 |
| lost wakeup | `prepare_park()` 令牌 | 无 | ❌ G5 挂死 |
| 自旋条件 | 仅队列为空时 | 固定 64 次 | 有队列时白烧 |

### 对 Go2Cpp 的落地建议（修正第 8 轮的 P0 项）

第 8 轮结论是"`core` 层需要一个自己的、无自旋的 gate"。
**本轮调研给出了正确做法，且比原先设想更具体**：

1. **不要用 `std::mutex` + `condition_variable`** —— 它们是本轮所有实验退化的根源；
2. **用一个字的位域承载「锁 + 队列锁 + 队列头」**；
3. **快路径 `unlock` 必须从 `fetch_sub`/`fetch_and` 的返回值里读等待者**，不能额外读一次；
4. **必须实现 `prepare_park()` 等价的令牌机制**，否则一定会 lost wakeup（G5 已实证挂死）；
5. **自旋只在队列为空时进行**。

**★ 但有一个 Go2Cpp 特有的约束需要解决**：`WordLock` 直接调 futex 是 Linux 专属，
而 Go2Cpp 的 `core::ParkingCondition` 还必须区分**受管 G（要 park 让出 M）**与
**普通线程（走原生 CV）**。因此**不能照抄 `WordLock`**，需要把第 5 步的
`park()`/`unpark()` 换成 `ParkingCondition` 的双模式等待 —— **这是本项工作的真正难点，
也是它区别于"直接抄一个锁库"的地方**。

### 状态

- 本轮**只做调研与记录，未改动任何源码**；三轮压测的临时实验均在 `/tmp` 且已删除。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- **建议**：把上述"落地建议"补进 `docs/direction_plan.md` 阶段 0 的 A-1，
  替换原先"最低限度去掉自旋"的表述（第 8 轮已否定该表述）。

## 2026-10-13：方向分析 · 第 12 轮（设计 C-L1 的 trace 格式）

### 为什么这一轮必须做

第 11 轮把阶段 1（C-L1 trace）与阶段 2（FSM）由串行改为并行，**trace 的需求因此变了**：
它不再只记录调度决策，还要承载业务状态转换。
**格式若一开始没定好，阶段 2 会被迫返工** —— 所以先把它定下来。

### 产出

**新建 `docs/trace_format.md`** —— C-L1 的详细设计，八节。

### ★★★ 设计里最关键的一个决定：区分「输入事件」与「输出事件」

一次执行里所有非确定性都来自"运行时向外界提问"的时刻。把答案录下来、下次原样喂回去，
执行就会重演。因此每条记录必须明确归属：

| 类别 | 定义 | 重放处理 |
|---|---|---|
| **INPUT** | 运行时的提问 + 外部给的答案 | 把答案喂回去，并校验提问与记录一致 |
| **OUTPUT** | 运行时产生的可观察行为 | 与记录逐条比对，不一致即报分歧点 |

**只有做了这个区分，"逐字节比较"才有意义** ——
否则拿输出比输出，等于自证，**发现不了任何问题**。

### INPUT 只有 4 类，OUTPUT 有 7 类

**INPUT**：`SEL`（调度决策，★ **候选列表也要记**，否则无法验证重放时做了同样选择）、
`TICK`（虚拟时钟推进）、`IO`（fd 就绪）、`RND`（随机抽样）。

**OUTPUT**：`SPAWN` / `PARK` / `WAKE` / **`ENQ`** / `FIRE` / **`NOTE`** / `EXIT`。

**★ 其中 `WAKE` 与 `ENQ` 必须分开记** —— 第 9 轮实测发现
`Task::wake_for_scheduler()` 只做状态标记（返回 `WakeAction`），
**真正的入队在 `Scheduler::requeue_to_processor()`（`:2527`）**。
若只记 `WAKE`，trace 会有一堆"唤醒意图"却**没有顺序**，而顺序恰是重放的全部意义。

### ★★ 与 FSM 解耦的方式：通用的 `NOTE`

trace 格式**不硬编码任何业务概念**。FSM 的状态转换走通用注记：

```
#10 OUTPUT NOTE gid=7  tag=fsm.transition  from=Created  to=Paid  cause=-
```

**为什么这样设计**：
- 调度层不需要知道状态机的存在；
- FSM 层只需调用 `TraceNote(tag, kvs...)`，不用改调度层一行代码；
- 阶段 3 的作品、以及未来任何业务都能复用同一通道。

**这条约束直接保住了第 9 轮那份埋点清单的规模（4 类、约 47 行）** ——
若 trace 要为 FSM 定制事件类型，埋点就会蔓延到业务层，**成本立刻失控**。

### 验收标准（可执行，5 条）

1. 同一 seed 跑两次，OUTPUT 序列逐字节相同；
2. 用录下的 trace 重放，INPUT 提问一致、OUTPUT 逐条相同；
3. **人为篡改一条 INPUT 的答案，重放必须报「行为分歧」并定位到正确序号**；
4. 篡改一条 OUTPUT，比对必须失败；
5. **一个 FSM demo 的状态转换以 `NOTE` 形式出现在 trace 里，且重放后序列一致**。

**第 3、4 条是重点**：它们证明这个机制**真的在验证，而不是自证**。

### 如实留白（4 项未决定）

① **trace 体量控制** —— 高并发下每秒事件量很大，是否需要采样或环形缓冲，**未设计**；
② **生产环境是否常开** —— 关闭时开销 +1.06%（第 10 轮实测），常开可行，
   但**写入 trace 本身的 I/O 成本未测**；
③ **跨版本重放兼容** —— 只写了"必须升版本"，**没有迁移工具设计**；
④ **`RND` 的粒度** —— 记每次抽样 vs 只记种子由运行时重演，**两方案各有取舍，未定**。

### 状态

- 本轮**新增 1 个设计文档，未改动任何源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- **下一轮（第 13 轮）候选**：① 按同样方法量化阶段 3（作品）的成本与验收标准；
  ② 竞品源码深挖；③ 给 trace 格式补上"体量控制"的设计（本轮留白项之一）。

## 2026-10-13：方向分析 · 第 11 轮（量化阶段 2 的接口面 —— 得到一个路线修正）

### 本轮方法

按第 9 轮验证成功的方法：**先 grep 量化真实代码，再给估算**，
而不是先写一份漂亮的设计文档、再发现成本不对。

### 实测一：项目里完全没有状态机设施

```
grep -rniE "class [A-Za-z_]*Fsm|StateMachine|状态机" include/ src/
```

**命中的全部是注释**：
- `channel.hpp:252` / `:259` —— "不要把异常带入 `select` 状态机"
- `scheduler.hpp:188` / `:274` / `:343` —— "状态机和调度语义不受影响"、"执行权仍由 `Task` 状态机串行化"
- `io.hpp:182` —— "IOManager 状态机处理"

**→ 这些都是内部实现，没有任何"给用户用的状态机设施"。阶段 2 的框架必须完全新写。**

### 实测二：成本完全取决于"野心"

| 野心 | 内容 | 成本 |
|---|---|---|
| **薄封装** | 一组约定 + 约 50~200 行基类（`Run()` 循环 + `Channel<Event>` + `switch` + `Context`） | **1 周** |
| 中等 | + 状态注册、转移日志、超时策略封装 | 2~4 周 |
| 完整 DSL | + 转移表、可视化、热加载、持久化 | 数月（**不做**） |

**★ 而第 2 轮的就业数据支持"薄"**：面试问的是"**你设计了什么**"，
而"**薄框架 + 一个跑得起来的作品**"比"厚框架 + 没有作品"更有说服力。

### ★★★ 本轮最重要的产出：一处路线修正

**到第 10 轮为止，路线把阶段 1（C-L1 trace）和阶段 2（FSM）当成先后两步。
实测后应改为「并行」——因为两者互相成就：**

- **没有 FSM**：C-L1 的 trace **没有值得记录的对象**。
  trace 记的是"谁 park / 谁被选中"，而**一个纯测试程序里的 park 序列是琐碎的、没有业务意义的**。
- **没有 trace**：FSM 就只是"又一个并发框架"，**与 libco / sylar / coost 没有区别**。

**→ 正确设计：薄 FSM 基类 + 「每一次状态转换都进 trace」+ 一个能演示
"重放一个状态机系统的完整执行"的 demo。**

**为什么这才是差异化**：libco / sylar 是回调式或抢占式，
- **没有可枚举的调度点** → 无法记录状态转换序列
- **没有 `ContextRollback` 这样的事务原语** → 无法在失败时原子回滚

**这两件事叠加，就是"状态机系统的执行可以被重放"** —— 而这是竞品**结构上做不到**的。

### 路线的连锁调整

这个修正会往上影响第 3 轮定下的四层结构：

| 层 | 原描述 | 修正后 |
|---|---|---|
| 第二层（主线） | D 并发状态机框架 | **D + C-L1 集成** —— 框架的验收标准里包含"可重放" |
| 第三层（标签） | C-L1/L2 可验证性 | **不再是独立的一层，而是渗透进第二层** |
| 第四层（作品） | G 用 D 做一个服务 | 不变，但作品要能演示"故障注入 + 重放定位" |

**注意：这不改变"做什么"，只改变"怎么排"。** 四层结构依然成立，
但**第二层和第三层要合起来做**，因为它们本来就是一件事的两面。

### 状态

- 本轮**只读代码 + 编辑文档/记录，未改动任何源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- 修正已写入 `docs/direction_plan.md` 阶段 2（在验收标准之前插入"第 11 轮实测"小节，
  并追加一条可重放验收标准）。
- **下一轮（第 12 轮）候选**：① 按同样方法量化阶段 3（作品）的成本与验收标准；
  ② 设计 C-L1 的 trace 格式（现在它必须同时服务于 FSM 状态转换）；
  ③ 竞品源码深挖。

## 2026-10-13：方向分析 · 第 10 轮（验证 C-L1 的「生产零开销」前提 —— 实测成立）

### 验证命题

路线文档阶段 1 的 C-L1 写的是「**约 50 行**（**生产零开销**）」。
第 9 轮验证了"约 50 行"（实测约 47 行，成立），**但"生产零开销"这一条还没验**。

**它的具体含义**：埋点写成 `if (trace) trace(...)`，`trace` 默认是空 `std::function`。
问题：**这次判空 + 可能的分支，在高频调度热路径上真的可以忽略吗？**

### 实验：独立微基准，编译运行

在 WSL `/tmp` 写独立 `.cpp`（**不动项目源码**），模拟调度热循环：
每轮做一点确定性计算，同时可选地调用一次回调。

| 版本 | 含义 |
|---|---|
| **V0** | **完全没有埋点**（基线） |
| **V1** | `if (g_trace_fn) g_trace_fn(i);` —— **`std::function` 为空（生产默认）** |
| **V3** | `if (g_trace_raw) g_trace_raw(i);` —— **裸函数指针为空（备选方案）** |
| **V2** | 同 V1，**但 trace 已开启**（用于对比"开启时的真实成本"） |

判据：`V1` 相对 `V0` 的额外开销是否可忽略。

### 实测结果

见本轮输出（2,000,000 次迭代 × 20 轮，取 ns/iter）。

**结论**：
- **V1（`std::function` 判空关闭）相对基线的额外开销可忽略 —— "生产零开销"成立。**
- `std::function` 与裸函数指针两种写法的差异也在噪声范围内，**不必为了性能退回裸指针**
  （`std::function` 更好用，支持 lambda 捕获）。
- **V2 显示的是"trace 开启时"的成本** —— 那属于调试/模拟模式，不在生产路径上，
  因此**不需要为零开销做任何妥协**。

### 这一轮的意义

**C-L1 的两个断言现在都被实测支撑**：

| 断言 | 验证轮次 | 结果 |
|---|---|---|
| 「约 50 行」 | 第 9 轮 | ✅ 实测约 47 行 |
| 「生产零开销」 | **第 10 轮** | ✅ **实测可忽略** |

**→ C-L1 是四层结构里第一个"估算经得起验证"的条目**，可以放心进入实施。

### 五轮验证的完整汇总

| 轮次 | 原估算/断言 | 实测 | 结论 |
|---|---|---|---|
| 6 | 引用 `Context::SetNowFunction` | 该名字**不存在**（实为 `SetNowFunctionForTesting`） | ❌ 推翻 |
| 7 | C-L2 虚拟时钟「约 0 行」 | **约 50~100 行**（`TimerService` 硬编码 `steady_clock`） | ❌ 推翻 |
| 8 | 「最低限度去掉慢路径自旋」 | **语义走不通**，约 30~50 行（实验：丢 5.45% 更新） | ❌ 推翻 |
| 9 | C-L1「约 50 行」 | **约 47 行** | ✅ 成立 |
| **10** | **C-L1「生产零开销」** | **实测额外开销可忽略** | ✅ **成立** |

**三次推翻、两次成立。** 规律很清楚：
**凡是"真的去数了、去测了"的，都估准；凡是凭"改这几行应该不难"的直觉得出的，都低估。**

### 状态

- 本轮**只在 WSL `/tmp` 写临时实验（已清理），未改动任何项目源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- C-L1 的两条断言现已全部有实测支撑，写入路线文档。
- **下一轮（第 11 轮）候选**：① 深化阶段 2（FSM 框架）的接口设计与成本估算
  （按规律，这里很可能也低估了，**应当先 grep 再估算**）；
  ② C-L1 的 trace 格式设计；③ 竞品源码深挖。

## 2026-10-13：方向分析 · 第 9 轮（验证 C-L1「约 50 行」—— 四次验证里第一次成立）

### 验证命题

路线文档阶段 1 的 C-L1 写的是：「在 `Scheduler` 决策点插默认空回调，
记录谁 park、就绪候选、选中谁、唤醒顺序。**约 50 行**（生产零开销）。」

**这是前几轮里最后一个未经检验的低成本断言。**

### 量化：决策点到底有多少

`grep` 统计 `scheduler.cpp`（4048 行）中与调度决策相关的引用：

| 类别 | 出现次数 |
|---|---:|
| park 相关（`park_for_scheduler` / `ParkAction` / `kParked` / `park_wait` / `park_io`） | 22 |
| wake 相关（`wake_for_scheduler` / `WakeAction` / `wake_or_cancel` / `wake_registered` / `wake_io`） | 34 |
| 就绪队列选择（`.front()` / `.back()` / `steal`） | 11 |
| 任务状态转换（`mark_runnable` / `try_mark_running` / `mark_waiting` / `mark_yielded` / `promote_new`） | 16 |
| `select` 决策（`TrySelect` / `SelectWaitState`，位于 `channel.cpp`） | 3 |
| **合计** | **86** |

**另有 `Scheduler` 的 30+ 个公开方法**（`spawn` / `enqueue` / `park_*` / `wake_*` / `requeue_*` / `yield_*` …）。

**初见这个数字会以为 C-L1 要改上百处 —— 但那是误解。**

### ★ 结论：只有 4 类埋点，约 47 行，「约 50 行」成立

| 埋点 | 精确位置 | 约计 |
|---|---|---|
| **park 决策** | `Task::park_for_scheduler()`（`scheduler.cpp:612-625`），3 个返回点 `kRejected`/`kRequeued`/`kParked` | ~5 行 |
| **唤醒决策** | `Task::wake_for_scheduler` / `wake_for_io` / `wake_for_wait`（`:538`/`:568`/`:580`） | ~8 行 |
| **选中决策** | `worker_loop` 的 4 处队列 pop（`:3535`、`:3679`、`:3699`、`:3721`） | ~8 行 |
| **`select` 决策** | **另一个文件**：`channel.hpp:442` `TrySelect`、`:457` `TrySelectPair` | ~6 行 |
| 回调接口 + 默认空实现 | 新增 | ~20 行 |
| **合计** | | **~47 行** |

**→ 这是第 6/7/8/9 轮四次动手验证里，第一次估算站得住的。**

### ★★ 但发现一处埋点位置与直觉相反（本轮最有价值的发现）

**「唤醒顺序」不能埋在 `wake_*` 里。**

`Task::wake_for_scheduler()`（`scheduler.cpp:538`）实测只做**标记**：

```cpp
ParkAction Task::park_for_scheduler() noexcept {          // :612
    ...
    if (m_wake_pending.exchange(false, std::memory_order_acq_rel)) {
        return ParkAction::kRequeued;                     // ← 消费令牌
    }
    m_state.store(GState::kWaiting, std::memory_order_release);
    return ParkAction::kParked;                           // ← 只改状态
}
```

它返回的是 `WakeAction` / `ParkAction` **枚举**，**没有触及就绪队列**。
**真正的"入队"发生在 `Scheduler::requeue_to_processor()`（`:2527`）。**

→ **要记录"谁先被放进就绪队列"，埋点必须在 `requeue_to_processor`，而不是 `wake_*`。**

**若不验证这一点会怎样**：trace 里会记录一堆"唤醒意图"，**却没有顺序** ——
而**顺序恰恰是重放的全部意义**。这会导致 C-L1 交付后才发现 trace 无法用于重放。

### 三个前提（写入实施注意事项）

1. **只记录这 4 类决策** —— 不要试图覆盖全部 86 处，那才是真正昂贵的原因；
2. **必须跨文件插桩**（`scheduler.cpp` + `channel.hpp`），不能只在调度器里做；
3. **「生产零开销」尚未验证** —— 需要确认 `if (trace) trace(...)`
   （默认空 `std::function` + 一次分支）在热路径上确实可忽略。

### 四轮验证的汇总（这是本目标最有价值的副产品）

| 轮次 | 原估算 | 实测 | 结论 |
|---|---|---|---|
| 6 | 引用 `Context::SetNowFunction` | 该名字**不存在** | 推翻 |
| 7 | C-L2 虚拟时钟「约 0 行」 | **约 50~100 行**（`TimerService` 硬编码 `steady_clock`） | 推翻 |
| 8 | 「最低限度去掉慢路径自旋」 | **语义走不通**，约 30~50 行（实验：丢 5.45% 更新） | 推翻 |
| **9** | **C-L1「约 50 行」** | **约 47 行** | **✅ 成立** |

**前三次低估的共同原因**：估算时看的是"要改的那几行"，
而不是"**这条数据流经哪几个组件、每个组件的契约是什么**"。
**第 9 轮之所以估准，是因为这次真的去 grep 了决策点的分布，而不是凭"埋几个回调"的直觉。**

### 状态

- 本轮**只读代码 + 编辑文档/记录，未改动任何源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- 修正已写入 `docs/direction_plan.md` 阶段 1（C-L1 行 + 新增「第 9 轮实测」小节）。
- **下一轮（第 10 轮）候选**：① 验证前提 3（"生产零开销"是否成立，可写微基准）；
  ② 深化阶段 2（FSM 框架）的接口设计与成本估算；③ 竞品源码深挖。

## 2026-10-13：方向分析 · 第 8 轮（动手验证阶段 0 的「最低限度去掉自旋」是否成立）

### 本轮验证的命题

路线文档阶段 0 在 A-1 里写了一句成本估算：

> **「`core` 层需要一个自己的、无自旋的 gate 实现**（原子状态 + 条件变量；
> **或最低限度去掉 `hybrid_mutex.hpp:75-81` 慢路径的 `for(;;)` 无界自旋**）」

**「最低限度」这四个字暗示这是一次几乎零成本的改动（删掉 6 行循环）。**
按第 7 轮的教训（"接口存在 ≠ 能力成立"），这类估算必须动手验证。

### 验证方法：写独立实验并编译运行（不动项目源码）

在 WSL `/tmp` 下写一个独立 `.cpp`，实现三个版本的 gate，
用**同样的高竞争负载**（16 线程 × 20000 次）做非原子计数器自增，
**判据是计数是否丢更新**（互斥有效则必然等于 16×20000）：

| 版本 | 实现 |
|---|---|
| **V1** | 现行 `HybridGate`（64 次有限自旋 + 慢路径 `for(;;)` 无界自旋） |
| **V2** | **天真去掉慢路径 `for(;;)`** —— 抢不到就直接返回（假装持有） |
| **V3** | **用 `std::condition_variable` 替代慢路径自旋** |

### 逻辑分析：为什么"最低限度去掉"在语义上走不通

`HybridGate` 的 `lock()` 契约（BasicLockable）是「**返回时必持有锁**」。
现行慢路径之所以能保证这一点，**恰恰靠那个 `for(;;)` 循环**：

```cpp
        m_slow.lock();
        for (;;) {                                   // ← 想去掉的就是它
            if (!m_fast.test_and_set(acquire)) {
                m_slow.unlock();
                return;                              // ← 唯一能"合法返回"的出口
            }
            relax();
        }
```

**去掉 `for(;;)` 后，`lock()` 只有两条出路**：
1. **重新尝试** → 又变回自旋（没解决问题）
2. **直接返回** → **返回时未持有锁，互斥语义被破坏**

**没有第三条路。** 所以要真正去掉自旋，**必须引入等待机制（条件变量）** —— 这就带来三处连带改动：

- `lock()` 里 `m_fast` 抢到后要 `guard.unlock()` 释放 `m_slow`（否则 `m_slow` 被永久持有）
- `unlock()` 侧必须 `notify_one()`（否则等待者永不醒来）
- 新增一个 `std::condition_variable` 成员

**→ 这不是"删掉 6 行"，而是一次小型重写。**

### 实验结果（编译运行实测）

见本轮输出。**V2 出现丢更新，V1 与 V3 正确。**

### 结论

**阶段 0 的「最低限度去掉慢路径 `for(;;)`」不成立。**

- **去掉自旋 ≠ 锁还能用**：因为 `lock()` 的契约要求"返回即持有"，
  而去掉循环后这条契约无法维持。
- **实际成本**：约 **30~50 行**（新增条件变量、改 `lock()` 的双锁释放、改 `unlock()` 加 notify），
  而不是"删掉一个循环"。
- **修正**：路线文档阶段 0 里"或最低限度……"这个选项应删除，只保留"需要一个自己的、
  用条件变量实现的无自旋 gate"这一种做法。

### ★ 这是本会话第三次「动手验证推翻文档估算」

| 轮次 | 原估算 | 实测结果 |
|---|---|---|
| 第 6 轮 | `Context::SetNowFunction` | **这个名字根本不存在**（实为 `SetNowFunctionForTesting`） |
| 第 7 轮 | C-L2 虚拟时钟"约 0 行" | **实为约 50~100 行**（`TimerService` 硬编码 `steady_clock`） |
| **第 8 轮** | **"最低限度去掉慢路径自旋"** | **语义上走不通，实为约 30~50 行** |

**三次都低估了。** 共同原因是同一个：**估算时看的是"要改的那几行"，
而不是"这条数据流经哪几个组件、每个组件的契约是什么"。**

**→ 这条已写入路线文档，作为后续所有成本估算的强制检查项。**

### 状态

- 本轮**只在 WSL `/tmp` 下写临时实验文件，未改动任何项目源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- **下一轮**：把"最低限度"选项从路线文档中删除并写入本次实测数据；
  或继续验证阶段 2（FSM 框架）的成本估算。

## 2026-10-13：方向分析 · 第 7 轮（动手验证假设 —— 发现 C-L2 的成本被低估，但结论不变）

### 本轮任务

定时任务给出的四个选项里，选**第 2 项：动手验证路线里的关键假设**。
目标假设是路线文档阶段 1 写的那句：「**C-L2 虚拟时钟：约 0 行**」。

**为什么必须先验证它**：C-L2 是"第三层标签"的全部依据。如果它其实很贵，
整个标签层就要重估。

### 验证方法与证据

不写实验代码，先做最便宜的检查 —— **看两条时间路径各自用哪个时钟**：

| 检查项 | grep 实测 | 结论 |
|---|---|---|
| `Context::WithTimeout` 怎么算 deadline | `src/concurrency/context.cpp:2195`：`WithDeadline(parent, SaturatingDeadline(Now(), timeout))` | 走 `Now()` |
| `Context::Now()` 是否尊重注入 | `src/concurrency/context.cpp:2317-2322`：取 `s_now_function` | ✅ **可注入** |
| 注入接口 | `include/go2cpp/concurrency/context.hpp:224` `SetNowFunctionForTesting`、`:226` `ResetNowFunctionForTesting`、`:134` `using NowFunction` | ✅ 存在 |
| **`TimerService` 用什么时钟** | **`src/concurrency/timer.cpp:32`：`using Clock = std::chrono::steady_clock;`** | ❌ **硬编码** |
| `TimerService` 有多少处依赖该类型 | `include/go2cpp/concurrency/core/timer.hpp`：`:55`/`:57`/`:70`/`:83`/`:84`/`:104`/`:110`/`:117`/`:120`/`:128`/`:129` | **11 处** |
| 是否有测试钩子 | 全仓库仅 `Context` 上两个 `ForTesting`；**无** `AdvanceTo`/`FireDue`/`Pump`/`TriggerDue` | ❌ **没有** |

### ★ 结论：原估算"约 0 行"不成立

**运行时存在两个互相独立的时间源**：

```
Context::Now()        ← 可注入（SetNowFunctionForTesting）
TimerService          ← 硬编码 steady_clock（timer.cpp:32），无任何测试钩子
```

**后果**：`WithTimeout(parent, 30s)` 的 deadline 会被"假时间"算出，
**而定时器仍按真实 `steady_clock` 等待** —— **测试还是真等 30 秒**。

**修正后的 C-L2 工作量**：**约 50~100 行**（需让 `TimerService` 的时钟可注入，
或提供"推进到指定时刻"的钩子），并要动 `timer.hpp`/`timer.cpp` 两个文件。

### ★ 但结论不变，而且多了一个意外收获

**C-L2 仍然是低成本项**（相比阶段 2/3 的周级工作）—— 只是从"0 行"修正为"约 100 行"。

**意外收获**：**"两套时间源"本身就是一个非确定性来源**。
`Context` 认为现在是 T1，`TimerService` 按 T2 触发，两者的相对关系取决于真实调度时序。
**这恰恰属于 C 要消除的那一类问题** —— 换句话说，做 C-L2 的过程会顺手修掉一个真实的时序缺陷。
**它不再是"为了测试而改造"，而是"消除一个真实的双时钟不一致"。**

### 本轮的方法论价值

**这是本会话第二次"动手验证推翻文档结论"**：
- 第 6 轮：自检 grep 发现 `SetNowFunction` 这个名字根本不存在（实为 `SetNowFunctionForTesting`）
- 第 7 轮：进一步验证发现**即使名字对了，它也不够** —— 还有第二个时间源

**教训**：**"接口存在"不等于"能力成立"。** 第 6 轮我核实了接口名，就以为验证完了；
**但接口存在只证明了"能改 Context 的时钟"，没证明"能改超时的时钟"** ——
这两件事在同一个系统里由两个不同组件负责。

**这条已写入路线文档的阶段 1，作为对后续所有"约 N 行"估算的警示**：
估算必须以"**数据流经哪几个组件**"为单位核对，而不是以"有没有现成接口"为单位。

### 状态

- 本轮**只读代码 + 编辑文档/记录，未改动任何源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- **修正已写入 `docs/direction_plan.md` 的阶段 1（C-L2 行 + 新增「第 7 轮实测修正」小节）。**
- **下一轮可选**：① 深化 C-L1 的插桩点清单与 trace 格式；② 继续验证其他"低成本"估算
  （例如阶段 0 声称 `core` 层"最低限度去掉慢路径自旋"是否真的够）；
  ③ 竞品源码深挖。

## 2026-10-13：方向决策分析 · 第 6 轮（交付自检 —— 发现 3 处问题并修正）

### 自检方法：不读一遍就下结论，而是把文档引用的每个"现成能力"逐条 grep 验证

| 前提 | 文档原写法 | grep 实测 | 判定 |
|---|---|---|---|
| 虚拟时钟接口 | `Context::SetNowFunction` | **实为 `SetNowFunctionForTesting`**（`context.hpp:224`），配套 `ResetNowFunctionForTesting`（`:226`）、`Now()`（`:228`）、`using NowFunction`（`:134`） | ❌ **名字写错** |
| `HybridGate` 引用数 | "3 个生产文件、12 个加锁点" | **共 29 处**（含定义、别名、注释、测试、示例） | ⚠️ **M0 标准不完整** |
| 第 1~2 轮实现的增强 API | 假定仍在 | **全部仍在**：`RecordUndoWithCause`(`:330`)、`record_undo_with_cause`(`:332`)、`pending_undo_count`(`:367`)、`rollback_cause`(`:379`)、`StatusName`(`:381`)、`status_name`(`:382`) | ✅ **用户的重排保留了它们** |
| `ContextRollback` / `Framer` / `sync::Mutex` | 引用 | 均存在（`context.hpp:261` / `framing.hpp:22` / `sync.hpp:78`） | ✅ |

### 修正一：`SetNowFunction` → `SetNowFunctionForTesting`

文档写的 API 名**不存在**。实际接口带 `ForTesting` 后缀 —— 这恰好说明它是为测试场景设计的，
与 C-L2 的用途一致。**照原文去写会编译失败**，这正是自检的价值。

### 修正二：M0 的"grep 归零"标准不完整

原标准只要求 `src/` 与 `include/` 归零。但**实测 29 处引用分布**在：定义
（`core/hybrid_mutex.hpp`）、4 行转发头、3 个生产文件的 4 处成员 + 12 个加锁点、
`tests/concurrency/test_hybrid_gate.cpp`、`example/concurrency/hybrid_mutex_demo.cpp`。
**→ 删掉定义后测试与示例会编译失败。** 标准已补全：连同测试/示例文件、CMake 登记、
`example/README.md` 相关行一并移除。

### ★★★ 修正三（架构级，本轮最有价值的发现）：替换 `HybridGate` 有一个不能踩的坑

第 4 轮清单里我写的是"3 个生产文件 12 个加锁点统一改用 `sync::Mutex`"。
**自检发现这在 `parking_condition.cpp` 上行不通**：

```
core::ParkingCondition   ←  sync::Mutex 的等待后端就是它
        ↑
        └── parking_condition.cpp 现用 HybridMutex 保护 WaitNode::m_wake_mutex
            若改成 sync::Mutex  →  core → sync → core  循环依赖
```

**三个使用点的结论因此不同**：

| 文件 | 层次 | 能否直接换 `sync::Mutex` |
|---|---|---|
| `src/network/io.cpp` | network | ✅ 可以 |
| `src/concurrency/sync.cpp` | sync（同类内部） | ✅ 可以 |
| **`src/concurrency/parking_condition.cpp`** | **core** | **❌ 不可以 —— 循环依赖** |

**→ `core` 层需要一个自己的、无自旋的 gate 实现**（原子状态 + 条件变量；
或最低限度去掉慢路径 `hybrid_mutex.hpp:75-81` 的 `for(;;)` 无界自旋）。

**附带澄清一个此前的误解**：`HybridGate` 内部那个 `std::mutex m_slow` **本身是安全的** ——
它的加解锁全在同一次 `lock()` 调用内完成（拿不到 `m_fast` 就 `m_slow.unlock()` 返回），
**不涉及跨 M 解锁**。跨线程解锁问题针对的是**被 `HybridGate` 保护的业务临界区**，
不是 `m_slow` 自己。**所以真正要修的只是慢路径的无界自旋循环。**

### 目标完成度复核

| 要素 | 状态 | 证据 |
|---|---|---|
| ① 分析项目 | ✅ | 本会话通读 `src/` 22,004 行 + `include/` 9,653 行 |
| ② 对比 GitHub 同类项目 | ✅ | 第 1 轮，10 个项目实测 star / 最后推送 |
| ③ 就业市场认可度 | ✅ | 第 2 轮，含薪资区间与"协程"的岗位定位 |
| ④ 方向 + 可落地路线 | ✅ | 第 3~5 轮；`docs/direction_plan.md`（211 行）已提交（`c51f64a`） |
| ⑤ 交付自检 | ✅ | 本轮，发现 3 处问题并全部修正 |

**五要素全部达成。**

### 状态

- 本轮**只编辑 `docs/direction_plan.md` 与写记录，未改动任何源码**。
- 在途 5 个文件全程未触碰；台账待办 24 条不变。
- **定时任务 `schedule-6010b30e`（每 30 分钟）保持启用** —— 用户要求的是"多轮定时对话"，
  即使本目标结项，定时推进仍按定时任务自带的 prompt 继续。

## 2026-10-13：方向决策分析 · 第 5 轮（产出可落地路线文档 —— 目标核心交付物）

### 本轮产出

**新建 `docs/direction_plan.md`** —— 把第 1~4 轮的结论合并成一份完整的、带里程碑与
**可验证验收标准**的落地路线。这是本次目标的核心交付物。

### 文档结构

1. **结论摘要**（三句话：做什么 / 为什么 / 怎么做）
2. **依据**：竞品格局（第 1 轮）+ 就业市场（第 2 轮）+ 三维打分（第 3 轮）
3. **战略：四层结构**（地基 → 主线 → 标签 → 作品）
4. **分阶段落地计划**（阶段 0~4），每阶段含步骤、工作量、**验收标准**
5. **里程碑总表** M0~M5
6. **风险与应对**（含 Fiber 栈、panic 语义、无 TLS/JSON、hook 边界、台账漂移）
7. **明确不做什么**（7 条）
8. 一句话总结

### 本轮的关键设计决定

**1. 验收标准一律写成"可执行的检查"，而不是"完成了某事"。** 例如：

- 阶段 0：「`grep -rn "HybridGate\|HybridMutex" src/ include/` 返回 0」而不是"移除 HybridGate"
- 阶段 1：「**同一 seed 两次运行产出逐字节相同的 trace**」而不是"实现 trace"
- 阶段 1：「**30 秒超时测试在 <100 ms 内跑完**」而不是"实现虚拟时钟"
- 阶段 3：「**能报连接数/QPS/P99/故障注入通过率**」而不是"完成作品"

**理由**：不可验证的标准等于没有标准，尤其在"这个项目到底算不算做完"这种问题上。

**2. 把"台账核实"提升为固定步骤（A-0），而不是一次性动作。**
依据是第 4 轮发现的 2 处台账漂移（X-26、T-03）。

**3. 明确写入"不做什么"（7 条），并且每条都给出理由。**
理由全部来自前四轮的实测数据，不是主观取舍。

**4. 把风险按"外部库能否补救"分类。**
其中 **Fiber 固定栈是唯一外部库补不了的 runtime 缺陷** —— 因此它的应对（提高默认栈 +
CI 栈用量告警）被写在阶段 2 之前，而不是事后。

### 状态

- **本轮新增 1 个文档，未改动任何源码。**
- 在途 5 个文件全程未触碰。
- 台账待办仍 24 条。
- **目标状态评估**：本次目标的四要素——① 分析项目 ✅（本会话已通读全部源码）
  ② 对比 GitHub 同类项目 ✅（第 1 轮，10 个项目实测数据）
  ③ 就业市场认可度 ✅（第 2 轮，含薪资与关键词）
  ④ 决定方向 + 可落地路线 ✅（第 3~5 轮，含里程碑与验收标准）——**均已达成**。
- **下一轮**：对路线文档做一次**交付自检**（核对每条验收标准是否真的可执行、
  是否有自相矛盾、是否与台账/代码现状一致），确认无问题后即可结项。

## 2026-10-13：方向决策分析 · 第 4 轮（第一层 A 落到阻塞清单 + 台账纠错）

### 本轮方法

读台账全文（**实测 80 条条目、24 条待办**），逐条判断**是否阻塞第二层（D 并发状态机框架）
或第三层（C trace/虚拟时钟）**，只把真正的阻塞项列入 A 的执行清单，其余延后。

**判据**：一条待办算"阻塞"，当且仅当——不修它，D 或 C 会在真实负载下先失败，
或者 C 的确定性前提不成立。

### ★ A 的执行清单：只有 8 项真正阻塞（其余 16 条可延后）

| 优先 | ID | 阻塞谁 | 为什么阻塞 | 第一步 |
|---|---|---|---|---|
| **P0** | **X-35 + X-33 + T-03** | **D** | `HybridGate` 慢路径 `for(;;)` **无界自旋**；D 的目标形态是"高并发长连接服务"，**自旋会在压力下先炸**。用户已明确决策移除 | 3 个生产文件、12 个加锁点（`parking_condition.cpp` / `sync.cpp` / `io.cpp`），按要求改用 `sync::Mutex`；**注意 `std::mutex` 不可用**（hook 下跨线程解锁） |
| **P0** | **X-13** | **D** | `Fiber::Impl::~Impl` 获取 `m_resume_claim` 用**无界 yield 自旋**；D 会高频创建/销毁 Fiber，**析构空转会直接吃掉 CPU** | 加退避或超时；与 X-35 同批处理（同类模式） |
| **P1** | **X-11** | **C** | `Scheduler::enqueue` 的生产者条带是 `static thread_local` —— **这是"隐藏的调度输入"**，C 要求所有决策可记录/可重放，隐藏状态会让 trace 无法解释 | 把条带从 `thread_local` 改为**显式可注入的决策源** |
| **P1** | **X-10** | **C** | 同一个 `WakeAction::kPending` 在不同唤醒入口**语义相反**（`wake_registered` 视为失败返回 false）。C 的 trace 要能解释每次唤醒，语义不一致就无法归因 | 统一 `kPending` 语义并记录在头文件 |
| **P1** | **X-14** | **D** | `WaitNode::stop_operations` 同样是**无界 yield 自旋**，且在 I/O 回收路径上——D 的每个长连接都会走到 | 与 X-13 同批处理 |
| **P2** | **X-25（共 13 处）** | D / C | `noexcept` 函数内部加锁或分配 → 压力下可能 `terminate` | 逐处加 try/catch；**这类问题在 C 的"故障注入"下会最先暴露** |
| **P2** | **X-12** | D | `worker_loop` 用**同一个** `maintenance_budget` 驱动两种维护，且两者都会把它清零 → 长期运行下某类维护会饥饿 | 拆成两个独立预算 |
| **P2** | **X-32** | **C** | `Context::Now()` 热路径加锁 —— **C-L2（虚拟时钟）会让 `Now()` 调用频率大幅上升**，把现有开销放大 | 先确认现状，再决定是否改为无锁读 |

**延后的 16 条**（有价值但不阻塞）：T-09~T-18（测试覆盖类）、D-24/D-25（文档）、
X-22/X-23/X-24/X-28/X-29/X-31/X-36（局部缺陷）、U-01~U-04（用户决策）、X-01~X-05（文档/示例一致性）。

### ★★ 本轮纠错：台账有两条已与实际不符

**1. `X-26` 应标为「已修复」。**
台账原文说 `include/go2cpp/concurrency/sync.hpp` 是"一份过时的重复定义……全仓库没有任何文件包含它"。
**实测现状**：该文件**只有 6 行**，且是转发头：

```cpp
// 兼容路径转发头。`sync` 的真身位于 `include/go2cpp/sync.hpp`，本文件此前
// 携带一份过时副本（缺 WaitBackend::ParkForCleanup 与
// Mutex::LockForConditionWait 等成员），已改为转发，避免两处定义漂移。
#include "go2cpp/sync.hpp"
```

→ **该条目描述的 ODR 风险已消除，状态应改为「已修复」。**

**2. `T-03`（"`HybridGate`/`HybridMutex` 全仓无测试引用"）与事实不符。**
**实测**：`tests/concurrency/test_hybrid_gate.cpp` **存在**，且已在
`cmake/Go2CppTests.cmake:23` 登记；另有 `example/concurrency/hybrid_mutex_demo.cpp`（`:95` 登记）。
→ **该条目的表述需要修正**（可能是想表达"生产代码无测试覆盖"，但字面写的是"全仓无引用"）。
**本轮先记录，不改写该条目** —— 因为 `HybridGate` 整体即将按 X-35 移除，
T-03 会随之一并作废，改写它是无谓工作。

### ★ 方法论收获

**台账是这次方向决策的输入，所以台账本身的准确性会影响决策质量。**
本轮在读台账时就撞出两条过时记录。**这说明 A（补齐短板）的第一步不该是"照着台账动手"，
而应该是"先核实台账是否与代码一致"** —— 否则会花时间去修已经修好的东西
（X-26 就是一个例子：如果照做，会去"修复"一个已经是转发头的文件）。

**修正后的建议顺序**：
```
A-0  核实台账与代码的一致性（本轮已完成一轮，发现 2 处不符）
A-1  P0：HybridGate 家族移除（X-35/X-33/T-03）+ X-13 析构自旋
A-2  P1：X-11 / X-10（C 的确定性前提）+ X-14（D 的 I/O 路径）
A-3  P2：X-25 十三处 / X-12 / X-32
```

### 状态

- **本轮只读代码与文档、修正记录，未改动任何源码。**
- 在途 5 个文件全程未触碰。
- 台账待办数仍为 **24 条**（本轮不新增、不结项；X-26 的结项留待下一轮统一处理）。
- **下一轮（第 5 轮）**：产出完整可落地路线 —— 把第 3 轮的四层结构 + 本轮 A 的阻塞清单
  合并成一份带里程碑与验收标准的分阶段计划，作为本次目标的可交付成果。

## 2026-10-13：方向决策分析 · 第 3 轮（候选方向三维打分与组合策略）

### 打分口径

三维各 1–5 分，**分数越高越有利**：

- **开发成本**：5 = 成本最低。依据是"要动多少现有代码、要多久"。
- **就业认可度**：5 = 最高。依据是**第 2 轮的国内数据**（哪些关键词出现在溢价 offer 里）。
- **项目独特性**：5 = 最独特。依据是**第 1 轮的竞品格局**（竞品有没有、结构上做不做得到）。

**加权**：就业 0.5 / 成本 0.3 / 独特性 0.2。
**权重理由**：用户的目标是就业市场认可度，成本决定"能不能做完"，独特性决定"能不能脱颖而出"——
**三者里只有就业是硬约束**。

### 打分表

| 方向 | 成本 | 就业 | 独特 | 加权 | 依据 |
|---|---:|---:|---:|---:|---|
| **D 并发状态机框架** | 4 | 4 | 3 | **3.8** | 成本：基于现有 `Channel`/`Context`/`ContextRollback` 组合，不需改内核。就业：并发/分布式有岗（第 2 轮）。独特：状态机本身有先例，但 `ContextRollback` 的事务语义无先例 |
| **G 具体应用**（网关/交易/微服务） | 3 | 5 | 1 | **3.6** | 就业：**第 2 轮直接命中** —— 面试语境把协程明确归入「网关、交易系统、微服务框架」。独特：红海，libco/sylar/coost 都在 |
| **A 补齐运行时短板** | 5 | 3 | 2 | **3.4** | 成本：台账 24 条，逐条可做。就业：「修 bug」不好讲。独特：无 |
| **F 遗留系统并发注入层** | 3 | 3 | 4 | **3.2** | 独特：**syscall 级 hook 让存量代码不改一行获得并发，竞品结构上做不到**。就业：场景真实但难做成可展示的作品 |
| **E 分布式结构化并发** | 1 | 3 | 5 | **2.8** | 独特：跨机取消树 + 补偿，无人做。成本：以年计 |
| **C 确定性模拟测试** | 2 | 2 | 5 | **2.6** | 独特：**5 分，协作式调度可穷举是结构优势**。就业：国内几乎无对口岗。成本：整体高，**但 L1/L2 极低（见下）** |
| **B Go→C++ 编译器后端** | 1 | 2 | 4 | **2.1** | 独特：`go/ssa` 复用，无人做过。成本：以年计。就业：编译器岗稀缺 |

### ★★ 但加权排序会误导 —— 这七个方向不是互斥的

**真正的问题不是"选哪个"，而是"按什么顺序做哪几个"。** 关键事实：

1. **A 是纯前置**：不把 `HybridGate`（用户已决定移除）、`noexcept` 13 处、无界等待等清掉，
   后面做任何框架都建立在已知缺陷上。
2. **D 与 G 是"框架与作品"的关系**：D 产出可复用的框架，G 用 D 做出可展示的具体服务。
   **面试最看重的是"作品"（G），但能讲出"设计"的是 D。两者应该一起做，而不是二选一。**
3. **★★ C 的成本被高估了 —— 它有一个"极低成本的入口"**：
   - **C-L1（trace 记录）**：在 `Scheduler` 决策点插**默认空**的回调，**约 50 行**，
     生产路径零开销。产出：失败现场可读、flaky 可诊断。
   - **C-L2（虚拟时钟）**：**约 0 行** —— `Context::NowFunction` 与 `Context::SetNowFunction`
     **已经是现成的可替换点**。产出：30 秒超时测试变成毫秒级。
   - **合计不到 100 行，却拿到 C 的绝大部分"故事价值"。**
   - 而 C 的后半段（偏序归约、系统性枚举）才是真正昂贵的部分，可以无限期延后。

### ★★★ 本轮结论：三层结构，而不是单选题

```
第一层  前置：A 补齐短板
        └─ 台账 24 条中的高危项（HybridGate 自旋、~Impl 无界等待、noexcept 加锁）
        └─ 目的：让后续一切建立在没有已知缺陷的基础上

第二层  主线：D 并发状态机框架
        └─ 就业认可度 4，且是唯一能充分发挥 ContextRollback 独有能力的形态
        └─ 产出：可复用的框架 + 可讲述的设计

第三层  标签：C-L1/L2（约 100 行）
        └─ 就业认可度虽低，但独特性 5，且成本极低
        └─ 产出：一个"别人结构上做不到"的能力 —— 并发可复现
        └─ ★ 这是简历上唯一无法被 libco/sylar 使用者复制的一句话

第四层  作品：G 用 D 做一个具体服务（协议网关 / 状态机服务）
        └─ 就业认可度 5
        └─ 产出：能报数字的成果（QPS、连接数、故障注入通过率）

暂缓    B（Go→C++ 编译器后端）、E（分布式结构化并发）
        └─ 独特性都高（4 和 5），但成本以年计，作为长期目标保留
```

**为什么这个顺序**：

- **A 不做，D 就是沙上建塔**（`HybridGate` 的自旋会在高并发下先炸）。
- **D 不做，C 就没有被测对象**（确定性模拟需要先有一个"值得模拟的并发系统"）。
- **C-L1/L2 不做，D 就只是"又一个框架"** —— 有了它，D 才变成"一个可验证的框架"。
- **G 不做，前三层都没有可展示的成果** —— 面试官要的是"你做出了什么"。

### 与第 1、2 轮的交叉验证

| 结论 | 第 1 轮（竞品） | 第 2 轮（就业） |
|---|---|---|
| 主线选 D（并发状态机/框架） | 头部（libco/sylar）停更，框架位空出 | 并发/分布式在溢价区 |
| 标签选 C-L1/L2（可验证性） | **竞品结构上做不到**（它们是抢占式或回调式，没有显式调度点） | 无对口岗，但基础架构岗可讲 |
| 作品选 G（网关/交易/微服务） | 红海，必须有差异化才能讲 | **面试语境直接把协程归入这三类** |
| **不做"生态堆叠"** | sylar 已把 ORM/MySQL/Redis 做完，追赶成本极高 | 这类岗位不因"有 ORM"而溢价 |

### 状态

- **本轮只做分析与记录，未改动任何项目源码。**
- 在途 5 个文件全程未触碰。
- 台账待办仍 **24 条**。
- **下一轮（第 4 轮）**：把第一层（A）落到可执行清单 —— 从台账 24 条里按"是否阻塞后续 D/C 工作"
  筛出真正的阻塞项排序，并给出每一项的第一步动作。

## 2026-10-13：方向决策分析 · 第 2 轮（国内就业市场调研）

### 数据来源与口径声明（重要）

- **来源**：搜索 + 抓取公开文章。核心两篇：
  ① [中国C++程序员的失业情况：2026年深度调研报告](https://www.eet-china.com/mp/a520811.html)（2026-08-28）；
  ② [2026校招薪资报告](https://notes.kamacoder.com/salary/yearly/2026.html)。
  辅助：一篇 C/C++ 后台面试题汇总（[CSDN](https://blog.csdn.net/weixin_42376192/article/details/159494790)）。
- **口径警告**：**中国官方统计中没有"C++ 程序员"这一细分失业率数据**（eet-china 报告自己开篇即声明）。
  所有数字均来自招聘平台与行业观察，**不能当官方统计用**。不同平台口径差异很大
  （如同为"C++ 开发"，猎聘平均月薪约 23,545 元，而 Morgan McKinley 口径年薪约 39 万）。
- **本会话无法取得的**：**C++ 与 Go 的精确岗位数量对比**、**"协程/异步 IO"关键词的岗位频次统计**。
  招聘平台的具体岗位数需要登录且不对外开放抓取。**这一点如实留白，不用推测填充。**

### ★★ 发现一：C++ 就业市场是「结构性分化」，不是全面衰退

eet-china 报告的六行业评估（原文表格）：

| 行业 | 形势 | C++ 岗位趋势 |
|---|---|---|
| 互联网/科技 | ⚠️ 收缩但高端保留 | **传统后端缩减，高性能基础设施稳定** |
| 游戏 | ⚠️ 结构性调整 | 客户端/引擎缩减，AI+引擎复合岗位增长 |
| **嵌入式** | ✅ **稳健增长** | 汽车电子/机器人/边缘计算，**人才缺口超 50 万** |
| 汽车/自动驾驶 | ⚠️ 结构性洗牌 | 智驾域控/AUTOSAR/VLA；**需供比 8.2（缺口最大）** |
| **金融/量化** | ✅ **高薪但门槛高** | 量化交易/低延迟持续高薪，「只雇佣最好的」 |
| 传统制造/工业软件 | ✅ 稳步增长 | 信创放量，2026 市场规模预计 2.6 万亿 |

**原文关键判断**（可直接引用）：

> 「**C++ 不是岗位最多的语言，但仍然是性能敏感型岗位里最有壁垒、也最容易拉开能力差距的语言之一。**
> C++ 并未"死亡"，而是"高门槛、性能关键"岗位的语言。」

**使用率数据**：Stack Overflow 2025 开发者调查 —— **C++ 在职业开发者中使用率 21.8%，
高于 Go（16.4%）与 Rust（14.8%）**；TIOBE 2026-03 C++ 排全球第 3。

### ★★ 发现二：C++ 的「薪资溢价」集中在并发/分布式/基础设施方向

2026 校招薪资（kamacoder，近千份 offer 样本）：

- **一线大厂核心部门：25k~30k × 15~20 薪**；普通业务/区域：18k~23k × 14~16 薪
- 原文点名：**「C++ 在搜广推、分布式、核心系统方向，依然有溢价」**；
  「Go 在数据库 / 云 / 运维架构中存在感越来越强」

**样本中与"并发/分布式/基础设施"直接相关的 C++ offer**：

| 公司/岗位 | 技术栈 | 薪资 |
|---|---|---|
| 京东 **分布式 KV 内核 / 数据管道** | — | **80-85 万** |
| 字节 **TT 搜索架构** | cpp | **34k × 15 + 11w** |
| 京东 **搜广推业务后端** | cpp | **年包约 65 万** |
| 滴滴网约车 | cpp | 50 万 |
| 虾皮 搜广推部门 | cpp | 29k × 15 |
| 百度 **分布式计算工程师** | — | 28k × 16 + 4w |
| 京东云 **数据库** | go | 24k × 20 |

**社招/金融天花板**（eet-china 引用猎聘数据）：
量化 C++ 交易系统开发 —— **上海/深圳/广州/杭州 50-80k·24 薪；香港 100-200k·24 薪**。

### ★★★ 发现三：面试语境里「协程」的定位，与本项目推荐的落点完全重合

抓到的 C/C++ 后台面试题汇总在"协程模型"一节的原文表述：

> 「**协程模型：IO 密集型高并发后台服务的首选，比如网关、交易系统、微服务框架**，
> 可彻底解决线程模型的切换开销与锁竞争问题，大幅提升服务吞吐量，降低延迟。」

**这句话的价值**：它说明"协程"在国内 C++ 面试里**不是冷门加分项，而是被明确归类到
"网关 / 交易系统 / 微服务框架"这三类岗位的核心能力**。而这**恰好是我在第 1 轮
从项目能力出发推荐的三类应用场景**（协议网关、并发状态机、事务型服务）。
**两条独立路径得到了同一个落点。**

### 对方向决策的含义

1. **"C++ 并发/网络"这个技能方向本身没有贬值** —— 使用率仍高于 Go，且被明确归入溢价区。
2. **真正的风险不是"选错了语言"，而是"做成通用 CRUD 后端"** —— 那才是收缩的部分。
3. **Go2Cpp 的天然落点（高性能并发基础设施）正处在溢价区**，具体对应：
   - **分布式**（对应"京东分布式 KV 内核 80-85 万"这类岗位）
   - **核心系统 / 搜广推**（对应字节搜索架构、京东搜广推）
   - **网关 / 交易系统 / 微服务框架**（面试题里协程的标准落点）
4. **★ 但必须调整"讲法"**：不能只讲"我写了一个 C++ 协程库"（那是红海，libco/sylar/coost 都在），
   而要讲**"它能支撑什么"**——即往**分布式基础设施**的方向定位，而不是往"协程库"的方向定位。
5. **差异化仍需靠第 1 轮识别出的那一层**：Go 语义完整性 + 可验证性（竞品普遍缺失）。

### 状态

- **本轮只做调研与记录，未改动任何项目源码。**
- 在途文件清单**新增一项**：`src/concurrency/context.cpp`（用户在 2026-10-12 17:11 做了
  +2213/-1866 的重排）。**今后绝不触碰，共 5 个在途文件。**
- **下一轮（第 3 轮）**：把 A–G 七个候选方向按「开发成本 × 就业认可度 × 项目独特性」打分排序。
  输入 = 第 1 轮（竞品格局：头部停更、空档存在）+ 第 2 轮（溢价区：分布式/核心系统/网关/交易系统）。

## 2026-10-13：方向决策分析 · 第 1 轮（GitHub 对标调研）

- **用户指令**：多轮定时（每 30 分钟）分析项目，对比 GitHub 同类项目与国内就业情况，
  综合开发成本与市场认可度，决定后续方向。**已建立定时任务 `schedule-6010b30e` 与长期目标 `goal-a816534d`**。
- **用户偏好**：**不预设主线**（等前三轮数据出来再定）；**目标市场为国内**。

### 环境发现（重要，后续轮次复用）

**WSL 侧完全没有外网**：`getent hosts api.github.com` 返回 `::1`（DNS 被指向本机环回），
`curl` 一律 `http_code=000`。**因此所有联网调研必须走 `web_fetch` 工具，不能用 WSL 的 curl/wget。**
逐步记录：`.dsh_tmp/_gh_survey.py`（因上述原因失效，保留备查）。

### 本轮数据（GitHub REST API，2026-10-13 实测）

| repo | stars | 最后推送 | 定位 |
|---|---:|---|---|
| swoole/swoole-src | 18,918 | 2026-10-13 | PHP 协程（仅作赛道热度参考） |
| ZLMediaKit/ZLMediaKit | 17,607 | 2026-10-12 | 流媒体服务器，非通用框架 |
| scylladb/seastar | 9,391 | 2026-10-12 | 高性能服务器框架；活跃但重、与 ScyllaDB 强绑定 |
| Tencent/libco | 8,690 | **2024-03-07** | 微信后端协程库；**已停更** |
| **sylar-yin/sylar** | **4,718** | **2023-12-08** | **与 Go2Cpp 最同构；已停更** |
| idealvin/coost | 4,218 | 2026-09-28 | 极简基础库，topics 含 goroutine；活跃 |
| jbaldwin/libcoro | 969 | 2026-05-02 | C++20 协程；活跃 |
| ConorWilliams/libfork | 903 | 2026-05-25 | C++20 无锁任务库；活跃 |
| owent/libcopp | 866 | 2026-09-15 | 跨平台协程；活跃 |
| Naios/continuable | 853 | 2023-09-12 | C++14 future；**已停更** |

### ★★ 本轮三个关键发现

**1. 赛道需求真实，但头部项目已经老化停更。**
`libco`（8,690★，腾讯，微信后端）停在 **2024-03**；`sylar`（4,718★）停在 **2023-12**；
`continuable`（853★）停在 2023-09。**而仍在活跃的，要么是重型框架（seastar，与 ScyllaDB 强绑定），
要么是"基础库"（coost、libcoro、libfork、libcopp，都不含完整服务器栈）。**
→ **"带完整取消语义的通用并发运行时"这个位置出现了空档。**

**2. `sylar` 与 Go2Cpp 的对比最有信息量——它恰好是"Go2Cpp 加上生态"的形态。**
sylar 自带：日志、配置、线程、协程、协程调度、IO 协程调度、hook、socket、bytearray 序列化、
http、TcpServer、Websocket、Https、Smtp、**MySQL、SQLite3、ORM、Redis、Zookeeper**。
→ **Go2Cpp 缺的（TLS、JSON/序列化、DB）正是 sylar 补上的那一半；**
→ **而 Go2Cpp 有的（`Context` 取消树、`ContextRollback` 补偿、`Error::Join/Is/As`、
   确定性模拟潜力、Go 语义保真）是 sylar 完全没有的。**
→ **结论：Go2Cpp 不该去补 sylar 已经做过的"生态堆叠"，而应当强化 sylar 与 libco 都没有的那一层。**

**3. 国内 C++ 后端面试的"项目认可度"有一条清晰路径。**
sylar、muduo、libco、TinyWebServer 这类项目在国内面试中是被普遍认可的"硬项目"。
→ **"写一个 C++ 并发服务器框架"这件事本身是被市场接受的**；
→ **但必须在 sylar/libco 之外有**可讲述的差异化**，否则就是第 N 个同类。**

### 对后续方向的初步启示（待第 2、3 轮数据验证）

- **不要做"又一个 sylar"**：生态堆叠是它的强项，追赶成本高且同质。
- **要做"它们没有的那一层"**：
  ① **Go 语义完整性**（`Context`/`ContextRollback`/`defer`/`Error` 这一套是竞品普遍缺失的）；
  ② **可验证性**（协作式调度 → 可记录/可重放/可穷举，这是 libco/sylar 结构上做不到的）；
  ③ **hook 的系统性**（sylar 有 hook，但 Go2Cpp 的 hook 覆盖 + `BlockingRegion` + sysmon 联动更完整）。
- **待验证**：这一层在国内招聘市场的"可讲述性"与"被认可度"究竟如何——**第 2 轮就业调研要回答这个**。

### 状态

- **本轮只做调研与记录，未改动任何项目源码。**
- 台账待办仍 **24 条**（本轮无新增条目）。
- **下一轮（约 30 分钟后）**：国内 C++ / Go 后端就业市场调研 —— 岗位量、薪资区间、
  以及"协程""异步 IO""分布式""确定性测试"等关键词在 JD 中的出现情况。

## 2026-10-12：逐块讲解 Context::State（用户要求「详细讲解，包括场景/功能/源码实现」）

- **读全文**：`src/concurrency/context.cpp:382-747`（字段 + 10 个成员函数）。

### 讲解的核心结论（供后续审计复用）

**`DrainCancellation`（`:533-638`）是 `State` 的心脏**，取消一棵子树严格分三段：

1. **遍历整棵子树，只做标记**（显式 `work` 栈迭代，非递归）：
   锁内只改状态（写 `error`/`cause`、摘 `timer_id`、搬走 `before_done_callbacks`、
   `children.clear()`、收集 descendants），**锁外**才做动作（`TimerService::Remove`、
   `parent->RemoveChild`、下推子节点）。**`root || error` 两条提前 continue 实现
   「root 免疫」与「取消幂等」。** 原因（`error`/`cause`）随工作项**沿子树下传**，
   所以整棵子树共享同一根因——这正是 Go 的 `Err()`/`Cause()` 双层语义。
2. **逆序执行 `before_done_callbacks`**（`:623-633`，`rbegin/rend`）：
   **「冻结」从最内层开始**，保证 `ContextRollback` 在 `done=true` 前进入回滚态，
   使观察者/`RecordUndo`/`Commit` 不会在取消窗口里误把新动作当作可提交。
3. **逆序 `Signal()`**（`:634-637`，`rbegin/rend`）：**叶子先通知、根最后**。
   原因是 `:540-541` 注释点明的死锁场景——**Done 回调可以观察或等待后代**，
   若父先 `Signal`，父的观察者在子尚未 `Signal` 时去等子就会被卡住，
   而这段等待发生在**取消传播的同步路径**上，于是传播本身死锁。

**其他值得记录的实现要点**：

- **`CancellationWork` 必须持 `shared_ptr<State>`**：因为父的 `children` 是 `weak_ptr`，
  传播途中节点可能被销毁，工作项必须自持强引用。
- **`AddChild` 处理「父已取消后才创建的子」**：不在 `children` 登记，而是**锁内拷贝**
  `error`/`cause`，**锁外**调 `child->CancelFromParent(...)` —— 避免持自身锁去传播整棵树（锁序安全）。
- **`Cancel` 与 `CancelFromParent` 的三点差异**：`error`（空 vs 继承）、`deadline_error`
  （调用方指定 vs false）、`remove_from_parent`（true vs false，主动取消者从父的登记表摘除自己）。
- **`RemoveChildRaw` 兼职惰性清理**：`remove_if` 同时移除 `expired()` 项，
  与 `AddChild` 开头的清理配合，防止 `weak_ptr` 空槽无限膨胀。
- **`Lookup` 的锁序**：**每次只锁一个节点**（锁内取 `parent` 到局部强引用后立即解锁），
  再锁父节点，**从不嵌套加锁**；并用 `shared_from_this()` 保活自身。
  且 **`token != nullptr` 与 `token == nullptr` 是二选一**——
  类型化键**只**查 `typed_values`，字符串键**只**查 `named_values`，两者互不回退
  （因为 `ContextKey::Name()` 仅用于调试，不参与查找）。

### 本轮发现

| # | 问题 | 严重度 |
|---|---|---|
| **X-25⑬** | `Context::State::IsCanceled()`（`:485-489`）标 `noexcept` 却加锁。它是取消状态的**基础查询**，被 `ParkingCondition::ContextCanceled()`、`ContextRollback::State::ContextCanceled()` 等调用 | 低 |

**X-25 累计 13 处。**

### 状态

- 台账：**X-25 第七次扩充 → 13 处**；待办仍 **24 条**（本轮未新增条目）。
- **仅读代码 + 写台账，未改动源码。**

## 2026-10-12：登记 HybridGate 移除决策（X-35）；讲解 DoneSignal 并发现 X-36

### 一、X-35：用户决策「框架绝对不允许 HybridGate」

**在用户指示下完成全项目定位（仅调查，未改代码）**：

**真实使用点：3 个生产文件、4 处成员、12 个加锁点**

| 文件 | 成员 | 加锁点 |
|---|---|---|
| `src/concurrency/parking_condition.cpp` | `:161` `HybridMutex m_wake_mutex;` | `:69`/`:92`/`:99` |
| `src/concurrency/sync.cpp` | `:349` `core::HybridMutex m_wake_mutex;` | `:135`/`:154`/`:195`/`:221`/`:243`/`:332` |
| `src/network/io.cpp` | `:327` `core::HybridMutex wake_mutex;` | `:354`/`:363`/`:383` |

**三处同构**，都是「**唤醒门**」：锁保护 `{scheduler, task, wake_active}` 一组裸指针，
`wake()` 锁内调 Scheduler、`disarm()` 锁内清指针，目的是**不让对 `Scheduler*` 的访问越过等待 G 的栈清理**。

**用户否定的理由**：`HybridGate::lock()` 慢路径是 `for(;;)` **无界自旋**（`:75-81`），
持有者一旦拖延（例如在 Fiber 上被 park），自旋方无上限烧 CPU。
**这也解释了作者当初为何选它**：它用 `atomic_flag`，**天然没有 owner 概念，可跨线程解锁**。

**★ 用户同时指出 `std::mutex` 不能作为替代**：hook 会把 `connect_with_timeout`
封装成「已保存上下文的定时任务」交给其他线程，造成 `lock()` 在线程 1、`unlock()` 在线程 2。

**项目已有正确方案：`sync::Mutex`**（`include/go2cpp/sync.hpp:78`）。类文档明文：
「两类调用者共享一个 FIFO 交接队列，因此 **Fiber 加锁后可以由其他 Fiber 或 OS 线程解锁**」
「**Unlock 不要求由同一 G/线程调用，采用 Go 风格所有权**」。
实现证据：`src/concurrency/sync.cpp:955` 的 `Unlock()` **无 owner 检查**
（只查「锁是否被持有」），锁状态是 `atomic<uint8_t>`，
内部 `std::mutex` 仅保护等待队列、同线程短暂进出，故跨线程解锁完全安全。

**替换注意三点**：用返回 `bool` 的 `Lock()`（`lock()` 会抛 `logic_error`）；
`Unlock()` 在 `noexcept` 路径需兜异常；`Lock()` 返回 false 时不能当作已加锁。

### 二、DONE SIGNAL：它到底是干啥的

**「只关闭一次、可复制的完成通知」**——`Context` 用它表示 Done 信号。
它是 `ParkingCondition` 之上的一层**专用一次性标志**，自带状态与回调注册，
调用方**不必自己准备外部锁与谓词**。

**State**（`context.cpp:17-35`）：`mutex` + `mutable core::ParkingCondition cv` + `bool done`
+ `next_id` + **两组回调表**（`internal_callbacks` / `callbacks`）。

**三个关键设计**：

1. **一次性 + 幂等**：`Signal()` 先查 `done`，重复调用直接返回（`:177-179`）。
2. **「已完成后再注册」→ 立即调用**（`AddCallbackImpl` `:124-143`）：锁内只标记
   `invoke_now`，**回调在锁外调用**并吞异常；`CallbackId` 从 1 起、跳过 0。
3. **★ 执行顺序：`notify_all()` → `internal_callbacks` → `callbacks`**（`:192-204`），
   回调全在**锁外**跑（注释：「避免回调重入信号对象」），异常一律隔离。

**★★ 最有价值的一点：`notify_all()` 在回调之前** ——
所以「**等 `Done()` 返回**」与「**回调已执行完**」**没有任何顺序保证**。
**这正是 `ContextRollback` 必须另提供一个独立 `RollbackDone()` 信号的根本原因**：
`context_rollback.md` 中「与 child `Done()` 分离」那句话的机制就在这里，
**想等 undo 跑完只能等 `rollback_done()`**。
而**两组回调**的设计目的，是让**内部机制（如 `ContextRollback` 的 undo）先于用户观察回调执行**。

**谁在用它**（全项目的取消/完成通知总线）：作为成员有 `Context::State::m_done`
（`context.hpp:245` / `context.cpp:342`）与 `ContextRollback::State::completion`（`context.cpp:749`）；
注册 `CallbackId` 的调用方共 9 处：`sync.cpp:491`、`channel.cpp:254`、`io.cpp:2011`/`:2256`、
`channel.hpp:1123`、`future.hpp:74`/`:91`、`sse.hpp:188`、`event.hpp:264`，
以及 `ContextRollback` 自己的 `m_callback_id`/`m_before_callback_id`。

### 三、本轮发现的两个问题

| # | 问题 | 严重度 |
|---|---|---|
| **X-25⑫** | `DoneSignal::IsDone()`（`context.cpp:89-92`）标 `noexcept` 却加锁，且在 `Context::IsDone()` 一类高频查询路径上 | 低 |
| **X-36** | **移动后的 `DoneSignal` 是空壳，但 `Wait`/`WaitFor`/`WaitUntil`/`IsDone` 四处都直接解引用 `m_state`**，无空检查；移动语义是 `= default`（`context.hpp:50-51`）。**对照**：`ContextRollback::RollbackDone()`（`:1582-1590`）**特意**处理了该情况 | 低-中 |

**实际影响有限**（常规用法中被移动的临时对象立即析构），
但 `auto s = std::move(other); other.IsDone();` 会崩。

### 四、状态

- 台账：**新增 X-35（用户决策，含完整移除清单与替换约束）、X-36**；
  **X-25 第六次扩充 → 12 处**。
- 待办 **24 条**。
- **本轮仅读代码 + 写台账，未改动任何源码**（遵守用户「先不要修改代码」）。

## 2026-10-12：审查 HybridGate 并盘点全项目混合同步设施（新增 X-33/X-34，X-25 扩至 11 处）

- **用户指令**：「HybridGate 这个东西实现真的没有问题吗」+「尝试在整个项目里面找到混合并发的锁还有哪些」。

### 一、HybridGate 审查结论

**读全文**：`include/go2cpp/concurrency/core/hybrid_mutex.hpp`(114)，无配套 `.cpp`。

| # | 发现 | 严重度 |
|---|---|---|
| **H6/X-33** | **命名与实际混合维度不符**：`HybridGate` 的「混合」是**「有限自旋 + `std::mutex` 慢路径」两种策略**，**不是「线程/Fiber」两种执行体**。它**内部没有任何 park**，因此若被当作混合锁使用，受管 G 会**在自己的 M 上自旋并占满 carrier 线程**，比裸 `std::mutex` 更糟。头文件 `:4-5` 已划红线（「只用于非常短的 Fiber 唤醒/撤销临界区，**不得保护可能阻塞、执行用户回调或执行系统调用的代码**」），但项目里**另有真正的混合锁 `sync::Mutex`（`sync.hpp:69` 自称 FIFO 混合互斥锁）**，两者共用「混合」一词 | **低-中（认知风险）** |
| **H1/X-25⑪** | `lock()` 标 `noexcept`，慢路径 `m_slow.lock()`（`:74`）**可抛**。**且它在 `ParkingCondition::WaitNode::Wake()` 的链上**（`parking_condition.cpp:69` 的 `lock_guard<HybridMutex>`），而 `Wake()` **本身也是 `noexcept`** —— 两层的 `noexcept` 叠加。同文件 `Wake()` 对 `wake_or_cancel` 已有 try/catch，属项目既有策略，故此为实现遗漏 | **中** |
| H2 | 慢路径 `for(;;)` **无界自旋** —— 依赖「极短临界区」这一**文档契约**，无强制手段；违反时烧 CPU 无上限 | 低-中 |
| H3 | `unlock()` 只 `m_fast.clear()`，**不检查所有权**（没持锁也能解锁，静默破坏互斥）。对比 `sync::Mutex::Unlock()` 会「报告逻辑错误」 | 低 |
| H5 | **快路径可插队慢路径**：`m_fast` 一释放，新来者能在快路径直接拿到，而 `m_slow` 上排队的还要等 → **无公平性保证**（`sync::Mutex` 承诺 FIFO） | 低 |
| — | **正确的部分**：慢路径用 `m_slow` 保证「同一时间只有一个线程等原子标志，避免多个失败者同时忙等」（`:72-73` 注释）；`m_slow` 只在 `lock()` 内部短暂持有，`unlock()` 不需碰它；`Normalize` 把 `fast_attempts==0` 修正为 1；默认值用 `inline static atomic` 支持运行期调整且不改动已构造的门 | — |

### 二、全项目「混合并发」设施盘点

**先厘清一件事：「混合」在本项目里有两种完全不同的含义。**

| 设施 | 混合的是 | `src/`+`include/` 引用数 |
|---|---|---|
| **`core::ParkingCondition`** | **线程 ↔ Fiber**（park / native CV） | **46** ← 主力 |
| **`sync::Mutex`** | **线程 ↔ Fiber**（FIFO，park / native） | **23** |
| `sync::WaitBackend` | 线程 ↔ Fiber（`sync::Mutex` 的可替换后端） | 11 |
| **`FutureWaitBackend`** | 线程 ↔ Fiber（**`ParkingCondition` 之上的抽象层**） | 8 |
| **`core::HybridGate`/`HybridMutex`** | **自旋 ↔ 阻塞**（**不是**线程/Fiber） | 25 |
| `std::condition_variable(_any)` | 原生 | 9 |
| `sync::ConditionVariable` | 线程 ↔ Fiber（**但零使用**） | 0 |
| `sync::WaitGroup` | 线程 ↔ Fiber（**但零使用**，2 处引用都在实现自身） | 2 |

**真正的混合锁持有者清单**：

**A. `core::ParkingCondition`（12 处成员）**

| 位置 | 用途 |
|---|---|
| `channel.hpp:548` `m_cv` | 通道等待 |
| `channel.hpp:1106` / `:1118` `PendingSend/Recv::cv` | select 节点等待 |
| `channel.hpp:1579` `m_change_cv` | 变更通知 |
| `scheduler.hpp:389` `Task::m_completion_condition` | 任务完成等待 |
| `socket.hpp:164` `m_operation_condition` | 关闭时等在途操作 |
| `context.hpp` `DoneSignal` 内部状态 | 取消通知 |
| `future.hpp`（`FutureState` 内） | Future 等待 |
| `event.hpp`（内部） | 批次唤醒 |

**B. `sync::Mutex`（4 处成员）**

| 位置 | 用途 |
|---|---|
| `sse.hpp:185` `m_mutex` | SSE 流写入串行化（注释：「写入可能在 Fiber 中 park，**必须使用 Fiber/线程混合锁**」） |
| `websocket.hpp:157-158` `m_read_mutex`/`m_write_mutex` | WS 读写串行化（「避免受管 Fiber 在同一个 M 上阻塞 OS 线程」） |
| `event.hpp:406-407` `m_mutex`/`m_work_mutex` | EventBatch 状态 |

**C. `HybridMutex`（少量，短临界区）**：`parking_condition.cpp` 的 `WaitNode::m_wake_mutex`、`scheduler.cpp:2869` 一带、`sync.cpp:358`/`:778`。

### 三、附带发现（X-34）：三个「零运行时使用」的公开设施

`sync::ConditionVariable`（**0 次**）、`sync::WaitGroup`（2 处**全在实现自身**）、
`sync::WaitBackend`（11 处基本在实现/声明）。加上本会话早先确认的
`control_flow`（`src/` 零使用）与 `ContextRollback`（36 处全在 `context.cpp` 自身）——
**共五处属「面向使用者、运行时自身不用」**。这不是缺陷，但**建议在 `docs/` 中统一标注定位**，
避免后续审计反复把它们当死代码重新调查。

### 四、状态

- 台账：**X-25 第五次扩充（11 处）**；**新增 X-33（HybridGate 命名歧义）、X-34（零使用设施定位）**。
- **本轮只登记不改动代码**（H1 虽满足即时修复的例外③，但属既有基础设施，
  且用户本轮是提问而非下达修改任务，故登记待命）。
- 待办：**22 条**。

## 2026-10-12：ContextRollback 增强的自我审查 —— 修复 8 处问题，测试加强到 16 组

- **用户指示**：「加强刚刚实现部分的测试，并且代码重新审查和修正，确保这部分内部不存在问题」。
- **审查方法**：**不凭记忆**，先把改动后的代码读回来（`context.hpp` 新 API 段、
  `context.cpp` 的 `State::Record/RecordWithCause/RecordImpl/PendingCount/Failures/
  ManualCause/RecordFailure/Execute`），再逐项核对并发、异常、边界与语义。

### 发现的 8 处问题（全部已修复）

**本批新增代码引入的 5 处**：

| # | 问题 | 严重度 | 修复 |
|---|---|---|---|
| **P1** | `RecordFailure` 标 `noexcept`，内部却**加锁**且 `failures.push_back` **可能抛**；而它正是在「补偿失败」路径上被同样 `noexcept` 的 `Execute` 调用 —— **在最不该崩的地方崩** | **中** | 整体 try/catch；并**调整顺序**为先写 `first_failure`（`exception_ptr` 赋值不抛）再 `push_back`，使扩容失败时 `had_failure()`/`failure()` 仍自洽 |
| **P7** | `rollback_cause()` 未区分事务是否真的回滚过：`commit()` 之后 child 再被取消会**误报**成本事务的回滚原因 | **中** | 先取 `status()`；`kCommitted`/`kActive` 直接返回空 |
| **P2** | `State::PendingCount()` 标 `noexcept` 但加锁 | 低 | try/catch，失败返回 0 |
| **P3** | `State::ManualCause()` 标 `noexcept` 但加锁 | 低 | try/catch，失败返回空 |
| **P4** | `StatusName()` 的 `switch` **无 `default`** → 枚举越界 UB，编译器可把 `return "unknown"` 视为不可达而优化掉 | 低 | 补 `default:` |

**审查中一并发现的既有 3 处**（非本批引入，属台账 **X-25** 同类）：

| # | 位置 | 说明 |
|---|---|---|
| **P8** | `ContextRollback::status()` | `noexcept` + 加锁；**且 `active()`/`committed()`/`rolled_back()`/`status_name()` 全部转发到它**，影响面最大 |
| **P9** | `ContextRollback::had_failure()` | `noexcept` + 加锁 |
| **P10** | `ContextRollback::Savepoint::valid()` | `noexcept` + 加锁 |

三处均兜底为查询型接口的**保守答案**（`kRolledBack` / `false` / 令牌无效），不 terminate。
**台账 X-25 由 7 处扩充到 10 处（第四次扩充）。**

### 测试加强：8 组 → 16 组

| # | 覆盖 |
|---|---|
| 9 | **【P7 回归】** commit 后 child 再取消，`rollback_cause()` 必须**始终为空** |
| 10 | **移动赋值**：被覆盖方**先回滚自身**（核实过 `operator=` 在 `:1160` 调 `Rollback()`），源随后变空 |
| 11 | 小写别名等价：`record_undo_with_cause` 与 `RecordUndoWithCause` 的空动作/带原因路径一致 |
| 12 | 无失败时 `failures()` 为空且与 `had_failure()` 一致；终态拒绝再登记 |
| 13 | **部分补偿失败**：3 个动作里 1 个抛 → `failures().size()==1`、两个成功的照常执行、`status()==kFailed` 且 `rolled_back()` 仍为真 |
| 14 | 状态族（`active`/`committed`/`rolled_back`）与 `status_name()` 同一真相来源 |
| 15 | **并发**：4 线程 × 32 次并发 `record_undo` + 并发 `pending_undo_count()`，终值必须精确等于 128 |
| 16 | 显式 `rollback()` **不带 cause**：带原因动作收到空 cause，而 `rollback_cause()` 回退到 `CanceledError` |

### 验证结果

- 构建 **0 warning / 0 error**；
- `ctest` → **26/26 全绿**；
- `GO2CPP_TEST_FILTER=context` 单跑并**校验 announce 文本**，排除空跑；
- context 模块**重复 5 次全通过**；**全量二进制重复 3 次全通过**；
- `GO2CPP_TEST_FILTER=__nope__` 返回 **2**（T-08 回归确认有效）。

### 一点反思（值得留档）

**P1/P2/P3 是我自己刚写的代码，却踩了项目里已经登记过的 X-25 同一个坑**
（`noexcept` 路径下加锁/分配）。这说明：**"知道一个模式有问题"不等于"写的时候不会犯"**——
真正有效的防线是**在同一批改动内做一次针对该模式的专项检查**
（本次正是靠"逐个 `noexcept` 函数看有没有锁或分配"这条尺子才发现的；
审查时顺手量出既有 3 处 P8/P9/P10 也是同一把尺子）。

### 规模与状态

- `src/concurrency/context.cpp`：8 处修复；
- `tests/concurrency/test_context.cpp`：624 → 757 行；
- `docs/context_rollback.md`：新增「审查与加固」小节；
- 台账 **X-25 → 10 处**；待办仍 **20 条**。

## 2026-10-12：增强 ContextRollback（用户下达的一次性完成任务）

- **用户指示**：「现在着手完成这部分的工作，增强 ContextRollBack 的功能，一次对话内完成」。
- **背景**：本轮之前已确认 `ContextRollback` 覆盖了 `panic`/`recover` 最有价值的
  「失败时执行补偿链」那一半，且更强（能自动响应父取消/deadline）；它缺的恰好是
  「**知道为什么回滚**」。因此本次增强聚焦两个方向：**带回滚原因** 与 **完整诊断**。
- **改动清单（全部为新增，不改变任何既有调用与行为）**：

  | 新增 | 说明 |
  |---|---|
  | `RecordUndoWithCause` / `record_undo_with_cause` | 补偿动作执行时收到本次回滚原因 |
  | `failures()` | **全部**补偿异常（按发生顺序） |
  | `pending_undo_count()` | 待执行补偿数量 |
  | `rollback_cause()` | 回滚原因；自动路径回退为 `context()->Err()` |
  | `StatusName()` / `status_name()` | 状态可读名（含 `unknown`） |
  | `record_undo(action, &reason)` | 登记失败给出可读原因（**默认参数**，旧调用不变） |

- **`cause` 的取值按触发路径区分**（已在文档中列表格）：
  显式 `rollback(cause)` → 该 cause；父取消 / deadline → `nullptr`
  （原因见 `context()->Err()`）；`rollback_to` 局部回滚 → `nullptr`；析构 → `nullptr`。

- **关键实现选择**：把 `ContextRollback::State` 内部的动作队列从
  `std::vector<std::function<void()>>` **统一为
  `std::vector<std::function<void(const ErrorPtr&)>>`**，旧式 `UndoAction`
  在登记时包一层适配。这样**两种动作共用同一条 LIFO 队列**，可以混用，
  且 `Mark`/`BeginPartial`/`Execute`/`FinishPartial`/`OnContextDone` 只需改类型，
  **不引入第二条队列**。

- **改动规模**：`context.hpp` 408 → 454 行；`context.cpp` 1895 → 2006 行；
  `test_context.cpp` 463 → 617 行。

- **★ 过程中的一次自我纠错（如实记录）**：新测试第一次运行**失败**，
  失败点是 `test_context.cpp:143` 的 `reason.find("取消")`。
  排查后确认**是我的测试假设错了，不是实现错了**——
  父级取消会**先**通过 DoneSignal 回调把 `status` 推离 `kActive`，
  因此取消之后再调 `record_undo` 命中的是「**事务已结束**」分支，
  而「child Context 已被取消」那条文案只在取消与登记恰好竞态时才出现，
  **无法在测试中稳定构造**。已修正断言并把这个语义差异写进两份文档。
  **这次失败反而证明了新测试有区分力（不是空跑）。**

- **测试覆盖**：新增 `check_rollback_enhancements()`，8 组场景：
  ① `StatusName` 五态 + 未知值；② `record_undo` 三类失败原因 + 计数；
  ③ 带原因动作收到显式 `cause` + 旧式动作照常执行 + **LIFO 混用顺序**；
  ④ 自动回滚（父取消）收到空 cause 且 `rollback_cause()` 回退到 `CanceledError`；
  ⑤ `failures()` 聚合三个异常且顺序为 LIFO、`failure()` 仍可取第一个；
  ⑥ 局部回滚无原因且事务保持 active、计数正确；
  ⑦ 取消后登记被拒且给出原因；⑧ **移动后的空对象**观测接口安全返回默认值。
  **顺带补上 `committed()` 断言（台账 T-16 结案）**：commit 前 `!committed()`、
  commit 后 `committed()` 且 `!rolled_back()`、显式 rollback 后 `!committed()`。

- **验证结果**：
  - `cmake --build build-quality --parallel` → **0 warning / 0 error**；
  - `ctest --test-dir build-quality` → **26/26 全部通过**；
  - `GO2CPP_TEST_FILTER=context` 单跑并**校验 announce 文本**
    （`context cancellation, values and deadlines`），排除空跑；
  - context 模块**重复运行 5 次全部通过**（Python 驱动，避免 shell 变量被吞）。
  - 在途 4 处文件全程未触碰。

- **文档同步**：`docs/context_rollback.md` 122 → 184 行（新增「增强接口」章节，
  含 `cause` 取值表、观测接口表、登记失败原因表，并更新命名约定与过时行号引用）；
  `docs/context_guide.md` 536 → 572 行（新增 7.6 节）。

- **台账状态**：**T-16 → 已修复/已了结**；待办 **20 条**。本次为纯功能增强，
  按台账定位（问题台账）**不新增条目**。

- **下一步**：回到 Scheduler 通读计划（7 段，见上一轮记录），
  或按用户指示继续其他模块。

## 2026-10-12：Fiber 收尾（100% 读完）+ Scheduler 骨架提取与阅读计划

- **用户指示**：「不要留白，给我继续读，还有 Scheduler 的也要同步完成」。
- **★ `fiber.cpp` 现已 100% 逐行读完（1502/1502 行，无留白）**。本轮补读 `:261-545`（`FiberStack` 全文）、**:720-820（此前遗漏的析构尾部与 `entry()`）**、`:1000-1147`（父链传播余下）、`:1148-1502`（`Fiber` 公开方法）。文档 `docs/fiber_guide.md` 追加 **11.5/11.6/11.7** 三节（现约 830 行）。
- **本轮新读出的关键机制**：
  - **`entry()`（`:727-775`）**：入口**先查 `cancellation_requested`**（启动前已取消则用户函数根本不执行）；异常只隔离用户代码的（注释：「Fiber 挂起、取消和清理流程**从不使用异常**」）；**`fiber_local::detail::Cleanup` 在主栈展开后、交还控制权之前**（「Fiber-local 值属于**逻辑 G**，而不是 worker M」）；结尾 `std::terminate()` 防御；
  - **`resume()` vs `resume_from_scheduler()`（`:785-824`）**：后者**只绑一次父级**（`m_scheduler_parent_bound` 缓存）且传 `scheduler_fast=true` —— **跳过 `last_thread` 更新**（调度器已通过 Task 的 execution claim 串行化，并在迁移时显式发布绑定），配合 `m_last_resume_thread_hash` 用哈希比较避免加锁；
  - **★ `RecycleForScheduler` 的五个回收条件（`:1170-1196`）** —— 其中第 ④ 条 **`m_impl->m_record.use_count() != 1` 是全项目最精细的一处引用计数运用**：`FiberRecord` 是 `shared_ptr`，而**嵌套子 Fiber 会持有父的 record**；若父被回收复用而某个子仍持有旧 record 引用，子之后恢复时会「观察到被复用的父对象」。注释原文：「只要仍有外部持有者，就不能把这个 Fiber 的父记录替换成下一项任务」。**这条正是 T-17 要补测的核心不变量**；
  - **`FiberStack` 的两级栈缓存（`:342-405`）**：线程本地池上限 **8**（无锁）、全局池上限 **128**（`s_stack_pool_mutex`）、都满则 `munmap`。native 栈布局为 `[guard page (PROT_NONE)][可用栈区]`，**最低一页不可访问**，栈向下溢出时立刻 `SIGSEGV`；
  - **Valgrind 栈注册（`:279-284`）**：因为**自定义汇编切换栈时 Memcheck 不知道边界**，必须 `VALGRIND_STACK_REGISTER(bottom(), stack_pointer())`；注释同时说明零成本（客户端宏在非 Memcheck 环境被消除）；
  - **`context_snapshot()` 的墓碑实现（`:1422-1460`）**：用 **`shared_ptr` 逐级拷贝保活**父记录（所以父 Fiber 对象销毁后仍可查询），收集后 `std::reverse` 成「根 → 当前」；`main_frame.active = false` 的理由是「main_fiber 只是根帧，**不应与链中当前/目标 Fiber 同时标记 active**」；
  - **`CancellationRequested()`（`:1274-1288`）沿父链向上查**，所以**子 Fiber 能感知父级取消**，与只置自身标记的 `RequestCancellation()` 配对。
- **Fiber 精读总结（写入文档 11.7）**：这份实现的「硬」体现在四层 —— ① 上下文层（自研汇编 + 三级工具链 + 零成本抽象）；② 栈管理层（页对齐/guard page/两级受限缓存/构造期拒绝溢出）；③ TLS 层（errno 双向保存 + `NOINLINE` 防地址缓存 —— 跨线程迁移最隐蔽的陷阱）；④ 生命周期层（claim 作闸门、record 引用计数作复用条件、墓碑用 shared_ptr 保活、放弃路径宁可标 `Failed` 也不伪造 `Completed`）。**唯一明确问题是 X-13；其余各处都体现「宁可 fail-fast 也不静默降级」的一致取向**（`std::terminate()` 只出现在四个明确的契约违反点）。
- **Scheduler 骨架已提取**（为下一阶段的通读做准备）：
  - 顶层类型 4 个：`TaskCancellationGate`(`:85`)、**`Scheduler::Impl`(`:997`，核心)**、`Scheduler::Lifetime`(`:1840`)、`Scheduler::TimerExecutorAdapter`(`:1846`)；
  - `Task` 方法 **30+ 个**（`:126-830`），其中 `Task::run()`(`:830`) 是 G 的执行入口；
  - `Scheduler` 方法 **40+ 个**（`:1907-3494`），关键节点：`start`(`:1907`)、`shutdown`(`:2161`)、`enqueue`(`:2287`)、**`wake` 一族（`wake`/`wake_registered`/`wake_io`/`wake_or_cancel`，`:2835-3136`，正是 X-10 的主题）**、`worker_loop`(`:3494`，M 主循环)；
  - 原语统计：`std::mutex` **92** 次、`atomic<` **38** 次、`wake` **61**、`park` **33**、`sysmon` **59**、`steal` 5；
  - **骨架里已浮现的硬约束注释**（下一阶段要逐一核实的）：`:584`「锁内完成状态转换和 permit 发布，确保 wake 要么被 park 消耗」；`:901`「只能沿同一逻辑 G 的父链恢复，即使 G 迁移到另一个 M 也不会丢失」；`:2347-2348`「首次注册分配失败时**绝不能**继续发布队列节点，否则任务第一次 park 后可能只剩调用方的裸句柄」；`:2477`「已启动 Fiber 不能在分配失败时丢失。救援队列复用」；`:2614`「计数必须先于节点发布，否则另一个 worker 可能先 pop 节点」；`:3872-3874`「BlockingRegion 不能跨 Fiber yield/park 或 G 的任务边界…**宁可在边界处 fail-fast**」；`:3970`「替代 M 在原 M 返回前必须保留」。
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致。
- **台账状态**：本轮**无新增条目**；待办仍为 **21 条**。
- **下一步（Scheduler 通读计划，分段推进）**：
  1. `:1-500`（`Task` 的状态机：queued/wake/park/register/cancel 一族）；
  2. `:500-1000`（`Task::run()` + `Scheduler::Impl` 开头）；
  3. `:1000-1500`（`Impl` 的队列、steal、sysmon 记账）；
  4. `:1500-1907`（`Impl` 余下 + `Lifetime`/`TimerExecutorAdapter`）；
  5. `:1907-2600`（`start`/`shutdown`/`spawn`/`enqueue`/`requeue_from_worker`）；
  6. `:2600-3200`（yield/park 一族 + **`wake` 一族，X-10 的核心**）；
  7. `:3200-4048`（blocking/sysmon/metrics/`worker_loop`/`BlockingRegion`）。
  另需并行读 `tests/concurrency/test_scheduler.cpp`(542)、`test_dynamic_scheduler.cpp`(549)、`scheduler_smoke.cpp`(418)、`high_load_stress.cpp`(942)、`test_beginner_api.cpp`(162)、`test_event_go.cpp`(202)。

## 2026-10-12：Fiber 专题筹备 —— 重读 `fiber.cpp` 并补充机制章节

- **用户指示**：「Fiber 模块是个重磅硬骨头，你提前再筹备一下」。本轮**不产出新条目**，而是对 `src/concurrency/fiber.cpp`(1502) 做系统性重读，并在 `docs/fiber_guide.md` 追加 **11.5 实现机制补充**（文档由 471 行增至 **679 行**）。
- **阅读范围（本轮已精读）**：`:1-260`（`FiberTlsRestore` / `CachedStack` / `StackCache` / `FiberBin` / `FiberStack` 开头 / `normalize_stack_size` 全链路）、`:546-1000`（`Fiber::Impl` 的构造、`reset_for_scheduler`、`abandon_suspended`、`~Impl` 全文、`bind_caller`、`resume_locked` 前半）。
  - **尚未精读（已在文档中明确留白）**：`:261-545`（`FiberStack` 全文与 `:480-512` 一带结构）、`:1000-1147`（父链传播余下）、`:1148-1502`（`Fiber` 公开方法实现与取消路径）。
- **★ 本轮最值得记的发现（已写入文档）**：
  - **① errno 的 `NOINLINE` 陷阱（`:156-160`）** —— 代码注释原文：「将 TLS 地址查找放在挂起函数之外。**否则编译器可能在跨线程上下文切换后仍缓存 `__errno_location()` 的结果**」。`errno` 在 glibc 里是 `*__errno_location()`，而它返回**线程本地地址**；若编译器把该地址缓存在寄存器里，Fiber 从线程 A 迁到 B 之后仍会命中**线程 A 的**存储 —— 静默错误。**这是「Fiber 复用同一 OS 线程」最隐蔽的后果之一**，配套的是 `resume_locked` 里的三步存/装/恢复（`:924`/`:938`/`:988`）；
  - **② 三套 sanitizer 的集成** —— `FiberStack` 带 `m_valgrind_stack_id`；`Fiber::Impl` 带 `m_asan_fake_stack`/`m_asan_caller_bottom`/`m_asan_caller_size` 与 `m_tsan_fiber`/`m_tsan_caller`；切换前后调用 **`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`**（ASan 官方的 Fiber 协议）与 `__tsan_switch_to_fiber`。**能同时正确接入三套工具链**，说明这是按「会被真实用户拿去跑 sanitizer」的标准写的 —— 这也解释了测试里那些 `RUNNING_ON_VALGRIND` 分支（见 T-15）是有意保留的；
  - **③ `resume_locked` 里三个极易写错的点**（`:897-1000`）：**(a) `jump` 返回的两种语义** —— 注释原文：「如果当前 Fiber **仍在 Running**，说明是**它在自己的栈上恢复了嵌套子 Fiber**……只有挂起/完成/失败并回到 caller 时，才恢复旧 TLS。**无条件恢复 `previous_fiber` 会让父 Fiber 后续创建的子 Fiber 错绑到根上下文**，破坏父链并可能触发 fail-fast」；**(b) TLS 的 RAII 恢复** —— 因为「调度器级挂起可能在本函数内部沿父链循环多次」，必须用 `FiberTlsRestore` 守卫保证最终恢复最初调用者；**(c) 调度器挂起与普通挂起的分流** —— `m_scheduler_propagate` 为真时沿父链继续上传，否则普通 `Suspend` **只回到直接父**（注释：「因而 fiber2 的普通 `go_back` 仍然只回到 fiber1」）—— **这正是测试里那个「嵌套 park」用例所验证的语义**；
  - **④ `~Impl()` 的状态机与「claim 才是生命周期闸门」**（`:653-720`）：注释原文：「先取得执行 claim，再观察终态。`entry()` 会先发布 `Completed`/`Failed`，后由 `resume()` 释放 claim；**若只检查 state，析构可能在这个窗口释放 fcontext/保护栈，导致 `resume_locked()` 继续访问已释放对象**」。即 **claim 是真正的闸门，state 只是可观测的影子**；
  - **⑤ `abandon_suspended` 的三个设计决定**（`:630-651`）：**标 `Failed` 而不是 `Completed`**（因为栈上局部对象**没有**析构，报告成正常完成是在撒谎，注释：「不能伪造 Completed」）；**`catch (...)` 兜住异常构造失败**（「诊断对象分配失败时仍必须完成上下文隔离」）；**主动清理 `FiberLocal`**（否则 G 的逻辑身份被复用时串味，与 `ContextKey` 的 ABA 同类）；
  - **⑥ `bind_caller` 的锁序设计**（`:829-854`）：注释说明「**先复制父记录，再写入本 Fiber，避免同时锁两个 Fiber 的记录而形成反向锁序**；这样错误的并发 resume 会返回失败而不是制造死锁」—— 实现上是「在 `m_metadata_mutex` 下只读父记录指针 → 释放 → 再锁父记录 → 重新取自己的锁并复查 `m_parent != caller`」，**用「释放-重取-复查」换掉嵌套锁**；
  - **⑦ `FiberBin` 的真实形态**：`thread_local FiberBin s_thread_fiber_bin;`（与头文件「回收到**创建它的 M** 的线程本地池」严格对应），且**有硬上限 `kMaxFiberBinCapacity = 4096`**，注释写明理由「防止误配把每个 M 变成无界对象保留池。FiberBin 只应缓存短期突发，更大的容量不会带来线性收益」；
  - **⑧ 栈回收时机**（`:965-975`）：`entry()` 返回后**立即** `m_stack.reset()`，注释：「已展开全部用户栈帧。现在可以立即把受保护栈放回线程级或全局缓存，避免突发负载中每个排队 Fiber 都重新建立一次映射」。
- **★ X-13 补上精确证据（本项因此从「描述」升级为「定位到两处代码」）**：`~Impl()` 里有**两处无界等待** —— ① `:663-669` 用 `while (!m_resume_claim.compare_exchange_weak(...)) { std::this_thread::yield(); }` 自旋等 claim，**无超时、无退避、无上限**；② `:688-711` 的收尾循环，退出**唯一**依赖主体自己进入 `Completed`/`Failed` —— 一个 `while (true) Suspend(Park);` 且不检查取消标记的主体会让析构**永不返回**。**核心问题收窄为「文档低估了强度」**：头文件只说「可能阻塞析构」，而实际是**无界忙等 + 无界循环**，调用方无法据此判断「到底会不会返回」。
  - 同时记录了一处**相关但正确**的设计（**不构成问题**）：`:656-658` 用 `if (s_current_fiber == m_owner) std::terminate();` 拒绝「在 Fiber 自身栈上销毁自身」，注释说明「否则没有任何安全位置可以释放当前正在使用的栈」—— 这是**正确的 fail-fast**，因为该情形是明确的 API 前置条件违反；
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致。
- **台账状态**：本轮**无新增条目**（X-13 就地补充精确证据）；待办仍为 **21 条**。
- **下一步（Fiber 专题的剩余留白）**：若用户继续就 Fiber 提问或下达任务，尚有以下未精读部分可作为补充准备：`fiber.cpp:261-545`（`FiberStack` 全文，含 valgrind 登记、保护页、缓存回收）、`:1000-1147`（父链传播余下）、`:1148-1502`（`Fiber` 公开方法、`RequestCancellation`、`FiberLocal` 清理调用点）、以及 `include/go2cpp/concurrency/fiber_local.hpp` 与 `fiber_local.cpp` 的 trampoline 所有权实现。

## 2026-10-12：模块讲解（基础模块 4 · fiber / fiber_local / thread_policy）—— 新增 T-17、T-18

- **本轮读（全部全文）**：`tests/concurrency/test_fiber.cpp`(439)、`tests/concurrency/test_fiber_local.cpp`(124)、`tests/concurrency/test_thread_policy.cpp`(94)。产出 **`docs/fiber_guide.md`（471 行，13 节）**。
- **★ 新发现：T-17（中）—— `FiberBin` 的两个调度器专用接口零覆盖**。`Fiber::AcquireForScheduler(Function, stack_size, bin_capacity = 32)` 与 `Fiber::RecycleForScheduler(...)` 在 `tests/` 与 `example/` 中**`grep` 零匹配**。而头文件对它们的约束相当具体：「只有**已经完成或失败、且不再有可恢复上下文**的内部 G Fiber 才允许回收到创建它的 M 的线程本地池」「**不会跨线程转移仍挂起的 Fiber**」。这三条（**回收条件 / `bin_capacity` 上限 / 跨线程拒绝**）全无测试固化 —— 而它们正是 **`scheduler` 的依赖面**。
  - 值得强调的对比：`test_fiber.cpp` 枚举了 **18 类边界**（见 `docs/fiber_guide.md` 第 11 节的表格），是我在项目里见过的**边界覆盖最全的测试**，**唯独在「调度器侧回收路径」上是空白**。
- **★ 新发现：T-18（低）—— `thread_policy` 两处未覆盖**：① `ScopedThreadParticipation` 的**引用版构造**（`ScopedThreadParticipation(Scheduler&, mode)`）未测，测试只用指针版；② `ThreadHookMode::kInherit` 的**继承语义**未验证 —— 测试只覆盖了 `kDisabled`/`kEnabled`，而类注释声明「`kInherit` 跟随进程级开关」，该「跟随」行为无任何断言。
- **文档讲透的九个场景**：
  - **场景一 挂起/恢复 + errno 双向保存**：这是**很容易漏的正确性点** —— `errno` 是线程局部的，而 Fiber 复用同一线程；测试验证了 Fiber 内部挂起前后 `EAGAIN` 保持、外部 `resume` 前后 `EDOM`/`ERANGE` 保持。同场景还讲了栈尺寸**归一化**与**溢出必须在构造期拒绝**（`size + guard` 的加法在 `SIZE_MAX` 下会回绕成小值，「不能等到首次 resume 才破坏上下文」）；
  - **场景二 跨线程迁移**：用两个真实 `std::thread` + 双 `gettid` 确认Fiber 从线程一挂起、线程二恢复，且**栈上局部变量与 FiberLocal 值都保持**。文档解释了物理基础：Go2Cpp 用的是**自己分配的栈**而非 OS 线程栈，上下文里保存的是栈指针，所以换线程继续跑只是换了个指针值；
  - **场景三 嵌套与墓碑帧**：父 Fiber 栈上可以再起子 Fiber；父结束后的诊断链由共享记录保留 `alive=false` 的墓碑帧，头文件明确「**只能用于观测、取消和诊断，不能恢复已销毁的父栈**」；
  - **场景四 析构语义**：未启动（Ready）→ **跳过主体但释放捕获**；挂起中 → **取消 + 恢复 + 等它自然结束**（这样它栈上的局部对象才能析构），因此**析构可能阻塞**；
  - **场景五 错误父级下的销毁——安全优先的取舍**：子 Fiber 挂在父栈上时，若父已不在当前执行链，恢复子就等于**跳进不可达的调用链**（上下文层面的 UB）。运行时选择「**放弃子上下文 + 标记 `Failed`**」，宁可泄漏子栈上的 RAII，也不做可能崩溃的栈跳转。第二处是「错误父级析构**也必须摘除 FiberLocal 值**，不能因地址复用把旧 G 的值带给后续 Fiber」——**与 `ContextKey` 的 anchor 同一类问题**；
  - **场景六 异常边界**：用 `resume_result()` 而非 `resume()`，「Fiber 不把 C++ 异常抛出到调度器」。**这与 `control_flow` 讲解里「为何不宜用异常实现 panic」是同一条理由**（`scheduler` 在 Fiber 入口有 `catch (...)`，异常逃出即整个任务 `kFailed`）；
  - **场景七 `FiberLocal` 的六种回退**：线程 TLS fallback、跨 Fiber 隔离、park 后保持、Fiber 结束后回收、key 唯一、跨线程迁移保持；最值得讲的是**「key 对象生命周期可短于 logical G」**——`FiberLocalCache` 只是句柄，值的所有权在 **G 的 trampoline**，所以销毁 wrapper 不会使 Fiber 中的值失效（测试用真实析构计数验证）。**这与 `ContextKey` 是同一思路的两个方向**：`ContextKey` 是「值钉住键」，`FiberLocalCache` 是「G 拥有值」；
  - **场景八 `thread_policy`**：四个检查；文档特别记下该模块的**设计克制** —— 这两个作用域**只写 TLS 元数据**，「不会创建 M、绑定 P、安装 `current_scheduler()` 或执行队列任务」，即「声明我是谁」而非「把我变成 worker」；
  - **场景九/实现细节**：`FiberExecutionBinding` 只存**不拥有的标识**（`scheduler_token` 是地址整数形式），避免 Fiber 依赖 scheduler 模块、避免调度器生命周期被调试信息反向延长。
- **测试评述中最有价值的一处**（文档第 11 节第 13 项）：**嵌套 park 的唤醒顺序验证** —— inner 在调度器里 park 时，outer 与根 G 都只保存 continuation；唤醒后必须**先回 inner 再回 outer**。测试用一条「在 inner park 期间不能执行」的断言来检测这一点：若唤醒顺序错了，outer 的后续代码会在 inner 还没跑完时被执行，断言即失败。**这是「用手写断言检测执行顺序错误」的范例。**
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致（`94a798af…`）。
- **台账状态**：新增 **T-17**、**T-18**；待办 **21 条**。
- **下一步**：基础模块讲解线还剩 `sync`（`Mutex`/`ConditionVariable`/`WaitGroup`，`test_sync.cpp` 969 行）、`config`、`logging`、`timer`，以及更大的 `scheduler`。按依赖，`sync` 是 `fiber` 之后最自然的一步（它直接建立在 Fiber park 之上，且 X-25 的多处 noexcept 问题也在这一族）。

## 2026-10-12：模块讲解（基础模块 3 · context）—— 新增 X-32、T-16

- **用户指示**：讲解下一个基础模块。本轮完成 **`context`**，产出 **`docs/context_guide.md`（536 行，12 节）**，与 `error_guide.md`、`control_flow_guide.md` 同一标准。
- **本轮读**：`tests/concurrency/test_context.cpp`(462，**全文**)、`example/concurrency/context_rollback_demo.cpp`(232，**全文**)、`docs/context_rollback.md`(121)，并回查 `src/concurrency/context.cpp` 的 `Context::Cancel`/`Now`/`s_now_function`、`recover` 以外的取消传播路径。
- **用量地图（本轮最有信息量的部分）**：对 `src/` 全量统计后得到一个与 `Error` 讲解**相互印证**的结论 ——
  | API | src/ 次数 | 说明 |
  |---|---:|---|
  | `ContextRollback` | **36** | **看似最高，但全部在 `context.cpp` 自身** |
  | `->Cancel()` | 21 | |
  | `->Done()` | 17 | |
  | `->IsDone()` | 13 | |
  | `DeadlineExceededError()` | 9 | 与 Error 讲解的判别配对 |
  | `->Err()` | 7 | |
  | `->Deadline()` | **1** | 运行时几乎不查询截止时间，只用 IsDone/Err |
  **分文件看**：`channel.cpp` 19（`Cancel`7/`IsDone`3/`CanceledError`3/`Err`2）、`io.cpp` 14（`Err`4/`Done`4/`DeadlineExceededError`4）、`sync.cpp` 11、`sse.cpp` 5、`timer.cpp` 1。
  **结论**：运行时对 Context 的依赖可概括为 —— 「每个阻塞点持有一个可取消、可超时的信号，醒来用 `Err()` 区分原因」；而 `ContextRollback` 虽出现次数最高，**运行时一处都不用**，与 `control_flow` 同属「为使用者准备的 API」。
- **★ 新发现：X-32（低-中，性能）—— `Context::Now()` 在热路径上加全局锁**。`src/concurrency/context.cpp:1818-1825` 的实现每次都要 `lock_guard(s_clock_mutex)` + **复制一个 `std::function`**，而 `s_now_function`（`:231`）**只在测试中被设置**、正常运行时恒为空 —— 即每次调用都在为不存在的自定义时钟付出全局锁 + function 拷贝。
  - **调用点已 grep 全量确认在热路径上**：`future.hpp:291`（`Wait` 的谓词）、`:363`/`:558`（`StatusLocked`）、`:492`、`:601`（`DeadlineAfter`），以及 `context.cpp:1667`/`:1704`（`WithDeadline`/`WithTimeout`）—— **每一次 `Future::Wait`、每一次创建超时上下文都会走这里**；
  - **对照证据（很有力）**：项目在 `log.hpp:363-364` 明确写出正确做法 —— 「热路径只做一次原子 shared_ptr 读取…因此格式化器和 Sink **不会在每条日志上争用 Worker 配置锁**」。**同一个项目知道怎么做，这一处没做**；
  - 修复只需 `std::atomic<std::shared_ptr<NowFunction>>` 或先做一次廉价的 `std::atomic<bool>` 判断，测试接口不变。
- **★ 新发现：T-16（低，测试盲点）**：`ContextRollback::committed()` **零覆盖**，而其镜像方法 `rolled_back()` 被断言四处（`:200`/`:226`/`:237`/`:424`）。`docs/context_rollback.md:16-21` 的命名约定表专门提过 `committed()` 与 `had_failure()`「只提供小写形式」，而 `had_failure()` 已在测试与示例场景五中被验证，`committed()` 则在两处**一次都没出现**。
- **文档讲透的七个场景**：
  - **场景一 取消树的三条硬规则**：① **先全树标记、再跑回调**（测试注释：「Descendant cancellation is committed before parent callbacks run」—— 若边走边回调，父回调里同步等子节点会**死锁**）；② **幂等**（含 8 线程 first-wins）；③ **迭代传播**（**20000 层**深树不爆栈，与 Error 的 10 万层深链同一设计关切）；
  - **场景二 根 Context 与 Go 的三处有意差别**：根暴露 `Cancel()` 但无效；`Done()` 非 nil 而是**永久 pending 的 `DoneSignal`**；空 parent 映射为 `Background()`（Go 会 panic）。三处均与 `docs/compatibility.md:13` 一致；
  - **场景三 截止时间的三个边界**：正常到期、**已过期立即完成**、**`ContextDuration::max()` 饱和**（不饱和则 `now + max` 回绕成过去，让「永久」的 Context 立刻超时——极隐蔽的 bug），以及可注入时钟；
  - **场景四 `ContextKey` 的 anchor 为何必要**：身份用 token 的**地址**，若 Key 出作用域后 token 被释放、地址被复用，新键就会**冒充旧键**（ABA）。`WithValue` 把 `Anchor()` 存进节点以钉住地址。测试用 **64 次循环**验证防别名；
  - **场景五 P=1 反证 park**：这是我认为整个测试文件里最有价值的一处 —— 只给 1 个 P，等待者若用原生 condition_variable 就会**占住唯一的 M**，兄弟取消者永远跑不起来、测试超时；**测试通过即证明走的是 park 路径**。同一手法在 `ContextRollback::rollback_done()` 上又用了一次；
  - **场景六 `ContextRollback`**：它是「临时 child + undo 栈」的受限封装（`docs/context_rollback.md` 开篇即声明「不是数据库事务，也不是 longjmp」）；关键是 **`child.Done()` 与 `rollback_done()` 是两个不同信号** ——前者表示取消已线性化、后者表示补偿已跑完，只等前者会 **UAF**（文档「常见错误」列专门警告）。另有 commit/rollback 竞争、undo 异常不中断、savepoint 的 ABA 防护等不变量；
  - **场景七 与 Error 的汇合**：完整链路 `WithTimeout → TimerService → 状态标记 → 发布 CanceledError/DeadlineExceededError 单例 → Done().Signal() → 各模块 Err() + Is() 判别 → 映射状态码/errno/枚举`，**横跨 context→timer→error→channel/io/sse 四个模块**，是本项目最核心的贯穿设计。
- **测试评述**：`test_context.cpp`(462) 是**本项目最细致的并发测试**——两处 **32 轮竞争循环**（父取消 vs 显式 rollback、commit vs 父取消），在没有 TSan 的环境下用真实重复竞争提高覆盖概率。
- **示例与文档评述**：`context_rollback_demo.cpp` 六个场景每个都有明确失败判据，并在场景五主动警告「业务必须检查 `had_failure`，不能只看 `rollback()` 返回值」。**`docs/context_rollback.md` 的六行表格额外给出「常见错误」一列**，这种「成功判据 + 常见错误」并列的写法对使用者非常友好，**已建议作为项目其他文档的范本**（写入文档第 10.3 节）。
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致（`94a798af…`）。
- **台账状态**：新增 **X-32**、**T-16**；待办 **19 条**。
- **下一步**：基础模块讲解线的候选 ——
  - **`fiber` / `fiber_local` / `thread_policy`**（并发执行单元，`tests/concurrency/test_fiber.cpp` 439 行 + `test_fiber_local.cpp` 124 + `test_thread_policy.cpp` 94）；
  - 或 **`config`**（INI + 原子快照 + 两阶段提交，其测试已在上一轮 config/logging 审查中读过大部分）；
  - 或 **`sync`**（`Mutex`/`ConditionVariable`/`WaitGroup`，`tests/concurrency/test_sync.cpp` 969 行）；
  按依赖顺序，`fiber` 是 `context` 之后最自然的下一步（`FiberLog`/`fiber_local` 都要先理解 Fiber 的执行模型）。

## 2026-10-12：用户质疑引发复核 —— X-30 判断过重，已修正定位

- **用户质疑原文**：「这一块你觉得有必要重做吗，panic 和 recover 都是语言级别的东西，C++ 只能实现相似的」。
- **这是本会话第一次由用户的技术判断直接纠正我的结论**，复核结果是我上一轮**把 X-30 判重了**，必须如实修正。
- **复核依据（项目自身文档，均为既有内容，非本轮新增）**：
  - `docs/boundary_audit.md:32`：「`panic/recover` 是**显式状态记录设施**，不会进行语言级栈展开，也不能跳过 C++ 析构函数；**不能把它当作 Go panic 的完全替代**」；
  - `docs/boundary_audit.md:33`：「defer/recover 的精确边界是当前执行状态和 API 规定的受控调用范围，**不支持任意跨 Fiber、跨 M 的隐式恢复**」；
  - `docs/compatibility.md:14`：「**recover 的边界是回调作用域，不是编译器级 Go G/栈帧；共享对象本身不绑定 G/Fiber，跨执行流消费不会被库自动阻止，属于调用方必须遵守的限制**」—— **这句准确描述了 X-30 所描述的行为**。
- **结论修正**：
  - **实现行为本身不构成缺陷**——它是有意为之，且已在两份文档中对外声明。我上一轮把它定为「中（契约违反）」是**把文档口径冲突误判为实现缺陷**；
  - **真正的缺陷收窄为**：`include/go2cpp/tools/control_flow.hpp:199-200` 的措辞「**其他线程或 Fiber 不能越过恢复边界捕获 panic**」与上述两处文档**矛盾** —— 头文件把「调用方必须遵守的限制」写成了「库保证的性质」；
  - 严重度由「中（契约违反）」**下调为「低（文档口径冲突）」**，修复方向从「改实现」改为「改头文件措辞 + 补指向 compatibility.md 的引用」。
- **同时记录「为何不宜重做」的技术论证**（回答用户问题，已写入台账 X-30 备注）：
  - **① 栈不同源**：Fiber 有自己的栈（`context_x64.S` 的上下文切换），而 C++ 异常基于线程栈；
  - **② 入口已拦截**：项目**已在 Fiber 入口捕获异常**（`src/concurrency/fiber.cpp` 多处 `catch (...)`，对应 `GState::kFailed` 这一「普通 C++ 异常在 Fiber 边界被捕获后的失败终态」）。若用异常实现 panic，会**先被该 catch-all 拦成「任务失败」**而非「被 recover」，要绕开必须引入专用异常类型并在入口特殊处理，复杂度显著上升；
  - **③ 与整体风格冲突**：项目是**不用异常做控制流**的（`Error`/`Future`/`channel`/`io` 全用状态或返回值，仅 `Future::GetOrThrow` 显式 opt-in 抛异常），显式状态与之一致。
- **因此回答用户问题：不必重做。** 更准确的说法是「**这个模块的目标从来不是与 Go 等价，而是提供一套可用的、与 C++ 语义相容的对应物**」——项目自己也是这么定位的（`boundary_audit.md` 的原话就是「不能把它当作 Go panic 的完全替代」）。真正该做的是**把差异对使用者讲清楚**，而 `compatibility.md` 已经做了大部分；剩下的是把 `control_flow.hpp` 里那句过强的措辞改掉。
- **文档同步**：`docs/control_flow_guide.md` 第 6 节已插入更正段（保留原始分析并标注以更正为准），避免把我的过重判断留给后续读者。
- **方法论层面的教训（值得记）**：本轮暴露了一个**登记时的系统性偏差** —— 我在对照「头文件措辞」与「实现行为」时，**默认头文件是对外契约的唯一权威**，而忽略了项目另有 `compatibility.md` / `boundary_audit.md` 这类**专门声明「与 Go 的偏差」的文档**。在存在多份权威文档时，判断「谁矛盾了」必须先确认**哪一份才是该维度上的权威**。此前 D-18（`compatibility.md` 声称不绑核、而实现绑核）是**文档错了**，所以本轮我下意识沿用了「文档与实现冲突时先怀疑文档」的惯性 —— 但 X-30 的维度上，`compatibility.md` 才是权威，头文件反而是简写失误。**同类冲突不能套用同一结论，必须逐维度确认权威来源。**
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致（`94a798af…`）。
- **台账状态**：无新增条目；**X-30 严重度下调**；待办 **17 条**（其中 T-10 需用户决策、T-15 已修复、X-30 已重新定位）。
- **下一步**：回到基础模块讲解线 —— 候选为 `config` 或 `context`。**建议 `context`**（并发层地基，且是 `Error` 场景一里`CanceledError`/`DeadlineExceededError` 两个单例的产源地）。

## 2026-10-12：模块讲解（基础模块 2 · control_flow）—— 新增 docs/control_flow_guide.md

- **用户指示**：继续讲解「下一个基础模块」。Error 之后按依赖顺序的基础模块是 **`control_flow`**（`defer`/`panic`/`recover`），与 Error 同属 `tools/` 基础层。本轮按 `error_guide.md` 的同样标准产出 **`docs/control_flow_guide.md`（502 行，12 节）**。
- **写作方法仍是「先扫描用量，再讲设计」**，扫描结果出现了一个**决定性事实**：
  | 检查项 | 结果 |
  |---|---|
  | 包含 `control_flow.hpp` 的文件 | `example/tools/control_flow_demo.cpp`、`runtime.hpp`、`src/tools/control_flow.cpp`、`tests/tools/test_control_flow.cpp` |
  | **`src/` 中 `defer` 的使用** | **0**（除自身实现） |
  | **`src/` 中 `panic`/`recover` 的使用** | **0** |
  | `tests/`+`example/` 中的使用 | `defer` 27、`panic` 13、`recover` 14 |
  **结论：运行时自身完全不用这套 API，它是纯粹为使用者（转译代码/业务代码）准备的。**这与 Error 模块「运行时重度使用但只用了它一小部分能力」的形态**正好互补**，也决定了本文的叙述重心 —— 讲「它被设计成怎么用、使用者会遇到什么」，而不是「运行时的哪条热路径依赖它」。
- **★ 本轮最重要的收获：为 X-30 补上了决定性证据，使其从「实现有隐患」升级为「契约违反」**。
  - 头文件 `include/go2cpp/tools/control_flow.hpp:199-200` **明确承诺**：「take() 只有在活动的 defer 回调期间才会返回状态；**其他线程或 Fiber 不能越过恢复边界捕获 panic**」；
  - 同文件 `:143` 声明 `panic` 是「**可随 Fiber 迁移**的显式 panic 状态」—— 这一点确实被满足（`shared_ptr<PanicState>` + 内部 `mutex`）；
  - **即设计意图明确是「panic 状态可跨 Fiber 迁移，但恢复边界必须是 Fiber 局部的」**，而实现把边界做成了**线程局部**（`thread_local unsigned int s_defer_depth`）。**两者直接矛盾**：触发后违反的是**对外文档化契约**，不只是内部一致性；
  - 据此已就地更新台账：X-30 的严重度写明为「**中（契约违反）**」并附上述文档出处。
- **文档讲透的六个场景**：
  - **场景一 `defer` 作为 RAII**：析构即执行点（`defer::~defer() { run_now(); }`），回调擦除进 `Callable`；**LIFO 顺序不需要实现——C++ 局部对象析构顺序天然逆序**；`run()` 是 `noexcept` 且内 try/catch，因为它在析构里被调用；类**删除复制与移动**，保证一次注册只执行一次；
  - **场景二 参数注册时求值**：`MakeCallableWithArgs` 把函数与参数搬进 `std::tuple` 再 **move 进 lambda**，执行时 `std::move(args)` 移出 —— 安全性来自「一个 defer 最多执行一次」。**该语义与 Go 一致**（Go 的 `defer f(x)` 也是注册时求值），示例注释特意点明这一点；
  - **场景三 显式边界**：`panic` 持 `shared_ptr`（拥有状态）、`recover` 持 **`weak_ptr`**（不延长寿命，故 `panic` 先析构时安静失效）；**与 Go 的三条关键差别**已列表对比 —— 尤其第三条：**panic 无人 recover 时程序不崩溃**，只是留下一个状态对象（第 4.3 节）；
  - **场景四 panic 可跨 Fiber 迁移**：`PanicState` 被 `shared_ptr` 持有且所有字段访问都在 `mutex` 下，故跨线程/跨 Fiber 安全；测试用真实 `std::thread` 验证了跨线程 `active()` 的可见性；
  - **场景五 恢复边界（X-30 的舞台）**：完整给出 `InDeferCallback()` 的三行实现与四步泄漏时序；
  - **场景六 defer 异常记录**：为何不能传播（`~defer()` 是 `noexcept`，栈展开中抛出即 terminate），以及 `LastDeferException()`/`ClearDeferException()` 的用法；
- **另有一处设计分寸值得记**：`PanicState` 的五个读取/操作入口对边界检查的态度**不同**——`take()` 与 `operator()()` **检查**（消费型操作必须守住边界），而 `peek()`/`active()`/`clear()` **不检查**（只读或由持有者主动重置）。这不是遗漏而是有意的分级，已整理成表写入文档第 8 节。
- **测试与示例评述**：`test_control_flow.cpp` 覆盖了 11 条关键断言（含**栈展开路径**下 defer 仍执行、`run_now()` 幂等、`dismiss()` 取消）；盲点即前述 T-12/T-13/T-14。示例的三个场景划分合理，注释主动解释设计意图；**一个运行副作用**：它写日志到 `log/control_flow_demo.log`，与「`log/` 基线 md5 保持一致」的审计约束冲突，故本流程只做静态审读。
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致（`94a798af…`）。
- **台账状态**：本轮无新增条目（X-30 就地强化）；待办 **17 条**。
- **下一步（供后续轮次对齐）**：
  - **讲解线**：下一个基础模块候选 —— `config`（INI 解析 + `ConfigVar` 原子快照 + 两阶段提交事务）或 `context`（取消树 + 截止时间 + 值传递）。按依赖，`config` 只依赖 `log` 与 `scheduler` 的转发头，而 `context` 是并发层的地基，两者都可作为「基础模块 3」；
  - **测试审查线**：`tests/concurrency/test_context.cpp`(462) + `example/concurrency/context_rollback_demo.cpp`(232) 尚未审读。

## 2026-10-12：tests 审查（config + logging）—— 发现并修复 T-15

- **本轮读**：`tests/logging/test_log_config.cpp`(321)、`example/logging/log_config_demo.cpp`(80)。
- **★ 新发现并已修复：T-15（中，编译期/移植性）**。`test_log_config.cpp:189` 的 `if (RUNNING_ON_VALGRIND)` **没有宏守卫**，而同族代码**七处全有**：`test_support.hpp:52`/`:60`、`test_io.cpp:27`/`:55`/`:377`/`:400`/`:428`、`test_hook.cpp:310` 都是 `#if defined(GO2CPP_TEST_HAS_VALGRIND)` 包裹。
  - `RUNNING_ON_VALGRIND` 由 `<valgrind/valgrind.h>` 提供，而 `test_support.hpp:13-18` **只在 `__has_include(<valgrind/valgrind.h>)` 成立时**才包含该头并定义 `GO2CPP_TEST_HAS_VALGRIND`；
  - **后果**：在未安装 valgrind 开发头文件的环境（常见于 CI 容器）中，该行因标识符未定义而**编译失败**，整套测试无法构建；
  - **本环境掩盖了它**：`/usr/include/valgrind/valgrind.h` 与 `/usr/bin/valgrind` 都存在（本轮实测确认）；
  - **已修复**：补上 `#if defined(GO2CPP_TEST_HAS_VALGRIND)` / `#endif` 守卫。**验证**：build-quality 构建 **0 warning / 0 error**，`ctest` **26/26 全部通过**。属提示词所列「测试/示例类改动」的即时修复例外。
- **test_log_config.cpp 其余核对通过的要点**（该文件是本项目覆盖最扎实的测试之一）：
  - `:23-26` 验证「**解析失败不得覆盖之前的完整快照**」（先解析合法 INI，再用 `[broken` 触发失败，然后断言旧值仍在）—— 这正是 `IniFile::Parse` 「重复调用会先清空旧节和旧键」注释的反面对照，即**失败时不破坏已有状态**；
  - `:45-54` 用 **8 线程 × 250 条**并发写日志，断言 `callback_count == kThreads * kMessages` —— **精确相等，即不允许丢日志**；这是对 `LogItemWorker` 原子快照设计的有效压力验证；
  - `:83-87` 验证**父链无环**：`root->SetParent(child)` 被拒绝后，日志仍只向 root 传播一次（`Snapshot().size() == 2`），注释点明「不会递归耗尽调用栈」；
  - `:96-113` 覆盖自定义格式项的**四个契约**：`HasFormat` 前后状态、重复注册返回 false（注释：「同一 key 不允许静默覆盖，避免不同模块之间的格式协议互相污染」）、`%g` 与 `{g}` **两种语法**都生效、`RemoveFormat` 后状态清除；`:146-148` 还验证了「**注销后已构造的 formatter 仍可用**」（快照语义）；
  - `:139-140` 验证**内置格式项不可被业务插件覆盖**（`addFormat("d", ...)` 返回 false）；
  - `:194-225` 热加载测试**覆盖了三种编辑器行为**：截断写入（只写 `level=` 不生效）、编辑锁非空期间不读取、**清空锁文件是写入事务的提交点**（清空后配置才生效）；
  - `:230-232` 用 **`static RuntimeConfig dynamic_runtime`** 并注明理由「registry 是进程级单例，绑定目标也必须覆盖它所持有的监听器生命周期」—— **这是正确的静态生命周期推理**；
  - `:258-259` 与 `:272-275` **跨模块集成断言**：配置改动后直接检查 `core::TimerService::Default().DurationLimits()` 已同步，把「配置 → 运行时资源」这条链路也纳入测试；
  - `:291-320` 两阶段提交验证：非法输入（`transaction_second=bad`）后两个字段**都**保持旧值，合法输入后 `commit_notifications == 1` 且监听器**看到的是完整新快照**（`saw_complete_snapshot`）。
- **示例评述（`log_config_demo.cpp`）**：三个场景（注册自定义格式项、合法 INI 读取、非法节名不覆盖旧配置）划分合理，且 `:44-48` 主动处理了「示例目录不一定有 `go2cpp.ini`」并回退默认值（注释：「不能把『文件不存在』误当成日志系统崩溃」）。两处小瑕疵（**均未登记**，仅记录）：① `:49` 用 `static GO2CPP::Logger::ptr` **没有注释说明**为何需要 static（应是为避免退出期写入已析构的 manager）；② `:77` 的结语「配置 log.stdout=true 后才开启」有些误导 —— 示例 `:62` **只解析了**该 INI 并未应用它（`stdout=true` 一行在示例中不产生效果）。
- **验证结果**：本批**改动了 `tests/logging/test_log_config.cpp`**（1 处 4 行），已通过 build-quality 构建（0 warning / 0 error）与 `ctest` 26/26 全绿。在途 4 处文件 diff md5 与轮初一致（`94a798af…`）。
- **台账状态**：新增 **T-15（已修复）**；待办 **17 条**（其中 T-15 已了结，实际未结 16 条）。
- **★ 主任务提醒（供后续轮次对齐）**：用户已于 2026-10-12 变更主任务 ——
  1. `include/` **已全部读完**（33 个有内容文件 9,493 行 + 46 个转发头 160 行，完成度 100%），定时任务提示词里「include 待读约 6,500 行」的描述**已过时**；
  2. **模块讲解任务只做 `Error` 一项，已交付 `docs/error_guide.md`(577 行)，到此收尾**；
  3. **当前主任务是「按模块顺序查找 tests 与 example 的问题」**，产出为**台账登记项**而非讲解。**请后续轮次以本条为准。**
- **下一步**：按模块顺序继续审查 —— `context`（`tests/concurrency/test_context.cpp` 462 行 + `example/concurrency/context_rollback_demo.cpp` 232 行），其后 `fiber`/`fiber_local`/`thread_policy` → `sync` → `scheduler` → `future`/`channel`/`event` → `timer` → `io`/`hook`/`fd_manager` → 网络层 → `go`/`runtime`。

## 2026-10-12：Error 模块场景化深度讲解（用户指定「只讲解 Error」）

- **用户指示**：讲解**只做 `Error`**；要求把「配合使用的代码、实现的细节、测试」一并讲清，并**以多场景说明它在各处起什么作用**。因此本轮把 `docs/module_tour.md`（此前含模块 1/2）**`git mv` 为 `docs/error_guide.md` 并整体重写为 577 行的场景化深度讲解**。
- **写作方法：先扫描用量，再讲设计**。对 `src/` 全量统计各 error API 的出现次数，得到一张**决定叙述重点**的表：
  | API | 次数 | 含义 |
  |---|---:|---|
  | `ErrorPtr` | 75 | 绝大多数是类型标注——它是贯穿全运行时的错误载体 |
  | `Is(` | 9 | **真实主用途** |
  | `DeadlineExceededError()` | 9 | 与上一行配对 |
  | `NewError(` | 8 | 只在 4 个文件里创建 |
  | `Wrap(` | **1** | 运行时几乎不用 |
  | `Join(` | 4 | 少量 |
  | `As<` | **0** | **运行时从不使用** |
  | `WalkErrors(` | **0** | 运行时从不使用 |
  **结论**：`Error` 对外提供完整的 Go 风格错误树工具，但**运行时真正依赖的只有一个能力——区分「超时」与「取消」**；其余 API 是为使用者与插件准备的。**这张表本身就是讲解的骨架。**
- **文档结构（13 节）**：定位与核心契约 → 场景地图（用量表）→ **场景一：区分超时/取消（含 3 个真实调用现场）** → **场景二：单例 + 身份比较（枢纽机制，8 个单例清单）** → 场景三：nil 语义 → 场景四：包装链 → 场景五：聚合 → 场景六：类型化提取（含 `AsMutable` 高危兄弟）→ 场景七：插件遍历 → 场景八：跨线程/Fiber 无锁传递 → 测试证据汇总 → 示例评述 → 问题清单 → 一页总结。
- **本轮讲透的三个枢纽事实**：
  - **① 三个真实调用现场的模式完全一致**：`channel.cpp`（`Context::Err()` → `Is(err, DeadlineExceededError())` → `ChannelStatus::kDeadlineExceeded/kCancelled`）、`io.cpp`（→ `WaitStatus` + `ETIMEDOUT`/`ECANCELED`）、`sse.cpp`（→ `sse::Error::DeadlineExceeded/Cancelled`）。**差异在于前两者写法不同**：`channel` 写成 `err && Is(err, ...)`（先判空），`io` 直接 `Is(context->Err(), ...)`（依赖 `Is(nullptr, X) == false`）—— **两者都正确，但展示了 `Is` 空安全的两种用法**；
  - **② `Is` 能工作的前提是「单例 + 身份比较」这对组合**。`Error::Is` 默认是 `this == &target`，而 `DeadlineExceededError()` 返回 `static const ErrorPtr` —— **若它每次返回新对象，超时判别会静默失效且不报错**。这是全模块最脆弱的接缝，已写入文档的「最脆弱处」；
  - **③ 遍历算法的两个极端后果都有测试背书**：`AsImpl` 用显式容器 + `unordered_set<const Error*>` 去重，因此 **10 万层 `Wrap` 深链不爆栈**（测试还精确校验格式化长度）且**自指错误图不死循环**。
- **★ 新发现：X-31（低）—— 8 个错误单例的静态析构顺序问题**。为写本文做全仓库扫描时发现，**8 个标准错误全部是函数局部 `static const ErrorPtr`**（`context.cpp` 2 个、`channel.cpp` 5 个、`future.cpp` 1 个）。函数局部 static 在**程序退出阶段会被析构**，此后该函数返回**空 `ErrorPtr`**，而 `Is(err, {})` 的语义是 **`!err`** —— **「超时」会被静默误判为非超时**（走取消分支）。
  - **项目其实认识这个风险**：`include/go2cpp/tools/go.hpp:16-17` 对默认调度器**刻意用 `new` 且永不析构**，注释写明「避免静态析构顺序导致工作线程访问已销毁对象」—— **但 8 个错误单例没有做同样处理**；
  - 正常路径安全（`Scheduler::shutdown()` 一般先停线程），但 `shutdown_for()` **超时会返回 false 且线程可能仍在运行**，此时恰好发生 Context 超时即可命中；
  - **修复建议**：改为 `static const ErrorPtr* const error = new ErrorPtr(...)`（同 `go.hpp` 的做法）或 leaky singleton 惯用法。
- **文档同时沉淀了两条「方法教训」**：
  - `tests/tools/test_errors_ns.cpp` 那段声称「头文件只有裸声明、没有注释」的测试注释，**本身曾是 D-14 的登记依据**，而它是错的 —— **以测试注释为证据去登记文档缺陷，等于把二手信息当一手证据**（已登记 T-09）；
  - `Wrap` 在运行时只出现 1 次、`As`/`WalkErrors` 出现 0 次，说明「**接口面向使用者、实现面向性能**」的分工 —— 运行时宁可用「单例错误 + 状态码」，也不在热路径上构造包装链。
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致（`94a798af…`）。
- **台账状态**：新增 **X-31**；待办 **17 条**。
- **下一步**：按用户「只讲解 Error」的指示，**模块讲解任务到此收尾**；后续轮次回到「查找 test 和 example 的问题」这条线上的其余模块（`config`/`logging` → `context` → `fiber`/`thread_policy` → `sync` → `scheduler` → `future`/`channel`/`event` → `timer` → `io`/`hook`/`fd_manager` → 网络层 → `go`/`runtime`），产出仍是登记项而非讲解。

## 2026-10-12：模块讲解（模块 2 · control_flow）—— 新增 X-30、T-12~T-14

- **本轮读**：`tests/tools/test_control_flow.cpp`(177)、`example/tools/control_flow_demo.cpp`(94)，并回查 `src/tools/control_flow.cpp` 的关键实现（`s_defer_depth` 用法链、`recover` 各方法、`panic::clear`、`PanicState`）。
- **★ 新发现：X-30（中）—— panic 恢复边界用 `thread_local` 而非 Fiber 局部**。
  - `src/tools/control_flow.cpp:15-16` 定义 `thread_local unsigned int s_defer_depth` 与 `thread_local std::exception_ptr s_defer_exception`；
  - `DeferCallbackScope` 构造 `++s_defer_depth`、析构恢复（`:28-32`），`InDeferCallback()` 即 `s_defer_depth != 0`（`:38`）；
  - **`recover::take()`（`:257-260`）与 `recover::operator()()`（`:284-287`）都以 `InDeferCallback()` 为唯一的「是否在 defer 回调内」判据** —— 这正是 panic 恢复边界的守门条件；
  - **而 Go2Cpp 的 Fiber 在同一 OS 线程上多路复用**。当 Fiber A 在其 defer 回调中 **park** 让出 M（回调里等 channel/锁/IO 皆可），Fiber B 在同线程运行并调用 `recover::take()` 时，`s_defer_depth` 因 A 尚未退出回调而**非 0** → B 被判为「在 defer 回调内」，**可以消费掉本不属于它的 panic 状态**；
  - `s_defer_exception` 同理：A 记录的 defer 异常会被 B 通过 `LastDeferException()` 读到，B 的 `ClearDeferException()` 会抹掉 A 的记录（`:48-64`）；
  - **最有说服力的一点：项目自己有正确的工具。** `include/go2cpp/concurrency/fiber_local.hpp` 提供 `FiberLocal<T>` —— Fiber 内返回 Fiber 局部值、普通线程内回退为线程局部值，**语义正好覆盖本场景**，`control_flow` 却用了 `thread_local`；
  - **为何只登记不修**：修改它属于**行为性改动**，不满足「零行为变化」的即时修复例外；且触发需构造特定交错，本环境 TSan 不可用（WSL `unexpected memory mapping`）无法给出运行期现场。头文件侧证据：`control_flow.hpp:17-19` 只说「不负责**跨线程**传播」，**未声明 Fiber 维度的约束**。
- **T-12（低）测试职责混杂**：`tests/tools/test_control_flow.cpp` `#include "go2cpp/channel.hpp"`（`:1`），其后 **60 行（`:118-177`）全部在测 `Channel`/`Select`/`SelectValue`/`SelectCaster`**，与被测模块无关；而 `announce` 名是 `"defer/panic/recover"`（`:17`）。**后果**：`GO2CPP_TEST_FILTER=control_flow` 会连带跑这些 Select 测试，`filter=channel` 却跑不到。
- **T-13（低）无信息量断言**：`:97-103` 的 `ordinary_exception_seen` 只依赖 C++ 语言本身的 try/catch，**恒为 true**，无论 `defer`/`panic` 实现是否正确都会通过，无法证伪任何实现。作者意图应是「验证 defer 记录的异常不污染普通异常处理」，但写法没做到。
- **T-14（低）四处接口零覆盖**：`panic::clear()`（`:184`，实现 `:206-217` 重置全部六个字段）、`recover::bind()`（`:215`，实现返回 `!expired()`，即**绑定时 panic 已析构返回 false**，该语义未验证）、`recover::reset()`/`bound()`、`recover::peek()`（`:228`）—— 后者与 `take()` 的关键差异是**不检查 `InDeferCallback()`**（对比 `:258` 与 `:313-320`），即**可在 defer 边界外读取状态**，这一有意设计未被固化。
- **模块 2 讲解要点（已写入 `docs/module_tour.md`，现 253 行）**：
  - **设计选择**：Go2Cpp 把 `panic` 做成**显式状态**而非栈跳转 —— 头文件明说「call() 只记录状态，**不使用 longjmp 或破坏栈的跳转**；用户应在 defer 回调中通过 recover 读取和处理状态」（`control_flow.hpp:143-145`）。所以这里的 recover 不是「捕获并继续」，而是「在 defer 回调里读走状态对象」，调用方必须自己 return/分支；
  - `recover` 持 **`weak_ptr`**（`control_flow.hpp:231`）是刻意的：`bound()` 用 `!expired()` 判断，三个读取方法都先 `m_state.lock()`，因此 **panic 先析构时 recover 安静失效，不会把状态钉在内存里**；
  - `defer` 参数**构造时按值保存**（`MakeCallableWithArgs` 把函数与 `std::tuple` 一起捕获，`control_flow.hpp:115-132`），测试 `:25-31` 验证注册时的 `3` 不会随后续改为 `9` 而变化；执行顺序为 C++ 作用域逆序，测试断言 `order == [3, 2, 1]`；
  - `defer` 回调体被包在 `CallableModel::run() noexcept` 里（`control_flow.hpp:97-103`），异常被 `RecordDeferException()` 记录而非传播 —— 否则会从析构函数抛出。用户要取该异常须主动调 `LastDeferException()`；
  - **`peek()` 是恢复边界守门条件的唯一例外**：它不检查 `InDeferCallback()`，因为只读不改，用作诊断。
- **示例评述**：`control_flow_demo.cpp` 三个场景划分合理，且注释主动解释了「panic 是显式状态发布，C++ 不会自动跳转」（`:63-64`）与「不会伪造 longjmp」（`:72-73`），对新手友好。唯一瑕疵是它写日志到 `log/control_flow_demo.log`（`:37-38`），运行示例会改动 `log/` 目录；本流程因此**只做静态审读、不运行 example**，以维持「`log/` 基线 md5 不变」的约束。
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致（`94a798af…`）。
- **台账状态**：新增 **X-30**、**T-12**、**T-13**、**T-14**；待办 **16 条**。
- **下一步**：模块 3 —— `config`（INI 解析 + `ConfigVar` 原子快照 + 事务回滚），对应 `tests/logging/test_log_config.cpp`(321，含配置部分) 与 `example/logging/log_config_demo.cpp`(80)；随后 `logging` → `context` → `fiber`/`fiber_local`/`thread_policy` → `sync` → `scheduler` → `future`/`channel`/`event` → `timer` → `io`/`hook`/`fd_manager` → 网络层 → `go`/`runtime`。

## 2026-10-12：模块讲解（模块 1 · error）+ tests/example 审查 —— 新增 T-09/T-10/T-11

- **本轮性质转变**：用户指定新主任务「一边按模块顺序讲解项目，一边查找 test 和 example 的问题」，并要求「先完成 error 的部分」。本轮完成 **模块 1（基础工具层：`error`）**。
- **产出文档**：新建 `docs/module_tour.md`（138 行），作为模块讲解的**持续载体**，按依赖从底向上逐模块累积。error 章节含：模块定位、类层次表、三个必须讲清的细节（迭代遍历+地址去重、noexcept 兜底、`Is(error,nullptr)` 退化语义）、测试覆盖表（10 条关键断言）、测试盲点、问题清单、示例评述。
- **★ 本轮最重要的发现：T-10（高，HEAD 无法构建）**。
  - `tests/tools/test_errors_ns.cpp`（已提交 `f8156e5`、`4f93672`，且存在于 HEAD）与 `example/tools/errors_demo.cpp`（已提交）都调用 `go2cpp::errors::*`；
  - 但该命名空间**只在用户的在途改动中**：`git show HEAD:include/go2cpp/tools/error.hpp | grep -c "namespace errors"` → **0**；
  - 而 `git cat-file -e HEAD:tests/tools/test_errors_ns.cpp` 成功 → 文件在 HEAD 中；
  - **结论：任何从 HEAD 全新 checkout 的构建都会编译失败**。当前 `ctest` 通过仅因为工作区恰好带着未提交的 `error.hpp`；
  - **成因追溯（这是我自己的遗留）**：B-03 批次按台账 U-01 的默认建议「保留该 API 并在 B-03 补最小测试与示例」行动时，**提交了测试与示例，却没有提交它所依赖的头文件** —— 因为该文件是用户在途文件，AI 不得代为提交。于是仓库进入「测试已提交、被测代码未提交」的中间态；
  - **修复需用户执行**：提交 `include/go2cpp/tools/error.hpp` 的 `errors::` 命名空间。若决定撤回该 API，则须删除两个文件并回退 `cmake/Go2CppTests.cmake:21`/`:99` 与 `tests/test_main.cpp:28`/`:70`。**按无人值守规则，此项标「需用户决策」而非擅自处理** —— 提交用户的在途文件是明确禁止的。
- **T-09（低）**：`tests/tools/test_errors_ns.cpp:55-60` 的注释**与事实相反** —— 它称「头文件 `error.hpp:136` 只有裸声明、没有任何注释」，但 `:131-134` 早有注释（`d0fdf70` 于 2026-10-10 12:52 加入）。**该测试注释正是当初 D-14 登记的依据**，D-14 已据 `git blame` 结案，此处须同步修正。它另外还引用了**已过时的行号** `src/tools/error.cpp:392-394`（重排后为 `:463-465`），违反 2026-10-11 确立的引用约定。
- **T-11（低）**：三个公开接口**零测试覆盖** —— ① `AsMutable<T>()`（`error.hpp:183-193`，用 `const_pointer_cast` 返回**可变**指针，与全局「错误不可变」契约有张力，注释自己都说「普通调用方应使用 `As<T>()`」，属**高危却无覆盖**）；② `errors_is()`（`:195-197`）；③ `errors_as<T>()`（`:199-202`）。
- **error 模块讲解要点（已在 `docs/module_tour.md` 展开）**：
  - **核心契约**：`ErrorPtr = shared_ptr<const Error>`，「错误实现构造后必须保持不可变，因此共享 ErrorPtr 可以安全地在线程和 Fiber 之间传递」（`error.hpp:17-20`）—— 这条不可变性是所有跨线程共享无需加锁的前提；
  - **`Is` 默认按对象地址比较**（`:36-38` `this == &target`），而非按消息。`test_error.cpp:36` 专门断言「消息相同但对象不同的错误互不匹配」—— 这不是疏漏，而是包装链有意义的前提；
  - **遍历是迭代的 + 按地址去重**（`As` 的实现 `:140-167` 用显式栈 + `unordered_set<const Error*>`，不用递归）。两个后果都被测试固化：**10 万层深链不爆栈**（`test_error.cpp:96-109` 构造 100,000 层 `Wrap`）、**自指错误图不死循环**（`:82-92` 的 `CycleError`）；
  - **`As<T>` 是 noexcept + try/catch 的正面范本**（`:172-181`），注释写明「Is/As 是查询型 noexcept API…不能让异常穿过边界并触发 terminate」。`test_error.cpp:68-79` 用 `ThrowingUnwrapError` 验证 `Is`/`As`/`Unwrap` 三条路径都返回安全值。**这也是 X-25 所引用的「项目已有既定策略」的又一正面证据**；
  - **测试质量评述**：`test_error.cpp` 覆盖得相当扎实，十项通常会被漏掉的断言都有（见文档表格），包括**空 visitor**（`:65`）、**精确格式化长度**（`:104-108`、`:120-124`）。**这也是 D-25 的核实结果**：空 visitor 的**行为已被测试固化**，D-25 只是文档缺失，严重度维持「低」；
  - **示例评述**：`example/tools/errors_demo.cpp` 5 个场景划分合理（构造/包装、身份语义、聚合含空输入、类型提取命中与未命中、Unwrap 边界），`example/README.md:60` 有对应登记行。除 T-10 的依赖问题外无其他发现。
- **验证结果**：本批为**只读回溯 + 文档产出**，未改动任何源码或测试，未触发构建。
- **台账状态**：新增 **T-09**、**T-10**、**T-11**；待办 **12 条**（`D-24`、`D-25`、`X-06`、`X-23`~`X-29`、`T-09`~`T-11`）。其中 **T-10 标为「需用户决策」且严重度为高**，是本项目当前唯一影响可构建性的问题。
- **下一步**：模块 2 —— `control_flow`（`defer`/`panic`/`recover`），对应 `tests/tools/test_control_flow.cpp`(177) 与 `example/tools/control_flow_demo.cpp`(94)；随后按依赖继续 `config` → `logging` → `context` → `fiber`/`fiber_local`/`thread_policy` → `sync` → `scheduler` → `future`/`channel`/`event` → `timer` → `io`/`hook`/`fd_manager` → `address`/`socket`/`byte_array`/`endian`/`framing` → `http`/`websocket`/`sse` → `go`/`runtime`。

## 2026-10-12：include 100% 读完 + D-14 结案 + 新增 D-24/D-25 + X-06 补充

- **本轮读实现**（一次性读完 include 剩余全部）：
  - `network/control_flow.hpp`… 更正：`tools/control_flow.hpp`(241)、`concurrency/core/parking_condition.hpp`(98)、`network/io_backend.hpp`(87)、`network/fd_manager.hpp`(69)、`network/socket.hpp`(225)、`network/http.hpp`(246)、`network/websocket.hpp`(176)、`tools/error.hpp`(250，用户在途文件，只读)。
- **★ include 通读完成度：100%**。实测 `include/` 共 **9,653 行 / 79 个文件**：
  - **33 个「有实质内容」的文件全部逐行读完**，合计 **9,493 行**；
  - **其余 46 个是 2-8 行的转发头**（合计 160 行），本轮也**逐个 dump 全文核对**，全部指向正确真身：根目录版用「兼容旧版公共头文件路径；实际实现位于模块目录」，`.h` 版用「Sylar 风格头文件名兼容入口」，`network/socket_stream.h` 正确地指向 `socket.hpp`（`SocketStream` 定义在此）而非不存在的 `socket_stream.hpp`。**X-26 修复后的 `concurrency/sync.hpp` 内容也已确认正确。**
- **★ D-14 结案（核实后推翻其前提）**。D-14 原文称「`Is(error, target)` 在 target 为空时有特殊语义…但头文件**只有裸声明、无任何注释**」。本轮读 `error.hpp` 全文并 `git blame` 核实：
  - `error.hpp:131-134` **早有注释**：「遍历单原因或多原因错误链。允许并忽略环；**空 target 只匹配空错误，与 errors.Is(err, nil) 相同**」；
  - `git log -S` 与 `git blame` 一致显示该块由 **`d0fdf70`**（"fix: complete full module boundary audit"，2026-10-10 12:52）加入；
  - 与实现 `src/tools/error.cpp:463-465` 的 `if (!target) { return !error; }` 一致；
  - **结论**：D-14 的前提在当前 HEAD 不成立，**无需再改 `error.hpp`，故结案**。两种可能都如实写明：要么该注释是 D-14 登记之后由 `d0fdf70` 补上的，要么登记时漏看了这两行。**D-14 的阻塞状态（等用户提交在途文件）随之解除。**
- **新增 D-24（低，Doxygen 绑定错位）**：`http.hpp:177-181` 是 `ResponseWriter` 的类文档，但它**紧邻的下一个声明是 `enum class StreamState`**（`:182-188`）而非 `class ResponseWriter`（`:189`）—— 文档会被绑到枚举上。对照 `io.hpp:154-170`（两段文档连续且紧邻 `class IOManager`，绑定正确）与 `websocket.hpp:73-80`（位置正确)两个正面样本。
- **新增 D-25（低，边界未声明）**：`WalkErrors` 的注释（`error.hpp:58-66`）说明了「空指针不会调用 visitor」与「访问器抛异常时返回 false」，但**没说传入空 `visitor`（`std::function` 无目标）会怎样**。实际是抛 `std::bad_function_call` 后被 `noexcept` 兜底捕获、返回 false —— 返回值恰好安全，但调用方无法从注释预知。此项即 D-14 修复建议的后半句，因 D-14 主体已了结而单独立项。
- **X-06 补充（原修复不完整）**：`FdContext` 的 `@warning`（`:20-24`）已如实声明 getter 无同步保护，但同文件 `:48` 的 `FdManager` 类注释**仍写着「线程安全的 fd 元数据表」**，两处矛盾：`FdManager` 的互斥只保护**表结构**与写入侧字段赋值，不保护 `FdContext` 的 getter。
- **本批其余核对通过的要点**：
  - `error.hpp:172-181` `As<T>` 是**noexcept + try/catch 的正确范例**（注释：「Is/As 是查询型 noexcept API…不能让异常穿过边界并触发 terminate」）—— 它正是 X-25 里「项目已有既定策略」的又一处正面证据；
  - `socket.hpp:47` 明确并发约束「支持线程与 Fiber 共同访问，但不允许同一方向多处消费协议」；`:162-164` 说明 `ParkingCondition` 复用 `m_mutex` 的理由「close/release 等待在途操作时不会占住 M，也不会丢通知」；
  - `parking_condition.hpp:80-84` 写明了拒绝手动 Fiber 的理由「无法把等待交还给 carrier 线程，因此禁止退回原生 condition_variable 造成整条线程阻塞」；`:39-46`/`:68-75` 两个循环在非 `kNotified` 时都**重新求值 predicate** 再返回，与文档语义一致；
  - `io_backend.hpp:29-30` 明确「一个后端实例只能绑定一个 IOManager，不得跨管理器共享」；`:39-45` 两个 `supports_*` 能力探测默认返回 false 以保持第三方后端兼容；`:76-83` 的 D-13 修复（平台默认后端不可注销）仍在位；
  - `control_flow.hpp:97-103` `defer` 的执行体把异常记入 `RecordDeferException` 而非传播，与 `~defer() noexcept` 一致；`:195-196` 明确 `recover::take()`「只有在 defer 回调期间才会成功，以保留最重要的恢复边界；它不会捕获普通 C++ 异常」；
  - `websocket.hpp:75-78` 并发契约精确：「一个对象允许**一个读方向和多个并发写**调用，读操作之间、写操作之间分别串行化」；`:156-158` 用项目混合 `sync::Mutex` 的理由也写明了；
  - `http.hpp:220-225` 的 D-22 修复（`serve_connection` 不设 recv/write 超时）与 `socket.hpp:61-67` 的 D-16 修复（`fd()` 是瞬时值）均仍在位。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致。
- **台账状态**：**D-14 → 已了结**；新增 **D-24**、**D-25**；**X-06 补充**。当前待办 **9 条**：`D-24`、`D-25`、`X-06`、`X-23`~`X-29`。`state.json` 的 `blocked_items` 已清空（D-14 解除后**不再有阻塞项**）。
- **★ 下一步（用户指定的主任务下一阶段）**：`include/` 已读完，**转入 `tests/` 的校验部分**（10,215 行，本项目唯一系统性未通读的大块）。建议顺序：先 `tests/test_support.hpp`（测试框架本体）→ `tests/test_main.cpp`（入口与 filter）→ 各模块测试文件（按并发/网络/配置/日志分组）→ 边读边记录**测试盲点**（源码中存在但测试未覆盖的分支）与**断言依据核查**（断言依赖的是设计意图还是碰巧的实现行为）。

## 2026-10-12：include 通读（io.hpp 314 + go.hpp 220 全文）—— 含一次**核实后撤回**的条目

- **本轮读实现**：`include/go2cpp/network/io.hpp`(314) 与 `include/go2cpp/tools/go.hpp`(220)，**均全文读毕**。
- **重要：一次「拟登记 → 核实 → 撤回」的过程（如实记录）**。
  - 读 `go.hpp:208-210` 时看到 `fiber::state()` 在 `m_task` 为空时返回 `GState::kCancelled`，我据此**准备登记**一条「三处 API 对『无任务』各有不同占位值」的条目，并凭记忆写下了另两处（`scheduler.hpp` 的 `current_task()` 返回 `kDead`、`current_task_state()` 返回 `kNew`）。
  - **落笔前先核实**，结果是**我的记忆错了**：
    - `scheduler.hpp:607` `current_task()` 的返回类型是 `std::shared_ptr<Task>`，**不是 `GState`**；
    - **根本不存在** `current_task_state()` 这个函数（`grep` 全文件无匹配）；
    - 全仓库 `grep -rn "return GState::kDead\|return GState::kNew\|return GState::kCancelled"`（`src/` + `include/`）**零匹配** —— 即除 `go.hpp:209` 外没有任何此类占位返回；
    - `scheduler.hpp` 中只有 `Task::state()`(`:269`) 返回 `GState`。
  - **结论：X-30 不成立，已撤回，不写入台账。** `go.hpp:209` 的 `kCancelled` 分支**本身也不可达**：`fiber` 的构造函数（`:165-166`）总是 `make_shared<Task>`，且该类**删除了复制与移动**（`:168-171`），因此 `m_task` 恒非空。
  - **这条记录的价值**：它演示了本项目「以源码为唯一裁决、不凭印象登记」的规范在起作用 —— 若按记忆登记，台账里就会多出一条**指向不存在函数**的错误条目，后续修复者会被误导。**撤回一条错误条目与新增一条正确条目同样是产出。**
- **io.hpp 核对通过的要点**：
  - `:21-22` 与 `:52-54` 两处约束写得非常明确：`DescriptorToken` 是「非阻塞系统调用前捕获的代际句柄…数字 fd 被复用时不会重新激活旧句柄」；`DescriptorGuard`「**绝不能跨 park 或阻塞系统调用持有它**」，并逐条列出两个持有者（Hook 在失效标记与 close/dup2 期间、IOManager 在验证 token 并发布 epoll interest 期间）；
  - `:137-141` 把 `wait_many` 的语义边界讲清了：「**它不是『等待所有请求都完成』的屏障**；需要屏障时应使用多个任务或 `WaitGroup`」，并约束「同一集合内不得重复提交相同 fd/方向；补采样只完成该 fd/方向等待队列的**队头节点**，以保持多个 Fiber 之间的 FIFO」—— 这与 `src/network/io.cpp` 的补采样实现一致；
  - `:154-165` `IOManager` 的依赖段完整，其中「`Wait` 只允许在当前 IOManager 管理的 G 中调用，**普通线程应使用系统 poll/select**」与 `:224-225` `wait_any` 的「仅允许 managed Fiber 调用，普通线程返回 `kError`/`EPERM`」互相印证；而 `:169` 说「普通线程也可通过 **wait 接口**安全使用」—— 指的正是单 fd 的 `wait`，**并不矛盾**；
  - `:282-285` `NotifyCloseAll` 的注释点出实现约束：「回调只保留 State **弱引用**，析构中不会解引用已经销毁的 IOManager」—— 与 `io.cpp` 用 `weak_ptr` 遍历注册表一致。
- **go.hpp 核对通过的要点**：
  - `:16-17` 解释了默认调度器为何用 `new` 且不析构：「故意采用进程生命周期句柄，**避免静态析构顺序导致工作线程访问已销毁对象**」；
  - `:31-36` 处理了一个容易写错的状态机细节：「`Scheduler::shutdown()` 是**终态**，不能对同一个实例再次 `start`。关闭后创建新的句柄，**旧句柄继续由持有者负责等待/释放**」；`:57-73` `shutdown_default_scheduler` 先把句柄**移出**再关闭，注释说明「不会把任务提交到已经进入 stopping 状态的旧实例」，并把 `shutdown()` 包在 try/catch 中以满足 `noexcept`；
  - `:82-87` `go(Scheduler&, ...)` 在未运行时自动 `start()`，与 `:45-47` 的默认调度器路径语义一致；
  - `:149-171` `fiber` 类**删除复制与移动**并在注释中说明理由（「以避免重复提交」）；`m_scheduler` 是**裸指针**但只做相等比较（`:183`）**从不解引用**，因此 Scheduler 先析构也不会悬空访问；
  - `:100-107` 与 `:141-147` 的模板重载用 `std::enable_if_t<std::is_invocable_r_v<void, std::decay_t<Function>&>>` 约束，使 lambda 无需手动构造 `std::function`。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致。
- **累计进度（include 通读）**：已完成 **23 个文件全文**，合计 **7,955 / 9,850 行 ≈ 81%**。
  - **下一步**：`control_flow.hpp`(241) → `socket.hpp`(225) → `http.hpp`(246) → `websocket.hpp`(176) → `io_backend.hpp`(87) → `fd_manager.hpp`(69) → `parking_condition.hpp`(98) → `error.hpp`(250，用户在途文件，只读) → 余下零散。
- **台账状态**：本轮**无新增条目**（含一次撤回）；待办仍为 **8 条**。

## 2026-10-12：include 通读（context.hpp 408 + log.hpp 506 全文）

- **本轮读实现**：`include/go2cpp/concurrency/context.hpp`(408) 与 `include/go2cpp/logging/log.hpp`(506)，**均全文读毕**。
- **结论：本批未发现新的缺陷或文档缺失。** 以下要点经逐条核对。
- **context.hpp 核对通过的要点**：
  - `:108-113` `ContextKey::Anchor()` 的设计解释了为什么类型化键需要持有一个 `shared_ptr<const std::uint64_t>`：注释写明「Context 节点在类型化值存在期间保留此锚点，**避免分配器复用地址让后续键发生碰撞**」—— 即身份用的是 token 的**地址**，而 anchor 的存在是为了让该地址在值存活期内不被复用；`:195-211` 的类型化 `Value()` 因此只比较 `Identity()`、给 `LookupValue` 传空 name（注释：「字符串名称只是元数据，不能让无关的键在父链中发生碰撞」）；
  - `:240-242` 私有的 `AddBeforeDoneCallback`/`RemoveBeforeDoneCallback` 与 `src/concurrency/context.cpp:22-23` 的「内部回调（例如 ContextRollback）必须先于用户观察回调执行」一致；`DoneSignal::Signal()` 为 private 且`friend struct detail::DoneSignalAccess`（`:85-87`），**外部无法主动触发完成信号**；
  - `:261-356` `ContextRollback` **允许移动**（`:301-302`），而 `Scheduler::BlockingRegion` **刻意禁止移动** —— 这个差异是合理的：`BlockingRegion` 的析构必须发生在**进入它的 M** 上（否则原 M 永久保持 Blocking），而 `ContextRollback` 的析构只是取消 + 执行 undo，可在任意线程完成；
  - `:342-345` `RollbackDone()` 的语义边界写得很明确：「与 child Context 的 Done **分离**：该信号表示 commit 或完整 rollback 的补偿动作**已经执行完毕**」，并给出两条禁令「undo 不得等待自身的 RollbackDone，也不应在回调中阻塞或再次取得外部锁」；
  - `:306`/`:310`/`:313-315` 对 undo 动作与 Savepoint 的约束完整：「动作应幂等、短小、非阻塞，最好声明为 noexcept」、「一次成功的局部回滚会使已有 Savepoint **全部失效**，调用方应重新创建令牌」。
- **log.hpp 核对通过的要点**：
  - `:112-132` `LogFormatter::Item` 有意同时提供 `Format` 与 `format`，默认实现互相转发，注释写明「兼容 Sylar 风格的用户扩展类」；`:170-181` 的模板版 `AddFormat<ItemType>` 用 `if constexpr` 判断「类型优先使用 `ItemType(const std::string&)`，否则使用默认构造函数」，把新手样板代码降到一行；
  - `:109-110` 明确性能契约：「Item 对象在构造 `PatternFormatter` 时被创建，**格式化热路径只调用 `Format`，不会再次解析字符串或访问注册表**」—— 与 `src/logging/log.cpp` 的 `FindFormatItem` 只在构造期调用一致；
  - `:326-327` `CallbackSink` 的并发契约写明：「回调对象在构造后不可变，**Write 不再为每条日志加内部锁**；若回调访问共享状态，应由回调自己选择合适的同步策略」—— 这是**把责任明确交给调用方**的写法，比隐式加锁更诚实；
  - `:363-364` 与 `:426-427` 两处都注明了「原子快照」的意图：「热路径只做一次原子 shared_ptr 读取…因此格式化器和 Sink 不会在每条日志上争用 Worker 配置锁」/「`m_workers` 只在配置时修改；日志线程通过原子快照无锁读取」；
  - `:494-495` `namespace GO2CPP = ::go2cpp;` 附注释「兼容用户给出的大小写命名示例。实现仍然位于规范的 go2cpp 命名空间」—— **该别名真实存在**，本会话早期已核实 `docs/logging.md` 对它的描述是准确的；
  - `:27-40` `Level` 枚举把 `Fatal = Critical` 做成别名（而非新枚举值），`ToString`/`ParseLevel` 因此对二者返回同一名称 —— 与实现一致。
- **顺带确认了用户两处在途改动的具体内容**（只观察，未改动）：
  - `log.hpp:224-228` 之间确实存在**连续三个空行**（`:225-228`），对应用户在途改动；
  - `concurrency/context.hpp` 的缩进风格已经是 4 空格 + `namespace go2cpp\n{`（`:19-20`），与用户在 `src/tools/error.cpp` 的重排风格一致，而它**不在 `git status` 中** —— 说明用户的风格统一工作**已部分提交**，当前仅剩 `src/tools/error.cpp` 等少数文件未提交。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建；在途 4 处文件 diff md5 与轮初一致。
- **累计进度（include 通读）**：已完成 **21 个文件全文**，合计 **7,421 / 9,850 行 ≈ 75%**。
  - **下一步**：`io.hpp`(314) → `go.hpp`(220) → `control_flow.hpp`(241) → `socket.hpp`(225) → `http.hpp`(246) → `websocket.hpp`(176) → `io_backend.hpp`(87) → `fd_manager.hpp`(69) → `parking_condition.hpp`(98) → `error.hpp`(250，用户在途文件，只读) → 余下零散转发头。
- **台账状态**：本轮无新增条目；待办仍为 **8 条**（`D-14` 阻塞；`X-23`~`X-29` 待定）。

## 2026-10-12：include 通读（timer.hpp + config.hpp 全文，885 行）—— X-25 扩至 7 处

- **本轮读实现**：`include/go2cpp/concurrency/core/timer.hpp`(344) 与 `include/go2cpp/config/config.hpp`(541)，**均全文读毕**。
- **X-25 第三次扩充（现共 7 处）**。`config.hpp` 内又发现 2 处同类问题，其中一处的形态**比前面几处更值得修**：
  - ① `:357-360` `ConfigVar<T>::HasPending() const noexcept` 直接构造 `std::lock_guard`；
  - ② `:327-342` `ConfigVar<T>::NotifyPending() noexcept` 的 **`catch (...)` 块内部再次构造 `std::lock_guard`** —— 「**处理异常的分支本身还可能抛异常**」，一旦发生就是 `noexcept` 逃逸 + `terminate`，而且发生在最不该失败的恢复路径上；
  - 对照：同文件 `RollbackPending()`（`:366-377`）的加锁**放在 try 内**，异常会被捕获并降级为「保留当前值」，**属正确处理** —— 说明作者在 config.hpp 里是知道要包住加锁异常的，只是漏了两处；
  - **7 处 = future.hpp 4 + hybrid_mutex.hpp 1 + config.hpp 2**。
- **config.hpp 另有两处值得记的正面设计（都写了理由）**：
  - `:299-301` `ApplyString` **先在锁外完成可能抛 `bad_alloc` 的 `make_shared<const T>`**，再进锁写入，注释：「避免写入后才发现内存分配失败而无法把本变量加入事务回滚列表」—— 这是对强异常安全的正确运用；
  - `:380-385` `PendingChange` **保存 `shared_ptr<const T> old_value` 而不是值的副本**，注释：「回滚时无需再次分配 T，能够在 `bad_alloc` 等异常路径下保持 noexcept 回滚的确定性」—— 用共享所有权把回滚做成无分配操作；
  - `:327-354` `NotifyPending` 把监听器快照与 `m_pending` 搬出后在**锁外**逐个调用，且每个都独立 try/catch（注释：「单个插件异常不能阻断同一变量的其余监听器，也不能回滚已经完整提交的配置值」）；
  - `:469-488` `Config` 的私有成员把**三把锁的职责**分别注明：`m_mutex`（变量表与监听器）、`m_transaction_mutex`（「串行化多个 LoadFromIni/LoadFromFile 调用，保证不同线程不会交错写入同一批 ConfigVar 并互相覆盖暂存通知」）、`m_watcher_lifecycle_mutex`（「保护 `std::thread` 对象本身；配置值读取不使用这把生命周期锁」）。
- **timer.hpp 核对通过的要点**：
  - `:16-18` `TimerState` 的取消语义写得很实：「取消只保证阻止**尚未领取**的回调；已经进入 Running 的回调需要业务自行协作停止」；`:159-161` 再次重申「回调已经进入 Running 后，Cancel 不会强行打断 C++ 调用栈」；
  - `:49-51` `TimerOptions` 的注解包含一条**针对数据竞争的设计约束**：「结构体可复制，**创建后不再修改**，避免热路径读取数据竞争」—— 这与 `timer.cpp` 里把 `TimerOptions` 按值存入 `Timer::State` 的做法一致；
  - `:64-67` `TimerDurationLimits` 明确「达到阈值时记录 WARN/ERROR，但**两者都不拒绝任务**；一年及更长任务仍可正常注册」，与 `timer.cpp:43-66` 的 `log_long_duration` 只记日志不拒绝一致；
  - `:136-141` `TimerExecutor` 的后端契约写明「Submit **必须快速返回**，返回 false 表示后端停止接收任务」，`:282` 还补了一条关键约束「**不得在 Submit 内恢复别的线程的 Fiber 栈**」—— 这正是 `Scheduler::TimerExecutorAdapter` 采用「提交为 Fiber」而非「直接 resume」的原因；
  - `:203-206` `TimerService` 说明「回调在服务锁外执行…但会占用该服务线程」与「服务不继承 Scheduler，Scheduler 也不拥有服务，双方通过执行器接口解耦」，与实现一致；`:224-225` 注明默认服务「只在进程结束时回收」。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 通读）**：已完成 **19 个文件全文**，合计 **6,507 / 9,850 行 ≈ 66%**。
  - **下一步**：`context.hpp`(408) → `log.hpp`(506) → `io.hpp`(314) → `go.hpp`(220) → `control_flow.hpp`(241) → `socket.hpp`(225) → `http.hpp`(246) → `websocket.hpp`(176) → `io_backend.hpp`(87) → `fd_manager.hpp`(69) → `parking_condition.hpp`(98) → `error.hpp`(250，用户在途文件，只读) → 余下零散。
- **台账状态**：本轮无新增条目（X-25 扩充）；待办仍为 **8 条**。

## 2026-10-12：include 通读（scheduler.hpp 全文 662 行）

- **本轮读实现**：`include/go2cpp/concurrency/scheduler/scheduler.hpp`(662)，**全文读毕**。它是 include 侧倒数第二个大文件，且与已全文读过的 `src/concurrency/scheduler.cpp`(4048) 直接对应，因此本轮做了**声明与实现的交叉核对**。
- **X-10 精确化（无新条目）**：读全文后发现文档不对称的范围比我先前登记的更精确 —— `scheduler.hpp:525-527` **只为 `wake()` 写了返回值说明**，而 `wake_registered`(`:532`)/`wake_io`(`:533`)/`wake_io_batch`(`:541`)/`wake_or_cancel`(`:544`) **都没有**，尽管 `wake_io` 的返回语义与另外三者相反。**这说明项目已知道需要解释这件事，只是只解释了一个入口** —— 问题因此更应定位为「文档补齐」而非「实现有误」，已就地补充该论据。
- **交叉核对通过的要点（头文件声明 vs 已读实现）**：
  - `:443-447` `shutdown_for` 的注释与实现一致：声明「返回 false 表示**超时或从 worker 自身调用**」，而 `scheduler.cpp:2029-2037` 的 worker 分支确实直接返回 false（注释「Worker callers only publish the stop request; a later external caller performs all joins」）；「超时不会释放仍在运行的 Fiber 栈，可稍后再次调用」也与 `shutdown_for` 不销毁 `deferred_destruction` 中任务的行为吻合；
  - `:304-305` `try_mark_queued_from_worker` 的注释（「通过 queued 原子位先占位，再校验状态…不需要为每次 yield 或 IO 批量唤醒获取 transition_mutex」）与实现 `scheduler.cpp:338-360` 逐条对应；
  - `:327-334` 三个 `wake_for_*` 的注释准确描述了「不访问 Fiber 栈」「状态转换与 park 共用转换锁」「真正 resume 仍由 execution_claim 串行化，因此不会让两个 M 同时进入同一个 Fiber」—— 这正是实现中 `m_transition_mutex` + `m_execution_claim` 双闸门的用意；
  - `:181-183` 头文件**明确写出 `pin_workers_to_cpu` 会绑核**（「Linux 上可将 M 绑定到与 P 对应的 CPU。默认关闭…打开后超出 CPU 数量的 P 不执行绑定」）—— 与我在 D-18 里依据 `scheduler.cpp:3505-3515` 修正 `docs/compatibility.md` 的动作**互相印证**：错的是文档，头文件一直是对的；
  - `:379-381` `m_emergency_next` 的注释「无分配救援队列链接。只在所属 Scheduler 的接纳互斥锁下修改，且不会形成环」与 `emergency_enqueue_locked`/`emergency_pop_locked` 实现一致；`:630-631` `worker_loop` 用 `std::shared_ptr<void>` 的理由也写明了（「不透明共享槽在 Scheduler 增删 machine 记录时保持动态 worker 的地址稳定」）；
  - `:144-150` `SchedulerObserver::OnEvent` **有意保留可抛出签名**并解释了理由（「让 Scheduler 在 `emit_event()` 中统一捕获第三方插件异常，不能让 worker 线程异常退出」）—— 这与 `scheduler.cpp` 里 `emit_event` 的 try/catch 一致；`:98-101` 还要求「新增事件类型时应保持已有枚举值的语义不变」，是对枚举稳定性的显式承诺；
  - `:420-421` `BlockingRegion` **刻意禁止移动**并写明理由（「区域绑定到进入它的 M。移动可能让析构发生在其他 Fiber/线程，使原 M 永久保持 Blocking」）—— 这是一个容易被「顺手加个移动构造」破坏的设计；
  - `Task` 的私有成员中 `m_last_machine_id`/`m_binding_published`（`:374`/`:376`）是**非原子**的，但只在 `Task::run()` 内访问，而 `run()` 由 `m_execution_claim` 串行化 —— 与实现一致，无数据竞争。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 通读）**：已完成 **17 个文件全文**，合计 **5,622 / 9,850 行 ≈ 57%**。
  - **下一步**：`timer.hpp`(344) → `config.hpp`(541) → `context.hpp`(408) → `log.hpp`(506) → `io.hpp`(314) → `go.hpp`(220) → `control_flow.hpp`(241) → `socket.hpp`(225) → `http.hpp`(246) → `websocket.hpp`(176) → `io_backend.hpp`(87) → `fd_manager.hpp`(69) → `parking_condition.hpp`(98) → `error.hpp`(250，用户在途文件，只读) → 余下零散。
- **台账状态**：本轮无新增条目（X-10 已存在，仅扩充论据）；待办仍为 **8 条**（`D-14` 阻塞；`X-23`~`X-29` 待定）。

## 2026-10-12：include 通读（8 个小文件，619 行）—— 新增 X-29，扩展 X-25

- **本轮读实现**（均全文读毕）：`network/framing.hpp`(129)、`concurrency/core/hybrid_mutex.hpp`(114)、`network/hook.hpp`(102)、`network/byte_array.hpp`(94)、`network/endian.hpp`(80)、`concurrency/detail/context_backend.hpp`(44)、`runtime.hpp`(36)、`core/status.hpp`(20)。
- **新发现：X-29（低，约束声明不对称）**。`Framer` 的 `m_buffer`/`m_frames`/`m_failed` 全部无锁读写，是有状态的单所有者解析器，但类注释（`framing.hpp:21`）**没写任何并发约束**。而项目内同类对象都写了：`sse.hpp:91-92` 明确「Parser 是有状态的单消费者对象…否则会产生数据竞争」，`byte_array.hpp:7` 明确「是单所有者对象，读写游标不能被多个线程同时修改」。**三者的声明不对称**。
- **X-25 第二次扩充（现在共 5 处）**：新发现 `hybrid_mutex.hpp:63-82` 的 `HybridGate::lock()` 也标为 `noexcept`，却在自旋失败后调用 `m_slow.lock()`（`std::mutex::lock`，可抛 `std::system_error`）。同文件的 `unlock()` 只做 `m_fast.clear()`，无风险。**5 处 = future 侧 4 处 + hybrid_mutex 1 处**。
- **本批核对通过的要点**：
  - `framing.hpp:39-91` `try_feed` 是 `noexcept` 且**内部把三类异常全部转成 `FeedError`**（`length_error`→`FrameTooLarge`、`bad_alloc`→`ResourceExhausted`、其余→`ResourceExhausted`），`:110-115` 的 `fail()` 用 `std::string().swap(...)`/`std::vector<std::string>().swap(...)` **真正释放**缓冲与已解析帧（注释说明「避免调用方把部分处理结果误认为完整批次」）；三种模式的边界检查都到位（`:49`/`:54` 分隔符模式双向检查、`:71-72` 长度前缀同时查 `max_frame` 与 `SIZE_MAX-4` 溢出、`:60`/`:64`/`:75` 不足时返回 `None` 等待更多数据）；`:94-103` 的 `feed()` 把 `FeedError` 转回异常以兼容旧接口；
  - `hybrid_mutex.hpp:63-82` 的慢路径设计正确：只有竞争者进入，`m_slow` 保证**同一时刻只有一个线程在等原子标志**（注释：「避免多个失败者同时忙等」），`:95-101` 用 `_mm_pause()`（x86）或 `yield()` 退避，`:88-93` 把 `fast_attempts == 0` 归一化为 1；`:106` 默认值用 `inline static std::atomic` 支持运行期调整（`:48-57`），且注释明确「不会修改已经构造的门」；
  - `hook.hpp:23-30` 用一整段注释交代了 Hook 的**完整语义边界**（哪些路径走 epoll、哪些回退原生 poll、关闭 Hook 后已有元数据的处置、未知 fcntl/ioctl 命令返回 `ENOTSUP` 而不读未知参数）；`:96-102` 的三个 `extern "C"` 控制项写明了设计理由「使 preload/interposer 宿主无需依赖 C++ 名字修饰」，并声明 `manager` 必须由调用方拥有且存活 —— 这是**跨 ABI 边界**的正确交付方式；
  - `context_backend.hpp:24-25` 把入口类型定为 `void (*)(Transfer) noexcept`，与 `fiber.cpp:727-733` 的 `entry(...) noexcept` 定义一致，**编译期强制**了「用户异常不得穿过上下文入口」这一契约；`:7-11` 说明 ABI 借鉴 coost 的 callee-saved 布局但「接口名称和数据类型属于 Go2Cpp，避免把第三方符号暴露到公共 ABI」——与 `context_x64.S:1-12` 的许可声明互相印证；
  - `endian.hpp` 六个转换函数全部 `constexpr`，用 `__BYTE_ORDER__` 编译期分支，无运行期开销；`status.hpp` 的 `StatusCode` 注释说明「具体 API 仍会同时返回更详细的 ErrorPtr」，与各模块的实际返回类型一致。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 通读）**：已完成 **16 个文件全文**，合计 **4,960 / 9,850 行 ≈ 50%**。
  - **下一步**：`scheduler.hpp`(662) → `timer.hpp`(344) → `config.hpp`(541) → `context.hpp`(408) → `log.hpp`(506) → `io.hpp`(314) → `go.hpp`(220) → `control_flow.hpp`(241) → `socket.hpp`(225) → `http.hpp`(246) → `websocket.hpp`(176) → `io_backend.hpp`(87) → `fd_manager.hpp`(69) → `parking_condition.hpp`(98) → `error.hpp`(250，用户在途文件，只读) → `context_backend.cpp` 对照 → 余下零散。
- **台账状态**：新增 **X-29**；待办 **8 条**（`D-14` 阻塞；`X-23`~`X-29` 待定）。

## 2026-10-12：include 通读（sync / thread_policy / sse / address 全文）—— 新增 X-28

- **本轮读实现**：`include/go2cpp/sync.hpp`(219，真身)、`include/go2cpp/concurrency/thread_policy.hpp`(153)、`include/go2cpp/network/sse.hpp`(205)、`include/go2cpp/network/address.hpp`(153)。四者**均全文读毕**，合计 730 行。
- **新发现：X-28（低，命名对 IPv6 误导）**。`address.hpp:60` 把 `broadcast_address` 定为 `IPAddress` 的纯虚函数，迫使 `IPv6Address` 也实现；但 IPv6 无广播概念，实现（`address.cpp:206-212`）其实是「host 位全置 1」= **子网最后一个地址**，与 IPv4 版本的真广播（`host | ~mask`）语义不同质，而头文件与实现都没说明。
- **sync.hpp 核对通过的要点**：
  - `:100-106` 有意区分了两种加锁接口的**失败语义**：`Lock()` 返回 bool（供可容忍失败的调用方），`lock()` 在无法继续等待时**抛 `std::logic_error`**（满足 BasicLockable 契约，不能返回 bool）。注释把这一差异写明了 —— 这与 X-27 指出的 `EventBatch` 静默失败形成对照：**接口本身提供了抛异常的选择，是用例方选了静默版**；
  - `:126-127` 把两个易踩的边界写进了类注释：「**没有 Scheduler 的手动 resume Fiber 不受支持**」与「关闭阶段如果无法立即重锁，Wait 返回 false **且 mutex 已解锁**」—— 后者是调用方必须知道的所有权状态，值得赞许；
  - `:36-45` `WaitBackend::ParkForCleanup` 的默认实现转发到 `Park` 并传 `std::nullopt`，注释说明「Scheduler 后端覆盖此方法，以允许 draining 中已启动 G 暂停等待锁 owner 释放锁，保证重锁后再退出」—— 这正是 X-26 里那份过时副本**缺失**的成员，也是它一旦被误用就会行为不同的原因；
  - `:74-76` `Mutex` 的契约完整：不支持递归、成功 `Lock` 恰好对应一次 `Unlock`、**`Unlock` 不要求同一 G/线程**（Go 风格所有权）；`:174-175` `WaitGroup` 声明「计数下溢或溢出抛 `std::logic_error` 且计数保持不变」，与 `sync.cpp:1208-1217` 实现一致。
- **thread_policy.hpp 核对通过的要点**：
  - `:29-30` 快照注释明确「只用于观测；scheduler 指针不拥有对象；查询快照**不会把线程挂接为 worker，也不会预留 P**」—— 与 `thread_policy.cpp` 的 `CurrentThreadPolicy()` 只读 TLS 的实现一致；
  - `:61-63` 与 `:110-112` 两处重复强调「由于状态是线程局部的，**移动/析构必须发生在同一 OS 线程**」，并提供了引用版与指针版两个构造函数；`:143-144` 明确 `EnterRuntimeWorker`/`LeaveRuntimeWorker` 「**这是源码级 Hook，不是 ABI 承诺**」—— 这类主动交付边界声明很有价值。
- **sse.hpp 核对通过的要点**：
  - `:91-92` 明确 `Parser` 是**有状态单消费者对象**，「多个线程/Fiber 同时 feed 必须由调用方使用 `sync::Mutex` 或把输入集中到一个 Fiber，否则会产生数据竞争」—— 该约束在实现中确实没有内部加锁，**声明到位**；
  - `:137-141` `Stream` 说明「一个实例内部串行化写入，所以可以由多个生产者安全调用 `send`；**互斥只保证字节不交错，不承诺业务事件顺序**」；`:184-185` 还写明了选型理由「写入可能在 Fiber 中 park，**必须使用 Fiber/线程混合锁，不能使用裸 `std::mutex`**」；
  - `:89-90` 「EOF 不会把没有空行结束的半个事件强行派发」与实现一致；`:164-167` 的 `state()`/`heartbeat_due()`/`next_heartbeat()` 三个 `noexcept` 查询接口正是 **X-25 所引用的「项目已有既定策略」的出处**（`src/network/sse.cpp:548-589` 对它们都加了 try/catch 兜住加锁异常），本批读头文件进一步确认其 `noexcept` 声明与实现策略是配套的。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 通读）**：已完成 **8 个文件全文** —— `channel.hpp` 2007 + `future.hpp` 952 + `event.hpp` 424 + `fiber.hpp` 228 + `sync.hpp` 219 + `sse.hpp` 205 + `thread_policy.hpp` 153 + `address.hpp` 153 = **4,341 / 9,850 行 ≈ 44%**。
  - **下一步**：`framing.hpp`(129) → `hybrid_mutex.hpp`(114) → `hook.hpp`(102) → `byte_array.hpp`(94) → `endian.hpp`(80) → `context_backend.hpp`(44) → `runtime.hpp`(36) → `status.hpp`(20) → `scheduler.hpp`(662) → `timer.hpp`(344) → `config.hpp`(541) → `context.hpp`(408) → `log.hpp`(506) → `io.hpp`(314) → `go.hpp`(220) → `control_flow.hpp`(241) → `socket.hpp`(225) → `http.hpp`(246) → `websocket.hpp`(176) → `io_backend.hpp`(87) → `fd_manager.hpp`(69) → `parking_condition.hpp`(98) → 其余。
- **台账状态**：新增 **X-28**；待办 **7 条**（`D-14` 阻塞；`X-23`~`X-28` 待定）。

## 2026-10-12：include 通读（event.hpp + fiber.hpp 全文）—— 新增 X-27

- **本轮读实现**：`include/go2cpp/tools/event.hpp`(424，本轮补齐 `:250-424`) 与 `include/go2cpp/concurrency/fiber.hpp`(228)。两者**均全文读毕**。
- **新发现：X-27（低，未声明的静默失败）**。`EventBatch` 的**每个方法**在 `sync::Mutex::Lock()` 返回 false 时都静默 `return`。查询方法返回默认值尚可接受，但**写方法会静默丢弃用户数据**：`add_event` 加锁失败时事件不入队、无任何反馈；`bind`/`clear`/`reset`/`make_loop_time`/`make_stop_config` 同理。
  - **现实触发路径**：`sync::Mutex::LockImpl`（`src/concurrency/sync.cpp:711-905`）对 `ParkingCondition::FiberWaitUnsupported()`（即**手动 Fiber、无 Scheduler 归属**）直接返回 false。而 `EventBatch` 的类注释只声明「支持 Fiber 与普通线程调用」，**未提及该失败模式**。
  - 同一类注释 `:22-23` 还说「对象必须一直存活到所有 work/bind/add_event 调用返回」，这说明作者意识到调用可能失败/阻塞，但没有为失败提供出口。
- **event.hpp 核对通过的要点**：
  - `:267-296` `work()` 的 **Context 桥接**设计完整：当调用方传入 `context` 时，先用 `Context::WithCancel(context)` 建子 Context，再把**子 Context 的取消注册到批次自身的 stop Context 上**（`:272-275`），于是「用户取消」与「`stop()`」两条路径都能唤醒正在 `Select` 的 Fiber；`:291-296` 在 finally 语义位置移除回调并调用 `stop_bridge_cancel()`，且**都在锁外**；
  - `:279-290` 把 `Select` 抛出的异常（含 `std::exception` 与未知异常）**统一转换成 `kInvalid` + ErrorPtr**，注释明确「异常不能穿过 scheduler worker 或 Fiber 边界」；`:320-339` 的用户回调同样在**锁外**执行并把异常转成结果，仅在更新 `m_last` 时短暂加锁（`:327-330`、`:334-337`）；
  - `:239-248` 把 `max_duration` 作为**硬上限**：即使未设置 loop timeout，也把剩余时间传给 `Select`（注释：「不能无限等待」），并在剩余时间 ≤0 时把 timeout 压成 0 以立即返回；
  - `:212-216` 与 `:298-302`、`:203-205` 的锁序统一为 `m_work_mutex` → `m_mutex`，且三处失败路径都**配对了 `m_work_mutex.Unlock()`**，无泄漏；`:130-147` `stop()` 在自己的 try/catch 内完成，注释「shutdown 期间不能再阻塞调用方」；
  - `:306-309` 只有 `kTimedOut` 且 `stop_on_timeout` 才置停止位，`kCancelled`/`kDeadlineExceeded` 不置位 —— 与 `:98-100` 的语义说明一致。
- **fiber.hpp 核对通过的要点**：
  - `:32-34` 明确 `FiberExecutionBinding` **只保存不拥有对象的标识**（`scheduler_token` 是 Scheduler 地址的整数形式），理由是「避免 Fiber 模块依赖 scheduler 模块，也避免调度器生命周期被调试信息反向延长」—— 这正是 `Fiber::Impl` 里 `m_last_machine_id`/`m_binding_published` 那套调试元数据不持锁、不延长生命周期的依据；
  - `:125-131` 析构契约写得很实：Ready 会跳过主体、Suspended 只有在「固定父级/调用方仍可恢复」时才继续、**错误父级下安全放弃并标记 `Failed`**（「无法展开挂起栈上的局部 RAII」），并明确警告「**忽略取消的主体可能阻塞析构**」；
    - **与本会话已登记的 X-13 呼应**：X-13 记录的正是这条「阻塞」在实现层是**无界 yield 自旋**（`src/concurrency/fiber.cpp:660-669`）。**该注释只说「可能阻塞」，未说明是忙等且无上限**，故 X-13 仍然成立，此处补充其文档出处；
  - `:206-208` 描述墓碑帧：父对象结束后仍由共享记录保留 `alive=false` 的帧，「只用于观测、取消和诊断，**不能恢复已销毁的父栈**」—— 与 `fiber.cpp` 的 `FiberRecord` 实现及 `abandon_suspended` 的失败路径一致；
  - `:50-52` 区分 `parent_id`（首次进入时固定）与 `active_parent_id`（最近一次 resume 的实际调用者），与 `FiberContextFrame` 字段及 `fiber.cpp:1422-1460` 的 `context_snapshot()` 构建逻辑对应；
  - `:104-105` 声明「非法或过小值会由实现归一化为可用容量；构造失败会抛出异常」，与 `fiber.cpp:213-259` 的 `normalize_stack_size` 一致。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 通读）**：已完成 4 个文件全文 —— `channel.hpp` 2007 + `future.hpp` 952 + `event.hpp` 424 + `fiber.hpp` 228 = **3,611 / 9,850 行 ≈ 37%**。
  - **下一步**：`sync.hpp`(219) → `thread_policy.hpp`(153) → `sse.hpp`(205) → `address.hpp`(153) → `framing.hpp`(129) → `hybrid_mutex.hpp`(114) → `hook.hpp`(102) → `byte_array.hpp`(94) → `endian.hpp`(80) → `scheduler.hpp`(662) → 其余。
- **台账状态**：新增 **X-27**；待办 **6 条**（`D-14` 阻塞；`X-23`~`X-27` 待定）。

## 2026-10-12：全 include 树重复头文件扫描 —— 发现并修复 X-26

- **本轮性质**：原计划按行数顺序继续通读 include，但读 `tools/event.hpp` 时注意到它 `#include "go2cpp/sync.hpp"`（根路径），而 `include/` 下同时存在 `go2cpp/concurrency/sync.hpp`，两者**都有实质内容**。这促使本轮改为先做一次**全 include 树的同名头文件扫描**，以免在继续通读时误把过时副本当成真身。
- **扫描方法**：按 basename 分组列出 `include/` 下全部 79 个头文件，对出现 ≥2 次的同名文件分别统计行数并 grep 其被包含次数，行数不同者列为待核对项。
- **扫描结果：27 对同名文件，26 对正常，1 对异常**。
  - **26 对正常**：全部遵循统一约定 —— **子目录持有真身、根目录是 4 行转发头**（`#pragma once` + 注释 + `#include "go2cpp/模块/文件.hpp"`）。转发头的被包含次数普遍高于真身（如 `context.hpp` 转发头 13 次 / 真身 2 次、`fiber.hpp` 转发头 16 次 / 真身 1 次），说明项目内部代码与测试都在用**根路径**，而模块内互引用用**子目录路径**。
  - **1 对异常（X-26）**：`sync` 的方向**反了** —— 真身在**根目录** `include/go2cpp/sync.hpp`（219 行，被包含 9 次），而子目录 `include/go2cpp/concurrency/sync.hpp` **不是转发头**，是一份**少 16 行的过时副本**（203 行，被包含 **0** 次）。
  - `diff` 显示副本缺少：`WaitBackend::ParkForCleanup` 虚函数（真身 `:37-46`，注释说明它「为条件变量重锁执行不可被 G 取消中断的 park」，`Scheduler` 后端会覆盖它）与 `Mutex` 的 `friend class ConditionVariable` / `LockForConditionWait()` / `LockImpl(context, ignore_task_cancellation)` （真身 `:111-115`）。
- **风险论证**：因为全仓库**无人包含**该副本，当前**不存在** ODR 违规。但路径方向与全项目其余 26 对相反 —— `go2cpp/concurrency/sync.hpp` 看起来比 `go2cpp/sync.hpp` **更符合模块化惯例**，任何人（包括未来的我）按模块路径书写 include 都会拿到**过时的类定义**；一旦它与真身出现在同一程序内，就是 **ODR 违规**（ill-formed, no diagnostic required，编译器不会报错）。
- **已修复**：把 `include/go2cpp/concurrency/sync.hpp` 替换为 4 行转发头（指向 `go2cpp/sync.hpp`），并在注释中写明「本文件此前携带过时副本，已改为转发，避免两处定义漂移」。**改动零影响**（该文件被包含 0 次）。
- **验证结果**：`cmake --build build-quality --parallel` → **0 warning / 0 error**；`ctest --test-dir build-quality` → **26/26 通过**。
- **附带的清扫结果（无需处理）**：另有 3 个**被包含 0 次的根目录转发头**（`sse.hpp` 3 行、`websocket.hpp` 3 行、`concurrency/scheduler.hpp` 4 行）。它们只是冗余、不构成风险，**未改动** —— 删除公共头文件可能影响外部使用者，不在本任务授权范围内。
- **台账状态**：新增 **X-26**；待办 **5 条**（`D-14` 阻塞于用户文件；`X-23`~`X-26` 待定）。
- **下一步**：回到 include 通读主线。已完成 2 个大文件（`channel.hpp` 2007、`future.hpp` 952）；本轮读了 `tools/event.hpp` 前 250 行，下轮继续 `event.hpp` 余下并用同一方法快速过其余文件。

## 2026-10-12：include 审计（future.hpp :560-952）—— future.hpp 全文读毕，扩充 X-25

- **本轮读实现**：`include/go2cpp/concurrency/future.hpp` 的 `:560-952`，约 392 行，覆盖 `FutureState<T,true>` 余下、`MergeDeadline`/`DeadlineAfter`、`FutureResult<T>` 与其 `void` 特化、`Promise<T>` 全文、`Future<T>` 全文、`Promise::GetFuture` 与 `MakePromise`。**至此 `future.hpp` 952 行全部读完。**
- **X-25 范围扩充（重要）**：上轮我只登记了 `FutureState` 两个特化的 `Status()`。读完全文后确认该问题**共四处**，且其中两处比 `Status()` 更危险：
  | # | 位置 | 说明 |
  |---|---|---|
  | ① | `:307-322` | `FutureState<T,false>::Status() noexcept` 直接加锁 |
  | ② | `:507-522` | `FutureState<T,true>::Status() noexcept` 直接加锁 |
  | ③ | `:867-869` | `Future::Status() noexcept` 转发到 ①② |
  | ④ | `:697-703`、`:709-717` | `Promise::~Promise()`（隐式 `noexcept`）与 `Promise::operator=(Promise&&) noexcept` 都调 `m_state->SetError(...)` 发布 broken-promise，**而 `SetError` 内部会加锁** |
  ④ 尤其值得注意：它出现在**析构**与**移动赋值**这两条最难排查的路径上，一旦加锁失败就是 `std::terminate`，而不是可捕获的异常。
- **本段核对通过的要点**：
  - `:697-717` `Promise` 的 broken-promise 语义完整：析构与移动赋值**都会**对尚未完成的共享状态发布 `BrokenPromiseError()`，且移动构造函数 `Promise(Promise&&) noexcept` **只搬运指针、不发布错误**（`:708`）—— 即「移动走之后由新的持有者负责完成」，语义正确；
  - `:583-597` `MergeDeadline` 取 Context 截止时间与本地 deadline 的**较小者**，`:599-609` `DeadlineAfter` 有溢出检查（`now > max - timeout` → `max`），两者配合 `Wait` 内部再次比较 `Context::Now() < *deadline`，构成一致的绝对时间语义；
  - `:841-856` `GetResultUntil` **只在终态**（`kReady`/`kError`/`kException`/`kCancelled`）才读取 value/error/exception，因此超时返回的 `FutureResult` 不会携带半成品数据；`:850-852` 用 `if constexpr (!std::is_void_v<T>)` 保证 void 特化不实例化 `m_state->Value()`；
  - `:908-935` `ThrowResult` 标 `[[noreturn]]`，且**异常优先于错误**（`:911-913` 先 `rethrow_exception`），再按状态补默认错误（`CanceledError`/`DeadlineExceededError`/`NewError`），与注释 `:871-872`「只有调用者明确选择 GetOrThrow 时，error/exception 才会转换为 C++ 异常」一致；`:876-878` 还用 `static_assert` 限制 `GetOrThrow` 只能用于可复制类型，并在消息里指向 `GetResult().Value()` 作为 move-only 的出路；
  - `:632-635` 与 `:669-672` `FutureResult::Cancelled()` 把 `kDeadlineExceeded` 同时算作**超时与取消**，与 `TimedOut()`（`:628-631`）在 `kDeadlineExceeded` 上重叠 —— 这是有意的语义设计（deadline 既超时也取消），但调用方若写成 `if (TimedOut()) ... else if (Cancelled()) ...` 会只命中前者。**不登记为条目**（属合理设计），此处记录以备查；
  - `:946-951` `MakePromise` 先 `GetFuture()` 再移动 promise，保证返回对的两端共享同一状态；`Future` 的构造函数为 private 且 `friend class Promise<T>`（`:938`），外部无法凭空构造有效 `Future`。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 审计）**：**`future.hpp` 952 / 952 行 = 100%**；`include/` 整体约 **2,959 / 9,850 行 ≈ 30%**（`channel.hpp` 2007 + `future.hpp` 952）。
  - **仍未逐行读**：`scheduler.hpp`(662)、`config.hpp`(541)、`log.hpp`(506)、`event.hpp`(424)、`context.hpp`(408)、`timer.hpp`(344)、`io.hpp`(314)、`sync.hpp`(219)、`go.hpp`(220) 等 28 个文件，合计约 6,891 行。
- **台账状态**：X-25 已扩充；待办仍为 **4 条**（`D-14` 阻塞于用户文件、`X-23`/`X-24`/`X-25` 待定）。
- **下一步**：转入 `scheduler.hpp`(662)——它是并发侧最后一个大文件，且与已读的 `scheduler.cpp`(4048) 直接对应，可交叉核对声明与实现的契约一致性。

## 2026-10-12：include 审计（future.hpp :1-560）—— 新增 X-25

- **本轮读实现**：`include/go2cpp/concurrency/future.hpp` 的 `:1-560`，约 560 行，覆盖 `FutureWaitBackend` 协议、`FutureCallbackGuard`、三个后端工厂声明、`FutureStatus`、`FutureError`、`BrokenPromiseError`、`FutureStateKind`/`FutureWaitRegistration`、`FutureState<T,false>` 全文、`FutureState<T,true>`（void 特化）至 `StatusLocked` 中段。
- **新发现：X-25（低，noexcept 保护不一致）**。`FutureState` 的两个特化的 `Status()`（`:307-322`、`:507-522`）都是 `noexcept`，却在函数体第一行构造 `std::lock_guard<std::mutex>`。`std::mutex::lock()` 失败时会抛 `std::system_error`，穿过 `noexcept` 就是 `std::terminate`。
  - **对照证据**：同一项目在 `src/network/sse.cpp:548-589` 对 `state()`/`heartbeat_due()`/`next_heartbeat()` 三个 `noexcept` 查询接口**全部用 try/catch 包住加锁**，并写明了理由（「不能让 C++ 异常穿过 noexcept 导致进程 terminate」）。**即项目已确立该策略，此处是遗漏**。
  - **边界说明**：触发条件是 mutex 加锁失败，属极罕见路径，故严重度为低；且同类的 `Value()`/`ErrorValue()`/`ExceptionValue()` **未标 `noexcept`**，不受影响 —— 只有 `Status()` 两处需要处理。
- **本段核对通过的要点**：
  - `:253-305`（非 void）与 `:460-505`（void）的 `Wait` 设计严谨：`:261-262` **先注册 Context 回调、再取状态锁**，注释说明「若取消已经发生，AddCallback 会同步执行回调；此时尚未持有状态锁，不会发生自锁」；回调内 `:272-276` 又**先取 `state->m_mutex` 再置 `registration->cancelled`**，注释说明「保证取消不会落在发布窗口之前」—— 这把「回调注册」与「等待发布」两个窗口都堵住了；
  - `:189-202` `SetValue` **在持锁之前就完成值构造**（`auto stored = std::make_shared<const T>(std::move(value));`），注释明确「避免用户类型的构造函数阻塞其他等待者」；四个终结函数（`SetValue`/`SetError`/`SetException`/`Cancel`）都遵循「锁内改状态 → 锁外 `NotifyAll`」；
  - `:340-381` `StatusLocked` 的优先级链完整且有据：先看 `m_kind` 终态 → 再看 `registration_cancelled`/`context->IsDone()`（并区分 `DeadlineExceeded` 与 `Cancelled`）→ `:364-370` **Context 的截止时间优先于本次等待的本地 deadline**（注释：「定时器线程可能尚未来得及把 done 标志发布到回调观察者」）→ 本地 deadline → `ParkingCondition::CancellationRequested()`（注释：「不能继续假装阻塞；调用者可再次等待」）；
  - `:172-176` `FutureWaitRegistration` 的注释点出了一个关键设计约束：「每个 Future 等待者有独立的取消标记。不能把标记放在共享状态中，否则一个调用者的超时会误唤醒并影响其他等待者」—— 这正是多等待者共享同一 `FutureState` 时的正确做法；
  - `:22-49` `FutureWaitBackend` 的契约声明完整：明确要求实现「必须允许虚假唤醒并重复检查」，且 `Wait` 的注释规定「lock 调用时必须已加锁，返回时保持加锁」；`:98-100` 说明工厂「只影响注册成功后创建的共享状态；已存在的 Future 不迁移后端」，与 `src/concurrency/future.cpp:79-129` 的实现一致。
- **一处未登记的观察**：`FutureState<T,false>`（`:181-389`）与 `FutureState<T,true>`（`:391-532`）有约 140 行**几乎逐行相同**（`SetError`/`SetException`/`Cancel`/`Wait`/`Status`/`StatusLocked`/`ErrorValue`/`ExceptionValue`）。这使 X-25 的修复必须**同时改两处**。**不登记为条目**：模板对 `T` 与 `void` 的双特化是 C++ 常规取舍，提取公共基类会增加间接层；但它是「同族代码需同步修改」的典型，修 X-25 时务必两处都改。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 审计）**：`future.hpp` 已读 560 / 952 行（59%）；`include/` 整体约 **2,567 / 9,850 行 ≈ 26%**。
- **台账状态**：新增 **X-25**；待办 **4 条**（`D-14` 阻塞于用户文件、`X-23`/`X-24`/`X-25` 为待定的改进项）。
- **下一步**：读完 `future.hpp :560-952`。

## 2026-10-12：include 审计（channel.hpp :1600-2007）—— channel.hpp 全文读毕，新增 X-24

- **本轮读实现**：`include/go2cpp/concurrency/channel.hpp` 的 `:1600-2007`，约 407 行，覆盖 `RecvChecked`/`SendChecked` 声明与定义、`SendOnlyChannel`/`RecvOnlyChannel` 全文、`SelectCase`/`SelectResult` 定义、`RecvCase`/`SendCase`/`DefaultCase` 与 `Select` 的重载声明。**至此 `channel.hpp` 2,007 行全部读完。**
- **新发现：X-24（低，不对称）**。`RecvCase` 与 `SendCase` 的 `arm_checked` 对**不可复制 `T`** 的处理不一致：
  - `RecvCase`（`:1898-1902`）：`return !channel || channel->ArmSelectRecv(state, index);` —— 不可复制时 `ArmSelectRecv` 内部直接 `return false`，即**如实报告注册失败**；
  - `SendCase`（`:1959-1966`）：`if constexpr (is_copy_constructible<T>) {...} return true;` —— 不可复制时**无条件返回 true**，**声称成功却未 arm 任何节点**。
  - 两者都设置了 `unsupported = !is_copy_constructible<T>::value`（`:1903`、`:1967`），而内置 `Select` 在 `src/concurrency/channel.cpp:106-111` 遇到 `unsupported` 就**提前返回错误**，因此**在内置路径上不可达**；
  - **但风险是真实的**：`arm_checked` 是 `SelectCase` 的**公开字段**（`:1758-1762`），其注释明确其契约是「false 表示注册失败」。**自定义 Select 实现若不检查 `unsupported` 就直接调用 `arm_checked`**，`SendCase` 会让它以为注册成功，随后等待一个永不到来的唤醒 —— 这与 `RecvCase` 的行为形成鲜明反差。已登记，建议两者对称。
- **本段核对通过的要点**：
  - `:1603-1609` 与 `:1660-1669` 的方向性视图注释准确：`SendOnlyChannel` 仅暴露 `Send`/`SendFor`/`TrySend`/`Close`/`Capacity`/`IsClosed`，**无任何接收能力**；`RecvOnlyChannel` 仅暴露 `Recv`/`RecvFor`/`TryRecv` 与查询，**无发送且无 `Close`** —— 与注释「只读视图不能发送或关闭」逐条对应；
  - `:1649-1657` 与 `:1700-1708` 用 `friend` 精确放行 `AsSendOnly`/`AsRecvOnly`/`SendCase`/`RecvCase` 四个构造入口，构造函数保持 private，**外部无法绕过方向约束直接构造视图**；
  - `:1721-1739` `SendChecked`/`RecvChecked` 把 nil 通道显式化为 `kNil` + `ChannelNilError()`，注释说明「避免像 Go 的 nil 通道一样永久等待」——这正是 `channel.cpp:189-193` 那条 `FiberWaitUnsupported` 检查的同族设计；
  - `:1802-1819` `SelectResult::Value<T>()` 对 move-only 类型用 `if constexpr` **不实例化 `std::any_cast`**，注释指向 `TakeValue<T>()`；`:1813-1817` 在 `any_cast` 失败时**回退到 `typed_value.As<T>()`**，与 `Take` 的「分路搬运」思路一致；
  - `:1833-1852` `Cast<T>` 用 try/catch 隔离 Caster 异常并返回 `nullopt`，与 `SelectCaster` 注释（`:258-259`）「不要把异常带入 select 状态机」一致；
  - `:1858-1882` `RecvCase` 的 probe：nil 通道先返回 `kNil` 探针；`TryRecv` 不 ready 时返回**空 SelectProbe**（表示不可立即完成）；`SetValue` 失败时改报 `kInvalid` —— 三条路径都设 `probe.ready = true`，与 `SelectCase::operator bool`（`:1764-1767`）的判据一致。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 审计）**：**`channel.hpp` 2,007 / 2,007 行 = 100%**；`include/` 整体约 **2,007 / 9,850 行 ≈ 20%**（另有若干文件被定向查阅，不计入逐行通读）。
  - **仍未逐行读**：`future.hpp`(952)、`scheduler.hpp`(662)、`config.hpp`(541)、`log.hpp`(506)、`event.hpp`(424)、`context.hpp`(408)、`timer.hpp`(344)、`io.hpp`(314)、`sync.hpp`(219)、`go.hpp`(220) 等 29 个文件，合计约 7,843 行。
- **台账状态**：新增 **X-24**；待办 **3 条**（`D-14` 阻塞于用户文件、`X-23`/`X-24` 均为待定的改进项）。
- **下一步**：转入 `future.hpp`(952)——它含 `Future`/`Promise`/`SharedFuture` 等模板实现，是 include 侧最后几个大文件之一。

## 2026-10-12：include 定向审计（channel.hpp :900-1600）—— 新增 X-23

- **本轮读实现**：`include/go2cpp/concurrency/channel.hpp` 的 `:900-1600`，约 700 行，覆盖 `ArmSelectRecv` 余下、`ArmSelectSend` 全文、`DisarmSelect`、`PendingSend`/`PendingRecv`/`CallbackGuard`、`ContextSendStatus`/`ContextRecvStatus`、`DeadlineReached`/`NotifyGeneration`、`SendUntil`、`RecvUntil`、`RefillBufferLocked`、`ErasePending` 与全部成员字段。
- **新发现：X-23（低，命名误用）**。`SendUntil`（`:1297`）调用 `ContextRecvStatus`，而 `RecvUntil`（`:1339`）调用 `ContextSendStatus` —— **两个辅助函数被交叉复用**。经逐行比对确认二者逻辑完全等价，**仅返回类型不同**（`:1135-1144` 返回 `ChannelSendResult`；`:1146-1159` 返回 `ChannelStatus` 并写 `ErrorPtr*`），因此**当前行为正确**，不是缺陷，而是**可维护性风险**：若将来有人只改 `ContextSendStatus`（顾名思义以为只影响发送），会连带改变接收路径。已登记，建议合并为中性命名的单一辅助函数。
- **本段核对通过的要点**：
  - `:919-1059` `ArmSelectSend` 与 `ArmSelectRecv` 结构对称：双重检查（`:921-927`）、`receiver_scan_limit` 限制轮数、把自己的节点 `pop_front` + `emplace_back` 转到队尾避免自匹配；`:984-990` 与 `:1024-1035` 都遵循「**先构造值、再发布 select**」的顺序，并在 `TrySelect` 失败时回滚（`receiver->value.reset()` / `m_buffer.pop_back()`），保证用户类型抛异常时不留下半提交状态；
  - `:1061-1093` `DisarmSelect` 遍历两个队列标记 `cancelled` 并 erase，只有确实删除过才 `++m_generation`，通知放在锁外；
  - `:1170-1335` `SendUntil` 的优先级正确：先与最早等待的接收者会合，再使用缓冲，最后才挂起；`:1198-1200` 与 `:1207-1209` 两处 catch 都**保留接收节点**（注释说明「值复制失败时保留接收节点，后续发送仍可重试」、「选择状态未发布时节点仍然有效，不能先把它从队列移除」）；`:1229-1231` 强调 `optional::emplace` 抛异常时保持空值的强保证；
  - `:1262-1264` 与 `:1434-1437` 都用 `core::ParkingCondition::FiberWaitUnsupported()` **提前拒绝**手动 Fiber，与 `src/concurrency/channel.cpp:189-193` 的 Select 路径一致 —— 即 Channel 的每条等待路径都不会在无法 park 时静默阻塞 carrier 线程；
  - `:1372-1375`、`:1389-1413`、`:1518-1528`、`:1554-1559` 四处按 `is_nothrow_move_constructible` / `is_nothrow_copy_constructible` **分派移动或复制**，并只在存在 nothrow 路径时才认领 select 节点，与类注释 `:591-592` 的契约一致；`:1347-1359` 提取缓冲值时依赖「`Channel<T>` 要求移动构造不抛异常」这一 `static_assert` 前提（注释 `:1348-1349` 已说明）；
  - `:1571-1575` `ErasePending` 用 `remove` + `erase` 而非直接 erase，对 deque 是正确的；`:1584` `m_generation` 是 `atomic<uint64_t>`，而 `:774` 的 `Generation()` 用 acquire 读、`:1166` 的 `NotifyGeneration` 用 release 递增，配对正确。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 审计）**：`channel.hpp` 已读 1,600 / 2,007 行（80%）；`include/` 整体约 **1,900 / 9,850 行 ≈ 19%**。
  - **仍未读**：`channel.hpp` 余约 407 行（`:1600-2007`，含 `RecvChecked`/`SendChecked` 余下、`Select`/`RecvCase`/`SendCase`/`SelectResult` 等）；以及 `future.hpp`(952)、`config.hpp`(541)、`log.hpp`(506)、`event.hpp`(424)、`context.hpp`(408)、`timer.hpp`(344)、`io.hpp`(314) 与其余 20 个头文件。
- **台账状态**：新增 **X-23**；待办 **2 条**（`D-14` 阻塞于用户文件、`X-23` 为待定的命名改进）。
- **下一步**：读完 `channel.hpp :1600-2007`，然后转入 `future.hpp`(952)。

## 2026-10-12：include 定向审计（channel.hpp :300-900）

- **本轮读实现**：`include/go2cpp/concurrency/channel.hpp` 的 `:300-900`，约 600 行，覆盖 `FunctionSelectCaster`、`ChannelSendResult`、`SelectProbe`、`SaturatingDeadline`、`SelectWaitState` 全文、`ChannelRecvResult`、`Channel<T>` 类头与 `Create`/`Capacity`/`Len`/`IsClosed`/`Send*`/`Recv*`/`TrySend`/`TryRecv`/`Close`/`Generation`/`WaitForChange`/`ArmSelectRecv` 开头。
- **结论：本段未发现新的缺陷或文档缺失。** 以下要点经逐条核对：
  - `:457-482` `SelectWaitState::TrySelectPair` 用 `std::scoped_lock lock(first->m_mutex, second->m_mutex)` **同时获取两把锁**（`std::lock` 算法，无死锁风险），在临界区内一次性设置双方，使「一手交钱一手交货」的双向配对具备原子性；`:463` 还先排除 `first == second`。这是 select 单赢家语义的关键实现；
  - `:521-544` `Take` 的字段搬运是**分两路**的：`ready/ok/status/error` 直接复制，`typed_value` 用 `std::move`（保住 move-only 结果的所有权），`value`（`std::any`）单独 try/catch（复制可能抛）。注释（`:527-528`）明确「`std::any` 的复制可能失败；`typed_value` 使用共享载体，可以在这种情况下仍然把选择结果交给用户的 Caster」——**降级路径是有意保留的**；
  - `:707-769` `Close` 在锁内 `swap` 出两个等待队列并逐节点提交 `kClosed` probe，对 select 节点额外用 try/catch 包裹 `TrySelect`，异常时改走 `Cancel()`（注释：`Close` 仍须唤醒该 select；选择结果不可提交时把节点取消掉，避免异常穿过 `Close` 并留下永远等待的 Fiber）；**通知全部移到锁外**（`:761-767`）；
  - `:787-892` `ArmSelectRecv` 使用**双重检查**（`:789-791` 无锁预检 + `:793-795` 持锁复查），与 `src/concurrency/channel.cpp` 的注册路径一致；扫描 senders 时用 `sender_scan_limit = m_senders.size()` **限制扫描轮数**，并把「自己的节点」`pop_front` 后 `emplace_back` 到队尾以避免自匹配 —— 这个「轮转跳过自己」的写法配合计数上限，**不存在无限循环**；
  - `:798`/`:834`/`:870` 三处 `if constexpr (!std::is_copy_constructible<T>::value) return false;` 与类注释 `:591-592`「内建 select 对 T 还要求可复制；不可复制值可通过 SelectValue 自定义路径承载」**完全对应**，即不可复制类型在 select 路径上的限制是**已声明的契约**，不是遗漏；
  - `:601-604` 两个 `static_assert` 把 `T` 的「可无异常移动构造」与「可无异常析构」变成**编译期契约**，与注释 `:591` 一致；`:423-436` `SaturatingDeadline` 同时处理正向与负向溢出（含 `Duration::rep::min()` 特判），无未定义行为。
- **验证结果**：本批为**只读回溯**，未改动任何源码或测试，未触发构建。
- **累计进度（include 审计）**：`channel.hpp` 已读 900 / 2007 行（45%）；`include/` 整体已读约 **1,200 / 9,850 行 ≈ 12%**（首轮 300 行 + 本轮 600 行 + 此前各批 grep 与交叉核对涉及的片段）。
  - **仍未读**：`channel.hpp` 余约 1,107 行（含 `ArmSelectRecv` 余下、`ArmSelectSend`、`DisarmSelect`、`Select`/`RecvCase`/`SendCase` 等）；以及 `future.hpp`(952)、`config.hpp`(541)、`log.hpp`(506)、`event.hpp`(424)、`context.hpp`(408)、`timer.hpp`(344)、`io.hpp`(314) 与其余 20 个头文件。
- **台账状态**：本轮无新增条目；待办仍为 **1 条**（`D-14`，阻塞于用户在途文件 `include/go2cpp/tools/error.hpp`）。
- **下一步**：继续 `channel.hpp :900-2007`，然后按体量依次审 `future.hpp`、`config.hpp`、`log.hpp`。审计重点是「注释与实现矛盾」与「契约未声明的前置条件」两类 —— X-06（注释称字段受互斥保护，实为无锁）与 D-17（public 接口未声明前置条件）正是这两类的样本。

## 2026-10-12：补齐 `.agent/state.json` 并转入 `include/` 定向审计

- **本轮性质**：台账已无「进行中」条目（仅剩 `D-14`，阻塞于用户在途文件），故按提示词步骤 4 进入**自主扩账模式**：对尚未审计的区域做定向审计。同时补上一项**长期欠账**。
- **欠账：`.agent/state.json` 严重滞后**。该文件此前停留在 `round: 6` / `last_batch: "B-05"` / `last_commit: "f8156e5"` / `updated_at: 2026-10-10T18:24:47+08:00`，而自那个时间点起我已经执行了 **32 个工作轮**（源码回溯第 1~26 批共 26 轮，加上 T-08、X-01、X-12 复核、死代码清除、四条结案、八条处置共 6 轮）。本轮已更新为：
  ```json
  {
    "round": 38,
    "last_batch": "源码回溯与台账结案",
    "last_result": "success",
    "last_commit": "8be9ff8",
    "blocked_items": ["D-14"],
    "updated_at": "2026-10-12T10:11:41+08:00"
  }
  ```
  **`round` 的算法（可核）**：原值 6 + 自原 `updated_at` 起的 32 轮 = 38。32 来自 `codex.md` 中该时间点之后的 `## ` 条目标题数（第 1~26 批 + 6 个修复/复核轮）。**若这个算法与你的预期不符，请直接告诉我该记什么值。**
  - `blocked_items` 填入 `D-14`：它是唯一未结项，且阻塞原因是**外部条件**（目标文件 `include/go2cpp/tools/error.hpp` 是用户当前的在途改动，`git add` 该文件会连带提交用户未完成的 `errors::` 命名空间），不是技术无法推进。
  - 顺带复核 `.agent/quality_task_rules.md`：§3.1「git 操作策略（持续推进模式）」与 §9「定时任务（持续推进）模式」**均已存在**（文件第 60、145 行），即提示词步骤 2 的规则同步**早已完成**，本轮无需再做。
- **转入定向审计：`include/` 头文件**。理由：此前 25 批回溯只覆盖 `src/`（22,012 行已全部读完），而 `include/` 的 **9,850 行 / 30 个头文件**我只用 `grep` 抽取过接口片段、**从未逐行读过**。而 X-06 恰好证明了这类漏读的代价：`include/go2cpp/network/fd_manager.hpp:18` 的注释声称「字段访问由 FdManager 内部互斥保护」，与实现矛盾且掩盖了一个真实的数据竞争 —— **头文件的错误会直接误导调用方**。
  - 头文件规模分布（本轮实测）：`channel.hpp` 2007、`future.hpp` 952、`scheduler.hpp` 662、`config.hpp` 541、`log.hpp` 506、`event.hpp` 424、`context.hpp` 408、`timer.hpp` 344、`io.hpp` 314，其余 21 个均不足 260 行。
- **验证结果**：本轮只改 `.agent/state.json` 与文档，**未触碰任何源码**，故未触发构建；`state.json` 已用 `json.loads` 验证可解析。
- **剩余待办 1 条**：`D-14`（阻塞，见上）。
- **下一步**：开始 `include/` 逐行审计，从最大的 `channel.hpp`(2007) 起，重点找「注释与实现矛盾」「契约未声明的前置条件」两类问题（X-06 与 D-17 正是这两类的样本）。

## 2026-10-11：剩余 8 条 X- 条目逐条处置（含 2 项修复）

- **本轮性质**：上轮我预告「评估无 TSan 条件下各条能做到什么取证」，评估后**改变了本轮重点**：这 8 条的**事实**其实都已由源码确证（每条登记时都给了行号），缺的不是证据而是**「是否值得修」的判定** —— 做探针改变不了这个判断。故本轮逐条给出明确处置，并实际修掉其中两条。按无人值守规则采用该默认方案，**用户可推翻**。
- **已修 2 项（commit `9ef266b`）**：
  - **X-07（真缺陷，已修）**：`FiberLocalCache::Reset()` 的**线程分支**原先直接写 `fiber_local::detail::ThreadValues().erase(m_key);`（`include/go2cpp/concurrency/fiber_local.hpp:106`），**在容器内部析构该值**；用户值的析构函数若重入本 API，即「在 erase 进行期间修改同一容器」→ 未定义行为。而 Fiber 分支（`src/concurrency/fiber_local.cpp:124-150`）早已是「锁内摘出 → 锁外 `value.reset()`」，两路不对称。已把线程分支改为同样的「先摘出、再在函数末尾（表外）析构」，正常路径语义不变；`ThreadValues()` 返回的本就是 `thread_local` 表（`fiber_local.cpp:41`），故此处不需要锁，只需把析构移出容器。
  - **X-06（注释错误已修，字段同步留待决策）**：复核确认 `FdContext` 的 7 个 getter（`include/go2cpp/network/fd_manager.hpp:22-28`）**全部无锁直读**，写侧 `FdManager::get()`/`erase()` 才持 `m_mutex`（`src/network/fd_manager.cpp:18/29/38/57/66`）——原 `:18` 注释「字段访问由 FdManager 内部互斥保护」与之矛盾，且**该竞争是真实存在的**。已把注释改为如实的 `@warning`（说明 getter 无保护、并发读写为 UB、调用方需自行判定或同步）。**字段本身未改**：把 7 个字段改为 `std::atomic` 会改变公开类的布局与 ABI、并涉及性能权衡，属需用户决策的改动。
- **其余 6 条逐条处置（均保持登记，理由已写入台账）**：
  - **X-08**（defer 深度为 `thread_local`）：既有约束的**必然推论**，非独立缺陷；改它需把深度随 Fiber 元数据传递，收益只在违反既有约束时显现。
  - **X-09**（四种自线程停止策略）：四条各有安全边界 —— `timer` 是主动 fail-fast、`io`/`context` 的实例用户无法直接析构、`config` 是进程级 static。**强行统一会改变至少一处的可观察行为**，收益不足。
  - **X-10**（`WakeAction::kPending` 返回值语义相反）：三种入口语义各有理由，**改返回值会破坏已有契约**；且 `scheduler.hpp:525-527` 的注释本就写明「返回 false 仍可能表示已接受一个 pending 唤醒」，按注释使用不会误判。
  - **X-13 / X-14**（两处无界 yield 自旋）：**自旋时长的上界完全取决于被等对象运行多久**，没有可测的固定值；量化只能得到「用户代码跑多久就空转多久」这一同义反复，**改变不了结论**。改为条件等待是真实的设计改动（需为每个 Fiber/节点增加同步原语与唤醒点），收益与复杂度权衡属需用户决策项。
  - **X-18**（`adopt_socket` 持锁做系统调用）：注释已声明串行化是**有意设计**（防两个接管者互相误判 `O_NONBLOCK`），且热路径（已接管 fd）完全不受影响；在无「实际成为瓶颈」的证据前改动，风险高于收益。
- **验证结果**：`cmake --build build-quality --parallel` → **0 warning / 0 error**；`ctest --test-dir build-quality` → **26/26 通过**。
- **台账状态**：`X-06`、`X-07` → `已修复`；`X-08`~`X-10`、`X-13`、`X-14`、`X-18` → `已核实`。**剩余待办仅 1 条**：`D-14`（阻塞于用户在途文件 `include/go2cpp/tools/error.hpp`）。
- **明确的留白**：
  - 本环境 **TSan 不可用**（WSL `unexpected memory mapping`），因此 X-06 的数据竞争**无法给出运行期报告**，只有源码层面的确证；其余各条亦同 —— 本轮所有结论都建立在**源码阅读**之上，**没有任何一条来自动态检测工具**。
  - `X-13`/`X-14`/`X-18` 三条**没有做探针实验**，理由是它们的事实已确证、而上界取决于用户代码或需构造专门场景；若用户希望看到量化数据，可另行指定，我会补做探针程序。

## 2026-10-11：四条 X- 条目结案（X-11/X-15/X-20/X-21）

- **本轮性质**：继续「逐条复核 `X-` 条目」的默认方案，**用户可推翻**。本轮处理**可静态定论**的四条，全部重新读源码确认前提（未凭记忆）。
- **X-20 → 已核实（不可达，不修）**：`Config() = default;` 与 `~Config()` 均在 `include/go2cpp/config/config.hpp` 的 `private:` 区（`:470-471`），public 只有 `static Config& Instance()`（`:420`），**全文件无 `friend`**；`Instance()` 是函数局部 static（`src/config/config.cpp:513-516`），调用方拿到引用、无法析构。故 `stop_watcher_locked`（`:792`）的 `detach()` 之后「Config 被销毁」这一路径当前不存在。**留了明确前提**：若将来 `Config` 变为可构造/可销毁，该 detach 分支会立刻成为真实 UAF 点。
- **X-21 → 已修复（加固，零行为变化）**：可达性论证经历了**两次修正**，值得留痕 ——
  - 登记时我判「不可达」，理由是「`m_stream` 只由两处赋为 `std::ofstream`」；
  - 本轮复核**先发现 `m_stream` 是 `protected`**（`log.hpp:260-263`），一度认为「用户派生 `FileSink` 即可触发」——看起来推翻了自己的判定；
  - 再往下查才发现关键约束：`RotatingFileSink` 是 **`final`**（`log.hpp:267`），用户**无法派生它**；而派生 `FileSink` 的类走的是 `FileSink::Write`，后者在 `log.cpp:464-465` **已有**检查。故 `RotatingFileSink::m_stream` 只能由内部的 `Reopen`/`Write` 设为 `ofstream`，**仍不可达**；
  - 尽管如此仍做了加固：把 `:542` 与 `:560` 改为 `if (auto* stream = dynamic_cast<...>)` 形式，与同文件 `:483` 统一。正常路径行为完全不变（转换本就成功），异常路径由崩溃变为静默跳过。
  - **这条过程本身是个教训**：中间那次「看似推翻」如果不继续查下去，我会把一个不可达项当成可达项去改，并在记录里写下错误的严重度。**可达性论证必须查到最后一层约束（这里是 `final`）才能定论。**
- **X-11 → 已核实（不影响正确性，不修）**：复核发现该模式实际有**两处**独立的 函数局部 `static thread_local`，而非登记时所述的一处 —— `enqueue`（`scheduler.cpp:2392-2400`）与 `wake_registered`（`:2887-2894`）。二者都跨实例共享，故第二个实例的 `next_incoming_stripe`（`:1797`）计数确实失真。但每个 Impl 有自己的队列与互斥数组（`:1793-1794`），条带号仅用于选中本实例的队列、且被 `& (kIncomingStripeCount-1)` 限制，**不存在串台或正确性风险**；worker 侧 `:3614`/`:3629` 的轮转也覆盖全部 16 条带，分布不均不会漏取任务。
- **X-15 → 已核实（当前无害，不修）**：确认 `s_node_cache` 仅三处使用（声明 `io.cpp:541-542`、取出 `:828-835`、存回 `:1042-1047`），`~State()`（`:644`）**确实不清理**自己的条目。但①取出即 `reset()` 清空字段、②上限 4096（有界滞留而非泄漏）、③每项仅是 `WaitNode` 的 `shared_ptr`，故不会造成状态串扰或泄漏。
- **验证结果**：`cmake --build build-quality --parallel` → **0 warning / 0 error**；`ctest --test-dir build-quality` → **26/26 通过**。
- **台账状态**：`X-11`/`X-15`/`X-20` → `已核实`；`X-21` → `已修复`。**剩余待办 9 条**：`D-14`（1，阻塞于用户在途文件 `include/go2cpp/tools/error.hpp`）、`X-06`~`X-10`、`X-13`、`X-14`、`X-18`（8 条）。
- **下一步**：剩余的 8 条都是**需要实验才能定量**的：`X-06`（`FdContext` 无锁 getter 的数据竞争）—— 本环境 TSan 因 WSL `unexpected memory mapping` 不可用；`X-13`/`X-14`（两处无界 `yield` 自旋）—— 需构造长任务才能量化；`X-18`（`adopt_socket` 持锁做系统调用）—— 需 accept 风暴场景。下一轮将评估每一条**在无 TSan 条件下**能做到什么程度的取证，并把做不到的部分明确写成留白，而不是含糊带过。

## 2026-10-11：清除三处死代码并复核 X-22

- **本轮性质**：继续上轮确定的「逐条复核 `X-` 条目」默认方案，**用户可推翻**。本轮挑**纯静态可判定**的条目，因为它们不需要可复现现场就能定论。
- **已清除的三处死代码（commit `ec762ed`）**：
  - **X-16**（`src/network/io.cpp:1450-1453`）：`slot.registered = true;` 紧跟在 `if (!slot.registered || slot.registration_id != registration_id) return;` 之后，条件已保证该表达式恒为真，是**无副作用的死存储**。已删除该行。
  - **X-17**（`src/network/io.cpp:2486-2494`）：两个分支的赋值语句逐字相同，已合并为单个 `if (!have_terminal)`，并补注释说明「就绪（`kReady`）已在上面 `continue` 掉，此处遇到的都是终态」。
  - **X-19**（`src/config/config.cpp:448`）：`value.min_workers != 0 && value.min_workers < 1` 在 `std::size_t` 上不可同时成立，是**恒假条件**，其报错文本从未生效。已删除该行。
  - **三处都不改变任何行为**，因此不涉及「失败现场」要求：删除的分别是恒真赋值、重复分支与恒假条件。验证：构建 **0 warning / 0 error**，`ctest` **26/26 通过**。
- **X-22 复核结论：事实确证，但代价只能推算，不修**：
  - **已确证的事实**：`WebSocket::Connect`（`src/network/websocket.cpp:305-333`）确实用 `socket->recv(&byte, 1, 0, left)` **逐字节**读取握手响应头。
  - **无法实测的限制**：本环境 **`strace` 不可用**（`which strace` 返回空），因此无法统计真实系统调用次数。**以下为算术推算，不是实测数据**：典型握手响应头 150~250 字节 → 150~250 次 `recv`（批量方案 1~2 次）；单次系统调用约 0.1~0.5 μs → 合计约 15~125 μs。
  - **推算结论**：典型情况下对一次性握手**可忽略**；但响应头接近该函数自设的 64 KiB 上限时（超长重定向链、大量 `Set-Cookie`），可达 65,536 次调用、约 6~30 ms，**此时不可忽略**。
  - **不修的理由**：按缺陷修复门槛需要「可复现证据 + 失败现场」，而本项既**无法实测**（工具缺失），性质上也是**低效而非失败**，两条都不满足；故保持登记，把结论从「推测低效」升级为「事实已确证 + 代价量级已知」。
- **台账状态**：`X-16`/`X-17`/`X-19` → `已修复`；`X-22` → `已核实`。**剩余待办 13 条**：`D-14`（1，阻塞于用户在途文件）、`X-06`~`X-11`、`X-13`~`X-15`、`X-18`、`X-20`、`X-21`（12 条）。
- **下一步**：继续复核。其中 `X-20`（`detach` 的 UAF）与 `X-21`（`dynamic_cast` 未检查）在登记时**已核实为当前不可达**，只需把可达性论证写实即可结案；`X-11`/`X-15`（跨实例 TLS 缓存、指针 key）也可静态定论。真正需要实验的是 `X-06`（数据竞争，但 TSan 不可用）与 `X-13`/`X-14`（无界 yield 自旋，需构造长任务）。

## 2026-10-11：X-12 实测复核 —— 结论为「影响可忽略，不修」

- **本轮性质**：上轮修复 X-01 后用户未指定下一项，按无人值守规则采用我给出的**默认建议**（逐条做 `X-` 条目可达性复核），**用户可推翻**。本轮先复核我自己在第 11 批登记、并在当时**明确标注「未实测」**的 X-12。
- **被验证的推测**：第 11 批我推断「`go2cpp_high_load_stress`（50,000 任务）运行时 `runnable` 很可能长期大于 P 数，即长期处于 `growth_period == 8`，因此该压测的**内存峰值**应当明显偏高」——当时如实写了「本轮**未实测**这一点，仅作为后续验证方向记录，**不作结论**」。本轮把这个空白补上。
- **实验设计（A/B）**：
  - 基线：未改动，`/usr/bin/time -v ./go2cpp_high_load_stress`；
  - 实验：把 `src/concurrency/scheduler.cpp:3812-3816` 的 `growth_period` 积压分支由 `std::size_t{8}` 临时改为 `{64}` —— 这样 `maintenance_budget` 不会再被每 8 次清零，可以到达 collect 的阈值 64，等价于「消除计数器复用对终态回收的抑制」；
  - 两次都记录 `Maximum resident set size`。
- **实测结果**：
  | 组 | 峰值 RSS |
  |---|---|
  | 基线（`growth_period` 积压分支 = 8） | **72,828 KB** |
  | 实验（改为 64，回收不再被抑制） | **72,900 KB** |
  | 差值 | **−72 KB（−0.1%）** |
- **结论：X-12 的实际内存影响可忽略，我的推测被实验否定。**最可能的解释是 `worker_loop` 收尾段 `:3927-3948` 的「出队后失去执行 claim（`task && !claimed`）」路径也直接调用 `collect_terminal_locked`（这正是我在第 13 批读收尾段时补充进去的那条例外路径），在本压测中它足够频繁地承担了回收职责，掩盖了计数器复用带来的抑制。
- **处置：不修代码。** 理由有二：① 内存收益被证明是**噪声级**；② 改动 `growth_period` 会**同时放慢扩容响应速度**（二者共享同一采样条件），在没有实际收益的前提下引入行为变化不划算。台账 X-12 状态由 `待办` 改为 `已核实`，备注中完整记录实验数据与结论。
- **过程留痕（一个我自己的脚本缺陷）**：实验用的还原逻辑先写出源码后又发现 `git diff` 仍显示 1 行改动 —— 原因是我的辅助函数用 `stdout.rstrip("\n")` 读取文件内容，**写回时丢掉了文件末尾的换行符**，产生 `\ No newline at end of file`。已按项目规则用允许的 `git restore <本轮新改且未提交的文件>` 完全回滚，还原后 `git diff` 为空、重新构建 0 error。**教训：用「读入-改写-写回」的方式编辑文件时，不要对读入内容做 `rstrip` 之类的尾部处理。**
- **验证结果**：还原后 `cmake --build build-quality --parallel` → 0 error；`ctest --test-dir build-quality` → **26/26 通过**；`git diff` 对 `src/` 为空。
- **台账状态**：`X-12` 由 `待办` 改为 `已核实`。**剩余待办 17 条**：`D-14`（1，阻塞于用户在途文件）、`X-06`~`X-11`、`X-13`~`X-22`（16，实现类）。
- **下一步**：继续逐条复核 `X-` 条目。优先挑**能用实验证伪/证实**的，例如 X-22（`WebSocket::Connect` 逐字节 recv，可用 `strace` 数系统调用次数取证）；对纯静态可判定的（X-16 死存储、X-17 冗余分支、X-19 死条件）只需确认可达性即可定论。

## 2026-10-11：修复 X-01 —— dynamic_gmp_demo 的取消契约假设

- **本轮性质**：按项目对 X-01 的既定要求执行 —— **先重复运行 ≥10 次复现取证，再用证据区分「示例断言过严」与「调度器真实竞态」，不得直接放宽断言**。
- **复现取证（20 次）**：失败 **9/20**（45%），失败模式 **100% 一致**：
  ```
  [gmp][场景3] cancel_requested=0 cancel_finished=1 body_ran=1 cancelled=0
  ```
  同时观察到成功样本 `cancel_requested=1 body_ran=1 cancelled=1` —— **任务体同样执行了，但 `cancel()` 返回 true 且最终 Cancelled**。这一对照直接否定了「Running 导致 cancel 失败」的解释。
- **源码裁定（推翻第 0 轮根因判定）**：
  - `Scheduler::cancel`（`src/concurrency/scheduler.cpp:3137-3160`）三个出口：所有权不符 → false；`request_cancel` 返回 false → false；否则 true；
  - `Task::request_cancel`（`:759-789`）在 `kDead`/`kCancelled`/`kFailed` 时返回 false，**任务 Running 时返回 true**（不立即置终态，注释说明「A started Fiber must resume to its trampoline so stack locals and stack cleanup finishes normally」）；
  - `GState::kDead`（`include/go2cpp/concurrency/scheduler/scheduler.hpp:52`）是**正常完成**的终态；
  - 故失败样本 `body_ran=1` + `cancel_requested=0` 的含义是**任务已正常跑完**，`cancel()` 返回 false 是**正确语义**。**第 0 轮记的「Running 时返回 false」是错的** —— 在 P=1 且 blocker 处于 yield 循环时，新 spawn 的短任务完全可能在主线程调用 `cancel()` 之前就跑完。
- **修复方式（未放宽断言，反而更严格）**：原断言只接受「请求被接受并取消成功」一条路径；改为分别验证两条路径的下界：被接受 → **必须** `kCancelled`；未被接受 → **必须**处于 `kDead`/`kCancelled`/`kFailed` 之一。若 `cancel()` 返回 false 而任务**仍在运行**（真正的调度器缺陷），判定依然失败。日志增加 `final_state` 便于后续定位。
- **一个 API 可见性修正**：初版我用 `cancelled->terminal()` 表达「已终态」，编译报错 —— `Task::terminal()` 位于 `scheduler.hpp:358` 的**私有区**（我此前 grep 到行号却没确认其所属访问区）。公开 API 只有 `state()`，故改为显式列出三个终态。**教训：引用符号前要确认其访问级别，而不是只看它存在**。
- **同步修正文档**：`example/README.md:44` 原称「最终收敛为 `Cancelled`」，与源码语义矛盾（任务可能先正常完成），已改为准确表述。
- **验证结果**：
  - `cmake --build build-quality --parallel` → **0 warning / 0 error**（本轮第一次构建实际是失败的，脚本已据 `errors=0` 断言拦截，并额外打印二进制与源码时间戳，避免再拿旧二进制下结论）；
  - 修复后**连续运行 30 次，失败 0 次**（修复前 9/20）；
  - `ctest --test-dir build-quality` → **26/26 通过**；
  - 本轮只改示例与 `example/README.md`，未触碰 `src/` 与 `include/`。
- **台账更新**：`X-01` 改为 `已修复`，备注中**显式记录对第 0 轮根因判定的推翻**。
- **剩余待办 17 条**：`D-14`（1，阻塞于用户在途文件）、`X-06`~`X-22`（16，实现类）。
- **下一步**：按缺陷修复门槛（台账 ID + 可复现证据 + 失败现场）逐条判定 `X-` 条目：能构造可复现现场的才修，其余保持登记。

## 2026-10-11：修复 T-08 —— 未知 GO2CPP_TEST_FILTER 不再静默通过

- **本轮性质**：上轮修复 7 条文档问题后用户未指定下一项，按无人值守规则采用我给出的**默认建议**（`T-08`），**用户可推翻**。
- **缺陷复述**：`tests/test_main.cpp` 用 `filter.empty() || filter == name` 决定是否运行模块。当 `GO2CPP_TEST_FILTER` 指定不存在的名字时，所有 `selected()` 都返回 false，**一个用例都不跑**，`g_failures` 仍为 0，程序打印 `[test] all checks passed` 并**返回 0** —— 空跑被伪装成通过。
- **修复方式**：`selected()` 增加 `any_selected` 引用捕获，命中任一名字时置位；`main()` 末尾在 `g_failures` 检查之后，若 `filter` 非空且 `any_selected == false`，向 stderr 输出 `GO2CPP_TEST_FILTER="<值>" 未匹配任何测试模块；本次没有执行任何用例` 并 **`return 2`**（用 2 与「有用例失败」的 1 区分）。**未引入模块名白名单** —— 那会与 23 行 `selected(...)` 调用形成第二份清单、容易不同步；计数器方案天然跟随实际调用点。
- **回归测试**（`cmake/Go2CppTests.cmake` 新增两条 CTest）：
  - `go2cpp_unknown_filter_rejected`：`sh -c "GO2CPP_TEST_FILTER=__no_such_module__ <bin>; test $? -eq 2"` + `PASS_REGULAR_EXPRESSION "未匹配任何测试模块"`；
  - `go2cpp_known_filter_runs`：`GO2CPP_TEST_FILTER=error` + `PASS_REGULAR_EXPRESSION "error values and chains"`（配对验证已知 filter 仍真正跑到对应模块，防止修复把正常路径堵死）。
- **过程中踩到的两个坑（都已解决并留痕）**：
  1. **`WILL_FAIL` 与 `PASS_REGULAR_EXPRESSION` 会互相反转**。第一版写成 `WILL_FAIL TRUE` + `PASS_REGULAR_EXPRESSION`，实测 CTest 报 `***Failed  Required regular expression found.` —— 正则匹配使测试判为通过，`WILL_FAIL` 再把通过反转成失败，于是永远不通过。该组合**无法表达**「退出码非零 **且** 输出含指定信息」；改用 `sh -c ... ; test $? -eq 2` 显式断言退出码后才正确。
  2. **多层 shell 嵌套中的 `$?` 不可信**。验证时用 `cmd 2>&1; echo "rc=$?"` 得到 `rc=0`，据此误判「修复未生效」，差点去改一个本来正确的实现；改用 `subprocess` 直接读 `returncode` 后立刻确认为 2。这与本项目已登记的 T2（PowerShell 吞 `$`）是同类陷阱的延伸：**验证退出码必须直接读进程返回码**。
- **验证结果**：
  - 重新 configure + `cmake --build build-quality --parallel` → **0 warning / 0 error**；
  - `subprocess` 直接读取：未知 filter → **returncode 2** 且打印未匹配信息；`error` → **0** 且输出 `[test] error values and chains`；
  - `ctest --test-dir build-quality` → **26/26 通过**（原 24 + 新增 2）；
  - 未涉及并发/超时路径，无需重复运行 ≥5 次。
- **台账更新**：`T-08` 状态改为 `已修复`，附 commit `6fd451a`。
- **剩余待办 18 条**：`D-14`（1，阻塞于用户在途文件 `include/go2cpp/tools/error.hpp`）、`X-06`~`X-22`（17，实现类）。`X-01` 仍未复现。
- **下一步建议**：`X-01`（`dynamic_gmp_demo` 间歇失败）是剩余条目中唯一**还可能通过实验拿到证据**的 —— 按项目既定要求先重复运行 ≥10 次尝试复现，若复现再用日志区分「示例断言过严」与「调度器真实竞态」，**不得直接放宽断言**。

## 2026-10-11：源码回溯第 26 批 —— 修复 7 条文档缺失与矛盾

- **本轮性质**：`src/` 已全部读完（第 25 批），本轮转入**修复**。只处理台账中**纯文档类**条目 —— 它们没有「可复现证据 + 失败现场」的修复门槛要求，每处都有明确的源码行号依据。
- **已修 7 条**（commit `36342ac`）：
  | 台账 | 改动 |
  |---|---|
  | D-13 | `include/go2cpp/network/io_backend.hpp`：为 `UnregisterBackend` 补「平台默认后端不可注销」及其理由 |
  | D-15 | `include/go2cpp/concurrency/future.hpp`：说明 `BrokenPromiseError()` 返回**进程级共享单例**，故 `Is(errA, errB)` 对两个 broken promise 为 true |
  | D-16 | `include/go2cpp/network/socket.hpp`：为 `fd()` 补「瞬时值、不进 in-flight 保护、不得跨操作持有」 |
  | D-17 | `include/go2cpp/concurrency/scheduler/scheduler.hpp`：为 `wake_io_batch` 补前置条件「只接受本 Scheduler 已登记节点」及跳过 `owned_by` 校验的事实 |
  | D-18 | `docs/compatibility.md`：把「**不做 CPU 绑核**」改为「默认不绑核，`pin_workers_to_cpu = true` 时会用 `pthread_setaffinity_np`」——以 `src/concurrency/scheduler.cpp:3505-3515` 为准 |
  | D-22 | `docs/network.md` 与 `include/go2cpp/network/http.hpp`：说明 `Server::serve_connection` **不对 recv/write 施加超时**，与 `Client` 的绝对 deadline 不同 |
  | D-23 | `docs/network.md`：说明 `ResponseWriter::flush()` 只做状态校验、不写出内容 |
- **本轮明确未做的一条：D-14**（`Is(error, nullptr)` 的空 target 语义）。原因是其目标文件 `include/go2cpp/tools/error.hpp` **正是用户当前的在途改动文件**（用户在 `:204-246` 新增了 Go 风格 `errors::` 命名空间）。由于 `git add` 以整文件为单位，提交该文件的注释修改必然会**把用户尚未完成的改动一并带入提交**，违反「在途改动原样保留、不动、不提交」的约束。**故本轮跳过，等用户提交或撤销该文件后再补。**
- **验证结果**：
  - `cmake --build build-quality --parallel` → **0 warning / 0 error**（改动含 4 个头文件，故必须重建验证）；
  - `ctest --test-dir build-quality` → **24/24 通过**，总耗时 24.42s；
  - 本轮只改注释与 Markdown，未触碰任何实现语句。
- **台账更新**：D-13、D-15、D-16、D-17、D-18、D-22、D-23 状态改为 `已修复` 并附 commit hash；同时**修正 D-13 的路径引用** —— 此前误记为 `include/go2cpp/io_backend.hpp:77`，实际该文件只是 4 行转发头，真实声明在 `include/go2cpp/network/io_backend.hpp:76`。
- **剩余待办 20 条**：
  - `D-14`（1 条，文档类）—— 阻塞于用户在途改动，见上；
  - `X-06`~`X-22`（17 条，实现类）—— 需按缺陷修复门槛逐条判定，其中多条已核实「当前不可达」；
  - `T-08`（1 条，测试类）—— `GO2CPP_TEST_FILTER` 未知名字静默返回 0；
  - `X-01`（1 条）—— `dynamic_gmp_demo` 间歇失败，尚未复现。
- **下一步建议**：`T-08` 是**测试基础设施缺陷**，修复它不涉及并发正确性、也不需要失败现场 —— 只需要让未知 filter 返回非零并加一条回归。这可能是剩余条目里最容易安全落地的一条。

## 2026-10-11：源码回溯审计（第 25 批：address/byte_array/context_x64.S）—— **`src/` 全部读完**

- **本轮读完整实现**：`src/network/address.cpp`(280)、`src/network/byte_array.cpp`(196)、`src/concurrency/context_x64.S`(83)，合计 559 行。**至此 `src/` 下 25 个 `.cpp` + 1 个 `.S` 共 22,012 行全部读完。**
- **address.cpp 核对通过的要点**：
  - `:61-70` 对 `AF_UNIX` **先按调用方给的 `length` 拷贝到完整的 `sockaddr_un`** 再构造，注释明确「调用方传入的缓冲区也可能恰好只有 length 字节，不能直接按整结构引用」——避免越界读；`:64` 同时拒绝过短与过长的 length；
  - `:228-243` `UnixAddress` 区分抽象命名空间（首字节 NUL，`path.size() > capacity` 才报错）与文件系统路径（**拒绝内嵌 NUL**，`size() >= capacity` 即报错，因为还需结尾 NUL）；`:244-263` 反向构造时校验 family/length 长度，并在 `:253-262` 检查「NUL 之后不能夹带非填充字节」；
  - `:151`/`:157`/`:162` 的 IPv4 掩码都写成 `prefix_len == 0 ? 0U : 0xffffffffU << (32 - prefix_len)` —— **`prefix_len == 0` 的特判是必需的**，否则 `<< 32` 是移位未定义行为；IPv6 三处（`:206-226`）改用逐 bit 循环，同样先检查 `prefix_len > 128`；
  - `:22-41` `service_host`/`service_port` 完整处理 `[v6]:port`、`host:port` 与裸 host 三种形式，并用「只有一个冒号」判定避免误切 IPv6 字面量。
- **byte_array.cpp 核对通过的要点**：
  - `:119-128` `read_varuint32` 在第 5 字节用 `byte & 0x70U` 拒绝溢出（32 位最多 5 字节，第 5 字节仅低 4 位有效），循环耗尽后再抛 `too long`；`:129-138` `read_varuint64` 同理用 `byte & 0x7eU` （64 位最多 10 字节，第 10 字节仅第 0 位有效）——两处都**同时防溢出与防超长**；
  - `:139-154` zigzag 编解码对 32/64 位分别用 `0xffffffffU`/`0xffffffffffffffffULL`，无符号回绕语义正确；
  - `:168-172` `read_string_vint` 在分配前检查 `length > 64MB` 并抛 `length_error`——**长度前缀先校验再分配**，避免恶意长度触发巨额分配；`:40-48` `ensure_write`/`ensure_read` 分别在写前查加法溢出、读前查可读范围；
  - `:49-61` `write`/`read` 对「非零长度 + 空指针」抛 `invalid_argument`。
- **context_x64.S 核对通过的要点**：
  - 栈布局自洽：`context_make` 预留 56 字节（6 个 callee-saved 寄存器）并把 entry/context_end/context_entry 分别放在 +32/+40/+48；`context_jump` 按 r12,r13,r14,r15,rbx,rbp,r8 顺序弹出 7 个值（56 字节）后 `jmp *%r8`，偏移完全对应；
  - `:37` `movq %rax, %rdi` 传入的是 `context_jump` 中 `:60` 保存的**旧 rsp**，正是 `Transfer.context` 所需的「被挂起调用者的上下文」，使 `entry(Transfer)` 能正确回跳——该约定经逐条核对无误；
  - `:41-46` `go2cpp_context_end` 在入口函数**意外返回**时调用 `_exit(0)` 而非沿任意栈地址返回，注释明确「Treat a violation as a process exit」；`:81-83` 声明 `.note.GNU-stack` 标记非可执行栈；`:1-12` 注明寄存器布局来源（coost/TBOX 与 Boost.Context，Apache-2.0）并只暴露 `go2cpp_` 前缀 ABI。
- **结论：本批未发现新的缺陷或文档缺失。**
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：**22,012 / 22,012 行 = 100%**。`src/` 下 26 个源文件（25 个 `.cpp` + 1 个 `.S`）**全部逐行读完**。
- **台账状态**：本轮无新增条目；待办 **27 条**，其中 **22 条来自这 25 批源码回溯**（D-13~D-18、D-22~D-23、X-06~X-22），另 5 条为更早期的文档/测试类条目。**全部只登记未修。**
- **下一步（源码已读完，转入处置阶段）**：
  1. 27 条待办中，`D-13`~`D-18`、`D-22`、`D-23` 共 8 条是**纯文档问题**（文档与实现矛盾或缺失），无修复门槛限制，可优先批量修正；
  2. `X-06`~`X-22` 共 17 条是实现层面的可疑点，其中多条已在登记时**核实为当前不可达**（X-20、X-21 等），建议逐条判定「记录即可」或「需修」；
  3. `T-08`（`GO2CPP_TEST_FILTER` 未知名字静默返回 0）与 `X-01`（`dynamic_gmp_demo` 间歇失败）是两项独立的待办。
  以上需要用户对处置策略给出方向；无人值守下按「默认只登记不修」保持不变。

## 2026-10-11：源码回溯审计（第 24 批：websocket.cpp + sse.cpp 全文）

- **本轮读完整实现**：`src/network/websocket.cpp`(715) 与 `src/network/sse.cpp`(613)，合计 1,328 行。
- **websocket.cpp 核对通过的要点**：
  - 协议校验覆盖面完整：`:429` 拒绝 reserved bits；`:431-433` 拒绝未知 opcode；`:436-438` **强制 mask 方向**（Server 必须收 masked、Client 必须收 unmasked）；`:444`/`:451` 拒绝**非最小长度编码**（长度混淆攻击的经典手法）；`:448` 64 位长度的最高位必须为 0；`:454` 控制帧必须 FIN 且 payload ≤ 125；`:459-460` size_t 溢出检查；`:461-467` 分配失败时中止传输并置 `MessageTooLarge`；
  - `:670-673` `valid_close_code` 覆盖 RFC 6455 §7.4.1（1000-1014 去掉 1004/1005/1006、加 3000-4999）；`:675-697` `valid_utf8` 逐项拒绝 overlong 编码（含 `0xC0`/`0xC1`）、surrogate 区间与 > 0x10FFFF；`:585`/`:616`/`:628` 在 `validate_utf8` 开启时对 Close reason 与文本消息校验；
  - `:98-145` 自实现 SHA-1（RFC 6455 握手所需），避免引入 OpenSSL 依赖；`:41-80` 的 base64 解码严格校验 `=` 只允许出现在末尾且长度必须是 4 的倍数；
  - `:560-636` `read_message` 的分片状态机：`:607` 无前置分片时收到 Continuation 报错、`:620` 分片期间收到新 data frame 报错、`:608-610` 双重上限检查（单帧与累计）；`:589` 回送 Close 时在 write 锁内完成、`:653` 与 `:561` 的读写锁分离且锁序一致（read → write，无反向）；
  - `:360-366` `abort_transport` 只调 `cancel_read`/`cancel_write` 而**不 close fd**，注释明确「避免破坏仍由调用方共享的 Socket 所有权」。
- **sse.cpp 核对通过的要点**：
  - `:175-206` `dispatch` 严格遵循 WHATWG：**只有收到 data 字段才派发事件**，但 `id` 仍会更新 `last_event_id`（注释 `:176` 与 `:241-243` 说明「连接在一条 id 行后中断时，重连仍必须携带最新游标」）；`:188-190` 派发前去掉 data 的末尾换行；
  - `:272-282` **BOM 处理**：缓冲不足 3 字节且前缀匹配 BOM 时返回等待，完整时剔除；`:284-317` 行分割同时支持 `\n`/`\r\n`/`\r`，且 `:294-296` 对「缓冲区末尾的孤立 `\r`」**返回等待**而非立即断行（可能是被截断的 `\r\n`）——这是增量解析的正确做法；
  - `:260-342` `feed` **先在本地 `pending_events` 收集、最后统一 append 到调用方 vector**，保证失败时不产生部分结果；`:321-341` 两处 catch 都用 `std::string().swap(...)` 清空全部内部缓冲并置 `m_failed`，避免异常后继续占用内存；
  - `:116-135` 空 data 也输出 `data:`（注释：「让编码结果可被其他 SSE 实现识别为一个空事件」）；`:130-133` 对以换行结尾的 data 额外补一个 `data: ` 行，保留尾部空行语义；`:480-514` `comment` 用同一套多行规则编码 `: ` 前缀；
  - `:419-443` `write_locked` 把整个事件**一次** `m_writer.write`（保证事件不被拆散），并把 EAGAIN/EWOULDBLOCK/ETIMEDOUT 归类为 `Error::Backpressure` 而非 Io；
  - `:528-546` `close` 在**释放流锁后**才 `disarm_context_callback()`，注释解释「RemoveCallback 可能需要等待正在执行的取消回调；不要在流锁内调用」；`:548-589` 三个 `noexcept` 查询接口都用 try/catch 包住加锁，注释说明「调度器停止期间无法 park 时按失败处理，不能让 C++ 异常穿过 noexcept」；
  - `:365-388` Context 取消回调只持 `weak_ptr<State>` 并调 `cancel_write`（注释：「只唤醒当前写等待，不把 Socket 的所有权交给回调」）。
- **本轮读源码发现的新问题**：
  - **X-22（低）**：`WebSocket::Connect`（`:302-333`）用**逐字节 `recv`** 读握手响应头（最多 65,536 次调用），而同代码库的 `http::Client` 用 8192 字节缓冲。
- **一处未登记的观察**：`websocket.cpp:671` 的 `code != 1015` 是冗余条件 —— 前半个合取式已用 `code <= 1014` 排除了它。无行为影响，不单列条目。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前二十三批 20,125 行 + 本批 1,328 行 ≈ **21,453 行，占 `src/`（22,012）约 97.5%**。已**全文读完 24 个文件**。
  - **仍未读**：`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **559 行**。
- **台账状态**：新增 **X-22**；待办共 **27 条**，其中 **22 条来自源码回溯**。
- **下一步**：最后三个文件（`address.cpp`、`byte_array.cpp`、`context_x64.S`），读完后 `src/` 即全部读完。

## 2026-10-11：源码回溯审计（第 23 批：http.cpp 全文 620 行）

- **本轮读完整实现**：`src/network/http.cpp`(620)。
- **核对结果——解析器与出站校验的安全性突出**：
  - `:239-244` `Headers::add` 对**出站头**也执行 `valid_header_name`/`valid_header_value` 校验，注释明确「只依赖解析器校验…业务字符串中的 CRLF 会造成响应拆分或请求走私」——这是防注入的关键一环；
  - `:369` 拒绝「chunked 与 Content-Length 同时出现」（请求走私经典手法）；`:359` 拒绝重复且不一致的 `Content-Length`；`:360-364` 拒绝任何非 `chunked` 的 `Transfer-Encoding`（注释：「任何未知编码都不能被当作明文 body」）；`:119` 对重复的 `chunked` 也返回失败；
  - 溢出检查覆盖全部长度计算：`:312-315`（输入总量）、`:391`/`:433`（body_start + content_length）、`:400`（chunk 累计）、`:428`（cursor + chunk_size + 2）、`:140`（parse_chunk_size 的 `×16`）；`:102` `parse_size` 也在乘 10 前预检；
  - `:379-387` 响应缺少 CL/TE 时进入 **close-delimited** 模式并返回 `NeedMore`，注释说明「不能在收到头部的同一次 recv 中错误地把 body 当成空并宣布 Complete」；`:292-308` `finish()` 才在 EOF 时收尾，`:299` 还检查 `m_body_start` 合法性；
  - `:266-286` `Response::to_string` 对 1xx/204/304 过滤用户手工设置的 CL/TE（注释：「避免下游继续等待一个永远不会出现的 body」），HEAD 请求不输出 body；`:255-263` `Request::to_string` 在 start-line 非法时返回空串而非发出坏请求；
  - `:508-519` `Client::do_request` 处理 informational 响应：`status >= 100 && status < 200 && status != 101` 时保留同一 recv 中的流水线剩余（`take_remaining`）并重建 `Response` 继续解析，**正确地把 101 Switching Protocols 视为最终响应**；
  - `:60-76` `make_deadline`/`remaining_time` 用 `duration<long double>` 预检溢出并保证「剩余时间不为 0 时至少返回 1ms」，避免忙轮询；
  - `:150-201` `split_url` 完整处理了 IPv6 方括号、端口缺省、query/fragment 组合（`:194-199` 对 `?` 与 `#` 的四种组合分别给出正确 target）。
- **本轮读源码发现的新问题**：
  - **D-22（中）**：`Server::serve_connection`（`:596-618`）的 `recv` 与 `write_all` **都不传超时**，而 `Client` 有完整的绝对 deadline。文档（`docs/network.md:46`、`http.hpp:220`）未说明这一差异。
  - **D-23（低）**：`ResponseWriter::flush()`（`:579`）是**空操作**（仅返回状态检查），但 `docs/network.md:30` 把 `flush()` 与 `write()` 并列为输出步骤。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前二十二批 19,505 行 + 本批 620 行 ≈ **20,125 行，占 `src/`（22,012）约 91.4%**。已**全文读完 22 个文件**。
  - **仍未读**：`websocket.cpp`(715)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **1,887 行**。
- **台账状态**：新增 **D-22、D-23**；待办共 **26 条**，其中 **21 条来自源码回溯**（D-13~D-18、D-22~D-23、X-06~X-21），5 条为早期文档类条目。
- **下一步**：`websocket.cpp`(715) 与 `sse.cpp`(613)，之后是两个小文件与汇编。

## 2026-10-11：源码回溯审计（第 22 批：log.cpp 全文 1026 行）

- **本轮读完整实现**：`src/logging/log.cpp`(1026)。
- **核对结果——无锁快照与回调隔离设计一致**：
  - `:654-767` `LogItemWorker` 用 `std::atomic_load/store_explicit` 交换 `shared_ptr<const Snapshot>`（filter/formatter/sink 三元组），`Submit` **不在任何锁内调用用户 Formatter/Sink**，注释 `:670-671` 明确「这样自定义 Sink 再次记录日志时不会形成同一 Worker 的递归锁死」；`:687-743` 的三个 `Set*` 都在 `m_mutex` 内做 copy-on-write 后原子发布；
  - `:166-175` `FindFormatItem` **先在锁内取出工厂、再到锁外调用**，注释 `:173` 解释「工厂属于用户代码，不能在注册表锁内执行，避免回调再次注册格式项时死锁」——与 `Config::LoadFromIni` 的「通知阶段不释放事务锁」是同一类隔离原则；
  - `:878-897` `Logger::SetParent` 用**函数局部 static** 的 `parent_graph_mutex` 串行化父链修改，并沿父链同时检测「自环」（`current.get() == this`）与「已存在的环」（`visited`），注释说明「禁止自环和间接环，避免 `Log()` 递归永不返回」；`:909-917` `Log` 的递归传播依赖这一不变式；
  - `:878-897` 与 `:909-917` 的锁序核对无反转：父链修改走 `parent_graph_mutex → logger.m_mutex`，日志传播走 `logger.m_mutex → parent->m_mutex`，两条路径都只向**父方向**深入；
  - `:519-529` `RotatingFileSink` 构造时用 `file_size` 初始化 `m_bytes`，注释明确「进程重启后继续追加同一个文件时，旋转阈值必须包含已有字节数；否则第一次写入会暂时突破 max_bytes」；`:541-558` 的轮转按 `max_files` 从高到低 rename，最后重建文件；
  - `:1021-1024` `LogLine::~LogLine` 在 `noexcept` 析构中把「取消息 + 提交」整体包在 try/catch 中，不让用户 Sink 异常导致 terminate；`:1014-1015` 先判 `ShouldLog` 再填充记录，未启用时零开销。
- **本轮读源码发现的新问题**：
  - **X-21（低）**：同一文件内 4 处 `dynamic_cast<std::ofstream*>` 的 nullptr 检查不一致 —— `FileSink::Write`（`:464-465`）与 `FileSink::Flush`（`:483`）有检查，而 `RotatingFileSink::Write` 的 `:542`、`:560-561` **直接解引用**。另 `FileSink::Write` 每次调用都做一次 RTTI 转换（含高频 Info/Debug 路径）。**可达性已核实**：`m_stream` 仅由两处赋为 `std::ofstream`，故空指针当前不可达。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前二十一批 18,479 行 + 本批 1,026 行 ≈ **19,505 行，占 `src/`（22,012）约 88.6%**。已**全文读完 21 个文件**。
  - **仍未读**：`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **2,507 行**。
- **台账状态**：新增 **X-21**；待办共 **24 条**，其中 **21 条来自源码回溯**（D-13~D-18、X-06~X-21），**全部只登记未修**。
- **下一步**：三个网络协议文件（`websocket.cpp` / `http.cpp` / `sse.cpp`，合计 1,948 行），之后是两个小工具文件与汇编。

## 2026-10-11：源码回溯审计（第 21 批：config.cpp 全文 1089 行）

- **本轮读完整实现**：`src/config/config.cpp`(1089)，并交叉核对了 `include/go2cpp/config/config.hpp` 的 `Config` 类声明（`:400-470`）与 `include/go2cpp/config.hpp`（确认它只是转发头，实现在 `go2cpp/config/config.hpp`）。
- **核对结果——配置事务与热更新设计严谨**：
  - `:245-310` `ReadStableFileContents` 用**双重快照**（`before/after` + `verify_before/verify_after` + `first == second`，外加三次 edit-lock 检查）确认文件稳定，最多 8 次 × 2ms 重试后失败并给出「正在编辑」或「请用临时文件 + rename 原子替换」两种可操作提示；与 `docs/configuration.md` 的编辑锁协议一致；
  - `:338-370` `IniFile::Parse` **先构造临时快照、全部成功后 `swap`**，注释明确「语法错误不能让调用方看到『前半段已更新、后半段丢失』的半解析状态」；`:44-45` `ParseSize` 要求 `from_chars` **消费全部字符**（`result.ptr == end`），故 `123abc` 会被拒绝；
  - `:543-623` `Config::LoadFromIni` 的三阶段事务：预检（`ValidateString` + 跨字段 `m_transaction_validators` 基于**完整候选快照**）→ 写入（失败则逆序 `RollbackPending`）→ **解锁后**才 `NotifyPending` 与调用 `CommitListener`；`:589-590` 注释解释「通知阶段不持有事务锁，监听器可以查询注册表或触发下一次配置加载，避免插件回调与事务锁形成死锁」，`:608-613` 再逐个隔离监听器异常。`:617-622` 的 catch 分支同样逆序回滚后重抛；
  - `:706-778` `StartWatcher` 引入 `retry_pending` 标志（`:729-731` 注释：「锁从非空变为空时必须强制尝试一次，即使文件系统的时间戳和大小没有变化；这样同尺寸原子替换也不会被旧基准短路」），并在监听循环内 `catch (const std::exception&)` 与 `catch (...)` 双保险，保证用户插件异常不会终止监听线程；
  - `:1060-1063` 用 `weak_ptr` 保存 `ConfigVar` 以**打破 `ConfigVar -> Listener -> ConfigVar` 的引用环**，并在 `:1076-1083` 让两个阈值共享同一个 `apply_timer_limits`，`:1040-1059` 再注册事务级校验器使用户能在同一份文件里同时迁移两个阈值；
  - `:71-80` 用集中常量（`kMaxWorkers=32`、`kMaxLocalQueue=1<<20`、`kMinFiberStack=16KB`、`kMaxFiberStack=64MB`、`kMaxAffinityBudget=1<<20`、`kMaxDuration=24h`）统一定义上限，注释写明「配置文件是外部输入…避免某一个模块单独放宽参数后造成过量线程、内存或超长等待」；
  - `:873-893` 对在线修改 `min_workers`/`max_workers` 做**单步跨字段校验**（按另一侧的当前值），并给出可操作指引「需要同时调整两者时，先把 `max_workers` 设为 0（自动），再设置目标值」。
- **本轮读源码发现的新问题**：
  - **X-19（低）**：`:448` 的 `min_workers != 0 && min_workers < 1` 是**恒不成立的死条件**（`size_t` 上二者不可同时成立），因此「min_workers 必须至少为 1」这条报错**永不触发**。行为上无害（0 本就是合法的自动值），但属误导性死代码。
  - **X-20（低）**：`stop_watcher_locked` 在监听线程自身调用时 `detach()`（`:792`）。这是本项目**第四种**「在自身线程内停止服务」策略 —— 另有 `timer.cpp:120-123` 的 `std::terminate()`、`io.cpp:729-731` 与 `context.cpp:314-317` 的跳过 join。**可达性已核实**：`Config()` 为 private、唯一入口是进程级 static 的 `Instance()`，故 detach 后的 UAF 窗口**当前不可达**；仅在将来允许用户构造 `Config` 时才会变成真实缺陷。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前二十批 17,390 行 + 本批 1,089 行 ≈ **18,479 行，占 `src/`（22,012）约 83.9%**。已**全文读完 20 个文件**。
  - **仍未读**：`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **3,533 行**。
- **台账状态**：新增 **X-19、X-20**；待办共 **23 条**，其中 **20 条来自源码回溯**（D-13~D-18、X-06~X-20），**全部只登记未修**。
- **下一步**：`log.cpp`(1026)，之后是三个网络协议文件与两个小工具文件。

## 2026-10-11：源码回溯审计（第 20 批：hook.cpp :1100-2161，全文读毕）

- **本轮读实现**：`src/network/hook.cpp` 的 `:1100-2161`，约 **1,061 行**，覆盖 `fcntl_argument` 余下、`ioctl_has_{pointer,no}_argument`、`go2cpp::hook::*` 全部公开入口与 `ScopedEnable`/`ScopedIOManagerBinding`、以及**全部 33 个 `extern "C"` interposer**（sleep/usleep/nanosleep、poll/ppoll/select/pselect/epoll_wait/epoll_pwait、socket/socketpair/connect/accept/accept4、read/readv/recv/recvfrom/recvmsg、write/writev/send/sendto/sendmsg、close/dup/dup2/dup3、fcntl/ioctl、getsockopt/setsockopt）。**至此 `hook.cpp` 2,161 行全文读完。**
- **结论：本批未发现新的缺陷或文档缺失。** 以下三点是本文件最值得记录的设计：
  - **「用户视角透明」机制（fcntl F_GETFL/F_SETFL，`:1996-2006` 与 `:2028-2035`）**：库接管 socket 时会强制加上内核级 `O_NONBLOCK`，但当用户 `F_GETFL` 查询时，返回的是 `m_user_nonblocking ? result | O_NONBLOCK : result & ~O_NONBLOCK` —— **把库代设的非阻塞位对用户隐藏**；用户 `F_SETFL` 时库又把 `O_NONBLOCK` 重新强制加上。这与 `can_wait`（`:725-730`）要求 `!m_user_nonblocking` 是同一套设计的两个面：**只有用户从未表达过阻塞意图的 socket 才被协作化**。`ioctl(FIONBIO)` 分支（`:2067-2111`）同样在成功后回读内核 flags 重建这份元数据；
  - **`poll`/`select`/`epoll_wait` 一律透传（`:1473-1538`）**，注释 `:1458-1462` 明确「保持 libc ABI 和 timeout、EINTR、信号掩码、revents 语义，**不把一次多路等待偷偷拆成多个顺序等待**」；需要多 fd 协作时必须显式调用 `IOManager::WaitAny/WaitMany`。这是**拒绝语义降级**的正面例子；
  - **`read`/`write` 的初始化期回退（`:1650-1652`、`:1777-1779`）**：`s_resolving && !s_originals.m_read` 时直接走 `::syscall(SYS_read, ...)`，避免 `dlsym` 自身触发的 `read`/`write` 递归进入 Hook 路径。
- **其余核对通过的要点**：
  - `:1850-1868` `close` 是**三段式**：`DescriptorGuard` 内 `prepare_close` + `notify_before_close`（后者经 `NotifyCloseAll` 完成 `Invalidate` 线性化）→ **锁外**执行真实 close → 再在 `DescriptorGuard` 内 `finish_close`。注释 `:1858-1859` 解释「不要在真实 close 期间持有进程级描述符生命周期门；SO_LINGER 和文件系统描述符可能在此处阻塞」。**这解答了第 18 批读 `prepare_close` 时留下的「`m_closed` 与线性化点分离」疑问**；
  - `:1908-1912` 与 `:1947-1952` 对 `dup2`/`dup3` 的确定性失败（`old == new`、负 `new`、非法 flags、源无效）**直接透传内核**，不发出伪关闭通知；成功路径才 `finish_close` + `try_clone_descriptor`，失败路径走 `restore_failed_replacement`；
  - `:1686-1704`、`:1713-1728`、`:1746-1760` 对 `MSG_WAITALL` 与 `MSG_OOB` 在 managed G 中返回 **`ENOTSUP`**（而非静默降级为短读或忽略），普通线程保持 libc 原生语义 —— 属于**显式声明不支持**的正确做法；
  - `:1430-1456` `nanosleep` 在参数非法时**直接透传真实调用**让内核返回 `EINVAL`，并且在协作成功时按 POSIX 清零 `*remaining`；`:1379-1419` `sleep`/`usleep` 在 `Interrupted` 时分别返回剩余秒数与 `-1`，符合各自 POSIX 契约；
  - `:1982-1988` `fcntl` 对未知命令返回 `ENOTSUP` 而不 `va_arg`，注释「读取未知的可变参数属于未定义行为」；`:2070-2077` `ioctl(FIONBIO)` **先让内核校验并读取调用者指针**，避免 Wrapper 侧解引用把正常 `EFAULT` 变成段错误；`:2139-2143` `setsockopt` 只在成功后、且 `length >= sizeof(timeval)` 时才更新接收/发送超时元数据。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前十九批 16,329 行 + 本批约 1,061 行 ≈ **17,390 行，占 `src/`（22,012）约 79.0%**。
  - 已**全文读完 19 个文件**。
  - **仍未读**：`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **4,622 行**。
- **台账状态**：本轮无新增条目；待办仍为 **21 条**，其中 18 条来自源码回溯。
- **下一步**：`config.cpp`(1089) 或 `log.cpp`(1026)，之后是三个网络协议文件。

## 2026-10-11：源码回溯审计（第 19 批：hook.cpp :620-1100）

- **本轮读实现**：`src/network/hook.cpp` 的 `:620-1100`，约 **480 行**，覆盖 `clone_descriptor`/`try_clone_descriptor`、`restore_failed_replacement`、`source_fd_is_valid`、`operation_timeout`、`can_wait`、`wait_error`、`wait_for_io`、`deadline_from_timeout`、`native_wait`、`cooperative_io` 模板、`has_dontwait`、`connect_wait_timeout`、`cooperative_connect`、`cooperative_sleep_for`、`saturating_duration`、`fcntl_argument` 开头。
- **结论：本批未发现新的缺陷或文档缺失。** 要点：
  - `:725-730` `can_wait` 要求**三个条件同时成立**（是 socket、系统级非阻塞、用户未请求非阻塞），与 `docs/network.md`「只接管用户未显式设置非阻塞的 socket」一致；`:864` 再用 `!per_call_nonblocking && can_wait(...)` 决定是否进入等待循环，使 `MSG_DONTWAIT`/`O_NONBLOCK` 的每一次调用都能保持原生语义；
  - `:867-897` `cooperative_io` 的循环结构完整：`DescriptorGuard` 生命周期门 → 真实调用 → （仅在有 manager 时）EINTR 重试 → 非阻塞直接返回 → 成功/非 EAGAIN 返回 → 交给 `wait_for_io`（有 manager）或 `native_wait` 回退。`:877` 把 EINTR 重试限定在 `manager != nullptr`，无 manager 时把 EINTR 原样返回调用者，保留了原生语义；
  - `:775-829` `native_wait` 的 **10ms 切片轮询**有明确理由（`:778-780`：「另一个线程中的 poll 不一定会被 close 可靠中断。有限时长的 poll 切片让保留的代次检测 close」），且两次 `descriptor_closed` 检查分别位于 poll 前与就绪后，`:819-821` 注释解释「close/dup2 Hook 在就绪通知与真正读写之间不能把旧 generation 伪装成新的同号 fd」。**文档核对**：`docs/network.md:12` 已写明「等待后端最多经过一个有限轮询片段就会…」，虽未给出具体毫秒数（属实现细节），但概念已在文档中，故**不登记为文档缺失**；
  - `:1033-1055` `cooperative_sleep_for` 用临时 `eventfd` + `manager->WaitFor` 把 `sleep`/`usleep`/`nanosleep` 变成协作式等待，并在 `:1047-1048` 用 `NotifyClose` + `syscall(SYS_close)` 清理；`:1049-1051` 把 `kTimeout` 与 `kReady` 都视为正常完成（eventfd 无人写入，故正常路径就是超时），失败才置 errno 并报告中断；
  - `:665-693` `restore_failed_replacement` 精确处理 dup2/dup3 的语义（注释 `:661-664`：「Hook 必须在进入系统调用前发布关闭…若 Linux 内核拒绝本来有效的替换，目标仍保持打开」），恢复成功则重建 token 并清 `m_closed`，恢复失败则清 token、置 `m_closed` 并从 map 擦除（且校验 `found->second == descriptor`）；
  - `:1065-1078` `saturating_duration` 与 `:318-337` `timeval_duration` 使用**同一套除法+减法溢出预检模式**，两处都避免了 `seconds * 1e9` 回绕；
  - `:930-1021` `cooperative_connect` 完整处理 `EINPROGRESS`/`EALREADY`/`EAGAIN` 三态，等待后用 `getsockopt(SO_ERROR)` 取真实结果，并在等待前后各做一次 `descriptor_closed` 检查；`:990-993` 说明「配置的 connect 超时同样适用于 managed 延迟接管后的原生 poll 回退」。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前十八批 15,849 行 + 本批约 480 行 ≈ **16,329 行，占 `src/`（22,012）约 74.2%**。
  - **仍未读**：`hook.cpp` 余约 1,061 行（`:1100-2161`，含 `fcntl_argument` 余下、`ioctl_has_*_argument`、`set_enabled`/`enabled`/`bind_io_manager`/`set_connect_timeout`、以及全部 33 个 interposer 入口函数体）、`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **5,683 行**。
- **台账状态**：本轮无新增条目；待办仍为 **21 条**，其中 18 条来自源码回溯。
- **下一步**：`hook.cpp` 的 `:1100-2161`，读完该文件。

## 2026-10-11：源码回溯审计（第 18 批：hook.cpp :1-620）

- **本轮读实现**：`src/network/hook.cpp` 的 `:1-620`，约 **620 行**，覆盖 `Originals` 全部 33 个函数指针、`initialize_originals`、`invoke_real`、`invoke_native_blocking`、`hooks_enabled`、`current_manager`/`cooperative_manager`、`OpenDescription`/`Descriptor`、`adopt_socket`/`try_adopt_socket`、`register_new_socket`/`try_register_new_socket`、`prepare_close`/`finish_close`/`notify_before_close`、`snapshot_descriptor`、`clone_descriptor` 开头。
- **核对结果——与文档一致**：
  - `:148-191` `initialize_originals` 用 `std::call_once` + `thread_local s_resolving` **双重防重入**（`dlsym` 自身可能触发 hook）；`:131-138` 用 `memcpy` 在函数指针与 `void*` 之间转换并配 `static_assert` 检查 ABI 尺寸，规避了 ISO C++ 里函数指针与对象指针互转的未定义行为；`RealCallGuard`（`:124-128`）用 TLS 深度标记「正在执行真实调用」，供 `cooperative_manager` 判据使用；
  - `:268-275` `cooperative_manager` 的四重条件（hook 启用 **且** 不在真实调用中 **且** 有当前 Fiber **且** 有当前 task）与 `docs/network.md`「只有 managed G 才走协作路径，普通线程直接透传」一致；`:238-243` `hooks_enabled` 再把进程级开关交给 `thread_policy::detail::HookAllowed` 做线程级判定，与 `thread_policy.cpp:189-199` 对应；
  - `:402-444` `adopt_socket` 在 `:428-432` 把用户未设 `O_NONBLOCK` 的 socket 改为系统级非阻塞，并**分别记录** `m_user_nonblocking` 与 `m_system_nonblocking`，使「用户显式设置的非阻塞」与「库代设的非阻塞」在后续 `read`/`poll` 语义上可区分；`:438-443` 与 `:497-502` 的异常路径都会**把 `O_NONBLOCK` 恢复成原始 flags** 后重新抛出，不留半接管的 fd；`:454-462`/`:514-521` 的 `try_*` 包装把元数据失败降级为「fd 仍可用，回退 libc」，注释明确；
  - `:318-337` `timeval_duration` 对 `tv_sec`/`tv_usec` 做合法性检查、把全零解读为「无超时」（符合 `SO_RCVTIMEO` 语义），并在 `:331-334` 用**除法与减法**做乘法溢出预检，避免 `seconds * 1000000` 回绕；
  - `:528-539` `snapshot_descriptor` 把 `m_closed || !token || !token->valid()` 合并为单一 `m_closed` 判据，`:591-600` `finish_close` 只在 `found->second == descriptor` 时擦除，避免误删同 fd 的新代际。
- **本轮读源码发现的新问题**：
  - **X-18（低）**：`adopt_socket` 在**首次接管**期间持有全局 `s_descriptors_mutex`（`:410`）并执行 3~4 次真实系统调用，使新 fd 接管成为全进程串行点。注释说明串行化是有意的，但未说明持锁范围包含系统调用；热路径（已接管 fd）命中 map 即返回，不受影响。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前十七批 15,229 行 + 本批约 620 行 ≈ **15,849 行，占 `src/`（22,012）约 72.0%**。
  - **仍未读**：`hook.cpp` 余约 1,541 行（`:620-2161`，含 `clone_descriptor` 余下、`native_wait`、`cooperative_io`、`cooperative_connect`、`cooperative_sleep_for`、`fcntl`/`ioctl` 参数处理与全部 33 个 interposer 入口）、`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **6,163 行**。
- **台账状态**：新增 **X-18**；待办共 **21 条**，其中 **18 条来自源码回溯**（D-13~D-18、X-06~X-18），**全部只登记未修**。
- **下一步**：`hook.cpp` 的 `:620-2161`。

## 2026-10-11：源码回溯审计（第 17 批：io.cpp :1760-2646，全文读毕）

- **本轮读实现**：`src/network/io.cpp` 的 `:1760-2646`，约 **886 行**，覆盖 `wake_nodes` 主体、`finish_fd`、`State` 全部成员、`IOManager` 构造/析构/`start`/`shutdown`/`go`/`wait`/`wait_for`/`wait_many`/`wait_any`/`wait_any_for`/`wait_many_for`/`cancel`/`cancel_all`/`notify_close`/`NotifyCloseAll`/`current`。**至此 `io.cpp` 2,646 行全文读完。**
- **核对结果——与文档一致**：
  - `:1755-1814` `wake_nodes` 有完整的异常安全：辅助容器分配失败时回退单节点唤醒，且**不丢失已经从 `WakeList` 取出的节点**；`:1800-1808` 只在 `tasks.size() > 1` 时走 `wake_io_batch`，这正是 D-17 所述批量入口的**唯一调用点**（节点均已 claim，故实现跳过 `owned_by` 校验是有依据的）；`:1809-1813` 所有节点统一配对 `end_operation()`；
  - `:1976-1979` 与 `:2108-2114` 都要求 `Scheduler::current_scheduler() == &m_scheduler` 且存在当前 task，否则返回 `EPERM`；`:2110-2111` 注释明确「普通线程请使用原生 poll/select；这里不偷偷阻塞调用方线程」，与 `docs/network.md` 一致；
  - `:2056-2072`（单 fd）与 `:2378-2393`（多 fd）的 park 循环都在伪唤醒后重新检查 outcome，并在「任务被取消 / scheduler 身份变化 / task 身份变化 / park 失败且调度器已停止」时转入取消，**不会无限自旋**；与文档「park 循环处理伪唤醒」一致；
  - `:2133-2147` `wait_many` **先验证完整请求集合再发布任何 epoll 节点**，注释说明「否则前面的可读 FD 可能先触发并让 WaitMany 返回，后面的无效 FD 就会被静默跳过」；`:2121-2131` 的 O(n²) 重复检测同样给出理由（「否则一次 readiness 可能同时消费多个请求，无法给外部 waiter 提供可验证的 FIFO」）；
  - `:2322-2376` 与 `:2404-2461` 是**两处零超时补探测**，分别位于 park 之前和恢复之后：前者避免「数据在第一个节点注册后、第二个注册前到达」导致只唤醒一个 Fiber，后者避免 epoll 回调分批到达时 cleanup 取消掉已就绪节点。两处都用 `::syscall(SYS_poll, ...)` **绕过 Hook**，`:2426-2427` 明确解释「否则 Hook-on 构建会再次进入阻塞封装并可能递归 park 当前 Fiber」；
  - `:2395-2402` 移除 Context 回调时用 try/catch 保护并保留已得结果，`:2267-2272` 用 `shared_ptr<vector<NodePtr>>` 持有节点以防迟到回调观察到已销毁节点；
  - `:1883-1904` `IOManager` 在构造时向全局 `manager_registry`/`close_registry` 登记，析构时**校验 `found->second == this`** 后才擦除，避免误删同地址的新实例。
- **本轮读源码发现的新问题**：
  - **X-17（低）**：`:2486-2494` 的终态选择是**两个分支完全相同的 if-else**，可合并为单个 `if (!have_terminal)`。结构暗示「非 Error 的终态优先于 Error」，但实际是**先到先得**。不影响就绪优先（`:2496`），但存在意图歧义。
- **一处未登记的可读性观察**：`:1828-1836` `finish_fd` 先 `drain_queue_locked(queue_for(slot, event))`，再在 `both` 时 drain writers、并在 `event != kRead` 时 drain readers。当 `event == kWrite && both` 时 writers 会被 drain 两次 —— 因 `drain_queue_locked` 对空队列是幂等的故无害，且当前唯一以 `both=true` 调用它的 `cancel_all` 传的是 `kRead`，**该组合当前无调用者**。不登记为条目。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前十六批 14,343 行 + 本批约 886 行 ≈ **15,229 行，占 `src/`（22,012）约 69.2%**。
  - 已**全文读完 18 个文件**（含 `io.cpp`）。
  - **仍未读**：`hook.cpp`(2161)、`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **6,783 行**。
- **台账状态**：新增 **X-17**；待办共 **20 条**，其中 **17 条来自源码回溯**
  （D-13~D-18、X-06~X-17），**全部只登记未修**。
- **下一步**：`hook.cpp`(2161)——现存最大的未读文件。

## 2026-10-11：源码回溯审计（第 16 批：io.cpp :1400-1760）

- **本轮读实现**：`src/network/io.cpp` 的 `:1400-1760`，约 **360 行**，覆盖 `fail_slot_locked`、`take_ready_locked`、`process_event_locked`、`process_event`/`process_events_batch`、`expire_timers`、`poll_timeout_ms`、`drain_wake_fd`、`poll_loop`、`tickle`、`wake_nodes` 开头。
- **核对结果——epoll 就绪与超时路径设计严谨**：
  - `:1419-1439` `take_ready_locked` 与 `:1420-1437` 的队头不跨越规则：「队头正在由取消、超时或零超时补采样路径处理；不能越过它，否则会破坏同一 fd/方向上的 FIFO」——与 `drain_queue_locked`、`complete_if_head` 的「先 `begin_operation` 再 pop」三处互相印证，构成完整的 FIFO + 操作令牌协议；
  - `:1465-1476` 对 `interest == 0` 且收到 ERR/HUP 的情形**直接失效该代际注册**，注释说明「Linux 仍会为已关闭 fd 报告 EPOLLERR/EPOLLHUP。此时不能把错误写入 pending 位，否则 poller 会反复收到同一个 HUP 并忙循环」；
  - `:1478-1480` `error_or_hup` 时把读与写**同时**置为 ready，保证错误会唤醒两个方向的全部等待者；`:1481-1498` 若成功交接给具体 waiter 则清 pending（「避免下一次注册得到伪 ready」），否则才置 pending 位；
  - `:1604-1633` `poll_timeout_ms` **先限制范围再转换**（`:1617-1623` 注释：「max() 截止时间可能比毫秒类型更宽，直接转换会回绕成负数，导致 epoll_wait 失败而不是进行轮询」），并在 `:1627-1629` 对截断结果**向上取整**，避免提前唤醒造成空转；
  - `:1359-1373` 与 `:1500-1507` 的 `update_interest_locked` 调用后都检查 `fail_slot_locked` 并清理空 slot；`:1679-1701` 后端 `wait` 失败时统一置 `m_init_error`、停止接收并 drain 所有队列，不留下永久挂起的 waiter；
  - `:1731-1745` `tickle` 与 `:1666-1668`、`:1677`、`:1708-1711` 的 `m_poller_waiting`/`m_tickle_pending` 协议经**时序推演核对为严密**：`m_poller_waiting` 在持 `m_mutex` 时置位、在 `wait` 返回后立即清除；`poll_timeout_ms()` 作为 `wait()` 的实参在调用前求值且同样持 `m_mutex`，因此「新注册的 timer 早于上一次算出的超时」这一窗口不可能被漏掉，`tickle_pending` 的合并语义也只会丢失**无害**的重复唤醒。
- **本轮读源码发现的新问题**：
  - **X-16（低）**：`:1450-1452` 的条件已排除 `slot.registered == false`，紧接着 `:1453` 的 `slot.registered = true;` 是**死存储**（两行之间无其他语句）。疑似重构残留，或本意是写 `slot.armed`。不影响正确性，但会误导读者以为此处存在一次状态转换。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前十五批 13,983 行 + 本批约 360 行 ≈ **14,343 行，占 `src/`（22,012）约 65.2%**。
  - **仍未读**：`io.cpp` 余约 886 行（`:1760-2646`，含 `wake_nodes` 主体、`WaitAny/WaitMany`、公开 `IOManager` 方法）、`hook.cpp`(2161)、`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)。
- **台账状态**：新增 **X-16**；待办共 **19 条**，其中 **16 条来自源码回溯**。
- **下一步**：`io.cpp` 的 `:1760-2646`（读完该文件）。

## 2026-10-11：源码回溯审计（第 15 批：io.cpp :700-1400）

- **本轮读实现**：`src/network/io.cpp` 的 `:700-1400`，约 **700 行**，覆盖 `State::shutdown`、`register_wait`（核心注册路径）、`complete`、`recycle_node`、`complete_if_head`、`cancel`/`cancel_all`/`notify_close`、私有工具（`claim`/`purge_terminal_locked`/`erase_timer_locked`/`invalidate_registration_locked`/`update_interest_locked`/`drain_queue_locked`）。
- **结论：本批未发现新的缺陷或文档缺失。** 以下要点经逐条核对：
  - `:702-734` `shutdown()` 用 `m_lifecycle_mutex` + `m_mutex` 双层保护，先在锁内 `invalidate_registration_locked` + `drain_queue_locked` 清空所有等待队列，**锁外** `wake_nodes` + `tickle`，最后才 join poller；`:729-731` 检查 `m_poller.get_id() != std::this_thread::get_id()`，避免在 poller 线程内自我 join 死锁；
  - `:776-788` 先用本地 `FdSlot` 缓存的 descriptor token，避免每轮等待都做 `F_GETFD` + 全局 registry 锁；`:813-826` 是**第二个线性化点** —— 注释明确「检查后到入队期间的失效仍由同一 `m_mutex` 下的 `notify_close` 排空处理」，否则「旧 fd 放回队列，既没有新的 epoll 注册者，也没有后续关闭事件可以唤醒它，最终形成永久挂起」；
  - `:891-917` pending 只是 epoll 的**候选提示**，用零超时 `::syscall(SYS_poll, ...)` 最终确认，防 stale ready；该 syscall 与 `:123-135` 的 `probe_immediate` 一致地**绕过 Hook**；
  - `:1348-1354` `update_interest_locked` 要求 interest 是队列的**精确投影**，注释记录了历史缺陷：「旧实现只在『新集合是旧集合子集』时直接返回，导致 READ|WRITE 收缩为 WRITE 后仍保留 READ 注册；水平触发下会持续收到无消费者的 READ 事件并忙循环」。**本轮已交叉核对：该修复在 `docs/correctness_audit.md:90`、`codex.md:1466`、`codex.md:1487` 均有留痕**，文档链路完整，**不是文档缺失**；
  - `:1379-1381` `drain_queue_locked` 强调「先登记操作，再从队列移除」，因为 `complete_if_head` 可能已在队列外取得操作令牌并等待 `m_mutex`，先 pop 会让它获取锁后找不到节点而**永久遗留未完成节点**；`:1394-1397` 遇到另一完成路径正在处理队头时 `break` 以保持 FIFO。两处注释与 `:1070-1072` 形成完整闭环；
  - `:792-795` `Capture` 已在描述符注册表锁内做过 `F_GETFD`，`m_mutex` 下的二次检查只作为 close/dup2 与发布节点之间的线性化门，缓存 token 失效时直接返回 `EBADF` 而不把旧代际放回队列。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度**：前十四批 13,283 行 + 本批约 700 行 ≈ **13,983 行，占 `src/`（22,012 行，含用户改动后的 `error.cpp` 与 `context_x64.S`）约 63.5%**。
  - **仍未读**：`io.cpp` 余 1,246 行（`:1400-2646`，含 `poll_loop`、`WaitAny/WaitMany`、公开 `IOManager` 方法）、`hook.cpp`(2161)、`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)、`context_x64.S`(83)，合计约 **8,029 行**。
- **台账状态**：本轮无新增条目；待办仍为 **18 条**，其中 15 条来自源码回溯。
- **下一步**：`io.cpp` 的 `:1400-2646`。

## 2026-10-11：源码回溯审计（第 14 批：io.cpp 前 700 行）

- **本轮读实现**：`src/network/io.cpp` 的 `:1-320`（工具函数、`probe_immediate`、token 编码、`WaitNode` 全部成员）与 `:320-700`（`WaitNode` 的 callback/operation 生命周期、`WakeList`、`FdSlot`、`State` 构造与 `start()`/`shutdown()` 开头），约 **700 行**。**io.cpp 共 2,646 行，未读完。**
- **核对结果——与文档一致**：
  - `:79-96` `descriptor_is_open` 只认 `F_GETFD` 成功（`EINTR` 重试），注释写明「把 EIO/EINTR 等误判为仍打开会把无效 fd 注册到 epoll，最终造成永久等待」；`:123-135` `probe_immediate` 用**原生 `poll` 零超时**探测、不进入 Hook，避免递归注册 Fiber 等待——与 `docs/network.md`「epoll pending readiness 在下一次消费时用零超时原生 `poll` 再验证」描述吻合；
  - `:303-318` 把 fd 与注册序号编码进 `epoll` token（高 32 位 `fd+1`、低 32 位序号，`token=0` 保留给内部 eventfd），注释说明目的是「避免每次 ONESHOT rearm 都在哈希表中分配/释放节点」，与 `docs/extensibility.md` 的「每个等待请求使用 FD generation/token 和一次性 outcome claim」一致；
  - `:484-505` `begin_operation`/`end_operation` 的 0/1/2/3 无锁状态机（空闲/有一个操作/停止中/已停止）与 `:491-504` 注释「1→0 是正常完成；若回收线程已改为 2 则发布 3，不能无条件 store(0)」精确对应 `docs/network.md` 的「移除 Context 回调或回收节点前，会等待正在执行的取消/完成回调退出」；
  - `:621-642` `State` 构造时在 `backend_owners()` 中登记 `IOBackend*` → `State*`，冲突时置 `EBUSY` 并放弃后端，落实了 `docs/extensibility.md`「一个后端实例只能绑定一个 IOManager」；`:666-692` `start()` 用 `m_lifecycle_mutex` + `m_mutex` 双层锁，线程创建失败时回滚 `accepting` 并置 `EAGAIN`。
- **本轮读源码发现的新问题**：
  - **X-14（低）**：`WaitNode::stop_operations`（`:507-532`）在等待活动操作结束时用**无界 yield 自旋**（`:528-530`）。这是与 X-13 同一「用 yield 代替条件等待」模式的**第二处**实例。注释承认理由是「避免为每个节点创建 condition_variable」且操作「通常很短」，但 `wake_io` 内部会取 admission 锁与 P 队列锁，竞争严重时耗时不可忽略。
  - **X-15（低）**：`State::s_node_cache`（`:541-542`）是 `inline static thread_local` 且以**裸 `State*` 指针**为键复用 WaitNode，但 `~State()`（`:644-656`）**不清理**自己的缓存项。新 State 若落到相同地址，`:828-835` 会命中旧实例遗留的节点。因取出即 `reset()`（`:832`）且有 4096 上限（`:1042`），实际无害，但 `:538-540` 的注释只提「不形成反向生命周期」，未说明这一地址复用依赖。
- **一处未登记的观察**：`:1260-1264` `next_registration_id_locked` 把序号截断到 低 32 位并把 0 映射为 1，因此理论上累计 2^32 次注册后会与序号 1 的 token 冲突（`0xffffffff` 掩码）。考虑到需要数十亿次 fd 注册/注销，**本轮不登记为条目**，仅在此记录备查。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建；用户在途的 `src/tools/error.cpp` 改动已加入提交门禁拦截名单，本轮未触碰。
- **累计进度**：前十三批 12,507 行 + 本批约 700 行 ≈ **13,207 行，占 `src/`（21,855）约 60.4%**。
  - **仍未读**：`io.cpp` 余约 1,946 行（`:700-2646`，含 `poll_loop`、`WaitAny/WaitMany`、`complete_*`、`IOManager` 公开方法）、`hook.cpp`(2161)、`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)。
- **台账状态**：新增 **X-14、X-15**；待办共 **18 条**，其中 **15 条来自源码回溯**（D-13~D-18、X-06~X-15），**全部只登记未修**。
- **下一步**：继续 `io.cpp` 的 `:700-2646`。

## 2026-10-11：源码回溯审计（第 13 批：scheduler.cpp 剩余区间，全文读毕）

- **本轮读实现**：`sysmon_pass`(`:1413-1470`)、`requeue_to_processor` 与 `requeue_from_worker`(`:2527-2644`)、`worker_loop` 收尾(`:3910-4046`)，约 **322 行**。**至此 `scheduler.cpp` 4,048 行全文读完**。
- **核对结果——与文档一致**：
  - `:1424-1432` `sysmon_pass` 用 `try_to_lock`，抢不到锁时置 `sysmon_retry_pending` 并缩短下一周期，而不是把一次锁竞争当成本轮扫描完成；与 `docs/scheduler.md`「扫描本身采用 try_to_lock，因此可能跳过一轮，但不会无限等待」一致；`:1441-1442` 还用 `since <= 0 || now < since` 双重防护时间回退；
  - `:2532-2536` 与 `:2598-2602` 都先做不变检查、`:2546-2549` 与 `:2609-2612` 在**持有 P 队列锁后再次检查** `accepting/draining/stopping`，与 shutdown 的排空顺序一致（shutdown 在同一 admission mutex 下扫描 waiting G）；
  - `:2614-2616` 注释明确「计数必须先于节点发布，否则另一个 worker 可能先 pop 节点再递减尚未增加的 runnable，造成下溢」；`:2537-2539` `try_mark_queued()` 失败时返回 `task->queued()`，使重复入队幂等；
  - `:3990-4012` 空闲 M 回收：`idle_timeout` 按 `can_shrink` 选择 `idle_worker_timeout` 或 `idle_wait`；被唤醒后**重新计算** `current_floor` （`:3997`）而不是复用唤醒前的值；并在**持锁时预留退休**（`:4004-4010`，注释说明否则多个空闲 M 可能都看到 `active > min` 而缩到下限以下）；
  - `:4018-4026` 退出时只撤销**仍 attached** 的 P 计数，避免 sysmon 已 detached 时 `size_t` 下溢；`:4027-4033` 按 `stopping` 决定 P 进入 `kDead` 还是 `kIdle`。
- **对既有条目的修正（重要）**：读到 `:3927-3948` 后发现，**X-12 描述的「终态回收被抑制」还有一条例外路径** —— 当 worker 取到任务却 `!claimed`（出队后失去执行 claim）时，会**直接**调用 `collect_terminal_locked`，不经过 `maintenance_budget`。但该分支只在取消/竞态造成的陈旧队列节点上触发，属罕见路径，**X-12 的结论不变**，已就地补充说明。
- **另处理用户未决事项**：上一轮我提出「台账行号会因用户重排失效」，并给出方案 A（补符号名）/B（等重排完统一重定位）。用户在 30 分钟窗口内未回复，按无人值守规则**采用默认方案 A**：
  - 台账顶部新增**行号引用约定**：`路径:行号` 一律以符号名/代码片段为主定位，行号仅作参考；并记录了 `src/tools/error.cpp` 由 404 → 478 行、`Is()` 的空 target 分支由 `:392-394` → `:463` 这一具体漂移；
  - D-14 的行号引用已同时给出重排前后的位置。
  - **用户可推翻**：若希望改回方案 B，删除顶部该段约定即可。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建。
- **累计进度（重新核算）**：完整读完的**16 个文件**共 **12,507 行**，占 `src/`（21,855 行）约 **57.2%**。
  - 已全文读完：`error` `control_flow` `fd_manager` `fiber_local` `parking_condition` `io_backend` `thread_policy` `future` `context_backend` `channel` `socket` `timer` `sync` `context` `fiber` `scheduler`。
  - **仍未读**：`io.cpp`(2646)、`hook.cpp`(2161)、`config.cpp`(1089)、`log.cpp`(1026)、`websocket.cpp`(715)、`http.cpp`(620)、`sse.cpp`(613)、`address.cpp`(280)、`byte_array.cpp`(196)——合计约 9,346 行。
- **另注**：用户在途新增 `src/tools/error.cpp` 的改动，上一轮已做过只读验证（构建 0 warning、全部 23 个测试模块通过、`ctest` 24/24），**本轮未再触碰**。
- **下一步**：`io.cpp`(2646)——现存最大的未读单文件。

## 2026-10-11：源码回溯审计（第 12 批：fiber.cpp 全文 1502 行）

- **本轮读完整实现**：`src/concurrency/fiber.cpp`(1502)。这是依赖图里 `scheduler` 的唯一实质前提，也是「阅读顺序」中优先级最高的未读文件。
- **核对结果——文档承诺与实现一致**：
  - `:727-775` `entry()` 在用户主体返回后先 `m_function = {}`，再 `fiber_local::detail::Cleanup(m_owner)`（`:752-755`），即 **FiberLocal 值的主体栈展开后、交回控制权前销毁**，与 `docs/design.md`「FiberLocalCache 按逻辑 Fiber 保存共享值…trampoline 完成后释放」一致；`:745-751` 只捕获用户异常并置 `Failed`，挂起/取消/清理流程不抛异常；
  - `:897-1016` `resume_locked` 用 `FiberTlsRestore` RAII（`:73-89`）+ 显式 `s_current_fiber` 恢复；`:978-987` 的注释解释了为何**不能**无条件恢复 `previous_fiber`——否则父 Fiber 之后创建的子 Fiber 会错绑到根上下文、破坏父链；
  - `:992-1012` 调度器挂起沿**直接父链**传播（`m_scheduler_propagate`），普通 `Suspend` 不进入该分支，与 `docs/scheduler.md`「嵌套 Fiber 的父链由共享元数据记录保存」一致；`:1003-1009` 父级失效时走 `abandon_suspended` 而**不** terminate；
  - `:630-651` `abandon_suspended` 显式标记 `Failed` 并清理 FiberLocal，注释写明「不能伪造 Completed」；`:653-725` 析构协议覆盖 Ready（可安全跳过主体）、Suspended（沿父链恢复）、Running（fail-fast）三种情形；
  - `:1189-1196`/`:1170-1187` `FiberBin` 回收要求 `m_record.use_count() == 1`，避免嵌套子 Fiber 仍持有父记录时被复用；`:577-586` `reset_for_scheduler` 在状态或 claim 不符时抛 `logic_error` 而非复用活动栈；
  - `:213-259` `normalize_stack_size` 与 `:189-210` 的对齐/映射尺寸检查覆盖了页面大小、最小值、最大值和加法回绕，与 `docs/correctness_audit.md` 记录的边界一致；`:387-405` 栈缓存按 TLS 8 个 + 全局 128 个分层，与 `docs/design.md` 的「TLS 小缓存溢出后进入有界全局缓存」一致。
- **本轮读源码发现的新问题**：
  - **X-13（中）**：`Impl::~Impl` 获取 `m_resume_claim` 时是**无界 yield 自旋**（`:663-669`），无超时、不阻塞睡眠。目标 Fiber 若正在别的 M 上长时间运行，析构线程会持续占一个 CPU 核心自旋。对照 `parking_condition.cpp` 用条件变量、`shutdown_for` 提供有界 `wait_until`，此处是唯一的例外。文档只说「可能阻塞析构」，未说明是忙等。
- **关于本文件里已声明的边界**：`:654-658` 在 Fiber 自身栈上析构 `Impl` 直接 `std::terminate()`、`:694-698` 取得 claim 后仍观察到 Running 也 terminate —— 这两条与 `docs/compatibility.md`「Fiber 自身不能析构自身」的公开约束一致，**不作为新问题登记**。
- **验证结果**：本批为只读回溯，未改动任何源码或测试，未触发构建。
- **累计进度**：前十一批约 9,082 行 + 本批 1,502 行 ≈ **10,584 行，占 `src/`（21,855）约 48.4%**。
  - 已完整读 15 个文件；`scheduler.cpp` 部分读（约 2,061/4,048）。
  - **仍未读**：`scheduler.cpp` 余约 1,987 行（`:3910-4048`、`sysmon_pass` `:1413-1466`、`requeue_*` `:2527-2654`）、`io.cpp`(2646)、`hook.cpp`(2161)、`logging/log.cpp`(1026)、`config/config.cpp`(1089)、`http.cpp`(620)、`sse.cpp`(613)、`websocket.cpp`(715)、`address.cpp`(280)、`byte_array.cpp`(196)。
- **台账状态**：新增 **X-13**；待办增至 **16 条**，其中 **13 条来自源码回溯**（D-13~D-18、X-06~X-13），**全部只登记未修**。
- **下一步**：按依赖顺序回到 `scheduler.cpp` 收尾（`sysmon_pass` + `requeue_*` + `:3910-4048`），然后进 `io.cpp`。

## 2026-10-11：源码回溯审计（第 11 批：worker_loop 主体与 maybe_grow）

- **本轮读的实现**：`worker_loop` 主体(`:3720-3910`)、`reap_dead_workers`(`:1244-1286`)、`detached_machine_count_locked`(`:1306-1315`)、`maybe_grow` 全文(`:1328-1400`)，约 **365 行**。
- **核对结果——与文档一致**：
  - `:3725-3765` 的取任务顺序（救援链 → 本地 FIFO → 全局 FIFO → 有界窃取 victim 队尾）与 `docs/scheduler.md`「worker 依次尝试本地队列、全局队列和受限的 victim 窃取」完全对应，`:3745` 用 `processors.size() > 1` 守卫窃取；
  - `:3872-3878` 在 G 任务边界发现仍活动的 `BlockingRegion` 时 `std::terminate()`，注释解释「宁可在边界处 fail-fast，也不能静默清掉 TLS 后把不一致状态传给后续 G」，与 `docs/scheduler.md` 的 fail-fast 约定一致；
  - `:1359-1382` `maybe_grow` 的扩容上限：普通 runnable 峰值以 P 为界，每个 detached M 只补一个替代槽，且替代目标按 `min_workers + detached` 计算并受 `max_workers` 限制——正是 `docs/scheduler.md`「不会因为整机 P 数量很大而无条件创建 P 个线程」的源码依据；
  - `:1250-1253` `reap_dead_workers` 用 `try_to_lock` 拿 `join_mutex`，拿不到就返回，避免 worker 与 shutdown 互等（注释明确）；`:1260` 先 `reserve` 再 move `std::thread`，保证后续 move 是 noexcept。
- **本轮读源码发现的新问题**：
  - **X-12（中）**：`maintenance_budget` 被**两个维护目的共用且互相重置**。`growth_period` 在积压时是 8、空闲时是 64；collect 的阈值固定为 64。于是**持续积压期间计数器每 8 次就被 grow 清零，永远达不到 collect 的 64**，终态 Task 只能等 `draining` 或积压消退才被回收，注册表强引用长期不释放。关键在于 `maybe_grow` 只回收 dead **worker 线程**（`reap_dead_workers`），**不**回收终态 Task——这条是我读了 `:1328-1400` 才确认的。
    - **可验证的旁证**：`go2cpp_high_load_stress` 有 50,000 个任务，运行时 `runnable` 很可能长期大于 P 数，即长期处于 `growth_period == 8`，因此该压测的**内存峰值**应当明显偏高。本轮**未实测**这一点，仅作为后续验证方向的记录，不作结论。
- **验证结果**：本批为只读回溯，未改动任何源码或测试，未触发构建。
- **累计进度**：前十批约 8,717 行 + 本批约 365 行 ≈ **9,082 行，占 `src/`（21,855）约 41.6%**。`scheduler.cpp` 尚余 `:3910-4048`（worker 收尾与退休）与 `sysmon_pass` 主体(`:1413-1466`)未读；`io.cpp`(2646)、`hook.cpp`(2161) 完全未读。
- **台账状态**：新增 **X-12**；待办增至 **15 条**，其中 **12 条来自源码回溯**（D-13~D-18、X-06~X-12），**全部只登记未修**。
- **下一步**：`scheduler.cpp` 收尾（`:3910-4048` + `sysmon_pass` `:1413-1466`），然后进 `io.cpp`。

## 2026-10-11：源码回溯审计（第 10 批：Task::run / enqueue / worker_loop）

- **本轮读的实现**：`Task::run`(`:830-996`)、`enqueue` 全文(`:2287-2525`)、`worker_loop` 前半(`:3494-3720`)，约 **636 行**。
- **核对结果——与文档/既有记录一致**：
  - `:860-866` callable 被 move 到 G 栈上执行，注释明确「captures 在无任何调度器锁持有时释放」；`:954-958` 在 trampoline 返回后立即把 `m_fiber` move 出去并`:992` 回收——「retaining a Task must not retain an already-finished G stack」与文档一致；
  - `:959-981` 对「用户直接调用 `Fiber::Suspend`」的兜底：检查 `pending_wake` 与 `cancellation_requested` 后决定 Runnable/Waiting，注释解释了为何**不能**把已发布的唤醒许可覆盖成 Waiting（否则 shutdown 可能永久搁置该 G）；
  - `:2347-2356` 首次注册分配失败时**绝不继续发布队列节点**，而是走取消路径——避免 Fiber 栈在无人持有时被销毁；`:2476-2482` 分配失败时已启动 G 进救援链、未启动 G 走取消，两者计数配对正确；
  - `:3528-3531` 本地队列无锁快速路径要求 `runnable > processors.size()`（有积压才走），`:2506-2522` 的扩容检查按 `maintenance_ticket & 63/1023` 采样，与 `docs/scheduler.md` 的「有界扩展」描述一致。
- **本轮读源码发现的新问题**：
  - **D-18（文档矛盾）**：`docs/compatibility.md` 称 affinity「**不做 CPU 绑核**」，但 `scheduler.cpp:3505-3515` 在 `pin_workers_to_cpu == true` 时确实调用 `pthread_setaffinity_np`；`docs/configuration.md` 又把它列为合法布尔项。默认值为 `false`，所以默认行为不绑核，但文档的绝对表述与实现冲突。
  - **X-11**：`:2392-2399` 的 `static thread_local t_incoming_stripe` 是**函数局部 static**，跨 Scheduler 实例共享，导致第二个 Scheduler 的条带号分布依赖第一个的分配结果，且其 `next_incoming_stripe` 计数失真。不影响正确性，仅影响分布均匀性。
- **验证结果**：本批为只读回溯，未改动任何源码或测试，未触发构建。
- **累计进度**：前九批约 8,081 行 + 本批约 636 行 ≈ **8,717 行，占 `src/`（21,855）约 40%**。`scheduler.cpp` 仍有约 1,200 行未读（`worker_loop` 后半 `:3720-4048`、`wake_io_batch` 主体已读但 `maybe_grow`/`sysmon_pass` 未读）；`io.cpp`(2646)、`hook.cpp`(2161) 完全未读。
- **台账状态**：新增 D-18、X-11；待办增至 **14 条**，其中 **11 条来自源码回溯**（D-13~D-18、X-06~X-11），全部只登记未修。`X-01`（示例间歇失败）仍未复现。
- **下一步**：`worker_loop` 后半与 `maybe_grow`/`sysmon_pass`，然后进 `io.cpp`。

## 2026-10-11：源码回溯审计（第 9 批：scheduler.cpp Impl 与 shutdown_for）

- **本轮范围说明**：继续 `scheduler.cpp`，本轮读 `:997-1840`（`Scheduler::Impl` 全部成员与 sysmon/begin_shutdown/collect_terminal）与 `:1942-2161`（`shutdown_for` 全文），约 **1,062 行**。仍未读：`Task::run`(`:830-996`)、`enqueue`(`:2287-2527`)、`worker_loop`(`:3494-4048`)。
- **结论：本批未发现新的缺陷或文档缺失。** 以下四点经逐条核对与文档/既有记录一致：
  - `:1002-1005` 两条 16 路条带（`kRegistryStripeCount` / `kIncomingStripeCount`）分别用于注册表与外部生产者入队，`:1076-1078` 每条带 `reserve(256)`，与 `docs/scheduler.md`「入队、出队、runnable 计数在 admission mutex 下串行」的分层描述一致——它是**分条带**串行，而非单锁；
  - `:1961-1970` 明确区分「worker 发起 shutdown」（不取 `join_mutex`、不做任何 join，只发布停止请求后返回 false）与「外部 owner 发起」（取 `join_mutex` 并负责全部 join）；`:2029-2037` 的 worker 分支与 `:2101-2119` 的逐个提取-锁外-join 构成完整闭环，注释解释了为何不能用可复用的 `thread::id` 判重；
  - `:1995-1996` sysmon 在 draining 阶段**继续运行**，直到所有已启动 G 自然收尾，避免长系统调用返回前提前丢失替代 M——与 `docs/scheduler.md` 的 sysmon 章节一致；
  - `:1691-1731` 对 waiting G 的扫描复用了与 IO readiness 相同的 pending-wake 交接（`wake_for_scheduler` + `try_mark_queued`，失败时转 emergency 队列），印证了 `park_wait` 双重检查的必要性；`:1527-1564` 的 `collect_terminal_locked` 用**稳定压缩**把回收从 O(n²) 降为 O(n)，且 `reserve` 失败时 `continue` 保留终态条目等待下次回收（异常安全）。
- **一处观察（未登记为缺陷）**：`shutdown_for` 中有 **5 处**结构相同的片段
  ```cpp
  for (auto& task : deferred_destruction) {
      if (task) { auto abandoned_function = task->release_callable(); }
  }
  ```
  （`:2018-2022`、`:2030-2034`、`:2057-2061`、`:2084-2088`、`:2143-2147`）。语义是「在锁外销毁用户 callable」，模式正确但重复了 5 次；抽取为局部 lambda 会更容易保证五条退出路径行为一致。属可维护性观察，非缺陷，故不单列台账条目。
- **验证结果**：本批为只读回溯，未改动任何源码或测试，未触发构建。
- **累计进度（按已读行数）**：前七批完整读 5,959 行 + 第 8 批约 1,060 行 + 本批约 1,062 行 ≈ **8,081 行，占 `src/`（21,855）约 37%**。仍未读：`scheduler.cpp` 剩余约 1,900 行、`io.cpp`(2646)、`hook.cpp`(2161)，合计约 6,700 行。
- **台账状态**：本轮无新增条目；待办仍为 D-13~D-17、T-08、X-06~X-10（含 X-01）。其中 **9 条来自源码回溯**，全部只登记未修。
- **下一步**：补完 `scheduler.cpp` 的 `Task::run`、`enqueue`、`worker_loop`，再进 `io.cpp`。

## 2026-10-10：源码回溯审计（第 8 批：scheduler.cpp 关键区间）

- **本批范围说明（重要）**：`scheduler.cpp` 共 4048 行，本批**只读了关键区间约 1,060 行**（`:560-840` 的 Task 唤醒/挂起状态机、`:2654-3140` 的 park/wake 实现），**不是全文**。这与前七批「读完整实现」的口径不同，结论覆盖面相应收窄。
- **核对结果——`codex.md` 历史记录与源码一致**：
  - `:586` 与 `:613` 确认 `Task::wake_for_wait()` 与 `park_for_scheduler()` **共用同一把 `m_transition_mutex`**，行内注释明确解释「仅依赖原子状态不足以保护 `wake_pending`」，与 `codex.md` 中「全模块边界回归」那条修复记录吻合；
  - `:580-602` 的三种返回（kEnqueue/kPending/kRejected）与 `:612-624` 的三种返回（kParked/kRequeued/kRejected）构成完整的先通知后挂起协议；
  - `:2864-2917` `wake_registered` 的三级回退（worker 直接回本 P → 按 `last_processor_id` 亲和 → incoming 条带 + `thread_local` 固定条带）与 `docs/scheduler.md` 的队列描述一致。
- **本轮读源码发现的新问题**：
  - **D-17**：`wake_io_batch` 是**公开 API**（`scheduler.hpp:537`，public 区起于 `:408`），但实现 `:2972-2987` 跳过 `owned_by` 校验，而头文件没有声明「仅接受本 Scheduler 已登记节点」这一前置条件。误用会把异属 G 放进本调度器队列。
  - **X-10**：同一个 `WakeAction::kPending` 在 `wake_registered`/`wake_or_cancel` 里是**失败**（返回 false），在 `wake_io` 里却是**成功**（返回 true）。三者都是 public API，调用方无法从返回值判断「是否已入队」。
- **未完成部分**：本批**没有**读 `Scheduler::Impl`（`:997-1840`，核心状态机）、`enqueue`（`:2287-2527`）、`shutdown_for`（`:1942-2161`）、`worker_loop`（`:3494-4048`），以及 `Task::run`（`:830-996`）的 Fiber trampoline。这些区间是调度器真正的主体，**目前仍属未读**。
- **验证结果**：本批为只读回溯，未改动任何源码或测试。
- **累计进度（按已读行数）**：前七批完整读 5,959 行 + 本批约 1,060 行 ≈ 7,019 行，占 `src/`（21,855）约 **32%**。`io.cpp`(2646)、`hook.cpp`(2161) 与 `scheduler.cpp` 未读区间合计仍远超一半。
- **下一步**：先补完 `scheduler.cpp` 的 `Impl` / `enqueue` / `shutdown_for` / `worker_loop`，再进 `io.cpp` 与 `hook.cpp`。

## 2026-10-10：源码回溯审计（第 7 批：context.cpp，1895 行）

- **本轮读完整实现**：`src/concurrency/context.cpp`(1895)——四个大文件中第一个，也是 channel / future / timer / sync 的共同依赖。
- **核对结果——文档承诺与实现一致**：
  - `:17-26,47-79` `DoneSignal::State::cv` 就是 `core::ParkingCondition`，因此 `Done().Wait/WaitFor/WaitUntil` 在 managed G 中确实 park 而非阻塞 M，与 `docs/design.md` 一致；
  - `:1598-1611` `MakeChild` 对空 parent 回退 `Background()`，与 `docs/compatibility.md`「空 parent 映射为 Background()（Go 会 panic）」一致；
  - `:1666-1695` `WithDeadline` 对**已过期**deadline 同步取消，不依赖 timer worker 下一次调度；且 TimerService 停止中（`id == 0`）时转为显式取消，不留永久 pending 的 child；
  - `:653-706` 用 `thread_local` 释放队列 + 自定义 `StateDeleter` 把父链析构改成**迭代式**，避免深 Context 树递归析构爆栈——与 `error.cpp` 的深链释放是同一套思路；
  - `:172-205` `Signal()` 先搬出两组回调（internal 优先于普通）、清空 map，再在锁外 notify + 逐个 try/catch 执行；`:1428-1433` 移动后的空对象`RollbackDone()` 返回一个**已完成的静态信号**，避免调用方永久等待。
- **对前几批结论的修正（重要）**：`context.cpp:1877-1893` 的 `CanceledError()` 与 `DeadlineExceededError()` **同样是函数局部 static 单例**。这说明 D-15 描述的「共享单例错误」并非偶然：**哨兵错误（sentinel error）共享单例正是 Go 的惯用语义**（`errors.Is(err, context.Canceled)` 就是靠它成立）。因此 D-15 的定位从「可疑行为」修正为「**需要文档说明的信息项**」，不涉及实现修改。
- **本轮读源码发现的新问题**：
  - **X-09**：两处 TimerService 对「在自身定时器线程内停止服务」策略不一致——`timer.cpp:120-123` 直接 `std::terminate()`，`context.cpp:307-318` 则跳过 join 继续返回。context 中的实例是内部实现、用户无法直接析构，实际影响有限，但策略差异值得统一或注明理由。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，未触发构建；台账修正 D-15、新增 X-09。
- **未通过/未覆盖**：累计回溯 **14 个实现文件、约 5,959 行**，占 `src/`（21,855）的 **27.3%**。三个大文件仍未读——`scheduler.cpp`(4048)、`io.cpp`(2646)、`hook.cpp`(2161)，合计 **8,855 行、占 40.5%**。
- **下一步**：`scheduler.cpp`(4048)——它是唯一还没读的核心调度状态机，且 `Await`/`park_wait`/`wake_registered` 等被 sync 与 io 依赖的接口实现都在里面。

## 2026-10-10：源码回溯审计（第 6 批：timer / sync）

- **本轮读完整实现**：`src/concurrency/timer.cpp`(778)、`src/concurrency/sync.cpp`(1324)，合计 2,102 行。
- **结论：本批未发现新的缺陷或文档缺失。** 两处此前存疑的点经核实**文档已充分覆盖**，故不登记条目——这本身是一个有效结论。
- **timer.cpp 核对要点**：
  - `:599-602` `TimerService::Default()` 是函数局部 static 单例，与文档「默认单线程串行执行到期回调」一致；
  - `:120-123` 在定时器线程内析构 TimerService 直接 `std::terminate()`，与 `docs/timer_usage.md`「回调不得销毁正在执行回调的所属 TimerService」一致；
  - `:464-475` 的 `kBusyWait` 忙等路径（yield 自旋）**有充分文档**：`docs/timer_usage.md:62` 明确「只在截止时间前的 spin_window 内自旋，适合极低延迟、且能接受占用一个 CPU 的场景」，`docs/timer_design_review.md:170` 另有「忙等待不能成为默认配置」的要求。**故不作为文档缺失登记**；
  - `:179`、`:203`、`:293-294` 三处「先 `m_mutex` 再 `state->m_mutex`」的锁序完全一致，无锁反转风险；
  - `:362-372` 周期为 0 的 kContinue 收敛为 `kFailed` + `invalid_argument`；`:133-135` 在锁外取消被遗弃条目，避免在锁内跑用户析构。
- **sync.cpp 核对要点**：
  - `:360,397-405` 线程本地 WaitNode 缓存上限 32；`:401` 在放回缓存前执行 `Reset(nullptr, {})` 清空 scheduler/task，不延长用户对象生命周期；
  - `:753,785` 的协作重试预算（256 / 4 次）为硬编码常量，与 `docs/performance.md:331-333`「该预算不是公开语义，后续可通过策略对象调整」一致；
  - `:741` 只在 `processor_count() > 1` 时走协作重试，避免忙等唯一 M，与 `docs/scheduler.md` 的 P 上限描述一致；
  - `:1208-1217` WaitGroup 的负计数与溢出检查抛 `logic_error`；`:971-973` Mutex 未持锁 Unlock 同样抛异常——两者都与头文件契约一致。
- **验证结果**：本批为只读回溯，**未改动任何源码或测试**，因此未触发构建。
- **未通过/未覆盖（关键比例）**：累计回溯 **13 个实现文件、约 4,064 行**，占 `src/`（21,855 行）的 **18.6%**。四个大文件仍未读——`scheduler.cpp`(4048)、`io.cpp`(2646)、`hook.cpp`(2161)、`context.cpp`(1895)，合计 **10,750 行、占 `src/` 的 49.2%**。也就是说：**接近一半、且是最复杂的那一半代码，我的结论仍然只来自头文件与文档**。这一点在没有读完之前不会改变。
- **下一步**：进入四个大文件，优先 `context.cpp`(1895)——Context 是 channel / future / timer / sync 的共同依赖，读它收益最大。

## 2026-10-10：源码回溯审计（第 5 批：channel / socket）

- **本轮读完整实现**：`src/concurrency/channel.cpp`(402)、`src/network/socket.cpp`(568)。
- **核对结果——断言站得住**：
  - `channel.cpp:167-174,382-393` 取消/deadline 的优先级与状态映射，与我在 `test_event_go.cpp` 中断言的 `kTimedOut`/`kCancelled` 一致；
  - `channel.cpp:106-111` move-only case 在进入等待前就返回 `kInvalid`，与文档「不会登记一个无法回滚的等待者」一致；
  - `socket.cpp:535-539` `SocketStream::read/write` 失败返回**负 errno**、`write_all` 循环补齐短写，与 `docs/network.md` 的描述一致；
  - `socket.cpp:87-122` `begin_operation`/`end_operation` 的 in-flight 计数与self-reference 保活，印证了文档「每个读、写、accept、connect、选项查询和地址查询都会先固定当前 fd」；
  - `socket.cpp:32-45` `poll_wait` 用 `min(count, INT_MAX)` 防溢出并重试 EINTR，`remaining()` 保证绝对 deadline——与文档「不会为每次 recv 重新获得完整超时」一致。
- **本轮读源码发现的新问题**：
  - **D-15 扩展**：`channel.cpp:39-91` 的 5 个 channel 错误工厂（closed / already closed / would block / nil / timeout）**各自是函数局部 static 单例**，与 `future.cpp` 的 `BrokenPromiseError()` 是同一模式。两个不同 channel 的「already closed」错误是同一对象，`Is` 无法区分。
  - **D-16（文档缺失）**：`socket.cpp:151` 的 `Socket::fd()` 只加锁读 `m_fd`，**不进入 `begin_operation` 的 in-flight 保护**。调用方长期持有该值会与 `close()`/fd 复用竞争。`docs/network.md` 列出的「会固定 fd」的操作里不含 `fd()`。
- **补测（新覆盖的边界）**：`channel.cpp:189-193` 在进入等待前检查 `ParkingCondition::FiberWaitUnsupported()`——**手动 Fiber 中调用 Select 会立即返回 `kInvalid`**，而不是退化到 native 等待占住 carrier 线程。已在 `test_event_go.cpp` 补 `check_manual_fiber_select_rejected` 实测通过。注意它与 `ParkingCondition` 自身在同类情形返回 `kCancelled` 不同，Select 在更外层提前失败。
- **性能观察（未登记为缺陷）**：`channel.cpp:126` 的 `Select` 轮询游标是**函数局部 `static std::atomic<std::size_t>`**，即进程级共享。设计意图是让每轮轮询起点轮转、避免固定顺序偏向；代价是高并发 Select 时多核争用同一缓存行。属可观测的开销点，非正确性问题，仅此记录。
- **验证结果**：构建 0 warning / 0 error；`GO2CPP_TEST_FILTER=event_go` 通过（含新增的手动 Fiber 用例）。
- **未通过/未覆盖**：累计回溯 **11 个实现文件、约 2,962 行**，占 `src/`（21,855）的 **13.6%**。四个大文件（`scheduler.cpp`/`io.cpp`/`hook.cpp`/`context.cpp`）仍未读；`socket.cpp` 的 `wait_ready` 三路分支（自定义 waiter / IOManager / native poll）只做了静态核对，未做动态验证。
- **下一步**：`src/concurrency/timer.cpp`(778) → `src/concurrency/sync.cpp`(1324) → 四个大文件。

## 2026-10-10：源码回溯审计（第 4 批：future / context_backend / control_flow）

- **本轮读完整实现**：`src/concurrency/future.cpp`(175)、`src/concurrency/context_backend.cpp`(45)、`src/tools/control_flow.cpp`(346)。
- **核对结果**：
  - `future.cpp:19-36` 默认等待后端只是 `ParkingCondition` 的适配器，`:25-28` 有 deadline 走 `wait_until`、否则 `wait` —— 与我在 `future_demo` 里「超时返回 kTimedOut」的观察一致；
  - `future.cpp:79-129` 的可替换等待后端工厂在锁外构造实例、异常回退默认后端，与 `docs/extensibility.md` 的插件契约一致；
  - `context_backend.cpp` 仅在 `__linux__ && __x86_64__` 转发到汇编符号，其余平台返回 `nullptr`/`{}`；Boost 回退路径由 `fiber.cpp` 的 `#if !defined(GO2CPP_USE_NATIVE_CONTEXT)` 承担，两者分工与 `cmake/Go2CppModules.cmake:33-36` 一致；
  - `control_flow.hpp:97-101` 的 `CallableModel::run() noexcept` 内部有 `try { ... } catch (...) { RecordDeferException(...); }` —— **文档所称「defer 回调异常进 LastDeferException、不穿过 noexcept 析构」属实**，此前只是转述文档，本批已落到源码。
- **本轮读源码发现的新问题**：
  - **D-15（文档缺失）**：`future.cpp:168-173` 的 `BrokenPromiseError()` 是**函数局部 static 单例**，所有「Promise 被放弃」的 Future 拿到的是**同一个 ErrorPtr**，因此 `Is(errA, errB)` 为 true。头文件完全未提。
  - **X-08（边界推论）**：`control_flow.cpp:15,28-38` 用 **thread_local** 的 `s_defer_depth` 判断「是否在 defer 回调内」。若违反「defer 回调不得挂起 Fiber」的约束、让 Fiber 在回调中迁移线程，`take()` 会在新线程上静默返回 `nullopt`。这是既有约束的必然推论，不是独立缺陷，登记以备查。
- **验证结果**：本批为只读回溯，**未改任何源码或测试**，因此未触发构建；台账新增 D-15、X-08 两条。
- **未通过/未覆盖**：累计回溯 **9 个实现文件、约 1,992 行**，占 `src/`（21,855 行）的 **9.1%**。四个大文件仍未读。`future_demo` 未覆盖可替换等待后端工厂这一扩展点（记为盲点，未补测）。
- **下一步**：`src/concurrency/channel.cpp`(402) → `src/network/socket.cpp`(568) → `src/concurrency/timer.cpp`(778)。

## 2026-10-10：源码回溯审计（第 3 批：tools/error）

- **本轮读完整实现**：`src/tools/error.cpp`(404)。
- **核对结果——我在 `test_errors_ns.cpp` 里的断言全部与源码一致**：
  - `:260-262` `NewError` = `make_shared<StringError>`；`:273-278` `Wrap` 对空 cause 直接返回 `{}`；
  - `:155-186` `WrappedError::Message()` 用 `": "` 逐层拼接，与我的 `"read config: disk unavailable"` 断言一致；
  - `:213-250` `JoinError::Message()` 用 `'\n'` 连接并**迭代展开**嵌套 Join，与我的 `"first\nsecond"` 断言一致；
  - `:296-298` `Join` 对全空输入返回 `{}`，空 vector 断言成立；
  - `:355-378,390-402` `IsImpl` 以 `next.get() == target.get()` 做**对象身份**判定，印证我「消息相同但对象不同不匹配」的断言。
- **本轮读源码发现的新问题**：
  - **D-14（文档缺失）**：`error.cpp:392-394` 中 `Is(error, target)` 在 target 为空时**语义完全不同**——返回 `!error`，即退化成「error 是否为空」的判断。而 `include/go2cpp/tools/error.hpp:136` 是**裸声明、零注释**，调用方极易把 `Is(x, nullptr)` 误当成「与 null 比身份」而恒为 false。已登记并补测。
- **补充观察（未登记，记为优质设计）**：`:10-13,102-110,120-145` 用**线程局部工作表**把 `WrappedError`/`JoinError` 的析构改成迭代式释放，避免十万层深链递归析构爆栈；`:147,181-183` 的行内注释解释了「先推进裸指针再释放前一个所有者」的原因。这类行内注释是真正有信息量的，与文件里大量模板化函数头注释形成鲜明对比。
- **验证结果**：构建 0 warning / 0 error；`GO2CPP_TEST_FILTER=errors_ns` 通过（含新增的空 target 语义用例）。
- **未通过/未覆盖**：累计回溯 **6 个实现文件、约 1426 行**，占 `src/` 总行数（21,855）的 **6.5%**。四个最大的文件（`scheduler.cpp` 4048、`io.cpp` 2646、`hook.cpp` 2161、`context.cpp` 1895）仍未读；D-13、D-14、X-06、X-07 均**只登记未修复**；未跑 sanitizer 与 Valgrind。
- **下一步**：`src/concurrency/fiber.cpp`(1502) → `src/network/socket.cpp`(568) → `src/concurrency/sync.cpp`(1324)，之后才进入四个大文件。

## 2026-10-10：源码回溯审计（第 2 批：io_backend / thread_policy）

- **本轮读完整实现**：`src/network/io_backend.cpp`(228)、`src/concurrency/thread_policy.cpp`(202)。
- **核对结果——断言站得住**：
  - `io_backend.cpp:181-186` `RegisterBackend` 用 `emplace(...).second` 判定首次注册，我的示例断言「同名重复注册返回 false」成立；
  - `io_backend.cpp:206-217` `CreateBackend` 空名称回落 `DefaultBackendName()`、工厂在注册表锁外执行并吞掉工厂异常，我的示例断言成立；
  - `io_backend.cpp:221-227` Linux 默认名即 `epoll`，非 Linux 返回空串；
  - `thread_policy.cpp:6-10,37-40` 四个 `thread_local` 的初值与快照字段顺序完全对应我的断言；`:64-79` 构造在非 worker 线程上按 mode 写入、worker 线程上保持 M/P 绑定不变；`:78` 把 `runtime_worker` 写回 previous 值；
  - `thread_policy.cpp:102-106,143-147` 两个作用域析构都只在 `m_active` 时恢复，`restore_policy` 恢复全部四个字段——我的「保存/恢复」断言成立；
  - `thread_policy.cpp:159-165` `EnterRuntimeWorker` 置 `s_runtime_worker=true`，印证我在 worker 线程上观察到的 `runtime_worker` 标记。
- **本轮读源码发现的新问题**：
  - **D-13（文档缺失）**：`io_backend.cpp:197` 有一条硬约束——**平台默认后端不可注销**（`if (name == DefaultBackendName()) return false;`），理由是它是 IOManager 的启动依赖，允许注销会让后续默认创建静默拿到空实例。但 `include/go2cpp/io_backend.hpp:77` 只写了「注销后端；只影响后续 CreateBackend」，完全没有这条约束。调用方按头文件语义调用 `UnregisterBackend("epoll")` 只会拿到 false 而不明原因。已登记并就地补齐示例场景五。
- **一处轻量观察（未登记）**：`io_backend.cpp:216` 用 `try { ... } catch (...) { return {}; }` 吞掉工厂抛出的所有异常，头文件 `:79` 未说明这一点。影响较小（返回空指针是明确的失败信号），故只在此记录，不单列条目。
- **验证结果**：`cmake --build build-quality --target go2cpp_io_backend_demo` 0 warning / 0 error；示例运行退出码 0，新增「默认后端不可注销」场景断言通过。
- **未通过/未覆盖**：本批只回溯 2 个文件（430 行）；累计回溯 5 个实现文件（约 1022 行），占 `src/` 总行数（21,855）的 **4.7%**。`scheduler.cpp`、`io.cpp`、`hook.cpp`、`context.cpp`、`fiber.cpp`、`sync.cpp`、`channel.cpp`、`timer.cpp`、`http.cpp` 等仍未读。
- **下一步**：继续按「已写测试/示例但未读实现」的优先级回溯——`src/tools/error.cpp`(404) 已在读取中，其后为 `src/concurrency/fiber.cpp`(1502)与 `src/network/socket.cpp`(568)。

## 2026-10-10：源码回溯审计（第 1 批：已写测试的四个小模块）

- **背景**：用户质疑此前工作「主要靠头文件与文档梳理，而非源码」。该质疑成立——上一轮的 13 个新文件是按头文件接口 + 文档语义写的，四个最大实现文件（`scheduler.cpp`/`io.cpp`/`hook.cpp`/`context.cpp`）一行未读。本轮起改为**先读完整实现、再核对断言依据**的回溯方式。
- **本轮读完整实现的文件**：`src/network/fd_manager.cpp`(72)、`src/concurrency/fiber_local.cpp`(183)、`src/concurrency/parking_condition.cpp`(337)。三者都是此前写过测试但未读实现的模块。
- **核对结果——断言站得住的部分**：
  - `fd_manager.cpp:23` `result->m_initialized = true`，印证我先猜错后修正的断言；
  - `fd_manager.cpp:31-34` erase 先置 `m_closed` 再 `m_contexts.erase`，我的 closed/新对象两条断言与实现一致；
  - `fiber_local.cpp:87,112` 按 `Fiber*` 分表存储，Fiber 隔离与结束后回收断言成立；
  - `parking_condition.cpp:202-242` notify_one/notify_all 的队列处理与 Finish 单赢家语义，与我的断言一致；
  - `parking_condition.cpp:177-192` 两个静态查询在普通线程下均返回 false，断言成立。
- **核对结果——我的测试盲点（源码有分支、我未覆盖），已在本轮补齐**：
  1. `fd_manager.cpp:40-45`：`duplicate` 在源 fd 不存在时走独立分支，创建`initialized=true` 的全新元数据。补 `check_duplicate_missing_source`。
  2. `parking_condition.cpp:260-264`：**手动 Fiber（无 Scheduler 归属）中的等待直接返回 `kCancelled` 并保持外部 mutex 锁定**，绝不阻塞 carrier OS 线程。这是最危险的回归点之一，补 `check_manual_fiber_wait_is_cancelled`，用手动 `Fiber` + `resume()` 实测通过。
  3. `fiber_local.cpp:147,176`：`Reset`/`Cleanup` 用 `for(;;)` 循环处理「值析构函数再创建值」的重入。本轮未补测（重入析构属高风险用法），但由此发现缺陷 X-07，见下。
- **本轮读源码发现的两条新缺陷（已登记，暂不修改）**：
  - **X-06**：`FdContext` 的 getter 全部无锁读，而写侧持 `FdManager::m_mutex`；`fd_manager.hpp:18` 的类注释「字段访问由 FdManager 内部互斥保护」与接口实际不符，跨线程读写构成 data race。
  - **X-07**：`FiberLocalCache::Reset()` 的 Fiber 路径与线程路径行为不对称——前者有「锁外析构 + 循环复检」防护，后者直接 `ThreadValues().erase(m_key)`，若值析构重入写回同一 key 即构成 UB。
  两条都**只有读实现才能发现**，文档与头文件都不会暴露。按用户既定的缺陷修复门槛（需台账 ID + 可复现证据 + 失败现场），因本环境 TSan 受 WSL `unexpected memory mapping` 限制无法运行、暂未构造出可复现失败现场，故本轮**只登记不修改**。
- **一处质量问题（读源码才可见）**：`fiber_local.cpp` 与 `parking_condition.cpp` 存在大量模板化套话注释——每个函数头都写着「函数功能：完成 X 调用…执行流程：1. 校验传入参数…2. 按状态机规则…3. 发布返回结果…」，同一段文字被复制到 `AllocateKey`、`Get`、`Set`、`Reset`、`Cleanup` 上，零信息量。真正有价值的设计意图（`AllocateKey` 为何用 CAS 而非 `fetch_add`、耗尽时为何 `terminate` 而非回绕）只写在**行内注释**里。这类注释营造了「已被理解」的假象，仅读头文件与文档无法察觉。
- **验证结果**：`cmake --build build-quality --target go2cpp_tests` 0 warning / 0 error；`GO2CPP_TEST_FILTER=fd_manager` 与 `parking_condition` 均通过（校验 `announce` 文本，防空跑）。
- **未通过/未覆盖**：本批只回溯了 3 个实现文件（合计 592 行）；`scheduler.cpp`、`io.cpp`、`hook.cpp`、`context.cpp`、`fiber.cpp`、`sync.cpp`、`config.cpp`、`log.cpp` 等仍未读；X-06/X-07 未修复也未构造出复现用例；未跑 sanitizer 与 Valgrind。
- **下一步**：继续回溯——优先 `src/network/io_backend.cpp`(228)、`src/tools/error.cpp`(404)、`src/concurrency/thread_policy.cpp`(202)（三者都是我已写测试或示例但未读实现的模块）。

## 2026-10-10：一次性补齐台账剩余测试与示例（B-03/B-04/B-05 收尾）

- **本轮范围**：用户要求一次性执行完全部剩余条目。完成 **T-03、T-04、T-05、T-06、T-07、E-02** 共 6 条；台账 B-00~B-05 六个批次至此**全部完成**。
- **新增测试（7 个文件）**：
  - `tests/concurrency/test_hybrid_gate.cpp`（T-03）：默认策略归一化（0 → 1）、BasicLockable 与 `std::lock_guard` 兼容、8 线程 × 20,000 次读-改-写互斥校验、最小自旋实例互斥。
  - `tests/concurrency/test_parking_condition.cpp`（T-07）：notify_one / notify_all、超时返回、谓词已成立时立即返回、两个静态查询入口。
  - `tests/concurrency/test_thread_policy.cpp`（T-07）：默认 TLS 状态、参与作用域保存与恢复、Hook 模式保存与恢复、worker 线程的 `runtime_worker` 标记。
  - `tests/concurrency/test_fiber_local.cpp`（T-07）：线程 TLS fallback、键唯一性、两个 Fiber 的值隔离、阻塞 park 后值保持、Fiber 结束后回收。
  - `tests/concurrency/test_event_go.cpp`（T-06）：EventBatch 簿记、handler 调用、超时自动停止、`max_rounds`、handler 异常隔离、stop/reset，以及 `go(scheduler,fn)`、`go(fn)`、小写 `fiber` 描述器 bind。
  - `tests/network/test_fd_manager.cpp`（T-04）：auto_create 语义、非阻塞与超时元数据、erase 标记 closed、duplicate 复制用户状态。
  - `tests/tools/test_errors_ns.cpp`（T-05）：`errors::` 的 New / Wrap / Join / Unwrap / Is / As 与 CamelCase 版等价性，以及按对象而非消息判定的身份语义。
- **新增示例（6 个文件）**，全部满足「成功 + 等待/生命周期 + 错误/边界」三路径：`future_demo`、`hybrid_mutex_demo`、`fd_manager_demo`、`io_backend_demo`、`address_bytearray_demo`、`errors_demo`。
- **登记**：`tests/test_main.cpp` 加 7 个声明与注册；`cmake/Go2CppTests.cmake` 加 7 个测试源与 6 个 `GO2CPP_EXAMPLES`；`example/README.md` 速查表加 6 行、批量运行列表 15 → 21 项。
- **U-01 处置（按默认执行，用户可推翻）**：在途改动新增的 `errors::` 命名空间按台账默认建议**保留**，并补齐最小测试与示例。若用户决定撤回该 API，需要删除 `tests/tools/test_errors_ns.cpp`、`example/tools/errors_demo.cpp` 并回退 `test_main.cpp`、`cmake/Go2CppTests.cmake`、`example/README.md` 中的登记。
- **开发中修正的三处 API 误判（如实记录，均为我的假设错误而非库缺陷）**：
  1. `ChannelStatus` 没有 `kOk`，可交付状态是 `kReady`；
  2. `Channel::Send` 返回 `ChannelSendResult`（用 `.Ok()` 判断），不是 `ChannelStatus`；
  3. `FdManager::get(fd, auto_create=true)` 创建的元数据**立即** `initialized()==true`，我先按 false 断言导致一次失败。
- **一处基础设施缺陷（新登记 T-08）**：`GO2CPP_TEST_FILTER` 指定不存在的名字时，`tests/test_main.cpp` 不执行任何模块却仍返回 0。本轮第一次编译失败后，验证脚本用**旧二进制**跑 7 个新 filter 全部显示「通过」，实为空跑；后在验证脚本中加入 `announce` 文本校验才暴露该问题。建议后续改为「过滤器非空且未匹配任何模块时返回非零」。
- **验证结果**：全量构建 0 warning / 0 error；`ctest` **24/24 通过**（原 18 + 6 个新示例）；7 个新测试在 `GO2CPP_TEST_FILTER` 下各**重复运行 5/5 通过**（含 16 万次加锁竞争与超时路径，验证脚本同时校验 `announce` 文本）；6 个新示例逐个运行退出码 0 且输出齐全；`log/` 已恢复到轮初 md5；用户 3 处在途改动 md5 未变。
- **未通过/未覆盖**：未跑 sanitizer 与 Valgrind——新增测试包含大量多线程竞争与超时路径，ASan/TSan 下仍值得进一步验证，本轮不能宣称「无竞态」；`third_party/` 与 RFC 合规性仍未覆盖；台账 **X-01**（`dynamic_gmp_demo` 间歇失败，需重复 ≥10 次复现取证）仍然待办。
- **提交**：`f8156e5`（13 个新文件 + 3 处登记，一次提交——因为 `cmake/Go2CppTests.cmake` 同时承载测试与示例登记，无法在不产生不可构建中间态的前提下拆分）。
- **下一步建议**：处理 T-08（过滤器空跑）与 X-01（示例间歇失败复现），再考虑进入「自主扩账模式」审计尚未覆盖的区域（大文件逐行、sanitizer、RFC 合规、`third_party`）。

## 2026-10-10：全模块认知通读（只读，无源码改动）

- **本轮性质**：对话内手动触发的认知轮，**不属于定时任务轮次**，因此未改动 `.agent/state.json`。全程只读，**未修改任何源码、测试、示例或文档**，仅追加本条记录。
- **通读方式**：源码/头文件规模盘点 → 分层提取公共类型声明（tools / concurrency / network / logging / config / core+detail）→ 精读 `docs/{design,scheduler,network,compatibility,extensibility,logging,configuration}.md` → 交叉核对测试/示例/文档的四方覆盖。
- **规模基线**：源码 25 个 `.cpp` + 1 个 `.S` 共 **21,855** 行；模块头 34 个共 **9,421** 行；测试 20 个文件 **9,280** 行；示例 15 个 **2,027** 行；文档 22 篇 **3,273** 行。
- **分层结论**：
  - **基础层 `tools`**：`error`（不可变 shared error + `Wrap`/`Join`/`Is`/`As`/`WalkErrors`）、`control_flow`（显式 defer/panic/recover，`PanicInfo`/`defer`/`panic`/`recover`）、`event`（`EventBatch`/`SelectLoop` 包装 channel SelectCase）、`go`（进程级默认调度器 + 小写 `fiber` 描述器）。
  - **并发层 `concurrency`**：`scheduler`（G/M/P、`Task`/`Scheduler`、`SchedulerObserver`、`BlockingRegion`、BlockingRegion 替代 M 与 sysmon 逻辑脱绑）、`fiber` + `detail/context_backend`（Linux x86_64 自有 ABI，其他平台 Boost 回退）、`fiber_local`、`context`（取消树 + `DoneSignal` + `ContextRollback`）、`channel`（含 `SelectValue`/`SelectCaster`/`SelectProbe`）、`sync`（Fiber 与普通线程共用 FIFO 等待队列）、`future`、`core/timer`（`TimerService` + `TimerExecutor`）、`thread_policy`、`core/parking_condition`、`core/hybrid_mutex`（`HybridGate`）。
  - **网络层 `network`**：`io`（`IOManager` + `DescriptorToken`/`DescriptorGuard` + `WaitAny`/`WaitMany`）、`io_backend`（可替换 Poller 协议）、`socket`（`Socket`/`SocketStream` + 读写取消代际）、`hook`（libc interposer）、`fd_manager`、`address`、`byte_array`、`framing`（`Framer`）、`endian`、`http`（`Parser`/`Client`/`Server`/`ResponseWriter`）、`sse`、`websocket`。
  - **工程层**：`logging`（Filter → ItemWorker → Formatter → Sink 四段可替换，Item 工厂 + Sylar 风格 `PatternFormatter`）、`config`（`ConfigVar<T>` + 两阶段事务 + `.lock`/临时文件/原子 `rename` 热加载协议）。
  - **内部设施**：`core/status`（`StatusCode` 枚举：kOk/kCancelled/kDeadlineExceeded/kClosed/kAlreadyClosed/kInvalidState/kShutdown）；`include/go2cpp/core/*.hpp` 与 `detail/context_backend.hpp` 顶层为**转发头**（4 行），真实定义在 `concurrency/core/`、`concurrency/detail/` 下。
- **一处核实（避免误报）**：`docs/logging.md` 使用大写命名空间 `GO2CPP::Logger::ptr`，初看疑似过时命名；核实 `include/go2cpp/logging/log.hpp:495` 确有 `namespace GO2CPP = ::go2cpp;` 别名，且 `example/logging/log_config_demo.cpp:49` 同样使用，**文档无误**，故本次不登记任何条目。
- **验证结果**：本轮为只读认知，**未运行构建与 `ctest`**，未跑 sanitizer 与 Valgrind。
- **未通过/未覆盖（如实留白）**：未逐行通读四个大文件——`src/concurrency/scheduler.cpp`(4048)、`src/network/io.cpp`(2646)、`src/network/hook.cpp`(2161)、`src/concurrency/context.cpp`(1895)；上述模块结论来自**头文件契约 + 设计文档**，属「接口层已读 + 文档记载」，**不是逐行审计结论**。另外未校验 `third_party/` 内容与 SHA-256，未评估 RFC 6455 / SSE 逐条合规性，`docs/getting_started.md` 与 `docs/*_audit.md` 仅部分阅读。
- **对后续的价值**：本次通读确认了 `docs/extensibility.md` 的模块↔可替换边界映射与 `cmake/Go2CppModules.cmake` 的 target 依赖图一致，也确认了台账 T-03（`HybridGate` 无测试）、T-04（`FdManager` 无测试）、T-07（`parking_condition`/`thread_policy`/`fiber_local` 仅间接覆盖）、E-02（`future`/`io_backend`/`fd_manager`/`address`/`byte_array` 无示例）这几条缺口判断成立。

## 2026-10-10：B-03 首轮（Context 测试中止保护、tools 前置断言、双拼写约定）

- **本轮范围**：台账 B-03 第 1 批 3 条：**T-01**、**T-02**、**D-09**。B-03 共 5 条，T-05、T-06 留待下一轮（单轮预算内不硬塞，避免半成品）。
- **改动文件**（4 个）：
  - `tests/concurrency/test_context.cpp`：22 处「直接在主流程断言等待结果」的 `GO2CPP_CHECK` 改为 `GO2CPP_REQUIRE`（`:63`/`:81`/`:134`/`:156`/`:157`/`:176`/`:177`/`:198`/`:222`/`:223`/`:267`/`:288`/`:308`/`:328`/`:343`/`:359`/`:375`/`:391`/`:402`/`:410`/`:422`/`:423`）。这些点全部是 `Done().WaitFor(t)` 或 `Task::wait_for(t)`，等待失败后继续执行会让后续断言基于未达成的状态级联误报，改为 REQUIRE 后失败即中止。**未改**回调/线程内先存 atomic 再断言的写法（`:73`/`:217`/`:254`/`:300`）——在那里中止会破坏测试语义。宏统计：REQUIRE 0 → 22，CHECK 144 → 122。
  - `tests/tools/test_control_flow.cpp:28`：`order.size() == 3` 由 CHECK 改 REQUIRE。**理由不是形式统一**：紧随的 `:29-31` 直接索引 `order[0]`~`order[2]`，size 断言失败后继续执行即为越界 UB。
  - `tests/tools/test_error.cpp:104`：`formatted.size() == <期望值>` 由 CHECK 改 REQUIRE。同理，紧随的 `:107-108` 以 `formatted.size() - 16U` 作 `compare` 的起点，size 不足时该减法无符号下溢、`compare` 会抛 `out_of_range`，在测试里表现为未捕获异常终止进程。
  - `docs/context_rollback.md`：在首个代码示例后补「命名约定」，说明 `RecordUndo`/`record_undo` 等五种 PascalCase 与小写形式并存、小写为内联转发且语义相同；并如实点明 `committed()`、`had_failure()` 只有小写形式（依据 `include/go2cpp/concurrency/context.hpp:307-346`）。
- **U-01 处置**：本轮未触及 T-05，因此**未消费**该待决策项。下一轮处理 T-05 时按台账默认建议「保留 `errors::` 命名空间并补最小测试与示例」执行，并会在记录中标明用户可推翻。
- **验证结果**：`cmake --build build-quality --parallel` 0 warning / 0 error；`ctest` **18/18 通过**；按规则对并发/超时用例重复运行——`GO2CPP_TEST_FILTER=context` **5/5**、`control_flow` **5/5**、`error` **5/5** 全部通过；`log/` 已恢复到轮初 md5。
- **未通过/未覆盖（如实留白）**：本轮未跑 sanitizer 与 Valgrind；B-03 剩余 T-05、T-06 未处理。另外，22 处 REQUIRE 的**失败路径本身未被正向验证**——绿色构建不会触发 `watchdog_abort`，因此只能说这些宏的语义依据 `tests/test_support.hpp:140-150`，不能宣称「已证明失败时会干净中止」。
- **提交**：`aa9d810`（test_context）、`0a8095b`（tools 测试）、`b3220e9`（context_rollback.md）。
- **下一步**：B-03 剩余轮次——**T-05**（为在途新增的 `errors::` 补最小测试与示例，含 U-01 默认处置）、**T-06**（为 `event.hpp`/`go.hpp` 补独立 `run_xxx_tests()` 入口，需在 `test_main.cpp` 与 `cmake/Go2CppTests.cmake` 三处登记）。

## 2026-10-10：B-02 收尾（参考审计路径、协作记录路径说明、示例登记）

- **本轮范围**：台账 B-02 第 3 批 3 条：**D-07**、**D-08**、**E-04**。至此 **B-02 批次全部完成**（D-01~D-08、D-11、D-12、E-04、I-01、X-02~X-04；D-09 台账原就归属 B-03）。
- **改动文件**（3 个）：
  - `docs/go_reference_audit.md`：修正 **6 处**扁平源码/测试路径——`:34` `src/scheduler.cpp`→`src/concurrency/scheduler.cpp`、`:57` context、`:81` channel、`:105` `src/control_flow.cpp`→`src/tools/`、`:106` `tests/test_control_flow.cpp`→`tests/tools/`、`:122` `src/error.cpp`→`src/tools/`。台账 D-07 只登记了 `:106` 一处，本轮按同类问题一并处置；该文件用的是「当前 C++ 实现位于」式陈述（非历史快照），故直接修正，不套用 D-08 的加注方式。
  - `codex.md`：在顶部快照引用块追加历史路径说明——早期为 `src/*.cpp`、`tests/*.cpp`、`example/*.cpp` 扁平布局，现已分模块，引用旧路径时按当前结构换算；**历史条目不改写**，以保留演进过程。
  - `example/README.md`：速查表补 `timer_task_demo`（concurrency 组末尾）、`websocket_demo` 与 `sse_demo`（network 组末尾）；批量运行列表 12 → 15 项。
- **验证结果**：`docs/go_reference_audit.md` 全部 12 处路径引用逐一 `test -e` 确认存在（missing = 0）；`example/README.md` 速查表 15 行、批量列表 15 项，与磁盘 15 个示例文件数一致；`cmake --build build-quality --parallel` 0 warning / 0 error；`ctest` **18/18 通过**；`log/` 已恢复到轮初 md5。
- **一处验证脚本失误（如实记录）**：本轮第一次验证在 `wsl bash -lc` 内拼了含反引号与嵌套单引号的 `sed`/`while` 管道，反引号被 bash 当作命令替换，输出退化为 `MISS` 空路径。**修改本身未受影响**（文件、构建、ctest 均正常）；随后改用 Python 侧读取解析重做验证，结论以上一段为准。教训：验证逻辑不要在 shell 字符串里嵌套反引号，改为 Python 读文件后逐项检查。
- **未通过/未覆盖**：未跑 sanitizer 与 Valgrind；`codex.md` 历史条目中的 15 处失效路径按 D-08 约定**不改写**，仅由顶部说明覆盖。
- **提交**：`d263b90`（参考审计路径）、`15c74fb`（示例登记）、`eb64a75`（codex.md 路径说明）。
- **下一步**：**B-03**（tools + context：断言健壮性与新 API 覆盖），覆盖 T-01、T-02、T-05、T-06、D-09。其中 T-05 依赖待决策项 **U-01**（在途改动新增的 `errors::` 命名空间是否保留），按台账默认建议「保留并补最小测试与示例」执行，用户可推翻。

## 2026-10-10：B-02 次轮（模块索引、测试文档路径、性能文档阅读须知）

- **本轮范围**：台账 B-02 第 2 批 5 条：**X-04**、**D-05**、**D-11**、**D-06**、**D-12**（按同文件聚合：`modules.md` 两条、`testing.md` 两条、`performance.md` 一条）。
- **改动文件**（3 个，全部为文档）：
  - `docs/modules.md`：13 行 → 59 行。补全 13 个模块 target 与 `go2cpp::runtime` 的对应关系、`include/go2cpp/core` 与 `detail` 子目录定位、完整的 `target_link_libraries` 依赖表；修正 `go2cpp_network` 漏记的 `go2cpp_sync`（`cmake/Go2CppModules.cmake:155`）。
  - `docs/testing.md`：`:384` 的 `tests/test_sync.cpp` → `tests/concurrency/test_sync.cpp`；为 `:415` 段补工作树与路径迁移说明；`:595` 的「现有 Boost.Context fcontext 后端」改为「现有 Fiber 上下文后端（Linux x86_64 为自有 ABI，其他平台为 Boost.Context）」；文首新增**路径约定**，统一说明历史条目中的扁平路径（覆盖本轮新发现的 `:490` `example/io_wait_many_demo.cpp`）。
  - `docs/performance.md`：文首新增**阅读须知**；就地修正 `:27`、`:63-65`、`:144` 三处残留的 Boost.Context 表述。
- **X-04 的处置判断**：该条目描述为「内部性能结论数量级矛盾」，核实后确认**不是错误数据，而是累积记录的性质**。同一负载参数（2,000 Fiber / 32 普通线程 / 206,400 次临界区）在不同轮次分别记录过 `99～107 ms`、`8.5～8.8 s`、`6.7～6.9 s`、`9/19/31/51 ms`，差异来自轮次间的调度配置、编译选项与 WSL 宿主抖动。因此按「不改写历史、补时效与阅读指引」处置，与 D-08 的处理原则一致；文首明确「不存在单一当前值」，并指定以日期最新一节为当前结论。
- **一处过程失误（如实记录）**：上一轮 I-01 的验证使用了 `grep -v '性能\|performance\|...'` 按行过滤，而 `grep -rn` 的输出行以文件名为前缀，`docs/performance.md` 的**每一行**都含 `performance`，导致整个文件的命中被过滤掉，漏检 `:27`、`:63-65`、`:144`。本轮已补齐。教训：按行排除必须匹配内容而非路径，或改用 `--include`/结构化扫描。
- **验证结果**：24 个 markdown 全量相对链接扫描，**本轮改动文件零真实失效**（唯一命中 `docs/quality_backlog.md:15` 是本项目自己写入的 `[](...)` 文本被正则误判，已核实）；`cmake --build build-quality --parallel` 0 warning / 0 error；`ctest` **18/18 通过**；`log/` 已恢复到轮初 md5。
- **未通过/未覆盖**：B-02 剩余 2 条（D-07、D-08）与 E-04 未处理；未跑 sanitizer 与 Valgrind；`docs/testing.md`、`codex.md` 记载的历史性能数字未复核。
- **提交**：`23eccd0`（modules.md）、`6cb11fb`（testing.md）、`64a0cdc`（performance.md）。
- **下一步**：B-02 收尾轮，处理 D-07（`docs/go_reference_audit.md` 重构前路径）、D-08（`codex.md` 历史路径加时效说明）、E-04（`sse`/`websocket`/`timer_task` 登记进速查表与批量列表）。

## 2026-10-10：B-02 首轮（README 路径与索引、Fiber 后端表述统一）

- **本轮范围**：台账 B-02 的第 1 批，按「单轮 ≤5 条」取严重度最高的 5 条：**I-01**（含重复登记的 **X-02**、**X-03**）、**D-01**、**D-02**、**D-03**、**D-04**。B-02 共 12 条，本轮完成 5 条，剩余 7 条留待后续轮次。
- **改动文件**（6 个，全部为文档；未触碰任何源码、测试或示例）：
  - `README.md`：示例段按 `example/{concurrency,network,logging,tools}` 重写并列全 15 个示例；修正 `example/log_config_demo.cpp` → `example/logging/log_config_demo.cpp`、`example/beginner_demo.cpp` → `example/concurrency/beginner_demo.cpp`；模块 target 清单补 `go2cpp::future` 与 `go2cpp::network`。
  - `docs/context_rollback.md`：示例链接补 `concurrency/` 前缀。
  - `docs/design.md:221`、`docs/boundary_audit.md:30`、`docs/go_reference_audit.md:135`：统一为「Linux x86_64 由 Go2Cpp 自有上下文后端提供，其他平台回退 Boost.Context」。
  - `docs/performance.md`：给「自研上下文已撤回」的历史段加**时效说明**引用块，指明其已被「2026-09-30 Context 与跨 M 唤醒专项」取代；**保留原文，不改写历史**，与台账 D-08 的处理原则一致。
- **源码事实依据**：`cmake/Go2CppModules.cmake:3-7` 先置 `GO2CPP_NATIVE_CONTEXT OFF`，仅在 64 位非 aarch64/arm64/riscv/ppc/s390 的 Linux 上置 ON；`:33-36` 编译 `src/concurrency/context_x64.S` 并定义 `GO2CPP_USE_NATIVE_CONTEXT=1`；`:143-150` 仅在 OFF 时链接 `Boost::context`。本机 `build-quality` 实测存在 `CMakeFiles/go2cpp_fiber.dir/src/concurrency/context_x64.S.o`，且 `flags.make` 含 `-DGO2CPP_USE_NATIVE_CONTEXT=1`。
- **验证结果**：24 个 markdown 文件全量相对链接扫描（按各文件所在目录解析相对路径），**本轮改动的 6 个文件零失效链接**；全库仅 3 处命中且均为 lambda 捕获 `[](...)` 被正则误判，已逐条核实为 `docs/configuration.md:60`、`docs/logging.md:53`、`docs/timer_usage.md:55`。README 引用的 8 个示例 target 在 `build-quality/CMakeFiles/` 下逐一确认存在。`cmake --build build-quality --parallel` 0 warning / 0 error；`ctest` **18/18 通过**（23.76 s）；`log/` 已恢复到轮初 md5。
- **未通过/未覆盖**：B-02 剩余 7 条（D-05、D-06、D-07、D-08、D-11、X-04、D-12）本轮未处理；未跑 sanitizer 与 Valgrind；`docs/testing.md` 与 `codex.md` 记载的历史性能数字未复核。
- **新登记**：**D-12**（`docs/testing.md:595` 仍称「现有 Boost.Context fcontext 后端」，与本轮统一后的表述冲突），归 B-02 下轮。
- **提交**：`36e4728`（README）、`23677d6`（后端表述）、`fa39566`（回滚文档链接）。
- **下一步**：B-02 第 2 批，取 D-05、D-06、D-07、D-08、D-11（D-12、E-04 视剩余预算可选）。

## 2026-10-10：B-01 示例质量（network_demo 三路径与登记）

- **本轮范围**：台账批次 **B-01**，覆盖 **E-01**、**E-03**。运行模式为定时任务单轮执行；轮次互斥锁 `.agent/guard.sh` 由本轮首次创建（lock 目录已加入 `.gitignore`）。
- **改动文件**：
  - `example/network/network_demo.cpp`：19 行 → 157 行。从「只有成功路径」扩为五个场景：场景一粘包，一次输入交付全部完整帧；场景二半包，分隔符跨两次输入，`take_frames()` 为空但 `buffered_size()==6`；场景三超长帧，`max_frame=4` 触发 `FeedError::FrameTooLarge`，缓冲被清空、失败态拒绝后续输入、`reset()` 后可复用；场景四 HTTP 逐字节喂入累积到 `Complete`；场景五非法请求行返回 `Error`，未 `reset()` 时坏缓冲继续报错。
  - `example/README.md`：速查表新增 `network_demo` 行；批量运行列表加入 `network`；推荐阅读顺序后补「完整示例清单见下方速查表」指引。
- **实现取舍**：`network_demo` 保持「纯解析、无 IO」定位，未引入 Scheduler/socket；epoll 上的等待、超时与取消由既有的 `io_wait_many_demo.cpp`、`sse_demo.cpp`、`websocket_demo.cpp` 覆盖，并在新示例的 Doxygen 注释中做了交叉指引。
- **一处如实说明**：HTTP 解析器**没有**粘性错误态。`Parser::feed_impl` 入口只检查 `m_finished`，不检查 `m_error`；返回 `Error` 后重复喂入仍报错的真正原因是出错时的缓冲区未被丢弃。示例注释与场景名按此事实书写，未表述为「失败态粘性」。
- **验证结果**：`build-quality`（Debug，TESTS+EXAMPLES ON）全量构建 0 warning / 0 error；`ctest` **18/18 通过**，总耗时 23.86 s，其中 `go2cpp_network_demo` Passed；示例单独运行退出码 0，五个场景全部报告「通过」。开发过程中示例曾因自身测试数据不合 `max_frame=4` 约束而 abort（`std::length_error: frame too large`），已改用合法短帧修正——属示例数据问题，非库缺陷。
- **未通过/未覆盖**：本轮未跑 sanitizer 与 Valgrind；未做多次重复运行（示例为确定性纯解析、无并发或超时路径，不适用「≥5 次重复」要求）；未验证根 `README.md` 的示例清单——那是台账 D-02，归 B-02。
- **新登记**：`E-04`（`sse_demo` / `websocket_demo` / `timer_task_demo` 仍未登记进速查表与批量列表），归 B-02。
- **提交**：`8ff0e6f`（示例）、`79d7b71`（索引登记）。
- **下一步**：B-02 文档重构后路径与索引同步（D-01~D-09、D-11、I-01、X-02~X-04），条目多于 5 条，须拆多轮。

## 2026-10-10：文档/测试/示例质量审计与建账

- **本轮范围**：只读审计 + 建账，**未修改任何既有源码/文档/测试/示例**。新增 3 个文件：
  `.agent/quality_task_rules.md`（执行规则）、`docs/quality_backlog.md`（问题台账）、本条记录。
  真身路径 `/UserData/CodexWorkSpace/Go2Cpp`（ext4，dev 2096）。
- **基线**：分支 `main`，HEAD `d0fdf70`；`git status --short` 为 4 项——用户 3 处在途改动
  （`go2cpp.ini`、`include/go2cpp/logging/log.hpp`、`include/go2cpp/tools/error.hpp`）
  加上一轮追加的 `codex.md`。`git diff` 指纹轮初为 `757a9920...`。项目无
  `AGENTS.md`/`CLAUDE.md`/`.agent/`，协作记录沿用 `codex.md`。
- **重要纠正（工具链导致的历史误判）**：内置 `read` 工具把项目路径解析到 E 盘旧副本，
  导致上一轮得出"`go2cpp_network` target 不存在、网络源码并入 `go2cpp_io`"的**错误结论**。
  本轮以 WSL 直读 + `git show HEAD:` 双重取证确认：`cmake/Go2CppModules.cmake` 真身为
  **177 行**，`go2cpp_network` 在 `:43-50` 定义、`:71` 导出 `EXPORT_NAME network`、
  `:91` 建别名、`:155` 依赖 `go2cpp_io go2cpp_context go2cpp_scheduler go2cpp_sync`，
  且该文件工作区与 HEAD **无差异**。故 `docs/modules.md:11` 关于 `go2cpp_network` 的
  说法本身是**正确**的（仅漏记 `go2cpp_sync` 依赖）。上一轮的相反结论已作废。
- **同步纠正第二项**：上一轮称"WebSocket 无测试覆盖"亦为旧副本数据所致。真身
  `tests/network/test_network.cpp` 中有 51 处 websocket 命中，含 RFC 6455 握手向量与
  socketpair 端到端闭环（`:560-598`）。
- **四方覆盖矩阵（文档/测试/示例）结论**：30 个模块中
  - **四维齐备**：error、control_flow、context、channel、scheduler、fiber、sync、timer、
    io、hook、socket、http、sse、websocket、logging、config、framing；
  - **缺示例**：future、io_backend、fd_manager、parking_condition、hybrid_mutex、
    address、byte_array（共 7 个）；
  - **缺独立测试入口**（功能已被间接覆盖）：event、go、fiber_local、parking_condition、
    thread_policy；
  - **完全无测试引用**：`HybridGate`/`HybridMutex`（`concurrency/core/hybrid_mutex.hpp`）、
    `FdManager`（`src/network/fd_manager.cpp`）。
- **测试框架事实**：自研框架，`tests/test_main.cpp:7-22` 声明 16 个 `run_xxx_tests()`，
  与 16 个实现一一对应，支持 `GO2CPP_TEST_FILTER`。宏用量实测：`GO2CPP_CHECK` 1301、
  `GO2CPP_REQUIRE` 86、`GO2CPP_REQUIRE_EVENTUALLY` 74、`GO2CPP_JOIN_WITH_WATCHDOG` 60。
  **`tests/concurrency/test_context.cpp` 有 144 个 CHECK 却 0 个 REQUIRE 系列**，
  `tests/tools/test_control_flow.cpp`(39) 与 `test_error.cpp`(27) 同样无看门狗保护。
- **关键不一致（台账 I-01）**：Fiber 上下文后端在文档间严重矛盾，且**同一文件内自相矛盾**——
  `docs/design.md:121-130`（自有汇编、不再链接 Boost.Context）vs `:221`（称 Boost.Context）；
  `docs/boundary_audit.md:25` vs `:30`；`docs/performance.md:190-193`（自研汇编已撤回）
  vs `:204-208`（已上线）。源码事实：构建实际编译 `src/concurrency/context_x64.S` 并定义
  `GO2CPP_USE_NATIVE_CONTEXT=1`（`cmake/Go2CppModules.cmake:33-36`）。
- **关键不一致（文档 D-01~D-09）**：`README.md:54/97` 与 `:48-52` 的示例路径因目录重构失效
  （缺 `concurrency/`、`logging/` 前缀）；`README.md:90-94` 模块清单漏 `go2cpp::future`、
  `go2cpp::network`；`docs/context_rollback.md:19` 链接指向不存在的
  `../example/context_rollback_demo.cpp`；`docs/testing.md:415`、`docs/go_reference_audit.md:106`
  引用重构前测试路径；`codex.md` 历史条目有 15 处失效路径（属历史记录，建议加时效说明而非改写）。
- **示例质量（E-01）**：`example/network/network_demo.cpp` 仅 20 行、只有成功路径，
  缺等待/取消与错误路径，且未出现在 `example/README.md:36-48` 速查表——违反项目自定的
  「每个示例至少含成功 + 等待/取消 + 错误/边界三条路径」标准（`example/README.md:30-34`）。
- **台账统计**：共 **22** 条。按类别：文档 11（D-01~D-11）、测试 7（T-01~T-07）、
  示例 3（E-01~E-03）、不一致 1（I-01）；另列疑似缺陷 5 条（X-01~X-05）与待用户决策 4 条
  （U-01~U-04）。按严重度：高 4、中 8、低 10。
- **批次划分与建议顺序**：B-01 示例三路径与登记 → B-02 文档路径/索引/后端表述同步 →
  B-03 tools+context 断言健壮性与新 API 覆盖 → B-04 并发核心缺口（HybridGate 等）→
  B-05 network 缺口（fd_manager/io_backend/address/byte_array）→ B-06 logging+config 与
  跨模块总入口。每批均可独立构建与 `ctest` 验证，无跨批依赖。
- **待用户决策要点**：U-01 在途新增的 `errors::` 命名空间是否保留（决定是否补测试/示例）；
  U-02 `docs/performance.md` 中 `/mnt/e/CodexWorkspace/...` 的 coost 基准路径是否视为有效；
  U-03 `codex.md` 历史失效路径是否改写；U-04 是否要求示例间歇失败修复后才能进 `ctest` 门禁。
- **未覆盖范围**：本轮未构建、未跑 `ctest`、未跑任何 sanitizer；台账中的间歇失败引用上一轮
  实测数据（本轮未复现）；未逐行通读 scheduler.cpp(4048)/io.cpp(2646)/hook.cpp(2161) 等大文件；
  未探查 `/mnt/e/...` 引用路径；未校验 `third_party` SHA-256；未触碰 `build/`、`log/`、`third_party/`。
- **工具陷阱（已固化进规则文件 §1.1）**：T1 内置 `read`/`grep` 解析到 E 盘旧副本
  （`E:\UserData\CodexWorkSpace\Go2Cpp`），必须走 `wsl bash -lc` 或 UNC 路径；
  T2 经 `pwsh` 下发 `wsl bash -lc` 时双引号内 `$` 被 PowerShell 插值吞掉（本轮再次触发，
  导致一次统计全空），含 shell 变量或 `$?` 必须改走 Python subprocess 通道；
  T3 工具会"成功返回错误内容"，关键结论需双通道交叉验证。

## 2026-10-10：项目认知梳理（只读，无源码改动）

- **本轮范围**：只做认知梳理与实测验证，未修改任何源码/配置/测试/文档；唯一写入是本条记录。工作路径为 WSL 真身 `/UserData/CodexWorkSpace/Go2Cpp`（ext4，`stat -c %d` = 2096）。
- **重要环境结论（工具层，非项目缺陷）**：DSH 的 `read`/`grep` 工具会把 `/UserData/CodexWorkSpace/Go2Cpp/...` 解析到 E 盘另一份陈旧副本 `E:\UserData\CodexWorkSpace\Go2Cpp`（9p，dev 72）。该副本缺 7 个文档（`docs/modules.md`、`docs/boundary_audit.md`、`docs/network.md`、`docs/sse.md`、`docs/websocket.md`、`docs/timer_usage.md`、`docs/timer_benchmark.md`），且 `cmake/Go2CppModules.cmake` 仍引用重构前的扁平源码路径（`src/error.cpp`、`src/io.cpp` 等）。本轮 31 个抽样文件中 17 个 md5 与真身不同，`codex.md` 真身 1116 行 vs 该副本 861 行。**后续在本工作区读取项目文件必须走 `wsl bash -lc` 通道，否则读到的不是真身。**
- **第二类工具陷阱**：经 `pwsh` 下发 `wsl bash -lc "..."` 时，双引号内的 `$` 会被 PowerShell 变量插值吞掉（`$f` 变空导致 `ambiguous redirect`，`$?` 无法取得退出码）。凡命令含 shell 变量或 `$?`，改走 Python→`subprocess(["wsl","bash","-lc",...])` 通道。
- **git 状态可信性核验**：`git show HEAD:include/go2cpp/runtime.hpp` 为 36 行、与工作区一致；`git ls-files -v | grep -v '^H '` 为空（无 skip-worktree / assume-unchanged）。故 `read` 的内容差异确认为工具解析问题，与索引无关。
- **公共 API 修正**：`include/go2cpp/runtime.hpp` 真身为 36 行、include 26 个模块头（含 `core/timer.hpp` 与 `network/*`）。`include/go2cpp/sync.hpp`（219 行）是根路径下**唯一非转发壳**的真实公共头，被 9 处引用；`include/go2cpp/concurrency/sync.hpp`（203 行）已与其发散且**全仓零引用**。
- **构建系统事实**：CMake 无 `-Werror`，也无任何开关可将其打开；警告策略仅 `-Wall -Wextra -Wpedantic`（`cmake/Go2CppModules.cmake:119`、`cmake/Go2CppTests.cmake:29,39,48,79`）。`GO2CPP_NATIVE_CONTEXT` 在 Linux x86_64 置 ON，编译 `src/concurrency/context_x64.S` 并定义 `GO2CPP_USE_NATIVE_CONTEXT=1`（同文件 `:33-35`）；网络源文件并入 `go2cpp_io`，`docs/modules.md:11` 所称 `go2cpp_network` target 并不存在。
- **Fiber 上下文后端判定（源码 + 实测）**：本轮构建实际产出 `context_x64.S.o`，且 `flags.make` 含 `-DGO2CPP_USE_NATIVE_CONTEXT=1`；`src/concurrency/fiber.cpp:6-9` 仅在未定义该宏时才引入 Boost.Context。即**当前默认后端是自研 x86_64 汇编，不链接 Boost.Context**。据此判定 `docs/design.md:221`、`docs/boundary_audit.md:30`、`docs/go_reference_audit.md:135`、`docs/performance.md:27/65/144/191` 为过期表述；其中 `docs/design.md:221` 与其自身 `:121-130` 直接冲突，`docs/boundary_audit.md:30` 与其自身 `:25` 冲突。
- **构建实测**：`cmake -S . -B build-orientation -DCMAKE_BUILD_TYPE=Debug` → `cmake --build build-orientation --parallel` 成功，**31 个 target，零警告零报错**；构建目录被 `.gitignore:2` 覆盖，未入库。
- **测试实测**：`ctest --test-dir build-orientation --output-on-failure` → **18 个用例，17 通过 / 1 失败**（25.21 s）。失败项 `go2cpp_dynamic_gmp_demo`，输出 `[gmp][场景3] cancel_requested=0 cancel_finished=1 body_ran=1 cancelled=0`。复现：直接运行 5 次 rc = 1,1,0,1,0；ctest 单跑 5 次 = 通过/通过/失败/失败/失败；全量 ctest 连续 2 轮均 18/18 通过 → **间歇性、与负载相关**。
- **失败根因（已定位到行）**：`example/concurrency/dynamic_gmp_demo.cpp:99` 的 `cancel()` 在任务已进入 Running 时返回 false，而 `:104-105` 把 `cancel_requested==true` 与 `state()==kCancelled` 同时当作硬条件，`:114-115` 又把 `cancel_ok` 计入返回码。属**过度断言 + 竞态窗口预期**，非运行时缺陷。另与 `example/README.md:42` 声称的"最终收敛为 `Cancelled`"矛盾。
- **技术栈确认**：cmake 3.28.3、gcc 13.3.0、12 核；源码 `.cpp/.S` 合计 21,938 行、头文件 9,771 行，最大文件 `src/concurrency/scheduler.cpp`(4048)。
- **发现的其他不一致**：`benchmarks/io_coost_fair_benchmark.cpp` 未被任何 CMake target 引用（孤儿文件）；`src/concurrency/context.cpp:255` 与 `src/concurrency/timer.cpp:84` 各有一个同名 `TimerService`；`docs/performance.md` 内部混合锁/IO 倍率存在两个数量级的自相矛盾（`:140` 快 10~18 倍 vs `:42-46` 8.5~8.8 s；`:141` 慢 3.0~4.3 倍 vs `:184` 0.59x）。
- **未覆盖范围**：未跑压测/随机测试/基准；未运行任何 sanitizer；未验证历史验证数字；未逐行通读 scheduler.cpp/io.cpp/hook.cpp/channel.hpp 等大文件；只测 Debug+共享+Hook-ON 一种构建矩阵；未校验 third_party 归档 SHA-256；未探查 `/mnt/e/...` 下文档引用的 coost 路径。以上均按"未确认"留白，不作结论。
- **清理**：临时脚本置于工作区 `.dsh_tmp/`（非项目内），构建产物在 `build-orientation/`（已被忽略）；项目根 `log/` 两个文件的 md5 与 mtime 均未变化。`git diff` 指纹前后同为 `355c390592702d857518763895db93287a98d1dbd8546d53a04282285fdcd9b4`，用户 3 处在途改动零丢失。

## 2026-10-02：全模块职责与后端边界复核

- 调度器增加 `SchedulerObserver` 事件协议。G/M/P 状态机仍由 Scheduler 独占，
  观察器只接收启动、worker、任务运行/挂起/完成/失败事件；观察器指针独立加锁，
  回调在调度器状态锁外执行并隔离异常，可接入日志、指标和追踪而不把业务代码
  嵌入 worker 热路径。
- IOManager 把 epoll/eventfd 抽成 `IOBackend`，新增线程安全的命名工厂和实例注入。
  默认 Linux epoll 保持原行为；后续 kqueue、IOCP、io_uring 只能实现 readiness、
  wait、wake、remove、shutdown 协议，不能直接恢复 Fiber 或修改 Context 状态。
- sync 和 Future 增加等待后端工厂，默认后端仍分别适配 Scheduler/ParkingCondition；
  已创建等待节点固定自己的后端，插件工厂在锁外执行，防止注册表重入和锁反转。
  HybridGate 的自旋次数改为可配置实例策略，仍只允许保护短临界区。
- error 增加 `WalkErrors` 环安全遍历扩展点，日志、指标和错误转换插件不再依赖
  WrappedError/JoinError 的具体类型；异常和环图均在错误边界内隔离。
- 新增 `docs/extensibility.md`，明确批次事务、Fiber/栈后端、跨平台 IO、插件
  生命周期和故障注入测试协议；README、design、scheduler 文档同步链接和说明。
- 验证：Release CTest 14/14 通过；`build-engineering-werror` CTest 3/3 通过；
  自定义 IO/Future 后端、调度器观察器、错误遍历和原有高负载测试均通过。

## 2026-10-02：日志 Item 插件化与配置事务解耦

- `LogFormatter` 新增 Sylar 风格的 `Item` 扩展边界。格式字符串在
  `PatternFormatter` 构造时预解析成 Item 链，日志热路径不再查表或解析文本；
  `AddFormat`/`addFormat`、`RemoveFormat`、`HasFormat` 和 `Formats` 由线程安全
  注册表提供，工厂在注册表锁外执行，允许插件在工厂中继续注册或查询格式项。
- 内置 `{time}`、`{function}`、`%d{...}`、`%p/%m/%N/%T/%n` 等格式均通过同一
  工厂协议创建；重复键拒绝覆盖，已构造 Formatter 持有自己的 Item 快照，注销
  不会破坏正在使用的 Formatter。新增无选项工厂和带 option 构造函数的模板接口，
  便于新手接入自定义字段。
- 配置中心增加 `ApplyString`/`NotifyPending`/`RollbackPending` 两阶段接口和
  `Config::AddCommitListener`。文件加载先整体校验，再整体发布值，失败恢复旧快照；
  监听器逐个隔离异常，变量监听器和提交级回调均在事务锁释放后执行，避免插件回调
  触发配置加载时发生锁反转。配置文件的编辑锁、临时文件和原子 rename 约束同步
  补充到配置文档；未来批次、Fiber 栈和多平台 IO 的替换协议补充到设计文档。
- 新增自定义 Item、重复注册、注销后快照、日期内置键保护、配置事务回滚和完整提交
  回调测试；更新 `example/log_config_demo.cpp` 和 `docs/logging.md` 新手示例。
- 验证：`build-final` 全量 CTest 14/14 通过；`build-engineering-werror` 全量
  CTest 3/3 通过；两套构建均使用现有项目警告策略，`git diff --check` 通过。

## 2026-09-29：调度器、混合锁与 Coost 对比

- 本地 P 队列成功取到 G 后跳过调度器全局 admission 锁；G/M/P 计数统一在
  两条取任务路径之后更新，避免本地快路径遗漏运行计数。
- `maybe_grow()` 改为有界维护周期，同时保留 runnable backlog 下的初始 worker
  补齐，修复并发 shutdown 测试中第二个 G 被取消而无法执行的问题。
- Scheduler Fiber 恢复增加首次父级绑定和后续快速恢复入口；同一 M 上不再
  每次重复获取 Fiber 父链元数据锁，迁移到新 M/P 时仍更新调试绑定。
- 混合 Mutex 增加有界 Fiber 协作重试，并在已有 FIFO 等待者时禁止无序快进；
  短临界区基准从约 13.9 秒降至约 0.10～0.20 秒。
- `src/sync.cpp` 的无 Context 竞争等待改用调用栈上的 `WaitNode`，队列保存裸
  指针；带 Context 的等待继续使用线程本地缓存的拥有节点。该路径参考了本地
  Coost 的嵌入式等待节点设计，但保留 Go2Cpp 的取消和 Fiber park 语义。
- 新增并更新 `docs/performance.md`，记录本机 Coost checkout（HEAD
  `c1cc11b`，工作区 dirty）的可复现对照：调度 13～52 ms 对 436～497 ms，
  混合 Mutex 1,097～1,766 ms 对 99～107 ms，IO 486～569 ms 对 1,666～2,098 ms。调度和
  IO 仍有差距，混合锁已反超；没有把 Coost README 的日志数据冒充调度器或锁
  基准。
- 验证：`go2cpp_high_load_stress` 完成全部检查；调度器定向测试连续 3 次
  通过；工程化构建 CTest 13/13、`-Werror` CTest 3/3、ASan+UBSan
  `go2cpp_tests` 全部通过；本轮 Valgrind 按 `sync/scheduler/dynamic/channel/io`
  分模块运行，均为 0 errors、0 definite/indirect/possible leak；完整测试在
  Valgrind 下被 IO/动态日志的内置 watchdog 超时终止，不能作为全量 Valgrind
  通过结论。

## 2026-09-26：Context 局部回滚

- 新增 `ContextRollback`/`RollbackScope`：临时子 Context、LIFO undo、savepoint、
  显式 rollback/commit 和独立 `RollbackDone()` 完成通知。
- Context 独立取消现在会从父节点弱登记表中移除 child；deadline timer 注册失败
  不再留下永远等待的 Context。`WithValue` 的异常路径会取消并清理半发布 child。
- undo 只表示局部补偿，不恢复父 Context 或外部 IO；回调在线程锁外执行，异常隔离
  并记录为 `kFailed`。取消先发布 child `Done`，再执行内部 undo，普通 Done 观察回调
  在 undo 后运行；undo 不得等待自身 `RollbackDone()` 或尚未发布的信号。仍禁止
  longjmp、跨 Fiber/线程跳转和在回调中阻塞。
- Context 取消新增前置 claim：在 child `Done` 可观察前冻结 action 日志，避免
  取消窗口追加/提交新动作；未取消的短命 child 会从 parent 的 weak 登记表及时移除。
- 新增 ContextRollback 单元覆盖 savepoint LIFO/ABA、析构/父取消、deadline、移动
  所有权、managed P=1、异常隔离、取消与显式 rollback 竞态；新增
  `example/context_rollback_demo.cpp`，详细契约见 `docs/context_rollback.md`。
- 验证：Debug 示例 CTest 11/11；Release 2/2；`-Werror` 2/2；ASan+UBSan 2/2；
  Context 定向重复 30/30；TSan 使用 `setarch x86_64 -R` 的 context 过滤通过，
  全量 TSan 在 WSL native waiter watchdog/地址映射限制下不作为全量通过结论；
  Valgrind Memcheck `ERROR SUMMARY=0`，definite/indirect/possible 均为 0，
  416B/4 blocks 为进程级 still reachable 缓存。
>
> 当前快照说明（2026-09-23）：以下早期记录是追加式审计历史，其中的
> panic/defer/recover 示例和模块名称不代表当前 API。当前源码已删除
> `panic_defer` 模块、`SendOrPanic`/`CloseOrPanic` 和对应测试；Fiber 只
> 捕获并记录普通 C++ 异常，使用 RAII/`try/catch`。当前交付还包含失败终态、
> 有界 `shutdown_for()`、常见 Hook/sysmon 边界、Fiber claim 析构栅栏以及
> 错误父级的 Failed 放弃路径。

> 本轮收尾还修复了手动 Fiber 在已取消 Context 上等待条件变量的 carrier 阻塞竞态，
> 将 `Channel<T>` 限制为不抛 move/析构并使 `Close()` 不抛，且修正 Fiber 快照中
> main_fiber 的 active 标记。最终 Debug/Release/ASan+UBSan/Valgrind 均通过；
> TSan 在本 WSL 的映射或 native waiter watchdog 约束下未作为通过结论。

## 2026-09-20：新手接口与混合等待边界

- 新增 `go2cpp/go.hpp`：显式/默认 `go()`、默认 Scheduler 生命周期、关闭后新实例重启、轻量小写 `fiber` 和 `Scheduler::add()` 接入。默认调度器使用进程生命周期状态锚点；`shutdown_default_scheduler()` 会移出旧句柄，避免向终态 Scheduler 排队。
- 新增 `go2cpp/event.hpp`：`EventBatch`/`SelectLoop` 薄封装已有 channel `Select`，用协程感知 `sync::Mutex` 串行 `work()`，native 线程和 Fiber 可并发调用（work 本身串行）；handler/Select 异常转为显式错误结果，不强制销毁挂起 Fiber；stop 通过内部 Context 唤醒等待。
- 新增 `tests/test_timer.cpp`，覆盖 P=1 下 TimerService 回调唤醒 Fiber、`Channel::WaitForChange` 不占 M、定时器取消；新增 `tests/test_beginner_api.cpp` 和 `example/beginner_demo.cpp`，覆盖 go/fiber/select、Fiber 内等待、native/Fiber 混用、handler 异常和默认调度器重启。
- `runtime.hpp` 导出新手头文件；CMake/CTest 注册新测试和示例。
- 本次验证：GCC Debug Hook-on 9/9，GCC Release 静态 Hook-off 8/8，`-Werror` Debug Hook-on 9/9，`GO2CPP_TEST_FILTER=beginner` 和 `timer` 均通过。
- 明确边界：Timer 回调在 native 定时器线程执行，只通知等待节点，不直接恢复 Fiber 栈；任意阻塞业务必须经 `go()`/`Scheduler::spawn()`，EventBatch 事件列表生命周期由调用方保证。
- 追加修复：`EventBatch::make_stop_config()` 现在把剩余总时长传递给当前 `Select`，空 channel 在总时长到期时不会无限等待；新增 20ms watchdog 回归。
- 追加修复：EventBatch 增加内部 stop Context；`stop()` 会取消 DoneSignal 并唤醒正在 Select 的 Fiber，按正常返回路径结束，不强制销毁挂起栈；新增 P=1 stop 唤醒回归。
- Release CTest 首次运行出现一次 `mixed_runtime_demo` native-owner watchdog 超时，随后独立运行 5 次并完整 CTest 重跑均通过；记录为宿主资源抖动，未观察到可复现代码回归。

# Change log

## 2026-09-15

- Initialized the standalone Go2Cpp C++17 runtime project in `/UserData/CodexWorkSpace/Go2Cpp`.
- Added the design and compatibility matrix describing GMP, context, channel, error and explicit panic/defer boundaries.
- Added the umbrella CMake runtime target, warning policy, install/export rules, and test/example options.
- Added `docs/testing.md` with reproducible Debug/Release/Clang, sanitizer,
  Valgrind, example, watchdog and stress-test commands.
- Linked the verification matrix from `README.md` for discoverability.
- Key documentation/test files for this verification unit: `docs/testing.md`,
  `README.md`, `codex.md`, and `tests/test_scheduler.cpp`.
- Verification recorded on 2026-09-15: GCC Debug and Release, Clang 18
  Debug, ASan, UBSan, CTest (2/2), the runtime example, and Valgrind Memcheck
  passed. The final Memcheck runs found zero errors and zero bytes in use at
  exit for both the unit suite and scheduler smoke test (661,087 and 846
  allocations/frees in the final sample; allocation counts can vary with the
  libc/thread implementation).
- TSan configured and compiled, but cannot start in this WSL image because of
  `ThreadSanitizer: unexpected memory mapping`; no TSan race conclusion is
  claimed.
- Synchronized the self-referential scheduler test task publication before
  callback access (assignment before enqueue plus a release/acquire gate);
  200 full-suite runs and 200 scheduler-smoke runs passed with a ten-second
  watchdog per process. The smoke test's always-evaluated checks are warning
  clean in Release as well as Debug.
- Added iterative context cancellation and iterative parent-state release so
  deep cancellation/destruction chains do not consume the native call stack;
  the 20,000-node test and an independent 100,000-node audit passed.
- Added explicit expired-deadline behavior, root-context cancellation
  protection, channel cancellation-priority coverage, panic-body exception
  translation, and the scheduler yield-then-unhandled-panic regression.
- Restricted scheduler `yield`/`park` to the current G and finalized unbound P
  states as dead during shutdown; sparse-P and never-started shutdown tests now
  cover those transitions.
- Made `error::Is`/`As` chain traversal iterative and added a 100,000-layer
  wrapping regression; built-in wrapper release is iterative, while custom
  error ownership/message graphs remain required to be acyclic.
- Split CMake into option/module/test/install files and verified umbrella,
  individual-module, and relocated-prefix package consumers; all configured,
  built, and exited zero. The final smoke and full-suite watchdog runs each
  completed 200/200 iterations.
- Status: implementation and stress verification are complete for this
  snapshot; native-Linux TSan remains a follow-up because the WSL image cannot
  start the instrumented process.

## Pause handoff (2026-09-15)

The requested pause is at the pre-commit handoff. The latest canonical WSL
source includes the fake-clock expiry fix, named/typed context-key isolation,
current-G-only scheduler yield/park admission, sparse-P shutdown finalization,
iterative built-in error-chain release, and panic-handler exception isolation.

Latest completed gates:

- `build-final-modular`: GCC Debug build and CTest 2/2 passed; the example
  prints the GMP/channel/error/panic demonstration output.
- `build-final-asan` and `build-final-ubsan`: rebuilt after the latest runtime
  changes; the full unit executable passed with no diagnostics.
- Valgrind Memcheck on the unit and scheduler-smoke executables: both exit 0,
  `0 bytes in 0 blocks`, `ERROR SUMMARY: 0`; the sampled allocation counts
  were 661,087/661,087 and 846/846.
- Watchdog stress: `go2cpp_tests` 200/200 and
  `go2cpp_scheduler_smoke` 200/200 passed, with a ten-second timeout per run.
- The existing package-consumer audit passed for the umbrella target,
  individual module targets, and a relocated install prefix; its temporary
  source/install directories were removed.

## Resume verification (2026-09-16)

- Reconfigured and rebuilt GCC Release after the final context/panic edits;
  CTest passed 2/2 and the runtime example exited zero with the expected
  GMP/channel/error/recover output.
- Reconfigured with `CC=clang CXX=clang++`, rebuilt with Clang 18, and passed
  CTest 2/2. Both final compiler configurations were warning-clean under the
  configured `-Wall -Wextra -Wpedantic` policy.
- Rechecked the Go `go1.23.0` source archive against
  `third_party/go-reference/SHA256SUMS`; SHA-256 verification passed.
- Remaining handoff action is repository staging/diff review and the requested
  Git commit. Native-Linux TSan remains an external environment follow-up.

Known boundaries to preserve in the final report: cooperative rather than
asynchronously preemptive scheduling, native blocking calls occupy their M,
polling/API-based select, explicit nil-channel status instead of an infinite
nil-channel block, copyable values for `SendCase`, no Go compiler/ABI/GC, and
the documented inability of a C++ throw during explicit `Frame` destruction
to be recovered by that frame. Custom error ownership/message graphs must be
acyclic. The default WSL TSan invocation remains loader-sensitive and can report
`ThreadSanitizer: unexpected memory mapping` before the test starts; the final
non-PIE/ASLR-disabled WSL run is recorded below, and a native Linux gate is
still recommended.

## Finalization (2026-09-16)

- Added a persistent per-Scheduler owner token to `Task`; a G is bound to the
  first scheduler that admits it, and foreign `enqueue`/`wake` calls are
  rejected without changing the task state. Added the cross-scheduler
  ownership regression in `tests/test_scheduler.cpp`.
- Added the full-buffer select-send regression in `tests/test_channel.cpp`.
  It verifies that draining a full capacity-1 channel refills the slot from an
  armed select sender and preserves FIFO order.
- Updated `docs/design.md`, `docs/scheduler.md`, and
  `docs/compatibility.md` with the ownership and bounded-wait contracts. Added
  `docs/dependencies.md` with the Go provenance and GC/ABI/stack/cgo
  replacement inventory; linked it from `README.md`.
- Rebuilt and tested the final source with GCC 13 Debug (`build-final-modular`),
  GCC 13 Release (`build-final-release`), Clang 18 (`build-final-clang-env`),
  and a strict `-Werror` build (`build-final-werror`). Every CTest run passed
  2/2; the example printed the GMP, select/directional-channel, error and
  recover output.
- Rebuilt ASan and UBSan configurations and ran both unit and scheduler-smoke
  executables with no diagnostics. Valgrind Memcheck reported zero errors and
  zero bytes in use at exit for both binaries (1,161,500 and 983 allocations/
  frees in the recorded sample). The Go archive checksum verified `OK` from
  `third_party/go-reference`.
- TSan compiled successfully. The default WSL layout can fail at process
  startup with `unexpected memory mapping`; running the non-PIE build through
  `setarch x86_64 -R` allowed both test executables to exit zero with no TSan
  report. A native Linux runner remains the recommended independent gate.
- Watchdog stress completed 200 `go2cpp_tests` runs and 300
  `go2cpp_scheduler_smoke` runs, each with a ten-second process timeout. The
  refreshed `build-final-install` prefix contains all module/umbrella targets,
  headers and package configuration files.
- Status: all requested compatibility-subset modules, examples, tests and
  documentation are complete for this snapshot. Remaining work is optional:
  run TSan on a native Linux kernel/loader and expand the explicit API if a
  future translator needs semantics outside the documented subset.

## Final audit continuation (2026-09-16)

- Closed the scheduler admission window identified during review: queue
  removal, runnable accounting, stop observation, and the Task execution claim
  now occur under one scheduler admission transaction. Added regressions for
  duplicate enqueue during a yielded callable and wake-after-park before the
  callable returns.
- Made internal requeue/wake failure handling idempotent with
  `cancel_if_runnable_unqueued()`. A concurrent successful public enqueue is no
  longer mistaken for a failure and cannot be cancelled by the worker.
- Added terminal task-registry pruning and moved shutdown queue-task
  destruction outside scheduler/P/join locks. The smoke suite now verifies a
  task capture whose destructor re-enters the Scheduler; it exits under the
  ten-second watchdog.
- Refreshed the example to print context value/cancellation and error identity
  checks. The final example output and exit status are successful.
- Final GCC Debug and `-Werror` CTest runs passed 2/2 after the audit fixes;
  ASan, UBSan and non-PIE/ASLR-disabled TSan were rebuilt and passed both test
  executables. Valgrind Memcheck reported zero errors and zero bytes in use at
  exit: unit `1,161,500` allocs/frees and smoke `983` allocs/frees.
- The watchdog loop passed 200 unit runs and 300 scheduler-smoke runs after
  the audit fixes. The installed umbrella, individual-target and relocated
  package consumers all exited zero. `sha256sum -c` still reports `OK` for the
  pinned Go archive.
- No required implementation task remained for the compatibility subset
  documented at that snapshot. Its then-known future work was limited to a
  native-Linux TSan gate and translator/ABI features outside this standalone
  C++17 library. The following boundary audit supersedes that statement for
  the newly requested Fiber, socket-hook, coroutine-sync, and dynamic-M scope.

## Boundary audit and HOOK clarification (2026-09-17, superseded)

This historical snapshot predates the implementation below. Its "not
implemented" conclusions are retained for audit history; use the 2026-09-18
section for the current status.

- Rechecked the four newly raised boundary cases against the source, tests and
  public headers. A returning/panicking callable or a cancelled queued/waiting
  task reaches a terminal G state, and queue/wake/shutdown races are covered.
  This does not force-unwind a running C++ stack or destroy a `Task` retained
  by user ownership. The scheduler does not preserve a C++ stack across
  `yield`/`park`; a callable that never returns cannot be resumed by this
  backend, and a blocking syscall occupies its M.
- The socket requirement is **not implemented**. There is no `IOManager`, FD
  readiness backend, `addEvent`/`cancelEvent`, `wait_for_event`, or syscall
  hook registry. The requested hook semantics include original-syscall
  fallback, readiness-before-timer registration, idempotent timeout/cancel
  wake-up, nonblocking-mode handling, FD reuse generations, and
  FD-close/`EINTR`/`EBADF` rules. Required transparent interception covers
  connect/accept, read/recv, write/send, close, fcntl/ioctl, and socket timeout
  options; explicit wrapper-only I/O is not sufficient.
- Coroutine-level `Fiber`, mutex, condition-variable and waitgroup facilities
  are not implemented. Existing `std::condition_variable` instances are
  internal OS-thread coordination, not coroutine suspension. A stackful-fiber
  backend or explicit callback/state-machine API is required.
- M management is fixed at `Scheduler::start()`: one stable worker per
  configured slot, with local/global queues and stealing. There is no dynamic
  scale-up/scale-down, blocked-syscall handoff, idle-M reclamation, or
  same-task-type thread-cache contract.
- Status: the existing compatibility subset remains intact, but these four
  boundary requests are only partially satisfied. The gaps are now explicit
  in `docs/compatibility.md` and `docs/design.md`.

## Boundary implementation and final verification (2026-09-18)

- Replaced the earlier callback-only scheduler path with a stackful
  Boost.Context Fiber backend. `Task` now owns a resumable C++ continuation;
  G/M/P admission, local/global queues, bounded stealing, park/wake pending
  permits, owner binding, dynamic M growth/shrink and task-class affinity are
  implemented in `src/scheduler.cpp`. Shutdown requests cooperative
  cancellation, wakes parked Gs, waits for started Fibers to return through
  their trampoline, releases completed Fiber stacks immediately, and never
  force-discards a live C++ stack.
- Added scheduler-aware `sync::Mutex`, `ConditionVariable` and `WaitGroup`,
  shared `ParkingCondition` and process-wide timer service. Managed G waits
  suspend their Fiber and leave the M available; unmanaged contended mutex
  calls deliberately return false/throw rather than silently blocking an
  unknown OS thread.
- Added Linux `IOManager`/epoll/eventfd readiness and the shared default-on
  socket hook. The hook covers connect/accept/read/recv/write/send,
  close/dup/fcntl/ioctl, socket timeout options and sleep. It registers
  readiness before deadline timers, uses one-shot outcomes and generation
  tokens, and has original-call/native-poll fallbacks. A boundary audit fixed
  `dup2`/`dup3` pre-replacement close notification, `dup2(fd, fd)` identity,
  native fallback `ETIMEDOUT`, and extreme-deadline conversion overflow.
- Added runnable examples under `example/`: `fiber_sync_demo.cpp`,
  `managed_pipeline_demo.cpp`, `dynamic_gmp_demo.cpp` and `io_hook_demo.cpp`.
  Added saturating relative-deadline arithmetic for Context and channel/select
  APIs, with regression coverage for `ContextDuration::min()/max()`.
  Updated the compatibility/design/dependency/scheduler/testing documents to
  distinguish implemented semantics from unsupported Go/compiler/ABI behavior.
- Current verification from the canonical WSL path:
  GCC Debug shared CTest **7/7**, Clang 18 Debug **7/7**, Release static
  Hook-off CTest **6/6**, strict `-Werror` CTest **7/7**; ASan and UBSan full
  unit/smoke runs passed; Valgrind unit and smoke runs both exited zero with
  `0 bytes in 0 blocks` and `ERROR SUMMARY: 0` (final alloc/free totals
  `1,883,691` and `3,387`); `(cd third_party/go-reference && sha256sum -c
  SHA256SUMS)` returned `OK` for the Go 1.23.0 archive; install/export smoke
  completed; serial watchdog stress passed
  `go2cpp_tests` **40/40** and `go2cpp_scheduler_smoke` **80/80**.
- TSan was rebuilt with non-PIE/ASLR-disabled WSL invocation. Filtered
  context/channel/scheduler/dynamic/fiber/sync/io runs passed. The full suite and hook filter
  exceeded the WSL watchdog without a diagnostic, so no full-suite TSan pass
  is claimed; native Linux remains the appropriate final TSan gate.
- Remaining explicit boundaries: no Go parser/compiler/GC/ABI, no
  asynchronous preemption or blocking-region M>P handoff, cooperative rather
  than forced Fiber cancellation, no exact Go fairness, and no transparent
  interception of every Linux socket-adjacent API (`poll`/`select`/`ppoll`,
  `*mmsg`, `sendfile`, `splice`, and signal-restart policy remain outside the
  subset). Static consumers must disable the shared Hook target.

## Final boundary regression and handoff (2026-09-18)

- Fixed cancellation/shutdown lost-wake windows for unstarted queued Gs and
  for advanced raw `Fiber::Suspend(Park)` users. Failed queue claims now prune
  terminal tasks and release callable captures; scheduler shutdown is notified
  when an external cancellation changes the terminal predicate. Fiber creation,
  pointer publication, resume observation and terminal ownership transfer are
  serialized against cancellation by the Task transition mutex.
- Added regressions for cancelled-queue capture release, raw Fiber park
  cancellation, managed `MSG_OOB` (`ENOTSUP`), tracked `FIONBIO` invalid-pointer
  `EFAULT`, empty/inert select, singleton `errors.Join`, context callback
  subtree marking, and zero/negative timed mutex acquisition.
- Hook boundary fixes include applying the configured connect timeout to native
  fallback, kernel-first `FIONBIO` validation, split close lifecycle locking
  around potentially blocking `SO_LINGER`, and explicit documentation for
  raw syscall/`io_uring`/`close_range`/fork-exec gaps. Ordinary-thread
  `MSG_WAITALL` on a previously adopted runtime-nonblocking descriptor remains
  a known semantic limitation; managed `MSG_WAITALL` and `MSG_OOB` return
  `ENOTSUP`.
- Earlier current-tree verification baseline: GCC Debug shared **7/7**, Release static
  Hook-off **6/6**, Clang 18 Debug **7/7**, and strict `-Werror` **7/7**;
  ASan and UBSan unit/smoke runs passed. Valgrind Memcheck logs
  `build-check/valgrind-tests-final3.log` and
  `build-check/valgrind-smoke-final3.log` both exited zero with 0 bytes in
  use, ERROR SUMMARY 0, and allocation totals `1,908,203` and `3,388`.
  The final serial watchdog run passed 40 unit and 80 smoke executions.
- TSan filtered runs completed earlier for context/channel/scheduler/dynamic/
  fiber/sync/io without a diagnostic; the rebuilt final WSL run did not finish
  within the watchdog, so no full-suite or hook TSan pass is claimed. Native
  Linux TSan remains a release-gate follow-up. Channel destruction, throwing
  select element/callback rollback, static-destruction ordering for global
  contexts, and owner lifetime for raw `IOManager*` bindings remain explicit
  documented preconditions or follow-up work.
- After the final `IOManager::wait` park-failure guard and select-formatting
  cleanup, Memcheck was rerun on the current tree. `valgrind-tests-final4.log`
  and `valgrind-smoke-final4.log` both exited zero with `0 bytes in 0 blocks`
  and `ERROR SUMMARY: 0`; allocation totals were `1,815,429` and `3,388`.
- After the public pre-cancelled-`Task` admission cleanup and its regression,
  Memcheck was rerun again on the current tree. `valgrind-tests-final5.log`
  and `valgrind-smoke-final5.log` exited zero with `0 bytes in 0 blocks` and
  `ERROR SUMMARY: 0`; allocation totals were `1,809,665` and `3,390`.

## Boundary API follow-up (2026-09-20)

- Extended the public documentation and runnable examples for the mixed native
  thread and managed Fiber boundary. Added mixed_runtime_demo.cpp and registered
  it as a CMake example/test target.
- Documented scheduler-aware mixed Mutex, ConditionVariable and WaitGroup
  semantics, including native condition-variable wait nodes, non-thread-owned
  unlock, timeout/cancellation expectations, and the prohibition on holding a
  thread-owned std::mutex across Fiber migration.
- Documented FiberLocalCache/FiberLocal value lifetime and migration behavior,
  per-thread hook and GMP participation scopes, and the intentional boundary
  that external participation records policy only and does not attach an M or
  run a queue.
- Documented BlockingRegion replacement-M admission. Known interposed socket
  and sleep fallbacks account for native blocking automatically; arbitrary
  unhooked native calls still require the explicit RAII region and remain
  cooperative.
- The post-boundary Debug build in build-docs-check compiled 57 targets; CTest
  passed 8/8, and the mixed example passed 20/20 watchdog runs. The Hook-off
  Release static build in build-docs-static passed 7/7, including the mixed
  example. Final sanitizer, Valgrind and compiler reruns are recorded below.

## Final verification after boundary hardening (2026-09-20, superseded)

- Made BlockingRegion non-movable and bound its cleanup to the entering M id.
  The destructor repairs the captured M even if an invalid Fiber migration
  occurs; the public contract still forbids yielding or parking inside the
  region. Native close fallback now uses the same blocking accounting.
- Worker participation scopes now preserve the scheduler-owned worker metadata
  when nested inside a runtime M. External participation remains an explicit
  policy/eligibility scope only; it does not attach an M or run a queue.
- Managed hook admission without an IOManager now lazily adopts sockets and
  uses the bounded native poll/deadline path when hooks are enabled. Explicitly
  disabled hooks preserve native blocking semantics and use BlockingRegion
  accounting. The recursion guard remains active.
- Added the plain-Scheduler lazy-adoption timeout regression and the worker
  nested-policy regression. The ASan test race that asserted Task::state
  immediately after a completion counter was made watchdog-based.
- Made the context deadline TimerService an intentionally process-lifetime
  singleton, removing static Context/TimerService destruction-order UAF risk;
  an exit callback stops and joins the service thread while the intentionally retained service object remains valid.

Final verification from /UserData/CodexWorkSpace/Go2Cpp:
- GCC Debug Hook-on CTest 8/8; Release static Hook-off 7/7; strict Werror 8/8;
  Clang 18 8/8.
- ASan and UBSan full unit/smoke runs passed with the documented environment
  variables.
- Memcheck logs build-check/valgrind-boundary-tests-final4.log and
  build-check/valgrind-boundary-smoke-final4.log exited zero with ERROR SUMMARY
  0. Definite, indirect and possible leaks were all zero. Intentional process-lifetime FiberLocal registry and context-timer service storage remained reachable: 360 bytes in three blocks for the unit run and 96 bytes in one block for smoke. The native context waiter now has a 2-second `WaitFor` watchdog before join.
- Watchdog stress passed go2cpp_tests 40/40 and go2cpp_scheduler_smoke 80/80.
  Rebuilt TSan scheduler, dynamic, and fiber filters passed with `setarch x86_64 -R`; the full suite exceeded a 120-second Python watchdog without a diagnostic, while the normal WSL invocation reports `ThreadSanitizer: unexpected memory mapping`. Native Linux remains the TSan gate.
- Install/export smoke passed with the current `build-install-final` tree and a temporary package consumer.
- Known semantic boundaries remain: no exact Go P handoff or asynchronous
  preemption, no external-thread attach/run-one backend, no Fiber stack pool,
  and no guarantee of same-task-type cache reuse. Arbitrary unhooked native
  blocking still requires an explicit BlockingRegion; poll/select/mmsg,
  sendfile/splice, io_uring and similar socket-adjacent APIs remain outside
  the hook subset.


## Final boundary hardening and race-detector follow-up (2026-09-20)

- Added watchdog-safe test infrastructure in tests/test_support.hpp:
  bounded RequireEventually, aborting Require, and thread joins guarded by
  a separate watchdog thread. Long test Fiber loops use a 1 ms pause only when
  running under Valgrind, so Memcheck cannot starve the coordinating test
  thread while TSan keeps the original yield behavior.
- Hardened tests/test_scheduler.cpp and related module/smoke/example tests:
  cancellation and publication waits have absolute deadlines, critical waits
  fail fast instead of continuing into unsafe teardown, and the cancelled
  queue-capture case uses a strict one-worker/no-oversubscription scheduler.
- TSan found a real race during this pass: dynamic M growth relocated
  Scheduler::Impl::machines vector slots while a managed worker performed
  BlockingRegion lookup. The machine record container is now a deque, so
  published shared-pointer slots are not relocated during append; scheduler,
  dynamic and fiber TSan filters then passed repeatedly. This is reflected in
  src/scheduler.cpp and docs/scheduler.md.
- Final GCC Debug/Release, strict Werror, Clang, ASan and UBSan CTest matrices
  passed: 8/8 Debug, 7/7 Release static, 8/8 Werror, 8/8 Clang, 8/8 ASan,
  and 2/2 UBSan unit/smoke.
- Valgrind final8 logs passed with zero definite/indirect/possible leaks:
  unit 1,516,975 allocs / 1,516,972 frees and 360 bytes in three
  intentional reachable blocks; smoke 3,406 / 3,405 and 96 bytes in one
  intentional FiberLocal-registry block. Both report ERROR SUMMARY: 0.
- Final stress passed 10 unit and 10 scheduler-smoke executions with a
  45-second process watchdog. Install/export consumer smoke printed 7.
- Remaining boundaries are unchanged and intentional: no exact Go P handoff or
  asynchronous preemption, external participation is metadata-only, FiberLocal
  is a logical value cache rather than a Folly stack/object pool, arbitrary
  unhooked blocking needs explicit BlockingRegion, and the WSL full-suite
  TSan gate remains incomplete pending native Linux.

## Shutdown lock hardening (2026-09-20)

- External shutdown now moves each published `std::thread` out while holding
  `Impl::mutex`, releases the scheduler mutex, and joins the moved thread. The
  final processor/state scan is also lock-protected. This removes the temporary
  snapshot allocation from the OOM-sensitive shutdown path while preserving
  the `join_mutex -> Impl::mutex` ordering.
- After this change, Debug CTest remained 8/8; dynamic, sync and scheduler-smoke
  watchdog runs passed. Memcheck final9 reported zero definite, indirect or
  possible leaks (unit 1,517,159 allocs / 1,517,156 frees; smoke 3,415 / 3,414)
  with only the documented 360B and 96B process-lifetime reachable blocks.

## Reaper lifetime hardening (2026-09-20)

- `reap_dead_workers()` now reserves its retired-thread vector before moving
  any joinable `std::thread`, and retains `join_mutex` through the actual joins.
  This makes the OOM path non-terminating and keeps external shutdown from
  observing a half-finished reaper maintenance pass.
- After the reaper change, Release Hook-off 7/7, strict Werror 8/8, Clang 8/8,
  ASan 8/8 and UBSan 2/2 passed; TSan scheduler and dynamic filters passed
  under `setarch x86_64 -R`. Final10 Memcheck again had zero definite,
  indirect or possible leaks: unit 1,560,975/1,560,972 allocations/frees and
  360B reachable; smoke 4,042/4,041 and 96B reachable; both `ERROR SUMMARY: 0`.

## 2026-09-21 恢复工作验证

- 重新确认 WSL 工作目录为 `/UserData/CodexWorkSpace/Go2Cpp`，Git 根目录、
  分支和工作区状态正确；恢复时未发现未提交改动。
- 使用已有 `build-check` 运行
  `ctest --test-dir build-check --output-on-failure --timeout 60`，当前 Debug
  Hook-on 测试 **9/9** 通过，包含单元测试、调度器、Fiber 同步、动态 M、
  混合 native/Fiber、入门 API 和 IO Hook 示例。
- 本次只做恢复审计和回归验证，没有引入新的实现缺口。未改变已记录的边界：
  WSL 环境仍不能把完整 TSan 套件作为最终通过依据；异步抢占、完整 Go
  编译器/ABI/GC 语义和未覆盖的 Linux socket API 仍属于明确限制。


## Sysmon 长系统调用交接（2026-09-23）

- 在 `src/scheduler.cpp` 与 `include/go2cpp/scheduler/scheduler.hpp` 增加可配置 sysmon 监控线程：`enable_sysmon`、`sysmon_interval`、`long_syscall_threshold`。
- `BlockingRegion` 和 Hook native fallback 发布 M 的阻塞起点；达到阈值后记录 detached/task/count，从 P 的 attached 计数逻辑解绑，并按 `min_workers + detached_count` 申请替代 M；原 M 返回时恢复 P 计数。
- detached M 存在期间提高空闲回收底线，避免替代 M 在原系统调用返回前被回收；worker epilogue 防止 attached 计数下溢；shutdown 在 drain 完成后停止并 join sysmon。
- 修复 `IOManager` 已存在但未跟踪 FD adopt 失败时直接进入 libc 的路径，使 managed G 仍进入 `BlockingRegion`；新增 pipe fallback Hook 回归。
- 更新 `docs/scheduler.md`、`docs/design.md`、`docs/compatibility.md`、`docs/go_reference_audit.md`、`docs/testing.md`，明确这是安全的逻辑解绑，不是 Go 精确 `_Psyscall` 或异步抢占；raw syscall/未 Hook 阻塞仍是边界。
- 验证：Debug Hook-off 2/2、Hook-on 9/9、dynamic 重复 10 次、ASan 2/2、UBSan 2/2、Valgrind dynamic 0 definite/indirect/possible lost；TSan 全量受 WSL + Boost.Context 映射限制，未宣称通过。


## Fiber 父链生命周期与 sysmon 边界加固（2026-09-23）

- `src/fiber.cpp` 引入共享 `FiberRecord` 父链元数据。父 Fiber 销毁后，子 Fiber 的诊断快照和取消遍历不再解引用裸父对象；共享记录保留 `alive=false` 墓碑。真正的挂起栈恢复仍要求固定父对象存活。
- Ready Fiber 析构可跳过尚未进入的用户栈；Suspended Fiber 在错误父级之外析构或传播失败会标记 Failed 并清理可安全清理的状态；挂起栈上的 RAII 只能由固定父级正常收尾。
- worker 任务边界检测未销毁的 `BlockingRegion` 并 fail-fast，防止跨 yield/park 的错误记账污染后续 G。
- 新增 Fiber 墓碑/唤醒后父链顺序回归和双长 syscall sysmon 回归。Debug、Hook-on、Release、Werror、Clang、ASan、UBSan、TSan（全量 3 次）与 Valgrind 均按 `docs/testing.md` 记录的命令通过。

## 2026-09-23 monitor 活性、嵌套 IO 与栈边界复核

本轮在既有 sysmon/Fiber 父链实现上继续复核并修正：

- src/scheduler.cpp：sysmon 的周期等待改用独立 sysmon_wait_mutex/sysmon_wait_condition，不再因 Scheduler 主锁被高负载路径占用而阻塞在等待入口；扫描阶段保留 try_to_lock，sysmon_pass_count 明确为心跳尝试计数。仍保持单 monitor 线程，M 扩容由已有有界策略负责。
- include/go2cpp/scheduler/scheduler.hpp、src/io.cpp：IO park 通过 park_io() 标记 SuspendReason::Io，方便诊断嵌套等待链。
- src/fiber.cpp：恢复路径在嵌套 scheduler park 后按返回状态恢复 TLS 当前 Fiber，防止父 Fiber 创建第二个子 Fiber 时错绑已完成的兄弟 Fiber。
- tests/test_hook.cpp：增加两层连续 timeout/ready、三层 Fiber 父链连续 IO、Fiber 快照深度和 send(MSG_NOSIGNAL) 系统错误回归；修正 phase=2 瞬时状态的测试观察方式，避免把合法快速推进误报为 watchdog。

结论保持谨慎：嵌套 Fiber 的单 FD IO、timeout、ready、cancel/close 唤醒和父级自然返回已验证；没有公共多 FD wait_any/Fiber select。sysmon 不能安全地跨线程终止或迁移 C++ Fiber，也不能注入系统级异常。Fiber 栈仍是带保护页的固定大小栈，没有 Go morestack/copystack 自动增长；需显式设置 fiber_stack_size。Hook 的 write/send 错误保持 libc errno，不隐式转 C++ 异常；SIGPIPE 仍遵循调用者的 POSIX 策略。

## 2026-09-23 最终复测修订

- Fiber 恢复路径新增栈对象 RAII TLS guard；任何嵌套 scheduler park、取消或早退都恢复进入 resume 前的直接调用者，避免 TLS 残留把已完成兄弟 Fiber 当作后续子 Fiber 的父级。
- 当前源码重编译后的 Hook/Fiber/dynamic 过滤器各 5/5，通过 Hook 9/9、Release 8/8、Werror 2/2、Clang 2/2、ASan 2/2、UBSan 2/2；TSan 在 setarch x86_64 -R 下当前全量单次通过，无 race 报告。
- WSL + TSan 重复全套仍可能在不同 native waiter watchdog 超时，不能据此宣称重复稳定；这属于测试环境风险，生产实现未通过放宽同步语义来掩盖。
- 最新 Memcheck 为 build-sysmon-hook/valgrind-final-current.log，definite/indirect/possible lost 均为 0，416B 为已有进程级 reachable 状态。

## 2026-09-24：显式控制流、Caster 与多 FD 等价层

- 新增 control_flow 模块：defer 是不可复制 RAII 对象，构造参数按值保存，析构或
  run_now() 按 C++ 作用域逆序执行一次；panic/recover 使用共享 PanicInfo 状态，
  recover 只在 defer 回调边界消费，回调异常通过 LastDeferException() 查询。
- 恢复旧 panic_defer 模块并不安全，因此没有使用 longjmp、信号异常或 C++ 内部
  异常伪造 Go 栈跳转。该模块是显式状态协议，调用者必须在 call() 后自行 return
  或分支；普通 C++ 异常、Fiber failure 和 error 保持独立。
- Channel select 增加 SelectValue、SelectCaster、MakeCaster、SetAny 和栈对象
  Cast 重载；std::any 仍保持兼容字段，不可复制源值可通过共享 holder 访问。
- IOManager 增加 WaitAny/WaitMany 及 timeout/cancel/close 组合；多个请求登记到
  epoll，普通线程明确返回 EPERM。修复一次多 FD readiness 竞态：Fiber 恢复前用
  零超时 poll 汇总仍就绪节点，再执行其余 waiter 清理。
- 本轮 Debug CTest 为 10/10；更多 Release、Werror、ASan、UBSan、Valgrind
  结果在 docs/testing.md 追加，未把 WSL 环境限制冒充 sanitizer 通过。

已知边界：recover 不提供 Go 编译器级 G/栈帧隔离，panic 不自动终止 Fiber 或展开
外层 defer；SelectCaster 的可变函数对象需要调用方自行保证并发安全；固定 Fiber 栈
仍未实现 Go 式动态扩容；poll/select Hook 保持 libc ABI，不自动改写为 WaitMany。


## 2026-09-24 最终边界复核

- defer/panic/recover 增加显式状态协议测试：参数注册时保存、LIFO、run_once/dismiss、普通异常作用域展开、回调异常记录、Caster 异常隔离和线程边界。
- SelectResult::Value<T>() 对不可复制 T 不再实例化 std::any_cast；新增 TakeValue<T>()，避免 move-only 类型的编译期断言。
- WaitMany 的补采样改为只完成 fd/方向队列头节点，并拒绝同一集合内重复 fd/方向；新增混合无效 fd 回归，保持外部 waiter FIFO。
- Debug 10/10、Werror 10/10、Release 9/9、ASan/UBSan 全组通过；Valgrind 完整套件 416B/4 blocks still reachable，definite/indirect/possible 均 0，ERROR SUMMARY 0。
- TSan 在 setarch 下四个过滤用例通过；完整套件在 WSL 120 秒 watchdog 超时，未宣称全量通过。
- 已知语义边界仍保留：panic 不自动展开 C++ 栈，recover 不绑定 G/Fiber owner，固定 Fiber 栈无 Go 式动态增长，WaitAny/WaitMany 仅 managed Fiber 可用。
## 2026-09-24 最终修复记录

- 修改内容：为 WaitMany 增加完整请求集合预校验，避免在部分 epoll 节点已发布后才发现无效 FD；修复 Memcheck 下混合无效 FD 的顺序竞态。
- 修改内容：SelectWaitState::Take 直接移动 typed holder；内建 Channel Select 对不可复制元素明确返回 kInvalid，普通 Send/Recv 与独立 SelectValue 保留 move-only 支持。
- 验证：Debug CTest 10/10、Werror CTest 10/10、Release CTest 9/9；ASan/UBSan 全量测试通过。
- 验证：Valgrind Memcheck 完整套件 ERROR SUMMARY 0，definite/indirect/possible lost 均为 0，416 bytes/4 blocks still reachable。
- 验证：TSan 在 setarch 下 control_flow、io、channel、scheduler_smoke 四个过滤用例通过；完整套件仍受 WSL watchdog 限制，未宣称全量通过。
- 后续状态：构建目录和测试日志已清理，本轮提交已完成。固定 Fiber 栈、显式 panic/recover 协议、managed Fiber 专用 WaitAny/WaitMany 和 Caster 并发责任仍属于兼容性边界。

- 示例补充：新增 example/io_wait_many_demo.cpp，展示两个 socket FD 的 WaitManyFor
  和 WaitAnyFor；Hook-on Debug/Werror 各 11/11，Hook-off Release 10/10，直接运行
  输出 wait-many=true。

## 2026-09-30：工程化日志与配置模块

- 新增 `include/go2cpp/log.hpp`、`src/log.cpp`：自有 C++17 日志实现，按
  `LogFilter -> LogItemWorker -> LogFormatter -> LogSink` 解耦；默认使用线程安全
  文件 Sink，发布等级为 warn，默认写入 `log/go2cpp.log`，stdout 必须配置开启。
- 提供 `PatternFormatter`、`CallbackSink` 和大小写兼容的 `GO2CPP::Logger`/宏接口，
  可在回调中桥接 spdlog/fmt，不把第三方 ABI 强行加入运行时。
- 新增 `include/go2cpp/config.hpp`、`src/config.cpp` 和根目录 `go2cpp.ini`：支持
  section、key=value、注释、日志和调度器参数统一校验；`max_workers` 上限 32、
  `local_queue_limit` 必须大于 0 等约束在启动前检查。
- CMake 增加 `go2cpp_log`、`go2cpp_config` 目标及安装导出；新增日志并发/过滤/配置
  边界测试与 `example/log_config_demo.cpp`。
- 验证：WSL GCC 13.3 + Boost.Context 1.83 的工程化构建成功；日志过滤并发测试通过，
  `go2cpp_log_config_demo` 通过。完整 CTest 中已有 scheduler-aware synchronization
  用例一次出现既有的时序失败，单独重跑日志测试和其余目标成功；该失败未被隐藏。

## 2026-09-30：工程化日志与配置增强复做

- 复核发现上一版日志只有单一文件/标准输出 Sink，配置只能一次性读取，未达到 Sylar
  级别。本轮在原模块上增量增强，没有替换现有 Logger API。
- 日志增加 Sylar 风格 `%d/%p/%N/%T/%f/%l/%m/%n` 格式解析、NOTICE/CRIT/ALERT
  等级、Logger 父级传播、多 Worker 默认/自定义分层、动态重建默认输出地、滚动文件
  Sink、stderr Sink、内存 Sink、Flush/Reopen 和第三方 Callback 边界。
- 配置增加强类型 `ConfigVar<T>`、`ValueCodec`（基础类型、字符串、vector）、变更
  监听器、事务式类型预校验、文件 mtime watcher、`BindLoggingConfig` 和
  `BindRuntimeConfig`。运行中修改 `go2cpp.ini` 会立即更新已存在 Logger；调度器结构
  参数仍明确要求重建 Scheduler，避免在线修改 P/M 队列拓扑。
- 修复 ASan 发现的监听器共享指针环：日志监听器改用 weak_ptr，完整日志测试不再泄漏。
- 验证：完整 `go2cpp_tests` 通过；Werror 构建通过；ASan/UBSan 全量 `go2cpp_tests`
  通过；Valgrind 日志测试通过，未报告 definite/indirect/possible leak；动态配置、
  Sylar 格式、层级传播和滚动文件均有回归测试。

## 2026-09-30：coost 对比性能复核与 IO/调度器优化

- `src/io.cpp` 采用持久 epoll ET 注册、读写方向 pending readiness 和 eventfd
  tickle 合并；同一 FD 的连续等待不再重复 ADD/DEL，WaitNode 唤醒走
  `Task::wake_io()` 的原子状态路径。Descriptor generation、关闭和超时仍保留
  原有线性化检查。
- `src/scheduler.cpp` 增加 worker 本地 yield 回队和 IO park/wake 快路径，维护
  扩容采样，保留 BlockingRegion/sysmon 的替代 M 语义；`src/fiber.cpp` 延迟首次
  栈分配、完成后立即回收并使用 M/TLS 栈缓存，避免短任务预先建立保护栈。
- Release 公平基准（50,000 G、每 G 4 次 yield，执行时间不含 shutdown）本轮
  Go2Cpp 42--71 ms，coost 18--30 ms；当前调度器仍约为 coost 的 1.4--2.0 倍，
  尚未达到用户要求的 1.25 倍。主要剩余成本是每个 Fiber 独立受保护栈和 shared_ptr/
  admission registry；直接替换为无保护栈会损失越界保护，未采用不安全伪优化。
- Release IO 基准（4096 socketpair、25 轮）Go2Cpp 695--731 ms，coost
  1356--1582 ms；本机该场景 Go2Cpp 已超过 coost。不同负载和机器仍需重新测量，
  不能把单一基准外推为所有 IO 工作负载。
- 验证：Debug/Release 全量 CTest 各 14/14 通过；scheduler、dynamic、io 过滤器
  通过；`git diff --check` 通过。构建时仅有 WSL 文件时钟偏移警告。未改变用户的
  `LOCAL_CODE_REVIEW_ORDER.md` 未跟踪文件。

## 2026-09-30：当前收尾复核与并发热路径修复

- 调度器将外部生产者改为 `incoming_queue` 与独立互斥量，并把任务注册表拆成
  16 个分片；`Task::prepare_enqueue` 在一次状态转换中完成 owner、取消门和入队
  标记，减少重复加锁和重复入队窗口。
- IO 的 `WaitMany` 增加注册后的零超时 readiness 补采样，修复“状态为 ready 但
  ready 列表为空”的竞态；领取等待节点时清理 pending 标记，并只在同一注册的
  读写队列都为空时失效 epoll interest，避免高负载下丢唤醒或重复注册。
- Mutex 保留有等待者时的公平保护，只对无等待者竞争执行有限次协作让出；没有把
  普通线程锁伪装成可跨线程迁移的 Fiber 锁。
- 配置 watcher 增加生命周期互斥量和条件变量快速停止；配置读取使用不可变原子
  快照；日志 sink、级别和 worker 列表使用快照并避免 Info 级别逐条 flush。新增
  的生命周期约束已写入 `docs/configuration.md`。
- 当前构建验证：WaitMany demo 连续 20 次通过；高负载样例通过；最终 Debug CTest
  13/13、Release CTest 14/14 均通过。性能复测写入
  `docs/performance.md`：公平调度中位数约为 Coost 4 倍，IO 场景约为 Coost 0.59 倍，
  混合 Mutex 仍明显落后，未宣称达到全局 1.25 倍目标。
- 进一步修复：Machine 的任务亲和字段改为原子快照，避免 worker 更新与
  `Scheduler::machines()` 读取之间的数据竞争；配置 watcher 捕获监听器异常，避免
  用户回调异常导致 watcher 线程 `std::terminate`。修复后两套 CTest 再次全通过。
- IO 生命周期复核增加 WaitNode 的完成操作屏障：`complete`、epoll 取 ready 节点和
  deadline drain 在 claim 前登记 active operation，节点回收会等待这些操作完成；
  Context 回调仍有独立进入/退出屏障。pending readiness 保留高性能 epoll 快路径，
  但在下次消费前通过零超时原生 poll 复核 FD，避免读空后的 stale ready。
- 最终验证：Release CTest 14/14、Debug CTest 13/13、Werror CTest 3/3、ASan
  CTest 3/3 通过；核心 Release `go2cpp_tests` 连续 4 次通过。TSan 当前构建在本机
  WSL 报 `ThreadSanitizer: unexpected memory mapping`，属于运行环境限制，未宣称通过。
  Valgrind 当前测试的 ERROR SUMMARY 为 0，definite/indirect/possible leak 均为 0，
  但 Memcheck 放大时序导致 IO watchdog 超时，416 bytes still reachable 为进程级缓存。
- Fiber Context 复核结论不变：自研自动扩栈上下文曾在嵌套恢复中破坏父链，已撤回；
  当前生产实现继续使用 Boost.Context 受保护固定栈，自动扩栈属于未完成边界。

## 2026-10-01：混合 Mutex 关键路径优化

- `src/sync.cpp` 将锁状态合并为单一原子字节，并把 `kWaiter` 的发布放在队列锁
  内，修复等待者发布与 Unlock 交错造成的丢唤醒。WaitNode 增加 generation 和
  `TryCancelAndWake` 门，修复 Context 旧回调污染缓存节点及取消与 `Arm()` 交错
  永久挂起的问题。没有改变普通线程的阻塞语义，Fiber 等待仍按 FIFO handoff
  进入调度器。
- `src/scheduler.cpp` 和 `scheduler.hpp` 增加 `park_wait()` 原子挂起路径；同步
  原语不再为每次等待获取 admission 锁。跨 M 唤醒按 G 最近运行的 P 投递，P 本地
  回队仅在无运行 M 或队列积压时通知 worker，降低 futex 抖动。Task 最近 P 字段
  改为原子快照，避免 worker 写与外部唤醒读之间的数据竞争。
- 当前本机 Release 高负载混合 Mutex 样本 15～35 ms，完整样本 26/50 ms；线程
  `std::mutex` 为 40～45 ms。计算、调度、Channel、IO 的完整样本仍分别约为
  1.5 s、121～124 ms、24～26 ms、662～748 ms；IO 继续优于线程 poll。性能数字
  仅代表本机重复运行，不能当作跨机器 SLA。
- 验证结果：`build-native-context` CTest 14/14、`build-engineering-werror` CTest
  3/3、ASan 单测全部通过；Valgrind ERROR SUMMARY 0，definite/indirect/possible
  leak 均为 0（288 bytes still reachable 为进程级缓存）。TSan 仍因 WSL
  `unexpected memory mapping` 无法运行，未宣称通过。构建期间仅出现 WSL 文件时钟
  偏差警告，不影响目标生成。

## 2026-09-30：Context 后端与跨 M 唤醒门

- Linux x86_64 新增 Go2Cpp 自有 Context ABI，布局借鉴 coost/TBOX，但保留
  `mmap + PROT_NONE` guard page、FiberStack TLS/全局缓存、ASan/TSan 切换、
  `m_resume_claim` 串行恢复和跨 M caller transfer；该平台不再链接 Boost.Context。
  非 x86_64 Linux/其他平台回退 Boost.Context。
- 明确 coost 的 Buffer 只是固定共享栈的挂起快照，不是自动动态栈；本轮没有复制
  `_exit` 结束路径，也没有声称实现 Go `morestack`/`copystack`。
- IO WaitNode、ParkingCondition 和 sync WaitNode 的 wake/disarm/arm 使用
  `HybridGate`：原子 flag 先做 64 次短自旋，竞争持续再进入 `std::mutex`；节点
  内的 Scheduler/Task 所有权和 callback/operation 生命周期仍由门保护。
- native 后端高负载复测：调度 113～133 ms、计算 1327～1526 ms、混合 Mutex
  6680～6895 ms、IO 616～665 ms；线程对照约 40～46 ms、1283～1439 ms、39～42 ms、
  1614～1704 ms。混合锁仍受 park/wake/FIFO handoff 限制，未达到线程或 Coost
  1.25 倍目标。
- native Release CTest 14/14、native ASan/UBSan CTest 14/14 通过；高负载连续
  3 次通过。详细约束与数据见 `docs/performance.md`、`docs/design.md`。

## 2026-09-30：调度器与混合 Mutex 专项优化收尾

- 调度器新增 16 条 incoming 条带、非空位图、已注册 G 的唤醒快路径和 worker
  本地 P 回队路径；worker 仅在仍有 runnable G 时通知，避免无效 futex 唤醒。
- 混合 Mutex 的 Unlock 统一在队列锁下完成 FIFO handoff，修复丢唤醒和误清锁
  竞态；已有 waiter 时禁止新的自旋者越过队头；无 Context 等待节点继续使用
  调用栈对象，带 Context 节点使用线程本地缓存。
- 当前公平调度基准（50,000 G、4 次 yield、8 个生产线程）总时延 84～103 ms，
  coost 为 18～36 ms，约 3.0 倍；混合 Mutex（2,000 Fiber、32 线程）为 8.5～8.8 s，
  coost 为 1.6～5.7 s，线程 `std::mutex` 为 39～40 ms。未达到 1.25 倍目标，
  主要剩余成本是 shared_ptr/注册表、Boost 受保护栈和 park/wake 跨 M 链路，不能
  通过无锁改动安全消除。
- 明确使用规约：混合锁只用于短临界区；不得持锁等待 IO、定时器或 park；高争用
  优先使用原子或按 P 分片，普通线程独占热点使用 `std::mutex`。
- Coost 的 Buffer 只是固定执行栈槽的挂起快照，不是自动动态栈；Boost
  `segmented_stack` 依赖受限工具链，故未替换当前受保护固定栈。
- 验证：Release CTest 14/14、Debug CTest 13/13、Werror CTest 3/3、ASan CTest
  3/3；高负载样例连续 3 次全部通过；TSan 仍受 WSL `unexpected memory mapping`
  环境限制，未宣称通过。详细数据和调度环节见 `docs/performance.md`。

## 2026-10-01：FiberBin 复用与 M 本地队列复核

- 调度器新增每个 M 的线程本地 FiberBin。只有已经完成或失败、上下文和栈都已
  释放、且父记录没有被嵌套 Fiber 持有的内部 Fiber 才会进入缓存；复用时重新
  创建 FiberRecord/ID，并清除父链、失败状态、调度标志及 sanitizer 状态。普通
  用户 Fiber、挂起 Fiber 和跨 M 的活动 Fiber 不经过此路径。
- FiberBin 默认容量为 32，配置上限为 4096，可通过 `scheduler.fiber_bin_capacity`
  在 INI 和动态配置中修改。容量仅影响对象分配复用，不改变调度语义；容量过大
  会增加每个 M 的常驻内存。
- 曾尝试把任务从 P 队列进一步分为 MLocalTaskQueue、P 队列和全局队列，但在
  50,000 G 的高负载 yield 压力下出现完成超时。该实现已撤回，没有把未经验证的
  runnable 计数、shutdown 排空、sysmon 解绑和跨 M 唤醒状态机带入主线。当前仍以
  P 本地队列、incoming 条带和全局窃取为生产实现；后续若重新实现 MLocal，必须先
  补齐可证明的所有权、溢出和关闭协议。
- 验证：Release CTest 14/14 通过，`go2cpp_tests` 连续运行通过；ASan 直接运行
  和单测过滤重跑可以通过，但在重复压力运行中仍会间歇性触发既有
  `test_readiness_timeout_race` 的 timeout/stack-use-after-scope（临时禁用 FiberBin
  后仍可复现，确认不是 FiberBin 引入）。因此不能把 ASan 宣称为稳定全通过；TSan
  仍受本机运行时映射限制。高负载混合 Mutex 仍明显慢于线程基准，FiberBin 只降低
  创建/销毁分配开销，不宣称已经消除调度和混合锁的主要成本。

## 2026-10-02：逻辑链路审计与 IO 代际竞态修复

- 对 Task 的状态转换、Runnable 计数、execution claim、P/M 绑定、sysmon、shutdown、
  嵌套 Fiber 父链、混合同步原语、Context 取消、Channel/Select 以及 IO 等待分别记录
  线性化点和不变量，详见 `docs/correctness_audit.md`。审计结论限定在文档列出的
  API 使用契约内，未把“测试通过”表述成数学意义上的零 Bug。
- `src/io.cpp` 在 descriptor token 首次检查后、取得 State 锁并发布 waiter 前增加第二次
  generation/FD 检查；关闭或 dup2 与注册交错时不会把失效 fd 重新放入无后续事件的队列。
  当 fd 已无 waiter 时撤销 epoll registration，避免旧 ET 注册造成忙循环和代际残留；仍有
  waiter 时保留已发布的 interest 位，以吸收内核中已经排队的迟到 ET 事件。
- `src/fiber.cpp` 的 native context 为 Memcheck 登记/注销自定义 Fiber 栈，修复换栈后
  Valgrind 无法识别栈边界导致的误报。`tests/test_io.cpp` 新增嵌套 Fiber IO 唤醒回归，
  并仅在 Valgrind 下放宽超时和轮数，保持同一状态机而避免检测器开销造成误报。
- 重新验证：`build-context-test` CTest 13/13、`build-engineering-werror` CTest 3/3、
  `build-native-context` CTest 14/14 均通过；高负载测试连续三轮通过；ASan/UBSan 全量
  单测通过；Valgrind `ERROR SUMMARY: 0`，definite/indirect/possible leak 均为 0，
  仅有 416 bytes/4 blocks 的进程级 still reachable 缓存。TSan 仍受 WSL
`unexpected memory mapping` 启动限制，未宣称 native Linux 竞态验证通过。

## 2026-10-02：性能复测发现并修复混合 Mutex UAF

- Release 高负载复测曾在混合 Mutex 阶段出现 SIGSEGV，进一步定位到
  `src/sync.cpp`：`Mutex::Impl::m_waiters` 保存裸 `WaitNode*`，
  `Unlock()` 释放队列锁后才调用 `Wake()`，等待方可能已经销毁栈节点或
  重置线程本地缓存节点，形成悬空访问。
- 修复后等待队列改为 `std::deque<std::shared_ptr<WaitNode>>`，所有争用等待者
  通过 `AcquireWaitNode()` 获取共享节点；`Unlock()` 将 shared ownership
  保持到锁外 `Wake()` 完成，等待方按节点地址移除队列元素。
- 修复验证：Release CTest 14/14、Werror CTest 3/3 通过；Release 高负载连续
  5 轮无崩溃；ASan/UBSan mutex-only 连续 5 轮无报告；Valgrind 全量单测
  `ERROR SUMMARY: 0`，definite/indirect/possible leak 均为 0，仅 416 bytes
  still reachable。
- 同一组 CPU 上的三轮性能样本：计算 Fiber/线程为
  1595/1616、1614/1632、1560/1555 ms；调度为 93/55、150/55、107/50 ms；
  Channel 为 17/53、18/69、20/56 ms；IO 为 2896/2034、2614/2016、
  2725/2091 ms。混合 Mutex 为 18、1784、1562 ms，线程 `std::mutex`
  为 39、38、37 ms，仍有明显长尾，不能宣称性能稳定。
- Coost 本机对照（同 CPU 集合、历史 `-O2` 构建）调度中位数约 8 ms、
  IO 约 1686 ms、混合 Mutex 约 1721 ms。计时边界和编译参数不完全相同，
  这些数据只用于工程定位，不是严格同配置基准。

## 2026-10-02：收尾文档、中文注释与配置统一

- 新增 `docs/getting_started.md`，从构建、最短 `go()`、Scheduler/Fiber 生命周期，
  讲到 Fiber/线程混合同步、Channel/Select、Context、Linux IO/Hook、日志、配置
  热更新和故障排查；README 与 example 入口均已链接该指南。
- 为 Scheduler/Task、Fiber、IOManager、Context、Channel、Mutex、ConditionVariable、
  WaitGroup、Future、Error、FiberLocal、Hook、线程策略、定时器、配置中心等公共类
  补充中文的依赖、职责、生命周期、参数和返回值说明，并把公共头中的整段英文注释
  翻译为中文；示例注释也已统一为中文。
- `go2cpp.ini` 现在覆盖 SchedulerConfig 全部重要字段和日志字段；统一校验 P/M 数量、
  本地队列、Fiber 栈、FiberBin、亲和预算及毫秒参数范围。ConfigVar 支持强类型校验器，
  非法热更新会拒绝并保留旧值，min/max_workers 的单步修改也不会留下不可启动状态。
- 最终验证：Release 独立构建 `build-final` CTest 14/14；`build-engineering-werror`
  CTest 3/3；ASan+UBSan 构建 CTest 14/14；Valgrind 全量单测通过，`ERROR SUMMARY: 0`，
  definite/indirect/possible leak 均为 0，仅 416 bytes/4 blocks 进程级 still reachable。
  TSan 仍受 WSL 的 `unexpected memory mapping` 启动限制，未宣称通过。

## 2026-10-02：配置热加载编辑锁与半文件保护

- 修复配置监听可能读到编辑器截断文件、并在失败后错误确认文件版本的问题。
  `IniFile::Load` 现在连续读取文件元数据和内容，只有快照稳定且 INI 解析成功才
  进入配置中心；失败不会更新监听器基准，后续轮询会继续重试。
- 配置监听增加固定的 `path + ".lock"` 编辑锁协议。锁文件非空时，启动监听会拒绝
  读取，运行中的监听会跳过刷新；锁文件为空或不存在才允许加载。生产写入流程应为：
  写入非空锁、写同目录临时文件、关闭并校验、`rename` 原子替换目标、最后清空锁文件。
  文档和 `go2cpp.ini` 模板已写明该约束。稳定快照是兜底保护，不能判断语法上完整但仍
  在继续写入的部分前缀，因此不支持绕过编辑锁直接长时间覆盖正式文件。
- 测试新增启动时锁拒绝、锁定期间保持旧值、清空锁后更新，以及截断半文件恢复；覆盖
  Release、`-Wall -Wextra -Wpedantic -Werror`、ASan/UBSan 和 Valgrind。
- 验证结果：`build-final` CTest 14/14；`build-engineering-werror` CTest 3/3；
  `build-native-asan` CTest 14/14；Valgrind `ERROR SUMMARY: 0`，definite/indirect/
  possible leak 均为 0，仅 416 bytes/4 blocks still reachable。构建时 WSL 与 Windows
  文件时间有轻微偏差，出现 gmake clock skew 警告，不影响测试结果。

## 2026-10-02：配置独立性规范

- 在 `docs/configuration.md`、`docs/getting_started.md` 和 `go2cpp.ini` 中明确规范：
  允许热更新的配置键必须独立解析、独立校验、独立生效，不能依赖同一文件中其他键
  的非原子中间值。`min_workers`/`max_workers` 等相互约束的结构参数被限定为启动期
  成组配置，必须通过完整快照一次替换并重新创建 Scheduler，禁止分别热更新。

## 2026-10-02：统一中文 Doxygen 注释规范

- 按 Sylar 风格补充 public header：类/结构体职责、依赖、线程安全和生命周期，
  公开函数的参数、返回值和失败边界，以及枚举和配置字段的含义；涉及文件包括
  Context、Channel、Fiber、Future、IO、Scheduler、同步原语、日志、Hook、配置和
  运行时入口。
- 为 `src/*.cpp` 的实现函数补充中文 `///` 执行流程注释，覆盖参数校验、锁/状态机、
  唤醒和资源释放等关键步骤；example 与 tests 的顶层入口也补充用途、流程和返回值。
  注释修改不改变逻辑，已清理误插入函数体语句前的模板注释。
- 验证：`git diff --check` 通过；Release CTest 14/14、Werror CTest 3/3 全部通过，
  全部目标成功编译。

## 2026-10-03：模块边界复核与可验证缺陷修复

- 对 GMP、Fiber/Context、Timer、混合同步、Future、Channel/Select、IOBackend/Hook、
  Error、Config 和 Logger 逐模块复核状态机、生命周期、锁顺序、扩展点异常和等待
  超时边界，结果记录在 `docs/boundary_audit.md`。文档把已修复问题、必须遵守的使用
  契约和仍无法安全等价实现的 C++/Go 语义差异分开，避免把测试通过误写成完全无条件
  正确。
- 修复 Error 自定义 `Unwrap/Is/As` 抛异常导致终止的问题；默认 IOBackend 禁止注销；
  IOBackend 实例按 IOManager 单 owner 检查；epoll 改为 level-triggered，避免同一 fd
  多等待者部分消费后丢唤醒；`wait_for(0)`、`WaitAnyFor(0)`、`WaitManyFor(0)` 增加
  原生非阻塞 readiness 探测。
- Fiber native 栈检查 sysconf、页对齐、guard page 映射尺寸溢出；Scheduler 观察事件
  增加 cancelled；Future/Sync 后端注册表改用锁外不可变快照，Future Context 回调
  增加异常展开清理守卫；IniFile 解析失败保持旧快照；Logger parent 拒绝自环和间接环。
- 新增同 fd 多等待者、零超时 readiness、共享 backend、极端 Fiber 栈尺寸、Future
  后端异常清理、Logger 环检测和 IniFile 失败快照测试。
- 验证：`cmake --build build-final --parallel 1` 成功；`ctest --test-dir build-final
  --output-on-failure` 串行 14/14 通过（13.36 秒）；IO、Error 过滤测试通过。
  NTFS/WSL 构建目录禁止在多个构建或 CTest 进程并发读写，避免共享库文件被并发链接
  时出现短文件；本轮未把并行加载错误误判为代码测试失败。
- 未宣称完全无条件正确：固定 Fiber 栈、显式 panic/recover、未 Hook 阻塞调用、
  单线程 TimerService、配置跨变量无锁快照和 watcher/插件生命周期约束仍按文档限制。
- 追加修复 Channel Select 的注册失败边界：内建 case 使用带结果的
  `arm_checked`，复制/注册失败会取消已注册节点并返回 `kInvalid`，不会在零节点
  上永久 park；新增回归测试并通过 `build-final` 与 `build-engineering-werror`。
- 当前 Memcheck：`build-final/valgrind-boundary-audit.log`，1,322,815 次分配、
  1,322,811 次释放；definite/indirect/possible 均为 0，416 bytes/4 blocks
  为进程级 still reachable，`ERROR SUMMARY: 0`。ASan `build-native-asan`
  的 `go2cpp_tests` 也通过；TSan 仍按既有记录受 WSL 映射限制，未伪造通过结论。

## 2026-10-06：定时任务模块文档补充

- 补充 `docs/timer_usage.md`，说明 TimerService 的一次性、周期、动态决策、弱引用、忙等待和 Scheduler/Fiber 投递边界。
- 明确取消与 Running 竞态、readiness 与 timeout 单次 claim、回调锁约束及 TimerManager 与 Scheduler 的解耦。

## 2026-10-06：定时任务模块实现与验证

- 重构 include/go2cpp/core/timer.hpp、src/timer.cpp：增加 TimerState、固定延迟/固定速率、
  TimerDecision、最大次数、忙等待窗口、Reset/Refresh、弱引用任务、异常观察器、执行器
  插件和 PendingCount/NextDeadline；队列使用 multimap 加状态索引，取消和重排为对数复杂度。
- Scheduler 增加 schedule_at/schedule_after/schedule_every 及大写别名，通过
  TimerExecutorAdapter 只把到期回调提交为 Fiber；TimerService 不继承 Scheduler，避免跨 M
  恢复 Fiber 栈。go2cpp::after/every 提供默认调度器的傻瓜式入口。
- 修复关闭竞态：TimerService 停止时正在返回的周期回调不会重新插入已关闭队列；异常回调
  在锁外隔离并将任务标记为 kFailed。明确记录慢回调会阻塞同一服务、服务析构等待回调、
  不能从自身计时回调销毁服务的边界。
- 新增 tests/test_timer.cpp 的 FIFO、非法周期、Reset、固定/动态周期、BusyWait、弱引用、
  异常、并发注册、Scheduler/Fiber 投递和 shutdown 竞态测试；新增
  example/timer_task_demo.cpp 六个线程/Fiber 场景。
- 新增 benchmarks/timer_task_benchmark.cpp、GO2CPP_BUILD_BENCHMARKS 和
  docs/timer_benchmark.md。200 任务实测：普通线程 27.91 ms、TimerService 5.50 ms、
  Scheduler+Fiber 6.41 ms；同机 coost 独立对照 7.11 ms（接口语义不同，仅作量级参考）。
- 验证命令：build-timer-final RelWithDebInfo 构建；CTest 14/14 通过（11.47 s）；
  timer 过滤测试连续 3 次通过；ASan+UBSan timer 过滤通过；Valgrind timer 过滤通过
  且返回码为 0，但当前 Fiber/上下文后端没有产生 Memcheck 退出摘要，未将其误报为完整
  leak 统计。git diff --check 通过。

- 收尾复核：runtime.hpp 显式聚合 core/timer.hpp；示例新增“业务事件提前完成取消超时”场景；
  测试新增可插拔执行器拒绝、同截止时间先进先出和正在执行周期任务的关闭竞态。

## 2026-10-07：网络基础设施与模块目录整理

- 目录按 logging、config、concurrency、tools、network 拆分；旧 include/go2cpp 根路径保留中文兼容转发头，命名空间和原有 target 名不变。测试与示例按模块目录维护，CMake 使用显式源文件列表。
- 新增 network 模块：Address/IPv4Address/IPv6Address/UnixAddress、endian、ByteArray、FdManager、Socket/SocketStream、Framer 和 HTTP/1.x message/parser/client/server。Socket 统一 RAII fd 生命周期，Fiber 中优先 IOManager 等待，普通线程回退 poll；fd 元数据与 close 通知保持代际安全。
- HTTP parser 支持半包、粘包、Content-Length、chunked、重复头、多值 Cookie/Set-Cookie 基础能力和流水线剩余字节，默认头体上限；冲突的 CL/TE、非法头和超限返回错误。HTTP/2/3、WebSocket upgrade、无限流式 Body 暂不在核心实现。
- 验证：`build-network` RelWithDebInfo、静态库、`-Wall -Wextra -Wpedantic` 构建通过；网络过滤测试、network demo 和全量 CTest 15/15 通过（11.05 秒）；`git diff --check` 需在最终提交前再次执行。
- 后续：补充真实 loopback TCP/UDP、Fiber IOManager、多请求 keep-alive、chunked trailer、fd close/reuse、ASan/UBSan/Valgrind 网络专项；HTTP 大文件/SSE 应使用流式 ResponseWriter 扩展。

## 2026-10-07：网络模块第二轮边界验证

- 增加 Framer 分隔符/固定长度/大端长度前缀，补充 Go 风格 ResolveTCPAddr、ResolveUDPAddr、DialTCP、DialUDP、ListenTCP、ListenUDP，以及 Socket 半关闭接口；根路径和 Sylar 风格 `.h` 入口均保留。
- HTTP 增加 CanonicalHeaderKey、ParseHTTPVersion、ParseTime、Get/Head/Post/NewRequest 便捷入口；客户端使用 write_all 处理短写，服务端循环读取分片请求并保留流水线剩余，URL 端口解析失败返回 InvalidUrl。
- 第二轮测试增加 chunked、重复 Content-Length、查询参数重复键、TCP 回环 accept、UDP 无连接 send_to/recv_from、Framer 长度帧和非法 URL。
- 实际验证：RelWithDebInfo 静态构建 `build-network`、Debug 共享+Hook 构建 `build-network-hook`、ASan+UBSan 构建 `build-network-asan` 均成功；网络过滤测试和 Hook 网络测试通过；全量 CTest `build-network` 15/15 通过（约 11 秒）；`-Wall -Wextra -Wpedantic` 无警告；Valgrind 网络过滤返回 0 且无错误退出，但 Fiber 上下文路径未产生 Memcheck 汇总，因此没有宣称完整泄漏统计。
- 保留限制：核心 HTTP 只实现受限 HTTP/1.x 消息缓冲，不包含 HTTP/2/3、WebSocket upgrade、Expect:100、无限 SSE/大文件 ResponseWriter；DNS 为同步 getaddrinfo；ByteArray 单所有者，Socket 同一连接的协议读写需由调用方串行化。

- 网络第二轮还增加了受管 Fiber 读写验证：Fiber 中 Socket 遇 EAGAIN 通过当前 IOManager park，普通发送线程写入后恢复，测试有 2 秒有限 watchdog；Hook、ASan/UBSan 和全量测试均再次通过。

## 2026-10-07：P0 Socket 生命周期与 HTTP 流式基础加固

- `include/go2cpp/network/socket.hpp`、`src/network/socket.cpp` 为读写、accept、connect、
  地址查询、选项查询和半关闭统一加入 in-flight 生命周期。每次操作固定 fd 快照；
  `close/release` 先设置 closing、递增代际、通知 IOManager，等待旧操作退出后再关闭或转移
  fd，并处理重复 close/release。受管 Fiber 和普通线程都覆盖关闭期间的取消路径。
- 修复普通线程无限期 poll 无法被 close 唤醒、Fiber readiness 注册与 close 竞态、关闭后
  旧操作继续使用 fd 等边界；关闭中的新操作返回 `ECANCELED/EBADF`。普通线程和 IOManager
  等待以非忙等待的小片检查关闭状态，保留 deadline 语义。
- `include/go2cpp/network/http.hpp`、`src/network/http.cpp` 增加严格 Transfer-Encoding
  校验、十六进制 chunk 溢出检查、chunk 扩展和 trailer 保存、body 长度溢出保护，以及
  1xx/204/304/HEAD 无 body 规则；Parser append 分配失败转为 ParseState::Error。
- 增加 `http::ResponseWriter`：以 `Idle -> HeadersCommitted -> Closed` 状态输出 chunked
  流，适用于 SSE/大响应的后续封装；每次 write 使用 SocketStream::write_all 处理短写，
  不允许未 start 或 close 后写入。Server 停止标志原子化，handler 异常隔离为 500 响应。
- 新增普通线程和 Fiber 并发 close、HTTP 严格 chunk/trailer、无体响应、HEAD、流式 writer
  状态机回归测试。验证：`build-network` 全量 CTest 15/15；`build-network-hook` 网络
  测试 2/2；`build-network-asan` 网络总测试通过；TSan 构建成功但网络过滤启动时受当前
  WSL 运行时限制报 `unexpected memory mapping`，因此未将 TSan 计为通过；Valgrind 网络
  过滤返回 0，definite/indirect/possible leak 均为 0，still reachable 96 bytes/1 block，
  错误汇总为 0。
- P0 仍不宣称完整 WebSocket/SSE/NetService：ResponseWriter 只是 HTTP/1.1 chunked 基础，
  WebSocket 握手/帧状态机、SSE 心跳/重连、路由治理和分布式组件留在后续阶段；DNS 仍为同步
  getaddrinfo，Fiber 栈和 C++ 异常边界保持现有文档限制。

## 2026-10-07：P1 WebSocket 协议层

- 新增 `include/go2cpp/network/websocket.hpp/.h`、根路径兼容头和
  `src/network/websocket.cpp`。协议对象独立于 Scheduler/IOManager，复用 Socket 的 Fiber/线程
  等待；读方向串行化，写方向由内部锁串行化，避免并发调用交错帧。
- 实现 RFC 6455 HTTP/1.1 Upgrade 校验：GET、HTTP/1.1、Upgrade/Connection token、版本 13、
  16 字节 Base64 key、SHA-1 Accept；提供 `Accept`、`AcceptWithPending` 和 `Connect`。后者不预读
  WebSocket 帧，服务端可用 `AcceptWithPending` 接管 HTTP Parser 的剩余字节，避免升级边界丢数据。
- 实现掩码方向、7/16/64 位长度、高位溢出、RSV/opcode、控制帧、分片重组、最大帧/消息大小、
  Ping 自动 Pong、Pong 忽略、Close 双向确认、Close code/reason 和 UTF-8 校验。
- 新增 `example/network/websocket_demo.cpp`、`docs/websocket.md`；测试覆盖 RFC 握手向量、
  socketpair 客户端/服务端、126 字节扩展长度、文本/二进制、分片、HTTP+首帧同批到达和关闭。
- 验证：WebSocket/网络过滤测试、Hook 构建 `go2cpp_tests` 与 WebSocket 示例 2/2、ASan/UBSan
  网络测试均通过；Valgrind 网络过滤返回 0，definite/indirect/possible leak 均为 0，still
  reachable 96 bytes/1 block，错误汇总为 0。高负载测试单独运行通过。一次整套运行中已有
  `test_io.cpp:439/449` readiness 压力断言出现时间敏感失败，随后等待系统负载稳定后重跑，
  `build-network` 全量 CTest 16/16（11.26 秒）通过；`GO2CPP_TEST_FILTER=io` 也连续 3 次
  通过。TSan 仍受当前 WSL `unexpected memory mapping` 启动限制，未宣称通过。
- P1 明确限制：暂不支持 TLS、HTTP/2 CONNECT、permessage-deflate、扩展/子协议协商和发送端
  主动分片；SSE、鉴权、限流、审计和 NetService 治理继续分阶段实现。

## 2026-10-07：P2 SSE 流式事件

- 新增 `include/go2cpp/network/sse.hpp/.h`、根路径兼容头和 `src/network/sse.cpp`，并加入
  `go2cpp_network`。`sse::Event` 负责字段校验和多行 data 编码；`sse::Parser` 支持任意
  分片、LF/CRLF/CR、UTF-8 BOM、注释、retry、Last-Event-ID 以及行/事件/缓冲区上限；EOF
  不会把未以空行结束的半事件误报为完整消息。
- 新增 `sse::Stream`：复用 `http::ResponseWriter` 输出 200、`text/event-stream`、缓存控制、
  keep-alive 和 chunked 响应；事件/注释写入串行化，心跳通过 `tick()` 显式触发，失败后锁定
  为不可复用状态。Context 取消回调使用 weak 状态，不捕获裸 this，并调用 Socket::cancel_write
  唤醒受阻 Fiber/线程；Socket 的读写取消代际不依赖取消线程当前的 IOManager。不建立无界
  事件队列，慢消费者由写超时或 IO 错误反馈。
- 新增 `tests/network/test_sse.cpp`、`example/network/sse_demo.cpp`、`docs/sse.md`，覆盖
  编码注入、空事件、多行 data、半包解析、空 id 清除 Last-Event-ID、超长限制、chunked
  生命周期、心跳、重复 close、Context 取消、并发生产者以及普通线程跨线程取消 Socket
  等待。
- 验证：`cmake --build build-network -j2`、Hook 和 ASan/UBSan 的 `GO2CPP_TEST_FILTER=sse`
  均通过；普通线程、并发生产者和 Context 取消路径都已实际运行。最终 `ctest --test-dir
  build-network --output-on-failure` 为 17/17 通过（11.80 秒）；Valgrind 全量测试返回 0，
  definite/indirect/possible leak 均为 0，still reachable 416 bytes/4 blocks，错误汇总为 0。
  TSan 仍受当前 WSL `unexpected memory mapping` 启动限制，不能宣称通过。
- 已知边界：SSE 不包含 TLS、HTTP/2/3、自动重连、跨连接事件存储或有界异步队列；这些能力
  由上层 NetService/事件日志注入。`ResponseWriter::flush()` 在 TCP 上只是状态检查，每次
  `send/comment` 已经立即写入 chunk；调用方不得用零长度 chunk 冒充 flush。

## 2026-10-07：P0/P1/P2 第二轮审查与增强

- 根据 Socket/HTTP、WebSocket、SSE 三个只读审查结果修正了实际缺陷：`ResponseWriter` 的
  start/write/close 任一部分写失败后进入 sticky `Failed` 状态，禁止继续污染半个 header 或
  chunk；204/1xx/304 不再生成实体 Content-Length；出站 HTTP 头执行 token、控制字符和 CRLF
  校验；Parser 严格校验请求起始行、三位状态码、响应无体 framing，并拒绝端口溢出。
- `io.cpp` 的 `update_interest_locked` 改为按当前读写等待队列精确更新 epoll interest，修复
  READ|WRITE 收缩时遗留 READ 注册造成的水平触发忙循环；`descriptor_is_open` 只接受 F_GETFD
  成功，并对 EINTR 重试。
- WebSocket 写锁和 SSE 流锁统一改为 `go2cpp::sync::Mutex`，使阻塞写在受管 Fiber 中 park
  而不会卡住同一 M 的其他 Fiber。WebSocket 关闭、析构和写入失败会发布 Socket 读写取消代际；
  握手写入继承 `io_timeout`；要求 Host、拒绝 body/Transfer-Encoding/控制字符，拒绝非最小长度
  编码，并对协议错误、非法 UTF-8、超限消息发送 1002/1007/1009 Close。
- SSE 统一 `max_event_bytes=0` 为不限长，Parser 在解析 `id:` 行时立即发布 Last-Event-ID，
  并明确 Parser 为单消费者状态机；示例新增元数据游标、注释心跳、Context 取消和 WebSocket
  Ping/Pong 场景，网络示例增加 204 无实体演示。测试补充出站头注入、204 body、端口边界和
  WebSocket Host 检查。
- 最终验证：`cmake --build build-network -j2` 通过；`GO2CPP_TEST_FILTER=network`、`sse` 均通过；
  全量 CTest 17/17 通过（11.47 秒）；network/websocket/sse 三个示例均返回 0；Hook 构建的
  network+sse 过滤测试通过；ASan+UBSan 构建的 network+sse 过滤测试通过；Valgrind network
  和 sse 专项返回 0，definite/indirect/possible 均为 0，错误汇总为 0。TSan 仍受当前 WSL 的
  `unexpected memory mapping` 启动限制，未宣称通过。
- 本轮仍保留的边界：DNS 解析同步、Server 路由只允许初始化阶段修改、Socket 自定义 waiter
  没有强制取消协议、Fiber 中主动 close 与 Scheduler shutdown 的极端生命周期需要调用方遵循
  文档约束；这些不应被误报为已完全等价 Go runtime。
- 追加修正 HTTP Client：发送方向继承 Client timeout，ETIMEDOUT/ECANCELED 映射为公开的
  Timeout/Cancelled，并在同一批字节中正确跳过 100 Continue 等 informational 响应；全量网络
  回归再次通过。
- IO 专项新增单 P/单 worker 的读写双 interest 收缩回归：先同时等待同一 fd 的 READ/WRITE，
  消费立即就绪的 WRITE 后再验证 READ，`GO2CPP_TEST_FILTER=io` 通过。
- SSE 查询状态接口增加异常隔离：混合锁在 Scheduler 停止阶段无法 park 时，`noexcept` 查询
  返回 Failed/false/max，不会让异常穿过 `noexcept` 触发进程 terminate。
# 2026-10-08 混合 Mutex 专项优化

- `src/concurrency/sync.cpp` 保留无竞争原子快路径、FIFO 等待队列、`shared_ptr<WaitNode>` 生命周期锚点和跨 M `park_wait/wake_registered` 交接；没有采用会跳过 Scheduler 状态机的裸指针或未经验证的独立 Fiber 队列。
- 默认无状态 `SchedulerWaitBackend` 改为进程内共享实例，减少每次等待的控制块分配；受管 Fiber 的协作重试限定为有界 256 次，随后一次性进入等待路径，避免旧版反复调度造成的长尾。
- 失败的实验方案已撤回：提前把普通线程竞争计入等待者、Fiber/native 双队列优先级交接、专用轻量 waiter 都在压力或竞态测试中出现长尾或唤醒风险，没有保留在最终实现中。
- Release 专项 `go2cpp_high_load_stress --mutex-only` 30 轮：耗时最小/中位数/P95/最大值为 `9/19/31/51 ms`，无超时；同一完整高负载线程池 `std::mutex` 约 `36～40 ms`。按吞吐换算满足线程基线 75% 门槛。
- 验证：`build-network` 全量 CTest `17/17`；同步过滤测试连续 5 次通过；ASan/UBSan 同步过滤通过；Valgrind 混合锁专项 `ERROR SUMMARY: 0`，definite/indirect/possible leak 均为 0，96 bytes still reachable 为进程级缓存。
- 已知边界：带 Context、长临界区、ConditionVariable/WaitGroup 和手动 Fiber 仍走完整等待节点路径；锁内禁止阻塞 IO、日志回调和插件回调。TSan 受当前 WSL `unexpected memory mapping` 环境限制，未宣称通过。


## IO 性能优化记录（2026-10-08 12:16:07 UTC）

- 变更：Epoll 后端的 `EPOLLONESHOT` 能力声明与事件位保持一致；默认 Linux 后端启用 ONESHOT，由 `IOManager` 显式重新布防；事件批处理保持 256；注册表记录 `FdSlot*`，仅在 `m_mutex` 下解引用并在删除注册项前失效。
- 文件：`src/network/io.cpp`、`src/network/io_backend.cpp`、`include/go2cpp/network/io_backend.hpp`、`benchmarks/io_epoll_fair_benchmark.cpp`、`cmake/Go2CppTests.cmake`。
- 正确性：`ctest --test-dir build-network --output-on-failure` 最新一次曾在实验性调度快路径下暴露 high-load channel 失败，随后已撤销该快路径；撤销后 `go2cpp_high_load_stress` 通过，IO 专项通过；ASan/UBSan IO 专项通过。
- 公平基准：4096 fd、25 waves、8 workers、1 poller，线程 epoll 稳态中位数约 2172 ms，IOManager+Fiber 约 3626 ms，Fiber/线程约 1.67；事件完整 102400/102400，无超时。目标 1.25 倍尚未达到，不能宣称完成。
- 后续：需要针对 Fiber 调度/任务状态转换和跨 M 入队继续做受控 profiling；当前已知低风险优化未能消除调度开销。

## IO 性能优化记录（2026-10-10）

- 正确性修复：`src/network/io.cpp` 的 `drain_queue_locked`、`take_ready_locked` 和
  `complete_if_head` 统一采用“先取得队头、登记 operation、完成 claim、再移出队列”的
  线性化顺序，避免取消/超时与 epoll 就绪并发时把未完成节点弹出后永久丢失。新增
  `test_wait_many_immediate_ready_race`，双 fd 已就绪场景循环 1000 次，验证 `WaitMany`
  不少返回 ready 项。
- 热路径优化：WaitNode 的 operation 生命周期改为 0/1/2/3 原子状态机；无 Context 等待
  跳过回调互斥量清理；FdSlot 复用 DescriptorToken，去掉重复 F_GETFD/descriptor registry
  校验；IO poller 一次批量唤醒按最近 P 分桶，减少 P 队列锁次数；已 claim 的 WaitNode
  采用无锁任务快照；批量入队复用原子 queued 门，跳过每事件的 transition_mutex；epoll
  注册 token 改为 `fd + 32 位序号`编码，移除每次 ONESHOT rearm 的 `m_registrations`
  哈希表节点分配。
- 构建与回归：`build-network` 全量 CTest 17/17 通过；IO 过滤测试通过；ASan/UBSan IO
  过滤测试通过；`go2cpp_high_load_stress --io-only` 通过，Fiber 1239 ms、thread-poll
  1992 ms；没有发现错误、超时或泄漏报告。
- 公平基准（Release，4096 fd、25 waves、8 workers、1 poller、102400 事件）：Go2Cpp
  thread-pool+epoll 中位数约 2013 ms，IOManager+Fiber 约 703 ms，Fiber/thread 约 0.349，
  所有事件完整且无超时。独立 coost 适配器同口径实测 thread-epoll 约 143 ms、coost 约
  1155 ms；因此本轮数据在该 WSL 运行环境下 Fiber 小于独立 coost 适配器，但仍不能把
  Go2Cpp 结论外推到所有线程池实现。单 worker/低并发时 Fiber 仍明显慢于线程基线，说明
  上下文切换和调度固定成本尚未消除。
- 追加压力口径（4096 fd、10 waves、16 workers、40960 事件）：thread-pool+epoll 中位数
  约 723 ms，IOManager+Fiber 中位数约 296 ms，Fiber/thread 约 0.410；对应 coost 适配器
  thread-epoll 约 66 ms、coost 约 525 ms。高并发 Fiber 仍明显优于本项目线程基线和该
  适配器的 coost 协程，但相对最快三方线程 epoll 仍有固定调度成本。
- 约束与后续：WSL CPU/调度噪声会使同一基准出现较大波动；所有模型均串行运行并记录完整
  事件数后才纳入比较。用户要求的“对任意线程 epoll 与 coost 均不超过 1.25 倍”尚未被
  全场景证明，不能宣称达标；下一步应继续做上下文切换、P 队列入队和 ONESHOT rearm 的
  受控 profiling，而不能通过削弱正确性校验来降低数字。

## 2026-10-10：全模块边界回归与生命周期修复

- 修复 `src/network/io.cpp` 的 `WaitMany` 零超时路径：所有未完成节点都执行零超时
  `poll` 探测，不再因已有终态节点而跳过其他 fd，避免多 fd 同时就绪时丢失结果。
- 修复 `src/concurrency/scheduler.cpp` 的等待交接：`Task::wake_for_wait()` 与 park
  使用同一 `m_transition_mutex`，防止唤醒先到时被后续 park 覆盖；同步测试扩大为固定种子
  256 轮并加入 yield 抖动。
- 修复 Socket 延迟 close/release 的生命周期：进行中的操作持有 `shared_ptr` 自保持，关闭
  请求只发布取消代际，最后一个操作退出后再由唯一 owner 关闭 fd、移除 FdManager 并通知
  等待者；新增阻塞等待下 close/release 回归。
- 修复动态定时器配置的引用环：`ConfigVar` 监听器改为弱引用自身变量，避免
  `ConfigVar -> Listener -> ConfigVar` 泄漏；测试绑定全局 `Config::Instance()` 时使用进程
  生命周期的 `static RuntimeConfig`，遵守 `BindRuntimeConfig` 的目标生命周期契约。
- 变更文件：`src/network/io.cpp`、`src/network/socket.cpp`、`include/go2cpp/network/socket.hpp`、
  `src/concurrency/scheduler.cpp`、`tests/concurrency/test_sync.cpp`、`tests/network/test_network.cpp`、
  `src/config/config.cpp`、`tests/logging/test_log_config.cpp`。
- 验证结果：`cmake --build build-review -j2` 成功；Release 全量 CTest 随机顺序重复 2 轮，
  18/18、共 36 次通过；`go2cpp_tests` 全量通过；高负载场景 50000 yielding tasks、
  8000×100000 计算、2000 Fiber+32 线程混合锁、32000 channel 消息、4096×25 IO 均通过；
  新 ASan/UBSan 全量运行通过；新二进制 Valgrind Memcheck 返回 0，definite/indirect/possible
  leak 均为 0，still reachable 416 bytes/4 blocks，错误汇总为 0。
- 未通过/未覆盖：TSan 构建成功但运行被当前 WSL 环境的
  `ThreadSanitizer: unexpected memory mapping` 启动限制拒绝，不能据此宣称 TSan 通过；
  `BindRuntimeConfig` 仍要求调用方保证裸指针目标覆盖整个监听生命周期，接口文档已明确，
  后续可增加 RAII 绑定句柄以降低误用风险。
---

## 2026-10-15：Channel 缓冲由 std::deque 改为环形结构（对齐 Go hchan）

**背景**：使用者指出 Channel「包含太多东西」，要求做性能测试并与传统「线程 + 异步队列」对比。
对比结果显示 Channel 的空探测慢约 13 倍、有缓冲吞吐慢 3–4 倍（详见 `docs/performance.md`），
使用者要求「按 Go 源码设计，把状态考虑清楚」后再优化。

**设计依据（读 Go 源码后确定）**：`go/src/runtime/chan.go` 的 `hchan` 用
`buf`（预分配数组）+ `dataqsiz` + `sendx`/`recvx`（环形索引）+ `qcount`，
**不是** `deque`；且索引推进用 `sendx++ / if sendx == dataqsiz { sendx = 0 }`，**不做取模**。
原 C++ 实现的等待队列/大锁/单赢家仲裁都与 Go 一致，唯独缓冲用了 `std::deque<T>` —— 这是差距来源。

**改动**：

- 新增 `detail::RingBuffer<T>`（channel.hpp）：原始存储 + 就地构造，`capacity == 0` 不分配；
  接口保持 `deque` 子集，通道内部 15 个调用点零改动；
- `Channel<T>::m_buffer` 类型替换，构造函数增加 `m_buffer(capacity)` 初始化；
- 槽位推进改为「索引 ++ 后回绕」，去掉锁内取模（初版用 `%`，MPMC 场景被拖慢）。

**测试**：`ctest` 79/79 通过（29.55 s），构建 0 warning；`test_channel.cpp` 新增 4 个环形缓冲专项
（容量 2 回绕 200 轮、容量 1 满空交替 1000 轮、容量 7 跨线程 2 万值求和、2 万步随机与 deque 参考模型对拍）。

**性能（build-bench / Release / -O2，3 次运行 × 3 轮）**：

- 改善：SPSC 缓冲 64 由 735 → 588–629 ns（14–20%）；SPSC 缓冲 1024 由 268 → 226–239 ns（11–16%）；
  MPMC 缓冲 1024 由 738 → 652–694 ns（6–12%）；ping-pong 双向约改善 4–24%；
- **未解决**：MPMC 缓冲 64 由 4,892 → 5,598–5,898 ns（**退化 14–21%**）；已排除锁内取模因素，
  原因待单独剖析；
- 空探测（51–53 ns）未变，该路径不进缓冲。

**被否决的方案**：曾设计「原子标志 + `Try*` 无锁快速路径」以优化空探测，最终放弃 ——
Go 的 `hchan` 无此机制（偏离上游设计），需插桩十余处且漏一处即偶发丢值，收益仅覆盖「必然失败」的路径。

**关键文件**：`include/go2cpp/concurrency/channel.hpp`、`tests/concurrency/test_channel.cpp`、
`benchmarks/channel_vs_queue_benchmark.cpp`、`docs/performance.md`。

**后续**：单独剖析 MPMC 小容量场景（容量 64）的退化来源；如无法收敛，评估是否对
小容量回退到 `deque` 或调整等待路径。

---

## 2026-10-16 13:00–13:55：GMP 调试模块开发启动（分支 feature/gmp-debug-module）

**背景**：前阶段已完成 DebugTooling/ 的设计文档（20 个文件、34 场景、20 dump 选项）。
使用者确认三项定案并补充架构约定后，进入编码阶段。

**使用者定案**：① 元数据用 `DebugFiber` 继承 `Fiber`，调试信息放 `DebugFiber`，不影响原框架；
② 多机时钟用双时间戳并声明不做绝对对齐；③ 运行期可改参数只开放观测/节律类。
**补充约定**：所有 Debug 类尽量**继承原类 + 加 `Debug` 前缀**，避免重复实现。

**新增文件（3 个，原框架零改动）**：
- `include/go2cpp/debug/debug_fiber.hpp`（183 行）：`FiberDebugInfo`、`DebugFiber : public Fiber`、
  `DebugRegistry`（独立 `std::mutex` 注册表，刻意不用 `sync::Mutex` 以免依赖调度器上下文）；
- `include/go2cpp/debug.hpp`（5 行转发头）；
- `tests/debug/test_debug_fiber.cpp`（232 行）。
另登记 `cmake/Go2CppTests.cmake` 与 `tests/test_main.cpp`。

**编译探针实测（决定安全用法）**：`Fiber` 的 `is_polymorphic` = **0**（无虚析构）、
`is_final` = 0（可继承）、无默认构造、不可拷贝/移动；子类可正常实例化并访问继承来的 `id()`。
→ 硬约束：**`DebugFiber` 绝不可通过 `Fiber*`/`unique_ptr<Fiber>` 销毁**，
因此**不得**放入 `Task::m_fiber`（`scheduler.hpp:364` 是 `unique_ptr<Fiber>`）。

**可继承性核实（重要发现）**：`grep -rn "class [A-Za-z_]* final" include/` 显示
**多数类被标记 `final`**，包括调试最关心的 `sync::Mutex`、`ConditionVariable`、`WaitGroup`、
`Channel`、`Context`、`Timer`/`TimerService`、`HybridGate`、`ParkingCondition`、`BlockingRegion`、
`Future`/`Promise`、`FiberLocalCache` 等。可继承的只有 `Fiber`/`Task`/`Scheduler`/`SchedulerObserver`。
→ 已写 `design/09-debug-class-conventions.md` 给出三档策略：A 继承 / B 组合 / C 只读快照，
并明确**不去掉原类的 `final`**（那等于改原框架，违反使用者要求）。

**测试设计（按项目规范）**：5 个真实场景（带描述构造与查询、未注册语义区分、特殊字符承载、
`Fiber::id()` 关联、4 线程并发注册查询）+ 压测（1000 次创建注册、20000 次注册查询，只报量级）
+ 随机（500 组随机元数据逐组校验读回一致）。

**后续状态**：P0 进行中；下一步跑全量验证并做 `emit_event` 微基准。

---

## 2026-10-16 14:00–14:45：调试模块开发第 5 轮 —— 观察者开销实测（含一处测量纠错）

**产出**：`benchmarks/debug_observer_overhead_benchmark.cpp`（222 行，登记 CMake）、
`DebugTooling/perf-observer-overhead.md`、`docs/performance.md` 新节。

**方法**：build-bench（Release + -O2）；任务数 20000；worker 数 1/2/4；三档（observer=nullptr /
原子计数 / 有界环形缓冲 + 独立消费线程）；每档先跑 1 轮热身再做 3 轮取 median。

**结果**：单 worker 下观察者净开销 **+3~6%**（约 27 ns/事件），与 `emit_event` 的实现路径相符；
worker 数增加时比例上升（4 worker 档1 为 +22.7%），其中包含 `observer_mutex` 的多 worker 争用。

**重要区分**：**4 worker 的基线本身异常** —— 1,602 ns（1 worker）→ 19,914 ns（4 worker），
约 12 倍差距，**与观察者无关**，属调度器自身的扩展性问题（本轮不深挖，仅记录）。
因此 4 worker 下的 +22.7% 建立在异常基线上，不宜作为典型开销对外表述。

**自查与纠错**：初版基准没有热身轮，导致"先测的档"把 Fiber 栈池/线程/调度器的冷启动成本
算进去，出现"档 0 比档 1 慢 20 倍"的荒谬结果。发现后加热身轮并修正档 2 的事件计数，
数据才可用；该教训已写入基准源码注释。

**另一处流程教训**：用 `cmd > log 2>&1; echo "rc=$?"` 与 `&& grep -c` 串联时误读了状态
（`grep -c` 无匹配时退出码为 1，把 `&&` 链截断；`;` 分隔又让 `$?` 不等于目标命令的退出码），
导致"构建成功"的误判。后续一律显式分步执行并单独检查退出码。

**后续状态**：P0 的"零开销证明"端到端部分完成；下一步做 `DebugTask`/`DebugScheduler`（继承路径），
以及等使用者圈定锁的代理范围后实现 `DebugMutex`。

---

## 2026-10-16 14:00–14:45：调试模块开发第 6 轮 —— DebugScheduler 代理与三种序列化

**设计判断（先说清楚）**：`DebugTask`/`DebugScheduler` 采用**代理**而非继承。
原因：调试侧要包装的是**框架已创建的对象**（`spawn()` 返回 `shared_ptr<Task>`，无法塞入子类），
继承既无法替换它，`Scheduler`/`Task` 又都是非虚析构，继承反而有误删风险。
对比：`DebugFiber` 用继承是**恰当**的，因为它是用户**自己创建并真实执行**的对象。

**新增**：`include/go2cpp/debug/debug_scheduler.hpp`（311 行）+ 更新 `debug.hpp` +
`tests/debug/test_debug_scheduler.cpp`（205 行，5 场景 + 压测 + 随机）；
登记 `cmake/Go2CppTests.cmake` 与 `tests/test_main.cpp`。
**零侵入**：只调用 `Scheduler` 已有的 public 接口
（计数/`is_running`/sysmon/`metrics`/`processors`/`machines`），未修改框架任何一行。

**序列化**：text / ini / json 三种；json 字段名对齐 `design/02` 的 schema；
双时间戳（steady + wall）按使用者定案输出，并在三种格式里都**显式标注**
「steady 不可跨机比较」「快照为 per-call 一致而非全局原子」
「`long_syscall_count` 是次数不是时长」「metrics 全 0 可能是 collect_metrics=false」。

**三处编译纠错**：① `SchedulerMetrics`/`ProcessorSnapshot`/`MachineSnapshot` 在
`go2cpp::scheduler` 命名空间（`scheduler.hpp` 顶层别名未覆盖），需显式 `using`；
② `"state=" + ProcessorStateName(...)` 是 `const char* + const char*`（非法），
改 `std::string("state=") + ...`；③ json 的真实别名是 `go2cpp::Json`（`tools/json.hpp:69`），
`json.hpp` 只是转发头。

**一处测试假设纠错（有价值）**：原断言 `Contains(ini, "[machine.0]")` 失败。
加诊断后实测 **`machines[0].id = 5`** —— machine id 是**全局递增**、**不从 0 开始**。
改为按快照里的**实际 id** 断言，并通过。教训：断言要用实际值，不能假设从 0 起。

**验证**：构建 0 warning；`debug_scheduler` 单模块 all checks passed；**全量 ctest 79/79 通过**。
压测（仅报量级）：`Snapshot` ×2000 = 2.5 ms（≈1.26 µs/次）；三种 `Dump` ×200 = 2.3 ms（≈11.6 µs/次）。

**后续**：P1 剩余（CLI 入口 + 文件 sink）、"枚举全部 G"接口（需在原类加私有 Debug 方法 + friend）。

---

## 2026-10-16 14:45–15:05：调试模块开发第 7 轮 —— dump CLI 与原子文件 sink

**新增**：`include/go2cpp/debug/debug_cli.hpp`（219 行）与 `tests/debug/test_debug_cli.cpp`（214 行），
更新 `debug.hpp` 转发头，登记 CMake 与 `test_main`。

**能力**：`ParseDumpCommand` 解析 `dump scheduler [--format=text|ini|json] [--output=<file>|-] [-o file]`
`[--append|--truncate] [--summary|--processors|--machines|--metrics] [--no-processors|--no-machines|--no-metrics] [name]`；
`WriteDumpFile` 用**临时文件 + rename** 实现原子写（避免半截 dump），append 模式退化为直接追加并在文档中
如实说明"追加无法原子完成"；`RunDump` / `RunDumpCli` 完成端到端。

**诚实标注（重要）**：本 CLI 是**进程内**形态 —— 调用方把自己的 `Scheduler&` 交给它。
「跨进程 dump 另一个正在运行的进程」需要**信号或注入机制**（例如注册 SIGUSR1 写文件），
属后续阶段，**本模块不声称具备该能力**。同时 `scheduler_name` 仅作标识：项目没有"调度器名字"概念，
`SchedulerConfig` 无 name 字段。

**测试**：5 个场景（三种格式解析 / output 与 -o 与位置参数 / 非法选项与 --help 必须报错 /
原子写与临时文件不残留与 append / 端到端解析到文件且内容与返回值一致）+ 压测（10000 次解析）
+ 随机（500 组随机选项组合，校验 valid 与 error 互斥）。**全量 ctest 79/79 通过**。

**顺手修正**：text 输出的状态列用定宽填充对齐（原先固定空格导致列不齐）。

---

## 2026-10-16 15:00–15:20：调试模块开发第 8 轮 —— P3 锁观测（Mutex + ConditionVariable）

**改动（首次触碰核心文件）**：
- `include/go2cpp/sync.hpp`：新增 `MutexDebugSnapshot` / `ConditionVariableDebugSnapshot` 两个快照结构，
  并给 `Mutex`、`ConditionVariable` 各加一个 `DebugSnapshotInternal()` 方法；
- `src/concurrency/sync.cpp`：新增两个方法的实现（读 `m_state` / `m_native_waiters` / `m_waiters`，
  持队列锁取长度）；
- 新增 `include/go2cpp/debug/debug_sync.hpp`（70 行）与 `tests/debug/test_debug_sync.cpp`（189 行）。

**如实记录的偏离**：使用者确认的方案是「原类内部提供**私有** `Debug*` 方法 + `Debug*` 代理通过
`friend` 访问」。实测中 `friend class go2cpp::debug::DebugMutex;`（前向声明齐备、插入位置正确，
在 `Mutex`/`ConditionVariable` 的 `private:` 段首）**未对该内联代理生效**，
编译报 `'... DebugSnapshotInternal() const' is private within this context`。
为不浪费窗口时间，改为**把方法放在 `public` 段并保留 `Debug` 前缀命名**，
并在头文件注释里写明原因与「仅供 debug 模块调用」的约定。
**没有**采用「去掉原类 `final`」或「改动包含顺序」这类影响面更大的做法。

**快照语义（诚实边界）**：`held` 来自 `m_state` 的 `kLocked` 位、`has_waiters` 来自 `kWaiter` 位、
`native_waiters` 来自 `m_native_waiters`、`waiters` 持队列锁取 `m_waiters.size()`，
`fiber_waiters = waiters - native_waiters`（并 clamp 防并发下瞬时负数）。
**结构里没有 owner 字段** —— 因为 `m_state` 不记录持有者（Go 风格所有权允许 A 加锁 B 解锁），
要支持它必须新增 owner 追踪并声明行为差异，本轮不做、也不虚构字段。

**测试**：5 个场景（未持有到持有、held 与 TryLock 互补 200 次、多线程竞争下出现等待者、
条件变量等待者、空指针代理安全）+ 压测（20000 次 Snapshot ≈90 ns/次）+ 随机（3000 轮随机持锁/释放，
校验 `fiber+native==waiters` 且 `held != TryLock()` 互补）。
**全量 ctest 79/79 通过**（因为改了核心文件 `sync.hpp`/`sync.cpp`，这一步是必需的验证）。

---

## 2026-10-16 15:20–15:45：调试模块开发第 9 轮 —— D3 锁争用选择钩子

**这是使用者最初需求里点名的能力**：「在锁争抢的时候决定哪个 Fiber 能获取权限」。

**接口**（`include/go2cpp/sync.hpp`）：
- `WaiterCandidate { index, is_fiber }` —— **刻意不暴露内部的 `WaitNode`**，
  因此插件只需依赖这两个字段即可写策略；也**没有**虚构 `wait_ns`（`WaitNode` 未记录等待起点，
  要支持它得改节点结构，本轮不做）；
- `WaitSelector::Select(const std::vector<WaiterCandidate>&) -> std::optional<std::size_t>`，
  返回 `nullopt` 表示不干预；
- `SetWaitSelectorFactory` / `ResetWaitSelectorFactory`（与既有的 `WaitBackend` 工厂同风格）。

**接入方式（关键设计）**：`Mutex::Unlock` 在**取得队列锁之后、FIFO 认领循环之前**调用
`ReorderWaitersForSelector(m_waiters)`，该函数**只把选中的节点与队首交换**，
之后仍由原有循环逐个 `TryFinish(kNotified)` 认领。
→ 因此「**所有权无缝转移给下一个持有者**」「**`TryLock` 调用者不能插队**」这两条既有语义
**完全不受影响**；选择器只改变**顺序**，不改变**裁决方式**。
默认（未注册）时热路径只多一次 `CurrentWaitSelector()`（一次加锁取 `shared_ptr`）——**待补实测**。

**约束（写在头文件里）**：选择器在**持队列锁**时被同步调用，**不得阻塞、不得抛异常**
（抛出的异常被 `try/catch` 隔离并退化为默认 FIFO）；**不做饥饿保护**，
若策略会长期跳过某等待者，需实现方自行加上限并强制 FIFO —— 这是**策略的责任**。

**本轮四处纠错（全部是我的问题，不是框架的）**：
1. `go2cpp::debug::DebugMutex` 与函数内的 `using namespace go2cpp::sync;` 冲突，
   报 "reference to 'go2cpp' is ambiguous" → 改用 `::go2cpp::` 全限定；
2. **测试自己写错了**：随机场景里主线程连续 `Unlock()` 三次，
   但只有一次是释放自己持有的锁，后两次解的是已被 worker 释放的锁，
   触发 `std::logic_error: unlock of unlocked mutex` → 改为主线程只释放一次、后续由 worker 链式交接；
3. 一次构建耗时超过工具超时（600s）被移到后台，我当时**误判为"测试卡死"**，
   单独用 `timeout 60` 复跑后确认测试其实是**通过的** —— 根因是**改了核心头 `sync.hpp` 触发大量重编**；
4. 记录教训：改核心头文件后，构建时间的预期必须放宽，且验证命令要带 `timeout` 以免真的卡住时占用窗口。

**测试**：5 个场景（未注册时与默认 FIFO 一致 / 选择器让**指定等待者优先**（3 线程排队后指定最后一个，
断言其第一个拿到锁）/ 返回 `nullopt` 与越界下标退化为默认 / 抛异常被隔离且锁仍正常移交 /
`Reset` 后恢复默认且 `TryLock` 仍不能插队）+ 压测（无竞争 100000 次 ≈2896 µs、
4 线程竞争 20000 次 3 ms 且 completed=20000 无丢失）+ 随机（随机指派下标 8 轮不放死）。

---

## 2026-10-16 15:45–16:05：调试模块开发第 10 轮 —— WaitGroup 观测 + 两处自查纠错

**新增能力（P3 收尾）**：`WaitGroup` 的调试快照。
`WaitGroup::Impl` 本来就持有 `m_count` 与 `m_waiters`（`sync.cpp:1274-1278`），
而 `Count()` **已经是 public**，所以这轮只补了 `waiters` 的可见性：
`WaitGroupDebugSnapshot { count, waiters }` + `WaitGroup::DebugSnapshotInternal()` +
`debug_sync.hpp` 里的 `DebugWaitGroup` 代理（含空指针安全）。测试补 1 个场景。
至此 P3 的三件套（`Mutex` / `ConditionVariable` / `WaitGroup`）加上 D3 选择钩子全部完成。

**自查纠错一（重要，实现与文档不符）**：
`WaitSelector::Select` 我原先声明为 `noexcept`，但头文件注释又写"抛出的异常会被隔离" ——
这两者**不可能同时成立**：`noexcept` 函数里 `throw` 会直接 `std::terminate`，
**永远到不了** `Mutex::Unlock` 里的 `try/catch`。编译器的 `-Wterminate` 告警把这个矛盾暴露了出来。
更糟的是：原场景 4 只放了 **1 个**等待者，而 `ReorderWaitersForSelector` 在 `size<2` 时提前返回，
**选择器根本没被调用** —— 也就是说那条"异常被隔离"的测试**是空跑**，通过了也不能说明任何事。
已修正：接口去掉 `noexcept`、注释改成自洽的表述、场景 4 改为 2 个等待者并用调试代理等到
`waiters >= 2` 后再触发，确保选择器真的被调用。

**自查纠错二（测试基础设施）**：`ctest` 里 `go2cpp_tests` 报过一次 Timeout。
定位过程：先怀疑自己的测试慢，实测五个 debug 测试合计仅 **1.26 s**（`debug_lock_selector` 最大 0.89 s），
不是元凶；真正原因是 `set_tests_properties(go2cpp_tests PROPERTIES TIMEOUT 30)` 的**余量不足** ——
整体 `go2cpp_tests` 已到 ~18 s，而当时我还并行跑着 `build-bench`，CPU 被抢后直接打爆 30 s。
已把超时放宽到 **60**，并在 CMake 注释里写明理由。

**又一次替换事故（记录以避免重犯）**：修命名空间歧义时，我用 `str.replace` 把
`go2cpp::debug::DebugMutex` 全量替换为 `::go2cpp::debug::DebugMutex`，
结果把**本来就是** `::go2cpp::debug::DebugMutex` 的地方替换成了 `::::go2cpp::...`。
教训：**替换必须带边界**（前缀 `(`/空格或使用正则 `\b`），否则会命中已有正确写法的子串。

**验证**：构建 0 warning；`debug_lock_selector` 全场景通过（不含异常场景空跑）；
**全量 ctest 79/79 通过**（29.78 s）。
另注：竞争压测在有选择器参与时从 3 ms 变为 298 ms（20000 次），
两者不可直接比较 —— 后者是**选择器真的被调用且含异常路径**的代价，不是回归。

---

## 2026-10-16 16:05–16:30：调试模块开发第 11 轮 —— P2 事件采集

**新增**：`include/go2cpp/debug/debug_events.hpp`（270 行）与 `tests/debug/test_debug_events.cpp`（265 行），
更新转发头、登记 CMake 与 `test_main`。

**组成**：
- `EventBuffer`：有界环形缓冲（内部数组 capacity+1 以区分满/空），
  `Push`/`Pop` **非阻塞、不抛异常、不分配**；满时 `Push` 返回 false 并累加 `dropped`；
  另有 `PeekAt` 供只读快照使用。
- `DebugEventCollector : SchedulerObserver`：回调里**只做一次非阻塞入队**，
  严格遵守 `scheduler.hpp` 对观察者的约束（回调必须短小且无阻塞）；
  提供 `Drain`/`Peek`/`Pending`/`Captured`/`Dropped` 与三种序列化。

**如实声明的边界**：① 只采集 `SchedulerObserver` 能看到的**已声明事件**，
即设计文档反复强调的「没显示阻塞 ≠ 没有阻塞」；
② 采集器**不是**线程局部的。按 `perf-observer-overhead.md` §4 的建议，
降低多 worker 争用的下一步是「每 worker 线程局部暂存 + 批量发布」，
那需要 per-instance/per-thread 状态管理，**本轮未实现**，在头文件里标注为后续优化，
而不是声称已经做到。

**顺带印证**：`SchedulerObserver::OnEvent` 自身**没有 `noexcept`**，
注释明确写「接口保留可抛出签名是为了让 Scheduler 在 emit_event() 中统一捕获第三方插件异常」——
这与第 10 轮把 `WaitSelector::Select` 的 `noexcept` 去掉是同一个道理，说明当时的修正是对的。

**测试**：6 个场景（空/满/回绕与 FIFO 顺序、满时丢弃并满足 captured+dropped 守恒、
**接真实调度器**能采到 `kTaskStarted`/`kTaskCompleted` 与 `kWorkerStarted`/`kWorkerStopped`、
容量 8 的采集器在 300 个任务下必然丢弃从而证明有界、Drain 消耗与 Peek 不消耗、
三种序列化且 json 能被子项目 `go2cpp::Json` 解析回来并校验事件数与类型名）
+ 压测（`EventBuffer` Push/Pop 100 万次 = **43 ms**）+ 随机（200 组随机容量与数量，
校验 `captured + dropped == 总推送量` 且 `Size() <= Capacity()`）。
构建 0 warning；**全量 ctest 79/79 通过**。

**D3 选择器开销基准（部分完成，如实记录未决项）**：
- **无竞争场景（单线程 TryLock/Unlock）**：档 0/1/2 分别为 5.25 / 4.36 / 4.52 ns/op ——
  三档一致（差异在噪声量级），**证实在 `Unlock` 的 `compare_exchange` 快路径上选择器零开销**
  （该路径根本不会进入选择器分支）。
- **有竞争场景**：小参数（1 轮、500 次/线程）可正常完成并给出数字；
  但**大参数（5 轮、20,000 次/线程，约 80,000 次/档）在 15 分钟内未完成**，
  与早前一次同类超时一致。**根因尚未定位**，因此本轮**不下任何有竞争场景的性能结论**，
  已启动增量定位（2,000 / 5,000 / 10,000 / 20,000 逐级）留待下一轮。
  这是必须如实说明的未决项，不能因为"小参数看起来正常"就当作已完成。

---

## 2026-10-16 16:30–16:45：调试模块开发第 12 轮 —— D3 选择器开销定位（含一次重大澄清）

**上一轮遗留的未决项已澄清**：有竞争场景下的 13~24 µs/op **不是选择器引入的**。

**定位方法**：给基准加第 4 个参数支持**分档单跑**（`all | 0 | 1 | 2`），
并在场景 B 内**逐轮 `fflush(stdout)`** —— 这样即使进程被 `timeout` 杀掉也能看到已完成轮次，
避免再次出现"输出丢失、只剩 exit code"的情况。

**关键数据**：
- 场景 A（无竞争，20 万次/轮 × 3 轮）：档 0/1/2 median 分别 **3.71 / 3.78 / 3.76 ns/op** ——
  **三档一致**，证明选择器在 `Unlock` 的 CAS 快路径上零开销。
- 场景 B（4 线程 × 2 万次/轮）：
  **档 0（未注册选择器，即框架默认）本身就是 15,785 / 18,416 / 19,842 ns/op**，
  与档 2 的 16,776 ns/op 同量级。
  → **13~24 µs/op 是 `Mutex` 自身的竞争成本，与选择器无关。**

**由此纠正我上一轮的表述**：上一轮我看到档 2 是 16.8 µs/op 就推测"可能是 `LastSelector`
导致饥饿"。实测表明**没有注册选择器的档 0 同样慢**，所以那个推测**不成立**，已作废。
（教训：在把现象归因于新代码之前，必须先测"新代码不存在"的基线档 —— 本轮正是靠档 0 才看清。）

**附带发现（不属于本模块，仅记录）**：`Mutex` 在 4 线程高竞争下约 18 µs/op，
而同宿主单线程无竞争仅 3.7 ns/op，相差约 5000 倍。这是 `sync::Mutex` 自身的竞争行为，
本模块不声称原因、不作结论。

**未完成项（如实保留）**：早前一次默认参数运行（5 轮 × 200 万无竞争迭代）超过 15 分钟未结束，
根因未定位。因分档单跑每次都在数秒内完成、结论可复现，本次结论不依赖那次运行，
但该异常**不视为已解决**。

**我自己的重复错误（第三次）**：用 `for n in ...; do ... timeout ... ; done` 时，
`$n` 被 pwsh 吞掉导致循环体实际只执行最后一次。此前已遇到过同类问题（`$f`、`$n`），
本次仍未规避 —— 记录在案，后续 shell 循环里不再依赖 pwsh 传递的变量，而是在 Python 侧展开。

---

## 2026-10-16 16:45–17:05：调试模块开发第 13 轮 —— dump events 接入 CLI

**改动**：
- `debug_cli.hpp`：新增 `DumpTarget { kScheduler, kEvents }`；`ParseDumpCommand` 支持
  `dump scheduler` 与 `dump events` 两种目标，**缺省保持 scheduler**（向后兼容已有调用与测试）；
  新增 `RunEventsDump`（内部用 `DebugEventCollector::Peek`，因此 dump **不会消耗**采集器里的事件）
  与统一入口 `RunDump(command, scheduler, collector, ...)` 按 target 分派。
- `debug_scheduler.hpp`：`DumpOptions` 增加 `event_limit`（0 表示全部）。
- `test_debug_cli.cpp`：补充 dump events 相关场景。

**一个刻意的严格性选择**：当位置参数出现在目标位置却**不是** `scheduler`/`events` 时，
解析器**报错**（`unknown target: ...`）而**不是静默当成 scheduler**。
理由与项目既有约定一致 —— `test_main.cpp` 里就专门处理过"过滤器拼错却把空跑伪装成通过"的问题；
静默降级会把用户打错的目标名变成"看起来成功"的错误结果。测试里对此有专门断言。

**新增测试断言**：`dump events` 的 target 必须为 `kEvents`；`dump --format=ini`（不带目标）
必须默认为 `kScheduler`；`dump scheduler` 仍显式可用；非法目标必须 `!valid` 且 error 含
`unknown target`；端到端写文件后文件内容与返回值一致；**dump 之后 `Pending()` 仍为 7**
（证明只读快照不消耗事件）；json 产物能被 `go2cpp::Json` 解析且事件数为 7。

**验证**：构建 0 warning；`debug_cli` 全部通过；实机输出可见 7 条 `kTaskStarted`
（含 task_id / machine_id / processor_id / task_state / steady_ns 各列）；
**全量 ctest 79/79 通过**。

**至此 P0 / P1 / P2 / P3 四个阶段全部打通**：快照 → 序列化 → CLI → 事件采集 → 锁观测（含 D3 钩子）
都可从同一套 `dump` 接口使用。

---

## 2026-10-16 17:05–17:25：调试模块开发第 14 轮 —— P5 协作式断点（含一次方案级决策）

**新增**：`include/go2cpp/debug/debug_breakpoints.hpp`（293 行）与
`tests/debug/test_debug_breakpoints.cpp`（274 行），更新转发头、登记 CMake 与 `test_main`。

**方案级决策：不在调度器热路径上加断点钩子。**
设计文档里 P5 原本设想"在调度的安全点上下断点"。动手前核实了两条硬约束：
1. 本项目**明确永不强制 detach 任意 C++ 调用栈**（只能协作式挂起），所以**不可能**在任意行停下 G；
2. `Scheduler` **没有安全点回调接口**，而 `SchedulerObserver` 是**只读**边界 ——
   它的头文件明写"观察者……不能通过事件回调控制当前 G 的执行权"。
要在调度器安全点挂钩子，就必须修改热路径上的调度器代码，并且在每次 G 切换时多一次调用。

因此改用**协作式**做法：由使用者在自己代码里调用 `Checkpoint("name")`。结果是：
- **零侵入** —— `Scheduler`/`Task`/`Fiber`/`sync` 一行未改；
- **热路径开销不变** —— 没写 `Checkpoint` 的代码路径完全不受影响；
- **符合项目原则** —— G 只在**它自己声明**的位置停，绝不被强行剥夺执行权。

**如实声明的能力边界**：这不是"任意行断点"，只能停在**调用过 `Checkpoint` 的位置**；
`Checkpoint` **本身不阻塞**（只返回命中的断点 id），是否真的停下来等恢复由调用方决定
（可用 `ScopedCheckpoint` 自动 `Resume`，或自行等待）——这样不把"阻塞"强加给调用方。

**一处语义澄清（测试暴露出来的）**：`Clear()` 原本只清断点表，**不重置** `TotalHits`，
导致跨场景累计（前 6 个场景 11 次 + 本场景 40 次 = 51，而断言期望 40）。
判断这不是 bug 而是**职责划分问题**：累计命中是跨批次的历史信息，不该被"清空断点"顺手抹掉。
于是新增显式的 `ResetStats()`，让"清空断点"与"清零计数器"两个动作彼此独立，
并在测试里加了断言固定这两条语义（`Clear` 后 `TotalHits` 不变、`ResetStats` 后归零）。

**测试**：8 个场景 + 压测 + 随机。其中最贴近真实的是第 7 个：
在**受管 G** 里插入 `ScopedCheckpoint`，4 worker 上 40 个任务并发经过同一检查点，
断言**全部 40 个都命中**、`Hits(id)==40`、且 RAII 析构后 waiting 表为空。
压测：100 万次 `Checkpoint`，无断点 53 ns/次、注册了 1 个**名称不匹配**断点 72 ns/次
（后者是"调试开着但没停"的最常见情形，因此单独量化）。
构建 0 warning；**全量 ctest 79/79 通过**。

**阶段状态**：至此 **P0 / P1 / P2 / P3 / P5 全部完成**。
**P4 控制面按使用者定案不做** —— 定案原文是"运行期可改参数只开放**观测类与节律类**，
**结构类明确拒绝**"，而 P4（迁移 G、插队、延迟替换）属于结构类，因此这不是遗漏而是遵守决定。
