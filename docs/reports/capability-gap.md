# Go2Cpp 内部能力盘点与缺口报告

> **范围**：WSL 真树 `/UserData/CodexWorkSpace/Go2cpp`（小写 `pp`；不是 `E:\CodexWorkspace\Go2Cpp` 旧副本）。
> 分支 `feat/isolated`，HEAD `a885d8c`。
>
> **性质**：只读盘点，不改代码、不 commit；唯一新增文件为本报告。
>
> **方法**：`include/go2cpp/` 逐模块统计（文件数/行数）× `tests/` 对照 × `docs/modules/` 文档对照；
> `docs/metrics-ledger.md` 逐条摘录；`scripts/ci.sh` 实测；`app/douyin`、`app/bilibili` 验收材料核查。
>
> **证据约定**：`文件:行号` / 命令输出 / 台账编号（`Mxx`）。凡无证据的判断一律标注「未验证」。
>
> **时间盒**：2026-11-04，单轮 30 分钟。

---

## 一、模块能力盘点

### 1.1 规模总览

| 项 | 实测值 | 命令 |
|---|---:|---|
| `include/go2cpp/` 子模块目录 | **33** | `ls -1d include/go2cpp/*/ \| wc -l` |
| 子模块头文件总行数 | **103,284** | `find include/go2cpp -name '*.h' -o -name '*.hpp' \| xargs cat \| wc -l` |
| 根级转发头 | 42 个 / 900 行 | `ls include/go2cpp/*.h*` / `cat ... \| wc -l` |
| `src/` 实现 | 238 文件 / **160,157 行** | `find src -type f \| wc -l`；`cat src/**/*.{cpp,hpp} \| wc -l` |
| `tests/` | 730 文件 / 429 个 `.cpp` / **240,840 行** | `find tests -type f`；`find tests -name '*.cpp'` |
| `app/`（两个 demo） | 143 文件 / 10,567 行 | `find app -type f`；`cat app/**/*.{cpp,hpp,go,py,js,html} \| wc -l` |
| ctest 注册用例 | **278** | `ctest --test-dir .build-bento-wsl -N`（末行 `Total Tests: 278`） |

测试代码量（24.1 万行）是头文件（10.3 万行）的 **2.3 倍**，测试投入高于库本体，这是本项目的显著特征。

### 1.2 子模块矩阵

判定规则（先写死，避免事后解释）：

- **★★★**：有专属测试目录（`tests/<mod>/` 非空）+ 有 `docs/modules/` 对应文档 + 台账/基准有该模块数字；
- **★★**：三者占两项；
- **★**：只占一项；
- **○**：转发头/空壳（头文件 ≤ 10 行，实现不在此目录）。

「生产可用」判定：**可** = 有测试 + 有文档 + 有台账且无已知语义缺陷；**有条件** = 有测试但缺基准或存在明确边界；
**不建议** = 已知语义缺陷 / 模块孤岛 / 无验收材料。

