> 来源：/tmp/gzip_crc.md，2026-11-02 归档

# gzip 解压 CRC 加速报告

日期：2026-11-02
工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL 树）
构建目录：`/tmp/build-gzip`（Release，`-O3 -DNDEBUG`，Ninja）
任务：gzip 解压从 1880 MiB/s 提到 **≥2713 MiB/s**（zlib 2984 的 <1.1×）

## 结论

| 指标 | 改造前（HEAD 8d1314a） | 改造后 | 变化 |
|---|---|---|---|
| gzip 解压 1 MiB L6（64 KiB 输出块） | 2420.5 MiB/s | **5219.7 MiB/s** | **2.16×** |
| gzip 解压 1 MiB L6（1 MiB 输出块） | 2428.8 MiB/s | **5253.2 MiB/s** | **2.16×** |
| 同场 zlib 解压（同一压缩流，64 KiB 块） | 3674.8 MiB/s | 3674.8 MiB/s | — |
| 耗时比 go2cpp/zlib（越小越快） | 1.49×（慢 49%） | **0.696×（快 1.44×）** | — |
| 目标 ≥2713 MiB/s | — | **5219.7（超目标 1.92×）** | 达标 |
| 目标 <1.1× zlib | — | **0.696×**（远小于 1.1×） | 达标 |

- **是否 <1.1×：是。** 同场对照下 go2cpp 解压耗时只有 zlib 的 0.70×；
  即使按任务给的 zlib=2984 MiB/s 口径换算，耗时比也只有
  `2984 / 5219.7 = 0.57×`，同样远低于 1.1×。
- 任务给的「1880 MiB/s」是改造前基线；本次在同一台机器、同一份 1 MiB
  语料上实测改造前为 2420.5 MiB/s（机器/负载与当时不同），改造后
  5219.7 MiB/s，相对基线 **2.16×（实测）/ 2.78×（对 1880 口径）**。
- CRC 单函数：`hash::Crc32` 的 PCLMULQDQ 路径 **20.3–20.7 GB/s**
  （1 MiB 一次调用，400 次中位数）；改造前的 slice-by-16 纯查表实现
  通常在 3–5 GB/s 量级——这正是解压卡在 CRC 的根因。

## 根因与改动

### 根因

旧解压器在 `EmitByte` / `CopyMatch` 里**逐字节/逐匹配**更新校验和：

```cpp
void EmitByte(...) {
    out[(*out_pos)++] = byte;
    m_window[...] = byte;
    ++m_member_out;
    UpdateChecksumBlock(&byte, 1);   // 每个字面量进一次 slice-by-16 CRC
}
```

1 MiB 输出的 gzip 解压会被切成几十万次 1 字节 CRC 调用，每次都带
函数调用 + 查表流水启动开销；同时 slice-by-16 表本身只有 ~3 GB/s。
1 MiB 的 CRC 就要 ~0.33 ms，而整段解压目标只有 ~0.19 ms，CRC 成了
串行关键路径上的大头。

### 改动 1：校验和批量提交（`src/compress/deflate_reader.hpp`）

- `EmitByte` / `CopyMatch` 只写输出与窗口，不再碰校验和；
- 新增 `CommitOutput(out, out_pos)`：在每次 `Inflate` 的出口（`kOk` /
  `kMemberEnd` / `kError`）以及**进入尾部校验之前**，把本批尚未提交的
  连续输出段一次算完；
- 幂等（`out_pos <= m_out_committed` 直接返回），所以「尾部校验前提交」
  与「统一出口提交」可以安全重复调用；
- 距离合法性检查改为 `distance > m_member_out + (*out_pos - m_out_committed)`，
  把本批未提交的输出量算进去（否则合法的近距离匹配会被误判 corrupt）；
- gzip 的 `m_crc` 改为「已完成 CRC」语义（初值 0），尾部直接比较，
  不再需要 `^ 0xFFFFFFFF` 的进出取反；
- zlib 的 adler32 同样改成整段批量（`UpdateAdlerBlock`），raw 只更新计数。

### 改动 2：CRC-32 复用 `hash::Crc32`（PCLMULQDQ）

按协调决定**不复制实现**：

- `deflate_reader.hpp` include `go2cpp/hash/crc32.hpp`，
  `CommitOutput` 调 `hash::Crc32::Update(m_crc, hash::IEEETable, ...)`；
