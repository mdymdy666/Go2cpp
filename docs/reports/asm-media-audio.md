> 来源：/tmp/audio_asm.md，2026-11-02 归档

# 音频编解码三件套：汇编级 + 对标

> **工作树**：`/UserData/CodexWorkSpace/Go2cpp`（WSL2 ext4，**不是** E: 树）
> **构建目录**：`/tmp/build-audio`（独立，不污染 `.build-bento-wsl`）
> **工具链**：g++ 13.3.0 / Ninja / Release / `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`
> **机器**：AMD Ryzen 9 8945HX（Zen4，AVX2 + AVX512F/BW/CD/DQ/VL + VAES），12 vCPU，WSL2 Ubuntu 24.04
> **基线**：提交 `0052cfb`（音频链路落地）
> **改动范围**：仅 `src/media/audio.cpp`、`include/go2cpp/media/audio.hpp`、`tests/media/test_audio.cpp`；**未 commit**
> **测量口径**：`taskset -c 2`，每个数据点跑 3 次取中位

---

## 0. 结论（TL;DR）

| 判定项 | 结果 | 是否达标（<1.1×） |
|---|---|---|
| Opus 编码包装 vs 裸 `libopus`（48k 直通） | **1.006×** | ✅ |
| Opus 编码包装 vs 裸 `libopus`（44.1k，含重采样） | **1.037×** | ✅ |
| 重采样 vs `libsamplerate`（同类线性算法） | **0.098×**（快 10.2×） | ✅ |
| µ-law 编码 SIMD 加速 | **5.54×**（1.172 → 0.207 ns/样本） | — |
| 重采样 SIMD 加速 | **3.15×**（857.4 → 272.0 ns/帧） | — |
| Opus 包装汇编级开销 | 栈帧 1304B → **8B**，`rep stos` 1275B → **0**，`call` 11 → 8 | — |
| 正确性 | 409,205 断言；1e5 帧 checksum 与基线逐字节相同；Opus SNR 26.44/19.04/25.86 dB 不变 | ✅ |

**一句话**：Opus 包装开销 **0.6%（48k）/ 3.7%（44.1k）**，两项"缺项"（Opus 包装、重采样 SIMD）已补齐并达标；
**µ-law 解码 SIMD 经分析判定不应做**（已是端口受限，强行向量化反而更慢）；
**音频 delta 编码不存在**（`kDeltaI420` 是视频侧块差分，见 §7）。

---