| 模块 | 头文件数 | 头文件行数 | 测试文件数 | 文档 | 台账/基准 | 成熟度 | 生产可用 |
|---|---:|---:|---:|---|---|---|---|
| `app` | 13 | 3,364 | 14 | `app-framework.md` | 无 | ★★ | 有条件（demo 级） |
| `bento` | 49 | 14,983 | 106 | 9 篇 `bento-*.md` | bench（M15） | ★★★ | 有条件（UDP 传输） |
| `bufio` | 9 | 2,404 | 4 | 无专文 | M08 | ★★ | 可 |
| `cache` | 15 | 4,233 | 16 | `cache.md` | M09 | ★★★ | 可 |
| `compress` | 10 | 1,817 | 10 | 无专文 | M11 | ★★ | 可 |
| `concurrency` | 20 | 13,124 | 94 | 5 篇（channel/fiber/scheduler/sync/select） | M01–M04、M16 | ★★★ | 有条件（协作式） |
| `config` | 5 | 3,664 | 5 | `config.md` | 无 | ★★ | 可 |
| `container` | 5 | 1,297 | 10 | 无专文 | 无 | ★ | 可（STL 级） |
| `core` | 3 | 28 | 0 | 无 | 无 | ○ | 不建议（空壳） |
| `debug` | 14 | 5,567 | 29 | `debug.md` | 无 | ★★ | 有条件（诊断用） |
| `detail` | 1 | 4 | 0 | 无 | 无 | ○ | 内部实现 |
| `distributed` | 7 | 1,263 | 11 | `distributed.md` | 无（本报告引用分析） | ★★ | **不建议**（见 §4.4） |
| `encoding` | 7 | 1,244 | 9 | 无专文 | reflect 另计（M13） | ★ | 可（JSON/CSV） |
| `error` | 7 | 2,893 | 6 | `error-thread-fiber.md` | 无 | ★★ | 可 |
| `frame` | 1 | 360 | 1 | `frame-codec.md` | 无 | ★ | 可（小模块） |
| `go` | 12 | 4,871 | 0（`tests/go_api` 13 个） | `go-api-compat.md`、`go-stdlib-gaps.md` | 无 | ★★ | 有条件（兼容层） |
| `hash` | 8 | 1,539 | 7 | 无专文 | M12 | ★★ | 可 |
| `logging` | 3 | 2,433 | 5 | `logging.md` | 无 | ★★ | 可 |
| `math` | 2 | 750 | 3 | 无专文 | 无 | ★ | 可 |
| `media` | 14 | 4,168 | 17 | `media-audio.md`、`webrtc-media.md` | 无 | ★★ | 有条件 |
| `metrics` | 3 | 978 | 4 | `metrics.md`、`metrics-prometheus.md` | 无台账 | ★★ | 可（缺 trace） |
| `network` | 33 | 6,821 | 31 | 7 篇（http1/http2/tls/ws/sse/network） | M05、M14、M15 | ★★★ | 有条件（Linux epoll） |
| `os` | 26 | 5,260 | 4（递归 13） | `os.md` | M10 | ★★★ | 可（Linux） |
| `p2p` | 11 | 1,529 | 10 | `p2p-nat-traversal.md` | 无 | ★★ | 有条件（NAT 环境） |
| `path` | 2 | 565 | 2 | 无专文 | 无 | ★ | 可 |
| `rd_net` | 4 | 846 | 8 | 无专文（有 `rdnet-v3-review.md`） | 无 | ★★ | 有条件（协议骨架） |
| `reflect` | 12 | 3,572 | 1 | 无专文 | M13 | ★★ | 有条件 |
| `relay` | 3 | 611 | 11 | `relay-pipe.md` | 无 | ★★ | 可（管道原语） |
| `rpc` | 8 | 2,130 | 6 | `rpc.md` | 无 | ★★ | 有条件 |
| `scheduler` | 1 | 4 | 0（测试在 `tests/concurrency`） | `scheduler.md` | M03、M04、M16 | ○（转发头） | 见 `concurrency` |
| `storage` | 7 | 2,954 | 15 | 6 篇 `storage-*.md` | M06、M07 | ★★★ | 有条件（SQLite 单写者） |
| `tools` | 13 | 6,629 | 11 | `tools.md` | M11、M12 | ★★★ | 可 |
| `tunnel` | 5 | 1,379 | 1 | `tunnel.md` | 无 | ★★ | 有条件（demo） |

**读表要点**：

1. 33 个目录中 `core`（28 行）、`detail`（4 行）、`scheduler`（4 行）是空壳/转发头，**有效模块约 30 个**；
2. `bento`（1.5 万行）、`concurrency`（1.3 万行）是体量最大的两块，也是测试最厚的两块（106 / 94 个测试文件）；
3. `go/`（4871 行）**没有同名测试目录**，测试在 `tests/go_api/`（13 文件），命名不对齐会误导盘点；
4. 文档覆盖率高：`docs/modules/` 58 篇，但 `bufio`/`compress`/`encoding`/`hash`/`reflect`/`rd_net` 等**没有专文**，只有 README 一句话。

---

## 二、性能账本（`docs/metrics-ledger.md`，297 行）

台账共 **19 条记录 ID**（`M01` 拆为 `M01b-a`/`M01b-b`，另有 `M05b`）。按台账自身定义的状态逐条摘录：

