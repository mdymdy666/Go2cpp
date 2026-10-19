/**
 * @file include/go2cpp/config/so_reloader.hpp
 * @brief 把 SO manifest 变更接入既有 `ConfigVar` 两阶段事务的编排器。
 *
 * ## 模块定位
 * 配置层的**事务编排**，位于 `SoManifest`（静态校验）与加载器层（`dlopen`）
 * 之间。它接收 `Config&`，把每次热更映射成 `ApplyString` → `NotifyPending`
 * （失败时 `RollbackPending`）两阶段流程，并通过注入的回调完成真正的加载、
 * 失败卸载与 vtable 切换。
 *
 * ## 本文件定义什么
 * - `SoLoader`：加载并校验一个新 SO；返回 false 表示本事务失败；
 * - `SoUnloader`：只用于回滚**本事务刚加载**的 SO，绝不用于旧版本；
 * - `SoActivator`：在全部 `NotifyPending` 之后执行 vtable 原子切换；
 * - `SoReloadResult`：成功/幂等/失败、本次加载列表与首条诊断；
 * - `SoReloader`：revision 栅栏、变更集、依赖顺序、保留清单与观测键回写。
 *
 * ## 阅读建议
 * 先看 `SoReloader` 类注释的状态机与不变量，再看 `Reload` 的步骤；实现细节
 * 见 `src/config/so_reloader.cpp`，状态转化表见 `docs/state-machines/SoReloader.md`。
 *
 * ## 边界与陷阱
 * - 本类**不包含** `<dlfcn.h>`，不直接 `dlopen`/`dlclose`；
 * - `SoActivator` 在 `NotifyPending` 之后执行，本层无法回滚这一阶段；回调必须
 *   只做一次原子指针替换，不允许失败；
 * - `SoUnloader` 只会收到本次 `loaded` 列表里的 spec；旧版本句柄永远不经过它；
 * - `Reload` 是阻塞调用，同一实例由内部互斥量串行；并发调用会按 revision 栅栏
 *   决定谁提交；
 * - 已加载的 SO 从 manifest 中消失会直接拒绝；下线旧版本是加载器层的独立流程。
 */
#pragma once

#include "go2cpp/config/so_manifest.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace go2cpp::config {

/**
 * @brief 加载并校验一个 SO；由加载器层注入。
 *
 * 实现必须在 `dlopen` 成功后校验 `go2cpp_so_abi_version()` 与
 * `go2cpp_so_name()`；失败时自行 `dlclose` 刚拿到的句柄。返回 true 表示句柄
 * 已由加载器层登记，随后可能被 `SoUnloader` 回滚或 `SoActivator` 激活。
 *
 * ## 并发/失败语义
 * 在 `SoReloader::Reload` 的串行区内调用；`error` 写中文诊断。不得抛异常
 * （抛异常会被本层按失败处理，但加载器应自己保证句柄不泄漏）。
 */
using SoLoader = std::function<bool(const SoSpec& spec, std::string* error)>;

/**
 * @brief 卸载**本事务刚加载**的 SO；由加载器层注入。
 *
 * 本层只在失败回滚时调用，且只对本次 `SoLoader` 返回 true 的 spec 调用；
 * 顺序与加载顺序相反。旧版本句柄永远不会传给本回调。
 */
using SoUnloader = std::function<void(const SoSpec& spec)>;

/**
 * @brief 在全部 `NotifyPending` 之后切换 vtable；由加载器层注入。
 *
 * 契约：只做一次原子指针替换，不阻塞、不失败；本层捕获并忽略异常。配置值在
 * 此阶段已经对监听器可见，因此本层无法回滚。
 */
using SoActivator = std::function<void(const SoSpec& spec)>;

/**
 * @brief 一次 `Reload` 的结果。
 *
 * ## 不变量
 * - `ok == false` 时 `loaded` 为空、`current_`/retention 未改变；
 * - `applied == false` 只出现在同 revision 幂等命中；
 * - `revision` 始终等于传入 manifest 的 revision。
 */
