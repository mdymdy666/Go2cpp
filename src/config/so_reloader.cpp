/**
 * @file src/config/so_reloader.cpp
 * @brief `SoReloader` 的事务实现：revision 栅栏、两阶段提交、失败卸载与保留清单。
 *
 * ## 模块定位
 * `include/go2cpp/config/so_reloader.hpp` 的实现层。它把 `SoManifest` 的变更
 * 映射到既有 `ConfigVarBase::ApplyString/NotifyPending/RollbackPending`，并在
 * 中间调用注入的 loader/unloader/activator；不引入任何 `dlopen` 依赖。
 *
 * ## 本文件定义什么
 * - `Reload` 的九步事务（见头文件）；
 * - 变更集与 A1 依赖一致性检查；
 * - 运行时观测键的暂存/发布/回滚；
 * - 版本路径保留清单与查询接口。
 *
 * ## 阅读建议
 * 先看 `Reload` 的 step 注释，再看 `RollbackStaged`/`UnloadLoaded` 的逆序
 * 语义；状态转化表见 `docs/state-machines/SoReloader.md`。
 *
 * ## 边界与陷阱
 * - 本文件不持有任何 SO 句柄；句柄生命周期由 loader 层管理；
 * - `m_reload_mutex` 只串行化本实例，跨实例的提交顺序由各自的 revision 决定；
 * - `m_highest_seen` 在取锁前抬高，是 E1 的核心；不要把它挪到锁内；
 * - `SoActivator` 已经在 `NotifyPending` 之后，任何失败都无法回滚配置值。
 */
#include "go2cpp/config/so_reloader.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <utility>

namespace go2cpp::config {
namespace {

std::string NowSeconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

// 组成员顺序不构成语义；只有集合变化才触发整组重新加载。
bool SameMembers(const std::vector<std::string>& left,
                 const std::vector<std::string>& right) {
    return std::set<std::string>(left.begin(), left.end()) ==
           std::set<std::string>(right.begin(), right.end());
}

SoReloadResult Failure(std::uint64_t revision, std::string error) {
    SoReloadResult result;
    result.revision = revision;
    result.ok = false;
    result.applied = false;
    result.error = std::move(error);
    return result;
}

}  // namespace

SoReloader::SoReloader(Config& config, SoLoader loader, SoUnloader unloader,
                       SoActivator activator, SoValidationOptions validation)
    : m_config(config),
      m_loader(std::move(loader)),
      m_unloader(std::move(unloader)),
      m_activator(std::move(activator)),
      m_validation(std::move(validation)) {}

void SoReloader::RollbackStaged(
    const std::vector<ConfigVarBase::ptr>& staged) noexcept {
    for (auto it = staged.rbegin(); it != staged.rend(); ++it) {
        try {
            if (*it) (*it)->RollbackPending();
        } catch (...) {
            // ConfigVar 的回滚本身是 noexcept；这里只防御自定义实现，
            // 避免回滚路径把异常传播到 loader 线程。
        }
    }
}

void SoReloader::UnloadLoaded(const SoManifest& manifest,
                              const std::vector<std::string>& loaded) noexcept {
    if (!m_unloader) return;
    for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
        try {
            // 必须用本次 manifest 的 spec：失败事务的 m_current 仍是旧版本，
            // 从 m_current 回查会把旧路径当成新句柄传给卸载回调。
            const auto found = manifest.specs.find(*it);
            if (found != manifest.specs.end()) m_unloader(found->second);
        } catch (...) {
            // 卸载是失败路径的最后一步；单个卸载异常不能阻断其余回滚。
        }
    }
}

bool SoReloader::StageString(const std::string& key, const std::string& value,
                             const std::string& description,
                             std::vector<ConfigVarBase::ptr>* staged,
                             std::string* error) {
    auto variable = m_config.Lookup<std::string>(key, std::string(), description);
    if (!variable) {
        if (error) *error = "运行时配置键类型冲突或注册失败: " + key;
        return false;
    }
    std::string stage_error;
    if (!variable->ApplyString(value, &stage_error)) {
        if (error) *error = "写入 " + key + " 失败: " + stage_error;
        return false;
    }
    if (variable->HasPending() && staged) staged->push_back(variable);
    return true;
}

void SoReloader::AppendRetentionLocked(const std::string& name,
                                       const std::string& path,
                                       std::size_t keep_versions) {
    auto& paths = m_retained[name];
    // 同一个路径重复提交（例如只改了 revision）不重复入清单。
    if (paths.empty() || paths.back() != path) paths.push_back(path);
    const auto keep = std::max<std::size_t>(1, keep_versions);
    while (paths.size() > keep) paths.erase(paths.begin());
}