| ID | 指标 | 结论 | 状态字段（原文口径） | 证据行 |
|---|---|---|---|---|
| M01 | Channel 1P1C | 历史 10.73 实为 FastChannel | `已拆条` | :39 |
| M01b-a | 通用 `Channel<uint64_t>` | 112.6 ns vs Go 23.36；**语义增强代价，不按不达标判** | `实测（口径不可比）` | :49 |
| M01b-b | `FastChannel<1024>` | 16.07 / 6.23 ns，优于 Go 23.36 | `实测（达标）` | :63 |
| M02 | Fiber 切换 | 19.84 ns/往返（推断口径，历史 33.80 未复现） | `实测（推断口径）` | :73 |
| M03 | Scheduler wall | 本轮中位 239 ns；历史 122 无出处；**不达标** | `实测（不达标）` | :89 |
| M04 | Scheduler submit | batch=1024 中位 119.0；batch=65536 中位 355.7 | `实测（双口径）` | :118 |
| M05 | HTTP/1.1 pipeline=16 | 120,755 rps（6 轮中位），p99 15–16 µs | `实测` | :127 |
| M05b | 「411k rps」 | 全库查无出处，禁止引用 | `无出处` | :138 |
| M06 | SQLite 1e6 插入 | 1.074×，达标（<1.1×） | `实测（达标）` | :147 |
| M07 | LevelDB 1e7 Put | 647.5k ops/s；1e8 RSS 82.4 MiB | `实测` | :157–160 |
| M08 | bufio 1 MiB | 1,853.9 MiB/s | `实测` | :170 |
| M09 | Cache hotspot | ARC 0.7764 / LRU 0.5594 | `实测` | :179 |
| M10 | os writev/readv | 写 ≈111×、读 ≈213× | `实测（已优化）` | :188 |
| M11 | gzip | 压缩 612.2 / 解压 3,648.0 MiB/s | `实测` | :201 |
| M12 | hash | SHA-256 2,002.6 MiB/s、crc32 18,731.6 MiB/s | `实测` | :211 |
| M13 | reflect | 编码 52.64 / 解码 54.89 ns | `实测` | :221 |
| M14 | TLS | 握手 1.061×（复跑 1.178×，略超线） | `实测（达标，需更多轮）` | :231 |
| M15 | bento/link | 单队列 282,210 pps；2 核多队列丢包 39–48% | `实测` | :241 |
| M16 | Go scheduler 基线 | Go@4 162.7；C++ 同轮 359 → **2.21×，不达标** | `实测（不达标）` | :252 |

### 2.1 达标率统计（按台账原文状态，不替我加戏）

| 分类 | 条数 | 占比 | 明细 |
|---|---:|---:|---|
| 明确达标 | **3** | 15.8% | M01b-b、M06、M14 |
| 明确不达标 | **2** | 10.5% | M03、M16 |
| 口径不可比/推断 | **2** | 10.5% | M01b-a、M02 |
| 无出处（禁止引用） | **1** | 5.3% | M05b |
| 已拆条（不构成独立结论） | **1** | 5.3% | M01 |
| 仅实测、**无达标判据** | **10** | 52.6% | M04、M05、M07、M08、M09、M10、M11、M12、M13、M15 |
| 合计 | 19 | 100% | — |

**结论一**：台账里能直接当「达标」引用的只有 **3/19（15.8%）**。
**结论二**：超过一半（10/19）是「测到了数，但没写清对标谁、多少算达标」——数字可复现，判据缺失。
**结论三**：M03/M16 的调度器差距已被明确收口为**架构代价**（台账 :99–101：「2.21× 不是实现差，而是语义更重的代价」），不再作为优化项。

---

## 三、测试与 CI

### 3.1 实测结果（两种口径都跑了，结论不同）

**注册数**：`ctest --test-dir .build-bento-wsl -N` → `Total Tests: 278`。

**口径 A：串行 `scripts/ci.sh`（复用构建目录）**

```bash
cd /UserData/CodexWorkSpace/Go2cpp
bash scripts/ci.sh --build-dir .build-bento-wsl --no-configure
```

结果：**未跑完**。`ctest` 从 10:49 开始，跑到 `#7 relay_hub_tsan` 后超过 20 分钟无推进（`ci-logs/ctest.log` 停在 `Start 7: relay_hub_tsan`），30 分钟时间盒内无法收口。
`ci.sh` 给 ctest 的 `--timeout 600` 没有让该测试在 10 分钟内退出——**串行全量 CI 在 WSL 上会卡死**，这本身是一个稳定性证据。

**口径 B：并行 `ctest -j16 --timeout 120`（同一构建目录）**

结果：**271 passed / 7 failed / 4 skipped**，总耗时 **124.82 s**（`Total Test time (real) = 124.82 sec`）。

