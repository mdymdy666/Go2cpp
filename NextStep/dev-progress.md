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

---

## 第 19 轮（2026-10-15 19:3x）：`tools/time.hpp`（Go `time` 核心子集）

### 复用优先的结论

C++ 有 `std::chrono`，但与 Go 的 `time` **语义不匹配**，因此自研：

| 差异点 | Go | `std::chrono` |
|---|---|---|
| 时长类型 | `Duration` 是**强类型 int64 纳秒**，带 `String()` 的特定格式（`1h2m3.5s` / `1.5ms` / `300ns`） | 模板化的 `duration<Rep, Period>`，无统一字符串格式 |
| 时间点 | `Time` 带**单调时钟**读数，`Sub` 优先用单调读数 | `system_clock` 与 `steady_clock` 是两个不同的类型，需自己组合 |
| 睡眠语义 | `Sleep` 挂起 **goroutine** | —— |

但本模块**复用了项目已有的两处能力**：
① 时钟用 `std::chrono`（`system_clock` 取墙钟 + `steady_clock` 取单调读数）；
② **`Sleep` 直接调用 `::nanosleep`** —— 项目的 Hook 已拦截 `sleep`/`usleep`/`nanosleep`
（`src/network/hook.cpp` 的 `cooperative_sleep_for`：临时 `eventfd` + `manager->WaitFor`），
在 Fiber 上下文中会变成**协作式等待**，M 会被让给其他可运行的 G。这正是 Go「挂起当前 goroutine」的语义，
**不需要新造调度机制**。

语义对照来源：`third_party/go1.23.0.src.tar.gz` 的 `go/src/time/`（Go 1.23.0，BSD-3-Clause），**独立实现**；
`Duration.String()` 的算法逐分支对照了上游 `time.go` 的 `Duration.format`。

### 实现

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/time.hpp` | ~410 | `Duration` + `Time` + `Since`/`Until`/`Sleep` |
| `include/go2cpp/time.hpp` | 6 | 根路径转发头（部署前确认该路径未被占用） |

- **`Duration`**：`int64` 纳秒；`Nanoseconds/Microseconds/Milliseconds/Seconds/Minutes/Hours`、
  `Abs/Truncate/Round`、`String()`、全套运算符（含**非成员** `int * Duration`，支持 Go 的 `2 * time.Second` 写法）；
  单位常量 `Nanosecond`~`Hour`。
- **`Time`**：`Now()`（同时取墙钟与单调读数）、`FromUnix()`、`Unix/UnixMilli/UnixMicro/UnixNano`、
  `Add/Sub/Before/After/Equal/IsZero`、`String()`、`ToSystemClock()`。
- **`Since`/`Until`/`Sleep`**：`Sleep` 对非正时长立即返回（Go 语义）。

### 对齐的非显然语义

- `Duration.String()`：`< 1s` 时按 ns/µs/ms 选单位与精度（`prec = 0/3/6`），`0 → "0s"`；
  否则秒带最多 9 位小数、**去掉尾随零**，再按需补 `m`/`h`（`1h1m1.5s`、`1m0s`、`1m30s`）；
  微秒符号用 **U+00B5 `µ`（两字节 UTF-8）**；
- `Time.Sub()` **优先用单调读数**（两端都有时），否则回落墙钟；`Equal()` **只用墙钟**（与 Go 一致）；
- **零 `Time` 是「1 年 1 月 1 日」而不是 Unix 0** —— `Time::FromUnix(0).IsZero()` 必须为 false。

### 验证（实测）

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -I include /tmp/time_check.cpp -o /tmp/time_check
# → 0 warning / 0 error；time.hpp check: failures=0（约 50 条断言）
```

其中 `Duration.String()` 有 **18 条逐条对照**：`0s`/`1ns`/`999ns`/`1µs`/`1.5µs`/`1ms`/`1.5ms`/`1s`/`1.5s`/
`1.0000005s`/`1m0s`/`1m30s`/`1h0m0s`/`1h1m1.5s`/`-1.5s`/`-1ns`/`2m0s`。

### 本轮修掉的问题

