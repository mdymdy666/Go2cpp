#pragma once

/**
 * @file runtime.hpp
 * @brief Go2Cpp 对外聚合头文件。
 * @details 一次包含全部公开运行时模块；大型工程也可以按模块头文件分别
 *          包含，以减少编译依赖。
 */

// 面向转译程序的便捷总头文件。需要更小依赖面时仍可单独包含各模块头文件。
#include "go2cpp/channel.hpp"
#include "go2cpp/config.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/control_flow.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/future.hpp"
#include "go2cpp/fiber.hpp"
#include "go2cpp/fiber_local.hpp"
#include "go2cpp/go.hpp"
#include "go2cpp/event.hpp"
#include "go2cpp/hook.hpp"
#include "go2cpp/io.hpp"
#include "go2cpp/log.hpp"
#include "go2cpp/scheduler.hpp"
#include "go2cpp/sync.hpp"
#include "go2cpp/thread_policy.hpp"