| # | 失败项 | 现象 | 性质 |
|---|---|---|---|
| 13 | `go2cpp_tests` | 线程数/调度假设不成立 | 环境相关（ci.sh:214 已列） |
| 77 | `go2cpp_debug_module_demo` | demo 退出码非 0 | 需串行复核 |
| 150 | `go2cpp_io_hook_demo` | WSL hook 语义 | 环境相关（ci.sh:214 已列） |
| 151 | `go2cpp_io_hook_full` | 同上 | 环境相关（ci.sh:214 已列） |
| 161 | `bento_http_server` | `***Timeout 120.01 sec` | 并行端口/资源竞争嫌疑，需串行复核 |
| 257 | `tunnel` | 失败 | 需串行复核 |
| 278 | `docs_consistency` | README 290 项 vs 实测 278 | 文档漂移（ci.sh:214 已列） |

4 项 skipped：`tools-migration`、`tools-cli`、`tools-replay`、`bento_netem`。

**与任务背景「270 passed / 4 failed / 4 skipped」的差异**：本轮没有复现该数字。口径不同（串行 vs `-j16`、timeout 600 vs 120），并行下新增 3 项失败（debug demo、bento_http_server 超时、tunnel）。**在拿到串行完整结果前，不应把 270/4/4 当作稳定事实引用**。

`scripts/ci.sh:210-216` 自己承认 `io_hook`/`go2cpp_tests`/`docs_consistency` 是「已知环境相关失败」，并提供 `CI_ALLOW_KNOWN=1` 排除开关——说明 **CI 红灯不是未知故障，而是被接受的常态**。

### 3.2 覆盖范围

`tests/` 下 35 个子目录，覆盖：app、bento、bufio、cache、compress、concurrency、config、container、debug、distributed、encoding、enhanced、error、frame、go_api、hash、logging、math、media、metrics、network、os、p2p、path、rd_net、reflect、relay、replay、rpc、scenarios、storage、tools、tunnel、windows。

- 有测试的模块：**30/33**（缺 `core`、`detail`、`scheduler` 独立目录）；
- 测试最厚的模块：bento（106）、concurrency（94）、network（31）、debug（30）；
- 最薄的模块：frame（1）、reflect（1）、tunnel（1）、windows（1）。

### 3.3 CI 的边界

`.github/workflows/ci.yml` 全部 job 都是 `runs-on: ubuntu-24.04`（:37、:74、:199）：

- **没有 Windows job**，也没有 macOS job；
- `tests/windows/` 只有 `test_windows_smoke.cpp` 一个文件，且不在主 ctest 配置里验证；
- 结论：**跨平台未进入 CI 门禁**，跨平台能力没有自动化证据。

---

## 四、应用验证能证明什么

### 4.1 `app/douyin`（单机 6 服务 + 网关）

**实测**（2026-11-04，本机，与 CI 并行执行，非独占）：

```
[accept_all] 结果：69 PASS / 0 FAIL        # 功能验收（scripts/accept_all.sh）
[accept_all] 结果：73 PASS / 0 FAIL        # 功能 + 压测（--bench，追加 4 项检查）
[loadgen] feed  QPS=10601.1 p50=2.946ms p99=4.230ms 非200=0 错误=0
[loadgen] video QPS=13667.7 p50=2.275ms p99=3.007ms 非200=0 错误=0
```

- 需求门槛是「≥2000 QPS、p99<50ms」（`app/douyin/REQUIREMENTS.md:272-273`），实测 **10.6k/13.7k QPS、p99 3–4ms**，功能与性能双达标；
- 覆盖注册/登录、发视频、feed、点赞幂等、评论、直播、搜索、鉴权 401/404、六条路由；
- 可靠性：`README.md:24` 记录 C10 实跑 **19 PASS / 0 FAIL**，kill -9 后数据不丢（`users=1`、`videos=2`）。

**不能证明什么**（`app/douyin/README.md:28-37` 自述）：

1. **SQLite 单写者**：六服务共享一个库，WAL 仍单写者，写压力大时 `SQLITE_BUSY`，「仅适合 demo，不是生产存储」；
2. **自写 HTTP 每连接一线程**：无 keep-alive，并发受线程/fd 限制，「压测数字不代表生产框架上限」；
3. **启动竞态**：同时拉起会偶发 `SQLITE_BUSY` 导致服务退出，需 0.4s 间隔；
4. 不做转码/推荐，token 无刷新/吊销。

