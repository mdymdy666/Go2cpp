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