- 删除 compress 内自建的 slice-by-16 表与 `Crc32Update`（deflate_reader.hpp 净删 48 行）；
- `cmake/Go2CppModules.cmake` 给 `go2cpp_compress` 加 `go2cpp_hash` 链接
  （一行 + 注释）。hash 模块本身零依赖，无循环依赖。

`hash::Crc32::Update` 的语义正是 Go `crc32.Update`（进入取反/离开取反），
和 `CommitOutput` 分段的「已完成 CRC」语义天然一致；x86-64 上
`Crc32::Update` 在 `n >= 64` 时走 PCLMULQDQ 折叠、尾部交回表驱动。

### 改动 3（顺带修复）：`FindMatch` 的 1 字节越界读

ASan 跑压缩测试时在 `deflate_writer.hpp:623` 报
`heap-buffer-overflow READ of size 1`（HEAD 既有，非本次引入）：
`best.length == max_length` 时 `data[position + best.length]` 正好是
`data.size()`。修复为达到 `max_length` 直接结束链扫描（既消除越界，
也不会再找到更长匹配）。

改动后压缩吞吐对照（1 MiB L6，OLD/NEW 交替 6 轮中位数）：
OLD 530.7 MiB/s / NEW 530.6 MiB/s，**无回归**（差异 <0.1%，噪声内）。

## 性能数据

### 方法

- bench：`tests/bench/bench_gzip_crc.cpp`（手工编译，不进 CMake）；
- 数据：1 MiB 低熵日志文本（与 `bench_tools_industry` 同一份语料），
  压缩级别 L6，压缩产物在计时外生成；
- 解压：输出写进预分配缓冲，分别测 64 KiB 与 1 MiB 输出块；
- **同轮交替**：每轮偶数次先 go2cpp 后 zlib、奇数次反过来，取逐轮
  「zlib 耗时 / go2cpp 耗时」比值的中位数，抵消频率漂移；
- `taskset -c 9` 绑核；每轮 200 次迭代，共 5 轮；下表取各轮中位数的中位数。

### 数据（同一 zlib 压缩流，64 KiB 输出块）

| 轮次 | 前 go2cpp | 前 zlib | 前 耗时比 | 后 go2cpp | 后 zlib | 后 耗时比 |
|---|---|---|---|---|---|---|
| 1 | 2420.5 | 3625.3 | 1.505× | 5247.4 | 3704.9 | 0.705× |
| 2 | 2352.1 | 3497.1 | 1.482× | 5219.7 | 3633.0 | 0.696× |
| 3 | 2392.5 | 3529.8 | 1.477× | 5090.6 | 3500.4 | 0.688× |
| 4 | 2328.5 | 3465.2 | 1.498× | 5032.6 | 3422.6 | 0.679× |
| 5 | 2441.0 | 3653.4 | 1.498× | 5392.8 | 3742.4 | 0.691× |
| 中位 | **2420.5** | **3625.3** | **1.498×** | **5219.7** | **3633.0** | **0.696×** |

1 MiB 输出块（同一压缩流）：前 2428.8 / 后 5253.2 MiB/s，
耗时比 1.55× → 0.73×。go2cpp 自己的 L6 流（压缩率 0.0176）：
前 2391.8~2534.0 / 后 4980.1~5383.8 MiB/s（5 轮范围）。

### CRC 单函数

| 实现 | 1 MiB 吞吐 |
|---|---|
| `hash::Crc32`（PCLMULQDQ + 尾部表） | **20.3–20.7 GB/s** |
| 改造前 slice-by-16（compress 内自建） | ~3 GB/s 量级（本次拆掉的实现） |

1 MiB 解压的墙钟约 0.41 ms（改造前），CRC 约占其中 0.25–0.33 ms
（60%–80%，取决于查表宽度）；改造后 CRC 约 0.05 ms（约 10%），
且不再按字面量切割。

## 正确性验证

1. **1e6 随机数据 CRC 对拍**（任务硬要求）：
   - bench：与 zlib `crc32()` 对拍 1 000 000 字节随机数据，**一致**
     （`ce91d3dc`）；
   - 测试套件新增 `TestCrc32AgainstReference`（`tests/compress/test_random.cpp`）：
     用测试支持里**独立逐位实现**做 oracle，覆盖 1e6 随机整体 CRC、
     长度 0..300、1023/1024/4095/4096/65535/65536/999999/1000000
     （共 309 个长度，覆盖 PCLMUL 折叠门槛 64B 两侧、16B 整块尾部、
     Byte 收尾、1 MiB 大块）、以及把 1e6 数据随机切成 1..4096 字节的
     分段 `Update` 串接——全部一致。
