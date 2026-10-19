# Go2Cpp 交接文档（面向新 AI Agent）

> 生成时间：2026-10-19 17:15（Asia/Shanghai）
> 工作树：`/UserData/CodexWorkSpace/Go2Cpp-work` ｜ 分支 `feat/isolated` ｜ HEAD `ab03bea`
> 主目录：`/UserData/CodexWorkSpace/Go2Cpp` ｜ 分支 `feature/gmp-debug-module` ｜ HEAD `77b405f`
> 目标：让接手的 Agent 在 10 分钟内完成开工准备，并知道第一周该做什么。
> 读法：结论后括号里是出处；**没写出处的数字就是未验证**，不要对外引用。

**30 秒速览**

- 项目：C++17 的 Go 运行时语义子集（GMP + Fiber + channel + epoll + hook），不是转译器（`README.md:3-5`）。
- 当前分支：工作树 `feat/isolated`，115 个 ctest 全绿（本文实测 63.86s）；主目录分支停在 `77b405f`，**落后 11 个提交**。
- 最大未解问题：计算 + IO 混合场景的 **sys 高**（s3 为 epoll 的 11.8 倍，§5/§6）。
- 工作树当前**不干净**：4 个文件、约 +75/−2 行未提交改动，是**两个**针对 s3 sys 的候选修复（§6.2-A1/A2）。
- 第一件事不是写代码，而是跑 §8 的「10 分钟开工检查单」。

---

## 1. 项目是什么

**三句话**

1. Go2Cpp 是一个 C++17 实现的 **Go 运行时语义子集**：GMP 调度器、Fiber、channel/context/sync、epoll IOManager、syscall hook、网络协议栈；它是运行时库，不是 Go 解析器，也不是 Go→C++ 源码转译器（`README.md:3-5`）。
2. 它解决的是「C++ 网络服务想要 Go 的 `go func()`、同步阻塞式 `read()`、取消树和 channel，但不想引入 Go runtime」这个问题（`README.md:3-5`、`codex.md:34-40`）。
3. 当前成熟度：**功能覆盖 L0~L5、115 个 ctest 全绿**，但**性能尚未追平 thread+epoll**——s3 的 sys 是对方的 11.8 倍、s4 的 QPS 低 12%（§5），并且所有改动都留在 `feat/isolated`，**未合入主目录分支**。

**分层（照 `codex.md:29-38` 抄，找代码时按这张表定位）**

| 层 | 内容 |
|---|---|
| L5 应用协议 | `http` `websocket` `sse` `http2` `quic`（picoquic 薄封装）`http3`（可选） |
| L4 网络基础 | `socket` `address` `streams` `bytearray`；`Transport` 抽象 |
| L3 IO 后端 | `io`（IOManager + epoll）`io_backend` `fd_manager` `hook` |
| L2 并发原语 | `channel` `sync` `future` `parking_condition` `context` `error` |
| L1 GMP 调度 | `scheduler`（Task=G / Scheduler / P / M / worker_loop / sysmon）、`fiber` |
| L0 地基 | Fiber/swapcontext、FiberBin、ParkingCondition、Timer |

**与 Go 的根本差异（所有设计约束的源头，`codex.md:42-48`）**

- 调度是**纯协作式**：只有显式 park/yield 点，长计算会饿死同 P 的 G。
- 栈是**固定 128 KiB**（`fiber_stack_size`），深递归会失败。
- 锁**支持跨 goroutine 解锁**（无 owner），因此不能退回 `std::mutex`。

**最近 48 小时落地了什么（读代码前先知道）**

| 内容 | 证据 |
|---|---|
| SO 热重载配置层：`SoManifest` 静态校验 + `SoReloader` 两阶段事务 + F2 三指标 + `ForceRescan` | `acca90d`、`58b31d4`、`docs/so-reload-progress.md:48-153` |
| `Channel::Select` 增加 `DoneCase` / `AfterCase`，并补 Fiber 场景 5 个用例 | `1bebf73`、`54effd4`、`codex.md:1096-1176` |
| 函数级注释：24 个模块中 21 个完成，scheduler/ParkingCondition 各 3/3 | `docs/function-annotation-progress.md:9-60` |
| 注释与文档收尾、模块表重写 | `77b405f`、`094a2ee`、`08c9b60` |

