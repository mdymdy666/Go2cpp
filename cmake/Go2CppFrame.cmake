# ---------------------------------------------------------------------------
# 通用帧编解码（include/go2cpp/frame/，JinShan REQ-001 的 FrameCodec）
#
# 目标：把「magic/version 定界 + 变长载荷 + 可插拔校验和 + 半包重组」做成与
# 协议无关的机制。RDRL / RDCH 只是两份不同的 FrameSpec，类型与标志的语义
# 全部由上层解释（见 docs/modules/frame-codec.md）。
#
# 默认 ON：只新增一个静态库和一个测试可执行文件，不改变既有目标的编译/链接
# 面，也不进入 install(EXPORT)（头文件随顶层 install(DIRECTORY include/) 安装）。
# 需要关闭时：-DGO2CPP_WITH_FRAME=OFF。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_FRAME "Build the generic frame codec (go2cpp::frame)" ON)

if (NOT GO2CPP_WITH_FRAME)
    return()
endif()

# 默认校验和复用 hash 模块的 CRC-32/IEEE（slicing-by-8），不重复实现一份 CRC。
if (NOT TARGET go2cpp_hash)
    message(FATAL_ERROR
        "GO2CPP_WITH_FRAME 需要 go2cpp_hash 目标（默认 CRC-32 校验和）；"
        "该目标由 cmake/Go2CppModules.cmake 定义，请在全量构建中使用。")
endif()

add_library(go2cpp_frame STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frame/frame_codec.cpp)
add_library(go2cpp::frame ALIAS go2cpp_frame)
target_include_directories(go2cpp_frame PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(go2cpp_frame PUBLIC cxx_std_17)
# 新增目标按零警告维护：本模块全部代码都在本次变更范围内。
target_compile_options(go2cpp_frame PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(go2cpp_frame PUBLIC go2cpp_hash)

if (GO2CPP_BUILD_TESTS)
    add_executable(go2cpp_frame_codec_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame/test_frame_codec.cpp)
    target_link_libraries(go2cpp_frame_codec_test PRIVATE go2cpp_frame)
    target_compile_options(go2cpp_frame_codec_test PRIVATE
        -O2 -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME go2cpp_frame_codec_test COMMAND go2cpp_frame_codec_test)
    set_tests_properties(go2cpp_frame_codec_test PROPERTIES TIMEOUT 900)
endif()
