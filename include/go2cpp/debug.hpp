#pragma once

// 转发头：调试模块（Fiber 元数据 / 调度器代理 / dump CLI / 锁观测 / 事件采集 / 断点 / 信号 dump）。
#include "go2cpp/debug/debug_fiber.hpp"
#include "go2cpp/debug/debug_scheduler.hpp"
#include "go2cpp/debug/debug_cli.hpp"
#include "go2cpp/debug/debug_sync.hpp"
#include "go2cpp/debug/debug_events.hpp"
#include "go2cpp/debug/debug_breakpoints.hpp"
#include "go2cpp/debug/debug_signal_dump.hpp"
#include "go2cpp/debug/debug_event_ring.hpp"
#include "go2cpp/debug/debug_dump.hpp"
#include "go2cpp/debug/debug_server.hpp"
