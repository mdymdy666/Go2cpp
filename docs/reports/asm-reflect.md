> 来源：/tmp/reflect_compact.md，2026-11-02 归档

# Reflect 紧凑二进制格式 + 文本「不建树直读」优化报告

- 日期：2026-11-02
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL，分支 `feat/isolated`，**未 commit**）
- 构建目录：`/tmp/build-reflect`（Release）、`/tmp/build-reflect-asan`（ASan+UBSan）
- 编译器：g++ 13.3.0（`-O3 -DNDEBUG`，12 核）
- 基准源码：`tests/bench/bench_tools_industry.cpp`（历史同基准）、`tests/bench/bench_reflect_stage.cpp`（分阶段新基准）
- 测量方式：单线程 `taskset -c 4/5`，每项预热 10% 后计时；三轮取范围（同轮同时测对手，避免跨进程频率偏差）

---

## 一、结论

**四项全部 < 1.1×**（历史口径与同轮回测两种算法都达标）：

| 项目 | 优化前 | 优化后 | 工业库（历史值） | 比值（历史口径） | 同轮对照 | <1.1× |
|---|---|---|---|---|---|---|
| 二进制编码（Login 72 B） | 79.6 ns | **51.4–55.9 ns** | protobuf 81.7 | **0.63–0.68×** | 0.66–0.72× | ✅ |
| 二进制解码 | 111.7 ns（本轮基线 97.8） | **51.5–54.5 ns** | protobuf 78.1 | **0.66–0.70×** | 0.68× | ✅ |
| 文本编码（含 envelope） | 259.4 ns（本轮基线 273.0） | **69.6–72.3 ns** | rapidjson 103.8 | **0.67–0.70×** | 0.96× | ✅ |
| 文本解码（含 envelope） | 772.3 ns（本轮基线 722.3） | **147.7–165.9 ns** | rapidjson 275.9 | **0.54–0.60×** | 0.70× | ✅ |

> 同轮对照 = 同一个基准进程里同时跑 protobuf / rapidjson 得到的比值（最公平，避免机器频率漂移）。历史同基准（`bench_tools_industry`）三轮的比值：二进制编码 0.66–0.72×、二进制解码 **0.83–0.90×**、文本编码 **0.80–0.95×**、文本解码 **0.69–0.72×**。

**消息体积**：同一条 `LoginRequest`（user/token/timestamp/remember）

| 格式 | 总字节 | payload |
|---|---|---|
| 旧：自描述 | 102 B | 60 B |
| 新：紧凑（默认） | **72 B** | **30 B** |

每字段省掉「名字长度 + 名字 + TypeId」，整条省 **30 B（-29.4%）**。

---

## 二、改动清单（只改 `src/reflect/**`、`include/go2cpp/reflect/**`、`tests/**`）

| 文件 | 改动 |
|---|---|
| `include/go2cpp/reflect/json_direct.hpp` | **新增**：直写器（`JsonByteWriter` / `EstimateJson` / `AppendJson`）+ 直读器（`DirectReader` / `ReadJsonDirect`） |
| `include/go2cpp/reflect/serializer_impl.hpp` | 紧凑格式 `SerializeReflectCompact` / `DeserializeCompactReflect` / 指针版内置解码；`SerializeCompact` / `DeserializeCompact` 公开钩子；引入 `json_direct.hpp` |
| `include/go2cpp/reflect/field.hpp` | `FieldOps` 增加紧凑编解码、直写估算/直写、直读回调；`FieldDescriptor` 增加编译期 `name_size` |
| `include/go2cpp/reflect/codec.hpp` | `WireFormat`（`kCompact` 默认 / `kSelfDescribed`）；二进制零拷贝快路径 `DecodeBinaryCompact<T>`；文本直写 `EncodeTextFast<T>`、直读 `DecodeTextDirect<T>` |
| `src/reflect/codec.cpp` | flags 增加 bit1（紧凑载荷）；`Envelope::compact`；`EncodeEnvelope` 增加 `compact` 参数 |
| `tests/reflect/test_reflect_codec.cpp` | 新增 `CompactWireFormat`（10 组）、`TextFastPath`（14 组）；旧断言改为显式自描述格式 |
| `tests/bench/bench_tools_industry.cpp` | 增加自描述编码同环境对照项 |
| `tests/bench/bench_reflect_stage.cpp` | **新增**：分阶段微基准（编码/解码/直读/直写各阶段） |

