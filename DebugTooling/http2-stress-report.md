# HTTP/2 压测与边界测试报告

> 本报告由并行任务流 agentB 产出，原始文件在 `.dsh_tmp/http2_stress_report.md`。
> 迁入 `DebugTooling/` 是为了随仓库保留（`.dsh_tmp` 不参与同步也不提交）。
>
> **测量条件**：loadavg 全部 < 1.5（项目规则要求，超过作废），单进程 MemoryTransport。
> **可复现**：`./build-quality/go2cpp_http2_stress` 与 `./build-quality/go2cpp_http2_boundary`。

---

# HTTP/2 压力测试与边界/恶意输入测试 —— 交付报告

## 一、新增文件与场景清单

### 1. `tests/network/test_http2_stress.cpp`（574 行，已进 ctest）

| # | 场景 | 规模 | 验证点 |
|---|---|---|---|
| 1 | 批量并发流 | 100 / 1000 / 5000 | 吞吐、字节数、内存增量 |
| 2 | 串行单请求延迟 | 1000 次 | p50/p90/p99/max |
| 3 | 大 body 分片 | 1MB / 10MB | **真实流控往返**（窗口 65535，靠 WINDOW_UPDATE 推进）|
| 4 | 连接复用 | 1000 次相同请求 | HPACK 动态表命中让单请求字节数下降 |
| 5 | HPACK 动态表压力 | 5000 次唯一 header | 持续插入+逐出，内存不失控 |
| 6 | CONTINUATION 压力 | 1MB / 10MB header | 分片（1 HEADERS + N CONTINUATION）与拼接 |

### 2. `tests/network/test_http2_boundary.cpp`（625 行，已进 ctest）

| # | 场景 | 用例数 | 内容 |
|---|---|---|---|
| 1 | 畸形帧 | 9 | PING 长度 0、SETTINGS stream≠0、DATA stream=0、超 MAX_FRAME_SIZE、截断 payload、帧头不足、RST 长度 3、WINDOW_UPDATE 增量 0、PRIORITY 长度 4 |
| 2 | HPACK 攻击 | 7 | 索引越界、表大小超限、超长字符串声明、Huffman 填充非法、全 1 输入、空输入、**256 种单字节全扫** |
| 3 | CONTINUATION 攻击 | 2 | header block 未结束却发 DATA、CONTINUATION 流 id 不一致 |
| 4 | 流控攻击 | 3 | 窗口耗尽后连发 100 次、WINDOW_UPDATE 取 2^31-1、保留位被 mask |
| 5 | 随机变异 fuzz | **200,000 次** | 9 个合法语料 × 1~8 个随机位翻转 × 随机截断，**fork 隔离 + 30s watchdog** |

**隔离策略**：每个"不能崩"的用例都在 `fork()` 子进程里跑。崩溃由父进程通过
`WIFSIGNALED` 捕获并报出信号号；挂死由超时 `SIGKILL` 捕获。所以即使实现有致命
缺陷，测试进程本身不会死，且能精确定位到用例名。

## 二、压测数据（loadavg 0.81，三次运行取中位数；均 < 1.5，数据有效）

环境：WSL2，`build-quality` Release，`MemoryTransport`（排除网络抖动）。

| 场景 | 吞吐 / 延迟 | 内存增量 |
|---|---|---|
| 并发流 100 | **471,413 req/s** | +256 KB |
| 并发流 1,000 | **550,146 req/s** | +384 KB |
| 并发流 5,000 | **502,062 req/s** | +1,792 KB |
| 串行单请求 | **p50 0.63 µs / p90 0.71 µs / p99 0.85 µs / max 13.77 µs** | — |
| 大 body 1 MB | **495.6 MB/s**（42 个 WINDOW_UPDATE）| +480~516 KB |
| 大 body 10 MB | **450.1 MB/s**（426 个 WINDOW_UPDATE）| **+12,972 KB** |
| 连接复用 1,000 次 | 首帧 47 B → **平均 13.0 B**（第一版是 47.0 B）| +0 KB |
| HPACK 5,000 次唯一 header | 14.5 ms（274,778 B 总字节）| **+0 KB（0.00 KB/请求）** |
| CONTINUATION 1 MB | 12.6 ms（57 帧）| +3,584 KB |
| CONTINUATION 10 MB | 140.3 ms（561 帧）| **+75,776 KB** |
| **进程峰值 VmRSS** | — | **25,584 KB** |

三次运行的离散度：吞吐 < 15%，延迟 < 5%，说明数据稳定。