**不要被旧文档的计数骗了**

- `docs/NEXT-STEPS.md:3-4` 写的是 2026-10-18、HEAD `37a2196`、**113/113**——它是历史清单，不是现状。
- `docs/so-reload-progress.md:68-78` 写 114/114；`docs/function-annotation-progress.md:5` 写 113/113。
- **当前以 `ctest --test-dir build-work -N` 和实跑结果为准（115）**；改测试后必须同步所有出现计数的地方（`codex.md:46`）。

**长期方向在哪**

- `docs/direction_plan.md:58-75` 给出四层路线：地基（A）→ 并发状态机框架（D）→ 确定性/可验证标签（C-L1/L2）→ 具体服务作品（G）。
- 据 `git log`，2026-10-18~19 的提交集中在性能归因、注释、SO 热重载，**没有阶段 2/3 的代码**；接手时不要把路线图当成已完成状态。

---

## 2. 双目录结构（最重要，新人最容易踩）

| 目录 | 分支 | 用途 | 能否改 |
|---|---|---|---|
| `/UserData/CodexWorkSpace/Go2Cpp` | `feature/gmp-debug-module` | **用户主目录**，用户在看 | **禁止修改** |
| `/UserData/CodexWorkSpace/Go2Cpp-work` | `feat/isolated` | Agent 工作树 | 可以改 |

**这是 `git worktree`，不是两份拷贝**

- `git worktree list` 实测输出：
  - `/UserData/CodexWorkSpace/Go2Cpp` → `77b405f [feature/gmp-debug-module]`
  - `/UserData/CodexWorkSpace/Go2Cpp-work` → `ab03bea [feat/isolated]`
- 两目录**同一仓库、不同分支/工作区**，共享对象库与 refs；在一个目录 `git commit`，另一个目录能立刻看到对象，但**看不到对方工作区的文件**。
- 主目录 HEAD 落后工作树 **11 个提交**（`git rev-list --count 77b405f..ab03bea`）。**不要以为主目录有最新代码。**
- 主目录 `git status --porcelain` 必须始终为 0 行；本文开始与结束时都实测为 0。

**你必须遵守**

- 动手前先 `pwd`，确认在 `/UserData/CodexWorkSpace/Go2Cpp-work` 下再改文件。
- **不要**在主目录里 build、commit、stash、checkout、改配置、删文件。用户可能正在看它。
- **不要**在子 agent 运行期间执行 `git add -A`：历史事故 `58b31d4` 把子 agent 在途的 `channel.hpp` / `test_channel.cpp` 一起提交了（`docs/so-reload-progress.md:154-184`）。
- 本文档由父 agent 统一提交；接手后**不要自作主张提交**，除非任务明确要求。
- Windows 侧的 `E:\CodexWorkspace\Go2Cpp` 是**另一个独立仓库副本**（各有 `.git`），不是这个 worktree；不要拿它替代 WSL 树，否则会得出「符号不存在」这类基于旧副本的错误结论（`codex.md:647-665`）。