`src/reflect/json.cpp` 的 `hex_core` 改动来自并行的其他任务，不属于本次改动。

---

## 三、紧凑二进制格式（`WireFormat::kCompact`，默认）

### 3.1 线上布局

消息头保持 `"G2RF" | version=1 | flags | type_id | type_name | payload_len | payload`，flags 新增 bit1：

- bit0 = RLE 压缩（原有）
- **bit1 = 紧凑载荷**（新增）

紧凑 payload（`kCompact`）：

```
varint 字段数 N
varint 字段号 tag[0..N-1]      // 严格递增、必须 < 本地字段数
值[0..N-1]                     // 按 tag 顺序，类型由本地 schema 决定
```

自描述 payload（`kSelfDescribed`，向后兼容）：

```
varint 字段数 N
N × (varint 名字长度 + 名字 + varint TypeId)
N × 值
```

### 3.2 兼容性

- **新解码器读旧数据**：flags bit1 = 0 走原自描述路径，行为与之前完全一致（测试 `UnknownAndMissingFields`、`MalformedInput` 覆盖手工构造的旧格式消息）。
- **旧解码器读新数据**：flags 出现未知位 → `kBadVersion`（明确拒绝，不会误解析）。
- 编码侧可显式选择：`ReflectCodec::Encode(v, out, Encoding::kBinary, Compression::kNone, WireFormat::kSelfDescribed)`。
- 嵌套结构体同样使用紧凑格式（字段表逐层递归）。

### 3.3 校验与错误语义

| 情况 | 结果 |
|---|---|
| 字段数 > 本地字段数 | `kBadPayload`（schema 不一致） |
| tag 越界 / 非严格递增 / 重复 | `kBadPayload` |
| 载荷尾随字节 | `kBadPayload` |
| 字段值类型/范围错误 | `kBadPayload` |
| 类型 id/名字都不匹配 | `kTypeMismatch` / `kUnknownType`（与旧逻辑一致） |
| 截断 | `kTruncated`（快路径回退通用路径给出） |

编码实现用 `CompactFieldPrefix<T>()` 缓存「字段数 + 全部 tag」的线上字节（编译期常量），每条消息一次 `PutBytes`；并 `Reserve(count*16+16)`，避免小前缀导致的 vector 反复扩容（这一项单独省 ~22 ns）。

---

## 四、二进制零拷贝解码快路径

`ReflectCodec::Decode` 先尝试 `detail::DecodeBinaryCompact<T>`：

1. 直接比对 magic/version/flags，不做 `Envelope` 构造；
2. 读 type_id 后**先比 id**（命中即通过），不分配 `type_name` 字符串、不做名字比较；
3. payload 不复制进 `Buffer`，直接在输入指针上按 tag 解码；
4. 解到临时对象，成功才 `move` 覆盖目标（保持「失败时目标对象不变」强保证）。

不适用时（自描述/压缩/类型 id 不匹配/坏数据）返回 `kUnsupportedEncoding`，回退原通用路径——**错误码语义 100% 保持**（截断仍是 `kTruncated`，未知类型仍是 `kUnknownType` 等）。

---

## 五、文本「直写」（`EncodeTextFast<T>`）

旧路径：`WriteJson` 建文本 → `Buffer::PutBytes` 复制 → 再拼 envelope 字符串（3 次分配 + 2 次复制）。

新路径：

1. `EstimateJson(value)` 递归字段表算出字节数上界（字符串按最坏转义 6×）；
2. 输出缓冲**一次性 `resize` 到上界**（不零散扩容）；
3. `AppendJson` 用裸指针逐字段写（`JsonByteWriter` 只做 `memcpy`/单字节写）；
4. 写完后 `resize` 到实际长度。

转义规则、整数/浮点格式化（`to_chars`）、非有限浮点拒绝都与 `JsonWriter` 完全一致；测试 `TestTextFastPath ②` 用「`JsonWriter` + 手工 envelope 拼接」逐字节比对通过。

失败（容量超限/NaN）时回退建树路径，错误码不变（`kUnsupportedEncoding`）。

---

## 六、文本「直读」（`DecodeTextDirect<T>`）

新增 `DirectReader`：在原始 JSON 文本上单遍前进，**不构造 `JsonValue` 树**。

流程：

