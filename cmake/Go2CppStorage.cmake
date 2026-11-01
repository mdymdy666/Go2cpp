# ---------------------------------------------------------------------------
# SQLite 嵌入式存储层（storage）
#
# 默认 OFF：不编译、不链接、不新增安装面。OFF 时本文件只注册一个 option，
# 因此 `GO2CPP_WITH_SQLITE=OFF` 的构建与没有本文件时完全一致。
#
# 打开方式：
#   cmake -S . -B build -DGO2CPP_WITH_SQLITE=ON
#   cmake --build build -j
#   ctest --test-dir build -R go2cpp_storage
#
# 依赖系统 libsqlite3（Ubuntu: libsqlite3-dev，已装）。优先用 pkg-config
# 拿 include/lib/版本；pkg-config 缺失时回退 find_path/find_library。两者都
# 失败直接 FATAL_ERROR —— 退化成「编译通过但运行时没有存储」比构建失败难查。
# ---------------------------------------------------------------------------
option(GO2CPP_WITH_SQLITE "Build the SQLite embedded storage layer" OFF)

if (NOT GO2CPP_WITH_SQLITE)
    message(STATUS "SQLite storage: OFF (set GO2CPP_WITH_SQLITE=ON to enable)")
    return()
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(GO2CPP_SQLITE3 QUIET sqlite3)
endif()

if (GO2CPP_SQLITE3_FOUND)
    set(GO2CPP_SQLITE3_INCLUDE_DIRS ${GO2CPP_SQLITE3_INCLUDE_DIRS})
    set(GO2CPP_SQLITE3_LIBRARIES ${GO2CPP_SQLITE3_LIBRARIES})
else()
    find_path(GO2CPP_SQLITE3_INCLUDE_DIR sqlite3.h)
    find_library(GO2CPP_SQLITE3_LIBRARY NAMES sqlite3)
    if (NOT GO2CPP_SQLITE3_INCLUDE_DIR OR NOT GO2CPP_SQLITE3_LIBRARY)
        message(FATAL_ERROR
            "GO2CPP_WITH_SQLITE=ON 但找不到系统 libsqlite3。"
            "Ubuntu/Debian 请安装 libsqlite3-dev，"
            "或设置 GO2CPP_SQLITE3_INCLUDE_DIR / GO2CPP_SQLITE3_LIBRARY。")
    endif()
    set(GO2CPP_SQLITE3_INCLUDE_DIRS ${GO2CPP_SQLITE3_INCLUDE_DIR})
    set(GO2CPP_SQLITE3_LIBRARIES ${GO2CPP_SQLITE3_LIBRARY})
endif()

message(STATUS "SQLite storage: ON (sqlite3 ${GO2CPP_SQLITE3_VERSION}, "
               "libs=${GO2CPP_SQLITE3_LIBRARIES})")

# 公共头只前置声明 sqlite3/sqlite3_stmt，不包含 <sqlite3.h>，所以 sqlite3
# 的 include 路径与库都作为 PRIVATE 依赖；使用方需要原生句柄时自己链接。
add_library(go2cpp_storage ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/db.cpp)
add_library(go2cpp::storage ALIAS go2cpp_storage)
target_include_directories(go2cpp_storage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${GO2CPP_SQLITE3_INCLUDE_DIRS})
target_compile_features(go2cpp_storage PUBLIC cxx_std_17)
# 零警告是验收条件：存储层是新增模块，-Werror 把回归挡在构建期。
target_compile_options(go2cpp_storage PRIVATE -Wall -Wextra -Wpedantic -Werror)
# 热路径选项与 Go2CppModules.cmake 对模块库的处理保持一致：
#   -fno-stack-protector：Step/Bind 这类函数含取地址局部变量，GCC 默认
#     -fstack-protector-strong 会给它们插 canary（实测 2–5ns/次）；
#   -fno-semantic-interposition：库内调用不走 PLT/GOT，允许跨 TU 直接调用；
# 共享库构建时再加 -Bsymbolic-functions 把库内引用绑定到库内定义。
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
    CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(go2cpp_storage PRIVATE
        -fno-semantic-interposition -fno-stack-protector)
    if (BUILD_SHARED_LIBS)
        target_link_options(go2cpp_storage PRIVATE
            "LINKER:-Bsymbolic-functions")
    endif()
endif()
target_link_libraries(go2cpp_storage
    PUBLIC Threads::Threads
    PRIVATE ${GO2CPP_SQLITE3_LIBRARIES})
set_target_properties(go2cpp_storage PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)

# 测试与基准：独立开关，不依赖 GO2CPP_BUILD_TESTS，因此 standalone
# （GO2CPP_BENTO_STANDALONE=ON）构建也能只构建存储层与它的测试。
option(GO2CPP_STORAGE_TESTS "Build SQLite storage tests" ON)
if (GO2CPP_STORAGE_TESTS)
    enable_testing()

    add_executable(go2cpp_storage_db_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/test_db.cpp)
    target_link_libraries(go2cpp_storage_db_test PRIVATE go2cpp_storage)
    target_compile_options(go2cpp_storage_db_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME go2cpp_storage_db_test
             COMMAND go2cpp_storage_db_test)
    set_tests_properties(go2cpp_storage_db_test PROPERTIES TIMEOUT 900)

    # 性能对比基准：封装 vs 原生 sqlite3。直接调用 sqlite3_*，所以显式
    # 链接系统库（go2cpp_storage 把 sqlite3 作为 PRIVATE 依赖，不传递）。
    # 不注册 ctest：它是基准而不是回归测试，按需运行。
    add_executable(go2cpp_storage_bench
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/bench_db.cpp)
    target_link_libraries(go2cpp_storage_bench PRIVATE
        go2cpp_storage ${GO2CPP_SQLITE3_LIBRARIES})
    target_include_directories(go2cpp_storage_bench PRIVATE
        ${GO2CPP_SQLITE3_INCLUDE_DIRS})
    target_compile_options(go2cpp_storage_bench PRIVATE
        -Wall -Wextra -Wpedantic)
endif()
