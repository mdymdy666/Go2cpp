#pragma once

/**
 * @file io_backend.hpp
 * @brief 兼容旧根路径的转发头：`IOBackend` 抽象。
 *
 * ## 模块定位
 * 转发到 `go2cpp/network/io_backend.hpp`。`IOBackend` 是 IO 多路复用的统一
 * 抽象（`BackendEventFlag`/`BackendEvent`/`IOBackendPtr`/`IOBackendFactory`），
 * 当前由 epoll 后端实现，并为未来的 io_uring 后端预留同一接口。
 *
 * ## 本文件定义什么
 * 本文件**不定义任何类型**，只做旧路径转发。
 *
 * ## 阅读建议
 * 新代码直接包含 `go2cpp/network/io_backend.hpp`，看 `IOBackend` 的虚接口。
 *
 * ## 边界与陷阱
 * 这是兼容层，不要在此添加实现；后端选择由工厂/配置决定，不在这里。
 */

// 兼容旧版公共头文件路径；实际实现位于模块目录。
#include "go2cpp/network/io_backend.hpp"
