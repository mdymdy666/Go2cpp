include(GNUInstallDirs)
find_package(Threads REQUIRED)
set(GO2CPP_NATIVE_CONTEXT OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|riscv|ppc|s390")
    set(GO2CPP_NATIVE_CONTEXT ON)
endif()
if (NOT GO2CPP_NATIVE_CONTEXT)
    find_package(Boost 1.70 REQUIRED COMPONENTS context)
endif()
include(CheckIncludeFileCXX)
check_include_file_cxx(valgrind/valgrind.h GO2CPP_HAVE_VALGRIND_HEADER)

option(BUILD_SHARED_LIBS "Build shared runtime modules" ON)
option(GO2CPP_BUILD_HOOK "Build the Linux syscall interposer" ON)
if (GO2CPP_BUILD_HOOK AND NOT BUILD_SHARED_LIBS)
    message(FATAL_ERROR "The hook requires shared runtime modules to keep one TLS/FD registry; use GO2CPP_BUILD_HOOK=OFF for a static runtime")
endif()

add_library(go2cpp_error ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/error.cpp)
add_library(go2cpp_control_flow ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/control_flow.cpp)
add_library(go2cpp_context ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/context.cpp)
add_library(go2cpp_channel ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/channel.cpp)
add_library(go2cpp_scheduler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/parking_condition.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/timer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/thread_policy.cpp)
add_library(go2cpp_fiber
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/fiber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/fiber_local.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/context_backend.cpp)
if (GO2CPP_NATIVE_CONTEXT)
    target_sources(go2cpp_fiber PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/context_x64.S)
    target_compile_definitions(go2cpp_fiber PRIVATE GO2CPP_USE_NATIVE_CONTEXT=1)
endif()
add_library(go2cpp_sync ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/sync.cpp)
add_library(go2cpp_future ${CMAKE_CURRENT_SOURCE_DIR}/src/concurrency/future.cpp)
add_library(go2cpp_io
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/io_backend.cpp)
add_library(go2cpp_log ${CMAKE_CURRENT_SOURCE_DIR}/src/logging/log.cpp)
add_library(go2cpp_network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/address.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/byte_array.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/fd_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/socket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/http.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/websocket.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network/sse.cpp)
add_library(go2cpp_config ${CMAKE_CURRENT_SOURCE_DIR}/src/config/config.cpp)

# math 模块：Go math 最小子集。绝大多数函数委托 libm；Max/Min 与 Modf 的
# 特殊值语义按 Go 文档自实现（详见 include/go2cpp/math/math.hpp）。
add_library(go2cpp_math ${CMAKE_CURRENT_SOURCE_DIR}/src/math/math.cpp)
# math/rand 模块：xoshiro256** + splitmix64 播种；全局函数用 std::mutex 保护，
# 本地 Rand 实例不加锁（对齐 Go 的顶层函数 / *rand.Rand 划分）。
add_library(go2cpp_rand ${CMAKE_CURRENT_SOURCE_DIR}/src/math/rand.cpp)

# os 模块：文件/目录/环境变量/参数。走 syscall 绕过 Hook（见 os/file.hpp 的边界论证），
# 依赖 config（src/os/config.cpp 用 Config::SaveToFile 导出 app.ini）。
add_library(go2cpp_os
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/dir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/file.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/os.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os/config.cpp)
target_link_libraries(go2cpp_os PUBLIC go2cpp_config)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND GO2CPP_BUILD_HOOK)
    add_library(go2cpp_hook SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/network/hook.cpp)
endif()

# The public umbrella target keeps one stable link name while the module
# targets remain independently inspectable and replaceable by embedders.
add_library(go2cpp_runtime INTERFACE)
add_library(go2cpp::runtime ALIAS go2cpp_runtime)

