# user/ 文档性能数字同步（2026-11-05）

> 目标：把 `user/` 下文档里过时或无法溯源的性能数字，对齐到 `docs/reports/` 的最新实测，并建立防漂移机制。
> 范围：`user/deep/*.md`、`user/modules/*/README.md`、`user/README.md`（`user/docs/` 只处理与今日报告**直接冲突**的两处）。
> 铁律遵守：只改 `user/` 文档 + 新建本报告与同步清单，**不改代码、不 commit**。
> 真树：`/UserData/CodexWorkSpace/Go2cpp`（HEAD `876f391`，2026-11-05）。

## 0. 结论先行

| 项 | 数 |
|---|---:|
| 扫描命中行（吞吐/倍数/延迟/分位关键词） | **606** 行 |
| 其中人工逐条核对的「性能数字所在段落」 | **33** 处 |
| 过时或错误的数字段落 | **13** 处 |
| 已同步（改完并附来源+日期） | **13** 处 |
| 无法溯源（禁止进文档） | **1** 组（rpc p50 149 µs / 177k/s） |
| 保留为「历史，未复跑」并加标注 | **3** 处（tunnel 旧值、AEAD 13.41×、WS 优化前基线） |
| 新建防漂移资产 | 3 件（README 约定 / deep 铁律第 6 条 / `docs/plans/doc-perf-sync.md`） |

**最重要的两个发现不是「数字过时」，而是「来源引用错配」**（见 §3）：任务给的 Parser 与 WS 两处引用指向的报告里**根本没有该数字**。若照抄，等于给文档植入两个假来源。

## 1. 扫描方法与口径

```bash
# 第一次粗扫（606 行）：命中大量配置参数（100 ms 分片、5 ms watchdog、200 ms 轮询），需人工筛
grep -rn -E "MiB/s|GB/s|GiB/s|MB/s|ns/op|ops/s|QPS|p99|p50|ms|µs" user/ --include=*.md | wc -l   # 606

# 分口径
grep -rn -E "MiB/s|GiB/s|GB/s|MB/s" user/ --include=*.md | wc -l                                  # 45（改动前）
grep -rn -E "[0-9]+(\.[0-9]+)?[×x]" user/ --include=*.md | wc -l                                  # 194
grep -rn -E "p99|p50" user/ --include=*.md | wc -l                                                # 51
```

筛除规则：**只有「吞吐 / 延迟 / 倍数 / 分位」的结论性数字算性能数字**；超时、心跳间隔、轮询周期、
缓冲区大小等**配置常量**不算（它们不是测量结果）。

人工核对落在 33 个段落，集中在：`deep/01,02,03,04,14,17` + `modules/app`（2 处）+ `docs/02,14`（2 处）。
`deep/05-cache`、`08-rpc`、`07-p2p` 无结论性性能数字（前者的章节明确写「不抄无出处倍数」）。

## 2. 对照结果：13 处过时 / 错误

