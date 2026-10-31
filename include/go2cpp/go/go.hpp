/**
 * @file go.hpp
 * @brief Go 接口对接层的聚合头文件。
 *
 * 一次包含 `go2cpp/go/` 目录下的全部模块：
 * `error`、`context`、`sync`、`chan`、`time`、`net`、`io`。
 *
 * @note 只想用某个子集时请直接包含对应头（如 `go2cpp/go/net.go.hpp`），
 *       聚合头会把 net/socket 一并拉进来，编译时间更长。
 */

#pragma once

#include "go2cpp/go/chan.go.hpp"
#include "go2cpp/go/context.go.hpp"
#include "go2cpp/go/error.go.hpp"
#include "go2cpp/go/io.go.hpp"
#include "go2cpp/go/net.go.hpp"
#include "go2cpp/go/sync.go.hpp"
#include "go2cpp/go/time.go.hpp"
