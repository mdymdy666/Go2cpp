/**
 * @file hash.cpp
 * @brief `hash` 模块的编译单元：头文件自包含检查 + 模块 link target 锚点。
 *
 * @details 存在两个目的：
 *   1. 用一个不依赖任何前置声明的 TU 同时包含全部公开头文件，漏 include 时
 *      这里会直接编译失败，而不是等使用者碰到；
 *   2. 让 `go2cpp_hash` 拥有真实的编译目标，沿用项目「一个模块一个 library
 *      target + 安装导出」的机制。
 */
#include "go2cpp/hash/adler32.hpp"
#include "go2cpp/hash/crc32.hpp"
#include "go2cpp/hash/hash.hpp"
#include "go2cpp/hash/md5.hpp"
#include "go2cpp/hash/sha1.hpp"
#include "go2cpp/hash/sha256.hpp"

namespace go2cpp::hash {
namespace {

// 显式锚点：不导出任何 ABI，只为避免空 TU 的工具链提示。
[[maybe_unused]] constexpr int kHashModuleAnchor = 1;

}  // namespace
}  // namespace go2cpp::hash