| # | 问题 | 处置 |
|---:|---|---|
| 1 | 缺 `int * Duration` 重载（Go 里 `2 * time.Second` 是常见写法） | 补非成员 `operator*` |
| 2 | **真 bug**：`Duration.String()` 的 `< 1s` 分支漏了「小数之后补整数部分」（Go 是 `fmtFrac` 之后还要 `fmtInt`）→ `1ns`/`1µs`/`1.5ms` 全部输出成空串 | 补 `AppendInteger` |
| 3 | **语义 bug**：`IsZero()` 用 `SystemClock::time_point{}`（即 Unix 0）当零值，而 Go 的零 Time 是「1 年 1 月 1 日」→ `FromUnix(0).IsZero()` 误判为 true | 改为显式零标记 `m_is_zero` |
| 4 | 编译命令用了 `set -e`，编译失败时诊断被吞掉、只剩空输出 | 改为显式打印 `compile_rc`/`warn`/`error` 计数与诊断头部 |

### 待办（下一轮）

| 项 | 说明 |
|---|---|
| 正式测试 | `tests/tools/test_time.cpp`：大数据量（10 万次 `Duration` 格式化与纳秒往返）+ 随机（随机纳秒 → `String()` → 语义校验、随机 `Add`/`Sub` 往返）+ ≥5 场景（超时预算、重试退避、指标窗口、日志时间戳、周期任务节拍） |
| 示例 | `example/tools/time_{demo,full}.cpp` + `GO2CPP_EXAMPLES` 与 `example/README.md` 登记 |
| 收尾 | 全量构建 0 warning + `ctest` + `codex.md` 第 29 轮 + 提交 |
| 未实现的 `time` 成员 | `After`/`Tick`/`Ticker`/`Timer`（需与 `Channel<Time>` 和 `core::TimerService` 对接）、`Format`/`Parse`（布局串小语言）、`LoadLocation`、`Duration.Parse` —— 已在头文件末尾列成表 |

---

## 第 20 轮（2026-10-15 19:5x）：`time` 的正式测试与示例（模块闭环）

### 交付

| 文件 | 行数 | 内容 |
|---|---:|---|
| `tests/tools/test_time.cpp` | 282 | **6 个真实场景**（超时预算、重试退避、指标窗口、日志时间戳、周期节拍零漂移、耗时统计）+ **大数据量**（10 万次格式化、10 万次纳秒换算往返）+ **3 组随机性质测试** |
| `example/tools/time_demo.cpp` | 81 | 简单版 3 场景 |
| `example/tools/time_full.cpp` | 169 | 全面版 6 场景（含 `Duration.String` 的 **15 条对照表**） |
| `tests/test_main.cpp`、`cmake/Go2CppTests.cmake`、`example/README.md` | — | 三处登记（测试模块 + 示例） |

随机测试的三组性质：
① 随机纳秒 → `String()` 与**独立参考实现**对拍（10 万轮）；
② 随机 `Add`/`Sub` 往返（10 万轮，含 `Add(-d)` 抵消）；
③ 随机比较的**三分律** + `Truncate` 是单位整数倍且不超过原值 + `Round`-`Truncate` 差值不超过一个单位（10 万轮）。

### 验证（实测）

```sh
GO2CPP_TEST_FILTER=time ./build-quality/go2cpp_tests     # → all checks passed
./build-quality/go2cpp_time_demo / go2cpp_time_full      # rc=0，输出逐条与注释预期一致
cmake --build build-quality --parallel                   # 0 warning / 0 error
ctest --test-dir build-quality                           # 79/79 通过（30.52 s）
```

### 本轮修掉的问题（都在测试代码里；实现经 10 万轮对拍证明无误）

1. `Minute / 2` 的期望写成 `"30m0s"` —— 它其实是 **30 秒**（`"30s"`）；
2. 单位换算断言多除了一次：应为 `FromNanoseconds(nanos).Microseconds()` 而不是 `FromNanoseconds(nanos/1000).Microseconds()`；
3. `Round`-`Truncate` 差值性质对**负数**不成立（差为负），应判断绝对值 ≤ 一个单位。

> 定位方式：先写独立诊断程序，用**同一随机种子**对拍 `Duration.String()` 与参考实现 ——
> 得到「0 差异」，从而把怀疑方向从实现转到测试，避免了盲改实现。

### 当前开发进度总览（目标③）

