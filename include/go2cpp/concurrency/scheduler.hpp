/**
 * @file scheduler.hpp
 * @brief Scheduler 模块的公共入口（转发头）。
 *
 * ## 为什么存在
 *
 * 调度器的真身位于 `go2cpp/concurrency/scheduler/scheduler.hpp`。这个文件只是
 * 一条**兼容入口**：让调用方可以写 `#include "go2cpp/scheduler.hpp"` 而不必
 * 关心内部子目录布局。
 *
 * 这样组织的好处是内部可以把 scheduler 相关的多个头文件收进子目录（那里还有
 * 实现细节与不打算对外承诺的类型），而对外只暴露一个稳定路径。**修改调度器
 * 时不要改这里** —— 它刻意不含任何逻辑。
 */
#pragma once

// Scheduler 模块公共入口。
#include "go2cpp/concurrency/scheduler/scheduler.hpp"
