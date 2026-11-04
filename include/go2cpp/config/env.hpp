/**
 * @file include/go2cpp/config/env.hpp
 * @brief 环境变量作为配置来源的统一读取入口（header-only）。
 *
 * ## 定位
 * 环境变量是配置的**外部来源**之一：`Config::FromEnv` 用它覆盖已注册变量；
 * 底层库与 `os` 原语用它读取进程级环境。本头是模块读取环境变量的**唯一入口**，
 * 模块内不得直接调用 `getenv` —— 否则读取逻辑不可审计、测试无法隔离、
 * 空值与未设置的区别会在各处各写一套。
 *
 * ## 为什么是 header-only
 * `go2cpp_config` 依赖 `go2cpp_scheduler`（见 `cmake/Go2CppModules.cmake`），
 * 因此 scheduler/sync/fiber 这些底层库不能反向链接 config，否则形成循环依赖。
 * 本头只依赖标准库，任何模块都可以安全包含；`Config::FromEnv` 的批量覆盖
 * 能力仍实现在 `src/config/config.cpp`。
 *
 * ## 语义约定（迁移时必须保持）
 * - 返回 `std::optional<std::string>`：`nullopt` 表示**未设置**，空串表示
 *   **已设置为空**。历史代码大量依赖这一区别，不要用 `GetEnvOr` 抹平；
 * - 返回值是拷贝：`getenv` 的缓冲区会被并发 `setenv` 复用，不能长期持有；
 * - 空名与内嵌 NUL 视为未设置（`getenv` 会截断到 NUL，可能读到别的变量，
 *   比返回 `nullopt` 更危险）。
 *
 * ## 线程安全
 * - 只读；`getenv` 自身在 glibc 下不加锁，并发 `setenv` 与 `getenv` 存在数据
 *   竞争（POSIX 未定义）。本头不提供同步，调用方应在启动期读取并缓存，
 *   这也是现有调试开关（`magic static` 缓存一次）的既有约定。
 */
#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace go2cpp::config {

/**
 * @brief 读取环境变量的原始值。
 * @param name [in] 变量名；空名或含内嵌 NUL 视为未设置。借用，不保存。
 * @return `nullopt` = 未设置；有值 = 已设置（可能是空串）。
 *         调用方拿到的是副本，可以长期保存。
 */
inline std::optional<std::string> GetEnv(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string key(name);
    const char* value = ::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

/**
 * @brief 读取环境变量，未设置或为空串时返回默认值。
 * @param name [in] 变量名；空名或含内嵌 NUL 视为未设置。
 * @param fallback [in] 默认值，按值传入并可被移动。
 * @return 环境变量文本；未设置或空串时返回 fallback。
 * @note 只适用于**不区分**「未设置」与「空串」的调用点；需要区分时用 GetEnv。
 */
inline std::string GetEnvOr(std::string_view name, std::string fallback) {
    const auto value = GetEnv(name);
    if (!value.has_value() || value->empty()) {
        return fallback;
    }
    return *value;
}

}  // namespace go2cpp::config
