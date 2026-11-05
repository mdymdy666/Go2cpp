# 文档性能数字同步清单（doc ↔ report）

> **用途**：user/ 文档里每个性能数字，都能在这张表里找到「它来自哪份报告、哪个口径、什么时候该复查」。
> **规则**（与 `user/README.md` 的「性能数字约定」、`docs/metrics-ledger.md` 同源）：
> 1. 裸数字禁止入文档；必须写成「数值（口径）+ `docs/reports/<report>.md` + 日期」。
> 2. 报告更新 → 本表对应行改状态 → 引用它的文档在同一批次内同步。
> 3. 状态只有四种：`已同步` / `历史（未复跑，仅对照）` / `无出处（禁止引用）` / `待同步`。
> 4. 本表**不重复**报告里的数值正文，只维护「谁引用谁」——避免出现第三份真相。

## 一、映射表

| # | user/ 文档位置 | 指标 | 来源报告（权威） | 日期 | 状态 |
|---|---|---|---|---|---|
| 1 | `deep/14-tunnel.md` 头部 + §2.10 表 | tunnel 8 MiB raw 2385.3 MiB/s / 1.27×；mux 对照 770.5 / 3.59×；1 KiB p50 1.282 / p99 1.846 ms | `docs/reports/tunnel-raw-default-2026-11-04.md` | 2026-11-04 | 已同步 |
| 2 | `deep/14-tunnel.md` §2.10「历史数字」 | 1.96× / 2.99× / 488 MiB/s；旧默认 mux 830 MiB/s / 3.56× | `docs/modules/tunnel.md` §6（历史文档） | 2026-11-03 前 | 历史（未复跑，仅对照） |
| 3 | `deep/03-bento.md` AEAD 段 + 附-2 自检 | AEAD vs EVP 1.02–1.04×（64 KiB 1.02–1.03×、1 MiB 1.03–1.04×）；Poly 14.8–15.9 GB/s | `docs/reports/asm-tls.md` §「冲 1.1×（2026-11-04）」 | 2026-11-04 | 已同步 |
| 4 | `deep/03-bento.md` §0/§13 引用的 13.41× | 自研 AEAD 旧口径（纯标量/8 路前） | `docs/reports/asm-tls.md` §0（2026-11-02） | 2026-11-02 | 历史（已被 #3 取代） |
| 5 | `deep/02-network.md` §5.1 表 | 同上 AEAD 1.03–1.04×；TLS 传输层 mean 1.070× / p50 1.091× / p99 1.120× | `docs/reports/asm-tls.md` §0 + §「冲 1.1×」 | 2026-11-02 / 11-04 | 已同步 |
| 6 | `deep/02-network.md` §5.1 表 | Parser 头重解析 66.3 ms → 1.95 ms；chunked 4017 ms → 17 ms | `docs/reports/http-link-audit.md:22-23`（`d538326` / `e84bbec`） | 2026-11-04 | 已同步（1.95 ms 本身未复跑） |
| 7 | `deep/02-network.md` §5.1 表 | 单连接 8.9k rps；pipeline 8/16 53.7k/72.9k rps；8 连接 239.5k rps | `docs/reports/asm-http-e2e.md:233/127` | 2026-11-01 | 已同步（历史报告口径） |
| 8 | `deep/17-bufio.md` §2.11 | bufio 12387 → 17695 MiB/s（+43%）；vs Go 0.895× | `docs/reports/asm-bufio.md` §3 | 2026-11-04 | 已同步 |
| 9 | `deep/04-storage.md` §5.2/§5.3 | kv 读 1.109× / 1.252×；写 703,517 ops/s = 1.09×；写放大 36×→7× | `docs/reports/storage-opt.md` §1/§2 | 2026-11-04 | 已同步 |
| 10 | `deep/03-bento.md` §3.2/§3.3 | WS RSS 227.7 MB = 0.96×；p99 1.691 ms = 0.93× | `docs/reports/three-piece-status-2026-11-04.md:37,47`；`docs/reports/iomanager-et-investigation.md:530` | 2026-11-04 | 已同步 |
| 11 | `deep/03-bento.md` §3.2 开头 | WS 优化前基线 RSS 2.20–2.32×、p99 2.06–3.06×、20 万未测 | `docs/reports/ws-200k-bench.md` §0 | 2026-11-04 | 已同步（明确标「优化前基线」） |
| 12 | `deep/03-bento.md` §3.1 + 附-2 | Link 收发分离 p99 1405 ms → 0.045 ms、7.4× | 提交 `5a2c0fa`（无报告） | — | 历史（未复跑，结构可读） |
| 13 | `deep/01-concurrency.md` §5 | spawn 656–764 ns、channel 10.73 ns、mutex 3.75 ns、fiber 切换 23.5 ns | `docs/reports/2026-11-01-asm-concurrency-review.md`（引 ledger M16） | 2026-11-01 | 已同步（历史报告口径） |
| 14 | `deep/README.md` 铁律第 1 条 | 「2026-11-03 本机实测」写法示例 | 本清单 | 2026-11-05 | 已同步 |
| 15 | `modules/app/README.md` + `modules/app/stream_demo/README.md` | 示例输出 58.8 / 55.6 MiB/s | 示例自身 stdout | 当日 | 已同步（标注「示例单次输出，非基准」） |
| 16 | `docs/02-network.md` §6.3 | AEAD 13.41×（历史快照） | `docs/reports/asm-tls.md`（2026-11-02） | 2026-11-02 | 历史（已加「已被推翻」标注） |
| 17 | `docs/14-tunnel.md` §性能 | 1.96× / 488 MiB/s（历史快照） | `docs/modules/tunnel.md` §6 | 2026-11-03 前 | 历史（已加「已被 raw 取代」标注） |

