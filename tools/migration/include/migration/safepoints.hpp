/**
 * @file safepoints.hpp
 * @brief 用稳定 id 取代「函数内指令偏移」的挂起点注册表（格式 v3）。
 *
 * ## 为什么需要 safepoint id
 *
 * v2 的栈快照里，返回地址被表达成 `{函数符号, 指令偏移}`。符号只解决
 * 「函数基址随 ASLR/加载地址变化」的问题；一旦新版本在挂起点之前插入或删除
 * 指令，同一个偏移会落到**错误的指令**上，恢复必然崩溃。
 *
 * v3 的做法：在挂起点处用 `MIGRATION_SAFEPOINT(id)` 声明一个**稳定编号**，
 * 快照只记录 id；恢复进程按 id 查自己镜像里的注册表，得到当前构建中该
 * 挂起点的代码地址。函数布局怎么变都不影响 id。
 *
 * ## 注册方式：链接期（section）而不是构造期
 *
 * 宏把一条 `SafepointAnchor` 记录放进名为 `migration_safepoints` 的 ELF 段：
 *
 * ```cpp
 * std::int64_t value = 0;
 * value = migration::ChannelRecvAt(channel, self, kSpRecv1);
 * MIGRATION_SAFEPOINT(kSpRecv1, value);   // 必须紧跟在挂起调用之后
 * ```
 *
 * 记录里的 `address` 不是 C 语言的 `&&label`，而是**内联汇编定义的全局符号**
 * （`g2c_safepoint_<id>`）：汇编器把它放在语句在指令流中的确切位置，链接器
 * 把段里的指针重定位到它。这样即使 `-O2` 重排基本块，锚点也精确落在
 * 「挂起调用之后」这个程序点上（`&&label` 在静态初始化里会被 GCC 折叠到
 * 函数入口，实测不可用，见 tools/migration/README.md「实现注记」）。
 *
 * 恢复进程不需要执行用户代码：`SafepointTable` 扫描**已加载模块的 ELF 文件**
 * （`dl_iterate_phdr` + 段表）即可拿到 `<id, address>` 映射。主程序与 dlopen
 * 的 SO 都支持；这也意味着 SO 不做任何注册调用也能被恢复侧看见。
 *
 * ## 恢复语义
 *
 * 挂起发生在 `ChannelRecvAt` / `ParkAt` 内部，调用链是
 * `用户函数 → ChannelRecvAt → WaitForValue → ParkSelf`。恢复时：
 *
 * 1. 栈上 `ChannelRecvAt` 帧的返回地址槽被改写成锚点地址（当前镜像）；
 * 2. `JumpContext` 从 `ParkSelf` 的保存点恢复，逐帧正常返回；
 * 3. `ChannelRecvAt` 的 `ret` 跳到锚点，寄存器/`rsp` 与「调用正常返回」
 *    完全一致（返回码在 `%rax`）；
 * 4. 锚点后的 `if (SafepointResumePending(id))` 把通道投递的值写回用户变量
 *    （编译器可能把赋值放在锚点之前，因此恢复侧必须显式补一次，见
 *    `MIGRATION_SAFEPOINT` 的 `out` 参数）。
 *
 * ## 约束
 *
 * - 宏必须**紧跟在**挂起调用之后，中间不得插入其它语句；
 * - 同一个进程内 id 必须全局唯一（建议高 16 位区分模块 / 文件）；
 * - 只支持返回 `std::int64_t` 级别的值（demo 通道是 int64）；其它类型需扩展
 *   `SafepointResumeValue` 的载荷表达；
 * - 与 v2 相同，可迁移函数要 `noinline` 且编译时开
 *   `-fno-omit-frame-pointer -fno-optimize-sibling-calls`。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace migration {

/// `migration_safepoints` 段里的一条锚点记录。字段布局写死，扫描侧按 40 字节解释。
struct SafepointAnchor {
    std::uint32_t id;               ///< 稳定编号（进程内唯一，0 保留）
    std::uint32_t flags;            ///< 保留，必须为 0
    const void* address;            ///< 锚点代码地址（链接器重定位填充）
    const char* pretty_function;    ///< `__PRETTY_FUNCTION__`（诊断）
    const char* file;               ///< `__FILE__`
    std::uint32_t line;             ///< `__LINE__`
    std::uint32_t reserved;         ///< 保留
};

static_assert(sizeof(SafepointAnchor) == 40, "SafepointAnchor 布局必须稳定");

/// 运行期查表用的锚点视图（字符串已拷贝）。
struct SafepointInfo {
    std::uint32_t id{0};
    std::uint64_t address{0};
    std::string function;
    std::string file;
    std::uint32_t line{0};
    std::string module;
};

/**
 * @brief 进程级挂起点表（单例）。
 *
 * 首次查询时扫描所有已加载模块；`dlopen` 之后必须调用 `Refresh()`，
 * 因为新模块的 `migration_safepoints` 段是加载后才出现在内存里的。
 */
class SafepointTable {
public:
    static SafepointTable& Instance();

    /// 重新扫描已加载模块（dlopen 之后必须调用）。
    void Refresh();

    /// 按 id 查锚点；不存在返回 nullptr。
    const SafepointInfo* Find(std::uint32_t id) const;

    /// 按地址反查（诊断用）：地址落在某个锚点上时返回该锚点。
    const SafepointInfo* FindByAddress(std::uint64_t address) const;

    /// 返回全部锚点（拷贝，按 id 排序）。
    std::vector<SafepointInfo> Entries() const;

    /// 返回重复 id 的描述（同一 id 出现在多个地址 = 危险配置）。
    std::vector<std::string> DuplicateIds() const;

