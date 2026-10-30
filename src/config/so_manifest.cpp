/**
 * @file src/config/so_manifest.cpp
 * @brief SO manifest 的解析、静态校验、哈希与拓扑排序实现。
 *
 * ## 模块定位
 * `include/go2cpp/config/so_manifest.hpp` 的实现层。上游是 `SoReloader` 与
 * 发布预检工具；下游只使用标准库文件系统/字符处理与既有 `hash::Sha256`。
 *
 * ## 本文件定义什么
 * - `Parse`/`LoadFromFile`：把 `[so]`、`[so.group.*]`、`[so.<name>]` 读成值对象；
 * - `Validate`：文件存在、SHA-256、声明 ABI、依赖环、符号前缀冲突；
 * - `BuildLoadOrder`：只对变更集内部做拓扑排序；
 * - `HashFile`：复用 `go2cpp_hash` 的 SHA-256，不重复实现摘要；
 * - `SameSoArtifact`：判断 artifact 身份是否变化。
 *
 * ## 阅读建议
 * 先看 `Parse` 的 INI 索引（为什么必须显式列 groups/sos），再看 `Validate`
 * 的检查顺序，最后看 `BuildLoadOrder` 如何忽略未变更的依赖。
 *
 * ## 边界与陷阱
 * - `IniFile` 不暴露 section 枚举，因此 manifest 必须用 `groups=`/`sos=` 显式列索引；
 * - `Validate` 计算的是**校验时刻**的文件哈希，校验后到 `dlopen` 之间仍有 TOCTOU；
 * - 空 `dependencies=` 是合法的「无依赖」，但列表中出现空项（如 `a,,b`）会失败；
 * - 前缀唯一性是全局的；同一个模块故意拆成多个 SO 时也必须使用不同前缀。
 */
#include "go2cpp/config/so_manifest.hpp"

#include "go2cpp/hash/sha256.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string_view>
#include <system_error>

namespace go2cpp::config {
namespace {

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseU64(const std::string& text, std::uint64_t* value) {
    if (!value) return false;
    const auto clean = Trim(text);
    if (clean.empty()) return false;
    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(clean.data(), clean.data() + clean.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != clean.data() + clean.size()) {
        return false;
    }
    *value = parsed;
    return true;
}

bool ParseSize(const std::string& text, std::size_t* value) {
    std::uint64_t parsed = 0;
    if (!ParseU64(text, &parsed)) return false;
    if (parsed > static_cast<std::uint64_t>(SIZE_MAX)) return false;
    *value = static_cast<std::size_t>(parsed);
    return true;
}

bool ParseBool(const std::string& text, bool* value) {
    if (!value) return false;
    auto clean = Trim(text);
    std::transform(clean.begin(), clean.end(), clean.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (clean == "1" || clean == "true" || clean == "yes" || clean == "on") {
        *value = true;
        return true;
    }
    if (clean == "0" || clean == "false" || clean == "no" || clean == "off") {
        *value = false;
        return true;
    }
    return false;
}

// 空文本表示「空列表」；非空文本中出现空项（a,,b 或尾逗号）视为书写错误。
bool SplitList(const std::string& text, std::vector<std::string>* out,
               std::string* error) {
    if (!out) return false;
    std::vector<std::string> parsed;
    if (Trim(text).empty()) {
        out->clear();
        return true;
    }
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto comma = text.find(',', begin);
        const auto part = Trim(text.substr(
            begin, comma == std::string::npos ? std::string::npos : comma - begin));
        if (part.empty()) {
            if (error) *error = "列表中出现空项: " + text;
            return false;
        }
        parsed.push_back(part);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    *out = std::move(parsed);
    return true;
}

bool IsNameToken(const std::string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool IsHexDigest(const std::string& text) {
    if (text.size() != 64) return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return text;
}

// B3：path 的文件名必须含版本 token，避免固定 libfoo.so 被原地覆盖。
bool VersionedFileName(const std::string& path, const std::string& version) {
    if (path.empty() || version.empty()) return false;
    const auto file = std::filesystem::path(path).filename().string();
    if (file.empty()) return false;
    const std::vector<std::string> tokens = {
        "-v" + version, "_v" + version, "-" + version, "_" + version,
        "." + version};
    for (const auto& token : tokens) {
        std::size_t position = 0;
        while ((position = file.find(token, position)) != std::string::npos) {
            const auto after = position + token.size();
            if (after == file.size()) return true;
            const char next = file[after];
            if (next == '.' || next == '-' || next == '_' || next == '+') {
                return true;
            }
            position += 1;
        }
    }
    return false;
}

std::string HexDigest(const std::vector<std::uint8_t>& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        hex.push_back(kDigits[(byte >> 4U) & 0x0FU]);
        hex.push_back(kDigits[byte & 0x0FU]);
    }
    return hex;
}

}  // namespace

bool SoManifest::Parse(const IniFile& ini, SoManifest* manifest,
                       std::string* error) {
    if (!manifest) {
        if (error) *error = "SoManifest::Parse 的输出对象为空";
        return false;
    }
    const auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };

