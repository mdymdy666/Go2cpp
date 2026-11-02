> 来源：/tmp/tools_vs_industry.md，2026-11-02 归档

# Go2cpp 工具类模块 × 工业库对标（Base64 / gzip / reflect JSON）

日期：2026-11-02
工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL），构建 `.build-bento-wsl`（Release，`-O3 -DNDEBUG`，gcc 13.3）
测量：单线程 `taskset -c 2`，1 MiB 日志文本 / 16 MiB 随机字节，三轮取代表值。
基准源码：`tests/bench/bench_tools_industry.cpp`、`tests/bench/tools_bench.proto`、`/tmp/gobench/main.go`（Go）。

## 一、结论总表

| 项目 | 优化前 | 优化后 | 工业库（同机） | 比值 | <1.1× |
|---|---|---|---|---|---|
| Base64 编码 16 MiB | 1004 MiB/s | **1980 MiB/s** | Go `encoding/base64` 1663 | **0.84×** | ✅ |
| Base64 解码 16 MiB | 600 MiB/s | **1566 MiB/s** | Go 1582 | **1.01×** | ✅ |
| Base64 编码 60 B | 58.5 ns | **30.9 ns** | Go 41.9 ns | **0.74×** | ✅ |
| gzip 压缩 1 MiB L6 | 235.7 MiB/s | **550 MiB/s** | zlib 421 | **0.77×** | ✅ |
| gzip 解压 1 MiB | 486.8 MiB/s | **1880 MiB/s** | zlib 2984 | **1.59×** | ❌ |
| reflect 二进制编码（60 B 登录消息） | 225.6 ns | **79.6 ns** | protobuf 81.7 ns | **0.97×** | ✅ |
| reflect 二进制解码 | 159.3 ns | **111.7 ns** | protobuf 78.1 ns | **1.43×** | ❌ |
| reflect 文本编码（含 envelope） | 340.4 ns | **259.4 ns** | rapidjson 103.8 / nlohmann 477.3 | 2.50× / **0.54×** | ❌ / ✅ |
| reflect 文本解码（含 envelope） | 929.3 ns | **772.3 ns** | rapidjson 275.9 / nlohmann 809.3 | 2.80× / **0.95×** | ❌ / ✅ |
| JSON 文档解析（285 B） | 334.3 ns | **292.4 ns** | rapidjson 275.9 | **1.06×** | ✅（边缘） |

**达标 7 项 / 未达标 3 项**（按「同一对手」口径；文本项对手 nlohmann 已达标、对手 rapidjson 未达标）。
未达标的共同原因见第五节，均已定位到明确的下一步（PCLMULQDQ CRC、去 envelope 的 SAX 直读）。

## 二、Base64（`include/go2cpp/encoding/base64.hpp`、`src/encoding/base64.cpp`）

### 改了什么

1. **反查表移到构造期**：旧 `Decode` 每次调用 `fill(-1)` + 64 次写重建 256 项表（小输入时比解码本身还贵）。现在 `Base64Encoding` 用 `constexpr` 构造把表展开成 256 字节成员，四个预定义实例在编译期就是常量表。
2. **编码单次 32-bit store**：旧实现每个输出字符一次 `push_back`（容量检查 + 长度更新）。现在先 `resize` 到最终长度，每 3 字节用一次 32-bit `memcpy` 写 4 个字符。
3. **解码单遍 + 线程本地 scratch**：旧实现「先全量扫描非法字符 → 再解码 → 每字节 push_back 到临时串」。现在正常路径单遍 quantum 解码（4 字节 store 写 3 字节），`thread_local std::string` 复用缓冲，零堆分配；只有长度错误时才走「先找第一个非法字符」的慢路径（保证 `"Zm9v!"` 仍报 `input byte 4`）。
4. 端序安全：32-bit store 由 `__BYTE_ORDER__` 分支，大端走逐字节路径。

### 汇编证据（objdump）

| 函数 | 指令数 | call | 栈帧 |
|---|---|---|---|
| `Base64Encoding::Encode` | 127 | 2 | 24 B |
| `Base64Encoding::Decode` | 436 | 31（错误路径 + `thread_local` guard） | 56 B |