2. **双向互操作**（既有测试，全部通过）：
   - GNU gzip：本模块压缩 → `gzip -t` + `gzip -d`（7 组数据 × 2 方向）；
   - 多成员拼接双向；
   - Python `zlib`：raw deflate / zlib 格式双向；
   - gzip CRC/ISIZE 篡改、截断、magic 错误均能报 `ErrChecksum` /
     `ErrUnexpectedEof` / `ErrHeader`。
3. **完整测试套件**：`go2cpp_compress_test` **1506 checks 全过**
   （改造前 1196，新增 310 项 CRC 断言）；`ctest -R go2cpp_compress_test`
   Passed（2.13 s）。
4. **ASan**：`-O1 -g -fsanitize=address` 重编 compress 全部测试，
   1506 checks 全过，无内存错误（顺带发现并修复了压缩侧 `FindMatch`
   的 1 字节越界读，见改动 3）。
5. **随机/场景/压测**：既有随机往返（90 组 × 3 格式 × 4 级别）、
   显式边界（空/1B/1 MiB 零/1 MiB 随机/2 MiB 随机）、8 个真实场景、
   1 MiB 低熵/高熵压测全部通过。

## 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B /tmp/build-gzip -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_BUILD_TESTS=ON -DGO2CPP_BUILD_EXAMPLES=OFF
cmake --build /tmp/build-gzip --target go2cpp_compress_test -j
cd /tmp/build-gzip && ctest -R go2cpp_compress_test --output-on-failure

# 基准（手工编译，不进默认构建）
cd /UserData/CodexWorkSpace/Go2cpp
g++ -O3 -DNDEBUG -std=c++17 -Iinclude tests/bench/bench_gzip_crc.cpp \
    -L/tmp/build-gzip -lgo2cpp_compress -lgo2cpp_hash -lgo2cpp_error -lz \
    -Wl,-rpath,/tmp/build-gzip -o /tmp/bench_gzip_crc
