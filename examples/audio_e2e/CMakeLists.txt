# ---------------------------------------------------------------------------
# audio_e2e：音频端到端演示（独立构建，不改主构建文件）
#
# 先构建主工程（媒体库必须已包含 src/media/audio.cpp）：
#   cmake -S . -B .build-bento-wsl -DGO2CPP_WITH_BENTO=ON -DGO2CPP_WITH_MEDIA=ON
#   cmake --build .build-bento-wsl -j
#
# 再构建本目录：
#   cmake -S examples/audio_e2e -B .build-audio-e2e -G Ninja
#   cmake --build .build-audio-e2e -j
#   ./.build-audio-e2e/audio_e2e_demo --frames 120 --artifacts .build-audio-e2e/artifacts
#
# 依赖：.build-bento-wsl 里的 libgo2cpp_media.a / libgo2cpp_media_bento_core.a /
#       libgo2cpp_p2p.so（media 库的链接依赖），以及 Threads、dl（libopus 是
#       dlopen，不需要开发头）。
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(go2cpp_audio_e2e LANGUAGES CXX)

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

set(AUDIO_E2E_LIBS
    "${GO2CPP_BUILD_DIR}/libgo2cpp_media.a"
    "${GO2CPP_BUILD_DIR}/libgo2cpp_media_bento_core.a"
    Threads::Threads
    ${CMAKE_DL_LIBS})

# media 库在 GO2CPP_WITH_P2P=ON 时链接 go2cpp_p2p；示例同样带上（存在才加）。
find_library(GO2CPP_P2P_LIBRARY go2cpp_p2p HINTS "${GO2CPP_BUILD_DIR}")
if(GO2CPP_P2P_LIBRARY)
    list(APPEND AUDIO_E2E_LIBS "${GO2CPP_P2P_LIBRARY}")
endif()

add_executable(audio_e2e_demo audio_e2e_demo.cpp)
target_include_directories(audio_e2e_demo PRIVATE "${GO2CPP_ROOT}/include")
target_compile_options(audio_e2e_demo PRIVATE -Wall -Wextra -Wpedantic -Werror -O3)
target_link_libraries(audio_e2e_demo PRIVATE ${AUDIO_E2E_LIBS})
target_link_options(audio_e2e_demo PRIVATE "-Wl,-rpath,${GO2CPP_BUILD_DIR}")

enable_testing()
add_test(NAME audio_e2e_demo
         COMMAND audio_e2e_demo --frames 40 --no-udp
                 --artifacts "${CMAKE_BINARY_DIR}/artifacts")
set_tests_properties(audio_e2e_demo PROPERTIES TIMEOUT 120)
