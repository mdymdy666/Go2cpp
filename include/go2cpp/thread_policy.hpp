#pragma once

/**
 * @file thread_policy.hpp
 * @brief 兼容旧根路径的转发头：线程参与/ Hook 策略。
 *
 * ## 模块定位
 * 转发到 `go2cpp/concurrency/thread_policy.hpp`。该模块提供
 * `ThreadParticipationMode`/`ThreadHookMode`、`ScopedThreadParticipation`、
 * `ScopedThreadHookMode` 与 `EnterRuntimeWorker`/`LeaveRuntimeWorker`，
 * 被调度器的 worker 入口与 IO Hook 使用。
 *
 * ## 本文件定义什么
 * 本文件**不定义任何类型**，只做旧路径转发。
 *
 * ## 阅读建议
 * 新代码直接包含 `go2cpp/concurrency/thread_policy.hpp`。
 *
 * ## 边界与陷阱
 * 这是兼容层，不要在此添加实现或类型；策略的实际判定在 concurrency 模块。
 */

// 兼容旧版公共头文件路径；实际实现位于模块目录。
#include "go2cpp/concurrency/thread_policy.hpp"