struct SoReloadResult final {
    bool ok{false};                 ///< 事务是否成功；失败时旧版本仍在服务
    bool applied{false};            ///< 是否真的提交了新 revision（幂等命中为 false）
    std::uint64_t revision{0};      ///< 本次尝试的 manifest revision
    std::vector<std::string> loaded;  ///< 本次新加载并提交的 SO 名（失败时为空）
    std::string error;              ///< 首条中文诊断；成功时为空

    /// 便于 `if (const auto result = reloader.Reload(m))` 使用。
    explicit operator bool() const noexcept { return ok; }
};

/**
 * @brief SO manifest 的两阶段事务编排器。
 *
 * ## 定位
 * 被进程启动路径与 manifest 监听回调使用；向上提供 `Reload` 与运行时版本
 * 查询，向下通过 `SoLoader`/`SoUnloader`/`SoActivator` 接入加载器层。它**组合**
 * 一个 `Config&` 和若干 `ConfigVarBase::ptr`，不继承任何既有配置类型。
 *
 * ## 生命周期与线程模型
 * 由进程在启动时创建，生命周期覆盖所有热更；`Config&` 必须比本对象活得久。
 * `Reload` 可被多线程并发调用：同一实例由 `m_reload_mutex` 串行，`revision`
 * 栅栏保证只有最大 revision 能提交。`CurrentManifest`/`CurrentVersion`/
 * `RetainedPaths` 只读，使用 `m_state_mutex` 保护。
 *
 * ## 状态机
 * Idle → Validated → Staged → Notified → Committed，失败走 RollingBack →
 * Rejected；完整转化表见 `docs/state-machines/SoReloader.md`（触发函数：
 * `Reload`、`SoManifest::Validate`、`ConfigVarBase::NotifyPending`、
 * `ConfigVarBase::RollbackPending`）。
 *
 * ## 不变量
 * - `m_committed_revision` 只增不减；
 * - `m_current` 只在成功提交时整体替换；
 * - `SoUnloader` 只接收本次新加载的 spec，旧版本永不卸载；
 * - `NotifyPending` 一定在 `SoActivator` 之前执行；
 * - 失败事务不修改 `m_current`/`m_retained`/`m_committed_revision`。
 */
class SoReloader final {
public:
    /**
     * @brief 构造编排器。
     * @param config 既有配置注册表；必须比本对象活得久。
     * @param loader 加载并校验一个 SO；为空时所有 `Reload` 失败。
     * @param unloader 失败回滚时卸载本次新加载的 SO；可为空（测试可只观察）。
     * @param activator `NotifyPending` 之后的 vtable 切换；可为空。
     * @param validation 静态校验参数；生产环境用默认值。
     */
    SoReloader(Config& config, SoLoader loader, SoUnloader unloader = {},
               SoActivator activator = {},
               SoValidationOptions validation = SoValidationOptions{});

    SoReloader(const SoReloader&) = delete;
    SoReloader& operator=(const SoReloader&) = delete;

    /**
     * @brief 按 manifest 提交一次 SO 热更事务。
     *
     * ## 步骤
     * step1. 原子抬高 `m_highest_seen`（在取串行锁之前，保证后到的高 revision 能抢占）；
     * step2. 取 `m_reload_mutex`；拒绝 revision 回退/相等/已被更高版本超越；
     * step3. `SoManifest::Validate` 做文件/哈希/ABI/环/前缀静态校验；
     * step4. 计算变更集，检查 A1 依赖一致性，拒绝已加载组/SO 被移除；
     * step5. 对变更集做拓扑排序，逐个调用 `SoLoader`；
     * step6. 全部加载成功后，对运行时观测键调用 `ApplyString`（不通知）；
     * step7. 再次比较 `m_highest_seen`；被更高 revision 抢占则回滚+卸载；
     * step8. 统一 `NotifyPending`，随后对每个新加载 SO 调用 `SoActivator`；
     * step9. 提交 `m_current`、`m_committed_revision` 与保留清单。
     *
     * ## 并发/失败语义
     * 失败路径按「卸载新句柄 → 回滚暂存变量」逆序执行，旧版本完全不参与；
     * 同 revision 幂等返回 `ok=true, applied=false`；`SoActivator` 阶段不可回滚。
     */
    SoReloadResult Reload(const SoManifest& manifest);

