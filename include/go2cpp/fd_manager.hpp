#pragma once

/**
 * @file fd_manager.hpp
 * @brief 兼容旧根路径的转发头：`FdContext` / `FdManager`。
 *
 * ## 模块定位
 * 转发到 `go2cpp/network/fd_manager.hpp`。真正的文件描述符生命周期管理
 * （引用计数、关闭回调、等待者登记）在 network 模块，被 `io`/`socket` 层使用。
 *
 * ## 本文件定义什么
 * 本文件**不定义任何类型**，只把旧路径 `go2cpp/fd_manager.hpp` 转发到模块头。
 *
 * ## 阅读建议
 * 新代码请直接包含 `go2cpp/network/fd_manager.hpp`；从这里进入只会多一层转发。
 *
 * ## 边界与陷阱
 * 这是兼容层，不要在此添加实现或类型；否则两条包含路径会各自维护一份定义。
 */
// 兼容旧式根路径包含；实现位于 network/fd_manager.hpp。
#include "go2cpp/network/fd_manager.hpp"
