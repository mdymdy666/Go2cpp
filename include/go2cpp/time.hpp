#pragma once

// 模块入口转发头：Go `time` 包核心子集的对应物。
// 真身位于 `go2cpp/tools/time.hpp`；命名空间为 `go2cpp::time`。
#include "go2cpp/tools/time.hpp"

/**
 * ## 模块定位
 *
 * 根路径入口，只为让 `#include "go2cpp/time.hpp"` 的写法与其它模块一致；
 * 真实实现在 `include/go2cpp/tools/time.hpp`（`Duration`/`Time`/`Sleep` 的完整说明）。
 *
 * ## 本文件定义什么
 *
 * 不定义任何符号：只有一行转发 `#include`。
 *
 * ## 阅读建议
 *
 * 请直接读 `include/go2cpp/tools/time.hpp`；本文件没有可读内容。
 *
 * ## 边界与陷阱
 *
 * 注意本模块**不包含** `Timer`/`Ticker`/`After`/`Tick`（未实现，见真身文件末尾待办），
 * 也不要在转发头里补任何定义 —— 那会让两条包含路径产生不同内容。
 */
