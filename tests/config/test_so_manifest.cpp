/**
 * @file tests/config/test_so_manifest.cpp
 * @brief SO manifest 解析、静态校验与两阶段热重载事务的假 loader 测试。
 *
 * ## 覆盖的能力
 * 1. A1：依赖组里只更新被依赖者 → 整次事务被拒绝；
 * 2. A2：循环依赖 → `Validate()` 失败；
 * 3. A3：两个 SO 声明同一符号前缀 → `Validate()` 失败；
 * 4. B2：SHA-256 不匹配 → `Validate()` 失败；
 * 5. B3：版本化路径不存在 → `Validate()` 失败；
 * 6. D1：第 2 个 SO loader 失败 → 第 1 个已加载的也必须回滚；
 * 7. D2：5 个里第 4 个失败 → 前 3 个也不生效；
 * 8. D3：`keep_versions=3` 时旧版本路径仍在保留清单里；
 * 9. E1：并发两个 revision → 只有更大的 revision 提交；
 * 10. F2：热更后 `runtime.current_version` 可读且等于 manifest 版本；
 * 11. C2（额外）：`SoActivator` 一定在 `NotifyPending` 之后执行。
 *
 * ## 测试策略
 * 所有 `.so` 都是临时目录里的普通文本文件：loader 是假回调，不编译、不
 * `dlopen`。这样可以精确控制「第几个 SO 失败」「在 loader 里阻塞」等时序，
 * 并且测试不依赖平台动态库工具链。
 *
 * ## 运行
 * `ctest --test-dir build-work -R so_manifest --output-on-failure`
 */
#include "go2cpp/config/so_manifest.hpp"
#include "go2cpp/config/so_reloader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "[so_manifest] FAILED line %d: %s\n", line, expression);
    ++g_failures;
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

struct TempDir final {
    std::string path;

    TempDir() {
        static std::atomic<int> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                ("go2cpp_so_manifest_" + std::to_string(::getpid()) + "_" +
                 std::to_string(sequence.fetch_add(1))))
                   .string();
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) {
            std::fprintf(stderr, "[so_manifest] 无法创建临时目录: %s\n",
                         path.c_str());
            ++g_failures;
        }
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

struct GroupDef final {
    std::string name;
    std::vector<std::string> members;
};

std::string Join(const std::vector<std::string>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) result += ",";
        result += values[index];
    }
    return result;
}

// 生成合法 manifest 文本；文件名固定为 <name>-v<version>.so，满足 B3。
std::string BuildIni(std::uint64_t revision, std::size_t keep_versions,
                     const std::vector<GroupDef>& groups,
                     const std::vector<go2cpp::config::SoSpec>& specs,
                     bool include_spec_keep = true) {
    std::ostringstream out;
    out << "[so]\n";
    out << "revision=" << revision << "\n";
    out << "abi_version=" << go2cpp::config::kSoAbiVersion << "\n";
    out << "keep_versions=" << keep_versions << "\n";
    out << "groups=";
    for (std::size_t index = 0; index < groups.size(); ++index) {
        if (index != 0) out << ",";
        out << groups[index].name;
    }
    out << "\nsos=";
    for (std::size_t index = 0; index < specs.size(); ++index) {
        if (index != 0) out << ",";
        out << specs[index].name;
    }
    out << "\n\n";

    for (const auto& group : groups) {
        out << "[so.group." << group.name << "]\n";
        out << "members=" << Join(group.members) << "\n\n";
    }
    for (const auto& spec : specs) {
        out << "[so." << spec.name << "]\n";
        out << "group=" << spec.group << "\n";
        out << "version=" << spec.version << "\n";
        out << "path=" << spec.path << "\n";
        out << "sha256=" << spec.sha256 << "\n";
        out << "abi_version=" << spec.abi_version << "\n";
        out << "symbol_prefix=" << spec.symbol_prefix << "\n";
        out << "dependencies=" << Join(spec.dependencies) << "\n";
        if (include_spec_keep) {
            out << "keep_versions=" << spec.keep_versions << "\n";
        }
        out << "retire_policy=" << spec.retire_policy << "\n";
        out << "candidate=" << (spec.candidate ? "true" : "false") << "\n\n";
    }
    return out.str();
}

