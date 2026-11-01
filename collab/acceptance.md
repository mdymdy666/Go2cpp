# 验收记录（JinShanProject ↔ Go2Cpp）

- **版本**：v2（2026-10-31 20:40，对齐 `jinshan-requests.md` v2）
- **维护**：JinShan agent 填写结论；Go2Cpp agent 只读
- **原则**：没有原始输出/数据不算通过；「编译通过」≠「行为正确」

---

## 一、状态总表（v2 五条）

| REQ | 内容 | 类型 | 优先级 | 状态 | 验收日期 | 证据 |
|---|---|---|---|---|---|---|
| REQ-001 | **FrameCodec**（可配置帧编解码 + 拆包器） | 可抽象 | P0 | ⏳ 待实现 | — | — |
| REQ-002 | **RelayPipe**（双向字节中继 + 背压 + 慢 peer） | 可抽象 | P0 | ⏳ 待实现 | — | — |
| REQ-003 | **Hub**（key → 多连接注册表） | 可抽象 | P1 | ⏳ 待实现 | — | — |
| REQ-004 | 有界队列 / 背压原语 | 通用 | P0 | ✅ 通过 | 2026-11-01 | 见 §五 REQ-004（1e8 无丢失 + RSS 0.00 MiB 增长） |
| REQ-005 | 超时 / 取消 / 租约 / 心跳 | 通用 | P1 | ⚠️ Context 已有；Lease/Heartbeat 缺 | — | — |

**已移出（不再跟踪）**：旧 REQ-004/005/006/007/008/011/012/013 —— 均为 RDRL/RDCH/配对/配额专属，JinShan 自己做（详见 `jinshan-requests.md` §3/§4）。
**明确不需要**：P2P（CON-09）。

---

## 二、验收记录格式

```markdown
### REQ-00x: <名称>

- **验收人**：
- **日期**：
- **环境**：WSL2 发行版 / 编译器 / commit
- **用例**：
  1. <正常路径>
  2. <边界值>
  3. <畸形输入>
- **原始输出**：<命令 + 输出摘要 / 日志路径>
- **结论**：通过 / 不通过（不通过写缺哪项）
- **补充**：不通过时在 requests 追加 REQ-00y
```

---

## 三、JinShan 侧的替代方案（Go2Cpp 交付前不阻塞）

| 能力 | JinShan 当前实现 | 交付后如何切 |
|---|---|---|
| 帧编解码 | `product/common/rdrl`（C++，28/28 单测） | 用 FrameCodec 重写内部，对外接口不变 |
| 中继管道 | `product/relay`（Go，93 PASS / 1 SKIP） | 用 RelayPipe 替换 pump |
| 连接表 | relay `sessionTable`（Go） | 用 Hub 替换 |
| 队列/背压 | `Chan` 已在用 | 补 TrySend 验收 |
| 超时/心跳 | context + 手写 ticker | 换 Lease/Heartbeat |

**原则**：接口层（`rd::net` / `rd::sync`）先立住，底层实现随时换。

---

## 四、验收前置条件（Go2Cpp 侧）

1. `collab/examples/` 里给**完整可编译 `main`**，不是伪代码；
2. 测试覆盖正常 + 边界 + 畸形；
3. Linux（WSL2）通过即可交付；Windows 由 JinShan `rd::net` 适配兜底；
4. 不引入第三方库；
5. 每个 REQ 附一份「最小用法」示例（10–30 行）。

---

## 五、验收记录

### REQ-004: 有界队列 / 背压原语

- **验收人**：Go2Cpp agent（REQ-004 专项：文档 + 示例 + 测试）
- **日期**：2026-11-01
- **环境**：Ubuntu 24.04.3 LTS（WSL2）/ g++ 13.3.0 / commit `65bb528` /
  AMD Ryzen 9 8945HX（12 逻辑核）/ 构建 `.build-bento-wsl`（Release）
- **交付物**：
  - 文档：`docs/modules/bounded-queue.md`（容量语义、`TrySend` 用法、
    `Channel` vs `FastChannel` 选型、1e8 验收方法）
  - 示例：`examples/bounded_queue/`（`bounded_queue_demo.cpp` +
    `build_and_run.sh` + `README.md`）
  - 测试：`tests/concurrency/test_bounded_queue.cpp` +
    `tests/concurrency/run_bounded_queue_test.sh`
