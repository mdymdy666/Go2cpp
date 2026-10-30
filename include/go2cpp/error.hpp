#pragma once

/**
 * @file error.hpp
 * @brief 公共错误入口：Go 风格错误值 + 带类型链的错误体系。
 *
 * @details 本头文件同时暴露两套互补的错误表示，二者都不改变对方语义：
 *
 * 1. **旧版 Go 风格错误**（`go2cpp/tools/error.hpp`，命名空间 `go2cpp`）：
 *    `go2cpp::Error` / `ErrorPtr` / `NewError` / `Wrap` / `Join` /
 *    `Is` / `As` / `errors::*`。全项目既有模块的返回类型，只承载文本与
 *    原因链；本次增强不改它一行代码。
 *
 * 2. **类型链错误体系**（`go2cpp/error/`，命名空间 `go2cpp::error`）：
 *    - `ErrorCode`：16 种稳定语义码（对齐 gRPC，含 `kWrappedError`）；
 *    - `Error`：在文本与原因之外增加 `TypeName()` / `ParentTypeName()` /
 *      `InheritanceDepth()` / `IsA()` / `Is()` 的类型链能力；
 *    - `StringError` / `ArgumentError` / `IOError` / `TimeoutError` /
 *      `NotFoundError` / `FileNotFoundError` / `PermissionError` /
 *      `WrappedError` / `JoinError`：带真实继承关系的常用错误类型；
 *    - `Registry`：`类型名 -> 工厂/父类型/深度` 反射表 + 动态错误码表；
 *    - `ToLegacy()` / `FromLegacy()`：与旧体系双向适配（`compat.hpp`）。
 *
 * 需要按名字创建错误、按错误码分类、或判断“是不是某种错误”时用第 2 套；
 * 只需要一条文本错误、且要与既有模块签名保持一致时用第 1 套。
 *
 * ## 快速上手
 *
 * ```cpp
 * #include "go2cpp/error.hpp"
 *
 * // 类型链判断：TimeoutError 是 IOError 的后代
 * const auto err = go2cpp::error::MakeError<go2cpp::error::TimeoutError>("read timed out");
 * assert(err->IsA("go2cpp.IOError"));
 * assert(err->Is(*go2cpp::error::MakeError<go2cpp::error::IOError>()));
 *
 * // 按名字创建 + 按码查类型
 * auto& registry = go2cpp::error::Registry::Instance();
 * const auto created = registry.Create("go2cpp.NotFoundError");
 * assert(registry.TypeNameOf(static_cast<std::int32_t>(go2cpp::error::ErrorCode::kNotFound)) ==
 *        "go2cpp.NotFoundError");
 *
 * // 交给旧模块
 * const go2cpp::ErrorPtr legacy = go2cpp::error::ToLegacy(err);
 * ```
 *
 * @note 两套 `ErrorPtr` 不是同一类型：`go2cpp::ErrorPtr` 与
 *       `go2cpp::error::ErrorPtr` 互不隐式转换，跨体系必须显式调用
 *       `ToLegacy()` / `FromLegacy()`，避免“看起来能编译、语义却混了”。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

// 旧版 Go 风格错误（兼容旧版公共头文件路径）；语义与实现保持原样。
#include "go2cpp/tools/error.hpp"

// 类型链错误体系：错误码 → 核心类型 → 注册表 → 旧体系适配。
#include "go2cpp/error/code.hpp"
#include "go2cpp/error/error.hpp"
#include "go2cpp/error/registry.hpp"
#include "go2cpp/error/compat.hpp"
