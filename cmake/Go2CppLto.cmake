# ---------------------------------------------------------------------------
# 链接时优化（LTO / IPO）：GO2CPP_ENABLE_LTO
#
# 为什么需要这个开关
# ------------------
# 实测（/tmp/lto.md）：`go2cpp_math` 的初等函数相对直接调用 libm 慢
# 1.11-1.41×（Sqrt 1.39-1.41×、Exp 1.11×、Log 1.12×、Modf 1.32-1.38×）。
# 实现本身与 libm 完全一致（math.cpp 里就是一行 `return std::exp(x);`），
# 差距全部来自**调用边界**：
#   - 库内部：`std::exp` 被编译成 `jmp exp@plt`，即一次 PLT 间接跳转；
#   - 库外部：应用调用 `go2cpp::math::Exp` 同样经 PLT，且无法内联。
# LTO 把优化推迟到链接期：同一链接单元内的 IR 全部可见，调用点可以被内联。
#
# 关键边界（读之前先看这条）
# --------------------------
# LTO **不能跨越共享库边界**。链接器生成可执行文件时拿不到
# `libgo2cpp_math.so` 的 IR（它已经是 .so 里的机器码），因此：
#   - 默认共享库构建（BUILD_SHARED_LIBS=ON）：LTO 只优化库内部（math 只有
#     一个 TU，无可优化空间），应用 -> 库的 PLT 调用与库内的 `exp@plt`
#     都保持原样，**math 差距不会消失**（实测仍约 1.4×）；
#   - 静态库构建（-DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF）：
#     库与调用方进入同一次 LTO 链接，math 调用被内联，
#     实测回到 0.99-1.04×（< 1.1× 达标）。
# 所以本选项默认 **OFF**：默认构建（共享库）开它没有性能收益，只有构建
# 变慢。需要消除 math/工具函数的跨库调用开销时，用下面的组合：
#     cmake -B build-static-lto \
#           -DGO2CPP_ENABLE_LTO=ON \
#           -DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF
#
# 实现方式
# --------
# 用 CMake 的 IPO 属性（CMP0069 NEW）而不是往 CMAKE_CXX_FLAGS 里塞 -flto：
#   - 编译：-flto=auto -ffat-lto-objects（见下面的覆盖原因）；
#   - 链接：-flto=auto（显式传给链接器驱动，与编译期并行度一致）。
# 用属性而不是全局 flags 的好处：CMake 在探测到工具链不支持 IPO 时会直
# 接报错，不会产出静默退化的二进制。
#
# 关于 -ffat-lto-objects
# ----------------------
# CMake 3.28 对 GCC 的默认 IPO 编译选项是 `-flto=auto -fno-fat-lto-objects`：
# 目标文件**只保留 IR**。这对本仓库的静态库场景不安全——第三方消费者若
# 用普通链接器（不做 LTO）链接 libgo2cpp_math.a，会得到 "plugin needed to
# handle lto object"；nm/objdump/调试器也读不到机器码。这里覆盖成
# `-ffat-lto-objects`：IR 与常规机器码同时保留，LTO 链接时仍走插件并
# 内联，普通链接/归档/调试也不受影响。代价是目标文件体积约翻倍。
#
# `CMAKE_INTERPROCEDURAL_OPTIMIZATION` 变量在 target 创建时作为默认属性
# 读取；本文件在顶层所有 add_library/add_executable **之前** include，
# 因此覆盖全部模块/测试/示例目标。例外：
#   - go2cpp_hook：LD_PRELOAD 拦截依赖 PLT，LTO 可能把被拦截的调用改写为
#     直接调用而绕过 hook，所以该目标在 Go2CppModules.cmake 里显式
#     INTERPROCEDURAL_OPTIMIZATION OFF；
#   - 汇编源（context_x64.S）不参与 LTO，保持原样。
# ---------------------------------------------------------------------------
option(GO2CPP_ENABLE_LTO
       "Enable link-time optimization (GCC -flto=auto / -ffat-lto-objects)" OFF)

if (GO2CPP_ENABLE_LTO)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "GO2CPP_ENABLE_LTO=ON 需要 GCC 或 Clang（当前：${CMAKE_CXX_COMPILER_ID}）")
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(RESULT GO2CPP_IPO_SUPPORTED
                        OUTPUT GO2CPP_IPO_PROBE_OUTPUT LANGUAGES CXX)
    if (NOT GO2CPP_IPO_SUPPORTED)
        message(FATAL_ERROR
            "GO2CPP_ENABLE_LTO=ON 但当前工具链不支持 IPO/LTO：${GO2CPP_IPO_PROBE_OUTPUT}")
    endif()

    # 变量形式（而不是逐目标 set_target_properties）：只在 target 创建时被
    # 读取一次，因此对**此后创建的所有 target** 生效。顶层 include 顺序保证
    # 它先于全部模块/测试/示例目标。
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

    # 恢复 fat object（CMake 默认是 -fno-fat-lto-objects），原因见文件头。
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_CXX_COMPILE_OPTIONS_IPO "-flto=auto" "-ffat-lto-objects")
    endif()

    # GCC 驱动在链接期会自动识别 LTO 目标文件；显式传 -flto=auto 让并行
    # LTO 的线程数与编译期一致。对不含 IR 的目标（如 hook）是无操作。
    add_link_options(-flto=auto)

    # 共享库构建下 LTO 无法跨 DSO 内联：提前说明，避免使用者误以为开了 LTO
    # 就解决了 math 的 1.4×。BUILD_SHARED_LIBS 在 Go2CppModules.cmake 里才
    # option() 定义；此处未定义时按项目的默认值（ON）处理。
    if (BUILD_SHARED_LIBS OR NOT DEFINED BUILD_SHARED_LIBS)
        message(WARNING
            "GO2CPP_ENABLE_LTO=ON 且构建共享库：LTO 只优化库内部，"
            "不能跨 libgo2cpp_*.so 边界内联，应用 -> 库的 PLT 开销保留"
            "（math 仍约 1.4×）。要消除该开销请同时 "
            "-DBUILD_SHARED_LIBS=OFF -DGO2CPP_BUILD_HOOK=OFF。")
    endif()

    message(STATUS
        "LTO: ON (compile -flto=auto -ffat-lto-objects, link -flto=auto)")
endif()