    SoManifest parsed;
    if (!ini.Has("so", "revision")) {
        return fail("manifest 缺少 [so] revision");
    }
    if (!ParseU64(ini.Get("so", "revision"), &parsed.revision) ||
        parsed.revision == 0) {
        return fail("[so] revision 必须是大于 0 的整数");
    }
    parsed.abi_version = Trim(ini.Get("so", "abi_version", kSoAbiVersion));
    if (parsed.abi_version.empty()) {
        return fail("[so] abi_version 不能为空");
    }
    if (ini.Has("so", "keep_versions")) {
        if (!ParseSize(ini.Get("so", "keep_versions"), &parsed.keep_versions) ||
            parsed.keep_versions == 0) {
            return fail("[so] keep_versions 必须是大于 0 的整数");
        }
    }

    std::string list_error;
    std::vector<std::string> group_names;
    if (!ini.Has("so", "groups") ||
        !SplitList(ini.Get("so", "groups"), &group_names, &list_error)) {
        return fail(list_error.empty() ? "manifest 缺少 [so] groups" : list_error);
    }
    if (group_names.empty()) return fail("[so] groups 不能为空");
    std::set<std::string> seen_groups;
    for (const auto& group : group_names) {
        if (!IsNameToken(group)) {
            return fail("非法组名（只允许字母数字下划线短横线）: " + group);
        }
        if (!seen_groups.insert(group).second) {
            return fail("重复组名: " + group);
        }
    }

    std::vector<std::string> spec_names;
    if (!ini.Has("so", "sos") ||
        !SplitList(ini.Get("so", "sos"), &spec_names, &list_error)) {
        return fail(list_error.empty() ? "manifest 缺少 [so] sos" : list_error);
    }
    if (spec_names.empty()) return fail("[so] sos 不能为空");
    std::set<std::string> seen_specs;
    for (const auto& name : spec_names) {
        if (!IsNameToken(name)) {
            return fail("非法 SO 名（只允许字母数字下划线短横线）: " + name);
        }
        if (!seen_specs.insert(name).second) {
            return fail("重复 SO 名: " + name);
        }
    }

    // step2: 组段只提供 members；成员是否存在留给下面的双向检查。
    for (const auto& group : group_names) {
        const auto section = "so.group." + group;
        if (!ini.Has(section, "members")) {
            return fail("组 [" + section + "] 缺少 members");
        }
        std::vector<std::string> members;
        if (!SplitList(ini.Get(section, "members"), &members, &list_error)) {
            return fail(list_error);
        }
        if (members.empty()) {
            return fail("组 [" + section + "] 的 members 不能为空");
        }
        std::set<std::string> seen_members;
        for (const auto& member : members) {
            if (!seen_specs.count(member)) {
                return fail("组 " + group + " 的成员不在 [so] sos 中: " + member);
            }
            if (!seen_members.insert(member).second) {
                return fail("组 " + group + " 中重复成员: " + member);
            }
        }
        parsed.groups.emplace(group, std::move(members));
    }

