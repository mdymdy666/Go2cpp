/**
 * @file compress.cpp
 * @brief `compress` 模块的自包含检查与常量一致性断言。
 *
 * @details 本文件**不产生运行时代码**，作用是让构建系统验证两件事：
 *
 *   1. `go2cpp/compress/compress.hpp` 是自包含的（单独编译能通过，不依赖
 *      任何使用方事先 include 别的头）；
 *   2. `Flate` / `Zlib` / `Gzip` 里 `static constexpr` 的级别别名与
 *      `compress::` 命名空间的常量保持同步（改一处忘另一处会编译失败）。
 *
 *   模块真正有链接期符号的实现分布在 `errors.cpp`（哨兵错误）、
 *   `inflater.cpp`（解压状态）与 `deflater.cpp`（压缩状态）；它们合起来
 *   构成 `go2cpp_compress`，使用方必须链接该 target 才能通过链接。
 */
#include "go2cpp/compress/compress.hpp"

namespace go2cpp::compress {

static_assert(Flate::NoCompression == NoCompression,
              "Flate::NoCompression 必须与 compress::NoCompression 一致");
static_assert(Flate::BestSpeed == BestSpeed,
              "Flate::BestSpeed 必须与 compress::BestSpeed 一致");
static_assert(Flate::BestCompression == BestCompression,
              "Flate::BestCompression 必须与 compress::BestCompression 一致");
static_assert(Flate::DefaultCompression == DefaultCompression,
              "Flate::DefaultCompression 必须与 compress::DefaultCompression 一致");

static_assert(Zlib::NoCompression == NoCompression,
              "Zlib::NoCompression 必须与 compress::NoCompression 一致");
static_assert(Zlib::BestSpeed == BestSpeed,
              "Zlib::BestSpeed 必须与 compress::BestSpeed 一致");
static_assert(Zlib::BestCompression == BestCompression,
              "Zlib::BestCompression 必须与 compress::BestCompression 一致");
static_assert(Zlib::DefaultCompression == DefaultCompression,
              "Zlib::DefaultCompression 必须与 compress::DefaultCompression 一致");

static_assert(Gzip::NoCompression == NoCompression,
              "Gzip::NoCompression 必须与 compress::NoCompression 一致");
static_assert(Gzip::BestSpeed == BestSpeed,
              "Gzip::BestSpeed 必须与 compress::BestSpeed 一致");
static_assert(Gzip::BestCompression == BestCompression,
              "Gzip::BestCompression 必须与 compress::BestCompression 一致");
static_assert(Gzip::DefaultCompression == DefaultCompression,
              "Gzip::DefaultCompression 必须与 compress::DefaultCompression 一致");

static_assert(IsValidLevel(DefaultCompression), "DefaultCompression 必须合法");
static_assert(IsValidLevel(NoCompression), "NoCompression 必须合法");
static_assert(IsValidLevel(BestSpeed), "BestSpeed 必须合法");
static_assert(IsValidLevel(BestCompression), "BestCompression 必须合法");
static_assert(!IsValidLevel(BestCompression + 1), "10 必须非法");
static_assert(!IsValidLevel(DefaultCompression - 1), "-2 未暴露，必须非法");

}  // namespace go2cpp::compress