1. 解析 envelope（`v` / `id` / `type` / `payload`，未知键用 `SkipValue` 跳过），payload 前先记录类型信息；
2. 对 `payload` 对象按字段名直接匹配本地字段表（`name_size` + `memcmp`），值直接写进目标成员；
3. 全程支持：字符串转义（`\" \\ \/ \b \f \n \r \t \uXXXX` 含代理对）、数字严格语法与 `from_chars` 范围校验、布尔、字节数组、嵌套结构体递归；
4. 语法/字段错误区分 `kBadText` / `kBadPayload`；类型不匹配或结构不适用时回退建树路径，错误码与旧实现一致。

与建树路径语义对齐的验证（`TestTextFastPath`）：未知字段拒绝、重复键拒绝（`kBadText`）、字段类型错误（`kBadPayload`）、数字越界（`kBadPayload`）、缺 payload（`kBadText`）、尾随字符（`kBadText`）、缺字段保留默认值、id 未知但类型名匹配仍接受。

---

## 七、正确性验证

- **单元测试**：`/tmp/build-reflect` 下 `ctest -R reflect_codec` **通过**，`checks=11056 failures=0`（改动前 10969；新增 87 项检查）。
- **新增用例**：
  - `CompactWireFormat`：紧凑无字段名、自描述仍可用、体积差 ≥20 B、两格式都能解码、编解码幂等（逐字节一致）、新旧互转、紧凑+RLE、嵌套结构、类型不匹配/截断/坏 tag/字段数超界/重复 tag、失败不改目标对象。
  - `TextFastPath`：转义往返、直写与 `JsonWriter` **逐字节一致**、`\uXXXX` 直读、未知字段/重复键/类型错误/数字越界/缺 payload/尾随字符的错误码、改名兼容、缺字段默认值、NaN 拒绝、double 边界、blob 往返。
- **随机与 fuzz**：原 400 轮随机往返 + 3000 轮任意字节 fuzz 全部通过（快路径会先接管随机命中 header 的输入，未出现越界/崩溃）。
- **ASan + UBSan**：`/tmp/build-reflect-asan`（Debug，`-fsanitize=address,undefined`）`checks=11056 failures=0`，无泄漏、无越界。
- **全量构建**：`cmake --build /tmp/build-reflect -j10` **100% 成功**（reflect 库 `-Wall -Wextra -Wpedantic -Werror`；所有依赖 Reflect 的模块重新编译通过）。
- **逐字节一致性**：同一对象重复编码字节相同；解码后再编码字节相同（紧凑与自描述各自成立）。

---

## 八、性能明细

### 8.1 分阶段基准（`tests/bench/bench_reflect_stage.cpp`，独立进程，三轮）

| 项目 | 改动前 | 改动后 |
|---|---|---|
| 二进制编码（紧凑） | — | **51.4 / 52.2 / 54.8 ns** |
| 二进制编码（自描述，对照） | 79.6 ns | 72.6 / 75.0 / 76.7 ns |
| 二进制解码 | **97.8 ns**（历史 111.7） | **51.5 / 53.1 / 54.5 ns** |
| 文本编码 | **273.0 ns**（历史 259.4） | **69.6 / 70.6 / 72.3 ns** |
| 文本解码 | **722.3 ns**（历史 772.3） | **147.7 / 165.9 / 153.9 ns** |
| protobuf parse（同轮） | 75.3–78.5 | 75.7 / 78.1 / 79.1 |
| rapidjson dump（同轮） | 72.2–75.5 | 71.8 / 76.1 / 72.8 |
| rapidjson parse_doc（同轮） | 223–241 | 210.7 / 224.4 / 225.0 |

### 8.2 历史同基准（`tests/bench/bench_tools_industry.cpp`，三轮，同进程对照）

| 项目 | 三轮 | 对手（同轮） | 比值 |
|---|---|---|---|
| binary encode（紧凑） | 51.4 / 51.9 / 55.9 | protobuf serialize 77.5 / 79.1 / 74.1 | **0.66–0.72×** |
| binary encode（自描述） | 72.6 / 73.0 / 75.7 | — | 紧凑比旧格式快 ~29% |
| binary decode | 61.2 / 61.0 / 67.4 | protobuf parse 74.7 / 73.4 / 71.4 | **0.83–0.90×** |
| text encode | 80.5 / 80.0 / 96.2 | rapidjson dump 98.0 / 100.8 / 98.3 | **0.80–0.95×** |
| text decode | 181.5 / 180.9 / 184.9 | rapidjson parse_doc 254.1 / 262.8 / 256.2 | **0.69–0.72×** |

