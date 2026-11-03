# Autopilot 进度（Go2Cpp）

> 节奏：每 30 分钟一轮（定时任务 `Go2Cpp 自动驾驶推进`，id `schedule-0ae729ea`）。每轮只做「下一个任务」一项（可派 1–2 个子 agent）。
> 规则：做完必须验证；失败如实记；WSL 真树 commit，push 走 Windows 侧 Python；更新本文件。
> 真树：`/UserData/CodexWorkSpace/Go2cpp`（分支 `feat/isolated`）。

## 总目标（三阶段，按顺序）

| 阶段 | 目标 | 完成判据 |
|---|---|---|
| A 数字台账 | 所有性能数字带「命令/环境/日期/脚本」，可一条命令复跑 | `tools/verify_metrics.sh` 跑完无未解释 STALE；docs 无孤立数字 |
| B 公网变服务 | VPS systemd + 本机 start-all + 探针 + 断线恢复 | VPS 重启后公网 200；kill client 自动恢复；探针 30 分钟无失败 |
| C 抖音 P0 | 6 服务 + 网关串成一个应用 | 5 个真实场景 curl 可复现；压测报告进 `docs/reports/` |

## 性能不达标处理规则（2026-11-03 用户补充）

当 ledger 判定某模块**同口径**下超判据（vs Go 或工业库 >1.1×）时：
1. 先开子 agent 做「口径对齐 + 基线复测」，确认不是测量/口径错误；
2. 确认不达标才优化，顺序强制：**架构（锁粒度/所有权/数据结构/并发模型）→ 汇编（指令/call/栈帧/分配）→ 正确性验证（功能 + 随机 + 压测 + 可选 TSAN）**；
3. 前后数据都写回 ledger；不得用「优化了」替代「达标了」；
4. 子 agent 不许 commit，主控审查（抽查行号/数字溯源/越界/正确性）后合并；
5. 风险高且时限内无法验证的改动，回滚并如实记「尝试过、因何回滚」。

## 进度表

| 任务 | 内容 | 状态 |
|---|---|---|
| A1 | ledger 建档（12 条，四要素） | ✅ be73a4e |
| A2 | `tools/verify_metrics.sh` 三基线 | ✅ 92dcb25 |
| A3 | M04/M05/M06 复跑填实 | ✅ 55a3f0e |
| A4a | M07 LevelDB 1e7 Put / 1e8 RSS | 🔄 子 agent `a535fda9` 在跑 |
| A4b | M10 os 批量 + gzip + hash + reflect | ✅ 本轮（M10 写 17.96×/读 14.68×；gzip 612/3648；hash SHA-256 2002 MiB/s；reflect 52.64/54.89） |
| A4c | TLS 握手/写路径 + bento/link 吞吐 | ⏳ 下一个 |
| A5 | ledger 一条命令全量对照 + CI 钩子 | ⏳ |
| A6 | 清理漂移（411k/160ns 等） | ⏳ |
| A7 | 不达标模块优化 | 🔄 首项 M01b 完成；**通用 Channel 无锁化待立项** |
| B1 | VPS `tunnel-server.service` | ⏳ |
| B2 | 本机 `start-all.sh` + 健康检查 | ⏳ |
| B3 | 外部探针（60s） | ⏳ |
| B4 | 断线恢复验证 | ⏳ |
| C1–C10 | 抖音 P0 | ⏳ |

## 下一个任务

**A4c** — TLS 与 bento/link：
1. **TLS**：`docs/reports/asm-tls.md` 的握手/32KB 写/64B 小包三组数字，找现成基准（`tests/network/*tls*` 或 `bench`），按当前源码重编后复跑 3 轮中位；跑不动写「无法复现 + 缺什么」。
2. **bento/link**：`tests/bento/` 是否有吞吐基准（Link loopback / PeerHub）；有就跑 3 轮，报 rps/字节吞吐与条件；没有就写「仅有功能测试，无吞吐基准，需新建」。
3. 同时确认 A4a 的 LevelDB 结果（若已回）并填 ledger M07。

## 本轮记录

- 13:20 启动；13:2x A1（be73a4e）；13:33 A2（92dcb25）；13:43 用户补充优化规则（456ec05）；13:50 A7 首项 M01b（3a7aa4f）；13:59 A3（55a3f0e）。
- 14:10 **A4b 完成**（`a6dffee9`）：
  - 新增 `tests/os/bench_vectored.cpp`（233 行，页缓存热/不 fsync/读前已写，3 次进程×3 轮）；
  - M10：写 822.8→49.1 ns/条 = **17.96×**（复现历史 17.9）；读 496.0→32~33 ns/条 = **14.68×**（未达历史 20.9×，条件不可考，不据此判退化）；主控复跑 17.66×/15.24× 可复现；
  - gzip：压缩 **612.2 MiB/s**、解压 **3648.0 MiB/s**（同轮 zlib 463.7/3138.3，压缩率不同不可只看吞吐）；交叉 bench 解压 5814.3；
  - hash：SHA-256 **2002.60 MiB/s**（SHA-NI）、crc32 18731.55 MiB/s；
  - reflect：二进制编码 **52.64 ns**、解码 **54.89 ns**；
  - 主控审查：文件 233 行、计时逻辑公平（memset 预热、不 fsync）、复跑数字同量级、`git status` 仅新增该文件 + 9 历史完好、旧库假低值说法成立（mtime：p1=10-30 / bento-wsl=11-03）。
  - ledger 新增 M11/M12/M13 并更新 M10。
