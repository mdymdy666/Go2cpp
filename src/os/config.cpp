/**
 * @file config.cpp
 * @brief `os` 模块的可配置项注册与 `app.ini` 生成。
 *
 * ## 为什么放在单独文件
 *
 * `file.cpp` 是纯 POSIX 实现，**不依赖** config 库；本文件才把 `os` 模块接进
 * 项目的配置中心。这样：
 * - 只想用文件 API 的程序可以只链接 `file.cpp`；
 * - 想用配置驱动的程序再链接本文件 + `go2cpp_config`。
 *
 * ## 注册的配置项
 *
 * | 键 | 作用 |
 * |---|---|
 * `os.default_read_buffer_size` | `ReadFile` 每次 `read` 的缓冲区字节数 |
 * `os.max_file_size` | `ReadFile` 允许读取的单文件上限（防 OOM） |
 *
 * 注册是**静态初始化期**完成的（宏内部构造 `AutoConfigRegistrar`），
 * 所以只要这个翻译单元被链接进来，`Config::Instance()` 里就有这两项。
 */
/**
 * ## 模块定位（补充）
 *
 * `os` 模块与配置中心的**唯一接线点**：把 `os.default_read_buffer_size`、
 * `os.max_file_size` 静态注册进 `Config::Instance()`，并让 `ReadBufferSize`/
 * `MaxFileSize` 在运行时读取它们。上游是 `go2cpp::config` 的 `[os]` 段与
 * `Config::DumpIni`；下游是 `src/os/file.cpp` 的 `ReadFile`。
 *
 * ## 本文件定义什么
 *
 * - 两个 `GO2CPP_CONFIG_REGISTER` 静态注册项；
 * - `ReadIntConfig`：缺失/类型冲突时回退默认值；
 * - `ReadBufferSize`/`MaxFileSize`：带安全钳制的读取入口；
 * - `SaveDefaultConfig`：转调 `Config::SaveToFile` 的原子导出。
 *
 * ## 阅读建议
 *
 * 先看文件头「为什么放在单独文件」（不链接 config 也能只用文件 API），再看
 * `ReadBufferSize` 的 4 KiB..16 MiB 钳制，最后看 `SaveDefaultConfig` 为何不自己
 * 写 INI。
 *
 * ## 边界与陷阱
 *
 * - 静态注册发生在 `main` 之前；未链接本翻译单元时 `[os]` 段不会出现；
 * - `MaxFileSize` 的 0 表示不限制，负数被归一为 0；
 * - 配置值是 `int`，超大上限会先被 `std::clamp` 钳制。
 */
#include "go2cpp/os/os.hpp"
#include "go2cpp/config/config.hpp"

#include <algorithm>

// 静态注册：模块名 "os" 会与 key 拼成 "os.default_read_buffer_size"。
GO2CPP_CONFIG_REGISTER("os", int, default_read_buffer_size, 65536,
                       "ReadFile 每次 read 的缓冲区字节数");
GO2CPP_CONFIG_REGISTER("os", int, max_file_size, 268435456,
                       "ReadFile 允许读取的单文件大小上限（字节），超过返回错误");

namespace go2cpp::os {

namespace {

/// 取一个 int 配置项；缺失或类型不符时回退到给定默认值。
// 配置读失败（缺失/类型冲突）时静默回退：os 模块不应因 config 未初始化而不可用。
int ReadIntConfig(const char* name, int fallback) {
    auto variable =
        go2cpp::config::Config::Instance().Lookup<int>(name, fallback, "");
    if (!variable) {
        return fallback;
    }
    return variable->GetValue();
}

// 缓冲区下限：再小会让 read 调用次数暴涨；上限防止配错成几个 G。
constexpr int kMinReadBuffer = 4096;
constexpr int kMaxReadBuffer = 16 * 1024 * 1024;

}  // namespace

// 钳制下限避免 read 次数暴涨，上限避免配错成几个 G 的缓冲区。
std::size_t ReadBufferSize() {
    const int configured = ReadIntConfig("os.default_read_buffer_size", 65536);
    const int clamped = std::clamp(configured, kMinReadBuffer, kMaxReadBuffer);
    return static_cast<std::size_t>(clamped);
}

// 0/负数都归一为「不限制」，与配置文档一致。
std::size_t MaxFileSize() {
    const int configured = ReadIntConfig("os.max_file_size", 268435456);
    return configured > 0 ? static_cast<std::size_t>(configured) : 0;
}

bool SaveDefaultConfig(const std::string& path, std::string* error) {
    // 直接复用配置中心的原子导出（临时文件 + rename），不自己写 INI。
    return go2cpp::config::Config::Instance().SaveToFile(path, error);
}

}  // namespace go2cpp::os