| # | 文件:位置 | 原内容 | 问题 | 处置 |
|---|---|---|---|---|
| 1 | `deep/14-tunnel.md:5-7` | 「凡涉及性能一律写未实测」 | 11-04 已有 raw 实测报告 | 改为「数字必须带报告名+日期」，写入 2385.3 MiB/s / 1.27× |
| 2 | `deep/14-tunnel.md` §2.10 | 「**性能全部未实测**，原因：build/ 为空」 | 理由已失效（有 `.build-bento-wsl` + soak 报告） | 换成 3 行实测表（raw/mux/1 KiB）+ 复跑命令 |
| 3 | `deep/14-tunnel.md:692` | 自检「知道所有数字都是未实测」 | 同上，教学点已反转 | 改为「能说清数字来自哪份报告/什么口径」 |
| 4 | `deep/03-bento.md:298` | 「现状是否追上 OpenSSL：**未实测**」 | 11-04 已复测 1.02–1.04× | 补 1.02–1.04×（64 KiB/1 MiB 分列）+ Poly 14.8–15.9 GB/s + 口径 |
| 5 | `deep/03-bento.md:702`（附-2） | 同上「现状未实测」 | 同一事实的第 2 处 | 同上（避免只改正文漏改清单） |
| 6 | `deep/02-network.md:805` | 「1MB **13.41×**」 | 旧口径（加 SIMD 前） | 改为 `13.41× → 1.03–1.04×（2026-11-04 复测）` + 来源 |
| 7 | `deep/02-network.md:807-808` | 「≥256B 被拉开——**纯标量代码的物理差距**，不要试图靠常量级优化追」 | **结论已被证伪**（IFMA 追上了） | 改为「已被复测证伪」+ 教训「别把当前实现当物理上限」 |
| 8 | `deep/02-network.md` §5.1 | **缺 Parser 数字** | 今日最大性能修复没进文档 | 补 2 行：66.3→1.95 ms、4017→17 ms（含提交号） |
| 9 | `deep/17-bufio.md` §2.11 | 只写「必须现场跑」，无任何数 | 有 11-04 实测却未收录 | 补 12387→17695 MiB/s（+43%）、0.895×，并写两条口径陷阱 |
| 10 | `user/README.md` | 无任何数字纪律条款 | 是漂移的根因（无约束） | 顶部新增「性能数字约定」6 条 |
| 11 | `deep/README.md` 铁律 | 只允许「2026-11-03 本机实测」写法 | 与「引用报告」冲突，实际不可执行 | 新增第 6 条：**可溯源**（报告名+口径+日期） |
| 12 | `modules/app/README.md:119` | 58.8 MiB/s 与 stream_demo 的 55.6 不一致 | 读者会以为其一有误 | 标注「示例单次输出，随机器波动，非基准」 |
| 13 | `docs/02-network.md:284`、`docs/14-tunnel.md:263-268` | 旧结论当事实陈述 | 交接摘要会被当现状引用 | 各加「⚠️ 历史快照，已被推翻/取代」+ 指向新报告 |

## 3. 两处「来源引用错配」（任务给的信息本身有误）

| 数字 | 任务给的来源 | 实际查证 | 正确来源 |
|---|---|---|---|
| Parser 66.3 ms → 1.95 ms；chunked 4017 → 17 ms | `docs/reports/http-parser-header-dup.md` | 该报告 150 行通篇讲**重复头 bug**，grep `66.3`/`4017`/`1.95` **0 命中** | `docs/reports/http-link-audit.md:22-23`（P2 `d538326`、P3 `e84bbec`）；`day-summary-2026-11-04.md:54-55` 有汇总 |
| WS p99 0.93× / RSS 0.96× | `docs/reports/ws-200k-bench.md` | 该报告 §0 结论是 **RSS 2.20–2.32×、p99 2.06–3.06×、未优于 Go**（优化前基线） | `docs/reports/three-piece-status-2026-11-04.md:37,47`（RSS 227.7 MB=0.96×、p99 1.691 ms=0.93×）；`iomanager-et-investigation.md:530` |

**教训**：报告文件名像不等于内容含有该数字。同步前必须 `grep` 报告正文确认命中，否则「附来源」反而制造假来源。这条已写进 `docs/plans/doc-perf-sync.md` §三 的复查命令。

## 4. 无法溯源：rpc p50 149 µs / 177k/s

```bash
grep -rn "149µs\|149us\|149 µs" . --include=*.md   # 0 命中
grep -rn "177k\|177,000\|177000" . --include=*.md   # 0 命中
ls tests/bench/results/                            # 只有 concurrency/persistence 产物，无 rpc
```

仓库内**没有任何报告或产物**记录这组数字。按 `metrics-ledger.md` 规则应归入「无出处，禁止引用」。

处置：**本轮不把这两个数字写进任何 user/ 文档**（`deep/08-rpc.md` 原本也没有 rpc 性能数字，只写配置参数）。
建议补一份 `docs/reports/rpc-bench-<date>.md`（含 p50/p99、QPS、连接数、payload、绑核、轮数），
落盘后再由本清单第 18 行回填。

## 5. 防漂移机制（三层，均已落地）