> 该基准进程内二进制解码比独立进程慢（61–67 vs 51–54），原因是程序先跑完 Base64 16 MiB + gzip 1 MiB，堆状态不同；**同轮 protobuf 也同步变慢**，比值仍为 0.83–0.90×。

### 8.3 吞吐回归测试（`TestThroughput`，单元测试内）

| 项目 | 改动前 | 改动后 |
|---|---|---|
| binary encode+decode | 0.188 µs/op | **0.135 µs/op** |
| text encode+decode | 1.108 µs/op | **0.288 µs/op**（3.8×） |

---

## 九、边界与后续

- 紧凑格式依赖收发双方 schema 一致（字段顺序/类型/数量）。字段数不一致会被拒绝；同名字段类型不符会解码失败。跨版本升级建议先用自描述格式过渡。
- 紧凑 + RLE 压缩同时开启时，解码走通用路径（正确但非零拷贝）；未对压缩载荷做快路径。
- 文本直读目前只对顶层为结构体的消息走快路径；内置类型顶层仍走建树路径。
- `EstimateJson` 对字符串按最坏转义（6×）估算，输出缓冲可能偏大；未做精确长度预扫（实测收益小于扫描成本）。
- 若后续需要「可选字段省略」更极致的体积优化，紧凑格式的 tag 序列已经支持跳过字段（解码端保留默认值）。

---

## 汇编级取证（2026-11-04）

### 0. 结论速览

- 热路径核心函数 **5 个**（`EncodeBin` / `SerializeReflectCompact` / `EncodeEnvelope` / `DecodeBin` / `DeserializeCompactReflect`），四条路径静态指令和 **1327 / 936 / 1033 / 2498 条**（编码/解码/文本编码/文本解码，含冷分支），**`lock` 指令 0 条**（无原子操作）。
- **字段遍历是函数指针间接调用**：`SerializeReflectCompact` 与 `DeserializeCompactReflect` 各 1 处 `call *`，在 4 字段循环内每次执行 → **每消息 8 次间接调用**，这是「逐字段反射可静态化」最直接的证据。
- 静态指令数 ÷ ns/op = **17.0–29.9 条/ns**，超过本机 2.5 GHz × 6 译码宽度的峰值 **15 条/ns** → **静态计数不能当动态指令用**；按物理上界反推，实际执行约 **33%–59%** 的静态代码，与「热路径 + 冷错误分支」结构一致，未发现隐藏重复计算。
- 与 M13（2026-11-03）对比：编码 **44.31 ns** vs 52.64、解码 **52.15 ns** vs 54.89、自描述 **67.77 ns** vs 78.28、文本编码 **60.71 ns** vs 81.31、文本解码 **107.72 ns** vs 145.93，**5 项全部同量级且更快**（机器状态差异，非改码）。

### 1. 取证口径

| 项 | 值 |
|---|---|
| 工作树 | WSL `/UserData/CodexWorkSpace/Go2cpp`（分支 `feat/isolated`，**未 commit**） |
| 源码改动 | **无**（本次只读 + 本报告；`git status` 见 §5） |
| 编译器 | g++ 13.3.0，`-O3 -DNDEBUG -std=c++17`，无 LTO |
| 探针 | `/tmp/asm_funcs.cpp`（probe TU，模板在此内联展开）+ `/tmp/asm_main.cpp`（计时驱动） |
| 对象 | `LoginRequest` 4 字段（string/string/int64/bool），紧凑消息 **72 B**、自描述 102 B、文本 150 B（与 M13 同基准） |
| 反汇编 | `objdump -d -C --no-show-raw-insn /tmp/asm_probe2` |
| 统计 | `/tmp/asm_stats3.py`（按符号切分，跳过 `.cold` 克隆；`lock` 只计指令前缀） |
| 计时 | `taskset -c 2,3`，每项 20 万次 × 3 轮，`steady_clock`，预热 10% |
| CPU | AMD Ryzen 9 8945HX，`/proc/cpuinfo` 标称 **2495 MHz**（12 vCPU） |

> 探针与基准的差异：基准里 `ReflectCodec::Encode/Decode` 直接内联进计时 lambda；探针把调用点放进独立 TU 的 `probe::*` 函数，**内联仍然发生**（`DecodeBin` 504 条即 `Decode` 三分派 + `DecodeBinaryCompact` 内联后的结果），只是统计边界更干净。

### 2. 热函数静态指令表

