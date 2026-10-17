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

std::size_t ReadBufferSize() {
    const int configured = ReadIntConfig("os.default_read_buffer_size", 65536);
    const int clamped = std::clamp(configured, kMinReadBuffer, kMaxReadBuffer);
    return static_cast<std::size_t>(clamped);
}

std::size_t MaxFileSize() {
    const int configured = ReadIntConfig("os.max_file_size", 268435456);
    return configured > 0 ? static_cast<std::size_t>(configured) : 0;
}

bool SaveDefaultConfig(const std::string& path, std::string* error) {
    // 直接复用配置中心的原子导出（临时文件 + rename），不自己写 INI。
    return go2cpp::config::Config::Instance().SaveToFile(path, error);
}

}  // namespace go2cpp::os
