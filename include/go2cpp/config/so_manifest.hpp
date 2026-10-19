/**
 * @file include/go2cpp/config/so_manifest.hpp
 * @brief SO 热重载的 manifest 解析与**静态**校验（不接触 dlopen）。
 *
 * ## 模块定位
 * 位于配置层与加载器层之间：上游是 `SoReloader`（事务）与发布工具；
 * 下游只依赖 `IniFile` 与既有 `hash::Sha256`，**不依赖** `<dlfcn.h>`。
 * 真正的 `dlopen`/`dlsym`/ABI 符号校验由调用方注入的 `SoLoader` 完成。
 *
 * ## 本文件定义什么
 * - `SoSpec`：一个 SO 的 artifact 身份（组、版本化路径、哈希、版本、依赖、
 *   符号前缀、保留策略）；
 * - `SoManifest`：一份 manifest 的完整快照，含 `revision`、组表、SO 表；
 * - `SoValidationOptions`/`SoDiagnostic`：静态校验的输入与逐条诊断；
 * - `SameSoArtifact`：判断两次 manifest 中的同一个 SO 是否需要重新加载。
 *
 * ## 阅读建议
 * 先看 `SoManifest::Parse` 的 INI 结构（`docs/so-reload-design.md` 第 3 节），
 * 再看 `Validate` 的检查顺序，最后看 `BuildLoadOrder` 如何只对变更集排序。
 *
 * ## 边界与陷阱
 * - `Validate` 只校验 manifest 里**声明的** ABI 字符串；`.so` 内
 *   `go2cpp_so_abi_version()` 的真实值必须由 loader 在 `dlopen` 后比较；
 * - 已加载的 SO 从 manifest 中消失不会被自动退役，`SoReloader` 选择直接拒绝；
 * - 依赖必须同组：跨组依赖会让 A1 的「整组切换」无法成立；
 * - 文件哈希是**读取时**的内容哈希；校验后到 `dlopen` 之间仍存在 TOCTOU
 *   窗口，生产环境应保证发布目录对运行进程只读。
 */
#pragma once

#include "go2cpp/config/config.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace go2cpp::config {

/// 当前配置层认识的 ABI 字符串；SO 内导出的同名函数必须返回这个值。
inline constexpr char kSoAbiVersion[] = "go2cpp-abi-v1";
/// 每个 SO 默认保留的版本路径数（含当前版本）。
inline constexpr std::size_t kSoDefaultKeepVersions = 3;

/**
 * @brief 一个 SO 的不可变 artifact 身份与加载参数。
 *
 * ## 定位
 * `SoManifest::specs` 的值类型；由 `SoReloader` 交给注入的 `SoLoader`。
 * 加载器只应读取本结构，不应修改它。
 *
 * ## 生命周期与线程模型
 * 随 `SoManifest` 拷贝/移动；`SoReloader` 在事务期间持有 manifest 的副本，
 * loader 回调执行时该副本有效。本结构本身不提供并发保护。
 *
 * ## 不变量
 * - `name`/`group`/`version`/`abi_version`/`symbol_prefix` 非空（Parse 保证）；
 * - `path` 的文件名包含 `version` token（Validate 保证）；
 * - `dependencies` 已排序、无重复，且都在同一组（Validate 保证）。
 */
struct SoSpec final {
    std::string name;              ///< SO 逻辑名；同时是 ini 节名后缀与 go2cpp_so_name 返回值
    std::string group;             ///< 依赖组；同组 SO 在一个事务里整体提交
    std::string path;              ///< 版本化文件路径；Validate 要求文件存在且 basename 含 version
    std::string sha256;            ///< 文件内容 SHA-256（64 位小写十六进制）
    std::string version;           ///< 版本标识（如 "2"）；必须出现在 path 文件名里
    std::string abi_version{kSoAbiVersion};  ///< manifest 声明 ABI；真实符号值由 loader 校验
    std::string symbol_prefix;     ///< RTLD_LOCAL + 显式 dlsym 的符号前缀；全局唯一
    std::vector<std::string> dependencies;  ///< 同组依赖名；已排序、无重复
    std::size_t keep_versions{kSoDefaultKeepVersions};  ///< 保留版本路径数；仅 SoReloader 使用
    std::string retire_policy{"never"};  ///< never/refcount；本层只解析，不执行 dlclose
    bool candidate{false};         ///< 候选版本标记；本层只透传，不做流量灰度
};

/**
 * @brief 一条静态校验诊断。
 *
 * ## 定位
 * `SoManifest::Validate` 在 `diagnostics` 非空时逐条追加；调用方可以一次性
 * 展示全部问题，而不是只看到第一条。
 */
struct SoDiagnostic final {
    std::string scope;    ///< 出问题的键或 SO 名（如 "so.libfoo.path"）
    std::string message;  ///< 中文诊断；面向运维，不面向最终用户
};

/**
 * @brief 静态校验参数。
 *
 * ## 定位
 * 允许测试关闭文件/哈希检查，或把宿主 ABI 换成测试值；生产环境使用默认值。
 */
struct SoValidationOptions final {
    std::string host_abi_version{kSoAbiVersion};  ///< 宿主期望 ABI；与 manifest 声明值比较
    bool check_files{true};   ///< 是否要求 path 存在且是普通文件
    bool check_hashes{true};  ///< 是否计算文件 SHA-256 并与声明值比较
};

