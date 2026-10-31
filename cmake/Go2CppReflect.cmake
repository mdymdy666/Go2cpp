# ---------------------------------------------------------------------------
# Reflect 序列化模块（独立于 Bento 传输层）——**库定义**
#
# 提供：
#   - TypeId / TypeTraits / TypeRegistry（内置固定 id + 动态注册）；
#   - Serializer<T> 钩子与字段反射（GO2CPP_REFLECT_FIELD）；
#   - 二进制紧凑编码 + JSON-like 文本调试编码；
#   - varint + 字段名去重 + 可选 RLE 压缩；
#   - ReflectCodec 编解码入口（帧对接见 include/go2cpp/reflect/frame_codec.hpp）。
#
# **为什么本文件在 Modules 之前 include**：`go2cpp_config` 的文本编解码
# （`include/go2cpp/config/config_codec.hpp`）直接使用 Reflect 的字段表、
# JSON 解析与 ReflectCodec，因此 `go2cpp_reflect` 目标必须先于
# `go2cpp_config` 定义。测试目标依赖 Bento 可选目标，放在
# `cmake/Go2CppReflectTests.cmake`，在 Bento 之后 include。
#
# 默认 ON。config 现在依赖 Reflect：关闭会让 config 缺实现，因此这里直接
# 报错而不是静默退化（比运行期链接失败/奇怪行为更容易定位）。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_REFLECT "Build the Reflect serialization module" ON)

if (GO2CPP_WITH_REFLECT)
    if (NOT TARGET go2cpp_reflect)
        find_package(Threads REQUIRED)

        add_library(go2cpp_reflect STATIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/reflect/type.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/reflect/json.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/reflect/compress.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/reflect/codec.cpp)

        target_include_directories(go2cpp_reflect
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_compile_features(go2cpp_reflect PUBLIC cxx_std_17)
        # 零警告是验收条件：本模块独立编译，-Werror 把回归挡在构建期。
        target_compile_options(go2cpp_reflect PRIVATE -Wall -Wextra -Wpedantic -Werror)
        set_target_properties(go2cpp_reflect PROPERTIES POSITION_INDEPENDENT_CODE ON)
        add_library(go2cpp::reflect ALIAS go2cpp_reflect)
    endif()

    message(STATUS "Reflect codec: ON")
else()
    message(FATAL_ERROR
        "GO2CPP_WITH_REFLECT=OFF 不再支持：go2cpp_config 的配置编解码依赖 Reflect "
        "（字段反射 + JSON + ReflectCodec）。请保持 ON，或同时移除 config 模块。")
endif()