> 关于任务背景里的「10/10 完成」：**全仓未找到该数字的书面出处**；可复核的口径是 69/73 PASS。本报告不引用无出处数字。

### 4.2 `app/bilibili`（弹幕服务 + Web demo）

- 有完整需求文档（`REQUIREMENTS.md`，FR-01..FR-05、数据模型、B1–B4 里程碑）；
- 有代码：`danmaku_service` + 4 个 include + Web demo（`web/index.html`、`demo.mp4`）；
- **但验收脚本缺失**：`REQUIREMENTS.md:197` 写明「脚本 `scripts/accept_bili.sh`」，实测 `app/bilibili/scripts/` 下**不存在该文件**（只有 demo/loadtest 脚本）；
- 没有 README，没有验收记录，没有 PASS 统计。

结论：bilibili 只能证明「WebSocket 房间广播 + 历史弹幕的代码路径存在」，**不能证明验收通过**。

### 4.3 应用验证的共同结论

两个 demo 合起来能证明：**Linux 单机上「网关 + 多服务 + SQLite + WebSocket」的端到端链路可以跑通，且性能远超 demo 门槛**。
不能证明：生产级数据层、跨平台、多机部署、可观测闭环。

---

## 五、已知短板（按「支撑真实应用」标准）

### 5.1 调度器：2.21× Go，已判定为架构代价

- 台账 M16：Go@4 `162.7 ns`，C++ 同轮 `359 ns`，比值 **2.21×**（:252）；最好组合 239/163 = **1.47×**（:89）。
- 台账 :99–101 明确「放弃 Scheduler 优化」：本项目在状态机/所有权/Context/栈增长/IO 模型 5 个维度比 Go 更复杂，**2.21× 是语义更重的代价，不是实现缺陷**。
- 工程后果：它不是「性能短板」而是「能力边界」——任何要求与 Go 同量级调度开销的场景，本项目先天吃亏。

### 5.2 无 GC：RAII + 引用计数，易错点集中在生命周期

- `README.md:357`：**无 GC**（RAII + 引用计数），「需显式管理生命周期」；
- `docs/compatibility.md:68`：固定保护栈而非 Go 自动增长栈，超容量属于可诊断失败；
- 具体易错点（`docs/compatibility.md:8-11`）：
  - Scheduler/IOManager 必须长于 worker，析构顺序错即悬空；
  - Fiber 不能析构自身；错误父级销毁会标记 Failed，但**挂起栈上的 RAII 无法补做**；
  - 持锁 park 是 Go 不存在的死锁形态，本项目会检测并报错（`README.md:169`）；
  - `Channel<T>` 只接受不抛 move/析构的值类型（`compatibility.md:60`）。

### 5.3 跨平台：仅 Linux/x86_64，且未进 CI

- `docs/modules/portability.md:3`：「**当前仅 Linux/x86_64；跨平台适配列入 backlog**」；
- 硬依赖：`context_x64.S`（汇编）、`epoll`、`futex`、`mmap/mprotect`、`sendmmsg/recvmmsg`、`sendfile`（`portability.md:10-18`）；
- CI 只有 `ubuntu-24.04`（`.github/workflows/ci.yml:37/74/199`）；`tests/windows/test_windows_smoke.cpp` 是唯一 Windows 测试，未纳入主构建；
- 结论：**Windows/macOS 不可用，连「能 configure」都未在 CI 验证**。

### 5.4 分布式设施：8 个组件，但模块孤岛 + 语义缺陷

依据 `docs/reports/distributed-analysis.md`（424 行，2026-11-04）：

> **时点校正**：以下缺口对应 HEAD `a885d8c`。11:30 `git status` 显示工作区已有**未提交**的 P0 实现（`node_id.hpp` 162 行、`trace.hpp` 486 行、`metrics.hpp` 126 行、`node_id.cpp` 140 行，CMake 已注册 2 个测试）。未 commit、未验收，故「已提交基线」上缺口仍成立；合并后需重估。

