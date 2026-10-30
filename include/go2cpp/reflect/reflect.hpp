#pragma once

/**
 * @file reflect.hpp
 * @brief Reflect 模块聚合头：类型身份、字段反射、序列化、压缩、格式化、编解码。
 *
 * @details 包含本文件即可使用除 Bento 帧对接外的全部能力；
 *          帧对接见 `go2cpp/reflect/frame_codec.hpp`。
 *
 *          依赖顺序有讲究：
 *          - `type.hpp` 里的 `FieldList` 主模板必须早于 `field.hpp`，否则
 *            `TypeIdOf<T>()` 无法对「只有字段表、没有 TypeTraits」的类型做
 *            SFINAE 回退；
 *          - `serializer_impl.hpp` 必须晚于 `field.hpp`，因为字段宏生成的
 *            lambda 要实例化序列化模板。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */

#include "go2cpp/reflect/buffer.hpp"
#include "go2cpp/reflect/codec.hpp"
#include "go2cpp/reflect/compress.hpp"
#include "go2cpp/reflect/field.hpp"
#include "go2cpp/reflect/json.hpp"
#include "go2cpp/reflect/serializer.hpp"
#include "go2cpp/reflect/serializer_impl.hpp"
#include "go2cpp/reflect/status.hpp"
#include "go2cpp/reflect/type.hpp"
