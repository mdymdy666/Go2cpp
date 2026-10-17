/**
 * @file os.cpp
 * @brief `os` 模块里不依赖具体实现的少量符号。
 */
#include "go2cpp/os/os.hpp"

#include <cstdlib>

namespace go2cpp::os {

// 见 os.hpp 对该函数的完整警告：这是逃生通道，不负责 Fiber 栈与挂起 Task 的清理。
void Exit(int code) { std::exit(code); }

}  // namespace go2cpp::os