- 模块外**零引用**：`grep -rln 'go2cpp/distributed'` 除自身与测试外无命中（:46-48）；
- 能力矩阵 11 项：**有 2**（分布式锁、租约）、**弱 3**（服务发现、选主、观测）、**缺 6**（配置同步、消息、限流、追踪、认证/传输安全、持久化）（:56-66）；
- **fencing 重启回退是语义缺陷**：计数只在内存（`coordinator.hpp:256-258`），协调者重启即回退，破坏「永不复用」（:66、:90-91）；
- 无 watch、无通用 KV、无消息、无持久化；每句柄一条 Heartbeat 线程，1000 实例即 1000+ 线程（:99）；
- 补全 P0–P5 约 **16.5 人天**（:379-394）。

### 5.5 cgo：同算法 C 比 Go 慢 4–20%

依据 `docs/reports/cgo-overhead.md`（274 行）：

- 固定开销 **17–32 ns/次**；64 B 小调用里 17 ns 占 36%（:68）；
- **同样的标量循环，C 比 Go 慢 4%–20%**（:17、:145-146）——「用 C++ 重写」本身不产生加速；
- 只有 SIMD/算法级改写才有收益；建议「能用纯 Go 就别开 cgo」（:230）；
- 对 Go2Cpp 的含义：它作为「C++ 侧向量化内核载体」成立，作为「Go 代码直译提速」不成立。

### 5.6 应用框架 / 数据层 / 可观测 / 工具链

| 维度 | 已有 | 缺口（有证据） |
|---|---|---|
| 应用框架 | `app/stream_table.hpp`、`app/gateway.hpp`（注册式路由/中间件）、`realtime_room` | douyin 仍自写 HTTP（每连接一线程、无 keep-alive，`README:30-31`）与手写 JSON；**没有业务服务模板/脚手架** |
| 数据层 | SQLite（M06 1.074×）、LevelDB 兼容 KV（M07 1e8）、ORM/迁移/队列/watch | SQLite 单写者仅 demo（`douyin/README:28-29`）；缺连接池/分片/多租户证据；迁移工具只在 CI 单测（`.github/workflows/ci.yml:106-113`） |
| 可观测 | `metrics`（Prometheus 文本）、`logging`（脱敏/轮转）、`debug`（事件环） | 无 `trace_id`/`span_id`（`distributed-analysis.md:63`）；`CoordinatorStats` 未接 `metrics::Registry`（:64、:98）；无日志-指标-追踪关联 |
| 工具链 | `tools/`（CLI、replay、migration、bench、verify_metrics） | 无打包/部署/发布脚本；CI 只 Ubuntu（§5.3）；`verify_metrics.sh` 硬编码 `.build-bento-wsl`（`ci.sh:23-24`） |
| 稳定性 | 278 项 ctest、ASAN/TSAN 选项、1e9 压测历史 | 并行口径 7 项失败；串行全量在 `relay_hub_tsan` 卡死 >20 分钟；4 项 skip 成常态；无 GC 的生命周期错误靠 review；固定栈深递归失败 |

---

## 六、两张清单

### 6.1 现在就能用（有证据、有边界）

| # | 能力 | 证据 | 使用边界 |
|---|---|---|---|
| 1 | **Linux 单机 HTTP/WS/RPC 服务** | M05 120,755 rps；M14 TLS 1.061×；douyin 69 PASS、10.6k QPS | 仅 Linux；epoll/hook 依赖；应用层需自己拼装 |
| 2 | **并发原语（Fiber/Channel/Context/sync）** | M01b-b 16.07/6.23 ns；M02 19.84 ns；M04 119 ns；94 个测试文件 | 协作式、无抢占；固定栈 128 KiB；持锁 park 禁止 |
| 3 | **基础工程库（缓存/存储/压缩/编码/哈希/日志/配置）** | M06 1.074×、M07 647.5k ops/s、M09/M10/M11/M12 实测；均有专属测试 | SQLite 只适合单写者/读多写少；LevelDB 是兼容实现 |
| 4 | **Go 语义迁移层（`go/**`）** | `go-api-compat.md`、`tests/go_api` 13 文件、M01b-b/M04 | 是「同形 API」不是转译器；无 GC 语义差异需人工处理 |

> 任选 3 类即可支撑一个真实单机服务；第 4 类是差异化能力，但成熟度低于前三类。

### 6.2 做不了 / 要先补（按阻塞程度排序）

