# 性能整改与 Coost 对比

本文记录本轮调度器和混合锁优化的实际结果。所有数据来自同一台
WSL2/Linux 主机、同一份源码和同一条 `go2cpp_high_load_stress` 命令；
时间单位为毫秒。

## 本轮实现的优化

- 调度器在本地 P 队列成功取到 G 后不再获取调度器全局 admission 锁。
- G/M/P 运行计数和任务亲和性记录统一放到两条取任务路径之后，避免本地
  快路径遗漏 M/P 计数。
- `maybe_grow()` 从每次 Fiber 恢复改成有界维护周期；当活跃 M 少于 P 且
  仍有 runnable G 时立即补齐初始 worker，保留 P=1/并发 shutdown 的语义。
- Scheduler 恢复 Fiber 时首次绑定父级，后续同一 G 的恢复跳过重复父链元数据
  锁；发生 M/P 迁移时仍更新调试绑定。
- 混合锁在短临界区竞争时先进行最多 16 次 Fiber 协作让出，只有持续竞争才
  创建等待节点并进入 FIFO handoff。已发布等待者存在时禁止无序插入，避免
  新的 TryLock/Lock 越过 FIFO 队列。

这些改动没有把普通线程锁伪装成 Fiber 锁，也没有用异步强行切断 C++ 栈；
Fiber 等待仍通过 Scheduler park，普通线程仍通过条件变量等待。

## 高负载结果

测试参数固定为：调度器 50,000 个任务、每个任务 4 次 yield；计算负载
8,000 个任务、每个任务 100,000 次循环；混合锁为 2,000 个 Fiber 和
32 个普通线程；Channel 为 32,000 条消息；IO 为 4,096 个 reader、25
轮 readiness。代表性运行结果如下：

| 场景 | Fiber/Go2Cpp | 线程基准 | 比例 |
|---|---:|---:|---:|
| 计算任务 | 1454 ms | 1415 ms | 1.03x |
| 调度与 yield | 474 ms | 65 ms | 7.29x |
| 混合 Mutex | 103 ms | 37 ms | 2.78x |
| Channel | 58 ms | 80 ms | 0.73x（Fiber 更快） |
| IO 等待 | 1748 ms | 1812 ms | 0.96x（Fiber 更快） |

旧实现同一基准的调度约 6.2 秒、混合 Mutex 约 13.9 秒；本轮分别降到
约 0.47 秒和 0.10 秒。重复运行存在宿主调度抖动：调度通常为
0.45～0.57 秒，混合 Mutex 在稳定运行时约 0.10～0.20 秒。高负载测试
连续完成全部校验，没有发现重复执行、丢任务、锁失败或消息丢失。

调度基准中的线程版本调用 `std::this_thread::yield()`，它不保存用户栈，
所以不能把 7.29x 直接解释为 Fiber 上下文切换的纯成本。当前指标中
`fiber_resume_ns` 可用于测量真实 Fiber 恢复累计时间；本轮代表性运行是
约 0.79 秒/250,000 次恢复，即约 3.2 微秒/次，其中包含安全状态、errno、
调度边界和统计开销。

## 与 Coost 的可比性

Coost 官方仓库明确提供多线程协程调度、共享栈、协程锁和 waitgroup；其
默认共享若干约 1 MB 的栈，目标是降低大量协程的栈内存占用。参见
[Coost 官方仓库](https://github.com/idealvin/coost)。

本工作区当前无法从 GitHub 拉取 Coost 源码（WSL 到 github.com:443 的网络
连接被环境拒绝），因此没有伪造 Coost 的本地运行数字，也没有把 Coost 的
日志吞吐表当成调度器或 Mutex 基准。官方页面公布的是日志与 glog/spdlog
的对比，不是本项目使用的同一任务、同一 IO 和同一锁测试，不能直接换算成
本项目的性能差距。

结构上，当前 Go2Cpp 使用 Boost.Context 的受保护固定栈、按线程和全局的栈
缓存，以及 P 本地队列加有界窃取；Coost 的共享栈方案仍是后续可以借鉴的
方向。当前未完成的性能缺口是可验证的共享/动态栈后端，以及把调度基准改成
同样保存用户栈的线程 Fiber 基线。两项都不能仅凭 Coost 的 README 数字宣称
已经解决。

## 复现实验

```sh
cd /mnt/e/CodexWorkspace/NewGo2Cpp
cmake --build build-engineering-wsl --target go2cpp_high_load_stress -j2
timeout 90s ./build-engineering-wsl/go2cpp_high_load_stress
```

完整功能测试仍需单独运行 `go2cpp_tests`；性能测试不是内存安全或竞态
证明，ASan、UBSan、TSan 和 Valgrind 仍按 `docs/testing.md` 的独立命令执行。
