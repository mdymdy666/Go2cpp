/**
 * @file encoding.hpp
 * @brief `go2cpp::encoding` 模块总入口：base64 / hex / binary / csv。
 *
 * @author DeepSeek Harness
 * @date 2026-10-18
 * @copyright BSD-3-Clause
 *
 * @details 四个子模块各自独立、互不依赖，可以单独包含；本头文件只是转发。
 *          模块整体只依赖 `go2cpp/tools/error.hpp`（错误值），不依赖运行时、
 *          IO 或调度器，因此可被最小程序直接链接 `go2cpp_encoding` 使用。
 */

#pragma once

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/encoding/binary.hpp"
#include "go2cpp/encoding/csv.hpp"
#include "go2cpp/encoding/hex.hpp"
