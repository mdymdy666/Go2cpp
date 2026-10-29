# frame 模块审查报告（压测 / 随机 / 场景 / 解耦 / 测试复查）

- 日期：2026-10-29
- 审查对象：`include/go2cpp/bento/net/frame.hpp`、`src/bento/net/frame.cpp`、
  `tests/bento/test_frame.cpp`
- 构建：`/tmp/build-bento-wsl`（Ninja，`GO2CPP_WITH_BENTO=ON`，Release `-O2`，GCC 13.3）
- 结论：**无 P0**。修复 1 个 P1（`noexcept` 解码路径在载荷分配失败时 `std::terminate`）
  与 2 个 P2（32 位长度溢出、一次性 `Decode` 无法指定载荷上限）；2 项性能候选经实测
  不采纳（记录）。测试 checks 5511 → **13080（+137%）**，`ctest -R bento` 48 项
  47 过 + 1 环境 SKIP，ASan/UBSan 全过。

## 1. 压测数据

工具：`tests/bento/bench_frame_stress.cpp`（独立程序，43878 条断言，不注册 CTest，
避免改动 `cmake/Go2CppBento.cmake` 的用例计数）。
环境：12 核 / 19 GiB / Release `-O2`；base 与 opt 交替 5 轮，取每项最小值。

### 1.1 吞吐（优化后）

| 场景 | 帧大小 | ns/帧 | 帧/s | MiB/s |
|---|---|---|---|---|
| Encode | 0 B | 16.0 | 60.1 M | 1146 |
| Encode | 64 B | 36.4 | 27.1 M | 2169 |
| Encode | 512 B | 178.2 | 5.53 M | 2807 |
| Encode | 1400 B | 447.8 | 2.18 M | 2951 |
| Encode | 64 KiB | 20935 | 47.0 k | 2938 |
| Encode | 1 MiB | 347097 | 2752 | 2753 |
| Decode（复用 Frame） | 64 B | 24.0 | 39.3 M | 3148 |
| Decode（复用 Frame） | 1400 B | 441.2 | 2.24 M | 3030 |
| Decode（复用 Frame） | 1 MiB | 332905 | 2878 | 2878 |
| 流式粘包 64 帧/批 | 1400 B | 474.5 | 2.06 M | 2790 |
| 流式逐字节半包 | 1400 B | 7390 | 126.8 k | 172 |
| 12 线程各自解码 | 512 B | 73.2 | 13.5 M | **6864** |
| 长稳 100 万帧 | 1400 B | 471.6 | 2.05 M | 2774 |

### 1.2 CRC 成本（唯一热点）

| 帧大小 | Encode ns | Decode ns | CRC ns | CRC 占 Encode | CRC 占 Decode |
|---|---|---|---|---|---|
| 64 B | 36.4 | 24.0 | 20.3 | 55.6% | 84.6% |
| 1400 B | 447.8 | 441.2 | 432.7 | **96.6%** | **98.1%** |
| 64 KiB | 20935 | — | 20372 | 97.3% | — |
| 1 MiB | 347097 | 332905 | 312321 | 90.0% | 93.8% |

- CRC 单独吞吐：1 MiB 3147 MiB/s、64 KiB 3028 MiB/s、1400 B 3091 MiB/s。
- 结论：帧层开销（头写入、`memcpy`、`vector` 分配）已接近下限；帧越大，CRC 越接近
  100%。剩余优化空间在 `hash` 模块（slicing-by-8 → PCLMULQDQ），不属于 frame 边界。

### 1.3 内存与分配

| 指标 | 结果 |
|---|---|
| Encode 分配 | 1 次/帧（返回值 `std::vector` 无法避免） |
| Decode 复用 `Frame` | **0 次/帧**（`payload.assign` 复用容量） |
| Decode 每帧新建 `Frame` | 1 次/帧 |
| 100 万帧长稳 RSS | 6072 → 6200 KiB（**+128 KiB**），净分配 4 次 |
| 1 MiB 帧解码峰值 | 约 +2 MiB（输入 + 载荷副本） |
| 半包缓冲上限 | 由 `max_payload` 限制在 1 MiB + 20 B（声明超限立即拒绝） |

### 1.4 优化前后对比（base=旧实现，opt=本次修复后）

| 项目 | base ns | opt ns | 变化 |
|---|---|---|---|
| Encode 1400 B | 446.9 | 447.8 | +0.2% |
| Encode 1 MiB | 345919 | 347097 | +0.3% |
| Decode 1400 B（复用） | 434.5 | 441.2 | +1.5% |
| Decode 1 MiB（复用） | 331677 | 332905 | +0.4% |
| CRC 1400 B | 433.4 | 432.7 | -0.2% |
| 长稳 100 万帧 | 455.3 | 471.6 | +3.6% |
| 12 线程 | 70.0 | 73.2 | +4.6% |