    // step3: 每个 SO 一段。所有字段先收集，最后做跨字段校验。
    for (const auto& name : spec_names) {
        const auto section = "so." + name;
        if (!ini.Has(section, "group")) {
            return fail("SO [" + section + "] 缺少 group");
        }
        SoSpec spec;
        spec.name = name;
        // keep_versions 先继承 [so] 的全局值；显式写了 [so.<name>] 才覆盖。
        // 不能依赖 SoSpec 的默认 3，否则全局配置对未写该键的 SO 失效。
        spec.keep_versions = parsed.keep_versions;
        spec.group = Trim(ini.Get(section, "group"));
        if (!seen_groups.count(spec.group)) {
            return fail("SO " + name + " 的 group 未在 [so] groups 中声明: " +
                        spec.group);
        }
        if (!ini.Has(section, "path") || !ini.Has(section, "version") ||
            !ini.Has(section, "sha256") || !ini.Has(section, "symbol_prefix")) {
            return fail("SO [" + section +
                        "] 必须同时提供 path/version/sha256/symbol_prefix");
        }
        spec.path = Trim(ini.Get(section, "path"));
        spec.version = Trim(ini.Get(section, "version"));
        spec.sha256 = Trim(ini.Get(section, "sha256"));
        spec.symbol_prefix = Trim(ini.Get(section, "symbol_prefix"));
        spec.abi_version = Trim(ini.Get(section, "abi_version", parsed.abi_version));
        if (ini.Has(section, "dependencies")) {
            if (!SplitList(ini.Get(section, "dependencies"),
                           &spec.dependencies, &list_error)) {
                return fail(list_error);
            }
            std::sort(spec.dependencies.begin(), spec.dependencies.end());
            if (std::adjacent_find(spec.dependencies.begin(),
                                   spec.dependencies.end()) !=
                spec.dependencies.end()) {
                return fail("SO " + name + " 的 dependencies 有重复项");
            }
        }
        if (ini.Has(section, "keep_versions")) {
            if (!ParseSize(ini.Get(section, "keep_versions"),
                           &spec.keep_versions) ||
                spec.keep_versions == 0) {
                return fail("SO " + name +
                            " 的 keep_versions 必须是大于 0 的整数");
            }
        }
        if (ini.Has(section, "retire_policy")) {
            spec.retire_policy = Trim(ini.Get(section, "retire_policy"));
        }
        if (spec.retire_policy != "never" && spec.retire_policy != "refcount") {
            return fail("SO " + name +
                        " 的 retire_policy 只能是 never 或 refcount");
        }
        if (ini.Has(section, "candidate") &&
            !ParseBool(ini.Get(section, "candidate"), &spec.candidate)) {
            return fail("SO " + name + " 的 candidate 不是合法布尔值");
        }
        parsed.specs.emplace(name, std::move(spec));
    }

    // step4: 组与 SO 必须双向一致，避免「只在 members 里、没有 spec」这类半配置。
    for (const auto& [group, members] : parsed.groups) {
        for (const auto& member : members) {
            const auto it = parsed.specs.find(member);
            if (it == parsed.specs.end()) {
                return fail("组 " + group + " 的成员缺少 SO 段: " + member);
            }
            if (it->second.group != group) {
                return fail("SO " + member + " 的 group=" + it->second.group +
                            " 与组成员关系 " + group + " 不一致");
            }
        }
    }
    for (const auto& [name, spec] : parsed.specs) {
        const auto group_it = parsed.groups.find(spec.group);
        if (group_it == parsed.groups.end()) {
            return fail("SO " + name + " 的 group 未定义: " + spec.group);
        }
        if (std::find(group_it->second.begin(), group_it->second.end(), name) ==
            group_it->second.end()) {
            return fail("SO " + name + " 未列在组 " + spec.group + " 的 members 中");
        }
    }