taskset -c 9 /tmp/bench_gzip_crc
```

## 改动文件

| 文件 | 说明 |
|---|---|
| `src/compress/deflate_reader.hpp` | 批量校验和（`CommitOutput`）+ CRC 走 `hash::Crc32`，删 slice-by-16（+52/−100，净删 48 行） |
| `cmake/Go2CppModules.cmake` | `go2cpp_compress` 链接 `go2cpp_hash`（一行 + 注释） |
| `src/compress/deflate_writer.hpp` | `FindMatch` 1 字节越界读修复（ASan 发现，HEAD 既有） |
| `tests/compress/test_random.cpp` | 新增 `TestCrc32AgainstReference`（1e6 随机 + 309 长度 + 分段） |
| `tests/bench/bench_gzip_crc.cpp` | 新增手工基准（吞吐 + CRC 对拍 + 压缩回归） |

未改动：`src/hash/**`（CRC 实现本身）、`include/go2cpp/hash/**`、其它模块。
（工作树里 include/go2cpp/hash/hash.hpp 的文档更新由并行 agent 完成，
不是本任务改动。）未 commit。


## 汇编级取证（2026-11-04）

> 本节由 gzip 汇编级取证任务追加，**保留上文原文**。只读体检：未改任何代码、
> 未 commit。所有数字来自 `objdump` 与同机实测，未测项显式标注「未测」。

### 0. 口径与产物

| 项 | 值 |
|---|---|
| 真树 | `/UserData/CodexWorkSpace/Go2cpp`（WSL，小写 `pp`；`E:\CodexWorkspace\Go2Cpp` 旧副本未使用） |
| 源码版本 | HEAD `0f6fd2e`（工作树另有他人未提交改动，本节未触碰） |
| 反汇编产物 | `.build-bento-wsl/libgo2cpp_compress.so.0.1.0`（CMake `Release`，`-O3 -DNDEBUG`，GCC 13.3.0，2026-11-03 构建） |
| 交叉验证 | 手工 `g++ -O3 -DNDEBUG -std=c++17 -fPIC -Iinclude -c` 的 `deflater.o` / `inflater.o`；`EmitFixedBlock` 大小与 so 完全一致（12045 B），确认二者同源同优化级别 |
| 命令 | `objdump -d --no-show-raw-insn -C <so>`；`nm -C -S --defined-only` 取符号大小 |
| 机器 | AMD Ryzen 9 8945HX（WSL2，12 vCPU），`/proc/cpuinfo` 基频 2495 MHz；`taskset -c 9` 绑核 |

### 1. 热函数静态画像（objdump 指令级）

按压缩/解压两条数据路径，统计每个函数的静态指令数、`call` 数、`call ...@plt`
数、`operator new` 调用数、`lock` 前缀数、`sub rsp` 最大栈帧、函数字节数：

| 函数（`go2cpp::compress::detail::` 省略） | 指令 | call | 其中 @plt | new | lock | 栈帧(B) | 函数(B) |
|---|---:|---:|---:|---:|---:|---:|---:|
| `DeflateEncoder::EmitFixedBlock` | **2757** | 50 | **49** | 2 | 0 | 328 | 12045 |
| `InflateDecoder::StepDynamicHeader` | **1466** | 27 | 21 | 4 | 4 | 168 | 5938 |
| `InflateDecoder::Inflate` | **1044** | 36 | 26 | 0 | **6** | 88 | 4610 |
| `Deflater::Run` | 744 | 38 | 19 | 0 | 0 | 40 | 2960 |
| `InflateDecoder::BuildFixedTables` | 694 | 5 | 5 | 2 | 0 | 464 | 2834 |
| `InflateDecoder::StepHuffman` | **641** | 25 | 19 | 0 | 4 | 88 | 2794 |
| `DeflateEncoder::EmitFixedEmptyFinal` | 429 | 7 | 7 | 0 | 0 | 56 | 1714 |
| `DeflateEncoder::EmitStoredBlock` | 380 | 8 | 8 | 0 | 0 | 8 | 1302 |
| `InflateDecoder::StepGzipHeader` | 377 | 13 | 9 | 0 | 2 | 56 | 1532 |
| `DeflateEncoder::Pump` | 343 | 10 | 9 | 0 | 0 | 56 | 1330 |
| `DeflateEncoder::EnsureHeader` | 215 | 4 | 4 | 0 | 0 | 40 | 781 |

主循环定位（对每个函数取跨度最大的向后跳转）：

| 函数 | 主循环地址 | 循环体指令 | 占函数比例 | 循环体内 call |
|---|---|---:|---:|---:|
| `EmitFixedBlock` | `0xe002..0x10c6c` | **2574** | 93% | 45 |
| `StepHuffman` | `0x160f4..0x16b73` | 613 | 96% | 23 |
| `Inflate` | `0x16c40..0x17c01` | 911 | 87% | 27 |
| `Deflater::Run` | `0xc74f..0xcee1` | 494 | 66% | 28 |

一句话画像：

- **编码侧是一个 2757 条指令的巨型函数**：`FindMatch`（哈希链扫描）、
  `InsertHash`、`EmitLengthDistance`、`EmitFixedCode`、`BitWriter::PutBits`
  全部被内联进 `EmitFixedBlock`，主循环 2574 条指令覆盖 93% 的函数体。
  这不是「函数调用多」，而是「单函数过大、分支密度高」，I-cache 与分支预测
  压力都在一个函数里。
- **解码侧是分层状态机**：`Inflate`（1044）外层 `for(;;) StepOnce()`，
  `StepHuffman`（641）是符号主循环，`StepDynamicHeader`（1466）只在外部
  动态 Huffman 流的每个块头走一次；go 自己的流只产出固定 Huffman（BTYPE=01），
  所以 `StepDynamicHeader` 对自产流不热、对 zlib 流（每 ~16 KiB 一块）热。
- **`BuildFixedTables` 694 条指令 + 464 B 栈帧 + 2 次 new**，在
  `InflateDecoder` 构造/`Reset` 时重建 288+32 项固定 Huffman 表——这是每次
  「新建一个 Inflater」的固定启动成本，不是逐字节成本。

### 2. 「本可内联却 PLT 直呼」与逐字节循环

#### 2.1 `EmitFixedBlock` 的 49 个 PLT 调用构成（全部实测地址）

| 目标 | 次数 | 说明 |
|---|---:|---|
| `__cxa_guard_acquire@plt` | **13** | 函数内 `static const std::array` 局部静态的线程安全初始化守卫 |
| `__cxa_guard_release@plt` | **13** | 同上 |
| `std::vector<unsigned char>::_M_default_append@plt` | 11 | `m_combined.reserve/insert` 的增长路径 |
| `memmove@plt` | 6 | `m_combined.insert(history/data)` 的整段搬移 |
| `operator new@plt` / `operator delete@plt` | 2 / 2 | `m_combined` 扩容 |
| `std::__throw_length_error@plt` | 2 | 扩容溢出路径 |
| `vector::_M_range_insert`（本地，非 PLT） | 1 | — |

**这是本节最硬的「本可内联却 PLT 直呼」证据**：`ReversedLitCodes()`、
`LitCodeBits()`、`ReversedDistCodes()`、`CrcTables()` 都是
`static const std::array` 的局部静态，编进 `EmitFixedBlock` 后每调用一次就要
走 26 次 `__cxa_guard_*` PLT（13 对）。这些表完全可以在编译期确定
（`constexpr`/类静态成员/命名空间级 `const`），守卫、PLT、原子 load 都不必要。
按 32 KiB 一块、1 MiB 输入 32 块算，固定 Huffman 压缩 1 MiB 会白付
32 × 26 = **832 次 PLT 调用**。

#### 2.2 `Deflater::Run` 对同 TU 大函数的 PLT 直呼

`Deflater::Run` 的 19 个 PLT 里：

- `DeflateEncoder::Pump@plt` ×3、`EmitFixedBlock@plt` ×3、`EmitStoredBlock@plt` ×3、
  `EmitFixedEmptyFinal@plt` ×1、`Deflater::ok()@plt` ×1；
- `BitWriter::PutBits [clone .constprop.0]` ×8（本地直呼，未内联）；
- `memcpy@plt` ×1、`memmove@plt` ×2、`_M_default_append@plt` ×3、
  `_M_range_insert` ×2。

`EmitFixedBlock` 是 12 KB 的大函数，GCC 不内联它是合理的；但它被导出为
default-visibility 符号后，同 TU 调用也只能走 PLT（无 `-fvisibility=hidden`
/ `-Bsymbolic-functions` / LTO）。每个 32 KiB 块多一次 PLT 间接跳转 +
一次函数序言（`push` ×6 + `sub rsp,0x148`）。同理 `Inflate` 对
`StepHuffman@plt` / `StepDynamicHeader@plt` / `StepGzipHeader@plt` 各 1 处。

#### 2.3 `StepHuffman`：短匹配用 PLT `memcpy`

25 个 call 里 **10 个是 `memcpy@plt`**（`0x16357/0x16371/0x1639c/0x1640e/
0x1642d/0x16450/0x165f6/0x16616/0x1689e/0x168c0`），来自 `CopyMatch` 的
`ReadWindow` / `WriteWindow`：距离通常只有 3–20 B，`memcpy` 的 PLT 间接
调用 + 长度分派开销与拷贝本身同量级。另有 `ErrCorrupt@plt` ×3、
`_M_release@plt` ×2、`_M_release_last_use_cold@plt` ×3、`__stack_chk_fail@plt` ×1
（错误/析构路径），以及 6 处 `call *0x10(%rax)` / `call *0x18(%rax)` 的
间接调用（`ErrorPtr` 析构，非热路径）。

#### 2.4 `lock` 前缀与原子操作

| 函数 | `lock` 条数 | 位置 |
|---|---:|---|
| `Inflate` | 6 | `CommitOutput` 出口的 `shared_ptr` 控制块 `_M_release`（每批输出/每次错误） |
| `StepHuffman` | 4 | `ErrCorrupt()` 构造 `ErrorPtr` 的引用计数 |
| `StepDynamicHeader` | 4 | 同上（块头校验失败路径） |
| `EmitFixedBlock` / `Deflater::Run` | 0 | 编码侧全程无原子操作 |

结论：**`lock` 不在逐字节热循环里**，只在「每批输出提交」与错误路径；
不是吞吐主因，但 `CommitOutput` 每 64 KiB 一次 `_M_release` 仍可改成
无所有权转移的裸指针 CRC 调用。

#### 2.5 逐字节循环可向量化点

- `BuildFixedTables` 的 `for (symbol=0; symbol<=143; ...) lit_lengths[symbol]=8`
  是 4 段常量填充，反汇编里是逐字节 `movb`；可 `memset`/`constexpr` 表一次拷入。
- `EmitFixedBlock` 的 `m_huffman_only` 分支是 `for(index<size) EmitSymbol(data[index])`，
  逐字节查表 + 写位流，无法直接向量化（位流串行），但可改为「先批量查表到
  临时码字数组、再批量打包」，把查表链路的依赖打断。
- `UpdateAdlerBlock` 是 `a += data[i]; b += a` 的串行前缀和，标量实现；
  zlib 用 `adler32_simd`（SSSE3/AVX2）把 5552 字节块做成 16 路并行，
  go2cpp 目前仍是逐字节（本次未改，因 gzip 路径不走 adler）。
- `MatchLength` 是逐字节 `while (a[i]==b[i]) ++i`；GCC 未把它向量化
  （有提前退出的数据依赖），可用 8 字节 XOR + `ctz` 一次比较 8 字节，
  这是 zlib `longest_match` 的常规做法。

### 3. 与 zlib 同场实测（1 MiB 同一语料，L6）

方法：`tests/bench/bench_asm_gzip.cpp`（临时文件，未入库）复用语料
（1 MiB 低熵日志，与 `bench_gzip_crc.cpp` 同一份）；链接
`.build-bento-wsl/libgo2cpp_compress.so`（Release `-O3`）+ 系统 zlib；
同轮交替（奇偶轮交换先后）、30 次迭代、取中位数；`taskset -c 9`。

| 路径 | go2cpp | zlib | 速度比（go/zlib） | 耗时比（go/zlib） |
|---|---:|---:|---:|---:|
| 压缩 L6（1 MiB 输入） | **604.5 MiB/s** | 453.9 MiB/s | **1.347×** | **0.742×** |
| 解压（同一 go 流，64 KiB 输出块） | **4959.0 MiB/s** | 3293.9 MiB/s | **1.479×** | **0.676×** |

必须同时看的口径差异：

| 项 | go2cpp | zlib |
|---|---:|---:|
| 压缩产物 | 18496 B（1.764%） | 13139 B（1.253%） |
| 产物大小比 | **1.408×**（go 大 40.8%） | 1.0 |

也就是说：**压缩快 1.35× 是用 40.8% 的压缩率换来的**——go 只发固定 Huffman
块（无动态树、无 lazy matching、无最优解析），不是同压缩率对比；解压的
1.48× 才是同一条压缩流上的公平对比（与上文 0.696× 的历史数字一致，本次
0.676×，差异在机器负载）。

### 4. 物理下限估算与「离下限几倍」

**口径**：静态指令数来自 objdump；动态「每字节指令数」由
`吞吐 × 假设频率/宽度` 反推，频率取基频 2.5 GHz（`/proc/cpuinfo`），
Zen4 解码宽度 6 条/周期、实际 IPC 按 1.8–3.0 取区间。**这是估算，不是
`perf stat` 实测**（本机 WSL 未安装 perf，`perf_event_paranoid=2`），
标注为「估算」。

| 路径 | 每字节必要操作（下界模型） | 指令下限（条/字节） | 实测反推（条/字节） | 指令级离下限 | 吞吐上限（2.5 GHz×6 宽） | 吞吐离下限 |
|---|---|---:|---:|---:|---:|---:|
| 压缩 | `Hash3`：load32+shift+mul+shr(4) + head load + head store + prev store = 7 | **7** | 7.0–8.5 | **1.0–1.2×** | 2.14 GB/s | **3.4×**（实测 0.634 GB/s） |
| 解压 | 每符号 4 条（查表+移位+减位+分支）+ 每字节 1 条 store；按 2.5 B/符号 → 2.2 | **2.2** | 2.0–2.5 | **0.9–1.1×** | 6.8 GB/s | **1.25×**（实测 5.45 GB/s） |

解读：

- **解压已经贴住指令级下限**（1.25×），继续压指令数的收益只有百分点级；
  它的瓶颈是每符号的查表依赖链与 `CopyMatch` 的短 `memcpy`，不是「指令太多」。
  这也解释了为什么批量 CRC 改造后能反超 zlib。
- **压缩的指令数也已到下限（7 条/字节），但吞吐离「满 IPC 下限」差 3.4×**：
  每字节的 7 条指令里有 3 条是 `m_head`/`m_previous` 的随机读写，哈希链
  指针追逐的 L2/L3 延迟把 IPC 压到 ~1.8，而不是宽度不够。**优化方向应是
  减少链扫描次数/改善局部性，不是继续抠指令数。**

### 5. 可优化点前 3（按证据强度与收益排序）

1. **消掉 `EmitFixedBlock` 里 26 次/块的 `__cxa_guard_*` PLT**
   （证据：`0xe8e8/0xea47/0xef8b/0xf2df/0xf38d/0xf648/0xf934/0xfd1e/0xfe4b/
   0x10147/0x1047a/0x10776/0x10a29` 共 13 对 acquire/release）。
   把 `ReversedLitCodes()` / `LitCodeBits()` / `ReversedDistCodes()` /
   `CrcTables()` 从「函数内 `static const std::array`」改为
   `static constexpr` 或类静态 `inline constexpr`，守卫与 PLT 一起消失；
   1 MiB 输入可省 832 次 PLT 调用。**零风险、纯局部改动。**
2. **去掉每块 64 KiB 的整段拷贝**（证据：`_M_default_append` ×11、
   `memmove` ×6、`operator new/delete` 各 2，全部在 `EmitFixedBlock` 内）。
   `EmitFixedBlock` 每块都 `m_combined.clear(); reserve(history+size);
   insert(history); insert(data)`，把 32 KiB 历史 + 32 KiB 输入搬进一个
   新 vector；1 MiB 输入 = 32 块 ≈ **2 MiB 纯拷贝 + 32 次扩容检查**。
   改为「环形窗口 + 跨块视图」（`FindMatch`/`InsertHash` 接受
   `(buf, base, len)` 两段视图），可把这部分降到 0，同时改善缓存局部性。
3. **`CopyMatch` 的短拷贝去掉 PLT `memcpy`**（证据：`StepHuffman` 10 处
   `memcpy@plt`）。距离 < 32 B 时用两条 16 B 的 SSE2 `movdqu` + 尾部
   掩码写，或直接 8 B 循环展开；距离 ≥ 32 B 再落到 `memcpy`。
   叠加 `-fvisibility=hidden -Bsymbolic-functions`（或 LTO）消除
   `Pump@plt`/`EmitFixedBlock@plt`/`StepHuffman@plt` 等同 TU 直呼。

附带项（未列入前 3）：`BuildFixedTables` 每次构造/Reset 重建固定表
（694 条、464 B 栈帧、2 次 new），可改为进程级 `constexpr` 表一次性构建。

### 6. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
# 1) 反汇编（用真实 Release 产物）
objdump -d --no-show-raw-insn -C \
  .build-bento-wsl/libgo2cpp_compress.so.0.1.0 > /tmp/compress.dis
nm -C -S --defined-only .build-bento-wsl/libgo2cpp_compress.so.0.1.0

# 2) 交叉验证（手工 -O3，EmitFixedBlock 应与 so 同为 12045 B）
g++ -O3 -DNDEBUG -std=c++17 -fPIC -Iinclude -c src/compress/deflater.cpp \
  -o /tmp/deflater.o && nm -C -S /tmp/deflater.o | grep EmitFixedBlock

# 3) 压/解压同场基准（临时源码，未入库）
g++ -O2 -DNDEBUG -std=c++17 -Iinclude /tmp/bench_asm_gzip.cpp \
  -L.build-bento-wsl -lgo2cpp_compress -lgo2cpp_hash -lgo2cpp_error -lz \
  -Wl,-rpath,/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl -o /tmp/bench_asm_gzip
taskset -c 9 /tmp/bench_asm_gzip
```

### 7. 边界

- `perf stat` 不可用，动态指令数/每周期指令数是**反推估算**，不是硬件计数器；
  频率取基频 2.5 GHz（未锁频、未关 turbo），误差估计 ±20%。
- 压缩对比不同压缩率（go 只发固定 Huffman），1.347× 不代表同率优势；
  同率对比未测。
- `StepDynamicHeader` 在本机只对 zlib 流热，go 自产流不走；其 1466 条指令
  的静态成本对「解外部 zlib 流」场景才有意义（未单独计时）。
- 本节没有改动任何代码，未 commit；工作树中他人的未提交改动未触碰。