正常解码路径没有 `operator new`/`memcpy` 调用点（scratch 只在首次或扩容时分配）。

### 结果

编码 1004 → **1980 MiB/s**（Go 1663，0.84×）；解码 600 → **1566 MiB/s**（Go 1582，1.01×）；60 B 58.5 → **30.9 ns**（Go 41.9，0.74×）。
Go 的 `encoding/base64` 本身是标量 3→4 字节展开；本实现用同样的数据布局 + 预置表，已到同一量级，SIMD 不是达标所必需（保留为后续余量）。

## 三、gzip（`src/compress/deflate_writer.hpp`、`src/compress/deflate_reader.hpp`）

### 压缩（235.7 → 550 MiB/s，zlib 的 0.77×）

1. **BitWriter 改 32 位整字落盘**：旧实现每字节一次 `push_back`；新实现 64 位累加器 + 一次 32-bit store，`Align()` 按 `m_bits & 7` 补位（32 位落盘会残留 8~31 位）。
2. **固定 Huffman 反转码表**：288 项 literal/length + 32 项 distance 的「已反转码字 + 位宽」静态表，消除每符号一次位反转循环。
3. **哈希链跨块持久**：位置改用流内绝对偏移、前驱表 32 KiB 环形索引；不再每 32 KiB 重建 128 KiB `head` 表、也不再把 32 KiB history 逐位置重插一遍。顺带获得跨块匹配（压缩率不变差）。
4. 匹配长度比较改 8 字节一跳 + `ctz` 定位差异（原逐字节）。
5. gzip 只算 crc32、zlib 只算 adler32（原来两样都算）；CRC 改 slice-by-8。

汇编：`DeflateEncoder::EmitFixedBlock` 2949 条指令 / 50 call / 328 B 栈帧（含内联的 LZ77 主循环）。

### 解压（486.8 → 1880 MiB/s，zlib 的 1.59×）

1. **LZ77 匹配整段拷贝**：旧实现每匹配字节一次 `EmitByte`（输出检查 + 32 KiB 环形窗口取模 + 校验和）。现在一次 `CopyMatch`：距离 ≥ 长度走两次 `memcpy`（环形跨界），距离 < 长度先落 `distance` 字节再倍增复制；随后一次批量写窗口、一次批量校验和。
2. **校验和按格式批量**：gzip 走 slice-by-16 CRC32（16 KiB 表，16 字节一批，xor 树并行），zlib 走 adler32 分块公式，raw 完全不更新（旧实现两种格式都逐字节更新）。
3. 结果：1 MiB 日志（压缩比 56:1）解压从 486.8 → **1880 MiB/s**。

汇编：`InflateDecoder::StepHuffman` 879 条指令 / 21 call / 88 B 栈帧；`CopyMatch` 与 `Crc32Update` 被内联进该函数。

## 四、reflect JSON（`src/reflect/**`、`include/go2cpp/reflect/**`）

### 改了什么

1. **`Buffer::GetStringView`**：字段名匹配不再为每个字段构造 `std::string`（旧实现每条消息 4 次分配）。
2. **`DeserializeReflect` 零分配**：匹配结果放栈数组（32 槽），只在字段数 > 32 时退回堆。
3. **`DecodeBinary` 指针直读**：旧实现把整条消息复制进 Buffer、payload 复制进 `raw`、再复制进 `Envelope.payload`（60 B 消息 3 次分配 + 3 次 memcpy）；现在直接在同一指针上解析，payload 只落一次。
4. **payload move**：`DecodeEnvelopeValue` 接收非 const Envelope，二进制分支 `std::move(envelope.payload)`，消除整段复制。
5. **字段表按类型缓存**：`WireFieldTable<T>()` 只构造一次（count + 每字段 name/type_id 的线上字节），每条消息一次 `PutBytes`。
6. **`JsonWriter::AppendEscaped` 零临时串**：旧实现每个字符串调用 `EscapeJsonString` 返回新 `std::string` 再拷贝；现在扫描到首个需转义字节后整段 `append`。
7. **`JsonParser` 字符串整段 append**（原逐字符 `push_back`）、对象 `members.reserve(8)`、`Envelope` 直接写 `out`（少一次中间 Buffer + 搬移）。

