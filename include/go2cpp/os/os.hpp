/**
 * @file os.hpp
 * @brief `go2cpp::os` 模块入口 —— 对齐 Go `os` 包的最小子集（开荒版）。
 *
 * ## 模块定位
 *
 * Go 的 `os` 包横跨三类能力：**文件**、**进程/环境**、**目录**。本模块只开荒
 * 前两类的**最小可用子集**，目录遍历留待后续（见文件末尾"未实现"）。
 *
 * ## 与项目其他模块的边界
 *
 * `os` 模块**刻意不参与协程调度**：文件 IO 在内核里是同步完成的（不会 `EAGAIN`），
 * 接进 `IOManager` 只会白付一次 park/唤醒。详细论证与「必须拒绝 socket fd」
 * 的原因见 `file.hpp` 顶部的边界结论 —— 那是本模块最重要的设计文档。
 *
 * ## 能力清单
 *
 * - `File`：RAII 句柄，`Open`/`Create`/`Adopt`/`Read`/`ReadAt`/`Write`/`WriteAt`/
 *   `WriteAll`/`Seek`/`Tell`/`Stat`/`Size`/`Close`，可移动不可复制；
 * - `ReadFile` / `WriteFile`：整文件读写的便利函数；
 * - `Getenv` / `Setenv` / `Unsetenv` / `Environ`：环境变量（返回值一律**拷贝**）；
 * - `SetArgs` / `Args` / `ProgramName`：命令行参数（**拷贝** `argv` 内容）；
 * - `Exit`：见下方警告。
 *
 * ## 明确不负责的事
 *
 * 它**不**接管 fd 的生命周期管理（那是 `FdManager` 对 socket 做的事），
 * **不**修改任何 fd 的标志位，**不**做平台抽象（POSIX only）。
 */
/**
 * ## 模块定位（补充）
 *
 * `go2cpp::os` 是**最低层的 POSIX 能力封装**，位于运行时调度器与业务代码之下：
 * - 上游调用方：`go2cpp::config` 的 `[os]` 段（`src/os/config.cpp` 静态注册
 *   `os.default_read_buffer_size` / `os.max_file_size`）、`tests/os/*`、examples；
 * - 下游：直接使用 Linux syscall/libc，不依赖 `IOManager`/`Scheduler`；
 * - 与 `FdManager` 的边界：本模块**不**接管 socket fd，也不修改 fd 标志位。
 *
 * ## 本文件定义什么
 *
 * - `Exit`：唯一的进程级操作，声明在此、实现见 `src/os/os.cpp`；
 * - `SaveDefaultConfig`：把当前全部注册项导出为带注释的 INI；
 * - `ReadBufferSize`/`MaxFileSize`：从配置读取并钳制后的 `ReadFile` 参数。
 * `File`/`ReadFile`/环境变量等实体在 `file.hpp`，目录能力在 `dir.hpp`。
 *
 * ## 阅读建议
 *
 * 先读 `Exit` 的警告（它不清理 Fiber/Task），再读 `file.hpp` 顶部「为什么文件
 * IO 不参与协程调度」的长文；`os.hpp` 本身只是入口与配置读取。
 *
 * ## 边界与陷阱
 *
 * - `ReadBufferSize` 有 4 KiB..16 MiB 的硬钳制，配置错得再离谱也不会让缓冲区爆炸；
 * - `MaxFileSize` 返回 0 表示**不限制**，不是「禁止读取」；
 * - 本模块 POSIX only，Windows 分支不存在。
 */
#pragma once

#include "go2cpp/os/dir.hpp"
#include "go2cpp/os/file.hpp"

#include <string>

namespace go2cpp::os {

/**
 * @brief 立即终止进程。
 *
 * @warning **这是一个逃生通道，不是正常退出路径。**
 *
 * @details `std::exit` 只跑**静态对象析构与 `atexit` 回调**，它**不会**：
 * - 释放挂起 Fiber 的栈（`FiberBin` 池里的栈会随进程一起消失，不泄漏但也不回收）；
 * - 析构仍在运行/挂起的 `Task`（它们的 `shared_ptr` 引用计数不会归零，
 *   析构函数不执行 —— 若有 RAII 资源在 `Task` 里，不会被释放）；
 * - 给 `IOManager` 里的 epoll 注册项收尾。
 *
 * 所以在有活跃调度器的程序里调用它，等于**放弃清理**。正确的顺序是：
 *
 * ```cpp
 * scheduler.shutdown_for(std::chrono::seconds(5));   // 先优雅收尾
 * go2cpp::os::Exit(0);                              // 再退出
 * ```
 *
 * 本模块**没有**提供「自动先 shutdown 再 exit」的版本，原因是：
 * 那需要持有 `Scheduler` 的引用，而 `Exit` 可能在静态析构期被调用
 * （此时 `Scheduler` 单例可能已析构），反而制造新的生命周期陷阱。
 * 把顺序交给调用方显式控制，比在一个 `[[noreturn]]` 函数里猜状态更安全。
 *
 * @note 用 `std::exit` 而非 `std::_Exit`：保留刷新 stdio、跑 `atexit` 的能力，
 *       那对日志落盘是必要的。若你连这些都不想要，直接 `std::_Exit`。
 */
[[noreturn]] void Exit(int code);


// ---------------------------------------------------------------------------
// 配置集成（需要额外链接 src/os/config.cpp 与 go2cpp_config）
// ---------------------------------------------------------------------------

/**
 * @brief 把当前全部 `os.*` 配置写成 INI 文件（默认 `app.ini`）。
 * @details 供启动路径调用：程序首次运行时生成一份带注释的默认配置，
 *          用户改完下次启动即可生效。内部直接调
 *           `config::Config::SaveToFile`（原子写：临时文件 + rename）。
 * @note 只有链接了 `src/os/config.cpp` 时 `os.*` 相关项才会被注册；
 *       没链接时本函数仍能工作，但生成的文件里不会有 `[os]` 段。
 */
bool SaveDefaultConfig(const std::string& path = "app.ini",
                       std::string* error = nullptr);

/** @brief `ReadFile` 使用的缓冲区大小（来自 `os.default_read_buffer_size`，带安全钳制）。 */
std::size_t ReadBufferSize();

/** @brief `ReadFile` 允许的单文件上限（来自 `os.max_file_size`；0 表示不限制）。 */
std::size_t MaxFileSize();

}  // namespace go2cpp::os
