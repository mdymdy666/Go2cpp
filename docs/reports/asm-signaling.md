# 汇编级性能审查：SDP 信令（signaling）

> 模块：`include/go2cpp/media/signaling.hpp`、`src/media/signaling.cpp`
> 基准：`tests/media/test_signaling.cpp`（10,000 次 SDP 文本往返解析）
> 日期：2026-11-01；状态：现行

---

## 1. 定位：控制面，不是数据面

信令只在会话建立时运行一次（offer/answer + 候选 trickle），不在每帧路径上。
它的物理下限不是内存带宽，而是：

1. 一次 `malloc/free` 的代价（约 50–100 cycles）；
2. 网络 RTT（10–100 ms，比解析时间大 3–4 个数量级）；
3. 字符串字段的必然拷贝（SDP 字段要长期保存在 `SessionDescription` 里）。

因此本文给出**真实数据**与**差距分析**，不把 7 µs 的解析包装成「接近极限」。

---

## 2. 测试环境

与 `docs/reports/asm-sfu.md` §1 相同：WSL2 / Ubuntu 24.04、Ryzen 9 8945HX、
g++ 13.3.0、Release `-O3 -DNDEBUG`、单线程。

复现：

```bash
cmake --build .build-bento-wsl --target media_test_signaling -j 8
.build-bento-wsl/media_test_signaling | grep "SDP parse"
```

---

## 3. 热函数反汇编

`SessionDescription::ParseSdp`（mangled
`_ZN6go2cpp5media18SessionDescription8ParseSdpESt17basic_string_viewIcSt11char_traitsIcEEPNSt7__cxx1112basic_stringIcS4_SaIcEEE`）：

| 指标 | 值 |
|---|---:|
| 静态指令数 | 1,609 |
| `call` 数 | 186 |
| 条件跳转数 | 244 |
| `lock` / 原子指令 | 0 |
| 栈 canary（`fs:0x28`）引用 | 4 |
| 直接 `@plt` 引用 | 0（同 TU 内调用被折叠为本地符号） |
| 自身函数动态指令（callgrind，10,050 次） | 80,900,136（≈ 8,050 Ir/次） |
| **含子调用动态指令（inclusive）** | **1,353,313,071（≈ 134,600 Ir/次）** |

```bash
objdump -d --no-show-raw-insn \
  --disassemble=_ZN6go2cpp5media18SessionDescription8ParseSdpESt17basic_string_viewIcSt11char_traitsIcEEPNSt7__cxx1112basic_stringIcS4_SaIcEEE \
  .build-bento-wsl/CMakeFiles/go2cpp_media.dir/src/media/signaling.cpp.o

valgrind --tool=callgrind --callgrind-out-file=/tmp/cg_sig.out --quiet \
  .build-bento-wsl/media_test_signaling
callgrind_annotate --inclusive=yes --auto=no /tmp/cg_sig.out | grep ParseSdp
```

「自身 8k / inclusive 134k」的 16 倍差距说明：**时间几乎全在
`std::string` 构造、`std::vector` 增长与 `malloc/free` 上**，不在解析逻辑本身。

---

## 4. 前/后性能数据

优化：`SplitLines` / `SplitWhitespace` 从「每行/每 token 构造 `std::string`」
改为返回 `std::string_view`（指向输入 SDP 的零拷贝视图），并预留容量。
`IceCandidate::Parse` 的 token 同样改为 view；解析结果的字段仍然拷贝一次到
`SessionDescription`（这是长期保存所必需的）。

同一基准（10k 次、同一份 2 路媒体 + 6 候选的 offer SDP，Release）：

| 版本 | 采样（ns/次） | 中位数 | 吞吐 |
|---|---|---:|---:|
| 优化前（`vector<string>` tokenizer） | 7399 / 7579 / 7412 | 7,412 ns | 135k parses/s |
| 优化后（`vector<string_view>`） | 6530 / 6693 / 6584 / 6725 / 6741 / 6789 / 6656 / 6494 / 6634 / 6477 | 6,656 ns | 150k parses/s |
| 变化 | — | **−10.2%** | +11% |

正确性：优化前后 `media_test_signaling` 均 **3998 checks / 0 failures**，
包含 SDP 逐字节往返、随机 500 次变异、1000 个随机候选。

---

## 5. 物理下限分析

### 5.1 分配成本（主项）

一次解析至少要保存：

- 1 个会话 id、1 个 origin 地址、1 个会话名；
- 2 路媒体：mid、protocol、connection address、ufrag、pwd、fingerprint、
  msid（每路约 5–6 个字符串）；
- 2 个编解码：encoding name、fmtp；
- 6 条候选（每路 3 条）：foundation、address、related address、transport。

按小字符串（SSO 阈值 15 字符）估算，**约 40–70 次堆分配/解析**，其中一部分
走 SSO 不分配，实际约 30–50 次。取 50–100 cycles/次，仅分配就
**2,500–5,000 cycles ≈ 0.6–1.1 µs**（按 4.5 GHz）；加上自身 8k Ir
（≈ 2k cycles）与分支/缓存代价，**理论下限约 1.5–3 µs**。
实测 6.66 µs → **差距约 2.2–4.4×**。

### 5.2 为什么不再继续优化

- 控制面一次调用 6.66 µs，相对一次 offer/answer 的网络 RTT（≥10 ms）是
  0.07%，端到端不可见；
- 继续优化需要「描述字段零拷贝」——把 SDP 文本保存在 session 内并用
  `string_view` 引用（arena 方案），会把 `SessionDescription` 的生命周期与
  输入缓冲绑定，API 复杂度显著上升，收益只在批量离线解析场景体现；
- 本轮已消除 tokenizer 的逐行/逐 token 拷贝（−10%），收益/风险比合理。

### 5.3 锁 / 原子 / 并发

反汇编 `lock` = 0；`SignalingSession` 单线程使用，无锁、无原子。
候选去重是 O(n²) 的小数组线性扫描（n ≤ 数十），比引入哈希表更省。

---

## 6. 结论

| 项 | 数据 |
|---|---|
| 单次 `ParseSdp` | 6.48–6.79 µs（中位 6.66 µs） |
| 自身动态指令 | ≈ 8,050 Ir/次 |
| 含子调用动态指令 | ≈ 134,600 Ir/次（分配主导） |
| 分配（估算） | 30–50 次/解析 |
| 锁 / 原子 | 0 |
| 与估算下限差距 | 2.2–4.4×（控制面，可接受） |
| 回归 | 3998 checks / 0 failures |

下一步（若需要）：arena 化 `SessionDescription`，目标 2–3 µs；当前不做，
因为端到端收益被网络 RTT 淹没。