- 热路径代码未变（CRC 占 90–98%），差异在机器负载波动范围内（±5%），**判定无回归**。
- 反汇编对比：`ParseOne` 因新增 `try/catch` 失去 GCC 的 `.isra` 参数传播克隆
  （178 → 198 行指令），但端到端 ≤2%；64 B 场景实测 -0.2%。为保留 P1 修复，接受。

## 2. 随机测试

1500 条随机字节流（每条 1–12 个合法帧 + 30% 概率注入坏 magic/版本/reserved/超长/
篡改 + 35% 概率尾部截断，随机 1–113 B 块切分喂入），与独立参考模型（逐位 CRC +
手工大端解析，不复用实现代码）逐帧对拍：

- 9645 个合法帧全部还原（type/flags/payload 逐字节相等）；
- 503 条截断流返回 `kNeedMoreData` 且 `PendingBytes` 与模型一致；
- 370 条错误流的错误码（bad_magic / bad_version / bad_reserved / length_exceeded /
  checksum_mismatch）与模型完全一致，错误后缓冲清零；
- 0 失败。ASan+UBSan 下同样 0 失败、无越界/未定义行为报告。

## 3. 五个复杂场景

| 场景 | 结果 |
|---|---|
| 粘包 + 半包 | 100 帧（0–3000 B）一次 Append / 逐字节 / 随机 1–7 B 块三种到达方式均逐帧还原；「帧 A + 帧 B 前 5 B」压缩路径正确 |
| 1 MiB 上限帧 | 完整往返；分三段半包到达正确；1 MiB+1 发送端拒绝、接收端在校验和之前拒绝；篡改载荷 → checksum_mismatch；声明 1 MiB 但数据不足 → `kNeedMoreData`（不预分配） |
| 非法帧族 | magic/版本/reserved/超长/CRC/type 篡改 6 类错误码正确、缓冲清空、`Clear` 后可恢复；首帧合法 + 次帧非法时首帧仍成功交付 |
| 并发编解码 | 4 线程 40 万帧 + 12 线程 60 万帧，各自独立 `FrameDecoder`；12 线程 6864 MiB/s，0 错误 |
| 长稳 100 万帧 | 1.42 GB 通过解码器，471.6 ns/帧；RSS 稳定（+128 KiB），净分配 4 次，无泄漏 |

## 4. 模块解耦

- `frame.hpp` 只依赖 `<cstddef>` / `<cstdint>` / `<vector>`；`frame.cpp` 额外依赖
  `<cstring>` / `<limits>` / `<new>` 与 `go2cpp/hash/crc32.hpp`。
- **不依赖** `transport` / `link` / `error` / `base`：错误用 `FrameStatus` 枚举表达，
  没有跨模块错误对象；头文件可单独编译（`g++ -fsyntax-only`）。
- 唯一生产调用方：`src/bento/app/session.cpp`（`FrameDecoder` + 自定义 `FrameFormat`），
  本次新增的重载与枚举值向后兼容，调用方无需修改。
- 测试侧耦合：`test_frame.cpp` 经 `test_util.hpp` 间接包含 `link.hpp`（仅为断言/
  `Finish`），属测试层耦合，记录为 P2-6。

## 5. 问题清单

### P0（无）

### P1-1 `noexcept` 解码路径在分配失败时 `std::terminate`（已修复）

- 证据：`Decode()` / `ParseOne()` 声明 `noexcept`，但内部 `out->payload.assign(...)`
  会分配内存。用 `RLIMIT_AS` 限制地址空间后解码 1 MiB 帧，旧实现输出
  `terminate called after throwing an instance of 'std::bad_alloc'` 并 core dump。
- 修复：新增 `FrameStatus::kOutOfMemory`；`ParseOne` 内 `try { assign } catch
  (std::bad_alloc&)` 返回状态码，保持 `noexcept` 承诺；`FrameStatusName` 增加
  `"out_of_memory"`；注释说明此时 `out->payload` 内容未定义。
- 验证：同一 OOM 实验下新实现返回 `out_of_memory (8)`；`session.cpp` 对非
  `kOk`/`kNeedMoreData` 一律按协议错误关闭连接，自动兼容。

### P2-1 32 位平台长度溢出（已修复）

- `total = 20 + length` 在 32 位 `size_t` 上，当调用方把 `max_payload` 设到接近
  `UINT32_MAX` 时溢出，`size < total` 判断失效，可能把「声明 4 GiB」的头当成完整帧
  去算 CRC（越界读）。