1. **`user/README.md` 顶部「性能数字约定」**（写文档的人第一眼看到）：
   禁止裸数字；来源只认 `docs/reports/`；无报告 → 写「未实测」；历史值必须标「历史数据，未复跑」；
   报告更新 → 引用方同步；指向同步清单。
2. **`deep/README.md` 铁律第 6 条**：数字可溯源 = 报告名 + 口径 + 日期，与既有 5 条铁律并列。
3. **`docs/plans/doc-perf-sync.md`**（新增，68 行）：17 行「文档 ↔ 报告」映射表（含状态）、
   无出处黑名单、3 条复查命令、已知缺口、变更记录。

设计取舍：**没有另起「第三套真相」**——清单只维护「谁引用谁」，数值正文仍只在 `docs/reports/`。
根因是项目已有 `docs/metrics-ledger.md`（ID 台账，规则更严），清单刻意与其互补而非替代。

## 6. 已知缺口（未做，如实记）

1. **`docs/metrics-ledger.md` 落后于报告**：台账只到 M16；M08 bufio 仍记 `1853.9 MiB/s`（旧值），
   **未收录** tunnel 2385 / TLS 1.02–1.04 / bufio 17695 / Parser 1.95 ms / WS 0.93× / kv 1.09×。
   台账规则要求「文档数字必须指到台账一条记录」，本轮这 6 组只能指到报告 → **台账待补**（不在本轮授权范围）。
2. **`user/docs/` 还有 8–10 份交接摘要含裸历史数字**（11-tools、12-app、13-relay-jinshan、15-os、
   16-go-api、04-storage、10-observability、07-p2p、03-bento、17-bufio）。本轮只标注了 2 处**与今日报告冲突**的；
   其余按新约定「允许保留但须标历史」，标注工作**未完成**。
3. **`docs/modules/*.md`** 含 `1.96×` / `488 MiB/s` 等旧值（`user/docs/` 的引用源头），本轮不动（任务限定 user/）。
4. **`deep/01-concurrency.md` §5 的 ns 级数字**来源于 `2026-11-01-asm-concurrency-review.md`（引 ledger M16），
   本轮未复跑、也未改（原本就有来源与日期，符合新约定）。
5. **未做端到端构建验证**：本轮只改 Markdown，未跑 `ctest`（任务不改代码、时间盒 40 分钟）。
   文档改动不参与编译，风险限于 Markdown 渲染，已逐段用 `sed -n` 复核表格/引用块闭合。

## 7. 验证

```bash
# 1) 13 处改动全部命中且唯一（脚本逐条断言 count==1）
#    → [OK] 10 + [OK] 3，失败 0
# 2) 高危旧值复查：残留处均带「历史/已推翻」标注
grep -rn -E '13\.41|488 MiB|1\.96×' user/ --include=*.md
#    → deep/14-tunnel.md:9,682（历史标注）、deep/02-network.md:805,810（新旧对照）、
#      deep/03-bento.md:298,702,305（旧口径被否定）、docs/02-network.md:284（历史快照）
# 3) 新增资产
wc -l docs/plans/doc-perf-sync.md                     # 68
grep -n "性能数字约定" user/README.md                  # 命中
grep -n "性能数字可溯源" user/deep/README.md            # 命中
```

## 8. 文件清单（本轮全部改动）

```
user/README.md                | +22  「性能数字约定」段
user/deep/README.md           | +4   铁律第 6 条
user/deep/14-tunnel.md        | +27/−14  头部纪律 + §2.10 实测表 + 自检
user/deep/03-bento.md         | +12/−2   AEAD 两处（正文 + 附-2）
user/deep/02-network.md       | +8/−3    AEAD 行 + Parser 两行 + 判读重写
user/deep/17-bufio.md         | +12     实测 + 两条口径陷阱
user/modules/app/README.md    | +1      示例输出标注
user/docs/02-network.md       | +5/−1   AEAD 历史快照标注
user/docs/14-tunnel.md        | +4      吞吐历史快照标注
docs/plans/doc-perf-sync.md   | 新建 68 行
docs/reports/doc-perf-sync-2026-11-05.md | 本报告
```
