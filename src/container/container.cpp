/**
 * @file container.cpp
 * @brief `container` 模块的编译单元：头文件自包含检查 + 模块 link target 锚点。
 *
 * @details 三个容器都是 header-only 模板。本文件不做任何实现，存在的目的有两个：
 *   1. 用一个**不依赖任何前置声明**的 TU 同时包含三个公开头文件，保证它们各自
 *      自包含（漏 include 时这里会直接编译失败，而不是等用户碰到）；
 *   2. 让 `go2cpp_container` 拥有真实的编译目标，从而沿用项目现有的
 *      「一个模块一个 library target + 安装导出」机制。
 */
#include "go2cpp/container/container.hpp"

namespace go2cpp::container {
namespace {

// 显式锚点：避免某些工具链对完全空的 TU 给出提示；不导出任何 ABI。
[[maybe_unused]] constexpr int kContainerModuleAnchor = 1;

}  // namespace
}  // namespace go2cpp::container
