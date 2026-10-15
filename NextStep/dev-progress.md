# 开发进度记录（目标 3：按项目规则开发并记录）

> 规则遵循声明：每一步都按 `PROJECT_RULES.md` 与用户约束执行 ——
> 复用优先（先检索再决定）、0 warning 门禁、测试要求（大数据量 / 随机 / ≥5 真实场景）、
> example 延续 demo/full 风格、变更写入 `codex.md` 与提交（只 add 明确路径）。

---

## 第 10 轮（2026-10-15 18:2x）：`tools/strings.hpp`

### 1. 选型：先检索，再决定（复用优先）

| 候选 | 许可证 | 结论与理由 |
|---|---|---|
| [abseil/abseil-cpp](https://github.com/abseil/abseil-cpp) `absl/strings` | Apache-2.0 | **不采用**：需要引入完整构建集成与多个子库，与项目「运行时零第三方依赖」定位冲突；且它的语义是 Abseil 自己的，不是 Go 的 |
| [boostorg/algorithm](https://github.com/boostorg/algorithm) | Boost 1.0 | **不采用**：项目当前只在非 native 上下文平台链接 `Boost::context`；为一个纯字符串模块引入 Boost 头依赖不划算 |
| [fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT | **不采用**：它是**格式化**库（对应 Go `fmt`），不是 `strings` |
| [tgalaj/strutil](https://github.com/tgalaj/strutil) | MIT（header-only） | **不采用**：功能集与 Go `strings` 不对齐（缺 `Cut*`/`Fields`/`Compare` 等），改造成本高于自研 |
| **自研薄封装** | — | **采用**：目标是与 Go `strings` **语义 1:1**，底层能力 `std::string` 已提供 |

### 2. 语义来源（对照，不是复制）

- 上游：本机 `third_party/go1.23.0.src.tar.gz` 内的 `go/src/strings/`（Go 1.23.0，**BSD-3-Clause**）。
- 做法：只提取该包做**行为对照**（例如 `Count` 的首个分支、`genSplit` 的 `sepSave` 语义），
  由本项目**独立实现**，未复制上游代码。
- 提取与对照命令（可复现）：

```sh
tar -xzf third_party/go1.23.0.src.tar.gz -C /tmp/go-subset go/src/strings
grep -hE "^func [A-Z][A-Za-z0-9]*\(" /tmp/go-subset/go/src/strings/*.go   # 48 个导出函数
```

### 3. 实现

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/strings.hpp` | **654** | header-only 真身：最小 UTF-8 工具（`DecodeRune`/`RuneCount`/`IsSpaceRune`）+ 32 个 Go 同名 API + `StringBuilder` |
| `include/go2cpp/strings.hpp` | 5 | 根路径转发头（与全项目布局一致） |

**已覆盖的 Go API**（同名同义）：`Contains`/`ContainsAny`/`ContainsRune`、`HasPrefix`/`HasSuffix`、`EqualFold`、
`Index`/`IndexByte`/`IndexRune`/`IndexAny`/`LastIndex`/`LastIndexByte`、`Count`、`Split`/`SplitN`/`SplitAfter`/`SplitAfterN`/`Fields`、
`Join`/`Repeat`、`Replace`/`ReplaceAll`、`Trim`/`TrimLeft`/`TrimRight`/`TrimSpace`/`TrimPrefix`/`TrimSuffix`、
`Cut`/`CutPrefix`/`CutSuffix`、`ToUpper`/`ToLower`、`Compare`/`Clone`、`Builder`（`StringBuilder`）。

### 4. 验证（本轮实测）

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/strings_check.cpp -o /tmp/strings_check
./tmp/strings_check      # → strings.hpp check: failures=0
```

- **编译：0 warning / 0 error**（`-Wall -Wextra -Wpedantic`）。
- **行为：49 条边界断言全部通过**，其中特意覆盖 Go 的反直觉语义：
  - `Count(s, "") == RuneCount(s) + 1`（`Count("", "") == 1`）
  - `Index(s, "") == 0`；`Contains(s, "") == true`；`ContainsAny(s, "") == false`
  - `Split(s, "")` 按 **rune** 切分：`Split("a,b,c", "") == ["a", ",", "b", ",", "c"]`
  - `SplitN(s, sep, 0)` 返回空；`n == 1` 返回原串
  - `Replace(s, "", new, -1)` 在每个 rune 前后插入（`Replace("abc","","-",-1) == "-a-b-c-"`）
  - `Repeat(_, -1)` 抛 `std::logic_error`（对齐 Go 的 panic）；`Builder.String()` 之后再写也抛
- **过程中修正的两个真问题**（由构建/断言暴露，非事后声称）：
  1. 注释里的 `go/src/strings/*.go` 触发 GCC `-Wcomment` → 改写措辞；
  2. 两条断言是我**测试期望写错**（`ContainsAny("golang","xyz")` 应为 false；
     `SplitAfter("a,b,c","")` 应为 `a|,|b|,|c`）—— 用诊断程序确认实现无误后修正测试。

### 5. 待办（下一轮）

| 项 | 说明 |
|---|---|
| `tools/strconv.hpp` | `Atoi`/`Itoa`/`ParseInt`/`ParseFloat`/`FormatFloat`/`Quote` 等（对照 Go `strconv`） |
| 正式测试 | `tests/tools/test_strings.cpp`（大数据量 + 随机 + ≥5 场景）并在 `tests/test_main.cpp` 与 `cmake/Go2CppTests.cmake` 登记 |
| 示例 | `example/tools/strings_demo.cpp`（≥3 场景）+ `strings_full.cpp`（≥6 场景），登记 `GO2CPP_EXAMPLES` 与 `example/README.md` |
| 全量验证 | `build-quality` 全量构建 0 warning + `ctest` |
| 记录与提交 | 写入 `codex.md` 第 26 轮，按「只 add 明确路径」提交 |

---

## 第 11 轮（2026-10-15 18:3x）：`tools/strconv.hpp`

### 复用优先的结论

C++ 侧**没有**与 Go `strconv` 语义对齐的现成库：`std::stoi` 不做 `base=0` 推断、不返回 errno 风格错误、
越界时不返回"钳制值"；`std::from_chars` 不认识下划线、也不做进制前缀推断。
因此自研，底层复用 `std::from_chars`/`std::to_chars`（C++17，保证 round-trip 精度）。
语义对照 Go 1.23.0 的 `go/src/strconv/`（BSD-3-Clause，**独立实现**）。

### 实现

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/strconv.hpp` | **546** | 错误锚点 + 4 个结果类型 + 解析/格式化 API |
| `include/go2cpp/strconv.hpp` | 5 | 根路径转发头 |

覆盖：`Atoi`/`Itoa`、`ParseInt`/`ParseUint`（`base=0` 推断、下划线规则、范围钳制）、
`FormatInt`/`FormatUint`（base 2..36）、`ParseBool`/`FormatBool`、
`ParseFloat`/`FormatFloat`（`'f'`/`'e'`/`'g'` + 最短往返）、`Quote`，
以及错误锚点 `StrconvErrSyntax()`/`StrconvErrRange()`（身份比较判定，与全项目一致）。

### 验证（本轮实测）

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/strconv_check.cpp src/tools/error.cpp \
    -o /tmp/strconv_check && /tmp/strconv_check      # → strconv.hpp check: failures=0
```

- **编译 0 warning / 0 error**；
- **60 条边界断言全通过**，重点覆盖：
  - `base=0` 推断：`"0777"` → **八进制 511**、`"0x1f"`/`"0b101"`/`"0o17"`；
  - **下划线只在 `base=0` 接受**且必须在数字之间（`"1_000"` ✓；`"1_0"`(base10)/`"_1"`/`"1_"`/`"1__0"` 全部语法错误）；
  - 越界返回 `ErrRange` **并给出钳制值**（`ParseInt("999",10,8)` → 127；`-999` → -128）；
  - `ParseUint` **不允许符号前缀**（Go 文档明确）；
  - `ParseBool` 精确 12 个接受值（`"True "` 带空格即语法错误）；
  - `ParseFloat` 的 `Inf`/`Infinity`/`NaN`（大小写不敏感）、十六进制浮点 `0x1p4` → 16、下划线 `1_000.5`；
  - `FormatFloat` 最短往返（`3.14`）、`'f'` 定点（`0.10`）、`'e'` 指数（`1.00e+20`）、`Inf`/`NaN` 文本；
  - 非法 `base` 与未实现的 `'x'` 格式**抛 `std::logic_error`**（明确拒绝，而不是给错结果）。

### 本轮修掉的两个问题（由断言暴露，非事后声称）

1. **实现 bug**：`std::from_chars(..., std::chars_format::hex)` 的输入**不含 `0x` 前缀**（C++ 标准），
   而 Go 的输入含前缀 → `ParseFloat("0x1p4")` 解析失败。修法：检出 `0x` 后先 `erase(0,2)` 再解析。
2. **测试期望错误**：Go 文档明确 `ParseUint` 不允许符号前缀，`ParseUint("+1")` 应为语法错误（实现本来就对）。

### 待办（下一轮）

| 项 | 说明 |
|---|---|
| 正式测试 | `tests/tools/test_strings.cpp` + `test_strconv.cpp`：**大数据量 + 随机 + ≥5 真实场景**；在 `tests/test_main.cpp` 与 `cmake/Go2CppTests.cmake` 登记 |
| 示例 | `example/tools/strings_{demo,full}.cpp`、`strconv_{demo,full}.cpp`（延续 demo ≥3 / full ≥6 场景）；登记 `GO2CPP_EXAMPLES` 与 `example/README.md` |
| 全量验证与提交 | `build-quality` 全量 0 warning + `ctest`；写 `codex.md` 第 26 轮；按「只 add 明确路径」提交 |

---

## 第 12 轮（2026-10-15 18:5x）：strings / strconv 的正式测试与两处登记

### 交付

| 文件 | 行数 | 内容 |
|---|---:|---|
| `tests/tools/test_strings.cpp` | 242 | **6 个真实场景**（日志行切分、CSV 清洗、URL 解析、HTTP 头名比较、Builder+UTF-8、配置取值）+ **10 万字段** Join/Split 往返 + **2 万轮**随机字节对拍 + **1 万轮**随机 UTF-8 对拍 |
| `tests/tools/test_strconv.cpp` | 245 | **6 个真实场景**（配置解析、指标格式化、JSON 数值往返、十六进制 ID、Unix 权限位、范围钳制）+ **20 万×2** 大数据量往返 + **4 组随机属性测试**（int64 / uint64×base(2..36) / double 位相等 / 语法判定与标准库对拍） |
| `tests/test_main.cpp` | 登记 | 新增 `run_strings_tests()`/`run_strconv_tests()` 声明与 `selected("strings")`/`selected("strconv")` 分支 |
| `cmake/Go2CppTests.cmake` | 登记 | `go2cpp_tests` 源文件列表新增两项（**该文件是 CRLF，插入行按 CRLF**） |

### 验证（实测）

```sh
cmake --build build-quality --target go2cpp_tests      # 0 warning / 0 error
GO2CPP_TEST_FILTER=strings ./build-quality/go2cpp_tests   # → all checks passed
GO2CPP_TEST_FILTER=strconv ./build-quality/go2cpp_tests   # → all checks passed
```

### 被测试抓出的三个缺陷（全部在**测试代码**里，实现无误）

1. 用 `Index`（首次出现）判断 `HasSuffix` → 重复子串场景（`text="abab"`、`needle="ab"`）必然误判 → 改用 `LastIndex`；
2. 参考实现直接调 `std::from_chars` 与 `Atoi` 对拍 → 它**不接受前导 `+`**，而 Go 的 `Atoi` 接受；
3. 修 2 之后仍失败：剥离 `+` 再交给 `from_chars`，会把 `"+-5"` 当成合法负数（Go 拒绝）→ 改为**显式语法检查**（至多一个前导符号 + 其余全数字）再用无符号解析复核范围。

> 这三条正是「随机对拍 + 独立参考实现」的价值：它们没有暴露实现错误，
> 但暴露了我对「参考实现应当等价于谁」的理解偏差 —— 已全部修正并留注释说明原因。

### 待办（下一轮）

| 项 | 说明 |
|---|---|
| 示例 | `example/tools/strings_{demo,full}.cpp`、`strconv_{demo,full}.cpp`（demo ≥3 场景 / full ≥6 场景），登记 `GO2CPP_EXAMPLES` 与 `example/README.md` |
| 全量验证 | `build-quality` 全量构建 0 warning + `ctest` 全绿（含新增两个测试模块） |
| 记录与提交 | `codex.md` 第 26 轮；按「只 add 明确路径」提交 |

---

## 第 14 轮（2026-10-15 18:5x）：`tools/fmt.hpp`（Go fmt 核心子集）

### 复用优先的取舍

| 候选 | 许可证 | 结论 |
|---|---|---|
| [fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT | **不采用为直接依赖**：它的语法是 `{}`（Python 风格），与 Go 的 `%v` 指令集不同；要用它必须先写「Go 格式串 → fmt 格式串」的转换层，而转换层本身就要实现本文件的解析器（等于双份复杂度）。**保留为底层候选**：将来若需要更强的浮点/编译期检查，可在 `detail` 层替换实现。 |
| libc `printf` 家族 | — | **不采用**：类型不安全（可变参数）；Go 的 width/precision 以 **rune** 计、C 的以字节计；`%v` 在 C 中不存在 |
| **自研** | — | **采用**：Go 语义 1:1，且天然复用本项目已有的 `strconv.hpp`（整数/浮点格式化与 `Quote`）与 `error.hpp`（`ErrorPtr`/`Wrap`/`Join`） |

语义对照来源：`third_party/go1.23.0.src.tar.gz` 内的 `go/src/fmt/doc.go`（Go 1.23.0，BSD-3-Clause），**独立实现**。

### 实现

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/fmt.hpp` | ~612 | 格式串解析 + 类型分派（可变参数模板 + `if constexpr`）+ 全部公开 API |
| `include/go2cpp/fmt.hpp` | 5 | 根路径转发头 |

- **动词**：`v` `d` `s` `q` `t` `f` `e` `g` `x` `X` `b` `o` `c` `T`（`Errorf` 另支持 `w`）
- **flags**：`-`（左对齐）、`0`（零填充，数字时在符号之后）、`+`、空格、`#`（`0b`/`0`/`0x` 前缀、浮点保留小数点）
- **width / precision**：字面数字；**按 rune 计算**（与 Go 一致，不是字节）；字符串 precision 按 rune 截断
- **特殊类型**：`ErrorPtr`（按 `Message()` 输出，对齐 Go 的 error 接口规则）、`std::vector<T>`（递归）、`std::pair`
- **对齐 Go 的错误标记**：`%!d(string=abc)`（动词/类型不匹配）、`%!d(MISSING)`（缺参数）、`%!(EXTRA int=2)`（多余参数）
- **API**：`Sprintf`/`Sprint`/`Sprintln`/`Errorf`（`%w` 支持 `Wrap` 与多 `%w` 的 `Join`）/`Printf`/`Print`/`Println`
- **边界**（文件头已列）：不实现 `Formatter`/`GoStringer` 接口分派（C++ 无反射）、`%U`、复数、`%p` 的 `#` 细节；`*` 形式的 width/precision **抛 `std::logic_error` 明确拒绝**

### 验证（实测）

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/fmt_check.cpp src/tools/error.cpp -o /tmp/fmt_check
# → 编译诊断 0 行；运行：fmt.hpp check: failures=0
```

断言覆盖：各 flag 组合、零填充与符号位置、右/左对齐、`%#b`/`%#o`/`%#x`、按 rune 截断、
`%v` 浮点最短表示与 `%e`/`%f` 默认精度 6、`%q` 对字符串/rune、容器与 pair 的递归、
`ErrorPtr` 的 `%v`/`%s`/`%q`/`%T`、三类错误标记、`Errorf` 的 `%w` 身份判定（`Is`）与多 `%w` 聚合、`*` 宽度的明确拒绝。

### 本轮修掉的问题（全部由编译器/断言暴露）

| # | 位置 | 问题 | 处置 |
|---:|---|---|---|
| 1 | `Sprint`/`Sprintln`/EXTRA | `((expr, 0), ...)` 触发 `-Wunused-value` ×3 | 直接用逗号折叠 |
| 2 | `TypeName` | 用 `sizeof` 推断 → `int` 被报成 `int32`（Go 的 `%T` 输出 `int`）；`char[2]` 衰变为 `char*`，落不到 `is_same_v<const char*>` 分支 | 改为按 **C++ 类型名**映射，字符串类判定改用 `is_convertible_v<..., string_view>` |
| 3 | vector 分支 | 只处理 `'v'`/`'s'`，**漏了 `'q'`**（Go 对 `[]string` 的 `%q` 会逐个引用） | 递归传递 verb |
| 4 | `Sprintln` | 三目写进逗号折叠 → **编译错误** `binary expression in operand of fold-expression` | 三目移入 `emit` lambda 内部 |
| 5 | `Sprint()`/`Sprintln()` 零参数 | 空包折叠导致 `emit` 未使用 → 模板诊断 | 用 `if constexpr (sizeof...(Args) > 0)` 保护 |
| 6 | **流程教训** | 上一次修正后「输出未变」，真实原因是**编译失败**，而命令写成 `g++ ... \| grep -c warning ; ./check`（`;` 分隔）→ 掩盖错误并运行了**旧二进制** | 验证命令改为 `&&` 串联，编译失败即不会运行旧产物；本文件为此留下记录 |

> 第 6 条是本轮最有价值的教训：**"看到输出"不等于"跑的是新代码"**。此后所有验证命令一律 `&&` 串联。

### 待办（下一轮）

| 项 | 说明 |
|---|---|
| 正式测试 | `tests/tools/test_fmt.cpp`：大数据量（10 万次格式化）+ 随机（随机格式串/参数对拍）+ ≥5 真实场景（日志、SQL 拼接、错误包装链、表格对齐、指标）；登记 `test_main.cpp` 与 CMake |
| 示例 | `example/tools/fmt_{demo,full}.cpp`（3 / 6 场景）+ `GO2CPP_EXAMPLES` 与 `example/README.md` 登记 |
| 收尾 | 全量构建 0 warning + `ctest` 全绿 + `codex.md` 第 27 轮 + 提交 |

---

## 第 16 轮（2026-10-15 18:4x）：`tools/streams.hpp`（Go `io` 接口族）

### 命名偏差（重要，必须记录）

Go 的 `io` 包提供「流式读写接口族」（`io.Reader`/`io.Writer`/`io.Copy`…），
而**本项目已经把这个名字用于描述符与多路复用调度层**（`go2cpp::io::IOManager` / `FdManager`，
见 `include/go2cpp/network/io.hpp`）。两者语义不同、不可合并，因此：

- 文件：`include/go2cpp/tools/streams.hpp` + 转发头 `include/go2cpp/streams.hpp`
- 命名空间：**`go2cpp::streams`**（而不是 `go2cpp::io`）

| Go | 本实现 |
|---|---|
| `io.Reader` | `streams::Reader` |
| `io.WriteString(w, s)` | `streams::WriteString(w, s)` |
| `io.EOF` | `streams::IoEof()` |

这是**有意保留的命名偏差**，其余 API 与 Go 同名同义。

### 复用优先的结论

C++ 侧没有与 Go `io` 语义对齐的现成库：`std::istream`/`std::ostream` 用**状态位**表达失败，
既不返回错误值，也不支持 Go 的「`n > 0` 同时带错误」契约，`ReadFull`/`CopyN` 这类组合语义更无处对应。
因此自研。语义对照来源：`third_party/go1.23.0.src.tar.gz` 的 `go/src/io/`（Go 1.23.0，BSD-3-Clause），**独立实现**。

### 实现（664 行）

| 类别 | 内容 |
|---|---|
| 结果类型 | `ReadResult`/`WriteResult`/`SeekResult`/`ByteResult`/`RuneResult`（对应 Go 的多返回值） |
| 接口 | `Reader`/`Writer`/`Closer`/`Seeker`/`ReaderAt`/`WriterAt`/`ByteReader`/`ByteWriter`/`RuneReader`/`StringWriter`/`WriterTo`/`ReaderFrom` |
| 错误锚点 | `IoEof()`/`IoErrUnexpectedEof()`/`IoErrShortWrite()`/`IoErrShortBuffer()`/`IoErrNoProgress()`（身份比较） |
| 核心函数 | `ReadAtLeast`/`ReadFull`/`ReadAll`/`WriteString`/`Copy`/`CopyBuffer`/`CopyN` |
| 适配器 | `LimitedReader`/`MultiReader`/`MultiWriter`/`TeeReader`/`SectionReader`/`DiscardWriter`（+`Discard()`） |
| 内存底座 | `StringReader`（bytes.Reader 最小版）、`BufferWriter`（bytes.Buffer 最小版） |
| 常量 | `kSeekStart`/`kSeekCurrent`/`kSeekEnd`（对应 `io.SeekStart` 等） |

### 对齐的非显然语义

- `ReadAtLeast`：`size < minimum` → `ErrShortBuffer`；**读够 minimum 后清除 `EOF`**；
  `0 < n < minimum` 且遇 `EOF` → `ErrUnexpectedEOF`（`n == 0` 时原样返回 `EOF`）；
- `ReadAll`：把 `EOF` **归一化为成功**（错误为空）；
- `Copy`：优先使用源端 `WriterTo` / 目标端 `ReaderFrom`，否则 32 KiB 缓冲循环；
  **短写 → `ErrShortWrite`**；连续 100 次空读 → `ErrNoProgress`；
- `CopyN`：写满 `n` 成功；`written < n` 且无错 → `EOF`；
- `MultiReader`：某个源「读到数据同时遇 `EOF`」时**清掉 EOF 先交付数据**，下次调用再切源；
- `CopyBuffer`：Go 在缓冲长度为 0 时 panic，这里返回 `ErrShortBuffer`（明确拒绝而非崩溃）。

### 验证

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/streams_check.cpp src/tools/error.cpp -o /tmp/streams_check
# → 编译诊断 0 行；streams.hpp check: failures=0（约 40 条断言）
```

覆盖：`ReadFull` 的三种结果、`ErrShortBuffer`、`ErrUnexpectedEOF`、读够后清除 `EOF`、
`ReadAll` 的 EOF 归一化、`Copy`/`CopyN`/`CopyBuffer`（含短写检测）、`LimitReader`、
`MultiReader` 跨源、`MultiWriter`、`TeeReader`、`Discard`、`SectionReader`（Read/ReadAt/Seek/Size）、
`ByteReader`/`WriteByte`/`StringWriter` 优化路径、`BufferWriter` 的 `Reset`/`Len`/消费式 `Read`。

### 本轮修掉的两个问题（都在验证侧）

1. **验证程序 bug**：把字符串字面量 `const_cast` 后当作可写缓冲传给 `Read` → `memcpy` 写入只读页 → **段错误**。
   现象是「编译通过但运行无输出」，又因为命令用了 `set -e` 被掩盖成静默失败 —— 与第 14 轮的教训同源：
   **验证脚本必须显式报告退出码与输出**。
2. **测试期望算错**：`SectionReader(source, 2, 4)` 的内容是 `"2345"`，`ReadAt(buf, 2, 1)` 应得 `"34"`（我写成 `"45"`），实现无误。

### 待办（下一轮）

| 项 | 说明 |
|---|---|
| 正式测试 | `tests/tools/test_streams.cpp`：大数据量（1 MiB 级复制 + 10 万次小块读写）+ 随机（随机分块/随机偏移/随机短读）/ ≥5 真实场景（文件式复制、压缩流模拟、多段拼接、限流读取、分片校验） |
| 示例 | `example/tools/streams_{demo,full}.cpp` + `GO2CPP_EXAMPLES` 与 `example/README.md` 登记 |
| 收尾 | 全量构建 0 warning + `ctest` 全绿 + `codex.md` 第 28 轮 + 提交 |

---

## 第 17 轮（2026-10-15 18:5x）：streams 的正式测试与两处登记

### 交付

| 文件 | 行数 | 内容 |
|---|---:|---|
| `tests/tools/test_streams.cpp` | 333 | **8 个真实场景**（1 MiB 复制、定长帧解析、配置头部限流、多段拼接、边读边算、多路输出、文件区间+Seek、丢弃旁路）+ **大数据量**（1 MiB、10 万次小块读写、2 万帧最恶劣短读）+ **3 组随机**（随机分块源 Copy、2 万次随机偏移 SectionReader、2 万次随机 LimitReader 上限） |
| `tests/test_main.cpp` | 登记 | `run_streams_tests()` 声明 + `selected("streams")` 分支 |
| `cmake/Go2CppTests.cmake` | 登记 | 测试源文件列表（**CRLF 文件，插入行按 CRLF**） |

### 验证（实测）

```sh
cmake --build build-quality --target go2cpp_tests      # 0 warning / 0 error
GO2CPP_TEST_FILTER=streams ./build-quality/go2cpp_tests   # → all checks passed
```

随机测试的设计要点：`ChunkedReader`（每次只给 1..37 字节）与 `DripReader`（每次只给 1 字节）
用来**强制触发短读路径**，断言 `Copy`/`ReadFull` 在任意分块下结果仍逐字节正确 ——
这是 Go `io` 契约里最容易写错的部分。

### 本轮修掉的两个问题（都在测试侧）

1. 场景 8 里我用 `new StringReader(...)` 造了个临时对象且从不释放（**内存泄漏的废代码**），自查后重写为「`Discard()` 作为 `TeeReader` 旁路」的有效断言。
2. 测试期望算错：`"MAGIC:0011"` 是 **10** 字节，而我按 11 写（第 11 字节是换行）→ 改为按「11 字节 = 10 字节头部 + 换行」断言。

### 待办（下一轮）

| 项 | 说明 |
|---|---|
| 示例 | `example/tools/streams_{demo,full}.cpp`（3 / 6 场景）+ `GO2CPP_EXAMPLES` 与 `example/README.md` 登记 |
| 收尾 | `build-quality` 全量 0 warning + `ctest` 全绿 + `codex.md` 第 28 轮 + 按明确路径提交 |
