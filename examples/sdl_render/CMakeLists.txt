# ---------------------------------------------------------------------------
# sdl_render：真实 SDL 窗口 + 纹理上传 + SDL_RenderPresent（60Hz vsync）
#             端到端延迟验证（对照 headless 校验和渲染）。
#
# 独立构建（不改主构建文件）：
#   cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
#   cmake --build .build-bento-wsl -j"$(nproc)"
#   cmake -S examples/sdl_render -B .build-sdl-render -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build .build-sdl-render -j"$(nproc)"
#
# 依赖：.build-bento-wsl 的 libgo2cpp_media.a / libgo2cpp_media_bento_core.a /
#       libgo2cpp_p2p.so，以及 X11、Xext、SDL2、Threads。
# 复用 examples/media_e2e/e2e_common.hpp（测试图案、SHA-256、PPM 写出），
# 不修改 media_e2e 与任何核心源码。
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(go2cpp_sdl_render LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(GO2CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "go2cpp 源码根目录")
set(GO2CPP_BUILD_DIR "${GO2CPP_ROOT}/.build-bento-wsl" CACHE PATH "已构建的 go2cpp 构建目录")

if(NOT EXISTS "${GO2CPP_BUILD_DIR}/libgo2cpp_media.a")
    message(FATAL_ERROR
        "缺少 ${GO2CPP_BUILD_DIR}/libgo2cpp_media.a；先构建主工程（-DGO2CPP_WITH_MEDIA=ON）")
endif()

find_package(Threads REQUIRED)
find_package(X11 REQUIRED)
find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2 REQUIRED)
find_library(SDL2_LIBRARY SDL2 REQUIRED)
find_library(GO2CPP_P2P_LIBRARY go2cpp_p2p HINTS "${GO2CPP_BUILD_DIR}" REQUIRED)

function(sdl_render_target target)
    target_include_directories(${target} PRIVATE
        "${GO2CPP_ROOT}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GO2CPP_ROOT}/examples/media_e2e"
        ${SDL2_INCLUDE_DIR})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -O3)
    target_link_libraries(${target} PRIVATE
        "${GO2CPP_BUILD_DIR}/libgo2cpp_media.a"
        "${GO2CPP_BUILD_DIR}/libgo2cpp_media_bento_core.a"
        "${GO2CPP_P2P_LIBRARY}"
        X11::X11
        ${X11_Xext_LIB}
        ${SDL2_LIBRARY}
        Threads::Threads)
    target_link_options(${target} PRIVATE "-Wl,-rpath,${GO2CPP_BUILD_DIR}")
endfunction()

# --- 基准主程序 -------------------------------------------------------------
add_executable(sdl_render_bench sdl_render_bench.cpp)
sdl_render_target(sdl_render_bench)

# --- 采集/渲染专项基准（XShm 零拷贝 + SDL 纹理直传 + 1e5 帧压测）-----------
add_executable(media_capture_render_bench media_capture_render_bench.cpp)
sdl_render_target(media_capture_render_bench)

# --- 真实验证测试（X11 采集往返 + SDL 渲染契约）----------------------------
if(EXISTS "${GO2CPP_ROOT}/tests/media/test_sdl_render.cpp")
    add_executable(sdl_test_render "${GO2CPP_ROOT}/tests/media/test_sdl_render.cpp")
    sdl_render_target(sdl_test_render)
endif()

enable_testing()
add_test(NAME sdl_render_bench
         COMMAND sdl_render_bench --quick --artifacts "${CMAKE_BINARY_DIR}/artifacts")
add_test(NAME media_capture_render_bench
         COMMAND media_capture_render_bench --quick
                 --out "${CMAKE_BINARY_DIR}/media_capture_render_data.md")
if(TARGET sdl_test_render)
    add_test(NAME sdl_render_contract COMMAND sdl_test_render)
endif()