| 模块 | 源码 | 测试 | 示例 | 提交 |
|---|---:|---:|---:|---|
| `strings` | 654 | 245 | 228 | `a88290d` |
| `strconv` | 550 | 244 | 243 | `a88290d` |
| `fmt` | 619 | 214 | 215 | `c64b2ec` |
| `streams` | 664 | 333 | 282 | `c2fe880` |
| `time` | 414 | 282 | 250 | 本轮提交 |
| **合计** | **2901** | **1318** | **1218** | 5 次提交 |

### 待办（后续轮次）

| 项 | 说明 |
|---|---|
| `time` 的剩余成员 | `After`/`Tick`/`Ticker`/`Timer`（需与 `Channel<Time>` + `core::TimerService` 对接）、`Format`/`Parse`、`Duration.Parse` |
| S1/S2 其余标准库 | `bytes`、`sort`、`path/filepath`、`os`、`bufio`、`slices`/`maps`/`cmp` |
| S5 决策 | `encoding/json` 的结构体编解码策略（代码生成 vs 显式绑定）—— 它是 T4 生态的前置条件 |

---

## 第 21 轮（2026-10-15 19:5x）：`tools/sort.hpp` + `tools/bytes.hpp`（S1 继续）

### 交付

| 文件 | 行数 | 说明 |
|---|---:|---|
| `include/go2cpp/tools/sort.hpp` | **260** | `Sort`/`Stable`/`IsSorted`、`Slice`/`SliceStable`/`SliceIsSorted`、`Ints`/`Float64s`/`Strings` + `*AreSorted`、`Search`/`Find`/`SearchInts`/`SearchStrings`/`SearchFloat64s`、`ReverseLess`/`Reverse` |
| `include/go2cpp/sort.hpp` | 5 | 根路径转发头 |
| `include/go2cpp/tools/bytes.hpp` | **532** | 函数家族（比较/查找/切分/变换/修剪，共 30+ 个）+ `Buffer`（含 `Next`/`ReadBytes`/`ReadString`/`UnreadByte`/`UnreadRune`/`ReadFrom`/`WriteTo`/`Truncate`/`Grow`）+ `Reader`（定位读取） |
| `include/go2cpp/bytes.hpp` | 5 | 根路径转发头 |

### 复用优先的结论

- **sort**：底层复用 `std::sort`/`stable_sort`/`is_sorted`/`lower_bound` —— Go 的 `sort.Sort` 是不稳定排序
  （1.19+ 为 pdqsort）、`sort.Stable` 是稳定排序（symmerge），与 C++ 的 `std::sort`（introsort，不稳定）/
  `std::stable_sort`（稳定）**语义正好对应**。**不引入** pdqsort 之类的第三方实现：那是性能优化而非语义，
  而性能结论必须由 `build-bench`（Release）实测后决定。接口形态上，Go 的 `sort.Interface`（`Len`/`Less`/`Swap`）
  映射为「容器/迭代器 + 比较器」的模板形式，避免为每次比较付出虚调用。
- **bytes**：Go 的 `bytes` 与 `strings` 是**同一套算法作用于不同类型**（`bytes.Split` 与 `strings.Split` 行为一致），
  因此函数家族**直接转发到 `go2cpp/strings.hpp`**，避免两份实现漂移；`Buffer`/`Reader` 独立实现
  （`streams` 的 `BufferWriter`/`StringReader` 是 `final` 且只覆盖最小子集，而 `bytes.Buffer` 还需要
  `Next`/`ReadBytes`/`ReadString`/`UnreadByte`/`Truncate`/`Grow`/`WriterTo`/`ReadFrom`），
  但**复用 `streams` 的接口**以保证互操作（验证里就把 `Buffer` 当 `streams::Reader` 交给了 `ReadAll`）。

### 对齐的非显然语义

- `sort.Search(n, f)`：返回**最小的** `i ∈ [0, n]` 使 `f(i)` 为真；**全为假时返回 `n`**（未找到用 `n` 表达，不是 -1）；
- `sort.Find(n, cmp)`：三向比较，命中返回 `(下标, true)`，未命中返回 `(插入位置, false)`；
- `sort.Float64s`：把 **NaN 视为小于任何其它值**（对齐 Go 的 `lessFloat64`），因此 NaN 排在最前；
- `bytes.Buffer.Read`：**缓冲为空时返回 `io.EOF`**（不是 0 字节且无错误）；
- `bytes.Buffer.Next(n)`：`n > Len()` 时返回剩余全部并清空；空缓冲返回空切片且**不带错误**；
- `bytes.Buffer.ReadBytes`/`ReadString`：**包含分隔符**；未找到时返回剩余全部并带 `io.EOF`；
- `bytes.Buffer.UnreadByte`/`UnreadRune`：只在**上一次操作是成功读**时有效，否则报错（与 Go 的报错语义一致）；
- `bytes.Trim(s, cutset)` 的 `cutset` 是**字符集合**（与 `strings.Trim` 同义）。