| # | 做不了的事 | 阻塞证据 | 先补什么 |
|---|---|---|---|
| 1 | **跨平台产品（Windows/macOS）** | `portability.md:3`；CI 无 Windows job（`ci.yml:37/74/199`）；汇编/epoll/futex 硬依赖 | IOCP/WaitOnAddress/VirtualAlloc 封装 + Windows CI job（估 1–2 月，`portability.md:82`） |
| 2 | **多机分布式协调** | `distributed-analysis.md`：零引用、无持久化、fencing 回退、缺 watch/消息/追踪；P0–P5 16.5 人天 | 先做 P0+P1（NodeId + 持久化 + fencing 修复），否则「静默破坏正确性」 |
| 3 | **生产级高写并发/多租户数据层** | SQLite 单写者（`douyin/README:28-29`）；无连接池/分片证据；bilibili 无验收脚本 | 连接池 + 写合并/分片 + 迁移演练；或明确「单写者」产品边界 |
| 4 | **CPU 密集长任务（需抢占）** | `README.md:355`：纯协作式，长计算饿死同 P；`compatibility.md:8` 无异步抢占 | 不能靠库补；必须业务侧显式 yield 或改用线程池 |

---

## 七、结论：现在能做的 3 类应用

1. **单机 API 网关 / 短视频后端（demo 级到小流量生产）**
   证据：douyin 实测 69 PASS、feed 10.6k QPS / p99 4.2ms、video 13.7k QPS / p99 3.0ms；M05 HTTP 120.8k rps。
   前提：接受 SQLite 单写者与「每连接一线程」的 demo HTTP；生产需替换数据层/接入层。

2. **实时长连接服务（WebSocket / 弹幕 / 推送 / 房间）**
   证据：bilibili 弹幕服务代码 + Web demo；bento M15 282k pps；network 31 个测试文件、M14 TLS 1.061×。
   前提：bilibili 验收脚本缺失，需先补 `accept_bili.sh` 才能说「验收通过」。

3. **基础设施组件库（缓存/存储/压缩/序列化/日志/配置）嵌入现有 C++ 服务**
   证据：cache M09、storage M06/M07、compress M11、hash M12、reflect M13 均有实测数字与专属测试；`tools/` 有 CLI/replay/migration 工具链。
   前提：仅 Linux；无 GC 的生命周期契约需团队培训。

## 八、结论：做不了 / 要先补的 3 类

1. **跨平台客户端/服务端产品**——`portability.md:3` 明说「仅 Linux/x86_64」，CI 无 Windows job，汇编上下文与 epoll/futex 是硬依赖。**不是补几个 `#ifdef` 能解决**，按文档估算 Windows 阶段 1–2 月。

2. **需要强一致的分布式系统（选主/分布式锁/配置中心）**——`distributed-analysis.md` 判定：模块孤岛、无持久化、fencing 重启回退、无 watch/消息/追踪；补全 P0–P5 约 16.5 人天，且「跨机房强一致」明确建议换 etcd。（注：工作区已有未提交 P0 实现，未验收。）

3. **与 Go 同量级调度开销的 CPU 密集服务**——M16 2.21×、M03 239 ns vs 122 历史；台账已收口为「语义更重的代价」，继续优化收益 <5%（:96-98）。要么接受 1.5–2.2× 的调度开销，要么不用这套运行时。

---

## 附：证据索引

| 结论 | 证据位置 |
|---|---|
| 278 项 ctest / 271 passed·7 failed·4 skipped | `ctest -N`；`ctest -j16 --timeout 120` 实测 |
| 达标 3/19 | `docs/metrics-ledger.md` 逐条状态字段 |
| 调度器 2.21× | `docs/metrics-ledger.md:252`、:99–101 |
| 无 GC/固定栈/协作式 | `README.md:355-358`、`docs/compatibility.md:8-11/68` |
| 分布式缺口 | `docs/reports/distributed-analysis.md:46-66/90-91` |
| cgo 慢 4–20% | `docs/reports/cgo-overhead.md:17/145-146` |
| douyin 69 PASS / QPS | 本报告 §4.1 实测输出；`app/douyin/README.md:24/28-37` |
| bilibili 无验收脚本 | `app/bilibili/REQUIREMENTS.md:197` + `ls app/bilibili/scripts/` |
| 「411k rps」无出处 | `docs/metrics-ledger.md:131-138` |

> 本报告只新增 `docs/reports/capability-gap.md`，未改代码、未 commit。
