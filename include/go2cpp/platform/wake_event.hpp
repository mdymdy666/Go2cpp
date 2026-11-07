/**
 * @file wake_event.hpp
 * @brief 跨平台「唤醒事件」——替代 Linux `eventfd` / Windows `CreateEvent`。
 *
 * ## 定位
 * IOManager / Scheduler 需要一种「从任意线程唤醒 IO 等待者」的机制。
 * Linux 用 `eventfd`（计数器），Windows **没有等价物**——最接近的
 * `CreateEvent` 是**二值信号**。本类把两者统一成同一契约，**但语义差异必须知道**：
 *
 * | 平台 | 底层 | 多次 Notify |
 * |---|---|---|
 * | Linux | `eventfd(0, EFD_NONBLOCK\|EFD_CLOEXEC)` | **累积**（Drain 一次读走全部计数） |
 * | Windows | `CreateEventW(NULL, FALSE, FALSE, NULL)` | **合并**（多次 SetEvent 只算一次） |
 *
 * **调用方注意**：如果你依赖「Notify 次数」做逻辑（例如「每次 notify 代表一个任务」），
 * 在 Windows 上会**丢失次数**。正确做法是「Notify 只作为提示，真正的状态从共享数据结构读」。
 *
 * ## 边界与陷阱
 * - `NativeHandle()` 返回的 fd/HANDLE **只读**，不要 close；
 * - Windows 的 `CreateEventW` 手动重置版本（`bManualReset=TRUE`）**不适合**本用途——必须自动重置；
 * - `Drain()` 在 Linux 上可能一次读走多个计数，Windows 上只清一个信号。
 */
#pragma once

#include <cstdint>
#include <string>

namespace go2cpp::platform {

class WakeEvent {
 public:
  WakeEvent() = default;
  ~WakeEvent();

  WakeEvent(const WakeEvent&) = delete;
  WakeEvent& operator=(const WakeEvent&) = delete;

  /**
   * @brief 创建底层事件对象。
   * @param err [out] 失败时写错误文本；成功时保持原值。
   * @return true=成功；false=失败（err 已写）。
   *
   * ## 并发
   * 必须在**单线程**初始化阶段调用；失败后对象不可用。
   */
  bool Open(std::string* err = nullptr);

  /**
   * @brief 通知等待者。
   *
   * ## 语义差异（重要）
   * - Linux：计数 +1（可累积）；
   * - Windows：SetEvent（**多次只算一次**，不累积）。
   *
   * 调用方**不应**依赖通知次数。
   */
  void Notify() noexcept;

  /**
   * @brief 排空事件（消费所有待处理通知）。
   *
   * ## 语义差异
   * - Linux：一次读走全部计数；
   * - Windows：只清当前信号（若期间有多次 SetEvent，也只清一次）。
   */
  void Drain() noexcept;

  /// @brief 底层句柄：Linux 返回 fd（int），Windows 返回 HANDLE（void* 转 intptr）。
  /// @return 未 Open 时返回 -1。**只读**，调用方不要 close。
  std::intptr_t NativeHandle() const noexcept;

  /// @brief 是否已成功 Open。
  bool valid() const noexcept { return m_handle >= 0; }

 private:
  std::intptr_t m_handle = -1;
};

}  // namespace go2cpp::platform