### 验证（实测）

两个模块各有独立验证程序，均 `0 warning / 0 error` 且 `failures=0`：

- `sort`：基础排序、`IsSorted`、反向比较器、`Stable` 的稳定性（10 万对同键元素的原序保持）、
  `Slice*` 的下标式比较器、`Ints`/`Strings`/`Float64s`、**NaN 最小**、`Search` 的四种边界（含全假返回 n）、
  `Find` 命中/未命中、`SearchInts`/`SearchStrings`/`SearchFloat64s`、**100 万个随机整数排序 + 有序性校验**；
- `bytes`：比较/查找/切分/变换/修剪家族、`Buffer` 的写读往返与空缓冲 `io.EOF`、`Next` 的边界、
  `ReadBytes` 的含分隔符与未找到语义、`UnreadByte`/`UnreadRune` 的前置条件、`Truncate`/`Grow`/`Reset`/`Cap`、
  `ReadFrom`/`WriteTo` 与 `streams` 互操作、`Reader` 的 `ReadAt`/`Seek`/`ReadRune`（UTF-8 三字节）。

### 本轮修掉的问题

| # | 位置 | 问题 | 处置 |
|---:|---|---|---|
| 1 | `sort.hpp` | `Slice*` 系列的 lambda **漏捕获基址**（`values` 未捕获），无法编译 | 提前取 `base` 指针并按值捕获 |
| 2 | `sort.hpp` | `Float64sAreSorted` 的比较器用了 `<=`，不是**严格弱序** | 改为与 `Float64s` 同一个 less |
| 3 | `sort` 验证 | `less(i, j) = i < j` 只比较位置，不是有效的排序谓词 | 改为通过下标访问元素比较 |
| 4 | `bytes.hpp` | 写了 `strings::Contains` 等前缀 —— `strings.hpp` 的函数在 **`go2cpp` 顶层**（无 `strings` 子命名空间），且同名会命中 `bytes::Contains` 自身导致**无限递归** | 全部改为显式 `go2cpp::` 限定（36 处） |

### 待办（下一轮）

| 项 | 说明 |
|---|---|
| 正式测试 | `tests/tools/test_sort.cpp` + `test_bytes.cpp`（大数据量 + 随机 + ≥5 场景）|
| 示例 | `example/tools/sort_{demo,full}.cpp`、`bytes_{demo,full}.cpp` + `GO2CPP_EXAMPLES` 与 `example/README.md` 登记 |
| 收尾 | 全量构建 0 warning + `ctest` + `codex.md` 第 30 轮 + 提交 |

---

## 第 22 轮（2026-10-15 20:2x）：按反馈做减法 —— 删除 C++ 标准库已有的重复实现

### 使用者反馈

> 「有的标准库其实有了，比如 map，完全不需要自己做。你把不需要的删掉，这些没什么用。」

### 采用的判据（新增，写进计划文档）

**一个 Go 包如果其功能在 C++ 标准库里已有等价物，就不该再封装一层。**
封装只会得到「同一个实现的第二个名字」：增加维护面、让转译器多一层无意义的间接，
而且一旦 C++ 标准库演进，还得跟着改两份。

### 从计划中移除（并给出替代写法）