### 结果

| 路径 | 优化前 | 优化后 | 工业库 | 比值 |
|---|---|---|---|---|
| 二进制编码 | 225.6 ns | 79.6 ns | protobuf 81.7 ns | 0.97× |
| 二进制解码 | 159.3 ns | 111.7 ns | protobuf 78.1 ns | 1.43× |
| 文本编码 | 340.4 ns | 259.4 ns | rapidjson 103.8 / nlohmann 477.3 | 2.50× / 0.54× |
| 文本解码 | 929.3 ns | 772.3 ns | rapidjson 275.9 / nlohmann 809.3 | 2.80× / 0.95× |
| JSON 解析（285 B） | 334.3 ns | 292.4 ns | rapidjson 275.9 | 1.06× |

汇编：`JsonParser::Parse` 453 条指令 / 21 call / 216 B 栈帧（递归下降，含深度/长度校验）。

## 五、未达标项与下一步

| 项 | 比值 | 原因 | 下一步 |
|---|---|---|---|
| gzip 解压 | 1.59× | zlib 1.3 在 x86 上用 PCLMULQDQ 折叠 CRC32（约 10 GB/s），我们用 slice-by-16（约 3~4 GB/s），CRC 仍是主瓶颈 | 引入 PCLMULQDQ CRC32（16 B/次折叠 + Barrett 归约），预计解压到 2.8~3.2 GB/s |
| reflect 二进制解码 | 1.43× | 自描述格式每字段带 `name + type_id`（本消息多 29 B），protobuf 只有 tag；剩余开销是 4 个字段的函数指针 decode 与目标字符串赋值（1 次分配） | 字段 name 的 `strlen`/比较可改预计算 hash；decode 回调可改为按 type_id 的 switch（牺牲扩展性） |
| reflect 文本编解码 | 2.50× / 2.80× | 文本模式多一层 `{"v","id","type","payload"}` envelope；rapidjson 是 SAX/DOM 极致优化且不写 envelope。对 nlohmann 已 0.54× / 0.95× | 若要对标 rapidjson，需要「不建树、直接按 schema 读 token」的直读解码器（工作量约 1~2 天） |

## 六、正确性

- `ctest -R 'encoding|compress|reflect'`：**5/5 通过**。
  - `go2cpp_encoding_test`（RFC 4648 向量、Python 参照向量、256 字节全值往返、随机/压力）全过。
  - `go2cpp_compress_test` **1196 checks** 全过：含 GNU gzip / Python zlib 双向互操作、多成员、stored/fixed 回退、损坏检测、短写/空闲源。
  - `go2cpp_reflect_codec_test` **10969 checks, 0 failures**：含随机往返、3000 轮 fuzz、RLE 压缩炸弹、帧粘包/半包、6 个真实场景。
- 全量构建 `.build-bento-wsl`（496 个目标，`-Wall -Wextra -Wpedantic -Werror`）成功，无 ABI 破坏（`Base64Encoding` 增加 256 B 私有成员，所有包含方已重编）。

## 七、复现

```bash
# C++ 侧（go2cpp + zlib + protobuf + nlohmann + rapidjson）
cd /UserData/CodexWorkSpace/Go2cpp
protoc --cpp_out=tests/bench/gen tests/bench/tools_bench.proto
g++ -O3 -DNDEBUG -std=c++17 -Iinclude -Itests/bench/gen -Itests/compress \
    tests/bench/bench_tools_industry.cpp tests/bench/gen/tools_bench.pb.cc \
    tests/compress/compress_test_support.cpp \
    -L.build-bento-wsl -lgo2cpp_reflect -lgo2cpp_encoding -lgo2cpp_compress \
    -lgo2cpp_error -lz -lprotobuf -o /tmp/bench_tools
LD_LIBRARY_PATH=$PWD/.build-bento-wsl taskset -c 2 /tmp/bench_tools

# Go 侧
GO111MODULE=off go build -o /tmp/gobench/bench /tmp/gobench/main.go
taskset -c 3 /tmp/gobench/bench
```

> 说明：`tests/bench/*` 为手工编译的基准，未改 CMake 目标，不进入默认构建。
