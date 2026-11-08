# JSON 数值类型契约（double / Decimal）

适用：`go2cpp/json`（GCC 13.3 / libstdc++，C++17）。实现：`include/go2cpp/json/json_build.hpp`
（写 API）、`src/json/writer.cpp`（`Value` → 文本）、`include/go2cpp/json/core/decimal.hpp`。
测试：`tests/json/test_number_contract.cpp`（1,008,438 checks，含 100 万位模式往返）。

## double 契约

| 场景 | 正确行为 | 测试 |
|---|---|---|
| 整值 `1.0` | 输出 `1.0`（不是 `1`） | 边界表 / 整值 -200..200 |
| 最短往返 | `to_chars(general)`，`parse(build(x)) == x` **位级相等** | 100 万随机位模式 |
| `-0.0` | 输出 `-0.0`（符号位保留） | 边界表 + 容器符号 |
| `NaN` / `±Inf` | **报错**：`ok()=false` + `error()`，不写任何字节；绝不输出 `nan`/`inf`/`null` | `TestNonFiniteRejected` |
| `DBL_MAX` / `DBL_MIN` / denormal | 输出可解析回同一位模式 | 边界表 / writer 极端值 |
| `1e300` / `1e-300` | 指数形式保留，必须往返 | 边界表 |
| 前文含 `.` 的字符串 | 不影响后续 double 的 `.0` 判定 | `TestPrefixInterference` |
| 前文含 `e`/`E` 的字符串 | 同上 | 同上 |

实现要点：`.0` 判定只在**本次 `to_chars` 的输出缓冲**里找 `.`/`e`/`E`，绝不搜索
`m_out`（整串搜索会让 `"a.b"` 之后的 `1.0` 输出成 `1`）。`writer.cpp` 走同一套规则；
它遇到非有限浮点返回 false 且不写任何字节。

## Decimal 契约

- 表示：符号 + 十进制数字串 + 小数位（`scale`），任意精度；规范化为无前导零、无尾随零，
  零恒为 `"0"`（`-0` 解析后也是 `"0"`）。
- 写入：`JsonBuilder::Decimal` 直接输出 `Decimal::ToString()`（JSON 数字，不套引号）；
  `writer` 对 Decimal 节点同样输出 `ToString()`（十进制展开，不带指数）。
- 往返：`Decimal → ToString → Decimal::Parse` 值相等（`Compare == 0`），且二次
  `ToString` 完全一致（小数位数稳定）。
- 边界：`0` / `-0` / 0 位小数（整数）/ 最大数量级 `1e±1000000`（指数上限 10^6，
  `1e-1000001` 报 `kOutOfRange`）/ 小数位受 1 MiB 输入上限与 `scale ≤ 2^20` 约束。
- **JSON 文本不能保 Decimal 类型**：JSON 没有 Decimal；小数形态回读成 double（可能丢精度），
  int64 范围内的整数回读成 int。只有超出 int64 / double 的数值才回读成 Decimal。
  精确往返在 `Decimal → Decimal` 层保证，不要依赖 JSON 文本保类型。

## 与 nlohmann 3.11.3 的实测差异

| 输入 | `nlohmann::json::dump()` | 本库 |
|---|---|---|
| `1.0` | `1.0` | `1.0` |
| `-0.0` | `-0.0` | `-0.0` |
| `DBL_MAX` | `1.7976931348623157e+308` | 同 |
| `NaN` / `±Inf` | **`null`（静默，不抛异常）** | **`ok()=false`，不写字节** |
| Decimal | 无此类型 | 有，定点精确 |

结论：nlohmann 3.11.3 对非有限浮点输出 `null`（实测），下游无法区分空值与坏值；
本库选择显式拒绝。`null` 不是「能解析的合法数字」，所以不能照抄。

## 失败实验（真实做过的）

1. **整串 `m_out.find('.')`**：前文 `"a.b"` 让后续 `1.0` 漏 `.0`（历史 bug）。改为只扫
   本次 `to_chars` 缓冲后修复。变异验证：把判定改回全串查找，`TestPrefixInterference`
   立即失败（`["a.b",1.0]` 变成 `["a.b",1]`，回读成 int）。
2. **`chars_format::fixed`**：对 `1.0` 仍输出 `1`（fixed 也是最短定点，不强制小数点），
   而 `DBL_MAX` 膨胀成 309 位数字——不解决问题，拒绝。
3. **全部用 `chars_format::scientific`**：永远含 `e`、无需补 `.0`，但 `0.1` 变成 `1e-01`
   （更长、可读性差），整数也不再紧凑，拒绝。
4. **writer 对 NaN/Inf 写 `null` 占位**（与 nlohmann 一致）：改为不写字节 + 返回 false，
   避免下游把坏数值当合法空值。

## 验证

```bash
cd /UserData/CodexWorkSpace/Go2cpp
g++ -std=c++17 -O1 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    tests/json/test_number_contract.cpp src/json/*.cpp src/json/core/*.cpp \
    src/tools/error.cpp src/reflect/type.cpp -o /tmp/tnc && /tmp/tnc
# ALL TESTS PASSED (1008438 checks, 136 ms)
```

原 10 个可编译的 JSON 测试全部 PASS（其中 8 个带 checks 输出，合计 142,454，与改动前
基线一致）；另 7 个测试是库并入时的命名空间迁移残留（`extened::` / `go2cpp::reflect::kDecimalType`），
与本契约无关。
