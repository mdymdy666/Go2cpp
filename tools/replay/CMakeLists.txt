cmake_minimum_required(VERSION 3.20)

project(go2cpp_replay_tools LANGUAGES CXX)

# 独立工程：**不**接入顶层 CMakeLists，因此默认构建行为完全不变。
# 只有显式 `-DGO2CPP_REPLAY=ON` 时才编译 demo；库与极简工具总是可编译
# （它们不依赖 go2cpp 运行时，CI 可以单独构建做格式单测）。
option(GO2CPP_REPLAY "Build the record/replay demo executables" OFF)

set(GO2CPP_REPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Go2Cpp 工作树根目录（提供 include/ 与 build-work/）")
set(GO2CPP_REPLAY_LIB_DIR "${GO2CPP_REPLAY_ROOT}/build-work" CACHE PATH
    "已构建的 libgo2cpp_*.so 所在目录")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

enable_testing()

add_library(go2cpp_replay STATIC
    src/format.cpp
    src/recorder.cpp
    src/replay_engine.cpp
    src/auto_entry.cpp
    src/split.cpp
    src/timer.cpp)
target_include_directories(go2cpp_replay
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${GO2CPP_REPLAY_ROOT}/include")
target_compile_options(go2cpp_replay PRIVATE -Wall -Wextra -Wpedantic)
# GO2CPP_REPLAY=ON 时本工程链接 go2cpp 运行时，Recorder 可以安装资源
# 事件汇（G2CRPLY v4）。OFF 时是纯格式库，不引用任何运行时符号。
if(GO2CPP_REPLAY)
    target_compile_definitions(go2cpp_replay PRIVATE
        GO2CPP_REPLAY_LINK_RUNTIME=1)
endif()

# 极简工具：静态重演 / 自动完整重放 / 分片。
# GO2CPP_REPLAY=ON 时内置 fanin demo 入口，让 `replay play` 可以直接自测；
# OFF 时仍是零依赖的纯格式工具。
add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE go2cpp_replay)

# 独立格式/分片/注册表单测（不链接 go2cpp 运行时，不影响顶层 ctest）。
add_executable(go2cpp_replay_tests tests/test_replay_tools.cpp)
target_link_libraries(go2cpp_replay_tests PRIVATE go2cpp_replay)
target_compile_options(go2cpp_replay_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME go2cpp_replay_tools COMMAND go2cpp_replay_tests)

if(GO2CPP_REPLAY)
    find_package(Threads REQUIRED)
    find_library(G2C_SCHEDULER NAMES go2cpp_scheduler
        PATHS "${GO2CPP_REPLAY_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(G2C_FIBER NAMES go2cpp_fiber
        PATHS "${GO2CPP_REPLAY_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(G2C_CONTEXT NAMES go2cpp_context
        PATHS "${GO2CPP_REPLAY_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(G2C_ERROR NAMES go2cpp_error
        PATHS "${GO2CPP_REPLAY_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(G2C_LOG NAMES go2cpp_log
        PATHS "${GO2CPP_REPLAY_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(G2C_SYNC NAMES go2cpp_sync
        PATHS "${GO2CPP_REPLAY_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    # 资源事件 sink 的实现位于零依赖的 go2cpp_debug_event
    # （SetResourceReplaySink / LookupResourceAddress）。
    find_library(G2C_DEBUG_EVENT NAMES go2cpp_debug_event
        PATHS "${GO2CPP_REPLAY_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)

    set(G2C_RUNTIME_LIBS
        ${G2C_SCHEDULER} ${G2C_FIBER} ${G2C_CONTEXT}
        ${G2C_ERROR} ${G2C_LOG} ${G2C_SYNC} ${G2C_DEBUG_EVENT}
        Threads::Threads)

    # `replay play` 自带 fanin 入口（仅用于自测/演示）。
    target_sources(replay PRIVATE demo/scenario_entry.cpp)
    target_include_directories(replay PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GO2CPP_REPLAY_ROOT}/include")
    target_link_libraries(replay PRIVATE ${G2C_RUNTIME_LIBS})
    target_compile_definitions(replay PRIVATE GO2CPP_REPLAY_BUILTIN_ENTRY=1)

    set(G2C_DEMO_TARGETS
        go2cpp_replay_record
        go2cpp_replay_replay
        go2cpp_replay_select
        go2cpp_replay_context
        go2cpp_replay_values)

    add_executable(go2cpp_replay_record demo/record_demo.cpp)
    target_include_directories(go2cpp_replay_record PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GO2CPP_REPLAY_ROOT}/include")
    target_link_libraries(go2cpp_replay_record PRIVATE
        go2cpp_replay ${G2C_RUNTIME_LIBS})
    target_compile_options(go2cpp_replay_record PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(go2cpp_replay_replay
        demo/replay_demo.cpp
        demo/scenario_entry.cpp)
    target_include_directories(go2cpp_replay_replay PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GO2CPP_REPLAY_ROOT}/include")
    target_link_libraries(go2cpp_replay_replay PRIVATE
        go2cpp_replay ${G2C_RUNTIME_LIBS})
    target_compile_options(go2cpp_replay_replay PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(go2cpp_replay_select demo/select_demo.cpp)
    target_include_directories(go2cpp_replay_select PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GO2CPP_REPLAY_ROOT}/include")
    target_link_libraries(go2cpp_replay_select PRIVATE
        go2cpp_replay ${G2C_RUNTIME_LIBS})
    target_compile_options(go2cpp_replay_select PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(go2cpp_replay_context demo/context_demo.cpp)
    target_include_directories(go2cpp_replay_context PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GO2CPP_REPLAY_ROOT}/include")
    target_link_libraries(go2cpp_replay_context PRIVATE
        go2cpp_replay ${G2C_RUNTIME_LIBS})
    target_compile_options(go2cpp_replay_context PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(go2cpp_replay_values demo/values_demo.cpp)
    target_include_directories(go2cpp_replay_values PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${GO2CPP_REPLAY_ROOT}/include")
    target_link_libraries(go2cpp_replay_values PRIVATE
        go2cpp_replay ${G2C_RUNTIME_LIBS})
    target_compile_options(go2cpp_replay_values PRIVATE -Wall -Wextra -Wpedantic)

    set_target_properties(${G2C_DEMO_TARGETS} replay PROPERTIES
        BUILD_RPATH "${GO2CPP_REPLAY_LIB_DIR}"
        INSTALL_RPATH "${GO2CPP_REPLAY_LIB_DIR}")
endif()

# GO2CPP_REPLAY=ON 时 go2cpp_replay 引用 go2cpp_debug_event 的 sink 符号
# （资源事件桥），独立单测也必须链接运行时库；放在文件末尾是因为
# G2C_RUNTIME_LIBS 在上面的 if(GO2CPP_REPLAY) 块里才定义。
if(GO2CPP_REPLAY AND TARGET go2cpp_replay_tests)
    target_link_libraries(go2cpp_replay_tests PRIVATE ${G2C_RUNTIME_LIBS})
endif()
