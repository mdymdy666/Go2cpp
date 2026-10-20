#pragma once

/**
 * @file auto_entry.hpp
 * @brief 编译期注册 fiber 入口，让重放工具自动重跑原程序。
 *
 * 用法（**必须在命名空间作用域**，通常是业务代码的 .cpp 顶层）：
 *
 * ```cpp
 * void MyEntry(std::uint32_t gid, go2cpp::replay::ReplaySource& source) {
 *     go2cpp::replay::ScopedGid guard(gid);
 *     go2cpp::replay::SyncChannel<int> ch(0, source);
 *     // ... 与录制端相同的业务逻辑 ...
 * }
 * RECORD_FIBER_ENTRY(MyEntry);
 * ```
 *
 * 之后 `ReplayFileAuto(path)` 或 CLI `replay play <file>` 就能直接重跑，
 * 不再需要手写 `FiberEntryFactory`。
 *
 * 限制：一个进程只保留**第一个**注册的入口（多场景请用不同的可执行文件 /
 * 或在库层显式调用 `ReplayFileWith`）。
 */

#include "replay/replay_engine.hpp"

#include <cstdint>

namespace go2cpp::replay {

/// 编译期注册的 fiber 入口签名。
using FiberEntry = void (*)(std::uint32_t gid, ReplaySource& source);

/// 注册入口（线程安全；重复注册返回 false）。`name` 仅用于报告。
bool RegisterFiberEntry(FiberEntry entry, const char* name = nullptr);

/// 是否有可用的自动入口。
bool HasFiberEntry();

/// 取入口（没有返回 nullptr）。
FiberEntry GetFiberEntry();

/// 入口名（没有返回空字符串）。
const char* FiberEntryName();

/**
 * @brief 用编译期注册的入口完整重放。
 *
 * 没有注册入口时返回 `status=kNotRun`、`error` 说明原因，而不是崩溃。
 */
ReplayReport ReplayFileAuto(const std::string& path,
                            const ReplayOptions& options = {});

}  // namespace go2cpp::replay

#define G2C_REPLAY_PP_CAT_INNER(a, b) a##b
#define G2C_REPLAY_PP_CAT(a, b) G2C_REPLAY_PP_CAT_INNER(a, b)

/**
 * @brief 注册一个 fiber 入口（一次性）。
 *
 * 宏展开为匿名命名空间里的一个静态 bool，初始化时调用注册函数。因此：
 * - 只能写在命名空间作用域（不能写在函数体内）；
 * - 同一个 TU 内多次使用会生成不同变量名（按行号）。
 */
#define RECORD_FIBER_ENTRY(fn)                                                \
    namespace {                                                               \
    const bool G2C_REPLAY_PP_CAT(g2c_replay_entry_, __LINE__) =               \
        ::go2cpp::replay::RegisterFiberEntry(&fn, #fn);                       \
    }

/// 同 `RECORD_FIBER_ENTRY`，但显式指定报告中显示的入口名。
#define RECORD_FIBER_ENTRY_NAMED(entry_name, fn)                              \
    namespace {                                                               \
    const bool G2C_REPLAY_PP_CAT(g2c_replay_entry_, __LINE__) =               \
        ::go2cpp::replay::RegisterFiberEntry(&fn, entry_name);                \
    }