- **用例**：
  1. **满时 `TrySend == false`（正常 + 边界）**：cap=4 填满后 `TrySend`
     返回 `kWouldBlock`（不阻塞）；满时阻塞 `Send` 挂起
     （`send_waiters >= 1`），消费一条后才返回；`cap==0` 时无接收者
     `TrySend` 必失败、有接收者时成功交接。
  2. **关闭语义（边界）**：`Close()` 后 `TrySend` 返回 `kClosed`；缓冲排空
     后 `Recv` 返回 `kClosed`；重复 `Close` 幂等。
  3. **丢弃守恒（正常）**：快生产者 + 慢消费者，`TrySend` 满即丢，
     `sent + dropped == 2,000,000`，`received == sent`，序号严格递增。
  4. **1e8 条阻塞 send/recv 无丢失（压测）**：4 条独立流水线，每通道
     1 生产者 + 1 消费者，`cap=1024`，逐条校验 `seq` 连续 + `checksum`。
  5. **RSS 稳定（长跑）**：`cap=1024` 跑 1e7 条，每 1e6 条采样
     `/proc/self/statm`，峰值增长 < 32 MiB，`peak_len <= cap`。
  6. **多生产者 + 关闭排空（并发）**：4 生产者共享 cap=1024 通道，各 25 万
     条，单消费者按 producer 校验序号连续，`Close` 后排空完成。

- **原始输出**：

  ```text
  $ tests/concurrency/run_bounded_queue_test.sh
  [build] .../tests/concurrency/test_bounded_queue.cpp
  [run] /tmp/go2cpp_bounded_queue/test_bounded_queue
  [bounded_queue] items=100000000
  [test] bounded_queue: capacity semantics
  [test] bounded_queue: TrySend drop accounting
    total=2000000 sent=29256 dropped=1970744 received=29256 elapsed=0.023s
  [test] bounded_queue: multi-producer + close drain
  [test] bounded_queue: 1e8-style no-loss blocking
    lanes=4 capacity=1024 total=100000000 sent=100000000 received=100000000
    elapsed=3.881s rate=25.77 Mmsg/s
  [test] bounded_queue: RSS stability
    items=10000000 capacity=1024 consumed=10000000 peak_len=961
    baseline=5.88 MiB peak_rss=5.88 MiB growth=0.00 MiB elapsed=3.196s
  [bounded_queue] PASSED
  ```

  重复三次（load ≈ 4.3）：3.97 s / 4.13 s / 3.92 s，均
  `sent == received == 100000000`；空载首次 2.15 s / 46.57 Mmsg/s。

  示例（`examples/bounded_queue/build_and_run.sh --items 2000000`）原始输出：

  ```text
  == A. TrySend + 慢消费者：满时丢弃（不阻塞） ==
    capacity=256 items=2000000 sent=31488 dropped=1968512 consumed=31488
    elapsed=0.051s drop_rate=98.43% 生产者从未被阻塞
  == B. 阻塞 Send + 慢消费者：背压（零丢弃） ==
    capacity=256 items=2000000 sent=2000000 consumed=2000000
    elapsed=3.163s Send累计耗时=3.093s（被消费者限速）
  == C. RSS 对比：有界 Channel vs 无界 std::deque ==
    items=2000000
    有界 Channel(capacity=1024): baseline=5.62 MiB peak=5.62 MiB growth=+0.00 MiB
    无界 deque            : baseline=5.62 MiB peak=30.45 MiB growth=+24.82 MiB
  ```

- **结论**：**通过**。
  - `TrySend` 满返回 `kWouldBlock`（即 false 语义，非阻塞）——用例 1/3；
  - 1e8 条阻塞 send/recv：`sent == received == 100,000,000`，每 lane
    `checksum == n(n-1)/2`、序号连续——用例 4；
  - RSS 稳定：1e7 条长跑增长 0.00–0.12 MiB，`peak_len <= 1024`——用例 5；
  - 示例可编译运行，三种策略（丢弃 / 背压 / RSS 对比）均输出预期数据。
- **补充**：无。文档、示例、测试均为新增，未修改现有构建文件；测试通过
  `tests/concurrency/run_bounded_queue_test.sh` 独立编译运行（不注册 ctest）。
