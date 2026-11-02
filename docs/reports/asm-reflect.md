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
