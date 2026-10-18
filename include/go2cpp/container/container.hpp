/**
 * @file container.hpp
 * @brief `container` 模块的汇总入口：一次引入 Heap / List / Ring。
 *
 * @details 三个容器各自位于独立头文件，按需单独 include 可以缩短编译时间；
 *          本头只做转发，不引入任何额外符号或依赖。
 *
 * ## 模块定位
 *
 * `container` 模块的**汇总入口**，位于 Go 标准库适配层的最外层。下游是
 * `src/container/container.cpp`（用它做头文件自包含检查）与 `tests/container/*`；
 * 上层业务代码若同时需要多个容器，包含本头即可。
 *
 * ## 本文件定义什么
 *
 * 本文件**不定义任何符号**：只按固定顺序引入 `heap.hpp`、`list.hpp`、`ring.hpp`，
 * 命名空间统一为 `go2cpp::container`。
 *
 * ## 阅读建议
 *
 * 想弄清某个容器的语义，直接读对应子头文件；本文件只有转发，没有可读的实现细节。
 *
 * ## 边界与陷阱
 *
 * - 只为「同时用到多个容器」的场景服务；只要一个容器时请直接 include 子头文件，
 *   避免把另外两个模板也拉进编译单元。
 * - 子头文件之间互不依赖，因此本头的引入顺序不影响语义。
 */
#pragma once

#include "go2cpp/container/heap.hpp"
#include "go2cpp/container/list.hpp"
#include "go2cpp/container/ring.hpp"