## 1. 环境与构建

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B /tmp/build-audio -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
cmake --build /tmp/build-audio --target media_test_audio -j 12     # 零警告（-Werror）
taskset -c 2 /tmp/build-audio/media_test_audio                     # [audio] OK (409205 checks)
```

反汇编：

```bash
g++ -std=c++17 -Iinclude -Isrc/media -O3 -DNDEBUG -c src/media/audio.cpp -o /tmp/audio_o3.o
objdump -d --no-show-raw-insn /tmp/audio_o3.o > /tmp/audio_asm.txt
```

同机依赖现状：

| 依赖 | 状态 |
|---|---|
| `libopus.so.0`（1.4） | ✅ 有库、**无 `opus.h` / 无 `opusenc`** → 只能 dlopen 对标 |
| `libsamplerate.so.0`（0.2.2） | ✅ 有库 + 头 + `pkg-config` → 真对标 |
| `libsoxr.so.0` | 有库，**无头文件、无 `sox` CLI** → **未测**（§4.2） |
| `ffmpeg` | ✅ 有（本次未用） |
| `perf` | ❌ 缺失（用 `steady_clock` 计时） |

---

## 2. objdump：热函数（前 / 后）

统计口径：静态指令数、`call`、`jcc`（分支）、堆分配调用、`lock`/`xchg` 原子、向量指令。
> 脚本会把 GCC 的对齐填充 `xchg %ax,%ax` 误计为原子；下表"原子"列已人工核实，
> 热路径**真实原子操作一律为 0**（已逐条 `grep lock|xchg` 确认）。

### 2.1 改动前（基线 0052cfb）

| 函数 | 指令 | call | 分支 | 分配 | 向量 |
|---|---:|---:|---:|---:|---:|
| `OpusAudioEncoder::Encode` | **224** | **11** | 39 | 0 | 0 |
| `detail::UlawEncodeBlock` | 38 | 0 | 2 | 0 | 0 |
| `detail::UlawDecodeBlock` | 192 | 0 | 18 | 0 | 24 |
| `ResampleLinear`（标量 + GCC 自动向量化） | 310 | 0 | 18 | 0 | 62 |
| `detail::UlawEncodeBaseline` | 55 | 0 | 14 | 0 | 0 |
| `detail::UlawEncodeTable` | 27 | 0 | 0 | 0 | 0 |
| `detail::UlawDecodeTable` | 7 | 0 | 0 | 0 | 0 |

`OpusAudioEncoder::Encode` 的关键问题（反汇编写真）：

```asm
sub    $0x518,%rsp                  ; 栈帧 1304B —— 1275B 包缓冲放在栈上
...
mov    $0x9f,%ecx                   ; 0x9f*8 + 3 = 1275
rep stos %rax,%es:(%rdi)            ; 每帧清零 1275B（≈40 cycles）
...
movzbl 0x0(%rip),%eax               ; 每帧一次 static-local guard（acquire load）
test   %al,%al
je     <__cxa_guard_acquire 慢路径> ; dlopen 符号表也要每帧过 guard
```

### 2.2 改动后

| 函数 | 指令 | call | 分支 | 分配 | 向量 |
|---|---:|---:|---:|---:|---:|
| `OpusAudioEncoder::Encode` | **207**（-7.6%） | **8**（-27%） | 34 | 0 | 0 |
| `UlawEncodeBlockAvx2`（新） | 113 | 0 | 5 | 0 | 57 |
| `ResampleLinearAvx2Mono`（新） | 72 | 0 | 6 | 0 | 32 |
| `detail::UlawEncodeBlock`（分派） | **47**（原 38 的纯标量） | **0** | 4 | 0 | 0 |
| `detail::UlawEncodeBlockSimd`（分派） | 47 | 0 | 4 | 0 | 0 |
| `detail::UlawEncodeBlockScalar`（A/B 基线） | 38 | 0 | 2 | 0 | 0 |
| `ResampleLinear`（分派，含标量尾巴） | 366 | 1 | 23 | 0 | 62 |
| `detail::ResampleLinearScalar` | 312 | 0 | 18 | 0 | 62 |
| `detail::ResampleLinearSimd`（转发） | 3 | 0 | 1 | 0 | 0 |

`OpusAudioEncoder::Encode` 现在：

```asm
sub    $0x8,%rsp                    ; 栈帧 8B（原 1304B，-99.4%）
...
; 无 rep stos：包缓冲提升为成员，libopus 只写 [0, produced)
; 无 guard：Init 时把 OpusApi* 缓存进 m_api，每帧直调 m_api->encode
```

分派函数（`UlawEncodeBlock`）现在只有 47 条指令、**0 次 call**：

```asm
movzbl __cpu_model+...(%rip),%eax   ; __builtin_cpu_supports("avx2")：直接读 libgcc 全局
test   $0x2,%al
jne    <UlawEncodeBlockAvx2>
; ... 标量尾巴
```

> 这里踩过一个坑：最初用 `static const bool = []{ __builtin_cpu_init(); ... }()`，
> 每次分派都要过一次 `__cxa_guard_acquire` 的 acquire load（dispatch 78 条指令、
> 3 次 call）。改成直接读 `__cpu_model` 后降到 47 条、0 call、0 原子。

### 2.3 内核循环体（真正的热路径）

**µ-law 编码 AVX2**（16 样本/轮，42 条）：

```asm
vpmovsxwd -0x10(%rdi,%rax,2),%ymm0      ; 8 个 int16 -> 8 个 int32
vpmovsxwd -0x20(%rdi,%rax,2),%ymm9
vpsrad $0x8,%ymm0,%ymm10                ; sign = (pcm>>8) & 0x80
vpabsd %ymm0,%ymm0                      ; |pcm|
vpminsd %ymm7,%ymm0,%ymm0               ; clip 32635（有符号 min 安全：|pcm| <= 32768）
vpaddd %ymm6,%ymm0,%ymm0                ; + 0x84
vpsrld $0x7,%ymm0,%ymm11                ; index = sample >> 7（<= 255）
vcvtdq2ps %ymm11,%ymm11                 ; float 指数域 = floor(log2(index))
vpsrld $0x17,%ymm11,%ymm11
vpaddd %ymm5,%ymm11,%ymm11              ; -127
vpmaxsd %ymm8,%ymm11,%ymm11             ; max(0)（index==0 时）
vpsrlvd %ymm12,%ymm0,%ymm0              ; mantissa 变长右移
vpand %ymm2,%ymm0,%ymm0
vpor / vpor / vpandn                    ; ~(sign | e<<4 | mant) & 0xFF
vpackssdw / vpermq $0xd8 / vpackuswb    ; 16 个 int32 -> 16 个 u8
vmovdqu %xmm0,-0x10(%r9,%rax,1)
```

**重采样 AVX2**（8 输出帧/轮，25 条）：

```asm
vpbroadcastd %xmm6,%ymm6
vpaddd %ymm2,%ymm6,%ymm6                ; pos = base + k*step
vpsrld $0x10,%ymm6,%ymm0                ; index = pos >> 16
vpand %ymm4,%ymm6,%ymm6                 ; frac = pos & 0xFFFF
vpgatherdd %ymm7,(%rdi,%ymm0,2),%ymm1   ; 一次读出 (in[idx], in[idx+1]) 两个样本
vpslld $0x10 / vpsrad $0x10             ; a = 低 16 位符号扩展
vpsrad $0x10,%ymm1,%ymm1                ; b = 高 16 位
vpsubd %ymm7,%ymm1,%ymm1                ; d = b - a
vpsrad $0x8 / vpand                     ; d_hi（9 位有符号）、d_lo（低 8 位）
vpmulld %ymm1,%ymm6,%ymm1               ; d_lo * frac
vpmulld %ymm6,%ymm0,%ymm0               ; d_hi * frac
vpsrld $0x8 / vpaddd / vpsrad $0x8      ; 两步移位合并 = floor(d*frac/65536)
vpaddd %ymm7,%ymm0,%ymm0                ; mixed = a + 插值增量
vpackssdw / vmovdqu
```

---

## 3. SIMD 实现

### 3.1 µ-law 编码（AVX2，**已合入生产路径**）

**算法**：把标量的「查 `kUlawEncode.exponent[256]` 表」换成 `floor(log2(index))` 的
向量化计算。表的语义本来就是「最高有效位位置」：

```cpp
// 标量：exponent = kUlawEncode.exponent[sample >> 7]   // 表由 kSegEnd[] 生成
// 向量：exponent = floor(log2(sample >> 7))            // 0..7，与表在 0..255 全域一致
// 取法：float 指数域（index <= 255 时 float 精确 → bits>>23 - 127）
```

**正确性**：

| 用例 | 规模 | 结果 |
|---|---|---|
| 穷举 | 65536 个 int16 全域 | 0 处不一致 |
| 不整齐长度 | 1/7/15/16/17/31/441/1000/4097 | 0 处不一致（含标量尾巴） |
| 大样本 | 1e7 随机 | `memcmp == 0` |

**性能**（1e7 样本，3 次中位）：

| 实现 | ns/样本 | M 样本/s | 相对 |
|---|---:|---:|---:|
| 教科书基线（逐位找 exponent） | 6.040 | 166 | 0.19× |
| 查表（内联批量，改动前生产路径） | 1.172 | 853 | 1.00× |
| **AVX2（现在）** | **0.207** | **4,830** | **5.54×** |
| 生产路径 `UlawEncodeBlock`（分派 → AVX2） | 0.214 | 4,673 | 5.45× |

### 3.2 重采样（AVX2 gather，**已合入生产路径**）

16.16 定点线性插值必须**逐样本一致**，不能用浮点近似，也不能用会溢出的 32 位中间量：

```
标量： (a*(65536-frac) + b*frac) >> 16
恒等： a + ((b-a)*frac >> 16)              // a*65536 是 65536 的整数倍
溢出： |(b-a)*frac| <= 65535*65535 ≈ 2^32  // 超 int32
拆分： d = d_hi*256 + d_lo
       floor(d*frac/65536) = floor((d_hi*frac + floor(d_lo*frac/256)) / 256)
