/**
 * @file container.cpp
 * @brief `container` 模块的编译单元：头文件自包含检查 + 模块 link target 锚点。
 *
 * @details 三个容器都是 header-only 模板。本文件不做任何实现，存在的目的有两个：
 *   1. 用一个**不依赖任何前置声明**的 TU 同时包含三个公开头文件，保证它们各自
 *      自包含（漏 include 时这里会直接编译失败，而不是等用户碰到）；
 *   2. 让 `go2cpp_container` 拥有真实的编译目标，从而沿用项目现有的
 *      「一个模块一个 library target + 安装导出」机制。
 *
 * ## 模块定位
 *
 * 编译单元层：`go2cpp_container` 这个 library target 的唯一源文件。上游没有调用方，
 * 下游是 CMake 的安装/导出机制与 `go2cpp_container_test` 的链接关系。
 *
 * ## 本文件定义什么
 *
 * 只有一个匿名命名空间内的 `kContainerModuleAnchor` 常量，用于占位；不导出任何符号，
 * 也不产生运行时代码。
 *
 * ## 阅读建议
 *
 * 不需要阅读。容器实现全在对应的三个头文件里，本文件只负责「让 target 有东西可编译」。
 *
 * ## 边界与陷阱
 *
 * - 不要把容器实现搬到这里：三个容器都是模板，必须留在头文件里才能被实例化。
 * - 若将来容器需要非模板实现，应新增独立的 .cpp，而不是把本文件变成大杂烩。
 */
#include "go2cpp/container/container.hpp"

namespace go2cpp::container {
namespace {

// 显式锚点：避免某些工具链对完全空的 TU 给出提示；不导出任何 ABI。
[[maybe_unused]] constexpr int kContainerModuleAnchor = 1;

}  // namespace
}  // namespace go2cpp::container