    /** @brief 返回当前已提交 manifest 的副本。 */
    SoManifest CurrentManifest() const;

    /** @brief 返回某个 SO 已提交的版本；未加载返回空串。 */
    std::string CurrentVersion(const std::string& name) const;

    /** @brief 返回某个 SO 已提交的路径；未加载返回空串。 */
    std::string CurrentPath(const std::string& name) const;

    /**
     * @brief 返回某个 SO 的版本路径保留清单（最近的在末尾）。
     *
     * 清单包含当前版本，长度不超过该 SO 生效的 `keep_versions`；它只记录路径，
     * 不表示旧文件已经被卸载。未加载过的 SO 返回空。
     */
    std::vector<std::string> RetainedPaths(const std::string& name) const;

    /** @brief 最近一次成功提交的 revision；初始为 0。 */
    std::uint64_t CommittedRevision() const;

    /** @brief 已经见过的最大 revision（含失败/被抢占的事务）。 */
    std::uint64_t HighestSeenRevision() const;

    /** @brief 运行时版本键名：`so.<name>.runtime.current_version`。 */
    static std::string RuntimeVersionKey(const std::string& name);

    /** @brief 运行时路径键名：`so.<name>.runtime.loaded_path`。 */
    static std::string RuntimePathKey(const std::string& name);

private:
    // 暂存变量的回滚栈：只记录 ApplyString 真正产生 pending 的变量。
    void RollbackStaged(const std::vector<ConfigVarBase::ptr>& staged) noexcept;

    // 逆序卸载本次新加载的 SO；使用本次 manifest 的 spec，避免误卸旧版本。
    // 异常被隔离，避免回滚路径二次失败。
    void UnloadLoaded(const SoManifest& manifest,
                      const std::vector<std::string>& loaded) noexcept;

    // 把一个字符串写进 ConfigVar 的第一阶段；失败时写中文诊断。
    bool StageString(const std::string& key, const std::string& value,
                     const std::string& description,
                     std::vector<ConfigVarBase::ptr>* staged,
                     std::string* error);

    // 追加保留路径并按 keep_versions 裁剪；调用方必须持有 m_state_mutex。
    void AppendRetentionLocked(const std::string& name, const std::string& path,
                               std::size_t keep_versions);

    Config& m_config;                 ///< 既有配置注册表；Reload 回写 runtime.* 使用
    SoLoader m_loader;                ///< 加载器回调；Reload 逐个调用，空则事务失败
    SoUnloader m_unloader;            ///< 失败回滚卸载新句柄；绝不接收旧版本 spec
    SoActivator m_activator;          ///< NotifyPending 之后的 vtable 切换；可为空
    SoValidationOptions m_validation;  ///< 静态校验参数；Reload 传给 SoManifest::Validate

    mutable std::mutex m_state_mutex;  ///< 保护 m_current/m_retained/m_committed_revision；查询与提交使用
    std::mutex m_reload_mutex;         ///< 串行化整个 Reload；保证不会交错 ApplyString
    SoManifest m_current;              ///< 最近一次成功提交的 manifest；只在提交阶段整体替换
    std::map<std::string, std::vector<std::string>> m_retained;  ///< SO 名 → 版本路径保留清单（末尾最新）
    std::uint64_t m_committed_revision{0};  ///< 最近成功提交的 revision；只增不减
    std::atomic<std::uint64_t> m_highest_seen{0};  ///< 已见最大 revision；Reload 入口 fetch_max，提交前比较
};

}  // namespace go2cpp::config
