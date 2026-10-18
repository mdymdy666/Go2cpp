#pragma once

// 模块入口转发头：`json` 的真身位于 `go2cpp/tools/json.hpp`。
// 与其余模块「根路径转发头 + 子目录真身」的布局保持一致；本文件不含任何定义。
#include "go2cpp/tools/json.hpp"

/**
 * ## 模块定位
 *
 * 根路径入口，只为让 `#include "go2cpp/json.hpp"` 这种与其它模块一致的写法可用；
 * 真实实现在 `include/go2cpp/tools/json.hpp`（含限深栅栏的完整论证与实测数据）。
 *
 * ## 本文件定义什么
 *
 * 不定义任何符号：只有一行转发 `#include`。
 *
 * ## 阅读建议
 *
 * 请直接读 `include/go2cpp/tools/json.hpp`；本文件没有可读内容。
 *
 * ## 边界与陷阱
 *
 * 不要把实现加到这里 —— 转发头的存在意义就是保持「根路径入口 + tools/ 真身」的布局；
 * 在转发头里加代码会让两条包含路径产生不同定义。
 */
