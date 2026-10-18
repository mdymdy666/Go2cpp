# JsonScanDepth 扫描器状态机

- 源文件：`include/go2cpp/tools/json.hpp`（header-only，类之外只有自由函数）
- 关键函数：`go2cpp::JsonScanDepth`
- 相关入口：`go2cpp::ParseJson` 在**进入 nlohmann 解析器之前**调用它
- 状态载体：函数内两个局部布尔量 `in_string`、`escaped`（没有独立的状态枚举）

## 为什么单独记一份状态机

`JsonScanDepth` 是整个 JSON 限深栅栏的唯一实现：它必须在不递归、不分配、`noexcept`
的前提下回答「最大嵌套有多深」。状态只有三个，但字符串内外的括号语义完全相反，
一旦把 `escaped` 与 `in_string` 的先后顺序写错，`"[[[["` 会被当成 4 层嵌套，
栅栏就会误伤合法输入（或漏放非法深文档）。这份表是审查该函数的第一手依据。

## 转化表（一行一个转化）

| 当前状态 | 触发（函数） | 下一状态 | 说明 |
|---|---|---|---|
| Normal | `JsonScanDepth` 读到 `"` | InString | 进入字符串字面量；随后括号不再计数 |
| Normal | `JsonScanDepth` 读到 `{` 或 `[` | Normal | `depth++`，刷新 `deepest`；若 `deepest > stop_at` 立即返回 |
| Normal | `JsonScanDepth` 读到 `}` 或 `]` 且 `depth > 0` | Normal | `depth--`（多余的右括号被忽略） |
| Normal | `JsonScanDepth` 读到其它字符 | Normal | 忽略（数字、逗号、空白、冒号等） |
| InString | `JsonScanDepth` 读到 `\` | Escaped | 标记「下一个字符被转义」 |
| InString | `JsonScanDepth` 读到 `"` | Normal | 字符串结束 |
| InString | `JsonScanDepth` 读到其它字符 | InString | 字符串内容，包含 `{`/`[`/`}`/`]` 也不计深度 |
| Escaped | `JsonScanDepth` 读到任意字符 | InString | 清除 `escaped`；即使该字符是 `"` 或 `\` 也不结束字符串 |
| 任意状态 | `JsonScanDepth` 观测到 `deepest > stop_at` | 返回 | 提前终止；返回值是真实深度的**下界** |

## 不变量

- `escaped == true` 蕴含 `in_string == true`：转义标记只可能在字符串内部置位。
- `in_string == false` 时 `escaped` 必为 `false`（进入 Normal 前已由 Escaped 分支清除）。
- `depth >= 0`：减少前先判 `depth > 0`，不存在无符号回绕。
- `deepest >= depth`：`deepest` 记录历史最大值，而 `depth` 只可能不超过它。
- 字符串内部出现的括号不改变 `depth`；`"[[[["` 这类输入返回 0。
- 整个函数不分配、不递归、不抛异常（`noexcept`），时间 O(n)、额外空间 O(1)。

## 非法转化（不存在 / 被忽略）

- **Escaped → Normal 不存在**：反斜杠后的引号是被转义的引号（如 `"a\"b"`），
  必须回到 InString 而不是结束字符串。
- **InString → Escaped 以外的分支处理引号**：只有 InString 且不在 Escaped 时，
  `"` 才表示字符串结束。
- **Normal 下右括号不匹配不会进入错误状态**：`depth == 0` 时 `}`/`]` 被直接忽略；
  语法合法性由上游客忍解析器负责，本扫描器**不做校验**。
- **超限不是错误状态**：函数直接返回，由 `ParseJson` 把它翻译成
  `Wrap(JsonDepthExceededError(), ...)`；扫描器本身不构造错误对象（保证 `noexcept`）。
