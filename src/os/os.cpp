/**
 * @file os.cpp
 * @brief `os` 模块里不依赖具体实现的少量符号。
 */
/**
 * ## 模块定位（补充）
 *
 * `go2cpp::os` 入口的**最小实现单元**：只放不依赖文件/目录实现的符号，便于
 * 只想调用 `Exit` 的程序不必链接 `file.cpp`/`dir.cpp`。
 *
 * ## 本文件定义什么
 *
 * - `Exit`：`std::exit` 的薄封装，警告与替代顺序见 `os.hpp`。
 *
 * ## 阅读建议
 *
 * 本文件只有一行实现；真正需要读的是 `os.hpp` 中 `Exit` 的长注释 —— 它解释
 * 为什么这不是正常退出路径。
 *
 * ## 边界与陷阱
 *
 * `Exit` 不释放 Fiber 栈、不析构挂起的 `Task`、不给 epoll 注册项收尾；有活跃
 * 调度器时应先 `scheduler.shutdown_for(...)` 再调用。
 */
#include "go2cpp/os/os.hpp"

#include <cstdlib>

namespace go2cpp::os {

// 见 os.hpp 对该函数的完整警告：这是逃生通道，不负责 Fiber 栈与挂起 Task 的清理。
void Exit(int code) { std::exit(code); }

}  // namespace go2cpp::os
