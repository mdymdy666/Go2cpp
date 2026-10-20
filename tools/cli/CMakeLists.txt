# go2cpp-cli：统一命令行入口（tools/cli）。
#
# 设计约束（与任务边界一致）：
#   - 只依赖 libc + 项目自带的 nlohmann/json 单头文件，**不链接任何 go2cpp 运行时库**：
#     dump/status/migrate dump 通过 Unix socket 与目标进程通信，replay/migrate
#     通过 fork/exec 调用外部后端，所以新增该 target 不会改变库目标的编译或链接；
#   - 不注册测试、不参与 install(EXPORT)；默认 OFF，只有显式
#     -DGO2CPP_WITH_CLI=ON 才会进入构建。
add_executable(go2cpp_cli
    main.cpp
    args.cpp
    formats.cpp
    transport.cpp
    commands.cpp
    help.cpp
    doctor.cpp)

set_target_properties(go2cpp_cli PROPERTIES
    OUTPUT_NAME go2cpp-cli
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# 项目头（本 CLI 只用到 include/ 下的转发头，不碰 src/）。
target_include_directories(go2cpp_cli PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

# nlohmann/json 是第三方单头文件；用 SYSTEM 抑制其内部警告，避免 -Werror 误伤。
target_include_directories(go2cpp_cli SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party)

target_compile_definitions(go2cpp_cli PRIVATE
    GO2CPP_CLI_VERSION="${PROJECT_VERSION}")

# doctor 的 --concurrency 用 std::thread；只给 CLI 自己加，不影响库目标。
target_link_libraries(go2cpp_cli PRIVATE Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(go2cpp_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# 仅在该 target 启用时安装 CLI 可执行文件；不加入库的 export 集合。
install(TARGETS go2cpp_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
