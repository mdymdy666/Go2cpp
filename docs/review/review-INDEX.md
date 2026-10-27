# Go2Cpp 五模块审查汇总（2026-10-27）

> 基线：`a2fa0df`。审查范围按用户最终澄清收敛为 **logging / config / fiber / hook / scheduler** 五个模块。
> **bento 传输协议、docs/modules 20 篇文档、channel/sync/io/os 不在本次范围内**，未产出对应报告；此前已完成的相关分析未写入本目录。
> 审查开始时工作区的 19 个 Bento 文件改动经 `git diff --summary` 核实仅为 `100644 → 100755` 模式变化；**审查期间工作区被并发修改**（`cmake/Go2CppBento.cmake`、`src/bento/net/link.cpp` 及新增 `peer_hub`/`tcp_proxy` 等文件）。已用 `md5sum` 核对：本报告涉及的五个模块源文件与审查快照完全一致，行号仍对应 `a2fa0df`；Bento 改动不在本次范围。

## 1. 总览

| 模块 | 报告 | P0 | P1 | P2 | P3 | 合计 |
|---|---|---:|---:|---:|---:|---:|
| scheduler | [review-scheduler.md](review-scheduler.md) | 2 | 4 | 6 | 1 | 13 |
| fiber | [review-fiber.md](review-fiber.md) | 0 | 6 | 7 | 1 | 14 |
| hook | [review-hook.md](review-hook.md) | 3 | 4 | 5 | 0 | 12 |
| config | [review-config.md](review-config.md) | 1 | 6 | 5 | 0 | 12 |
| logging | [review-logging.md](review-logging.md) | 1 | 6 | 5 | 0 | 12 |
| **合计** | — | **7** | **26** | **28** | **2** | **63** |

严重度口径：P0 = 必然崩溃/死锁/安全或核心契约破坏；P1 = 数据丢失、静默失效、明确 UB 风险、可导致 shutdown 挂死；P2 = 边界/性能/可维护性；P3 = 一致性/可观测性小问题。

## 2. P0 清单（7 条，建议优先排期）

| ID | 模块 | 位置 | 一句话 | 触发条件 |
|---|---|---|---|---|
| SCHED-01 | scheduler | `scheduler.cpp:3981,4121-4131,2621` | 慢路径持 admission mutex 调 `recover_unclaimed()→enqueue()`，同线程重复锁同一非递归 mutex | 队列节点被取出但 claim 失败，且 G 回到 Runnable 未 requeue 的窗口 |
| SCHED-02 | scheduler | `scheduler.cpp:3877-3919,1781,2113-2120` | incoming 锁内补发布 → 锁 admission，与 shutdown 的 admission→incoming 反向锁序 | incoming 中路径 claim 失败 + 并发 shutdown/drain |
| HOOK-01 | hook | `hook.cpp:956-970,1009,1153-1159` | `SO_RCVTIMEO/SO_SNDTIMEO` 超时被映射为 `ETIMEDOUT` 而非 `EAGAIN` | 任何启用 socket 超时的 hook 宿主，依赖 EAGAIN 的库会误判断链 |
| HOOK-02 | hook | `hook.cpp:2517-2529` | 白名单外 `ioctl` 一律 `ENOTSUP`，宿主终端/网卡/文件系统 ioctl 全线失败 | LD_PRELOAD 宿主调用 `TIOCGWINSZ/TCGETS/SIOCGIFADDR/FS_IOC_*` 等 |
| HOOK-03 | hook | `hook.cpp:1138-1140,1217-1224` | EINTR 静默无上限重试；connect 的 EINTR 语义被破坏 | 信号风暴/带超时的阻塞调用/可取消 connect |
| CONF-01 | config | `config.cpp:657` | 事务锁在 `NotifyPending` 之前释放，并发 `LoadFromIni` 破坏「整批一次发布」 | 热加载线程与手动加载并发，且其中一个事务随后失败回滚 |
| LOG-01 | logging | `log.cpp:611-612,581-583,895-898` | Sink/回调内再打日志无重入防护，沿同一 worker 无限递归至栈溢出 | 自定义 Sink/CallbackSink 中调用 `GO2CPP_LOG_*` |

## 3. 模块要点

### scheduler（13 条）
- 核心风险集中在 `recover_unclaimed`：锁内 enqueue 导致 P0 死锁，失败返回值被忽略导致 Runnable 悬挂（SCHED-06）。
- `park_wait_for_cleanup` 缺 draining 二次检查，配合只扫描一次的 shutdown，可无界挂死（SCHED-03）。
- 超时放弃路径释放未终态任务的 callable，用户代码被静默跳过并报告 Completed（SCHED-05）。
- 亮点：三个状态位同临界区翻转、park 五步契约、shutdown 的 worker/外部调用者区分、锁层次文档。

