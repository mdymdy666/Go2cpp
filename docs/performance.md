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

本轮直接使用本机已有的 Coost checkout：
`/mnt/e/CodexWorkspace/Go2Cpp/coost`，构建时的 HEAD 为 `c1cc11b`。
该 checkout 在开始前已经是 dirty 状态，因此下面的数字代表这份本地快照，
不代表某个干净发布版；没有修改 Coost 源码。参见
[Coost 官方仓库](https://github.com/idealvin/coost)。

两个项目都在同一台 WSL2/Linux 主机上以 Release 构建。调度测试使用 50,000
个任务、每个任务 4 次让出、8 个生产线程；Coost 使用 `co::sleep(0)` 让任务
被定时器重新放回 runnable 队列，因而只比较“自动重新调度”这条路径，不能把
Coost 的 `co::yield()`（需要调用者显式再次 resume）当成同一个 API。混合锁
测试使用 2,000 个协程、32 个普通线程，共 206,400 次加解锁；IO 测试使用
4,096 个 socketpair reader、25 轮 readiness。

| 场景 | Coost | Go2Cpp（本轮） | 结果 |
|---|---:|---:|---|
| 自动调度与 yield | 13～52 ms | 436～497 ms | Go2Cpp 慢约 8～38 倍 |
| 混合 Fiber/线程 Mutex | 1,097～1,766 ms | 99～107 ms | Go2Cpp 快约 10～18 倍 |
| 多 fd IO 等待 | 486～569 ms | 1,666～2,098 ms | Go2Cpp 慢约 3.0～4.3 倍 |

调度差距主要来自实现模型：Coost 使用少量共享栈，切换时保存/恢复栈内容；
Go2Cpp 当前使用 Boost.Context 受保护固定栈，并在安全性优先的路径中维护
G/M/P、Fiber 父链和任务状态。共享栈可以显著减少栈分配和切换成本，但必须
补齐 C++ 栈对象生命周期、栈地址失效和异常边界验证，不能直接替换成未验证
的 memcpy 方案。

混合锁结果已经反超 Coost。本轮将无 Context 的竞争等待节点改为调用栈上的
节点，队列只保存裸指针并保持 FIFO handoff；带 Context 的等待仍由缓存的
拥有节点保证取消期间的生命周期。这样去除了短等待的 shared_ptr 控制块和
分配开销，同时没有改变普通线程阻塞语义。

IO 仍是明确的性能缺口。Go2Cpp 的 DescriptorToken、fd generation 校验、
取消/超时竞争和 epoll interest 更新增加了锁与生命周期管理成本；Coost 的
实现使用更轻量的嵌入式等待节点。下一步可以在保持 generation 校验的前提下
增加 Fiber 私有 IO 等待节点和批量 interest 更新，但当前不宣称已经解决。

上述 Go2Cpp 调度对照临时将 Fiber 栈设为 32 KiB；生产默认值仍由
`SchedulerConfig` 控制，没有为了单个基准改变公开默认值。Coost 的调度和锁
数据来自三次连续运行，IO 数据也来自三次连续运行；宿主调度抖动会影响区间。

## 复现实验

```sh
cd /mnt/e/CodexWorkspace/NewGo2Cpp
cmake --build build-engineering-wsl --target go2cpp_high_load_stress -j2
timeout 90s ./build-engineering-wsl/go2cpp_high_load_stress
```

完整功能测试仍需单独运行 `go2cpp_tests`；性能测试不是内存安全或竞态
证明，ASan、UBSan、TSan 和 Valgrind 仍按 `docs/testing.md` 的独立命令执行。
