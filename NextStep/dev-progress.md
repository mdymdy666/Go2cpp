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
