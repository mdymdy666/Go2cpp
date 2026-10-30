# ---------------------------------------------------------------------------
# Bento Transport Protocol v0.1（自研最简 UDP 可靠/不可靠混合协议）
#
# 设计参考 yojimbo 的 reliable 层（包序号/ACK 位图/PTO 重传/分片重组），
# 但代码为独立实现：12B 帧头、32 位 seq、累积 ACK + 32 位 SACK、
# 每通道独立不可靠 seq（DropStale）、dep_seq/BARRIER 因果原语。
#
# 默认 OFF：不影响既有库、测试、安装面。
# 打开方式：
#   cmake -S . -B build-bento -DGO2CPP_WITH_BENTO=ON
# 测试由 GO2CPP_BENTO_TESTS 独立控制（默认 ON），不依赖
# GO2CPP_BUILD_TESTS，因此可以只构建 Bento 与它的 3+1 个测试目标：
#   cmake -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=OFF
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_BENTO "Build the Bento minimal UDP transport" OFF)

# standalone 模式下本文件先于 Go2CppModules.cmake 被 include，需要自己解析
# Threads；完整模式下重复 find_package 是幂等的。
include(GNUInstallDirs)
find_package(Threads REQUIRED)

if (GO2CPP_WITH_BENTO)
    add_library(go2cpp_bento STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/protocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/relation.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/channel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/link.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bento/net/token_pairing.cpp)

    target_include_directories(go2cpp_bento
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(go2cpp_bento PUBLIC cxx_std_17)
    # 零警告是验收条件：本模块独立编译，-Werror 把回归挡在构建期。
    target_compile_options(go2cpp_bento PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(go2cpp_bento PUBLIC Threads::Threads)
    set_target_properties(go2cpp_bento PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(go2cpp::bento ALIAS go2cpp_bento)

    option(GO2CPP_BENTO_TESTS "Build Bento transport tests" ON)
    if (GO2CPP_BENTO_TESTS)
        enable_testing()

        add_executable(bento_test_link_loopback
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_link_loopback.cpp)
        add_executable(bento_test_unreliable_dropstale
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_unreliable_dropstale.cpp)
        add_executable(bento_test_video_fragment
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_video_fragment.cpp)
        add_executable(bento_test_token_pairing
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_token_pairing.cpp)
        add_executable(bento_netem_probe
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/netem_probe.cpp)

        foreach(bento_test_target
                bento_test_link_loopback
                bento_test_unreliable_dropstale
                bento_test_video_fragment
                bento_test_token_pairing
                bento_netem_probe)
            target_include_directories(${bento_test_target} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento)
            target_link_libraries(${bento_test_target} PRIVATE go2cpp_bento)
            target_compile_options(${bento_test_target} PRIVATE
                -Wall -Wextra -Wpedantic -Werror)
        endforeach()

        add_test(NAME bento_link_loopback COMMAND bento_test_link_loopback)
        add_test(NAME bento_unreliable_dropstale COMMAND bento_test_unreliable_dropstale)
        add_test(NAME bento_video_fragment COMMAND bento_test_video_fragment)
        add_test(NAME bento_token_pairing COMMAND bento_test_token_pairing)
        # 用户态弱网代理：无 CAP_NET_ADMIN 时替代 tc netem 的可重复版本。
        # 20% 双向丢包 + 200ms 单向延迟，可靠流要求 100% 按序。
        add_test(NAME bento_weaknet_proxy
            COMMAND bento_netem_probe --port 34568 --messages 200 --payload 256
                    --timeout-ms 40000 --loss 20 --delay-ms 200)
        # netem 需要 root/CAP_NET_ADMIN；脚本在无权限时 exit 77 -> SKIP。
        add_test(NAME bento_netem
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/bento/test_netem.sh
                    $<TARGET_FILE:bento_netem_probe>)
        set_tests_properties(
            bento_link_loopback bento_unreliable_dropstale bento_video_fragment
            bento_token_pairing
            PROPERTIES TIMEOUT 60)
        set_tests_properties(bento_weaknet_proxy PROPERTIES TIMEOUT 120)
        set_tests_properties(bento_netem PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endif()

    message(STATUS "Bento transport: ON (tests=${GO2CPP_BENTO_TESTS})")
else()
    message(STATUS "Bento transport: OFF (set GO2CPP_WITH_BENTO=ON to enable)")
endif()
