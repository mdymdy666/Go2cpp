# ---------------------------------------------------------------------------
# Reflect 序列化模块——**测试目标**
#
# 必须在 `cmake/Go2CppBento.cmake` 之后 include：帧对接测试优先复用
# `go2cpp_bento` 目标；Bento 关闭时退化为只把 Frame/CRC 源文件编进测试目标。
#
# 测试默认 ON，独立于 GO2CPP_BUILD_TESTS，便于在 Bento standalone 构建里
# 只验证 Reflect 与帧对接：
#   cmake -S . -B build-reflect -DGO2CPP_WITH_REFLECT=ON -DGO2CPP_BUILD_TESTS=OFF
# ---------------------------------------------------------------------------
option(GO2CPP_REFLECT_TESTS "Build Reflect tests" ON)

if (GO2CPP_WITH_REFLECT AND GO2CPP_REFLECT_TESTS)
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
