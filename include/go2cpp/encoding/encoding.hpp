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
 *
 * ## 模块定位
 *
 * 基础编解码工具层，不参与协议状态、不做 IO：把「字节 ↔ 文本」和「字节 ↔ 整数」
 * 这两类纯函数集中在一处。上游是 `http`/`json`/`websocket`/`quic` 等需要
 * base64/hex/varint 的协议模块，下游只有 `go2cpp::tools::error`。
 *
 * ## 本文件定义什么
 *
 * 本文件不定义任何类型，只转发四个子模块：
 * - `base64.hpp`：`Base64Encoding` 与四种预定义编码；
 * - `hex.hpp`：`Hex` 静态工具集；
 * - `binary.hpp`：`Binary` 定长字节序 + LEB128；
 * - `csv.hpp`：`CsvReader` / `CsvWriter`。
 *
 * ## 阅读建议
 *
 * 四个子模块互不依赖，按需单独包含即可；只有同时用到多种编码时才包含本文件。
 * 先读各子模块文件头的「关键语义」，再查具体函数。
 */

#pragma once

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/encoding/binary.hpp"
#include "go2cpp/encoding/csv.hpp"
#include "go2cpp/encoding/hex.hpp"
