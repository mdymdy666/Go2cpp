/**
 * @file locks.hpp
 * @brief 并发锁设施汇总头：`RWMutex` / `RecursiveMutex` / `TimedMutex`。
 *
 * @date 2026-10-18
 * @author DeepSeek Agent
 * @copyright MIT License（见项目根目录 LICENSE）
 *
 * ## 什么时候用哪把锁
 *
 * | 锁 | 语义 | 等待路径 | 典型场景 |
 * |---|---|---|---|
 * | `sync::Mutex` | 独占、Go 风格所有权 | Fiber park / OS 线程阻塞 | 默认选择 |
 * | `RWMutex` | 读写锁、**写者优先** | 同上（内部 `Mutex` + CV） | 配置热加载、连接池、路由表 |
 * | `RecursiveMutex` | 可重入、绑定 owner | 同上 | **移植遗留代码**（见其文件头警告） |
 * | `TimedMutex` | 超时返回 false | 同上（复用 `Mutex::LockFor`） | 持锁 IO / 跨服务调用的兜底 |
 *
 * 这三把锁都是**独立模块**：只包含 `go2cpp/sync.hpp` 与调度器的公开头，
 * 不修改 `sync::Mutex` / `sync::ConditionVariable` / `Scheduler` 的任何实现。
 * 它们与 `sync::Mutex` 共享同一套协程 / 线程双等待路径：
 *
 * - 受管 G 等待时 **park**（让出 M，其他 G 继续跑）；
 * - 普通 OS 线程等待时 **真阻塞**（原生条件变量 / futex）；
 * - 两者可以互相同步：线程解锁唤醒 G，G 解锁唤醒线程。
 *
 * ## 共同边界
 *
 * 1. 临界区内**不要 park**（IO / `time.Sleep` / 阻塞 channel 操作）。这会
 *    把「协作式等待」变成「持锁消失」，是 Go2Cpp 特有的死锁来源。详见
 *    `include/go2cpp/concurrency/spin_mutex.hpp` 的论证。
 * 2. 锁对象必须长于所有使用者和等待者。
 * 3. 三把锁都不可拷贝 / 移动。
 *
 * ## 用法
 *
 * ```cpp
 * go2cpp::sync::RWMutex mu;
 * // 读路径
 * mu.RLock();
 * const auto snapshot = config;   // 锁内只拷贝，不做 IO
 * mu.RUnlock();
 * // 写路径
 * mu.Lock();
 * config = next;                  // 锁外先算好 next
 * mu.Unlock();
 * ```
 */
#pragma once

#include "go2cpp/concurrency/recursive_mutex.hpp"
#include "go2cpp/concurrency/rwmutex.hpp"
#include "go2cpp/concurrency/timed_mutex.hpp"