**一分钟核对**

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work && pwd && git branch --show-current && git status --short
cd /UserData/CodexWorkSpace/Go2Cpp && git status --porcelain   # 必须无输出
git worktree list                                              # 两行，分支不同
```

---

## 3. 硬性环境约束

1. **禁止写 PowerShell 逻辑。** 终端入口统一 `wsl bash -lc "..."`；本环境的 `pwsh` 只是执行器，命令体必须是 bash，不要用 cmdlet、`$env:`、PowerShell 管道。
2. **Windows 侧杂务用 Python 临时脚本。** 写到 `.dsh_tmp/`，用 `python` 执行，**用完删除**；不要留脚本（本项目已有 `.dsh_tmp/_bench_work.py` 这一例外，它是当前压测驱动，见 §4）。
3. **`/UserData/...` 不能用 `read` / `write` / `edit` 工具**——会被解析成 `E:\UserData\...`。WSL 文件只能通过 `wsl bash` 操作（`sed -i`、`cp`、heredoc），Windows 文件才用编辑工具。
4. **编译必须 `flock`。** 多 agent 共享 `build-work`，不加锁会互踩：
   `flock /tmp/go2cpp_build.lock <命令>`。`cmake --build`、benchmark 构建、`ctest` 都建议加。
5. **压测必须 `timeout`。** 任何压测、benchmark、可能挂住的命令都套 `timeout`（例如 `timeout 900 ...`）。
6. **loadavg 高时数据不可用。** 每点测前 `cat /proc/loadavg`；`loadavg` 明显高于基线（>2）时不要测。曾有一次 `loadavg 10.09` 的数据把 QPS 与 sys 全带偏（`docs/parameter-tuning.md:189-196`）。同一组 A/B 必须在**同一窗口**连续跑，多 rep 取中位数（`codex.md:774-796`）。
7. **`$` 会被外层吞掉。** `wsl bash -lc "echo $HOME"` 实测得到 `C:Usersm`；要传多行脚本用 Python `subprocess.run(["wsl","bash","-s"], input=script, ...)`（`docs/NEXT-STEPS.md:196-205`）。
8. **git 网络操作（clone/fetch/pull/push）一律在 Windows 侧。** WSL 内 `github.com` 解析成 `::1`，`push` 必失败；WSL 只做本地 `add/commit/log/status`（`codex.md:659`）。
9. **`cmake --build <dir> --target <test>` 不会重编它链接的库。** 改了 `src/network/*.cpp` 后要显式重建对应库，否则手工跑的是旧 `.so`；改完 CMake 必须单独 configure 并确认 `CMake Error` 计数为 0（`codex.md:621-631`）。
10. **仓库行尾不统一**（CMake 文件 CRLF、C++ 源码 LF）。批量改文件按 bytes 读写，否则整文件 diff（`codex.md:666-680`）。

**当前工作区可能被并行使用。** 动工前看 `ps -eo pid,cmd | grep -E 'cmake|ctest|cc1plus'` 与 `git status`；本文生成时 `build-work` 的产物在 17:05 被重编过（含未提交候选修复），别假设你看到的是上一分钟的状态。

---

## 4. 构建与测试（可直接复制）

**工作树构建（`build-work` 已存在，Release / TESTS=ON / BENCH=OFF，实测 cache 一致）**

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work
cmake -B build-work -DCMAKE_BUILD_TYPE=Release -DGO2CPP_BUILD_TESTS=ON
flock /tmp/go2cpp_build.lock timeout 900 cmake --build build-work -j 8
```

**全量测试（当前 115/115）**

```bash
flock /tmp/go2cpp_build.lock timeout 900 ctest --test-dir build-work
```

本文实测结果：`100% tests passed, 0 tests failed out of 115`，耗时 **63.86s**（起点 `loadavg` 0.94，结束时升到 2.25）。**注意**：这次 `ctest` 期间构建产物被并行进程重编过，严格基线应在你自己的干净窗口重跑一次。

**单个测试**

```bash
ctest --test-dir build-work -R so_manifest --output-on-failure
ctest --test-dir build-work -R 'go2cpp_tests$' --output-on-failure
```

**先数用例，别背数字**

```bash
ctest --test-dir build-work -N | tail -3          # Total Tests: 115
```

**115 是怎么来的**：113 个旧基线 + `go2cpp_so_manifest_test`（`acca90d`）+ `go2cpp_fiber_select_test`（`54effd4`，`cmake/Go2CppTests.cmake:436-443`）。

**构建压测程序（独立于 CMake，链接 `build-work/*.so`）**

```bash
flock /tmp/go2cpp_build.lock timeout 900 bash benchmarks/build_gmp_vs_thread_epoll.sh build-work
# 产物：build-gmp-vs-thread/gmp_vs_thread_epoll_benchmark
# 注意：该脚本只重编 benchmark 本体，不重编库；库要用上面的 cmake --build 更新。
```

**跑一次 s3（约 1 分钟/模型）**

```bash
cat /proc/loadavg    # >2 就别测

# GMP P=24
timeout 600 python3 .dsh_tmp/_bench_work.py --scenario s3 \
  --conns 8000 --duration-ms 5000 --model gmp --server-args "--pin --workers 24"

# epoll 256 线程
timeout 600 python3 .dsh_tmp/_bench_work.py --scenario s3 \
  --conns 8000 --duration-ms 5000 --model epoll --server-args "--workers 256"
```

**两个必知陷阱**

- 服务端 `--workers` **默认 8**，会覆盖源码里的 `processor_count` 默认值；要测「源码默认」必须传 `--workers 0`（`docs/parameter-tuning.md:132-146`、`benchmarks/gmp_vs_thread_epoll_benchmark.cpp:1799`）。
- `run` 编排器里的 epoll 模型**写死 `--workers 8`**（`benchmarks/gmp_vs_thread_epoll_benchmark.cpp:1883`），所以 **256w 不能靠 `run` 子命令复现**，必须用上面的手动驱动或自己起 server/client。
- `.dsh_tmp/_bench_work.py` 被 `.gitignore` 忽略、未入库。若已被清理，改用手动 server/client：`server --model gmp --port 0 --workers 24 --pin`，从 `READY ... port=N` 取端口，再 `client --scenario s3 --port N --conns 8000 --duration-ms 5000`（`benchmarks/gmp_vs_thread_epoll_benchmark.cpp:1781-1787`）。

---

## 5. 性能基线（2026-10-19 实测，计算注入）

| 场景 | 模型 | QPS | P50 | µs/请求 | user | **sys** | CPU% |
|---|---|---:|---:|---:|---:|---:|---:|
| s1 短连接 | GMP P=24 | 37,372 | 1.26ms | 211.4 | 93.4 | 118.0 | 790% |
| | epoll 256w | 23,421 | 2.53ms | 123.0 | 76.9 | 46.1 | 288% |
| **s3 8000 长连接** | **GMP P=24** | 7,935 | 0.32ms | **911.2** | 197.4 | **713.8** | **723%** |
| | epoll 256w | 7,952 | 0.39ms | 138.2 | 77.7 | 60.5 | **110%** |
| s4 混合 | GMP P=24 | 33,784 | 1.81ms | 227.4 | 79.9 | 147.5 | 768% |
| | epoll 256w | 38,374 | 1.43ms | 100.7 | 70.1 | 30.6 | 387% |

**口径**：计算注入 = `GO2CPP_BENCH_COMPUTE_ITEMS=4096`，随机 0~4096 个 int 排序（`docs/parameter-tuning.md:3-5`）；宿主 12 核；Release。

**核心结论（必须记住）**

> **s3 场景 QPS 相同（7,935 vs 7,952），但 GMP 的 sys 是 11.8 倍、CPU 是 6.6 倍。**

**表是自洽的，可以交叉验算**：`µs/请求 ≈ user + sys`（s3 GMP：197.4+713.8=911.2）；`CPU% ≈ µs/请求 × QPS / 1e4`（s3 GMP：911.2×7935÷10000≈723%）。这说明三列来自同一次采样，不是拼出来的。

**其他读法**

- s1：GMP 的 QPS 高 60%，但 sys 是 2.6 倍、CPU 是 2.7 倍，并且有约 3% 错误率（§7）——**吞吐赢了，效率没赢**。
- s4：GMP 的 QPS 低 12%，sys 是 4.8 倍；这正是 `docs/coost-comparison.md:478-509` 归因的「计算 + IO 跨 M 协调」。
- **不要跨表横比**：`docs/gmp-optimization-log.md` 里 s3 曾录到 9.8k（不同窗口/客户端数），与本表 7.9k 不可比（`codex.md:682-690`）。

**出处说明（不要跳过）**

- 这张表在 `docs/` 里 **grep 不到**（`37,372`、`911.2`、`713.8` 均无命中）。仓库内唯一能产出这组列（`user` / `sys` / `cpu_us_per_req`）的驱动是 `.dsh_tmp/_bench_work.py`，其配套二进制 `build-gmp-vs-thread/` 的构建时间是 16:54（早于本文生成）——**据此判断**它是该脚本的 2026-10-19 实测，属「未落盘数据」，不是从现有文档抄来的。**这是推断，不是逐条复跑确认。**
- 因此接手的第一个文档任务：**把这张表补进 `docs/performance.md` 或 `docs/gmp-perf-analysis.md` 并标注命令/loadavg**，否则下一轮又只能靠口头交接。

---

## 6. 当前最大的未解问题：sys 高

### 6.1 已知的 strace 数据（s3，2000 连接，5000ms）

| syscall | GMP 每请求 | epoll 每请求 | 倍数 |
|---|---:|---:|---:|
| `epoll_ctl` | **10.3** | 1.8 | **5.7×** |
| `futex` | **9.3** | 3.5 | **2.7×** |
| `epoll_wait` | 2.1 | 0.003 | 700× |
| `read` | 1.8 | 0.004 | 450× |

**关键线索**：`src/network/io.cpp:565-578` 的注释说「保留 slot 后 epoll_ctl 降为命中缓存 + MOD」，**但实测仍是 10.3 次/请求**——说明那次优化没完全生效，或 s3 的 MOD 本身被放大。

### 6.2 四个可疑点：两个候选修复 + 两个已确证机制（按证据强度排序）

**A1. 候选根因一：Hook 把 `accept4(SOCK_NONBLOCK)` 误判成「用户要非阻塞」，导致读写不 park、退化成 100ms 分片轮询**

- `accept4` 的 hook 分支把 `SOCK_NONBLOCK` 当成用户意图：`hook.cpp:2175` 传 `(flags & SOCK_NONBLOCK) != 0`，`register_new_socket` 里 `m_user_nonblocking = user_nonblocking || originally_nonblocking`（`hook.cpp:695`）。
- `can_wait()` 要求 `!m_user_nonblocking`（`hook.cpp:930-934`），于是被 accept 出来的连接**不能 park**。
- `Socket::wait_ready` 在拿不到 park 时，以 **100ms 为片**循环 `IOManager::wait_for`（`src/network/socket.cpp:441-460`）；每片至少一次注册/注销。s3 的连接 99.99% 时间在等下一次 PING，1 秒空闲 ≈ 10 片，**与 10.3 次/请求同一量级**（精确构成仍需 strace 分类，见 §6.3）。
- 工作树**未提交**的 diff 正是在修这个：新增 `hook::mark_system_nonblocking`（`include/go2cpp/network/hook.hpp:60-81`、`src/network/hook.cpp:1465-1500`），在 `Socket::accept` 里 `accept4` 返回后、`FromFd` 之前调用（`src/network/socket.cpp:280-291`，工作树行号）。注释里直接写了「s3 实测每请求 10.3 次 epoll_ctl、9.3 次 futex」。
- **状态：已编译、未压测。** `build-work/libgo2cpp_hook.so.0` 在 17:05 重编并含 `go2cpp::hook::mark_system_nonblocking` 符号；但性能效果**未验证**。不要假设它一定有效，也不要直接把它当成已完成的优化。

**A2. 候选根因二：`register_wait` 无条件 `tickle()`，把空闲 worker 全部踢醒**

- `register_wait` 收尾处原本无条件 `tickle()`（`src/network/io.cpp:999` 附近，工作树 diff 前），每次新等待都写一次 eventfd，唤醒所有正在 `epoll_wait` 的 worker。
- s3 的 8000 个空闲长连接每秒各注册一次 read 等待 ⇒ 每秒上万次 eventfd 写，每个 worker 都被叫醒再睡回去；这**很可能**是 strace 表里 `epoll_wait 2.1`（700×）与 `read 1.8`（450×，含 eventfd read）的来源（该 diff 注释记录 eventfd read 10.9 次/请求，口径与 strace 表不同，需重测确认）。
- 工作树未提交 diff（`src/network/io.cpp:999-1007`）把 `tickle()` 包进 `if (deadline.has_value())`：只有新增定时器才需要打断 `epoll_wait`（旧的最近截止时间可能更晚）；无 deadline 的 fd 注册交给内核，就绪事件会正常唤醒 poller。注释记录「每请求 0.93 次 eventfd 写被放大成 10.9 次 epoll_wait 返回和 10.9 次 eventfd read」。
- **状态：未提交、未压测。** 它和 A1 同在 `build-work` 里编译过（`io.cpp` 在 17:12 又被改了一次）；两个改动必须**分开 A/B**，否则分不清收益归谁。

**A1/A2 是本工作树里的两个候选修复，B/C 是历史实测确认的机制。** 改代码前先 `git diff` 确认你测的是哪一个组合。

**B. 已确证机制一：`EPOLLONESHOT` 强制 rearm（设计层面的必然成本，但可能被放大）**

- epoll 后端启用 `EPOLLONESHOT`（`src/network/io_backend.cpp:89`）。
- 事件被消费后 `slot.armed = false`（`src/network/io.cpp:1454-1457`）；下一次等待进 `update_interest_locked`（`io.cpp:1286`）时，快路径要求 `(!one_shot || slot.armed)`（`src/network/io.cpp:1327-1330`），armed 已 false ⇒ 必须发一次 `EPOLL_CTL_MOD` 重新布防。
- 也就是说：**每消费一次事件，下一次等待就多一次 MOD**。这解释了「为什么 interest 没变还要 MOD」，但解释不了 10.3 这么高——需要先知道一次请求到底消费了几次事件、arm 了几次。
- `update_interest_locked` 有 6 个调用点（工作树行号，会随未提交 diff 漂移，按函数名找）：`io.cpp:980`（register_wait 收尾）、`1032`（取消）、`1127`（超时补采样）、`1496`（事件处理）、`1623`（取消/停止）、`1954`（finish_fd 批量）。**先数调用频率，再谈优化。**

**C. 已确证机制二：`ADD/DEL` 而不是 `MOD`（历史实测，可能与直觉相反）**

- s2 的原始 strace 显示 **`ADD 1.49` + `DEL 1.49`、`MOD 0`**（`docs/gmp-optimization-log.md:1157-1197`）。
- 轮次 26 的结论：DEL 全部发生在「readers/writers 都为空」时，是**取消注册**，保留监听会导致无等待者时忙循环；改成 `MOD events=0` 次数不变且 WSL2 上同价（`docs/gmp-optimization-log.md:1247-1290`）。
- 所以：**先把 s3 的 10.3 分类成 ADD/MOD/DEL**，再决定改哪里。直接照 `io.cpp:575` 的「降为 MOD」注释去优化，可能改错对象。

### 6.3 下一步（按顺序做，每步带验收）

1. **同窗口复跑 s3 + strace 分类。** 命令：
   `strace -f -c -e trace=epoll_ctl,futex,epoll_wait,read`，再对 `epoll_ctl` 展开操作类型（ADD/MOD/DEL）。
   验收：给出三类操作的「次/请求」，解释 10.3 的构成。
2. **统计 6 个 `update_interest_locked` 调用点。** 加临时计数器（或用 `gdb` 断点），记录每请求进入次数、`interest`、`registered`、`armed`。
   验收：每请求调用次数 × 每次操作数 = 10.3；并判断 A/B/C 哪个是主因。
3. **分别验证两个候选修复（一次只开一个变量）。**
   - A1（nonblocking）：确认 `hook.hpp`/`hook.cpp`/`socket.cpp` 三个文件是你要测的版本；
   - A2（tickle）：确认 `io.cpp:999-1007` 的条件版在；
   - 先单独测 A1、再单独测 A2、最后测叠加（同一窗口，各 2~3 rep，取中位数）。
   验收：A1 使 `epoll_ctl` 10.3→≤2/请求、`futex` 9.3 明显下降；A2 使 `epoll_wait`/`read` 从 2.1/1.8 降到接近 epoll 的量级；叠加后 QPS 不低于 7,935、`ctest` 115/115。
4. **保留或回退。** 若只改善 s3 而拖累 s2/s4/s1，按项目惯例回退并记录（`codex.md:774-796` 第 8 条）。
   若保留：把 s3 的 strace 前后对比写进 `docs/gmp-optimization-log.md` 和 `docs/performance.md`。

**不要重复的尝试（均已回滚，别再花时间）**

| 尝试 | 结论 | 出处 |
|---|---|---|
| `interest==0` 不 DEL、保留注册 | s2 +5.9%，但 s4 开注入恶化，已回退 | `docs/gmp-optimization-log.md:1402-1446` |
| 单 iovec 改 `::recv`/`::send` | 关注入微涨，开注入变差，已回退 | `docs/gmp-optimization-log.md:1486-1531` |
| pending 快速路径 | 净收益为零 | `docs/gmp-optimization-log.md:1119-1155` |
| generation 校验 / RST 关闭 | 都未降低 s1 错误 | `docs/gmp-optimization-log.md:1002-1045` |

**为什么先攻 s3 而不是 s4**：s3 的 QPS 已与 epoll 打平，差距**纯在 sys**，变量最少；s4 的 sys 另有「跨 M 协调」成分（`docs/coost-comparison.md:478-516`），把 s3 的修复当成 s4 的答案会误判。

---

## 7. 其他已知问题

| 问题 | 状态 |
|---|---|
| s1 短连接 **3% 错误率**（6,802/224,224） | 未定位，疑为多 P 竞争 accept / 共享 worker 收尾；3.03% 与 `docs/parameter-tuning.md:117` 的 3.1% 同量级；`docs/gmp-perf-analysis.md:445-453` 的 0.68% 是 4 客户端另一口径，**不要混用** |
| `test_io.cpp:736` flaky | **已修**（`ab03bea`，20/20 通过）；根因是测试自身竞态（`WaitManyFor` 是「任一完成即返回」，不是「等全部」）；断言现在在 `tests/network/test_io.cpp:741-742` |
| Fiber 场景 `DoneCase`/`AfterCase` | **已补**（`54effd4`，5 个用例 F1~F5，`tests/concurrency/test_fiber_select.cpp`；ctest 新增第 115 项） |
| 两个既有 flaky（`go2cpp_tests` 偶发） | **未完全根除**；`codex.md:754-772` 记录 Fiber cancel+cleanup 丢唤醒根因未定位、`test_sync_cancel_stress.cpp` 尚未注册 ctest；`docs/NEXT-STEPS.md:146-156` 记录 `go2cpp_fiber_stack_limit_full` 在 ctest 下偶发（单跑 3/3 过） |

**补充（会影响你判断的）**

- **`hook.enable_shutdown` 是无效配置**：`shutdown` 从未被 hook，开了也没用；项目决定保持现状（`docs/NEXT-STEPS.md:99-110`）。
- **SO 热重载未合并、也未做真实 `dlopen`/ABI 符号校验**：本层只做 manifest 校验 + 两阶段事务，loader 由调用方注入；F1 灰度、F3 跨进程、fuzz/压测未做（`docs/so-reload-progress.md:79-93`、`:144-153`）。
- **函数注释剩余可选缺口**：`io::register_wait`、`hook::cooperative_connect`、`channel::RecvUntil`（`docs/function-annotation-progress.md:37-41`）。注释规范见 `docs/annotation-style.md`。
- **工作树有未提交改动**（截至本文 17:13：4 个文件、+75/−2 行——`include/go2cpp/network/hook.hpp`、`src/network/hook.cpp`、`src/network/io.cpp`、`src/network/socket.cpp`），是两个候选修复（§6.2-A1/A2）。它们**可能正在被父 agent 继续修改**：动它之前先 `git status`/`git diff`，不要 `git checkout --` 一把清掉，也不要在未确认时 `git add -A`。

---

## 8. 建议的接手顺序（按性价比排序）

### 8.1 10 分钟开工检查单（先做这个，不写代码）

```bash
cd /UserData/CodexWorkSpace/Go2Cpp-work && pwd && git branch --show-current
git status --short                 # 应有 4 个 M；若已提交/新增，以实际为准
cd /UserData/CodexWorkSpace/Go2Cpp && git status --porcelain   # 必须无输出
cat /proc/loadavg                  # >2 先别测
ctest --test-dir build-work -N | tail -3                       # 115
```

逐项确认：

- [ ] 当前目录是 `Go2Cpp-work`，分支 `feat/isolated`。
- [ ] 主目录干净；两个目录是同一 worktree、不同分支（§2）。
- [ ] 知道当前 HEAD 与未提交改动是什么（`git log -1`、`git diff --stat`）。
- [ ] 知道测试基线是 115，不是 113/114。
- [ ] 知道「不要 PowerShell、不要碰主目录、编译要 flock、压测要 timeout」（§3）。

### 8.2 然后按这四步走

**第 1 步：读 `docs/coost-comparison.md`（约 30 分钟）**

- 重点读第 2 轮（`110-136`）、第 3 轮（`219-286`）、第 7 轮（`456-516`）和文末的参数结论（`518-534`）。
- 验收：能复述三个根因（P 被写死、`usleep` 被 hook 成协作睡眠、futex park），以及**哪些旧结论已废弃**（`usleep` 口径的数字不要引用）。
- 读完后你就知道：GMP 不是「全面落后」，而是**分场景**——纯 IO 和批量就绪更强，计算+IO 混合更弱。

**第 2 步：跑一次 s3 压测（约 5 分钟）**

- 命令见 §4；先看 `loadavg`，再跑 GMP P=24 与 epoll 256w 各一次。
- 验收：亲眼看一次 `[server] cpu=... user=... sys=...`，建立对「QPS 相同、CPU 6.6 倍」的直觉。
- 记录：QPS、P50、user/sys、loadavg，写进你的工作日志（`codex.md` 新章节）。

**第 3 步：查 `epoll_ctl` 10.3 次/请求（最有可能拿下的优化）**

- 按 §6.3 的 1→4 做；**先分类 ADD/MOD/DEL，再改代码**。
- 优先验证工作树里的两个候选修复（§6.2-A1/A2）：它们是目前证据最强、改动最小（合计 +75/−2 行）的路径。**A1 已在 17:05 编译过；A2 的 `io.cpp` 到 17:12 才改，必须重编后再测。**
- 验收：给出「10.3 的构成」和「A/B 前后对比」；没有这两个数字就不算完成。

**第 4 步：如果不做优化，至少把 `docs/` 与代码对齐**

已知的四处不一致（按优先级）：

1. `src/network/io.cpp:575` 的注释说「降为命中缓存 + MOD」，但历史 strace 是 ADD/DEL、0 MOD（`docs/gmp-optimization-log.md:1157-1197`）——改成「命中缓存 + 一次注册操作」或补充实测分类。
2. `docs/parameter-tuning.md:100` 的标题写「默认值已改（核数 → 2×核数）」，但轮次 6 已回退；当前源码默认是**核数**（`src/concurrency/scheduler.cpp:1211-1219`）。加一句「本轮结论已被轮次 6 覆盖」。
3. `docs/NEXT-STEPS.md:3-4` 的日期/HEAD/113 计数全部过期；要么更新，要么在开头加「历史文档，现状见 AGENT-HANDOFF.md」。
4. `docs/function-annotation-progress.md:5`、`docs/so-reload-progress.md:68-78` 的 113/114 计数过期——统一改为指向 `ctest -N`，不要写死数字。

### 8.3 不要做的事

- 不要修改 `/UserData/CodexWorkSpace/Go2Cpp`（用户主目录）。
- 不要在 `loadavg > 2` 时采信压测数据，也不要用不同窗口的数字做 A/B。
- 不要重新尝试 §6.3 表里已回滚的优化。
- 不要在子 agent 运行期间 `git add -A`。
- 不要把 `docs/NEXT-STEPS.md` 的 113/113 当成当前基线。
- 不要用 PowerShell 写逻辑；不要把 `/UserData/...` 传给 `read`/`write` 工具。

### 8.4 交接完成的判定标准

- [ ] 能在 `Go2Cpp-work` 独立完成一次「configure → build → ctest 115/115」。
- [ ] 能在同一窗口复现 s3 的 QPS 与 user/sys（允许 ±10% 波动）。
- [ ] 能说清 `epoll_ctl` 10.3 的构成，并对候选修复给出「保留/回退」的结论。
- [ ] 把 2026-10-19 的性能表落盘到 `docs/performance.md`（或 `docs/gmp-perf-analysis.md`）。
- [ ] 在 `codex.md` 末尾追加一节，记录你这一轮改了什么、测了什么、结论是什么。

---

**最后一句**：这个项目最贵的不是代码，是**测量口径**。同一段代码在不同 `loadavg`、不同注入方式、不同客户端数下能差出 2 倍。先问「这个数字是在什么条件下测的」，再问「怎么优化」。