| 移除项 | C++ 等价物 |
|---|---|
| `slices` | `std::ranges` / STL 算法（`std::sort`/`std::find`/`std::ranges::contains`） |
| `maps` | `std::unordered_map` / `std::map`（`contains`/`erase_if`/range-for） |
| `cmp` | C++20 `operator<=>` |
| `math/bits` | C++20 `<bit>`（`std::popcount`/`std::rotl`/`std::countl_zero`/`std::bit_width`） |
| `container/heap` | `std::priority_queue`（或 `std::push_heap`/`pop_heap`） |
| `container/list` | `std::list` |
| `path` / `path/filepath` | `std::filesystem::path` + `recursive_directory_iterator` |
| `math` 的大部分 | `<cmath>`（只保留 Go 特有且易写错的，按需再加） |
| `bytes` 的函数家族 | `go2cpp/strings.hpp` 同名函数（C++ 里 `std::string` 就是字节序列） |
| `bytes.Reader` | `go2cpp::streams::StringReader` |
| `sort` 的 `Ints`/`Strings`/`Float64s`/`*AreSorted` | `std::sort` / `std::is_sorted` |
| `sync/atomic` | `std::atomic` |

（`container/ring` 无直接等价物，保留评估。）

### 代码精简（本次实际执行）

| 文件 | 变化 | 说明 |
|---|---|---|
| `include/go2cpp/tools/bytes.hpp` | **532 → 269 行** | 删除 30+ 个函数家族转发（与 `strings` 同类型，纯转发）与重复的 `bytes::Reader`；只保留 Go 特有的 `Buffer`，并在文件头列出每个被删 API 的替代写法 |
| `include/go2cpp/tools/sort.hpp` | 删除 6 个函数 | 删掉 `Ints`/`Strings`/`Float64s`/`IntsAreSorted`/`StringsAreSorted`/`Float64sAreSorted`（都是标准库一行调用）；**只保留** `Float64LessNaN` 比较器，因为「NaN 视为最小」是 Go 特有语义、容易写错 |

### 文档更新

- `NextStep/go-stdlib-map.md`：新增 **§4.4「不实现：C++ 标准库已提供」**（15 行映射表 + 判据 + 本次精简记录）；
  §3 表格中 `slices`/`maps`/`cmp`/`math/bits`/`path`/`path/filepath`/`container/*` 的结论列由「缺」改为「**不实现**」，
  `sort`/`bytes` 改为「**部分**」并写明只做哪部分；S1/S3/S6 的阶段清单同步收缩。
- `NextStep/roadmap.md`：S3 从「容器与工具补齐（8 个模块）」收缩为「**IO 工具补齐**（`bufio` + `encoding/binary`）」；
  S6 移除 `container/*`，`hash/*` 标注为「无标准等价物，需实现」。

### 保留（这些有真实的 Go 特有语义，不删）

`strings`（Go 的 `Split`/`Trim` 语义与 STL 不同）、`strconv`（`base=0` 推断、下划线、`ErrRange` 钳制值）、
`fmt`（`%v`/`%w` 指令集与 `%!verb(...)` 标记）、`streams`（Go 的 `n>0` 同时带错误契约）、
`time`（`Duration.String()` 格式、单调读数、零值语义）、
`sort` 的 `Slice*`/`Search`/`Find`（Go 特有返回语义）、`bytes::Buffer`（读写游标分离与 `Next`/`UnreadByte` 等）。

### 验证

两个模块的验证程序在精简后重新编译运行：**0 warning / 0 error，failures=0**
（`sort` 的断言改为直接用 `std::sort`/`std::is_sorted` + `Float64LessNaN`；`bytes` 只断言 `Buffer`）。

### 待办

| 项 | 说明 |
|---|---|
| `sort`/`bytes` 的正式测试与示例 | 按项目规范仍需补齐（源码与头文件级验证已就绪） |
| 后续标准库模块 | 按收缩后的 S3 只做 `bufio` + `encoding/binary`；再往后是 S5（JSON 绑定策略决策） |

---

## 第 23 轮（2026-10-15 20:1x）：按澄清恢复已实现内容 + 确立「薄封装」原则

### 使用者澄清

> 「已经做了就不需要删了，恢复，但是后面不需要自己做轮子，在严谨考虑好差别后，薄封装即可。」

我上一轮把「不要重复造轮子」理解成了「不实现」，于是删掉了已经写好的 `bytes` 函数家族与
`sort` 的六个便捷函数，并把计划里的 `slices`/`maps`/`cmp`/`math bits`/`path*` 标为「不实现」——
这过头了。正确的做法是：**已经做的保留**，**后续模块做薄封装**。

### 本轮动作

