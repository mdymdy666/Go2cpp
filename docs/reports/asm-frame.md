# frame 三件套补做（A41 / TOP3）

日期：2026-11-04 · 环境：WSL2 / 12 vCPU / GCC 13.3 `-O3 -DNDEBUG` / 单线程（`taskset -c 4`）
范围：补 `Feed` 逐字节路径 + `Encode` 的 objdump 与 1 B 切分基准，**未改任何代码**。

## 1. objdump（`.build-bento-wsl/libgo2cpp_frame.a` / Release）

统计口径与 `docs/modules/rpc.md:312-313` 一致（指令数 / call / `sub rsp` 栈帧）。

| 函数 | 指令数 | call | memcpy | 栈帧 B（`sub rsp` / 含 push） |
|---|---|---|---|---|
| `FrameCodec::Encode` | **328** | **11** | 0（`memcpy` 被内联为 `movups`/`rep movsb`） | **8** / 64 |
| `FrameCodec::Feed`（外层校验 + try/catch） | 151 | 7 | 0 | 72 / 88 |
| `FrameCodec::FeedImpl`（**逐字节喂入走这里**） | **284** | **13** | 0（同上，内联） | **88** / 136 |
| `FrameCodec::ValidateHeader` | 615 | 23 | 0 | 8 / 120 |
| `FrameCodec::ExtractFrame` | 259 | 12 | 0 | 8 / 64 |
| `Crc32IeeeFrameChecksum` | 3 | 0 | 0 | 0（跳转到 hash Crc32） |

**与已有数据对照（`rpc.md:312-313`）**：

| 符号 | rpc.md 记录 | 本次实测 | 结论 |
|---|---|---|---|
| `FrameCodec::Encode` | 328 指令 / 11 call / 8 B | 328 / 11 / 8 | **逐位一致** ✅ |
| `FrameCodec::FeedImpl` | 284 / 13 / 88 | 284 / 13 / 88 | **逐位一致** ✅ |

→ `rpc.md` 的两项数字可复现，非手抄估算。新增三项：
- `ValidateHeader` **615 条指令**是大头，几乎全是 `snprintf` 错误文本格式化的内联展开
  （magic/version/length 三条错误路径），热路径只执行前 ~30 条（`memcmp` + `LoadLe32` + 比较），
  **代码体积大但不占执行时间**，不建议拆。
- `Feed` 外层 151 条（含 `DescribeInvalidSpec` 冷调用与 `bad_alloc` catch），
  每次 `Feed()` 调用固定走一次 `err->Clear()` + 5 个参数校验。
- `ExtractFrame` 259 条：每帧一次 `payload.assign`（一次分配/拷贝）+ 校验和回调。

## 2. 逐字节喂入基准

临时程序（未入库）直接调用已发布头文件 + `libgo2cpp_frame.a`：
`FrameCodec(MakeFrameSpec(MakeMagic("RDRL"), 1, 65536))`，CRC32 开启，
帧 = 12 B 头 + 88 B 载荷 + 4 B CRC = **104 B**；每轮 5 次取最快。

| 场景 | 每帧耗时 | 帧率 | 每次 `Feed` |
|---|---|---|---|
| A 整帧一次喂（104 B） | **30.9 ns** | **32.39 Mframes/s** | — |
| B 1 B 分 104 次喂（104 B 帧） | **941.9 ns** | **1.06 Mframes/s** | **9.06 ns/字节** |
| C 1 B 分 4100 次喂（4 KiB 帧） | 38026.8 ns | 0.03 Mframes/s | **9.28 ns/字节** |

结论：

1. **不是 O(n²)**：B 与 C 的「每字节成本」几乎相同（9.06 vs 9.28 ns），
   说明每次 `Feed()` 的开销是**固定摊还**的，缓冲增长走 vector 倍增，没有按帧长重复扫描整段。
2. **但逐字节比整帧慢 30×**（941.9 / 30.9）。9 ns/字节 ≈ 27 cycles，
   对应 `FeedImpl` 阶段 1 的「每字节：`resize+1` → `memcpy(1)` → `ValidateHeader`」。
3. 与 `docs/modules/frame-codec.md:219` 的 **4.59 Mframes/s**（1e6 帧 encode→随机切分 Feed，
   混合载荷 0–119 B）对照：本次整帧 32.4 Mframes/s 更快，差异来自
   「随机切分 + 逐帧字节校验 + 载荷分布」的口径不同，**不是性能回退**；同口径应以 4.59 为基线，
   1 B 路径为 1.06 Mframes/s（**慢 4.3×**）。

## 3. 可优化点（**只记录，未改**）

1. **重复解析帧头**：`frame_codec.cpp:237-268` 阶段 1 在 `m_buffer.size() >= header_len` 后，
   每来 1 字节都会重新 `ValidateHeader`（`memcmp` + `LoadLe32` + 上限比较）。
   104 B 帧喂 104 次 → 帧头被校验 **93 次**。可缓存「已解析出的 `frame_bytes`」，
   仅在帧边界变化时重算；改动集中在 `FeedImpl` 一处，预期把 1 B 路径的 9 ns/字节降到约 5–6 ns。
2. **`append` 每次 `resize(old+1)`**：1 B 喂入时每字节一次 `resize`（带容量检查与 `size` 更新）。
   可改为「剩余空间够就只 `memcpy` + 手工推进 size」，或按 `frame_bytes` 一次性预留。
   与 ① 同一处，**必须一起做才有意义**。
3. 不做：`ValidateHeader` 的 `snprintf` 内联（错误路径，只在畸形流触发）；
   `ExtractFrame` 的 `payload.assign`（语义需要副本，调用方拿到的 `Frame` 必须独立存活）。

> 以上两项均有「改一处影响半包/粘包语义」的风险，**本次只记录、不改**；
> 若后续实施，需重跑 `tests/frame/test_frame_codec.cpp` 全部 1130 行断言 + 1e6 帧压测。

## 4. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
g++ -O3 -DNDEBUG -std=c++20 -I include <bench.cpp> \
  .build-bento-wsl/libgo2cpp_frame.a \
  .build-bento-wsl/CMakeFiles/go2cpp_hash.dir/src/hash/crc32.cpp.o \
  .build-bento-wsl/CMakeFiles/go2cpp_hash.dir/src/hash/hash.cpp.o -o /tmp/frame_bench
taskset -c 4 /tmp/frame_bench
```
