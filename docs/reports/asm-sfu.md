# 汇编级性能审查：SFU 转发（sfu）

> 模块：`include/go2cpp/media/sfu.hpp`、`src/media/sfu.cpp`
> 基准：`tests/media/test_sfu.cpp`（1e6 帧裸转发 + VideoChannel 端到端）
> 日期：2026-11-01；状态：现行

---

## 1. 测试环境

| 项 | 值 |
|---|---|
| 系统 | WSL2 / Ubuntu 24.04.3 LTS |
| CPU | AMD Ryzen 9 8945HX（12 vCPU，Zen 4） |
| 缓存 | L1d 32 KiB/core、L1i 32 KiB/core、L2 1 MiB/core、L3 32 MiB |
| 编译器 | g++ 13.3.0 |
| 构建 | Release：`-O3 -DNDEBUG`，`-Wall -Wextra -Wpedantic -Werror` |
| 频率 | `/proc/cpuinfo` 报 2.5 GHz 基频；按实测 IPC 反推实际约 4.5 GHz（见 §4） |

复现命令：

```bash
cd /UserData/CodexWorkSpace/Go2cpp
cmake --build .build-bento-wsl --target media_test_sfu -j 8
.build-bento-wsl/media_test_sfu          # 打印 [PERF] 行
```

---

## 2. 热函数反汇编

`SfuRouter::ForwardFragment`（mangled
`_ZN6go2cpp5media9SfuRouter15ForwardFragmentERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEjjPKhmh`）：

```bash
objdump -d --no-show-raw-insn \
  --disassemble=_ZN6go2cpp5media9SfuRouter15ForwardFragmentERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEjjPKhmh \
  .build-bento-wsl/CMakeFiles/go2cpp_media.dir/src/media/sfu.cpp.o
```

| 指标 | 优化前（`ParseFrame`） | 优化后（最小解码） |
|---|---:|---:|
| 静态指令数 | 345 | 347 |
| `call` 数 | 8 | 8 |
| 间接调用（`std::function` sink） | 2 | 2 |
| 条件跳转数 | 61 | 63 |
| `lock` / 原子指令 | 0 | 0 |
| `operator new` / `malloc` 调用 | 0 | 0 |
| `@plt` / GOT 引用 | 0 | 0 |
| 栈 canary（`fs:0x28`）引用 | 3 | 3 |
| **动态指令数（callgrind，1,001,000 帧）** | **276,308,455** | **262,988,800** |
| 每帧动态指令数 | ≈ 276 | ≈ 263 |

静态指令数几乎不变（345 → 347），但动态执行路径变短、分支显著减少：
`ParseFrame` 为通用帧解析准备了 ACK/FEC/长度/类型多重分支，而 SFU 的输入契约
是 `VideoChannel` 的视频数据分片（不携带捎带 ACK）。优化后只做
`DecodeHeader` + `DecodeVideoHeader` 与 SFU 自己的 4 个边界检查，
`kFlagAck` 分片直接拒绝。

`callgrind_annotate` 命令：

```bash
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.out --quiet \
  .build-bento-wsl/media_test_sfu
callgrind_annotate --auto=no /tmp/cg.out | grep ForwardFragment
```

---

## 3. 前/后性能数据（同一基准、同一机器）

`media_test_sfu` 的 1e6 帧裸转发（924B 分片、2 个订阅者、单线程），
每版连续跑 5 次：

| 版本 | 5 次 samples（M frames/s） | 中位数 | 每帧耗时 |
|---|---|---:|---:|
| 优化前（`ParseFrame`） | 35.65 / 34.70 / 35.99 / 35.50 / 35.58 | **35.58** | **28.1 ns** |
| 优化后（最小解码） | 60.48 / 59.68 / 59.96 / 60.73 / 60.47 | **60.47** | **16.5 ns** |
| 提升 | — | **1.70×** | −41% |