1. **恢复代码**：
   - `include/go2cpp/tools/bytes.hpp`：269 → **532 行**（恢复 30+ 个函数家族 + `bytes::Reader`）；
   - `include/go2cpp/tools/sort.hpp`：恢复 `Ints`/`Strings`/`Float64s`/`IntsAreSorted`/`StringsAreSorted`/`Float64sAreSorted`，
     并**保留** `Float64LessNaN`（差别处理的产物）；
2. **恢复验证断言并复验**：两个程序重新编译运行 → **0 warning / 0 error、failures=0**；
3. **文档改为「薄封装」原则**：`go-stdlib-map.md` 的 §4.4 整节重写（32 处提到"薄封装"），
   §3 表格 8 处结论列由「不实现」改回「薄封装」，S1/S3/S6 清单恢复；
   `roadmap.md` 的 S3 段改为「全部按薄封装实施」（7 处提到"薄封装"），S6 恢复 `container/*`。

### 薄封装原则（三条，写进 §4.4）

1. **不重写算法** —— 功能在 C++ 标准库已有等价物时，不要自己实现一遍（否则维护两份，且标准库演进后还得跟着改）；
2. **但要薄封装** —— 提供与 Go **同名**的 API，内部一行调用标准库。收益很实际：让 Go 源码里的
   `slices.Contains(x, v)` 转译后仍有**一一对应**的 API 名，而不是要求转译器硬编码 `std::ranges::contains`；
3. **前提是「严谨考虑差别」** —— 动手前逐项对比语义，**有差别的地方必须显式处理**。

### 已确认的差别（§4.4 里做成对照表）

| 差别点 | Go 语义 | C++ 标准库 | 处置 |
|---|---|---|---|
| 浮点排序 | `sort.Float64s` 的 `lessFloat64` 把 **NaN 视为最小** | `operator<` 对 NaN 恒为 false（得到未定义序） | 显式传 `Float64LessNaN`（**已实现**） |
| 二分查找 | `sort.Search` 返回最小 `i`，**未找到返回 `n`** | `std::lower_bound` 返回迭代器（未找到是 `end()`） | `Search` 返回 `std::int64_t`，未找到给 `n`（**已实现**） |
| 查找结果 | `sort.Find` 三向比较，未命中返回**插入位置** | 无直接等价物 | 自行实现约 15 行（**已实现**） |
| 字节 vs 字符串 | `bytes`/`strings` 是**两个类型**、两套 API | `std::string` 同时充当两者 | `bytes` 函数薄封装到 `strings` 同名函数（**已实现**） |
| `Trim` 的 cutset | 是**字符集合**（不是子串） | 无直接等价物 | 已在 `strings::Trim` 实现（**已实现**） |
| 浮点取余 | `math.Mod`（符号随被除数） | `std::fmod` 语义相同 | 薄封装 `math::Mod` → `std::fmod` |
| 取整 | `math.Round` 半值**远离零** | `std::round` 同 | 薄封装即可 |

### 计划影响

- **S1**：保持完整 —— `bytes.hpp`/`sort.hpp` 已实现并保留（含薄封装）；
- **S3**：保留 8 个模块（`bufio`/`path/filepath`/`slices`/`maps`/`cmp`/`math`/`math/bits`/`encoding/binary`），
  但**一律按薄封装实施**，且**每个模块动手前先做差别核对**、把结论写进模块文件头（DoD 里也加了"差别说明"）；
- **S6**：恢复 `container/*`（薄封装：`heap`→`std::priority_queue`、`list`→`std::list`，
  `ring` 无等价物需自行实现约 30 行）。

### 待办

| 项 | 说明 |
|---|---|
| `sort`/`bytes` 的正式测试与示例 | 源码与头文件级验证已就绪，按规范补测试 + demo/full 示例 + 三处登记 |
| 后续模块 | 按薄封装原则推进（先差别核对，再写一行转发） |

---

## 第 24 轮（2026-10-15 20:5x）：第三方库调查报告（`third-party-survey.md`）

### 背景

使用者指出「主要是第三方库，你也要搞一下，这么多不错的第三方库，也要写进调查 md 里面」。
此前的 `third-party-plan.md` 只列了 T1~T4 分层候选与取舍，**广度不足**；本轮补一份系统性的调查报告。

### 调查方式（可信度分层）