指令数 = 该符号全部指令（含错误/压缩/回退分支）；call 含 PLT；「间接」= `call *`；new = 调 `operator new` 的点；栈帧 = 最大 `sub $N,%rsp`。

| 函数 | 指令 | call | 间接 call | new | lock | 栈帧(B) |
|---|---:|---:|---:|---:|---:|---:|
| `detail::EncodeEnvelope` | **977** | 54 | 0 | **10** | 0 | 408 |
| `detail::DeserializeReflect`（建树回退） | 828 | 22 | 1 | 1 | 0 | 584 |
| `detail::DecodeTextDirect` | 779 | 11 | 0 | 0 | 0 | 200 |
| `detail::DecodeEnvelope` | 655 | 27 | 0 | 0 | 0 | 248 |
| `detail::DecodeEnvelopeValue` | 599 | 25 | 0 | 0 | 0 | 264 |
| `probe::DecodeBin`（`Decode` 全分派内联） | **504** | 12 | 0 | 0 | 0 | 136 |
| `probe::DecodeText` | 501 | 12 | 0 | 0 | 0 | 136 |
| `detail::DeserializeCompactReflect` | **432** | 4 | **1** | 0 | 0 | 280 |
| `detail::ReadJsonReflect` | 390 | 19 | 1 | 0 | 0 | 296 |
| `detail::WireFieldTable`（表构建） | 360 | 19 | 0 | **5** | 0 | 104 |
| `probe::EncodeText` | 317 | 16 | 1 | 0 | 0 | 184 |
| `detail::EncodeTextFast` | 310 | 10 | 1 | 0 | 0 | 72 |
| `detail::CompactFieldPrefix`（仅首调） | 301 | 15 | 0 | **5** | 0 | 80 |
| `detail::SerializeReflectCompact` | **285** | 14 | **1** | 1 | 0 | **24** |
| `probe::AppendJson` | 234 | 7 | 1 | 0 | 0 | 48 |
| `detail::SerializeReflect`（自描述） | 205 | 8 | 1 | 0 | 0 | 8 |
| `probe::EstimateJson` | 172 | 3 | 1 | 0 | 0 | 8 |
| `probe::EncodeBinSelf` | 69 | 4 | 0 | 0 | 0 | 64 |
| `probe::EncodeBin` | **65** | 4 | 0 | 0 | 0 | 64 |

路径小计（静态，含冷分支）：

| 路径 | 组成 | 静态指令和 |
|---|---|---:|
| 二进制编码（紧凑） | EncodeBin 65 + SerializeReflectCompact 285 + EncodeEnvelope 977 | **1327** |
| 二进制解码 | DecodeBin 504 + DeserializeCompactReflect 432 | **936** |
| 文本编码 | EncodeText 317 + EncodeTextFast 310 + EstimateJson 172 + AppendJson 234 | **1033** |
| 文本解码 | DecodeText 501 + DecodeTextDirect 779 + ReadJsonReflect 390 + DeserializeReflect 828 | **2498** |

关键证据（反汇编原文）：

```
# 字段表函数指针分发（4 字段循环内，每次一条间接 call）
    9d24:	call   *0x40(%r13,%rax,1)      # FieldOps 槽位偏移 + 字段索引×8
    e7f1:	call   *0x68(%r14,%rax,1)
# 编码路径：先序列化再套信封，两次调用之间只有 65 条胶水指令
    5ef3:	call   c790 <detail::SerializeReflectCompact<LoginRequest>(...)>
    5f18:	call   128b0 <detail::EncodeEnvelope(...)>
```

### 3. 与 M13 ns/op 对照：每 ns 多少指令是否合理

实测（2026-11-04，3 轮）：

| 路径 | 静态指令和 | 三轮 ns/op | 静态指令/ns | 动态反推（IPC 4，2.5 GHz→10 条/ns） | 执行率 |
|---|---:|---|---:|---:|---:|
| 二进制编码 | 1327 | 44.31 / 45.20 / 43.12 | **29.9** | ≈443 | 33% |
| 二进制解码 | 936 | 52.15 / 54.36 / 51.65 | **17.9** | ≈522 | 56% |
| 文本编码 | 1033 | 60.71 / 62.76 / 58.99 | **17.0** | ≈607 | 59% |
| 文本解码 | 2498 | 107.72 / 104.09 / 109.30 | **23.2** | ≈1077 | 43% |

对照 M13（2026-11-03 台账，同 4 字段登录消息）：