/**
 * @brief 一份 SO manifest 的完整快照：解析、静态校验、拓扑排序。
 *
 * ## 定位
 * `SoReloader` 的输入与 `current_` 快照类型；发布工具可以用
 * `LoadFromFile` + `Validate` 在切流前做预检。它只描述「应该加载什么」，
 * 不持有任何句柄，也不执行加载。
 *
 * ## 生命周期与线程模型
 * 值类型，可拷贝；`Parse`/`LoadFromFile` 在调用方线程同步执行。
 * 不提供内部锁：同一对象被多线程读写时由调用方同步。
 *
 * ## 状态机
 * 无状态；`Parse` 失败不修改输出对象（先在临时对象上构造，成功才 move）。
 *
 * ## 不变量
 * - `groups` 的每个成员都出现在 `specs` 中，且 `specs[name].group` 反向一致；
 * - `Validate` 成功返回 true 后，文件存在、哈希、声明 ABI、依赖环、前缀
 *   唯一性全部通过；
 * - `revision` 必须 > 0；单调性由 `SoReloader` 检查，不在本类。
 */
class SoManifest final {
public:
    SoManifest() = default;

    /**
     * @brief 从已解析的 INI 构造 manifest，不做文件系统/哈希校验。
     *
     * ## 步骤
     * step1. 读 `[so]` 的 revision/abi_version/keep_versions 与 groups/sos 索引；
     * step2. 按 groups 索引读每个 `[so.group.<g>]` 的 members；
     * step3. 按 sos 索引读每个 `[so.<name>]` 的 artifact 字段；
     * step4. 双向检查组与成员的归属，任何不一致都失败；
     * step5. 全部成功后才 move 到输出对象。
     *
     * ## 并发/失败语义
     * 纯函数式解析；失败时 `*manifest` 保持不变，`error` 写中文诊断。
     */
    static bool Parse(const IniFile& ini, SoManifest* manifest,
                      std::string* error = nullptr);

    /** @brief 读取并解析 manifest 文件；失败时不修改输出对象。 */
    static bool LoadFromFile(const std::string& path, SoManifest* manifest,
                             std::string* error = nullptr);

    /**
     * @brief 静态校验：文件、哈希、声明 ABI、依赖图、符号前缀。
     *
     * ## 步骤
     * step1. 全局字段（revision、abi_version、keep_versions、groups/sos 非空）；
     * step2. 组与 SO 的双向归属、名字合法性；
     * step3. 每个 SO 的 version/path 版本 token/sha256 格式；
     * step4. 符号前缀非空且全局唯一（A3）；
     * step5. 依赖存在、非同组拒绝、无自依赖、三色 DFS 无环（A1/A2）；
     * step6. 文件存在且为普通文件（B3）；
     * step7. 计算 SHA-256 并与声明值比较（B2）；
     * step8. 声明 ABI 与 `[so] abi_version`、宿主期望值一致。
     *
     * ## 并发/失败语义
     * 只读文件系统，不写任何状态；`diagnostics` 收集全部问题，`error`
     * 写入第一条，便于调用方快速失败。**不会**调用 `dlopen`。
     */
    bool Validate(const SoValidationOptions& options,
                  std::vector<SoDiagnostic>* diagnostics,
                  std::string* error = nullptr) const;

    /** @brief 使用默认 `SoValidationOptions` 做静态校验。 */
    bool Validate(std::string* error = nullptr) const;

    /** @brief 按名字查找 spec；不存在返回 nullptr。 */
    const SoSpec* Find(const std::string& name) const;

    /**
     * @brief 对给定名字集合做依赖拓扑排序（只考虑集合内部的边）。
     *
     * ## 步骤
     * step1. 去重并校验每个名字都存在；
     * step2. 按名字升序 DFS，先递归依赖，再把自身追加到结果；
     * step3. 若集合内部仍有环（Validate 之后理论上不会），返回失败。
     *
     * ## 并发/失败语义
     * 只读；失败时 `*order` 保持不变。集合外的依赖被视为「已就绪的旧版本」，
     * 不进入结果。
     */
    bool BuildLoadOrder(const std::vector<std::string>& names,
                        std::vector<std::string>* order,
                        std::string* error = nullptr) const;

    /** @brief 计算文件的 SHA-256，输出 64 位小写十六进制。 */
    static bool HashFile(const std::string& path, std::string* hex,
                         std::string* error = nullptr);

    std::uint64_t revision{0};       ///< manifest 单调版本号；SoReloader 用它做幂等与栅栏
    std::string abi_version{kSoAbiVersion};  ///< 本 manifest 的声明 ABI；每个 SoSpec 必须与它一致
    std::size_t keep_versions{kSoDefaultKeepVersions};  ///< 全局默认保留数；SoSpec 可覆盖
    std::map<std::string, std::vector<std::string>> groups;  ///< 组名 → 成员名（Parse 保证双向一致）
    std::map<std::string, SoSpec> specs;  ///< SO 名 → artifact；SoReloader 按名字取用
    std::string source_path;         ///< LoadFromFile 记录的文件路径；仅用于诊断
};

/**
 * @brief 判断两个 spec 是否需要重新加载（artifact 身份是否变化）。
 *
 * ## 定位
 * `SoReloader` 计算变更集时使用。只比较会影响已加载代码的字段：`group`、
 * `path`、`sha256`、`version`、`abi_version`、`symbol_prefix`、排序后的
 * `dependencies`。`keep_versions`/`retire_policy`/`candidate` 变化不触发 `dlopen`。
 *
 * ## 并发/失败语义
 * 纯函数，不分配除字符串比较外的资源；不抛异常（`noexcept` 仅用于声明
 * 本函数不主动失败，标准库分配失败仍可能抛出 —— 调用方在事务失败路径处理）。
 */
bool SameSoArtifact(const SoSpec& left, const SoSpec& right);

}  // namespace go2cpp::config