bool ParseManifest(const std::string& text, go2cpp::config::SoManifest* manifest,
                   std::string* error) {
    go2cpp::config::IniFile ini;
    return ini.Parse(text, error) &&
           go2cpp::config::SoManifest::Parse(ini, manifest, error);
}

go2cpp::config::SoSpec MakeSpec(
    const TempDir& directory, std::string name, std::string group,
    std::string version, std::vector<std::string> dependencies = {},
    std::size_t keep_versions = 0, std::string content = "") {
    go2cpp::config::SoSpec spec;
    spec.name = std::move(name);
    spec.group = std::move(group);
    spec.version = std::move(version);
    spec.dependencies = std::move(dependencies);
    spec.keep_versions = keep_versions == 0
                             ? go2cpp::config::kSoDefaultKeepVersions
                             : keep_versions;
    spec.symbol_prefix = spec.name + "_";
    spec.path = (std::filesystem::path(directory.path) /
                 (spec.name + "-v" + spec.version + ".so"))
                    .string();
    if (content.empty()) content = spec.name + "-payload-" + spec.version;
    std::ofstream stream(spec.path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    if (!stream) {
        std::fprintf(stderr, "[so_manifest] 无法写测试 SO: %s\n",
                     spec.path.c_str());
        ++g_failures;
    }
    std::string error;
    if (!go2cpp::config::SoManifest::HashFile(spec.path, &spec.sha256, &error)) {
        std::fprintf(stderr, "[so_manifest] 哈希失败: %s\n", error.c_str());
        ++g_failures;
    }
    return spec;
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// ---------------------------------------------------------------------------
// A1：只更新被依赖者 B，依赖者 A 未升级 → 拒绝整组，loader 不增加调用。
// ---------------------------------------------------------------------------
void ScenarioA1() {
    TempDir directory;
    const auto a = MakeSpec(directory, "a1a", "a1g", "1", {"a1b"});
    const auto b1 = MakeSpec(directory, "a1b", "a1g", "1");
    go2cpp::config::SoManifest manifest1;
    std::string error;
    CHECK(ParseManifest(
        BuildIni(1, 3, {{"a1g", {"a1a", "a1b"}}}, {a, b1}), &manifest1,
        &error));

    int loader_calls = 0;
    go2cpp::config::SoReloader reloader(
        go2cpp::config::Config::Instance(),
        [&loader_calls](const go2cpp::config::SoSpec&, std::string*) {
            ++loader_calls;
            return true;
        });
    const auto first = reloader.Reload(manifest1);
    CHECK(first.ok);
    CHECK(loader_calls == 2);
    const int calls_before = loader_calls;

    // B 升级到 v2；A 的 path/sha/version 完全没变，构成 A1 的拒绝条件。
    const auto b2 = MakeSpec(directory, "a1b", "a1g", "2");
    go2cpp::config::SoManifest manifest2;
    CHECK(ParseManifest(
        BuildIni(2, 3, {{"a1g", {"a1a", "a1b"}}}, {a, b2}), &manifest2,
        &error));
    const auto second = reloader.Reload(manifest2);
    CHECK(!second.ok);
    CHECK(second.error.find("依赖组不自洽") != std::string::npos);
    CHECK(loader_calls == calls_before);
    CHECK(reloader.CurrentVersion("a1a") == "1");
    CHECK(reloader.CurrentVersion("a1b") == "1");
    CHECK(reloader.CommittedRevision() == 1);
    std::printf("[A1] 拒绝整组，loader 调用数保持 %d\n", calls_before);
}

// ---------------------------------------------------------------------------
// A2：A↔B 循环依赖 → Validate 失败，错误信息带环。
// ---------------------------------------------------------------------------
void ScenarioA2() {
    TempDir directory;
    const auto a = MakeSpec(directory, "a2a", "a2g", "1", {"a2b"});
    const auto b = MakeSpec(directory, "a2b", "a2g", "1", {"a2a"});
    go2cpp::config::SoManifest manifest;
    std::string error;
    CHECK(ParseManifest(
        BuildIni(1, 3, {{"a2g", {"a2a", "a2b"}}}, {a, b}), &manifest, &error));
    CHECK(!manifest.Validate(&error));
    CHECK(error.find("依赖环") != std::string::npos);
    std::printf("[A2] Validate 拒绝循环依赖: %s\n", error.c_str());
}

// ---------------------------------------------------------------------------
// A3：两个 SO 声明同一 symbol_prefix → Validate 失败。
// ---------------------------------------------------------------------------
void ScenarioA3() {
    TempDir directory;
    auto a = MakeSpec(directory, "a3a", "a3g", "1");
    auto b = MakeSpec(directory, "a3b", "a3g", "1");
    b.symbol_prefix = a.symbol_prefix = "dup_";
    go2cpp::config::SoManifest manifest;
    std::string error;
    CHECK(ParseManifest(
        BuildIni(1, 3, {{"a3g", {"a3a", "a3b"}}}, {a, b}), &manifest, &error));
    CHECK(!manifest.Validate(&error));
    CHECK(error.find("符号前缀") != std::string::npos);
    std::printf("[A3] Validate 拒绝重复前缀: %s\n", error.c_str());
}

// ---------------------------------------------------------------------------
// B2：manifest 的 sha256 与文件内容不符 → Validate 失败。
// ---------------------------------------------------------------------------
void ScenarioB2() {
    TempDir directory;
    auto spec = MakeSpec(directory, "b2so", "b2g", "1");
    spec.sha256 = std::string(64, '0');
    go2cpp::config::SoManifest manifest;
    std::string error;
    CHECK(ParseManifest(
        BuildIni(1, 3, {{"b2g", {"b2so"}}}, {spec}), &manifest, &error));
    CHECK(!manifest.Validate(&error));
    CHECK(error.find("内容哈希不匹配") != std::string::npos);
    std::printf("[B2] Validate 拒绝哈希不匹配: %s\n", error.c_str());
}

// ---------------------------------------------------------------------------
// B3：版本化路径指向不存在的文件 → Validate 失败。
// ---------------------------------------------------------------------------
void ScenarioB3() {
    TempDir directory;
    go2cpp::config::SoSpec spec;
    spec.name = "b3so";
    spec.group = "b3g";
    spec.version = "2";
    spec.path = (std::filesystem::path(directory.path) / "b3so-v2.so").string();
    spec.sha256 = std::string(64, 'a');
    spec.symbol_prefix = "b3_";
    go2cpp::config::SoManifest manifest;
    std::string error;
    CHECK(ParseManifest(
        BuildIni(1, 3, {{"b3g", {"b3so"}}}, {spec}), &manifest, &error));
    CHECK(!manifest.Validate(&error));
    CHECK(error.find("文件不存在") != std::string::npos);
    std::printf("[B3] Validate 拒绝缺失的版本化路径: %s\n", error.c_str());
}

// ---------------------------------------------------------------------------
// D1：第 2 个 SO 的 loader 失败 → 第 1 个已加载的也要回滚。
// ---------------------------------------------------------------------------
void ScenarioD1() {
    TempDir directory;
    const auto a1 = MakeSpec(directory, "d1a", "d1g", "1");
    const auto b1 = MakeSpec(directory, "d1b", "d1g", "1");
    go2cpp::config::SoManifest manifest1;
    std::string error;
    CHECK(ParseManifest(
        BuildIni(1, 3, {{"d1g", {"d1a", "d1b"}}}, {a1, b1}), &manifest1,
        &error));

    std::atomic<int> unloads{0};
    std::atomic<int> activations{0};
    std::vector<std::string> unloaded;
    std::mutex unloaded_mutex;
    std::atomic<bool> failed_once{false};
    go2cpp::config::SoReloader reloader(
        go2cpp::config::Config::Instance(),
        [&failed_once](const go2cpp::config::SoSpec& spec, std::string* why) {
            if (spec.name == "d1b" && spec.version == "2" &&
                !failed_once.exchange(true)) {
                if (why) *why = "假 loader 对 d1b v2 故意失败";
                return false;
            }
            return true;
        },
        [&unloads, &unloaded, &unloaded_mutex](
            const go2cpp::config::SoSpec& spec) {
            ++unloads;
            std::lock_guard<std::mutex> lock(unloaded_mutex);
            unloaded.push_back(spec.name + ":" + spec.version);
        },
        [&activations](const go2cpp::config::SoSpec&) { ++activations; });

    CHECK(reloader.Reload(manifest1).ok);
    CHECK(activations.load() == 2);

    const auto a2 = MakeSpec(directory, "d1a", "d1g", "2");
    const auto b2 = MakeSpec(directory, "d1b", "d1g", "2");
    go2cpp::config::SoManifest manifest2;
    CHECK(ParseManifest(
        BuildIni(2, 3, {{"d1g", {"d1a", "d1b"}}}, {a2, b2}), &manifest2,
        &error));
    const auto result = reloader.Reload(manifest2);
    CHECK(!result.ok);
    CHECK(result.loaded.empty());
    CHECK(unloads.load() == 1);
    CHECK(activations.load() == 2);
    {
        std::lock_guard<std::mutex> lock(unloaded_mutex);
        CHECK(unloaded.size() == 1);
        CHECK(!unloaded.empty() && unloaded[0] == "d1a:2");
    }
    CHECK(reloader.CurrentVersion("d1a") == "1");
    CHECK(reloader.CurrentVersion("d1b") == "1");
    std::printf("[D1] 失败后仅卸载新加载的 d1a:2；旧版本保持 1\n");
}

// ---------------------------------------------------------------------------
// D2：5 个 SO 第 4 个失败 → 前 3 个也不生效，全量回滚。
// ---------------------------------------------------------------------------
void ScenarioD2() {
    TempDir directory;
    std::vector<go2cpp::config::SoSpec> first_specs;
    std::vector<go2cpp::config::SoSpec> second_specs;
    std::vector<GroupDef> groups{{"d2g", {"d2s1", "d2s2", "d2s3", "d2s4", "d2s5"}}};
    for (int index = 1; index <= 5; ++index) {
        const auto name = "d2s" + std::to_string(index);
        first_specs.push_back(MakeSpec(directory, name, "d2g", "1"));
        second_specs.push_back(MakeSpec(directory, name, "d2g", "2"));
    }
    go2cpp::config::SoManifest manifest1;
    go2cpp::config::SoManifest manifest2;
    std::string error;
    CHECK(ParseManifest(BuildIni(1, 3, groups, first_specs), &manifest1, &error));
    CHECK(ParseManifest(BuildIni(2, 3, groups, second_specs), &manifest2, &error));

    std::atomic<int> version2_calls{0};
    std::atomic<int> unloads{0};
    std::atomic<int> activations{0};
    go2cpp::config::SoReloader reloader(
        go2cpp::config::Config::Instance(),
        [&version2_calls](const go2cpp::config::SoSpec& spec, std::string* why) {
            if (spec.version == "2") {
                const int call = version2_calls.fetch_add(1) + 1;
                if (call == 4) {
                    if (why) *why = "假 loader 第 4 个 SO 故意失败";
                    return false;
                }
            }
            return true;
        },
        [&unloads](const go2cpp::config::SoSpec&) { ++unloads; },
        [&activations](const go2cpp::config::SoSpec&) { ++activations; });

    CHECK(reloader.Reload(manifest1).ok);
    CHECK(activations.load() == 5);
    const auto result = reloader.Reload(manifest2);
    CHECK(!result.ok);
    CHECK(version2_calls.load() == 4);
    CHECK(unloads.load() == 3);
    CHECK(activations.load() == 5);
    for (int index = 1; index <= 5; ++index) {
        const auto name = "d2s" + std::to_string(index);
        CHECK(reloader.CurrentVersion(name) == "1");
    }
    CHECK(reloader.CommittedRevision() == 1);
    std::printf("[D2] 第 4 个失败 → 3 个新句柄全部卸载，5 个版本全部保持 1\n");
}

// ---------------------------------------------------------------------------
// D3：keep_versions=3，v1..v4 依次提交 → 保留清单只留 v2/v3/v4。
// ---------------------------------------------------------------------------
void ScenarioD3() {
    TempDir directory;
    // 全局 keep_versions 必须能被未写 [so.<name>] keep_versions 的 SO 继承。
    const auto inherited = MakeSpec(directory, "d3inherit", "d3g", "1", {}, 0);
    go2cpp::config::SoManifest inherited_manifest;
    std::string inherit_error;
    CHECK(ParseManifest(
        BuildIni(1, 5, {{"d3g", {"d3inherit"}}}, {inherited}, false),
        &inherited_manifest, &inherit_error));
    CHECK(inherited_manifest.specs.at("d3inherit").keep_versions == 5);

    go2cpp::config::SoReloader reloader(
        go2cpp::config::Config::Instance(),
        [](const go2cpp::config::SoSpec&, std::string*) { return true; });
    std::vector<std::string> paths;
    for (int version = 1; version <= 4; ++version) {
        const auto spec = MakeSpec(directory, "d3so", "d3g",
                                   std::to_string(version), {}, 3);
        paths.push_back(spec.path);
        go2cpp::config::SoManifest manifest;
        std::string error;
        CHECK(ParseManifest(
            BuildIni(static_cast<std::uint64_t>(version), 3,
                     {{"d3g", {"d3so"}}}, {spec}),
            &manifest, &error));
        const auto result = reloader.Reload(manifest);
        CHECK(result.ok);
        if (version == 2) {
            const auto retained = reloader.RetainedPaths("d3so");
            CHECK(Contains(retained, paths[0]));
            CHECK(Contains(retained, paths[1]));
        }
    }
    const auto retained = reloader.RetainedPaths("d3so");
    CHECK(retained.size() == 3);
    CHECK(!Contains(retained, paths[0]));
    CHECK(Contains(retained, paths[1]));
    CHECK(Contains(retained, paths[2]));
    CHECK(Contains(retained, paths[3]));
    CHECK(reloader.CurrentVersion("d3so") == "4");
    std::printf("[D3] keep_versions=3，最终保留 [v2,v3,v4]，v1 已裁剪\n");
}

// ---------------------------------------------------------------------------
// E1：旧 revision 在 loader 中阻塞，新 revision 到达 → 只有新 revision 提交。
// ---------------------------------------------------------------------------
void ScenarioE1() {
    TempDir directory;
    const auto spec1 = MakeSpec(directory, "e1so", "e1g", "1");
    const auto spec2 = MakeSpec(directory, "e1so", "e1g", "2");
    go2cpp::config::SoManifest manifest1;
    go2cpp::config::SoManifest manifest2;
    std::string error;
    CHECK(ParseManifest(BuildIni(1, 3, {{"e1g", {"e1so"}}}, {spec1}),
                        &manifest1, &error));
    CHECK(ParseManifest(BuildIni(2, 3, {{"e1g", {"e1so"}}}, {spec2}),
                        &manifest2, &error));

    std::atomic<bool> entered{false};
    bool release = false;
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<int> unloads{0};
    go2cpp::config::SoReloader reloader(
        go2cpp::config::Config::Instance(),
        [&entered, &release, &mutex, &condition](
            const go2cpp::config::SoSpec& spec, std::string*) {
            if (spec.version != "1") return true;
            std::unique_lock<std::mutex> lock(mutex);
            entered.store(true);
            condition.notify_all();
            condition.wait(lock, [&release] { return release; });
            return true;
        },
        [&unloads](const go2cpp::config::SoSpec&) { ++unloads; });

    go2cpp::config::SoReloadResult low_result;
    go2cpp::config::SoReloadResult high_result;
    std::thread low([&] { low_result = reloader.Reload(manifest1); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(5),
                                [&entered] { return entered.load(); })) {
            CHECK(false);
        }
    }
    std::thread high([&] { high_result = reloader.Reload(manifest2); });
    for (int attempt = 0; attempt < 5000 && reloader.HighestSeenRevision() < 2;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(reloader.HighestSeenRevision() == 2);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_all();
    low.join();
    high.join();

    CHECK(!low_result.ok);
    CHECK(low_result.error.find("更高 revision") != std::string::npos ||
          low_result.error.find("回滚") != std::string::npos);
    CHECK(high_result.ok);
    CHECK(reloader.CommittedRevision() == 2);
    CHECK(reloader.CurrentVersion("e1so") == "2");
    CHECK(unloads.load() >= 1);
    std::printf("[E1] 旧 revision 回滚，最终提交 revision=2/version=2\n");
}

// ---------------------------------------------------------------------------
// F2：热更后 runtime.current_version 可读，且与 manifest 版本一致。
// ---------------------------------------------------------------------------
void ScenarioF2() {
    TempDir directory;
    const auto spec1 = MakeSpec(directory, "f2so", "f2g", "1");
    const auto spec2 = MakeSpec(directory, "f2so", "f2g", "2");
    go2cpp::config::SoReloader reloader(
        go2cpp::config::Config::Instance(),
        [](const go2cpp::config::SoSpec&, std::string*) { return true; });
    std::string error;
    go2cpp::config::SoManifest manifest1;
    go2cpp::config::SoManifest manifest2;
    CHECK(ParseManifest(BuildIni(1, 3, {{"f2g", {"f2so"}}}, {spec1}),
                        &manifest1, &error));
    CHECK(ParseManifest(BuildIni(2, 3, {{"f2g", {"f2so"}}}, {spec2}),
                        &manifest2, &error));
    CHECK(reloader.Reload(manifest1).ok);
    CHECK(reloader.Reload(manifest2).ok);

    auto& config = go2cpp::config::Config::Instance();
    const auto version = config.Lookup<std::string>(
        go2cpp::config::SoReloader::RuntimeVersionKey("f2so"), "",
        "SO 当前版本");
    const auto path = config.Lookup<std::string>(
        go2cpp::config::SoReloader::RuntimePathKey("f2so"), "",
        "SO 加载路径");
    const auto revision = config.Lookup<std::string>(
        "so.runtime.revision", "", "SO manifest revision");
    CHECK(version != nullptr && version->GetValue() == "2");
    CHECK(path != nullptr && path->GetValue() == spec2.path);
    CHECK(revision != nullptr && revision->GetValue() == "2");
    CHECK(reloader.CurrentVersion("f2so") == "2");
    std::printf("[F2] runtime.current_version=%s, revision=%s\n",
                version ? version->GetValue().c_str() : "(null)",
                revision ? revision->GetValue().c_str() : "(null)");
}

// ---------------------------------------------------------------------------
// C2（额外）：SoActivator 必须在 NotifyPending 之后执行。
// ---------------------------------------------------------------------------
void ScenarioC2Ordering() {
    TempDir directory;
    const auto spec = MakeSpec(directory, "c2so", "c2g", "1");
    go2cpp::config::SoManifest manifest;
    std::string error;
    CHECK(ParseManifest(BuildIni(1, 3, {{"c2g", {"c2so"}}}, {spec}),
                        &manifest, &error));

    auto& config = go2cpp::config::Config::Instance();
    auto variable = config.Lookup<std::string>(
        go2cpp::config::SoReloader::RuntimeVersionKey("c2so"), "",
        "SO 当前版本");
    CHECK(variable != nullptr);
    std::atomic<bool> notified{false};
    std::atomic<bool> activated{false};
    std::atomic<bool> activated_after_notify{false};
    if (variable) {
        variable->AddListener([&notified](const std::string&, const std::string&) {
            notified.store(true);
        });
    }
    go2cpp::config::SoReloader reloader(
        config, [](const go2cpp::config::SoSpec&, std::string*) { return true; },
        {}, [&activated, &activated_after_notify, &notified](
                const go2cpp::config::SoSpec&) {
            activated.store(true);
            activated_after_notify.store(notified.load());
        });
    CHECK(reloader.Reload(manifest).ok);
    CHECK(notified.load());
    CHECK(activated.load());
    CHECK(activated_after_notify.load());
    std::printf("[C2] NotifyPending 先于 SoActivator\n");
}

}  // namespace

int main() {
    std::printf("=== SO manifest / 热重载事务测试 ===\n");
    std::printf("[场景 A1] 依赖组只更新被依赖者\n");
    ScenarioA1();
    std::printf("[场景 A2] 循环依赖\n");
    ScenarioA2();
    std::printf("[场景 A3] 符号前缀冲突\n");
    ScenarioA3();
    std::printf("[场景 B2] 哈希不匹配\n");
    ScenarioB2();
    std::printf("[场景 B3] 版本化路径缺失\n");
    ScenarioB3();
    std::printf("[场景 D1] 第 2 个 SO 加载失败\n");
    ScenarioD1();
    std::printf("[场景 D2] 5 个里第 4 个失败\n");
    ScenarioD2();
    std::printf("[场景 D3] keep_versions=3 保留清单\n");
    ScenarioD3();
    std::printf("[场景 E1] 并发 revision 栅栏\n");
    ScenarioE1();
    std::printf("[场景 F2] runtime.current_version\n");
    ScenarioF2();
    std::printf("[额外 C2] NotifyPending → SoActivator 顺序\n");
    ScenarioC2Ordering();

    if (g_failures == 0) {
        std::printf("\n[so_manifest] failures=0\n");
    } else {
        std::printf("\n[so_manifest] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
