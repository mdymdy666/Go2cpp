/**
 * @file container.hpp
 * @brief `container` 模块的汇总入口：一次引入 Heap / List / Ring。
 *
 * @details 三个容器各自位于独立头文件，按需单独 include 可以缩短编译时间；
 *          本头只做转发，不引入任何额外符号或依赖。
 */
#pragma once

#include "go2cpp/container/heap.hpp"
#include "go2cpp/container/list.hpp"
#include "go2cpp/container/ring.hpp"
