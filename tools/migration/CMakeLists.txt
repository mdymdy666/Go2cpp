# 「可迁移协程」演示工具（v3：safepoint id + 句柄 + 签名；独立构建，不并入主工程）
#
# 编译：
#   cmake -S tools/migration -B build-migration
#   cmake --build build-migration -j
#
# 运行：
#   export MIGRATION_SIGNING_KEY="$(head -c 32 /dev/urandom | xxd -p -c 64)"
#   ./build-migration/migrate dump    /tmp/fiber.mig   # 进程 A
#   ./build-migration/migrate restore /tmp/fiber.mig   # 进程 B
#
# v3 仍不要求 -no-pie / 固定 VA：代码与数据地址以「符号名 + 偏移」写入快照，
# 用户函数内的挂起点以 safepoint id 写入，恢复时按 id 查当前镜像的锚点。
# 为此需要：
#   -fPIE/-pie            允许 ASLR（正是要被验证的场景）
#   -rdynamic             主程序符号进入动态符号表，供 dladdr/dlsym 兜底
#   -fno-omit-frame-pointer / -fno-optimize-sibling-calls
#                         保证 rbp 链完整，dump 时能遍历并重写每一帧返回地址；
#                         ChannelRecvAt 也依赖帧指针用 __builtin_frame_address(0) + 8
#                         记录调用点
#   -fno-stack-protector  挂起帧的 canary 来自生成进程，跨进程必然不匹配
cmake_minimum_required(VERSION 3.20)

project(go2cpp_migration_tools LANGUAGES CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(GO2CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(MIGRATION_COMMON_FLAGS
    -O2 -g -Wall -Wextra
    -fPIE
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls
    -fno-stack-protector)

add_library(migration_core STATIC
    src/format.cpp
    src/handles.cpp
    src/runtime.cpp
    src/safepoints.cpp
    src/sha256.cpp
    src/signature.cpp
    src/symbol_table.cpp
    # 复用 Go2Cpp 的真实上下文切换实现，不另写汇编。
    ${GO2CPP_ROOT}/src/concurrency/context_backend.cpp
    ${GO2CPP_ROOT}/src/concurrency/context_x64.S
)

target_include_directories(migration_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GO2CPP_ROOT}/include
)
target_compile_options(migration_core PRIVATE ${MIGRATION_COMMON_FLAGS})
set_target_properties(migration_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(migrate migrate_main.cpp)
target_link_libraries(migrate PRIVATE migration_core)
target_compile_options(migrate PRIVATE ${MIGRATION_COMMON_FLAGS})
target_link_options(migrate PRIVATE -pie -rdynamic)
set_target_properties(migrate PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 反例二进制：与 migrate 内容不同（build-id 不同），验证快照拒绝跨可执行文件恢复。
add_executable(migrate_alt migrate_main.cpp)
target_link_libraries(migrate_alt PRIVATE migration_core)
target_compile_options(migrate_alt PRIVATE ${MIGRATION_COMMON_FLAGS} -DMIGRATION_ALT_BINARY=1)
target_link_options(migrate_alt PRIVATE -pie -rdynamic)
set_target_properties(migrate_alt PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 反例二进制：启动期把两个不同函数注册成同一个符号名，验证「符号名冲突」拒绝。
add_executable(migrate_conflict migrate_main.cpp tests/symbol_conflict.cpp)
target_link_libraries(migrate_conflict PRIVATE migration_core)
target_compile_options(migrate_conflict PRIVATE ${MIGRATION_COMMON_FLAGS})
target_link_options(migrate_conflict PRIVATE -pie -rdynamic)
set_target_properties(migrate_conflict PROPERTIES POSITION_INDEPENDENT_CODE ON)

# demo 主体 SO：符号名相同、代码布局 / 加载地址不同，用于验证跨版本恢复。
# SO 不链接 migration_core —— 它对 migration::Log / ChannelRecvAt / 符号注册表
# 的引用由主程序在运行期解析（主程序带 -rdynamic）。
function(add_migration_demo target source)
    add_library(${target} SHARED ${source})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${GO2CPP_ROOT}/include)
    target_compile_options(${target} PRIVATE ${MIGRATION_COMMON_FLAGS} -fPIC)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        PREFIX "lib")
endfunction()

# v2 时代的两个版本：函数体一致、仅加载地址不同（保留原有跨版本场景）。
add_migration_demo(migration_demo_v1 demo_lib/demo_body_v1.cpp)
add_migration_demo(migration_demo_v2 demo_lib/demo_body_v2.cpp)

# v3 safepoint 场景：函数体语义一致，但 v2 在挂起点前后插入了无关代码，
# 指令布局不同 —— 恢复依赖 safepoint id 而不是裸偏移。
add_migration_demo(migration_demo_sp_v1 demo_lib/demo_body_sp_v1.cpp)
add_migration_demo(migration_demo_sp_v2 demo_lib/demo_body_sp_v2.cpp)

# lambda 主体：Fiber 入口是一个 noinline 的 lambda，验证 MIGRATION_SYMBOL_FN。
# 关掉 IPA 克隆：.constprop/.isra 克隆会让栈上的实际函数与注册地址不一致。
add_migration_demo(migration_demo_lambda demo_lib/demo_body_lambda.cpp)
target_compile_options(migration_demo_lambda PRIVATE
    -fno-ipa-cp -fno-ipa-cp-clone -fno-ipa-sra -fno-ipa-icf)

# 反例：挂起调用位于匿名静态函数内且未注册，dump 必须给出「需要注册」诊断。
add_migration_demo(migration_demo_unregistered demo_lib/demo_body_unregistered.cpp)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "可迁移协程 demo 只在 Linux x86-64 上验证过")
endif()