    /// 返回本进程内已注册的 id 数量。
    std::size_t Size() const;

    /// 扫描失败的模块（文件被删、段表异常等），用于 verify 诊断。
    std::vector<std::string> ScanErrors() const;

    /// 清空（仅测试用）。
    void ClearForTest();

private:
    SafepointTable() = default;
    void EnsureLoaded() const;

    mutable bool m_loaded{false};
    mutable std::vector<SafepointInfo> m_entries;
    mutable std::vector<std::string> m_errors;
};

/// 显式刷新：`dlopen` 之后调用，等价于 `SafepointTable::Instance().Refresh()`。
void RefreshSafepoints();

// ---------------------------------------------------------------------------
// 恢复侧状态：本次 resume 落在哪个挂起点、投递了什么值
// ---------------------------------------------------------------------------

/// 由 `ChannelSend` / `ResumeParkedFiber` 在恢复前调用。
void ArmSafepointResume(std::uint32_t id, std::int64_t value);

/// 当前恢复是否落在 `id` 上；命中时消费掉标记。
bool SafepointResumePending(std::uint32_t id);

/// 本次投递的值（`MIGRATION_SAFEPOINT` 的 `out` 赋值来源）。
std::int64_t SafepointResumeValue();

/// 清除恢复标记（测试 / 异常路径）。
void ClearSafepointResume();

}  // namespace migration

// ---------------------------------------------------------------------------
// 宏
// ---------------------------------------------------------------------------

/// 内部：在当前位置定义汇编锚点并把记录写进 `migration_safepoints` 段。
#define MIGRATION_SP_DETAIL_ANCHOR(id)                                       \
    extern char _migration_sp_address_##id[] __asm__("g2c_safepoint_" #id);  \
    [[maybe_unused]] static const ::migration::SafepointAnchor               \
        _migration_sp_anchor_##id                                            \
            __attribute__((section("migration_safepoints"), used, aligned(8))) = { \
                static_cast<std::uint32_t>(id), 0,                            \
                reinterpret_cast<const void*>(_migration_sp_address_##id),    \
                __PRETTY_FUNCTION__, __FILE__,                                \
                static_cast<std::uint32_t>(__LINE__), 0};                     \
    asm volatile(".local g2c_safepoint_" #id "\n g2c_safepoint_" #id ":\n")

/// 内部：带返回值的恢复补写。
#define MIGRATION_SP_DETAIL_ASSIGN(id, out)                                  \
    if (::migration::SafepointResumePending(static_cast<std::uint32_t>(id))) { \
        (out) = static_cast<decltype(out)>(                                  \
            ::migration::SafepointResumeValue());                            \
    }

/// 内部：无返回值的恢复（消费掉标记即可）。
#define MIGRATION_SP_DETAIL_PLAIN(id)                                        \
    if (::migration::SafepointResumePending(static_cast<std::uint32_t>(id))) { \
    }

#define MIGRATION_SP_DETAIL_SELECT(_1, _2, NAME, ...) NAME

/**
 * @brief 声明一个挂起点。
 *
 * 两种用法：
 * ```cpp
 * std::int64_t value = 0;
 * value = migration::ChannelRecvAt(ch, self, kSpRecv);
 * MIGRATION_SAFEPOINT(kSpRecv, value);   // 有返回值：恢复时补写 value
 *
 * migration::ParkAt(self, kSpPark);
 * MIGRATION_SAFEPOINT(kSpPark);          // 无返回值
 * ```
 *
 * 宏必须紧跟在挂起调用之后。`id` 在同一进程内唯一。
 */
#define MIGRATION_SAFEPOINT(...)                                             \
    MIGRATION_SP_DETAIL_SELECT(__VA_ARGS__, MIGRATION_SAFEPOINT_2,           \
                               MIGRATION_SAFEPOINT_1)(__VA_ARGS__)

#define MIGRATION_SAFEPOINT_1(id)                                            \
    do {                                                                     \
        MIGRATION_SP_DETAIL_ANCHOR(id);                                      \
        MIGRATION_SP_DETAIL_PLAIN(id);                                       \
    } while (0)

#define MIGRATION_SAFEPOINT_2(id, out)                                       \
    do {                                                                     \
        MIGRATION_SP_DETAIL_ANCHOR(id);                                      \
        MIGRATION_SP_DETAIL_ASSIGN(id, out);                                 \
    } while (0)

/**
 * @brief 带挂起点 id 的通道接收：等价于 `ChannelRecv` + `MIGRATION_SAFEPOINT`。
 *
 * 展开为两次调用会让「返回地址槽」落在用户函数里；如果用户把
 * `ChannelRecvAt` 包进自己的 helper，`__builtin_frame_address(0) + 8` 记录到的
 * 就是 helper 的帧，挂起点 id 与恢复落点会错位，因此必须直接展开：
 *
 * ```cpp
 * MIGRATION_RECV(kSpRecv, value, channel, self);
 * ```
 */
#define MIGRATION_RECV(id, out, channel, self)                               \
    do {                                                                     \
        (out) = ::migration::ChannelRecvAt((channel), (self),                \
                                           static_cast<std::uint32_t>(id));  \
        MIGRATION_SAFEPOINT(id, out);                                        \
    } while (0)

/**
 * @brief 带挂起点 id 的 park：由调度方调用 `ResumeParkedFiber` 唤醒。
 *
 * ```cpp
 * MIGRATION_PARK(kSpPark, self);
 * ```
 */
#define MIGRATION_PARK(id, self)                                             \
    do {                                                                     \
        ::migration::ParkAt((self), static_cast<std::uint32_t>(id));         \
        MIGRATION_SAFEPOINT(id);                                             \
    } while (0)