    // step5: 全部成功后才替换输出，解析失败不会留下半份 manifest。
    *manifest = std::move(parsed);
    return true;
}

bool SoManifest::LoadFromFile(const std::string& path, SoManifest* manifest,
                              std::string* error) {
    if (!manifest) {
        if (error) *error = "SoManifest::LoadFromFile 的输出对象为空";
        return false;
    }
    IniFile ini;
    if (!ini.Load(path, error)) return false;
    SoManifest parsed;
    if (!Parse(ini, &parsed, error)) return false;
    parsed.source_path = path;
    *manifest = std::move(parsed);
    return true;
}

bool SoManifest::Validate(const SoValidationOptions& options,
                          std::vector<SoDiagnostic>* diagnostics,
                          std::string* error) const {
    std::vector<SoDiagnostic> local;
    auto& diags = diagnostics ? *diagnostics : local;
    diags.clear();
    if (error) error->clear();
    const auto note = [&diags, error](std::string scope, std::string message) {
        if (error && error->empty()) *error = message;
        diags.push_back({std::move(scope), std::move(message)});
    };

    // step1: 全局字段。
    if (revision == 0) {
        note("so.revision", "revision 必须大于 0");
    }
    if (abi_version.empty()) {
        note("so.abi_version", "abi_version 不能为空");
    } else if (!options.host_abi_version.empty() &&
               abi_version != options.host_abi_version) {
        note("so.abi_version", "manifest ABI " + abi_version +
                                   " 与宿主期望 " +
                                   options.host_abi_version + " 不一致");
    }
    if (keep_versions == 0) {
        note("so.keep_versions", "keep_versions 必须大于 0");
    }
    if (groups.empty()) note("so.groups", "groups 不能为空");
    if (specs.empty()) note("so.sos", "sos 不能为空");

    // step2: 组与成员的归属。
    std::set<std::string> claimed;
    for (const auto& [group, members] : groups) {
        if (!IsNameToken(group)) {
            note("so.group." + group, "非法组名");
        }
        if (members.empty()) {
            note("so.group." + group, "members 不能为空");
        }
        std::set<std::string> seen;
        for (const auto& member : members) {
            const auto spec_it = specs.find(member);
            if (spec_it == specs.end()) {
                note("so.group." + group,
                     "成员不在 sos 中: " + member);
                continue;
            }
            if (spec_it->second.group != group) {
                note("so." + member + ".group",
                     "spec.group=" + spec_it->second.group + " 与组 " + group +
                         " 不一致");
            }
            if (!seen.insert(member).second) {
                note("so.group." + group, "重复成员: " + member);
            }
            if (!claimed.insert(member).second) {
                note("so." + member + ".group", "同一个 SO 被多个组声明");
            }
        }
    }
    for (const auto& [name, spec] : specs) {
        if (!IsNameToken(name)) {
            note("so." + name, "非法 SO 名");
        }
        if (spec.group.empty()) {
            note("so." + name + ".group", "group 不能为空");
        } else if (!groups.count(spec.group)) {
            note("so." + name + ".group", "group 未定义: " + spec.group);
        }
        if (spec.version.empty()) {
            note("so." + name + ".version", "version 不能为空");
        }
        if (spec.path.empty()) {
            note("so." + name + ".path", "path 不能为空");
        } else if (!spec.version.empty() &&
                   !VersionedFileName(spec.path, spec.version)) {
            note("so." + name + ".path",
                 "path 的文件名必须包含版本 token " + spec.version +
                     "（例如 libfoo-v" + spec.version + ".so）");
        }
        if (spec.abi_version.empty()) {
            note("so." + name + ".abi_version", "abi_version 不能为空");
        } else if (spec.abi_version != abi_version) {
            note("so." + name + ".abi_version",
                 "spec ABI " + spec.abi_version + " 与 [so] abi_version " +
                     abi_version + " 不一致");
        }
        if (spec.keep_versions == 0) {
            note("so." + name + ".keep_versions", "keep_versions 必须大于 0");
        }
        if (spec.retire_policy != "never" && spec.retire_policy != "refcount") {
            note("so." + name + ".retire_policy",
                 "retire_policy 只能是 never 或 refcount");
        }
        if (!IsHexDigest(spec.sha256)) {
            note("so." + name + ".sha256",
                 "sha256 必须是 64 位十六进制字符串");
        }
        // Parse 已经保证双向归属；这里再查一次，防止调用方手工构造出
        // 「spec 存在但不在任何组」的半配置。
        if (!claimed.count(name)) {
            note("so." + name + ".group", "未列在任何组的 members 中");
        }
    }

    // step4: 符号前缀必须全局唯一。
    std::map<std::string, std::string> prefixes;
    for (const auto& [name, spec] : specs) {
        if (spec.symbol_prefix.empty()) {
            note("so." + name + ".symbol_prefix", "symbol_prefix 不能为空");
            continue;
        }
        const auto [it, inserted] = prefixes.emplace(spec.symbol_prefix, name);
        if (!inserted) {
            note("so." + name + ".symbol_prefix",
                 "符号前缀 " + spec.symbol_prefix + " 与 " + it->second +
                     " 冲突（RTLD_GLOBAL 会互相覆盖）");
        }
    }

    // step5: 依赖存在性、同组约束与环。
    for (const auto& [name, spec] : specs) {
        for (const auto& dependency : spec.dependencies) {
            if (dependency == name) {
                note("so." + name + ".dependencies", "不能依赖自身");
                continue;
            }
            const auto it = specs.find(dependency);
            if (it == specs.end()) {
                note("so." + name + ".dependencies",
                     "依赖不存在: " + dependency);
                continue;
            }
            if (it->second.group != spec.group) {
                note("so." + name + ".dependencies",
                     "依赖 " + dependency + " 必须与 " + name + " 同组");
            }
        }
    }
    std::map<std::string, int> colors;
    std::vector<std::string> stack;
    bool cycle_reported = false;
    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        colors[name] = 1;
        stack.push_back(name);
        const auto it = specs.find(name);
        if (it != specs.end()) {
            for (const auto& dependency : it->second.dependencies) {
                if (!specs.count(dependency)) continue;
                if (colors[dependency] == 1) {
                    if (!cycle_reported) {
                        cycle_reported = true;
                        std::string chain;
                        const auto begin = std::find(stack.begin(), stack.end(),
                                                     dependency);
                        for (auto position = begin; position != stack.end();
                             ++position) {
                            if (!chain.empty()) chain += " -> ";
                            chain += *position;
                        }
                        chain += " -> " + dependency;
                        note("so.dependencies", "检测到依赖环: " + chain);
                    }
                } else if (colors[dependency] == 0) {
                    visit(dependency);
                }
            }
        }
        stack.pop_back();
        colors[name] = 2;
    };
    for (const auto& [name, spec] : specs) {
        (void)spec;
        if (colors[name] == 0) visit(name);
    }

