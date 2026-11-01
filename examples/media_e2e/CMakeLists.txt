# ---------------------------------------------------------------------------
# media_e2e：媒体端到端真实验证（独立构建，不改主构建文件）
#
# 先构建主工程（媒体库与 Bento core 必须已存在）：
#   cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
#   cmake --build .build-bento-wsl -j
#
# 再构建本目录：
#   cmake -S examples/media_e2e -B .build-media-e2e -G Ninja
#   cmake --build .build-media-e2e -j
#
# 依赖：.build-bento-wsl 里的 libgo2cpp_media.a / libgo2cpp_media_bento_core.a /
#       libgo2cpp_p2p.so，以及 X11、Xext、SDL2、Threads。
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(go2cpp_media_e2e LANGUAGES CXX)

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

set(MEDIA_E2E_COMMON_INCLUDES
    "${GO2CPP_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}")

function(media_e2e_target target)
    target_include_directories(${target} PRIVATE ${MEDIA_E2E_COMMON_INCLUDES} ${SDL2_INCLUDE_DIR})
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

# --- 端到端 demo -----------------------------------------------------------
add_executable(media_e2e_demo media_e2e_demo.cpp)
media_e2e_target(media_e2e_demo)

# --- 端到端回归测试（jitter/degradation 接进 pipeline 的断言）--------------
if(EXISTS "${GO2CPP_ROOT}/tests/media/test_media_e2e_jitter.cpp")
    add_executable(media_test_e2e_jitter "${GO2CPP_ROOT}/tests/media/test_media_e2e_jitter.cpp")
    media_e2e_target(media_test_e2e_jitter)
    target_include_directories(media_test_e2e_jitter PRIVATE "${GO2CPP_ROOT}/tests/media")
endif()

enable_testing()
add_test(NAME media_e2e_demo
         COMMAND media_e2e_demo --artifacts "${CMAKE_BINARY_DIR}/artifacts")
if(TARGET media_test_e2e_jitter)
    add_test(NAME media_e2e_jitter COMMAND media_test_e2e_jitter)
endif()
