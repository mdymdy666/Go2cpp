# ---------------------------------------------------------------------------
# Reflect 序列化模块（独立于 Bento 传输层）
#
# 提供：
#   - TypeId / TypeTraits / TypeRegistry（内置固定 id + 动态注册）；
#   - Serializer<T> 钩子与字段反射（GO2CPP_REFLECT_FIELD）；
#   - 二进制紧凑编码 + JSON-like 文本调试编码；
#   - varint + 字段名去重 + 可选 RLE 压缩；
#   - ReflectCodec 编解码入口（帧对接见 include/go2cpp/reflect/frame_codec.hpp）。
#
# 默认 ON：模块不依赖 Bento/网络，按头文件独立使用；GO2CPP_WITH_REFLECT=OFF
# 可完全关闭，不影响既有目标。
#
# 测试默认 ON，独立于 GO2CPP_BUILD_TESTS，便于在 Bento standalone 构建里
# 只验证 Reflect 与帧对接：
#   cmake -S . -B build-reflect -DGO2CPP_WITH_REFLECT=ON -DGO2CPP_BUILD_TESTS=OFF
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_REFLECT "Build the Reflect serialization module" ON)

if (GO2CPP_WITH_REFLECT)
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

    option(GO2CPP_REFLECT_TESTS "Build Reflect tests" ON)
    if (GO2CPP_REFLECT_TESTS)
        enable_testing()

        add_executable(go2cpp_reflect_codec_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/reflect/test_reflect_codec.cpp)
        target_compile_options(go2cpp_reflect_codec_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        target_link_libraries(go2cpp_reflect_codec_test PRIVATE
            go2cpp_reflect Threads::Threads)

        # 帧对接测试需要 Bento 通用帧（Frame + CRC32）。完整构建或
        # GO2CPP_WITH_BENTO=ON 时直接复用 go2cpp_bento；否则只把帧实现和
        # CRC 的两个源文件编进测试，不为了一个测试拉起整个 Bento 目标。
        if (TARGET go2cpp_bento)
            target_link_libraries(go2cpp_reflect_codec_test PRIVATE go2cpp_bento)
        else()
            target_sources(go2cpp_reflect_codec_test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/frame.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/crc32.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/src/hash/hash.cpp)
        endif()

        add_test(NAME reflect_codec COMMAND go2cpp_reflect_codec_test)
        set_tests_properties(reflect_codec PROPERTIES TIMEOUT 180)
    endif()

    message(STATUS "Reflect codec: ON (tests=${GO2CPP_REFLECT_TESTS})")
else()
    message(STATUS "Reflect codec: OFF (set GO2CPP_WITH_REFLECT=ON to enable)")
endif()