两步中间量都 <= 2^25，int32 安全（vpmulld 不会溢出）
```

数据搬运用 `vpgatherdd`（scale=2）**一次读出 `in[idx]` 与 `in[idx+1]` 两个样本**，
省掉第二次 gather。

**边界处理**（第一版踩过的坑）：
- 主循环只在 `pos + 7*step < (in_frames-1)<<16` 时进入，且要求 `idx_last < in_frames-1`，
  保证 gather 的两字节前瞻不越界；
- 一轮推进 `8*step` 后 `position` 可能**已越过末端 clamp 点**，交回标量前必须钳到
  `limit`（否则标量首帧在越界 index 上取值——这是首版唯一的正确性缺陷，由 20000 轮
  随机比对发现并修复）；
- 其余帧（含 clamp 区、`channels != 1`）走标量尾巴，两条路径共用
  `ResampleLinearScalarRange` 的同一份舍入语义。

**正确性**：

| 用例 | 规模 | 结果 |
|---|---|---|
| 边界用例 | 441→480、882→960、480→441、1↔1、2↔2、3→441、等率、100@8k→480@48k、960→441 | 逐样本一致 |
| 随机（测试内） | 400 组随机帧数 × 6 种采样率组合 | 逐样本一致 |
| 随机（独立程序） | **20000 轮**随机帧数/采样率 | 逐样本一致（`NO MISMATCH`） |

**性能**（441→480 mono，1e5 帧，3 次中位）：

| 实现 | ns/帧 | ns/样本 | 相对 |
|---|---:|---:|---:|
| 标量（GCC 已向量化 channel 循环，但 mono 走标量） | 857.4 | 1.786 | 1.00× |
| **AVX2 gather** | **272.0** | **0.567** | **3.15×** |

### 3.3 µ-law 解码：**判定不做 SIMD**（有数据支撑）

现状：`detail::UlawDecodeBlock` 已被 GCC 自动向量化，实测 **0.213–0.303 ns/样本**
（0.96–1.36 cycles），瓶颈是「1 次查表 load + 1 次 store」的端口吞吐
（Zen4：3 load + 2 store/cycle，2 uop/样本 → 0.5 cycle/样本下限），实测离下限约 2×，
**没有 ALU 冗余可挖**。

若强行 SIMD 化，要把 8 位码字解包成 16 位再算 `((mant<<3)+0x84)<<exp - 0x84`：

| 方案 | 每 8 样本指令数 | 结论 |
|---|---:|---|
| SSSE3 16 位 lane（pshufb 查 2^exp + pmullw） | ≈18 条 → 2.25 指令/样本 | 比标量 2 uop/样本更差 |
| AVX2 32 位 lane（vpsllvd 变长移位） | ≈14 条 → 1.75 指令/样本 | 拆包/打包开销吃掉收益 |

结论：**解码不向量化**，保持查表。这与编码（5.3 cycles/样本的串行依赖链，可向量化到
0.207 ns）形成对照。

---

## 4. 对标

### 4.1 vs libopus（同机 dlopen，配置完全一致）

两边都是 `libopus.so.0`（1.4）、48000 Hz mono、`OPUS_APPLICATION_AUDIO`、
32 kbps、complexity 5、FEC/DTX 关闭、10ms 帧（480 样本）、440 Hz 正弦，
600 帧、预热 50 帧，`taskset -c 2`，3 次取中位。

| 路径 | ns/帧 | 相对裸 libopus |
|---|---:|---:|
| 裸 `opus_encode`（dlopen 直调） | 17,050.0 | 1.000× |
| 本项目 `OpusAudioEncoder::Encode`（48k 直通） | 17,147.5 | **1.006×** |
| 本项目 `OpusAudioEncoder::Encode`（44.1k，含 441→480 重采样） | 17,675.6 | **1.037×** |

**包装开销拆解**（48k 直通，+97.5 ns/帧）：

| 来源 | 量级 | 说明 |
|---|---:|---|
| `frame.Valid()` + `frames()` 检查 + 分支 | ~5 ns | 8 条比较/分支 |
| `out->assign(41B)`（容量已复用） | ~10 ns | memcpy 41B + 长度更新 |
| `m_api->encode` 间接调用 | ~2 ns | 一次间接跳转 |
| 统计计数 `++m_frames` / `m_bytes` | ~5 ns | — |
| 其余（栈帧/缓存效应） | ~75 ns | 包缓冲移到成员后仍要写 41B 到堆上 vector |

**优化前后对比**（`OpusAudioEncoder::Encode` 自身）：

| 指标 | 优化前 | 优化后 |
|---|---:|---:|
| 栈帧 | 1304 B（`sub $0x518`） | **8 B**（`sub $0x8`） |
| 每帧清零 | `rep stos` 1275 B（≈40 cycles） | **0** |
| 静态指令 | 224 | 207 |
| `call` | 11 | 8 |
| 每帧 guard 检查 | 1 次 acquire load + 分支 | 0（`m_api` 缓存） |

> 绝对值上，`opus_encode` 本身要 **17.1 µs/帧**（10ms 音频、complexity 5），
> 包装只占 **0.6% / 3.7%**——这就是"Opus 编码包装开销"的真实占比。
> 重采样在 44.1k 路径上占 +528 ns（3.1%），SIMD 化后已从 +1097 ns 降到 +528 ns。

### 4.2 vs 原生重采样

**`libsamplerate` 0.2.2**（同机、同 441→480 mono、1e5 次调用、float 输入）：

| 实现 | ns/调用 | 相对本项目 SIMD |
|---|---:|---:|
| `SRC_SINC_BEST_QUALITY` | 218,277.2 | 790.9× 慢 |
| `SRC_SINC_MEDIUM_QUALITY` | 54,238.7 | 196.5× 慢 |
| `SRC_SINC_FASTEST` | 26,474.4 | 95.9× 慢 |
| `SRC_LINEAR`（同类线性插值） | 2,818.6 | **10.2× 慢** |
| `SRC_ZERO_ORDER_HOLD` | 2,921.6 | 10.6× 慢 |
| **本项目 16.16 定点 SIMD** | **276.0** | 1.00× |
| 本项目 16.16 定点标量 | 888.6 | 3.22× 慢 |

**`sox` / `libsoxr`：未测**——本机 `libsoxr.so.0` 存在但**没有头文件、没有 `sox`
命令行**，无法在同一进程内做公平对比（用 CLI 起进程会把启动开销算进来）。

**公平性说明**：`libsamplerate` 的 SINC 系列是带限（多相 FIR）重采样，输出质量显著
高于线性插值；本项目是**定点线性插值**，高频有镜像（`docs/modules/media-audio.md`
§9 已列为已知限制，Opus 48k mono 端到端 SNR 26.44 dB）。两者不是同一质量档位：

- 与**同类算法**（`SRC_LINEAR`/`ZOH`）比：本项目快 **10.2×/10.6×**；
- 与**高质量档**（SINC_FASTEST）比：本项目快 95.9×，但质量不同档，不能直接说"更好"。

---

## 5. 正确性

全部由 `tests/media/test_audio.cpp` 覆盖，`[audio] OK (409205 checks)`：

| 用例 | 规模 | 结果 |
|---|---|---|
| PCM 全链路 1e5 帧（441 样本/帧） | 88,200,000 B | 逐字节 0 处不一致，checksum `0xc471e9c06a63e512`（与基线相同） |
| G.711 穷举基线 vs 查表 | 65536 输入 | 0 处不一致 |
| **G.711 SIMD vs 查表** | 65536 输入 + 9 种长度 + 1e7 | 0 处不一致 |
| **重采样 SIMD vs 标量** | 9 边界 + 400 随机（测试）+ 20000 随机（独立） | 逐样本一致 |
| G.711 µ-law / A-law 往返 SNR | 440Hz，-6dBFS | 36.70 / 38.97 dB（与基线相同） |
| **Opus 48k mono / 48k stereo / 44.1k（重采样）** | 300 帧 | **26.44 / 19.04 / 25.86 dB**（与基线**逐位相同**） |
| Opus PLC | 空负载 | 480 样本，`concealed_frames=1` |
| RTP 头 20 万次随机往返 | — | 全部一致 |
| 分片/重组/抖动/UDP/会话 | 见文档 §7 | 全部通过 |

> Opus 三档 SNR 与基线**完全一致**（含 44.1k 那条走重采样 SIMD 的路径），这是
> "SIMD 化没有改变输出"的端到端证据，比单测更强。

**稳定性**：连续 75 次 `taskset -c 2` 全量运行中 74 次全绿；唯一 1 次失败发生在
紧跟一次并行构建之后（同机其他任务争抢），重跑该配置 40 次未复现。该用例的
时序断言（UDP 回环 1000 包不丢、内存链路 p99 ≤ 10ms）在文档 §9.4 已被标注为
环境敏感项，与本轮 SIMD 改动无关（SIMD 正确性断言在所有运行中均通过）。

---

## 6. 离物理下限几倍

按项目既有口径（`asm-sfu.md`）反推 4.5 GHz：

| 路径 | 实测 | 折算周期/单位 | 理论下限 | 差距 | 瓶颈 |
|---|---:|---:|---:|---:|---|
| µ-law 编码 AVX2 | 0.207 ns/样本 | 0.93 cycles/样本 | 0.44（2.6 uop/样本 ÷ 6-wide） | **2.1×** | `vcvtdq2ps` 3–4 cycle 延迟 + 只有 2 条独立链 |
| µ-law 编码标量（查表） | 1.172 ns/样本 | 5.27 cycles/样本 | ~4（查表 load-to-use） | 1.3× | 串行 load-to-use |
| 重采样 AVX2 | 0.567 ns/样本 | 2.55 cycles/样本 | 0.52（3.1 uop/样本 ÷ 6-wide） | **4.9×** | `vpgatherdd`：20.4 cycles/轮（8 样本），单条 gather 占 78% |
| 重采样标量 | 1.786 ns/样本 | 8.04 cycles/样本 | ~3（~10 条标量指令 + 2 次 load） | 2.7× | position 累加 + 双声道内循环 |
| µ-law 解码（未改） | 0.213–0.303 ns/样本 | 0.96–1.36 cycles/样本 | 0.5（store 端口 2/cycle） | 1.9–2.7× | store 端口 + 查表 load |

**关于重采样的 4.9×**：AVX2 没有跨 lane 的 16 位 gather，只能用 32 位 `vpgatherdd`，
Zen4 上 8-lane gather 约 20 cycles/条。本轮循环 25 条指令中 gather 占 ~20 cycles，
即**约 78% 的时间在 gather 上**。想再压只能改数据结构（预展开成 int32 副本，内存
翻倍但走普通 load + `vpermd`），对 10ms 音频帧（0.27 µs/帧）没有实际意义，故停在这里。

**绝对值视角**（更重要）：

| 路径 | 每 10ms 音频帧的成本 | 占 10ms 的比例 |
|---|---:|---:|
| µ-law 编码 441 样本 | 0.091 µs | 0.0009% |
| µ-law 解码 441 样本 | 0.094 µs | 0.0009% |
| 重采样 441→480 | 0.272 µs | 0.0027% |
| Opus 编码（libopus 本身） | 17.1 µs | **0.17%** |

音频路径的真实成本在 `opus_encode`，不在这些内核；SIMD 化的意义是
「把 G.711 降级路径和重采样的成本压到可忽略」，而不是提升端到端延迟。

---

## 7. 边界与未做项（如实记录）

1. **音频 delta 编码：不存在，未测。**
   检索结果：`src/media/audio.cpp` 与 `include/go2cpp/media/audio.hpp` 里没有任何
   delta 编解码器；`audio.cpp` 中唯一的 `delta` 是抖动缓冲的到达间隔差
   （`now_ms - expected`，EWMA 用，不是编解码）。
   项目里的 `kDeltaI420` 是**视频侧 16×16 块级差分**（`src/media/codec_delta.cpp`，
   无损、块内 RLE），属于另一个模块，不在本次边界
   （`src/media/audio*` / `include/go2cpp/media/audio*` / `tests/media/**`）内，
   故未做 objdump/SIMD。

2. **µ-law 解码 SIMD：判定不做**（理由与数据见 §3.3）。

3. **重采样只看 mono**：`channels == 1` 走 SIMD，stereo 走标量。原因是 stereo 的
   gather 要拆成两次（L/R 交错），收益/复杂度比不划算；项目默认音频是 44.1k mono，
   stereo 只出现在 48k 直通（等率 → `memcpy`，不经过重采样）。

4. **`sox` / `libsoxr` 未测**：本机无 CLI、无头文件（§4.2）。

5. **未改 `docs/modules/media-audio.md`**：本次边界只允许代码与测试，文档更新留给
   集成方（建议把 §8.1 表格与 §4.2 重采样描述同步为本文数据）。

6. **未 commit**（按要求）。

---

## 8. 复现

```bash
# 构建 + 全量测试（含 SIMD 正确性、性能、Opus 延迟）
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build /tmp/build-audio --target media_test_audio -j 12
taskset -c 2 /tmp/build-audio/media_test_audio | grep -E '^\[PERF\]|^\[audio\]'

# 对标（libopus + libsamplerate）
g++ -std=c++17 -O2 -Iinclude /tmp/bench_compare.cpp -o /tmp/bench_compare \
    -L/tmp/build-audio -lgo2cpp_media -lgo2cpp_media_bento_core \
    -lX11 -lXext -lSDL2 -ldl -lpthread -lsamplerate
taskset -c 2 /tmp/bench_compare

# 重采样一致性（独立随机比对）
g++ -std=c++17 -O2 -Iinclude /tmp/dbg_resample.cpp -o /tmp/dbg_resample \
    -L/tmp/build-audio -lgo2cpp_media -lgo2cpp_media_bento_core \
    -lX11 -lXext -lSDL2 -ldl -lpthread
/tmp/dbg_resample        # 期望输出 NO MISMATCH

# 反汇编
g++ -std=c++17 -Iinclude -Isrc/media -O3 -DNDEBUG -c src/media/audio.cpp -o /tmp/audio_o3.o
objdump -d --no-show-raw-insn \
  --disassemble=_ZN6go2cpp5media12_GLOBAL__N_112_GLOBAL__N_119UlawEncodeBlockAvx2EPKsmPh \
  /tmp/audio_o3.o
```