SoReloadResult SoReloader::Reload(const SoManifest& manifest) {
    // revision 0 不是合法 manifest；必须先拒绝，否则 committed 初值 0 会让
    // 下面的「相等即幂等」分支把它误判成成功。
    if (manifest.revision == 0) {
        return Failure(0, "manifest revision 必须大于 0");
    }

    // step1: 在进入串行区之前抬高 highest_seen。这样正在加载的旧 revision
    //        会在提交前看到更大的值并主动回滚；后到的旧 revision 直接出局。
    auto seen = m_highest_seen.load(std::memory_order_acquire);
    while (seen < manifest.revision &&
           !m_highest_seen.compare_exchange_weak(
               seen, manifest.revision, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }

    // step2: 同一实例的加载/回写串行化，避免两个事务交错 ApplyString。
    std::lock_guard<std::mutex> reload_lock(m_reload_mutex);
    {
        std::lock_guard<std::mutex> state_lock(m_state_mutex);
        if (manifest.revision < m_committed_revision) {
            return Failure(manifest.revision,
                           "revision 回退被拒绝: 当前已提交 " +
                               std::to_string(m_committed_revision) +
                               "，收到 " + std::to_string(manifest.revision));
        }
        if (manifest.revision == m_committed_revision) {
            SoReloadResult result;
            result.ok = true;
            result.applied = false;
            result.revision = manifest.revision;
            return result;
        }
    }
    if (manifest.revision < m_highest_seen.load(std::memory_order_acquire)) {
        return Failure(manifest.revision,
                       "已有更高 revision 进入，丢弃旧事务: " +
                           std::to_string(manifest.revision));
    }

    // step3: 纯静态校验；失败时 loader 一次都不会被调用。
    std::vector<SoDiagnostic> diagnostics;
    std::string validation_error;
    if (!manifest.Validate(m_validation, &diagnostics, &validation_error)) {
        if (validation_error.empty()) validation_error = "manifest 静态校验失败";
        if (!diagnostics.empty() && !diagnostics.front().scope.empty()) {
            validation_error = diagnostics.front().scope + ": " + validation_error;
        }
        return Failure(manifest.revision, validation_error);
    }

    SoManifest current;
    {
        std::lock_guard<std::mutex> state_lock(m_state_mutex);
        current = m_current;
    }

    // 本层不负责退役：已加载的组/SO 从 manifest 消失时直接拒绝，避免静默
    // 把仍在服务中的旧版本从记录里抹掉。
    for (const auto& [group, members] : current.groups) {
        (void)members;
        if (!manifest.groups.count(group)) {
            return Failure(manifest.revision,
                           "本层不支持移除已加载的组: " + group);
        }
    }
    for (const auto& [name, spec] : current.specs) {
        (void)spec;
        if (!manifest.specs.count(name)) {
            return Failure(manifest.revision,
                           "本层不支持移除已加载的 SO: " + name);
        }
    }

    // step4: 计算变更集。组成员变化 → 整组重载；artifact 身份变化 → 单 SO 重载。
    std::set<std::string> changed;
    for (const auto& [group, members] : manifest.groups) {
        const auto old_group = current.groups.find(group);
        const bool membership_changed =
            old_group == current.groups.end() ||
            !SameMembers(old_group->second, members);
        for (const auto& member : members) {
            const auto next = manifest.specs.find(member);
            if (next == manifest.specs.end()) continue;
            const auto old = current.specs.find(member);
            if (membership_changed || old == current.specs.end() ||
                !SameSoArtifact(next->second, old->second)) {
                changed.insert(member);
            }
        }
    }

    // A1: 被依赖者变更时，依赖它的 SO 必须一起变更；否则旧 SO 的 GOT/PLT
    //     仍然指向旧实现，会出现两个版本同时在用。
    for (const auto& changed_name : changed) {
        for (const auto& [name, spec] : manifest.specs) {
            if (changed.count(name)) continue;
            if (std::find(spec.dependencies.begin(), spec.dependencies.end(),
                          changed_name) != spec.dependencies.end()) {
                return Failure(manifest.revision,
                               "依赖组不自洽: " + name + " 依赖已变更的 " +
                                   changed_name + "，但 " + name +
                                   " 未在本次更新中升级");
            }
        }
    }

    std::vector<std::string> changed_names(changed.begin(), changed.end());
    std::vector<std::string> order;
    if (!manifest.BuildLoadOrder(changed_names, &order, &validation_error)) {
        return Failure(manifest.revision, validation_error);
    }

    // step5: 按拓扑顺序加载；任一个失败就逆序卸载本次已成功的句柄。
    std::vector<std::string> loaded;
    for (const auto& name : order) {
        if (!m_loader) {
            UnloadLoaded(manifest, loaded);
            return Failure(manifest.revision,
                           "未注入 SoLoader，无法加载 SO: " + name);
        }
        const auto* spec = manifest.Find(name);
        if (!spec) {
            UnloadLoaded(manifest, loaded);
            return Failure(manifest.revision, "manifest 中找不到 SO: " + name);
        }
        std::string load_error;
        bool ok = false;
        try {
            ok = m_loader(*spec, &load_error);
        } catch (const std::exception& exception) {
            load_error = exception.what();
        } catch (...) {
            load_error = "loader 抛出未知异常";
        }
        if (!ok) {
            UnloadLoaded(manifest, loaded);
            if (load_error.empty()) load_error = "loader 返回 false";
            return Failure(manifest.revision,
                           "加载 SO " + name + " 失败: " + load_error);
        }
        loaded.push_back(name);
    }

    // step6: 全部 SO 已加载；现在才进入 ConfigVar 第一阶段。此时监听器还看不到
    //        新值，任何失败都可以完整回滚。
    std::vector<ConfigVarBase::ptr> staged;
    const auto rollback = [&staged, &loaded, &manifest, this]() {
        RollbackStaged(staged);
        UnloadLoaded(manifest, loaded);
    };
    const std::string epoch = NowSeconds();
    for (const auto& name : loaded) {
        const auto* spec = manifest.Find(name);
        if (!spec) {
            rollback();
            return Failure(manifest.revision, "manifest 中找不到 SO: " + name);
        }
        std::string stage_error;
        if (!StageString(RuntimeVersionKey(name), spec->version,
                         "SO " + name + " 当前版本", &staged, &stage_error) ||
            !StageString(RuntimePathKey(name), spec->path,
                         "SO " + name + " 实际加载路径", &staged, &stage_error) ||
            !StageString("so." + name + ".runtime.loaded_sha256", spec->sha256,
                         "SO " + name + " 加载内容哈希", &staged, &stage_error) ||
            !StageString("so." + name + ".runtime.abi_version", spec->abi_version,
                         "SO " + name + " ABI 版本", &staged, &stage_error) ||
            !StageString("so." + name + ".runtime.loaded_at", epoch,
                         "SO " + name + " 加载时间（Unix 秒）", &staged,
                         &stage_error)) {
            rollback();
            return Failure(manifest.revision, stage_error);
        }
    }
    std::string stage_error;
    if (!StageString("so.runtime.revision", std::to_string(manifest.revision),
                     "SO manifest 当前 revision", &staged, &stage_error) ||
        !StageString("so.runtime.last_reload_at", epoch,
                     "最近一次 SO 热更时间（Unix 秒）", &staged, &stage_error)) {
        rollback();
        return Failure(manifest.revision, stage_error);
    }

    // step7: 被更高 revision 抢占时，在通知监听器之前回滚。
    if (manifest.revision < m_highest_seen.load(std::memory_order_acquire)) {
        rollback();
        return Failure(manifest.revision,
                       "提交前发现更高 revision，回滚本次事务: " +
                           std::to_string(manifest.revision));
    }

    // step8: 全部成功后统一发布。监听器只会看到完整的一代。
    for (const auto& variable : staged) {
        if (variable) variable->NotifyPending();
    }
    for (const auto& name : loaded) {
        if (!m_activator) continue;
        const auto* spec = manifest.Find(name);
        if (!spec) continue;
        try {
            // 激活点必须在 NotifyPending 之后；此时配置值已可见，本层无法回滚。
            m_activator(*spec);
        } catch (...) {
            // 契约要求 activator 只做原子指针替换、不失败；隔离异常避免
            // 已经完整提交的事务在最后一步崩溃。
        }
    }

    // step9: 提交快照与保留清单。失败事务永远不会走到这里。
    {
        std::lock_guard<std::mutex> state_lock(m_state_mutex);
        m_current = manifest;
        for (const auto& name : loaded) {
            const auto* spec = manifest.Find(name);
            if (!spec) continue;
            AppendRetentionLocked(name, spec->path, spec->keep_versions);
        }
        m_committed_revision = manifest.revision;
    }

    SoReloadResult result;
    result.ok = true;
    result.applied = true;
    result.revision = manifest.revision;
    result.loaded = std::move(loaded);
    return result;
}

SoManifest SoReloader::CurrentManifest() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_current;
}

std::string SoReloader::CurrentVersion(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    const auto* spec = m_current.Find(name);
    return spec ? spec->version : std::string();
}

std::string SoReloader::CurrentPath(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    const auto* spec = m_current.Find(name);
    return spec ? spec->path : std::string();
}

std::vector<std::string> SoReloader::RetainedPaths(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    const auto it = m_retained.find(name);
    return it == m_retained.end() ? std::vector<std::string>() : it->second;
}

std::uint64_t SoReloader::CommittedRevision() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_committed_revision;
}

std::uint64_t SoReloader::HighestSeenRevision() const {
    return m_highest_seen.load(std::memory_order_acquire);
}

std::string SoReloader::RuntimeVersionKey(const std::string& name) {
    return "so." + name + ".runtime.current_version";
}

std::string SoReloader::RuntimePathKey(const std::string& name) {
    return "so." + name + ".runtime.loaded_path";
}

}  // namespace go2cpp::config