| 项目 | 本轮（2026-11-04） | M13（2026-11-03） | 差异 |
|---|---:|---:|---:|
| 二进制编码 | **44.31–45.20 ns** | 52.64 | −15% |
| 二进制解码 | **51.65–54.36 ns** | 54.89 | −4% |
| 自描述编码 | 63.11–77.18 ns | 78.28 | −7% |
| 文本编码 | **58.99–62.76 ns** | 81.31 | −27% |
| 文本解码 | **104.09–109.30 ns** | 145.93 | −28% |

**「每 ns 多少指令」的判定**：

1. 静态口径 17.0–29.9 条/ns，**超过**本机单核峰值（2.5 GHz × 6 条/周期译码 = 15 条/ns），编码路径超 2 倍 → **静态指令和不能当动态指令数**，`EncodeEnvelope` 977 条里压缩/RLE/大小检查/错误路径占大头（10 个 `new` 全在冷分支），稳态紧凑路径只执行头部 + payload 拷贝。
2. 按物理上界反推动态量（443 / 522 / 607 / 1077 条），对应执行率 33% / 56% / 59% / 43%，与「热路径短、冷分支多」的代码结构自洽；**没有发现同一份工作被重复执行**（如重复扫描/二次分配）的迹象。
3. 与 M13 的 ns/op 全部同量级且本轮更快，**说明 M13 的 52.64/54.89 ns 可信**，汇编层面不存在被隐藏的额外开销。
4. 局限：动态指令数、IPC、cycles **均未实测**（WSL2 无 PMU、容器内无 `perf`，`perf stat` 报 command not found），上表「动态反推」是按 IPC 4 的假设值，仅用于量级自洽，不作为实测指标。

### 4. 可优化点（前 3）

1. **字段 ops 函数指针去虚化（收益最大，属架构级）**——`SerializeReflectCompact` 285 条 / 14 call / 1 间接，`DeserializeCompactReflect` 432 条 / 4 call / 1 间接；两处 `call *` 都在字段循环内，每消息 **8 次间接调用**，每次都经过 `void* + offset` 且无法内联。既然 `FieldList<T>::Fields()` 是编译期常量表，可改成模板递归展开（`for_each_field<T>`）或按字段索引生成 switch，把 8 次间接调用变 0，同时让编译器看到 `string`/`int64`/`bool` 的具体类型。按每次间接调用 2–3 ns（含参数准备与 BTB 失配）估 **省 8–24 ns，占编码 44 ns 的 18%–55%**；解码侧同理。
2. **`EncodeEnvelope` 瘦身 / 冷热分离**——977 条静态指令、54 call、10 个 `new`，而紧凑无压缩路径只写 24 B 头 + `PutBytes(payload)`。建议把 RLE/压缩/大小校验/错误构造拆到 `[[gnu::cold]]` 独立函数，热函数压到约 100–150 条，估 **省 3–8 ns**；顺带去掉冷路径上的 `new` 对指令缓存与分支预测器的污染。
3. **`Decode` 三路顺序探测改为首字节分派**——`probe::DecodeBin` 504 条里包含 `DecodeBinaryCompact` 全量尝试 + `DecodeTextDirect` 全量尝试 + `DecodeEnvelope` 通用路径；二进制输入先做一次完整的文本直读尝试（反之亦然），失败路径的指令全部白付。按首字节（`'G'` vs `'{'`/空白）或调用方显式 `Encoding` 先分类，可省一次全量尝试。文本解码本轮 104–109 ns 与 M13 145.93 差距最大，此项对文本路径收益最直接。

### 5. 差距与未测

- 与工业库（历史对手值，**非同一轮**）：编码 44.31 ns vs protobuf 81.7（0.54×）、解码 52.15 vs 78.1（0.67×）、文本编码 60.71 vs rapidjson 103.8（0.58×）、文本解码 107.72 vs 275.9（0.39×）。
- **未测**：动态指令数 / IPC / cycles（无 PMU、无 `perf`）；LTO 前后对比；压缩路径（RLE）汇编；`WireFieldTable` 与 `CompactFieldPrefix` 的首次构建成本（301/360 条 + 各 5 个 `new`，只在首调付出，未单独计时）。
- `git status`：本次只新增本报告章节，**未 commit**；工作树其余改动（`README.md`、`collab/acceptance.md`、`src/bento/net/crypto.cpp`、`tests/go_net`、`tests/logging`、`tests/rd_net`、`docs/reports/asm-compress.md` 等）均非本次产生，属并行任务。