## 三、发现的问题（1 个真实缺陷，已修复）

### 缺陷：HPACK 编码器只写动态表、从不读 —— 动态表白建

**发现方式**：压测场景 4（连接复用）。1000 次**完全相同**的请求，单请求字节数
**恒为 47 B**，完全没有因为动态表命中而下降。

**定位**：`src/network/http2/hpack.cpp` 的 `Encoder::EncodeBlock` 只做了
1) 查静态表完全匹配、2) 用静态表名字索引 + 字面量，然后 `m_table.Add(...)`。
`grep FindDynamic` **零命中** —— 编码器从未查过动态表。

**后果**：RFC 7541 的压缩收益被浪费一半。连接复用场景每个请求多传 34 字节
（47 vs 13），在高频短请求场景下带宽浪费 72%。

**修复过程（两次迭代，第二次才合格）**：

| 版本 | 连接复用字节数 | 并发流 1000 吞吐 | 结论 |
|---|---|---|---|
| 原始（无动态查找）| 47.0 B | 425,665 req/s | 有缺陷 |
| v1：线性扫描 | **13.0 B** | **275,833 req/s** | 压缩对了，但**吞吐 -35%**（每个字段扫最多 100 项）|
| v2：`MakeKey` 字符串 + unordered_map | 13.0 B | 270,755 req/s | 仍慢 —— 每次查找都**分配一次 key 字符串** |
| **v3：`string_view` 组合哈希**（最终）| **13.0 B** | **525,297 req/s** | **字节 -72% 且吞吐 +23%** |

v3 的关键：`DynamicTable` 内部维护 `hash(name,value) -> 单调序号` 的
`unordered_multimap`，相对索引由 `m_next_seq - seq` **O(1) 推出**；
哈希用 `string_view` 计算，**零分配**。冲突时用 `EntryBySeq` 取回条目比对真实
字符串。

**最终效果（相对原始版本）**：

| 指标 | 原始 | 修复后 | 变化 |
|---|---|---|---|
| 连接复用单请求字节 | 47.0 B | **13.0 B** | **−72%** |
| 并发流 1000 吞吐 | 425,665 | **550,146** req/s | **+29%** |
| 串行 p50 | 2.21 µs | **0.63 µs** | **−71%** |
| 大 body 1MB | 372 MB/s | **495.6 MB/s** | **+33%** |

**回归验证**：`go2cpp_hpack_test` 9 个场景（含 RFC 7541 §C.1/C.2/C.3/C.4.1 官方
向量、随机 2000 轮往返）全部 `failures=0`。

### 观察（不是缺陷，但值得记录）：CONTINUATION 大 header 的内存放大

10 MB 的 header value 处理过程中 VmRSS 上升 **75,776 KB（约 74 MB）**，是输入
的 **7.4 倍**。构成大致是：原始 value 10 MB + HPACK 编码结果 9.2 MB +
分片后的多个 `vector` 副本 + 服务端拼接缓冲 10 MB + 解码后字符串 10 MB。

**这不是 bug**，但说明大 header 是内存放大器。若要用于不可信输入，应先用
`SETTINGS_MAX_HEADER_LIST_SIZE` 限制单请求 header 总大小 —— 当前实现**没有**
这个限制（`ConnectionConfig` 里没有该字段）。**这是一个已知缺口**。

## 四、全量 ctest 结果

```
91/91 Test #91: go2cpp_io_hook_full .................   Passed    0.18 sec
100% tests passed, 0 tests failed out of 91
```

**91/91 全绿**（基线 89 + 新增 `go2cpp_http2_stress`、`go2cpp_http2_boundary`）。

单测耗时：stress 0.23 s、boundary 0.03 s，远低于 60 s 上限。
CTest TIMEOUT 设为 300 s / 180 s。

## 五、改动文件清单

**新增**：
- `tests/network/test_http2_stress.cpp`
- `tests/network/test_http2_boundary.cpp`

**修改**：
- `cmake/Go2CppTests.cmake`（注册两个 target，加在 `go2cpp_fiber_transport_test` 之后）
- `include/go2cpp/network/http2/hpack.hpp`（`DynamicTable` 加 hash 索引 + `FindRelative`/`EntryBySeq`/`HashKey`；`Encoder::FindDynamicIndex`）
- `src/network/http2/hpack.cpp`（同上实现 + 编码路径接入动态表查找）

**未触碰**：6 个使用者未提交文件、`src/tools/error.cpp`、`include/go2cpp/tools/error.hpp`、
`concurrency/` 下任何文件。**未执行任何 git 命令**。