VideoChannel 端到端（`SfuPublisher → SFU → SfuSubscriber`，20,000 帧，
256B 载荷）：**6.15–6.48 M frames/s**（0.003 s/轮），优化前后在噪声范围内，
说明收益集中在 SFU 裸转发路径。

正确性：优化前后 `media_test_sfu` 均为 **31800 checks / 0 failures**；
每帧内容逐字节比对（`test_sfu` 用例 1/2/5），随机拓扑 3000 步对账通过。

---

## 4. 物理下限分析

### 4.1 数据量与拷贝

- 热路径**不读 payload、不拷贝 payload**：只访问 16B Bento 头 + 8B 视频子头；
  转发给订阅者的是指针（`SfuFragment::data`）。
- 若按「每帧 924B 拷贝给 2 个订阅者」估算：`924B × 60.47M × 2 ≈ 111.8 GB/s`，
  单核 `memcpy` 实测通常只有 15–25 GB/s，**拷贝方案在物理上就达不到当前吞吐**；
  零拷贝是本设计能跑到 60M frames/s 的前提。

### 4.2 依赖链估算（L1 命中）

工作集：1 个 room 节点 + 1 个 stream 节点 + 2 个订阅 + 统计字段 ≈ 1 KiB，
全部在 L1d（32 KiB/core）内。逐帧依赖链：

| 步骤 | 估算 cycles |
|---|---:|
| `std::map<std::string>` 房间查找（1 节点 + 短字符串比较，L1 4c） | 15–20 |
| `std::map<uint64>` 流查找（1 节点） | 8–12 |
| 两次 `std::function` 间接调用（目标稳定，可预测） | 6–10 |
| `DecodeHeader` + `DecodeVideoHeader` + 边界检查 | 8–12 |
| 统计字段自增（无原子） | 5–8 |
| **合计（估算下限）** | **42–62 cycles** |

实测：16.5 ns/帧。若按 4.5 GHz 计 ≈ **74 cycles/帧**，与估算下限的差距约
**1.2–1.8×**；若按基频 2.5 GHz 计则约 41 cycles，已在下限区间内。实际 boost
频率未锁定，因此这里只给区间，不给单点结论。

### 4.3 IPC 反推

- 优化前：276 Ir/帧 ÷ 28.1 ns ≈ 9.8 GIPS；按 4.5 GHz → IPC ≈ 2.2。
- 优化后：263 Ir/帧 ÷ 16.5 ns ≈ 15.9 GIPS；按 4.5 GHz → IPC ≈ 3.5
  （Zen 4 最大 6-wide，3.5 属于健康区间）。

指令数只降了 4.8%，吞吐却提升 70%，说明瓶颈不是「指令多」，而是
`ParseFrame` 在热路径上的**不可预测分支**（ACK/FEC/长度/类型）造成流水线
停顿；这属于微架构层面的优化证据。

### 4.4 分配 / 锁 / 原子

反汇编统计：`lock` = 0、`operator new`/`malloc` = 0、`@plt` = 0。
帧转发路径**零堆分配、零锁、零原子**；`std::function` 的调用目标在
`Subscribe` 时固定，热路径只有两次间接调用。

---

## 5. 结论与下一步

- 当前 16.5 ns/帧（2 订阅者）已接近「两次 map 查找 + 两次回调 + 头解析」的
  依赖链下限（估算 42–62 cycles），剩余差距主要来自每次调用重复解析房间名
  与流键。
- 下一步可选优化：给上层暴露 `SfuStreamRef`（订阅/发布时解析一次），
  `ForwardFragment` 直接按句柄转发，预计再降 20–30%（目标 ~11–13 ns/帧）。
  该改动会扩大 API 面，本轮未做。
- 已知边界：带 `kFlagAck` 的视频帧与 FEC 校验片不转发（`VideoChannel` 的
  数据分片不会带 ACK；FEC 跨 SFU 的恢复能力当前未实现，见模块文档 §9）。