    // step6/step7: 文件与内容哈希；B3 的版本化路径已在前面检查。
    for (const auto& [name, spec] : specs) {
        if (spec.path.empty() || !options.check_files) continue;
        std::error_code ec;
        const auto status = std::filesystem::status(spec.path, ec);
        if (ec || !std::filesystem::exists(status)) {
            note("so." + name + ".path", "文件不存在: " + spec.path);
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            note("so." + name + ".path", "不是普通文件: " + spec.path);
            continue;
        }
        if (!options.check_hashes || !IsHexDigest(spec.sha256)) continue;
        std::string actual;
        std::string hash_error;
        if (!HashFile(spec.path, &actual, &hash_error)) {
            note("so." + name + ".sha256", hash_error);
            continue;
        }
        if (Lower(actual) != Lower(spec.sha256)) {
            note("so." + name + ".sha256",
                 "内容哈希不匹配: 声明 " + Lower(spec.sha256) + "，实际 " +
                     Lower(actual));
        }
    }

    return diags.empty();
}

bool SoManifest::Validate(std::string* error) const {
    return Validate(SoValidationOptions{}, nullptr, error);
}

const SoSpec* SoManifest::Find(const std::string& name) const {
    const auto it = specs.find(name);
    return it == specs.end() ? nullptr : &it->second;
}