| 手段 | 说明 |
|---|---|
| **GitHub API 一手核验** | `https://api.github.com/repos/{owner}/{repo}` 与 `.../license`，取 SPDX 标识与 star 数；执行日期 **2026-10-15**。这是**一手证据**，报告中标 **★**。 |
| 检索 | 只用于**发现候选**与确认生态格局，不用于断言许可证。 |
| 项目内 Go 源码 | `third_party/go1.23.0.src.tar.gz`，用于判断第三方库依赖了哪些标准库能力。 |

> 过程中发现：**WSL 内无网络**（`curl` 无输出），`web_fetch` 抓 `raw.githubusercontent.com` 会 30 秒超时；
> 但 **`api.github.com` 稳定可用**（且直接返回 SPDX 标识，比抓 LICENSE 原文更省），
> 因此选它做批量核验。这一条记下来，供后续核验直接复用。

### 交付

`NextStep/third-party-survey.md`（约 400 行）：

- **§2 一手核验样本（14 个）**：gin/cobra/gorm/chi/echo/fiber/gorilla-mux/sqlx/**ent(17,208★,Apache-2.0)**/
  **pgx(14,327★,MIT)**/**go-redis(22,263★,BSD-2-Clause)**/**bigcache(8,168★,Apache-2.0)**/
  **zerolog(12,519★,MIT)**/**logrus(25,763★,MIT)** —— 许可证分布：MIT 8 / Apache-2.0 3 / BSD-3 1 / BSD-2 1，
  **全部宽松许可，无一传染性许可**；
- **§3 分领域清单（15 个领域、100+ 个库）**：Web/路由、数据库与 ORM、缓存、消息队列、RPC、
  日志、配置、CLI、测试、校验与 DI、监控、序列化、任务队列与爬虫、图像与 Office、认证与安全。
  每个库给出：许可证（★核验 / ○待核验）、定位、**依赖特征**、可移植性评级（A~D）、Go2Cpp 建议；
- **§4 四类处置**：① 可较早推进（协议/算法型，无反射，约 16 个）；
  ② 需标准库底座先行（6 个）；③ 不建议移植（反射/HTTP2/全家桶，13 个）；
  ④ 自研优于移植（配置/日志/测试/DI 四处）；
- **§5 与实施计划的衔接**：把本报告的结论一一映射到 `third-party-plan.md` 的 T1~T4 与 §3.5 计划产出时间；
- **§6 下一步核验清单**：按优先级列出 10 个待核验库及其要核验的内容（LICENSE + 依赖闭包）。

### 本轮结论（对开发路线的实际影响）

1. **许可证不是门槛，技术可移植性才是** —— 14 个样本全是宽松许可，但其中一半因**反射**无法移植；
2. **数据库方向应走「协议 + 代码生成」而不是 ORM**：`pgx`（PostgreSQL 协议）可移植，
   `sqlc` 式「SQL → 生成代码」的思路与 Go2Cpp 替代反射的方向天然一致；`gorm`/`sqlx` 依赖反射 + `database/sql`，不做；
3. **意外的高价值目标**：`allegro/bigcache` —— 它本身就是为「降低 GC 压力」而设计的，
   与 Go2Cpp **没有 GC** 的世界观完全一致，可移植性评为 A；
4. **消息方向首选 `nats.go`**（文本协议），缓存方向首选 `go-redis`（RESP 文本协议）；
5. **DI 应学 `wire`（编译期代码生成），而不是 `fx`/`dig`（运行时反射）**。

### 待办

| 项 | 说明 |
|---|---|
| 按 §6 核验下一批库 | `pflag`/`uuid`/`decimal`/`x-time`/`lumberjack`/`nats.go`/`godotenv`/`cron` 的 LICENSE 与依赖闭包 |
| 核验结论落地 | 写进 `docs/dependencies.md`（来源 + commit/tag + 许可证 + 日期） |

---

## 第 25 轮（2026-10-15 21:2x）：第三方库第二批许可证核验（累计 21 个）

### 核验方式

`api.github.com`（`/repos/{owner}/{repo}` 给 SPDX + star + 默认分支；`/license` 给 LICENSE 原文）。
**执行日期 2026-10-15**。

### 本轮结果（8 个）

| 库 | 许可证 | Star | 备注 |
|---|---|---:|---|
| `spf13/pflag` | **BSD-3-Clause** | 2,779 | 是 `ogier/pflag` 的 fork，但事实标准是 spf13 版 |
| `google/uuid` | **BSD-3-Clause** | 6,148 | |
| `shopspring/decimal` | **MIT**（GitHub 显示 `NOASSERTION`） | 7,492 | 需抓原文才能确认 |
| `golang/time`（`x/time`） | **BSD-3-Clause** | 426 | |
| `natefinch/lumberjack` | **MIT** | 5,478 | 默认分支是 `v2.0`，不是 master/main |
| `joho/godotenv` | **MIT** | 10,661 | |
| `robfig/cron` | **MIT** | 14,196 | |
| **累计** | **21 个全部宽松许可** | | MIT / Apache-2.0 / BSD-2 / BSD-3 |

### 本轮最有价值的发现

**`NOASSERTION` 不等于「许可证不明」**：`shopspring/decimal` 的 GitHub SPDX 字段是
`NOASSERTION`，抓 LICENSE 原文才发现**实际是 MIT** —— 该文件同时包含自身 MIT 文本与
一段引自 `oguzbilgic/fpd` 的 MIT 文本，自动检测器因此放弃断言。
**结论：SPDX 字段只作快速筛选，遇 `NOASSERTION` 必须回落到 LICENSE 原文。**

另两条经验（已写进 survey 第 6.2 节）：

- **端点可用性**：WSL 内无网络；`raw.githubusercontent.com` 经 `web_fetch` 会 30 秒超时；
  `api.github.com` 稳定可用，其中 `/repos` 端点一次拿到 SPDX + star + 默认分支，最省；
- **注意 `default_branch`**：lumberjack 的默认分支是 `v2.0`，自行拼 raw URL 会 404。

### 待办

下一批核验：`nats.go`、`prometheus/client_golang`、`golang-jwt/jwt`、`gobreaker`、
`excelize`、`colly`+`goquery`、`imaging`+`gg`、`opentelemetry-go`；
核验结论最终要写进 `docs/dependencies.md`（含 commit/tag 与日期）。

---

## 第 26 轮（2026-10-15 20:5x）：第三方库第三、四批核验（累计 29 个）

### 本轮核验（8 个）

| 库 | 许可证 | Star |
|---|---|---:|
| `nats-io/nats.go` | **Apache-2.0** | 6,778 |
| `prometheus/client_golang` | **Apache-2.0** | 6,042 |
| `golang-jwt/jwt` | **MIT** | 9,241 |
| `sony/gobreaker` | **MIT** | 3,705 |
| `qax-os/excelize` | **BSD-3-Clause** | 20,994 |
| `gocolly/colly` | **Apache-2.0** | 25,551 |
| `PuerkitoBio/goquery` | **BSD-3-Clause** | 14,988 |
| `disintegration/imaging` | **MIT** | 5,766 |

**累计 29 个库全部为宽松许可**（MIT / Apache-2.0 / BSD-2 / BSD-3），无一传染性许可。

### 观察

- 本轮 8 个**未再出现 `NOASSERTION`**（上一轮的 `shopspring/decimal` 是特例），
  说明 GitHub 的自动识别总体可靠，但**不能因此省略原文核验**这一步；
- `default_branch` 又是两种写法（`main` 3 个、`master` 5 个），进一步印证
  「不要自行拼 raw URL，用 API 端点」这条经验；
- **高 star 与可移植性不相关**：`colly`（25.5k）与 `excelize`（21k）体量大、依赖重，
  可移植性只有 B；而 `bigcache`（8.2k）因设计取向与 Go2Cpp 无 GC 的世界观一致，反而评 A。

### 流程教训（记一笔）

本轮的记录脚本先写 `survey`、后判 `dev-progress` 是否已有内容，判断条件是
`"第 26 轮" not in 文件内容` —— 但文件里**早在第 10~12 轮的待办里就出现过「第 26 轮」字样**，
于是脚本误判为「已存在」而跳过写入，导致记录缺失。**改用更精确的锚点（`## 第 26 轮`）后补写。**
教训与之前那次「编译失败的旧二进制」同源：**用于判断的锚点必须唯一且足够精确**。

### 待办

下一批：`opentelemetry-go`、`sarama`、`amqp091-go`、`asynq`、`fsnotify`、`viper`、`zap`、`testify`；
核验结论最终要写进 `docs/dependencies.md`（含 commit/tag 与日期）。
