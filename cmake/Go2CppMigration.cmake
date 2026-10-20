# 跨进程 Fiber/Channel 迁移（实验性，GO2CPP_MIGRATION_API=ON）
#
# 只复用 tools/migration 的**格式层**（G2CMIG03 快照、CRC32、HMAC-SHA256、
# 符号名哈希），不编译它的 runtime/demo —— 那些是独立 CLI 的历史实现。
# 格式层做成不安装的静态库，PRIVATE 链接进 go2cpp_fiber，所以：
#   - 默认构建不编译、不链接；
#   - install(EXPORT) 的模块清单不增加；
#   - tools/migration 的独立构建（53/53 场景）完全不受影响。
add_library(go2cpp_migration_format STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/src/format.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/src/sha256.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/src/signature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/src/symbol_table.cpp)
target_include_directories(go2cpp_migration_format PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/include)
target_compile_options(go2cpp_migration_format PRIVATE -Wall -Wextra)
set_target_properties(go2cpp_migration_format PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(go2cpp_migration_format PUBLIC ${CMAKE_DL_LIBS})

target_include_directories(go2cpp_fiber PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/include)
target_link_libraries(go2cpp_fiber PRIVATE go2cpp_migration_format)
# 挂起帧的 stack canary 来自生成进程，跨进程必然不同。可迁移函数（Fiber
# trampoline / suspend / channel 等待路径）必须关闭栈保护；只影响这个
# 实验性构建，默认构建仍然继承发行版的 -fstack-protector 策略。
target_compile_options(go2cpp_fiber PRIVATE -fno-stack-protector -fno-omit-frame-pointer)
# 挂起栈上可能出现任意 go2cpp 模块的函数帧（例如 Scheduler 的 Task::run
# lambda 的 std::function invoker）。这些帧在恢复后正常返回时会检查由生成
# 进程写入的 canary，跨进程必然失败，因此迁移构建统一关闭栈保护。
foreach(migration_module IN LISTS GO2CPP_MODULE_TARGETS)
    if (TARGET ${migration_module})
        target_compile_options(${migration_module} PRIVATE -fno-stack-protector)
    endif()
endforeach()

# 跨进程 demo：同一可执行文件的 dump / restore / selftest 三个子命令。
# 不注册 ctest 用例，不改变默认构建的 116 项计数。
add_executable(go2cpp_fiber_adopt_demo
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/adopt_demo.cpp)
target_link_libraries(go2cpp_fiber_adopt_demo PRIVATE go2cpp_runtime)
target_compile_options(go2cpp_fiber_adopt_demo PRIVATE
    -Wall -Wextra -fno-stack-protector -fno-omit-frame-pointer)

# 反例二进制：内容不同（build-id 不同），用于验证跨可执行文件的快照拒绝。
add_executable(go2cpp_fiber_adopt_demo_alt
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/migration/adopt_demo.cpp)
target_link_libraries(go2cpp_fiber_adopt_demo_alt PRIVATE go2cpp_runtime)
target_compile_definitions(go2cpp_fiber_adopt_demo_alt PRIVATE
    GO2CPP_ADOPT_DEMO_ALT=1)
target_compile_options(go2cpp_fiber_adopt_demo_alt PRIVATE
    -Wall -Wextra -fno-stack-protector -fno-omit-frame-pointer)