set_target_properties(go2cpp_error PROPERTIES EXPORT_NAME error)
set_target_properties(go2cpp_control_flow PROPERTIES EXPORT_NAME control_flow)
set_target_properties(go2cpp_context PROPERTIES EXPORT_NAME context)
set_target_properties(go2cpp_channel PROPERTIES EXPORT_NAME channel)
set_target_properties(go2cpp_scheduler PROPERTIES EXPORT_NAME scheduler)
set_target_properties(go2cpp_fiber PROPERTIES EXPORT_NAME fiber)
set_target_properties(go2cpp_sync PROPERTIES EXPORT_NAME sync)
set_target_properties(go2cpp_future PROPERTIES EXPORT_NAME future)
set_target_properties(go2cpp_io PROPERTIES EXPORT_NAME io)
set_target_properties(go2cpp_network PROPERTIES EXPORT_NAME network)
set_target_properties(go2cpp_log PROPERTIES EXPORT_NAME log)
set_target_properties(go2cpp_config PROPERTIES EXPORT_NAME config)
set_target_properties(go2cpp_math PROPERTIES EXPORT_NAME math)
set_target_properties(go2cpp_rand PROPERTIES EXPORT_NAME rand)
if (TARGET go2cpp_hook)
    set_target_properties(go2cpp_hook PROPERTIES EXPORT_NAME hook)
endif()
set_target_properties(go2cpp_runtime PROPERTIES EXPORT_NAME runtime)

# Keep the source-tree and installed package spellings identical. The aliases
# are not a second library or ABI; they only make add_subdirectory consumers
# use the same names as find_package consumers.
add_library(go2cpp::error ALIAS go2cpp_error)
add_library(go2cpp::control_flow ALIAS go2cpp_control_flow)
add_library(go2cpp::context ALIAS go2cpp_context)
add_library(go2cpp::channel ALIAS go2cpp_channel)
add_library(go2cpp::scheduler ALIAS go2cpp_scheduler)
add_library(go2cpp::fiber ALIAS go2cpp_fiber)
add_library(go2cpp::sync ALIAS go2cpp_sync)
add_library(go2cpp::future ALIAS go2cpp_future)
add_library(go2cpp::io ALIAS go2cpp_io)
add_library(go2cpp::network ALIAS go2cpp_network)
add_library(go2cpp::log ALIAS go2cpp_log)
add_library(go2cpp::config ALIAS go2cpp_config)
add_library(go2cpp::os ALIAS go2cpp_os)
add_library(go2cpp::math ALIAS go2cpp_math)
add_library(go2cpp::rand ALIAS go2cpp_rand)
if (TARGET go2cpp_hook)
    add_library(go2cpp::hook ALIAS go2cpp_hook)
endif()

set(GO2CPP_MODULE_TARGETS
    go2cpp_error
    go2cpp_control_flow
    go2cpp_context
    go2cpp_channel
    go2cpp_scheduler
    go2cpp_fiber
    go2cpp_sync
    go2cpp_future
    go2cpp_io
    go2cpp_network
    go2cpp_log
    go2cpp_config
    go2cpp_os
    go2cpp_math
    go2cpp_rand
)
if (TARGET go2cpp_hook)
    list(APPEND GO2CPP_MODULE_TARGETS go2cpp_hook)
endif()

foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
    target_include_directories(${module_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_features(${module_target} PUBLIC cxx_std_17)
    set_target_properties(${module_target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION 0)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(${module_target} PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
    target_compile_options(${module_target} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(${module_target} PUBLIC Threads::Threads)
endforeach()

# 显式链接 libm：std::sqrt/exp/log 等符号在部分工具链/链接方式下不会由
# libstdc++ 传递；Go math 模块依赖它们，不能靠隐式依赖碰运气。
if (UNIX)
    target_link_libraries(go2cpp_math PUBLIC m)
endif()

target_include_directories(go2cpp_runtime INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(go2cpp_context PUBLIC go2cpp_error go2cpp_scheduler)
target_link_libraries(go2cpp_channel PUBLIC go2cpp_context go2cpp_error)
target_link_libraries(go2cpp_scheduler PUBLIC go2cpp_fiber go2cpp_log)
target_link_libraries(go2cpp_config PUBLIC go2cpp_scheduler go2cpp_log)
if (GO2CPP_NATIVE_CONTEXT)
    # Linux x86_64 uses the internal context ABI; it no longer needs the
    # Boost.Context protected-stack implementation at link time.
else()
    target_link_libraries(go2cpp_fiber PUBLIC Boost::context)
    if (GO2CPP_HAVE_VALGRIND_HEADER)
        target_compile_definitions(go2cpp_fiber PRIVATE BOOST_USE_VALGRIND=1)
    endif()
endif()
target_link_libraries(go2cpp_sync PUBLIC go2cpp_context go2cpp_scheduler)
target_link_libraries(go2cpp_future PUBLIC go2cpp_context go2cpp_scheduler)
target_link_libraries(go2cpp_io PUBLIC go2cpp_context go2cpp_scheduler)
target_link_libraries(go2cpp_network PUBLIC go2cpp_io go2cpp_context go2cpp_scheduler go2cpp_sync)
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_hook PUBLIC go2cpp_io go2cpp_fiber
                          go2cpp_config ${CMAKE_DL_LIBS})
endif()
target_link_libraries(go2cpp_runtime INTERFACE
    go2cpp_error go2cpp_control_flow go2cpp_context go2cpp_channel go2cpp_scheduler
    go2cpp_fiber go2cpp_sync go2cpp_future go2cpp_io go2cpp_network go2cpp_log go2cpp_config
    go2cpp_math go2cpp_rand
    Threads::Threads)
if (TARGET go2cpp_hook)
    target_link_libraries(go2cpp_runtime INTERFACE go2cpp_hook)
    # An interposer provides symbols through libc calls, not explicit API
    # references. Keep the DSO even when the executable never names a hook.
    target_link_options(go2cpp_runtime INTERFACE "LINKER:--no-as-needed")
endif()

if (GO2CPP_ENABLE_TSAN)
    foreach(module_target IN LISTS GO2CPP_MODULE_TARGETS)
        target_compile_options(${module_target} PUBLIC
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${module_target} PUBLIC -fsanitize=thread)
    endforeach()
endif()


# ---------------------------------------------------------------------------
# 可选的 picoquic 薄封装
#
# 为什么默认 OFF：picoquic + picotls 是外部依赖（合计约 240 万字节静态库），
# 且头文件路径因机器而异。强制依赖会让每个开发者都要先装它们。
#
# 打开方式：
#   cmake -S . -B build -DGO2CPP_WITH_PICOQUIC=ON \
#         -DPICOQUIC_ROOT=/path/to/picoquic \
#         -DPICOTLS_ROOT=/path/to/picotls
#
# 关着的时候 `picoquic_connection.cpp` 不参与编译；头文件仍在但**不提供实现**
# （使用者会得到链接期 undefined reference —— 这是正确的，不会静默用到空实现）。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_PICOQUIC "Build the picoquic-based QUIC transport" OFF)
set(PICOQUIC_ROOT "" CACHE PATH "picoquic source root (contains picoquic/picoquic.h)")
set(PICOTLS_ROOT "" CACHE PATH "picotls root (contains include/picotls/ and libpicotls-*.a)")

if (GO2CPP_WITH_PICOQUIC)
    if (NOT PICOQUIC_ROOT OR NOT PICOTLS_ROOT)
        message(FATAL_ERROR
            "GO2CPP_WITH_PICOQUIC=ON 需要同时设置 PICOQUIC_ROOT 与 PICOTLS_ROOT")
    endif()

    add_library(go2cpp_quic_picoquic
        ${CMAKE_CURRENT_SOURCE_DIR}/src/network/quic/picoquic_connection.cpp)
    target_include_directories(go2cpp_quic_picoquic
        PRIVATE
        ${PICOQUIC_ROOT}/picoquic
        ${PICOTLS_ROOT}/include)
    target_link_libraries(go2cpp_quic_picoquic
        PUBLIC go2cpp_network
        PRIVATE
        ${PICOQUIC_ROOT}/build/libpicoquic-core.a
        ${PICOQUIC_ROOT}/build/libpicoquic-log.a
        ${PICOTLS_ROOT}/libpicotls-core.a
        ${PICOTLS_ROOT}/libpicotls-openssl.a
        ${PICOTLS_ROOT}/libpicotls-minicrypto.a
        ${PICOTLS_ROOT}/libpicotls-fusion.a
        ssl crypto pthread)

    add_executable(go2cpp_picoquic_hq_client
        ${CMAKE_CURRENT_SOURCE_DIR}/example/network/picoquic_hq_client.cpp)
    target_link_libraries(go2cpp_picoquic_hq_client PRIVATE go2cpp_quic_picoquic)

    message(STATUS "picoquic QUIC transport: ON (picoquic=${PICOQUIC_ROOT})")
else()
    message(STATUS "picoquic QUIC transport: OFF (set GO2CPP_WITH_PICOQUIC=ON to enable)")
endif()
