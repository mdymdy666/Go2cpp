# ---------------------------------------------------------------------------
# WebRTC 媒体端到端最小闭环（M0 采集+编码 / M1 采集→编码→分片→UDP→解码→渲染）
#
# 新增模块，不修改既有库与测试；默认 OFF，打开方式：
#   cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
#
# 依赖：
#   - go2cpp_bento（VideoChannel 的 1200B 分片/重组，必须已构建）；
#   - X11 + Xext（WSLg 提供，`include/go2cpp/media/capture.hpp` 的 XShm 采集）；
#   - SDL2（可选，窗口渲染；缺失时只用无头渲染器）。
#
# 三者中有缺失时按「能力降级而非静默失败」处理：
#   - Bento 缺失   → 整个模块不构建并打印原因（无 VideoChannel 无法闭环）；
#   - X11 缺失     → 采集层只剩 SyntheticCapture，X11 测试 SKIP；
#   - SDL2 缺失    → 渲染层只剩 HeadlessRenderer，SDL 测试 SKIP。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_MEDIA "Build go2cpp media (X11 capture -> encode -> Bento -> decode -> render)" OFF)

if (GO2CPP_WITH_MEDIA)
    # media 只用到 Bento 的「帧编解码 + VideoChannel 分片/重组」这一条最小链路
    # （protocol/relation/congestion/bandwidth/channel 五个 TU），因此单独组一个
    # 子集目标而不是链接完整的 go2cpp_bento：
    #   ① 不把 link/peer_hub/server 等无关 TU 拖进 media 的构建与链接面；
    #   ② 其他模块并行开发期间（例如正在改 link.cpp），media 仍能独立构建。
    # 头文件仍复用 include/go2cpp/bento/net/*，线格式与 VideoChannel 行为完全一致。
    set(MEDIA_BENTO_CORE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/protocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/relation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/congestion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/bandwidth.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/fec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/channel.cpp)
    foreach(media_bento_source ${MEDIA_BENTO_CORE_SOURCES})
        if (NOT EXISTS ${media_bento_source})
            message(STATUS "Media pipeline: OFF (bento source missing: ${media_bento_source})")
            return()
        endif()
    endforeach()

    find_package(Threads REQUIRED)

    add_library(go2cpp_media_bento_core STATIC ${MEDIA_BENTO_CORE_SOURCES})
    target_include_directories(go2cpp_media_bento_core
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_media_bento_core PUBLIC cxx_std_17)
    target_compile_options(go2cpp_media_bento_core PRIVATE
        -Wall -Wextra -Wpedantic -Werror -O2)
    target_link_libraries(go2cpp_media_bento_core PUBLIC Threads::Threads)
    set_target_properties(go2cpp_media_bento_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # --- X11（采集）---
    find_path(MEDIA_X11_INCLUDE_DIR X11/Xlib.h)
    find_library(MEDIA_X11_LIBRARY X11)
    find_library(MEDIA_XEXT_LIBRARY Xext)
    find_path(MEDIA_XSHM_INCLUDE_DIR X11/extensions/XShm.h)
    if (MEDIA_X11_INCLUDE_DIR AND MEDIA_X11_LIBRARY AND MEDIA_XEXT_LIBRARY AND
        MEDIA_XSHM_INCLUDE_DIR)
        set(MEDIA_HAVE_X11 TRUE)
    else()
        set(MEDIA_HAVE_X11 FALSE)
    endif()

    # --- SDL2（渲染）---
    find_path(MEDIA_SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
    find_library(MEDIA_SDL2_LIBRARY SDL2)
    if (MEDIA_SDL2_INCLUDE_DIR AND MEDIA_SDL2_LIBRARY)
        set(MEDIA_HAVE_SDL2 TRUE)
    else()
        set(MEDIA_HAVE_SDL2 FALSE)
    endif()

    add_library(go2cpp_media STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/pixel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/stats.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/codec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/codec_delta.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/ffmpeg_codec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/capture_synthetic.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/capture_x11.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/render.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media/pipeline.cpp)

    target_include_directories(go2cpp_media
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_include_directories(go2cpp_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/media)
    target_compile_features(go2cpp_media PUBLIC cxx_std_17)
    # 零警告是验收条件：与 Bento 一致，-Werror 把回归挡在构建期。
    target_compile_options(go2cpp_media PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(go2cpp_media PUBLIC go2cpp_media_bento_core Threads::Threads)

    if (MEDIA_HAVE_X11)
        target_include_directories(go2cpp_media PRIVATE ${MEDIA_X11_INCLUDE_DIR})
        target_link_libraries(go2cpp_media PUBLIC ${MEDIA_X11_LIBRARY} ${MEDIA_XEXT_LIBRARY})
        target_compile_definitions(go2cpp_media PUBLIC GO2CPP_MEDIA_HAVE_X11=1)
        message(STATUS "Media pipeline: X11 capture ON (${MEDIA_X11_LIBRARY})")
    else()
        message(STATUS "Media pipeline: X11 capture OFF (headers/libs missing)")
    endif()

    if (MEDIA_HAVE_SDL2)
        target_include_directories(go2cpp_media PRIVATE ${MEDIA_SDL2_INCLUDE_DIR})
        target_link_libraries(go2cpp_media PUBLIC ${MEDIA_SDL2_LIBRARY})
        target_compile_definitions(go2cpp_media PUBLIC GO2CPP_MEDIA_HAVE_SDL2=1)
        message(STATUS "Media pipeline: SDL2 render ON (${MEDIA_SDL2_LIBRARY})")
    else()
        message(STATUS "Media pipeline: SDL2 render OFF (headers/libs missing)")
    endif()

    set_target_properties(go2cpp_media PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::media ALIAS go2cpp_media)

    option(GO2CPP_MEDIA_TESTS "Build go2cpp media tests" ON)
    if (GO2CPP_MEDIA_TESTS)
        enable_testing()

        add_executable(media_test_frame ${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test_frame.cpp)
        add_executable(media_test_codec ${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test_codec.cpp)
        add_executable(media_test_capture ${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test_capture.cpp)
        add_executable(media_test_transport ${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test_transport.cpp)
        add_executable(media_test_render ${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test_render.cpp)
        add_executable(media_test_e2e ${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test_e2e.cpp)
        add_executable(media_test_h264 ${CMAKE_CURRENT_SOURCE_DIR}/tests/media/test_h264.cpp)

        foreach(media_test media_test_frame media_test_codec media_test_capture
                media_test_transport media_test_render media_test_e2e media_test_h264)
            target_include_directories(${media_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/media)
            target_link_libraries(${media_test} PRIVATE go2cpp_media)
            target_compile_options(${media_test} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endforeach()

        add_test(NAME media_frame COMMAND media_test_frame)
        add_test(NAME media_codec COMMAND media_test_codec)
        add_test(NAME media_capture COMMAND media_test_capture)
        add_test(NAME media_transport COMMAND media_test_transport)
        add_test(NAME media_render COMMAND media_test_render)
        add_test(NAME media_e2e COMMAND media_test_e2e)
        add_test(NAME media_h264 COMMAND media_test_h264)
        set_tests_properties(media_frame media_codec media_capture media_transport
            media_render media_e2e media_h264
            PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

        message(STATUS "Media pipeline: tests ON (7 targets)")
    endif()

    message(STATUS "Media pipeline: ON")
else()
    message(STATUS "Media pipeline: OFF (set GO2CPP_WITH_MEDIA=ON to enable)")
endif()
