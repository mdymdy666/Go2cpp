# bufio 三件套补做（A20 / TOP2）

日期：2026-11-04 · 环境：WSL2 / 12 vCPU / GCC 13.3 `-O3 -DNDEBUG` / 单线程（`taskset -c 4`）
范围：只补取证，**未改任何代码**（结论：无可零风险改动，优化点只记录）。

## 1. objdump（真实实例化符号）

数据取自 `.build-bento-wsl/go2cpp_bufio_test`（Release，无 sanitizer）里的模板实例化符号，
即 `tests/bufio/test_bufio.cpp` 中 M08 场景实际执行的同一份代码：

```bash
python3 asmstat.py .build-bento-wsl/go2cpp_bufio_test \
  "Reader<...MemorySource>::fill()" "Reader<...MemorySource>::Read(ByteSpan)" \
  "Writer<...MemorySink>::Flush()"
# 等价手写：objdump -dC --no-show-raw-insn <bin> | <按符号分段统计>
```

| 函数（符号） | 指令数 | call | 其中 memcpy/memmove | 栈帧 B（`sub rsp` / 含 push） |
|---|---|---|---|---|
| `Reader<MemorySource>::fill()` | 88 | 6 | 1（`memmove` 压缩前缀） | 56 / 72 |
| `Reader<MemorySource>::Read(ByteSpan)` | 103 | 8 | 1（交付 `memcpy`） | 72 / 104 |
| `Writer<MemorySink>::Flush()` | 58 | 6 | 0 | 48 / 72 |

- 与 Go 的对应关系：`fill` 对应 `bufio.Reader.fill`（同样一次底层 `Read` + 一次前缀搬移），
  `Read` 对应 `bufio.Reader.Read`（Go 1.22+ 有同样的「缓冲区空且 `len(p) >= len(buf)` 直读」快路径，
  本实现在 `reader.hpp:242-255` 也已实现）。
- `Read` 的 8 个 call 中 6 个是错误/EOF 冷路径（`clone .cold` 已分离，不进热路径）。
- 分配/释放：三个函数直接 `malloc/free` 为 0；缓冲在构造时一次分配。

## 2. 对标（Go 1.27.1，同口径）

Go 对照程序 `/tmp/gobufio/main.go`（临时，未入库）与 `test_bufio.cpp` 场景 5 **逐项同构**：
1 MiB 数据、底层按 `plan={1,3,7,13,64,1024,4096}` 循环返回、缓冲 4096、上层 512 B 读循环到 EOF、
每块 append 到预分配目标（两边拷贝次数一致）。

| 实现 | 轮数 | 吞吐 | 每次 512 B Read |
|---|---|---|---|
| **C++ bufio**（本仓，`go2cpp_bufio_test` 场景5） | 1（M08 历史值） | **1853.9 MiB/s** | 约 276 ns |
| **C++ bufio**（本次复跑 5 次） | 5 | 1596.7 / 1654.2 / 1707.5 / 1723.5 / 1767.3 MiB/s | 290–321 ns |
| **Go bufio**（`bufio.NewReaderSize(src,4096)`） | 21 | **median 20634.3 MiB/s**（best 22429.1） | 约 24 ns |

**结论：M08 有对标了，但结果是「落后约 12×」。** 这不是测量口径差异（拷贝次数、分块计划、
请求大小、缓冲容量、绑核全部一致），差异必须在实现层面：同样的 fill+memcpy 结构，
C++ 单次 512 B 交付耗时是 Go 的 12 倍，而 objdump 显示热函数只有 88/103 条指令
（物理下限：512 B 在 L1 上 memcpy ≈ 8–15 ns，加上两次分支约 20 ns；实测 290 ns，
**离下限约 15–20×**）。

**该结论的置信度**：中。Go 侧 21 轮已稳定（median 20.6 GiB/s、best 22.4 GiB/s），
C++ 侧 5 轮也稳定在上限 1.6–1.8 GiB/s，抖动不足以解释 12×。但**尚未定位根因**——
热函数指令数不支持「代码本身慢 12 倍」，怀疑点在 `MemorySource` 打包方式（`std::string` 值成员
+ `Reader` 持有指针，每轮构造）或 `Read(ByteSpan)` 的 `InvalidateUnread()`/`ByteSpan` 构造开销，
需要用 `perf stat`/`perf record` 定位，不在本次时间盒内。

## 3. 可优化点（**只记录，未改**）

1. `Reader::Read` 每次调用先 `InvalidateUnread()`（清 `m_can_unread_byte` + `m_last_rune_size`），
   512 B 路径上属于可省的 2 次 store；Go 用 `r.lastByte`/`r.lastRuneSize` 同样每次清，
   **不算退化**，不建议改。
2. `fill()` 的前缀压缩用 `memmove`，与 Go 同款（Go 也是 `copy` 搬移），**不建议改**。
3. 唯一值得跟进的是 §2 的 12× 差距本身——先 perf 定位，**不要在未定位前改代码**。

## 4. 复现

```bash
cd /UserData/CodexWorkSpace/Go2cpp
taskset -c 4 ./.build-bento-wsl/go2cpp_bufio_test          # 看「分块传输 1MiB」
cd /tmp/gobufio && taskset -c 4 go run main.go             # Go 对照（需 /tmp 脚本，未入库）
```

> Go 对照脚本是一次性的，未写入仓库（避免新增无法维护的基准）；若需要长期回归，
> 建议后续在 `tests/bufio/` 增 `bench_bufio_go_parity` 并登记 M08 的双口径。