bool SoManifest::BuildLoadOrder(const std::vector<std::string>& names,
                                std::vector<std::string>* order,
                                std::string* error) const {
    if (!order) {
        if (error) *error = "SoManifest::BuildLoadOrder 的输出对象为空";
        return false;
    }
    std::vector<std::string> result;
    std::set<std::string> requested;
    for (const auto& name : names) {
        if (!specs.count(name)) {
            if (error) *error = "拓扑排序遇到未知 SO: " + name;
            return false;
        }
        requested.insert(name);
    }
    std::map<std::string, int> colors;
    std::vector<std::string> stack;
    std::string cycle;
    std::function<bool(const std::string&)> visit = [&](const std::string& name) {
        colors[name] = 1;
        stack.push_back(name);
        const auto& spec = specs.at(name);
        for (const auto& dependency : spec.dependencies) {
            if (!requested.count(dependency)) continue;
            if (colors[dependency] == 1) {
                cycle = "依赖环: ";
                const auto begin = std::find(stack.begin(), stack.end(),
                                             dependency);
                for (auto position = begin; position != stack.end(); ++position) {
                    if (cycle != "依赖环: ") cycle += " -> ";
                    cycle += *position;
                }
                cycle += " -> " + dependency;
                return false;
            }
            if (colors[dependency] == 0 && !visit(dependency)) return false;
        }
        stack.pop_back();
        colors[name] = 2;
        result.push_back(name);
        return true;
    };
    for (const auto& name : requested) {
        if (colors[name] == 0 && !visit(name)) {
            if (error) *error = cycle;
            return false;
        }
    }
    *order = std::move(result);
    return true;
}

bool SoManifest::HashFile(const std::string& path, std::string* hex,
                          std::string* error) {
    if (!hex) {
        if (error) *error = "SoManifest::HashFile 的输出对象为空";
        return false;
    }
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        if (error) *error = "无法打开待哈希文件: " + path;
        return false;
    }
    go2cpp::hash::Sha256 hasher;
    char buffer[64 * 1024];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        const auto count = stream.gcount();
        if (count > 0) {
            hasher.Write(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
    }
    if (!stream.eof()) {
        if (error) *error = "读取待哈希文件失败: " + path;
        return false;
    }
    *hex = HexDigest(hasher.Sum());
    return true;
}

SoArtifactIdentity ArtifactIdentityOf(const SoSpec& spec) {
    // 依赖顺序不构成语义：排序后连接成一个字符串字段，既保留「集合」语义，
    // 又让字段表只含可序列化的标量，能直接参与统一的字段级 diff。
    std::vector<std::string> dependencies = spec.dependencies;
    std::sort(dependencies.begin(), dependencies.end());
    std::string joined;
    for (const auto& dependency : dependencies) {
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += dependency;
    }
    SoArtifactIdentity identity;
    identity.group = spec.group;
    identity.path = spec.path;
    identity.sha256 = spec.sha256;
    identity.version = spec.version;
    identity.abi_version = spec.abi_version;
    identity.symbol_prefix = spec.symbol_prefix;
    identity.dependencies = std::move(joined);
    return identity;
}

std::vector<std::string> SoArtifactFieldChanges(const SoSpec& left,
                                                const SoSpec& right) {
    const std::vector<ConfigFieldChange> changes = ConfigFieldDiff(
        ArtifactIdentityOf(left), ArtifactIdentityOf(right));
    std::vector<std::string> names;
    names.reserve(changes.size());
    for (const auto& change : changes) {
        names.push_back(change.name);
    }
    return names;
}

std::vector<std::string> SoArtifactFieldNames() {
    std::size_t count = 0;
    const go2cpp::reflect::FieldDescriptor* fields =
        go2cpp::reflect::FieldList<SoArtifactIdentity>::Fields(count);
    std::vector<std::string> names;
    names.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        names.emplace_back(fields[index].name);
    }
    return names;
}

bool SameSoArtifact(const SoSpec& left, const SoSpec& right) {
    // 字段级比较：字段表由 GO2CPP_REFLECT_FIELD 生成，新增/调整参与比较的
    // artifact 字段只需改 FieldList，不再同步维护一长串手写比较链。
    return ConfigFieldDiff(ArtifactIdentityOf(left), ArtifactIdentityOf(right))
        .empty();
}

}  // namespace go2cpp::config
