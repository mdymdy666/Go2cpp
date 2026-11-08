# go2cpp-cli：统一命令行入口（tools/cli）。
#
# 设计约束（与任务边界一致）：
#   - 依赖 libc + 自研 JSON 引擎（go2cpp_json）：JSON 的解析器与序列化器在
#     src/json/ 里，不是 header-only，所以 CLI 必须链接 go2cpp_json（它自己
#     再 PUBLIC 链接 go2cpp_error）。除此之外**不链接任何 go2cpp 运行时库**：
#     dump/status/migrate dump 通过 Unix socket 与目标进程通信，replay/migrate
#     通过 fork/exec 调用外部后端，所以新增该 target 不会改变库目标的编译或链接；
#     （doctor/graph 的 G2CD/1 帧编解码来自 header-only 的
#     include/go2cpp/debug/diagnostic_protocol.hpp。）
#   - 不注册测试、不参与 install(EXPORT)；默认 OFF，只有显式
#     -DGO2CPP_WITH_CLI=ON 才会进入构建。
add_executable(go2cpp_cli
    main.cpp
    args.cpp
    formats.cpp
    transport.cpp
    commands.cpp
    help.cpp
    doctor.cpp
    graph.cpp)

set_target_properties(go2cpp_cli PROPERTIES
    OUTPUT_NAME go2cpp-cli
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# 项目头（本 CLI 只用到 include/ 下的转发头，不碰 src/）。
target_include_directories(go2cpp_cli PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

target_compile_definitions(go2cpp_cli PRIVATE
    GO2CPP_CLI_VERSION="${PROJECT_VERSION}")

# doctor 的 --concurrency 用 std::thread；只给 CLI 自己加，不影响库目标。
# go2cpp_json 提供 JSON 解析/序列化实现（含 go2cpp_error）。
target_link_libraries(go2cpp_cli PRIVATE Threads::Threads go2cpp_json)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(go2cpp_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# 仅在该 target 启用时安装 CLI 可执行文件；不加入库的 export 集合。
install(TARGETS go2cpp_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