### fiber（14 条）
- 无 P0；6 条 P1 全部是生命周期/竞态：`debug_stack` 与 `m_stack.reset()` 竞争、`~Impl` 自旋等 claim、`m_record` 替换竞争、abandon 跳过 RAII、bind_caller 父 Fiber UAF、SIGSEGV handler 覆盖宿主。
- P2 集中在 altstack 泄漏/永久降级、`fiber_bin_capacity=0` 不可禁用、entry 在 Fiber 栈析构 callable、growable 降级原因不可观测、256 KiB fault window、栈池 4 GiB 虚拟地址、跨进程迁移缺校验。
- 亮点：栈地址不变的增长设计、async-signal-safe handler、errno 隔离、ASAN/TSAN 钩子配对。

### hook（12 条）
- 3 条 P0 均与 libc 契约相关：超时 errno、ioctl 白名单、EINTR 重试。作为 LD_PRELOAD 运行时会静默改变宿主行为，建议最高优先级修复。
- P1：accept4 漏开关、未接管 fd 无负缓存（热路径 2 syscall+2 锁）、connect 超时默认值覆盖显式无限、sleep 不响应信号。
- 亮点：Descriptor 的 shared_ptr 保活 + token/generation 校验 + Guard 三重门，未发现确定 close/poll UAF；两阶段 close 设计正确。

### config（12 条）
- P0：事务锁提前释放破坏两阶段提交（CONF-01）。
- P1：SoReloader 的 `highest_seen` 失败也抬高且无重置、ReloadImpl 异常安全、watcher 自线程 detach UAF、reload_mutex 包住回调、BindRuntimeConfig 裸指针竞争、`min_workers` 漏检。
- 亮点：候选快照 + 跨字段校验 + 逆序回滚的设计方向正确；INI `\xHH` 编解码对称；`ReadStableFileContents` 的双快照抗截断。

### logging（12 条）
- P0：日志重入无防护（栈溢出）。
- P1：写失败静默且 Sink 永久失效、每 Logger 独立 FileSink 无跨实例互斥、Rotating 绕过 FileSink 语义且 `exists` 可抛、锁内 flush 与文档矛盾、BufferedSink 析构丢日志、静态初始化/析构顺序。
- 亮点：热路径原子 shared_ptr 快照、锁外调用用户代码、格式化缓冲边界安全。

## 4. 审查方法

1. 仓库状态核实：`git log/status/diff --summary`，确认 HEAD `a2fa0df`、工作区仅 19 个 Bento 文件的 mode 变化；建立只读快照（`C:\Temp\go2cpp_review`），避免误改工作树。
2. 逐模块阅读：scheduler 状态机/worker_loop/shutdown、fiber 栈与 handler/resume、hook 的 C ABI 分派与 fd 生命周期、config 的事务与 SoReloader、logging 的 worker/Sink/滚动。
3. 交叉验证：对子审查给出的 P0/P1 逐条回读源码确认行号与触发条件；对不确定项统一标「需人工确认」，不猜测。
4. **未运行**：本环境未在 WSL 构建/执行 ctest；hook 需要 LD_PRELOAD 宿主，fiber 的 ASAN/TSAN、scheduler 的并发压测均未实测。P0/P1 的触发条件按代码路径与锁序推断，报告已给出精确行号供复核。

## 5. 排期建议

1. 第一优先：HOOK-01/02/03（宿主兼容性）、SCHED-01/02（死锁）、LOG-01（栈溢出）、CONF-01（事务语义）。
2. 第二优先：SCHED-03/05（shutdown 挂死/用户代码丢失）、FIBER-01~06（生命周期 UAF/竞争）、CONF-04（detach UAF）、LOG-02/04（日志静默丢失）。
3. 第三优先：各模块 P2（性能/边界/可维护性）与 P3（可观测性）。

## 6. 范围与限制

- 本次**未**审查 bento、docs/modules 20 篇文档、channel/sync/io/os；若需要，应在下一轮单独立项，避免与本次五模块排期混在一起。
- 所有问题均可按 `file:line` 定位；「需人工确认」条目（FIBER-05/13、HOOK-12、CONF-04/12、LOG-07/08 等）依赖运行环境或外部用法，不能仅凭静态代码定性。