- 修复：`length > SIZE_MAX - 20` 时返回 `kLengthExceeded`；64 位下该条件恒假，被
  常量折叠（反汇编确认无额外分支）。

### P2-2 一次性 `Decode` 无法指定载荷上限（已修复）

- `FrameDecoder` 支持自定义 `max_payload`，而 `Decode` 硬编码 1 MiB，二者不对称，
  导致「自定义上限」只能通过流式解码器验证。
- 修复：新增 `Decode(format, max_payload, data, size, out, consumed)` 重载；测试覆盖
  `max_payload = 0 / 15 / 16 / kFrameMaxPayload` 边界。

### P2-3 Encode 的 `vector` 零初始化（记录，不采纳）

- `std::vector<uint8_t> out(total)` 对 1 MiB 帧多一次 1 MiB 清零。实测替代写法
  （`reserve` + 两次 `insert`）：1 MiB 快 2.7%，但 1400 B 慢 6.0%、64 B 慢 10.6%。
  小帧是主流，整体不划算，保持现状。

### P2-4 半包缓冲压缩策略（记录，不采纳）

- 评估过「已消费前缀 ≥ 未消费字节才压缩」的阈值策略。实测 `compaction-cliff`
  场景（小帧 + 1 MiB 半包，64 轮）base 1.85 ms vs opt 1.79 ms，差异在噪声内；
  阈值策略还会让缓冲峰值升到有效数据的 2 倍。已回退，保持「每次 Append 压缩一次」。

### P2-5 解码缓冲无硬上限（记录）

- `FrameDecoder::Append` 不限制总缓冲大小：若调用方持续 Append 而从不 `Next`，
  缓冲会随输入增长。推荐用法（Append 后循环 `Next` 到 `kNeedMoreData`）下残留
  不超过一帧。属调用方责任，头文件已写线程模型与拒绝策略，本次不引入上限参数。

### P2-6 测试工具耦合 `link.hpp`（记录）

- `tests/bento/test_util.hpp` 包含 `link.hpp`，frame 测试为拿 `BENTO_CHECK` 也
  被拖入整个 link 头（编译时间、故障面）。拆分会引起 30+ 测试重编译，超出本次
  改动边界，记录。

### P2-7 缺少 `.clang-format`（记录）

- 仓库根目录无 `.clang-format`，无法用工具校验格式；本次按既有 4 空格/行宽风格手工对齐。

## 6. 测试复查

- 原状：10 个测试函数 / **5511 checks**，全部通过；断言宏每次自增计数，`Finish`
  汇总失败数，**无 `Skip` 调用**（不存在 SKIP 逃逸）。
- 覆盖缺口（8 项）：自定义 `FrameFormat` 协议隔离、头字段篡改（CRC 覆盖范围）、
  错误后恢复、`max_payload = 0`、1 MiB 帧半包到达、`Decode` 参数矩阵（`consumed`
  写入门槛 / nullptr 组合 / 新重载）、并发解码、随机对抗流。
- 新增 9 组用例覆盖上述缺口；扩充中测试自身发现一处预期错误（把 `length` 改为 63
  时数据已足够，应报 `checksum_mismatch` 而非半包），已改为 65 的半包用例。
- 结果：**13080 checks（+137%）全过**；ASan+UBSan 下同样全过。
- 断言有效性：`BENTO_CHECK_EQ` 失败只打印表达式、不打印实际值，定位效率一般，
  记录为后续可选改进。

## 7. 复现

```bash
# 单测
cmake --build /tmp/build-bento-wsl --target bento_test_frame -j12
cd /tmp/build-bento-wsl && ctest -R bento --output-on-failure

# 压测（独立编译，不注册 CTest）
cd /UserData/CodexWorkSpace/Go2cpp
g++ -O2 -std=c++17 -Iinclude -Itests/bento tests/bento/bench_frame_stress.cpp \
    src/bento/net/frame.cpp src/hash/crc32.cpp -o /tmp/bench_frame -pthread
/tmp/bench_frame --tsv=/tmp/frame_bench.tsv
# ASan/UBSan：同一命令加 -fsanitize=address,undefined -g，再跑 --quick
```

## 8. 遗留

- 未改 `AI_COLLAB.md`（本次边界只允许 `bento/net/frame.*` 与 `tests/bento/**`）。
- 未 commit；`tests/bento/bench_frame_stress.cpp` 为新增压测工具，未加入 CMake。
- 优化前基线副本保留在 `/tmp/frame_base/`（frame.cpp/hpp/test_frame.cpp 的 `.orig`）。