## 二、无出处 / 禁止引用

| 数字 | 出处情况 | 处置 |
|---|---|---|
| rpc p50 **149 µs**、**177k/s** | `docs/reports/` 全目录 grep `149µs` / `177k` **0 命中**；`tests/bench/results/` 无 rpc 产物 | **禁止写进文档**。等报告落盘（建议 `docs/reports/rpc-bench-*.md`）后再引用 |
| 「153 项检查」tunnel | 历史文档 `docs/modules/tunnel.md:245`，`build/` 无产物 | 已在 `deep/14-tunnel.md` 标「未实测」 |
| HTTP pipeline 「411k rps」 | ledger 头部已记为「全库查无此数」 | 禁止引用 |

## 三、复查命令（防止再漂移）

```bash
# 1) 找 user/ 下的裸吞吐/倍数（人工判断是否带来源）
grep -rn -E '[0-9]+\.?[0-9]* *(MiB/s|GiB/s|GB/s|MB/s)' user/ --include=*.md

# 2) 高危旧值：出现即说明还有文档没同步
grep -rn -E '13\.41|488 MiB|1\.96×|0\.37×' user/ --include=*.md

# 3) 报告更新后，反查谁在引用它
grep -rn '<report-name>.md' user/ --include=*.md
```

## 四、已知缺口（本清单未覆盖，下一批要补）

1. **`docs/metrics-ledger.md` 落后于本轮报告**：台账只到 M16，M08 bufio 仍记 1853.9 MiB/s（旧值），
   **未收录** tunnel raw 2385 / TLS 1.02–1.04 / bufio 17695 / Parser 1.95 ms / WS 0.93× / kv 1.09×。
   台账规则第 1 条要求「文档数字必须指到台账一条记录」——当前这 6 组只能指到报告，**台账待补**。
2. **`user/docs/` 另有 8 份交接摘要含历史性能数字**（11-tools、12-app、13-relay-jinshan、15-os、
   16-go-api、04-storage、10-observability、07-p2p、03-bento、17-bufio）。本轮只给与今日报告**直接冲突**
   的两处（02-network 的 AEAD、14-tunnel 的吞吐）加了「历史快照」标注，其余仍是裸历史值。
3. **`docs/modules/*.md`** 同样含旧数字（`user/docs/` 引用的 1.96×/488 MiB/s 就出自
   `docs/modules/tunnel.md` §6），未在本轮范围（任务限定 user/）。

## 五、变更记录

| 日期 | 动作 |
|---|---|
| 2026-11-05 | 首版：13 处 user/ 文档数字同步到 2026-11-04/05 报告；新增 `user/README.md`「性能数字约定」、`deep/README.md` 铁律第 6 条；本清单建立 |
