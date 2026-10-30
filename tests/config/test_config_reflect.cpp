/**
 * @file tests/config/test_config_reflect.cpp
 * @brief Config 文本编解码改走 Reflect 的回归 / 场景 / 随机 / 压测测试。
 *
 * ## 覆盖范围
 * 1. 旧配置兼容：裸标量、`yes/no/on/off`、`.5`、逗号分隔数组；
 * 2. 结构体配置：`GO2CPP_REFLECT_FIELD` 字段表、缺字段保留默认值；
 * 3. 嵌套结构体：结构体字段里再套结构体；
 * 4. 往返：标量 / 结构体 / 数组 / `log::Level` 编码后解码一致；
 * 5. 热重载：`LoadFromFile` + 监听器 + 字段级 diff；
 * 6. 错误类型：字段类型不符、整型越界；
 * 7. 未知键：结构体 JSON 里出现字段表之外的键；
 * 8. 范围校验：`ConfigVar::SetValidator` 与结构体字段范围；
 * 9. 字段级 diff：`ConfigFieldDiff` 的名称/偏移来自 `FieldDescriptor`；
 * 10. 注册表集成：`Config::Lookup` + `LoadFromIni` + `DumpIni` 往返；
 * 11. SO 热更字段级 diff：`SoArtifactFieldChanges` / `SoReloader::changed_fields`；
 * 12. 随机往返：1000 组随机结构体/字符串；
 * 13. 并发压测：4 线程并发读写同一变量，值始终合法；
 * 14. 编解码吞吐：10 万次结构体编解码，报告 ops/s。
 *
 * @author go2cpp contributors
 * @date 2026-10-30
 * @copyright MIT License
 */
#include "go2cpp/config/config.hpp"
#include "go2cpp/config/so_reloader.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

// ---------------------------------------------------------------------------
// 被反射的结构体：字段表由 GO2CPP_REFLECT_FIELD 生成。
// ---------------------------------------------------------------------------

/// @brief 线程池配置（第一层结构体）。
struct PoolConfig {
    int threads = 2;                 ///< 工作线程数。
    std::size_t queue_limit = 128;   ///< 队列容量。
    bool enabled = true;             ///< 是否启用。
    std::string name = "default";    ///< 池名。
};

/// @brief 服务配置（嵌套结构体：字段里再放一个结构体）。
struct ServiceConfig {
    std::string host = "127.0.0.1";  ///< 监听地址。
    int port = 8080;                 ///< 监听端口。
    PoolConfig pool{};               ///< 线程池子配置。
    bool verbose = false;            ///< 调试输出开关。
};

template <>
struct go2cpp::reflect::FieldList<PoolConfig> {
    static constexpr go2cpp::reflect::TypeId kId = 201;
    static constexpr const char* kName = "PoolConfig";

    static const go2cpp::reflect::FieldDescriptor* Fields(std::size_t& count) {
        static const go2cpp::reflect::FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(PoolConfig, threads),
            GO2CPP_REFLECT_FIELD(PoolConfig, queue_limit),
            GO2CPP_REFLECT_FIELD(PoolConfig, enabled),
            GO2CPP_REFLECT_FIELD(PoolConfig, name),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

template <>
struct go2cpp::reflect::FieldList<ServiceConfig> {
    static constexpr go2cpp::reflect::TypeId kId = 202;
    static constexpr const char* kName = "ServiceConfig";

    static const go2cpp::reflect::FieldDescriptor* Fields(std::size_t& count) {
        static const go2cpp::reflect::FieldDescriptor fields[] = {
            GO2CPP_REFLECT_FIELD(ServiceConfig, host),
            GO2CPP_REFLECT_FIELD(ServiceConfig, port),
            GO2CPP_REFLECT_FIELD(ServiceConfig, pool),
            GO2CPP_REFLECT_FIELD(ServiceConfig, verbose),
        };
        count = sizeof(fields) / sizeof(fields[0]);
        return fields;
    }
};

namespace {

using go2cpp::config::Config;
using go2cpp::config::ConfigDecode;
using go2cpp::config::ConfigEncode;
using go2cpp::config::ConfigFieldDiff;
using go2cpp::config::ConfigVar;
using go2cpp::config::ConfigValuesEqual;
using go2cpp::config::IniFile;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "[cfgreflect] FAILED: %s\n", what);
    ++g_failures;
}

void CheckMessage(bool condition, const char* what, const std::string& detail) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "[cfgreflect] FAILED: %s（%s）\n", what, detail.c_str());
    ++g_failures;
}

bool WriteFile(const std::string& path, const std::string& text) {
    std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << text;
    stream.flush();
    return static_cast<bool>(stream);
}

bool Contains(const std::vector<std::string>& names, const std::string& target) {
    for (const auto& name : names) {
        if (name == target) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 场景 1：旧配置兼容（裸标量 / 布尔词表 / 浮点写法）
// ---------------------------------------------------------------------------
void ScenarioLegacyScalars() {
    std::printf("[场景1] 旧配置标量兼容\n");
    ConfigVar<int> number("cfgreflect.number", 0, "整数");
    Check(number.FromString("42"), "int 42");
    Check(number.GetValue() == 42, "int 42 value");
    Check(number.FromString("-7"), "int -7");
    Check(number.GetValue() == -7, "int -7 value");
    Check(!number.FromString("42x"), "尾随字符必须拒绝");
    Check(number.GetValue() == -7, "失败后值不变");
    Check(!number.FromString("3000000000"), "int 越界必须拒绝");

    ConfigVar<bool> flag("cfgreflect.flag", false, "布尔");
    Check(flag.FromString("yes") && flag.GetValue(), "bool yes");
    Check(flag.FromString("1") && flag.GetValue(), "bool 1");
    Check(flag.FromString("on") && flag.GetValue(), "bool on");
    Check(flag.FromString("off") && !flag.GetValue(), "bool off");
    Check(flag.FromString("NO") && !flag.GetValue(), "bool NO");
    Check(flag.FromString("true") && flag.GetValue(), "bool true");
    Check(!flag.FromString("maybe"), "非法布尔必须拒绝");

    ConfigVar<std::string> text("cfgreflect.text", "", "文本");
    Check(text.FromString("demo") && text.GetValue() == "demo", "string 裸文本");
    Check(text.ToString() == "demo", "string 编码为裸文本");
    Check(text.FromString("") && text.GetValue().empty(), "string 空值");
    Check(text.FromString("\"\\\"x\\\"\"") && text.GetValue() == "\"x\"",
          "string JSON 字符串（值本身以引号开头）");
    Check(text.ToString() == "\"\\\"x\\\"\"", "以引号开头的字符串按 JSON 转义");
    Check(text.FromString("raw") && text.GetValue() == "raw", "string 回到裸文本");

    ConfigVar<double> ratio("cfgreflect.ratio", 0.0, "浮点");
    Check(ratio.FromString("0.25") && ratio.GetValue() == 0.25, "double 0.25");
    Check(ratio.FromString(".5") && ratio.GetValue() == 0.5, "double .5 旧写法");
    Check(ratio.FromString("1e3") && ratio.GetValue() == 1000.0, "double 1e3");
    Check(!ratio.FromString("abc"), "非法浮点必须拒绝");

    ConfigVar<go2cpp::log::Level> level("cfgreflect.level", go2cpp::log::Level::Warn,
                                        "日志级别");
    Check(level.FromString("debug") && level.GetValue() == go2cpp::log::Level::Debug,
          "level debug");
    Check(!level.FromString("not-a-level"), "非法级别必须拒绝");
}

// ---------------------------------------------------------------------------
// 场景 2：数组（新 JSON 数组 + 旧逗号分隔）
// ---------------------------------------------------------------------------
void ScenarioVectors() {
    std::printf("[场景2] 数组编解码与旧格式兼容\n");
    ConfigVar<std::vector<int>> list("cfgreflect.list", {}, "整数数组");
    Check(list.FromString("[1,2,3]"), "JSON 数组");
    Check(list.GetValue() == std::vector<int>({1, 2, 3}), "JSON 数组值");
    Check(list.ToString() == "[1,2,3]", "数组编码为 JSON");
    Check(list.FromString("4,5,6"), "旧逗号格式");
    Check(list.GetValue() == std::vector<int>({4, 5, 6}), "旧逗号格式值");
    Check(list.FromString("") && list.GetValue().empty(), "空数组");
    Check(!list.FromString("[1,\"x\"]"), "数组元素类型错误必须拒绝");

    ConfigVar<std::vector<std::string>> names("cfgreflect.names", {}, "字符串数组");
    Check(names.FromString("[\"a\",\"b\"]"), "字符串数组");
    Check(names.GetValue() == std::vector<std::string>({"a", "b"}), "字符串数组值");
    Check(names.FromString("a,b"), "字符串数组旧格式");
    Check(names.GetValue() == std::vector<std::string>({"a", "b"}), "字符串数组旧值");
}

// ---------------------------------------------------------------------------
// 场景 3：结构体配置——缺字段保留默认值（“新字段默认”）
// ---------------------------------------------------------------------------
void ScenarioStructDefaults() {
    std::printf("[场景3] 结构体配置缺字段保留默认值\n");
    ConfigVar<PoolConfig> pool("cfgreflect.pool", PoolConfig{}, "线程池");
    const PoolConfig defaults{};
    Check(pool.FromString(R"({"threads":8})"), "结构体缺字段解析");
    const PoolConfig parsed = pool.GetValue();
    Check(parsed.threads == 8, "显式字段生效");
    Check(parsed.queue_limit == defaults.queue_limit, "缺字段 queue_limit 用默认值");
    Check(parsed.enabled == defaults.enabled, "缺字段 enabled 用默认值");
    Check(parsed.name == defaults.name, "缺字段 name 用默认值");

    // 编码必须包含全部字段，便于下一版配置比对。
    const std::string encoded = pool.ToString();
    Check(encoded.find("\"threads\":8") != std::string::npos, "编码包含 threads");
    Check(encoded.find("\"queue_limit\":128") != std::string::npos,
          "编码包含默认字段 queue_limit");
    Check(encoded.find("\"name\":\"default\"") != std::string::npos,
          "编码包含默认字段 name");
}

// ---------------------------------------------------------------------------
// 场景 4：嵌套结构体 + 往返
// ---------------------------------------------------------------------------
void ScenarioNestedRoundTrip() {
    std::printf("[场景4] 嵌套结构体与往返\n");
    ConfigVar<ServiceConfig> service("cfgreflect.service", ServiceConfig{}, "服务");
    const char* text =
        R"({"host":"10.0.0.1","port":9000,"pool":{"threads":6,"name":"io"},"verbose":true})";
    Check(service.FromString(text), "嵌套结构体解析");
    const ServiceConfig parsed = service.GetValue();
    Check(parsed.host == "10.0.0.1", "嵌套字段 host");
    Check(parsed.port == 9000, "嵌套字段 port");
    Check(parsed.pool.threads == 6, "嵌套子结构体 threads");
    Check(parsed.pool.name == "io", "嵌套子结构体 name");
    Check(parsed.pool.queue_limit == PoolConfig{}.queue_limit, "子结构体缺字段默认");
    Check(parsed.verbose, "嵌套字段 verbose");

    ServiceConfig decoded{};
    std::string error;
    Check(ConfigDecode<ServiceConfig>(service.ToString(), &decoded, &error),
          "嵌套结构体往返解码");
    Check(ConfigValuesEqual(decoded, parsed), "嵌套结构体往返一致");
    Check(!ConfigValuesEqual(decoded, ServiceConfig{}), "不同结构体不相等");
}

// ---------------------------------------------------------------------------
// 场景 5：错误类型 / 整型越界 / 未知键
// ---------------------------------------------------------------------------
void ScenarioErrors() {
    std::printf("[场景5] 错误类型、越界与未知键\n");
    ConfigVar<PoolConfig> pool("cfgreflect.errors", PoolConfig{}, "线程池");
    PoolConfig before = pool.GetValue();

    std::string error;
    Check(!pool.FromString(R"({"threads":"abc"})", &error), "字段类型错误必须拒绝");
    Check(error.find("threads") != std::string::npos, "诊断包含字段名");
    Check(ConfigValuesEqual(pool.GetValue(), before), "失败后值不变");

    Check(!pool.FromString(R"({"threads":true})", &error), "字段类型错误（bool）");
    Check(!pool.FromString(R"({"threads":4,"unknown":1})", &error), "未知字段必须拒绝");
    Check(error.find("unknown") != std::string::npos, "未知字段诊断包含键名");
    Check(!pool.FromString("4"), "结构体不接受标量");
    Check(!pool.FromString("{bad json"), "非法 JSON 必须拒绝");

    ConfigVar<std::int8_t> tiny("cfgreflect.tiny", 0, "小整数");
    Check(!tiny.FromString("300"), "int8 越界必须拒绝");
    Check(!tiny.FromString("-300"), "int8 下界必须拒绝");
    Check(tiny.FromString("-128") && tiny.GetValue() == -128, "int8 下界可接受");

    ConfigVar<std::uint8_t> byte("cfgreflect.byte", 0, "字节");
    Check(!byte.FromString("256"), "uint8 越界必须拒绝");
    Check(byte.FromString("255") && byte.GetValue() == 255, "uint8 上界可接受");
}

// ---------------------------------------------------------------------------
// 场景 6：范围校验（业务 Validator）
// ---------------------------------------------------------------------------
void ScenarioValidators() {
    std::printf("[场景6] 范围校验\n");
    ConfigVar<int> port("cfgreflect.port", 8080, "端口");
    port.SetValidator([](const int& value, std::string* validation_error) {
        if (value < 1 || value > 65535) {
            if (validation_error != nullptr) {
                *validation_error = "端口必须在 1..65535";
            }
            return false;
        }
        return true;
    });
    std::string error;
    Check(!port.FromString("70000", &error), "端口越界必须拒绝");
    Check(error.find("65535") != std::string::npos, "范围诊断");
    Check(port.GetValue() == 8080, "越界后值不变");

    ConfigVar<PoolConfig> pool("cfgreflect.pool_range", PoolConfig{}, "线程池");
    pool.SetValidator([](const PoolConfig& value, std::string* validation_error) {
        if (value.threads < 1 || value.threads > 32) {
            if (validation_error != nullptr) {
                *validation_error = "threads 必须处于 1..32";
            }
            return false;
        }
        return true;
    });
    Check(!pool.FromString(R"({"threads":64})", &error), "结构体字段越界必须拒绝");
    Check(pool.FromString(R"({"threads":16})"), "结构体字段合法值");
    Check(pool.GetValue().threads == 16, "结构体字段合法值生效");
}

// ---------------------------------------------------------------------------
// 场景 7：字段级 diff（FieldDescriptor 名称/偏移）
// ---------------------------------------------------------------------------
void ScenarioFieldDiff() {
    std::printf("[场景7] 字段级 diff\n");
    PoolConfig before{};
    PoolConfig after = before;
    after.threads = 8;
    after.name = "changed";

    const auto changes = ConfigFieldDiff(before, after);
    Check(changes.size() == 2, "字段级 diff 数量");
    if (changes.size() == 2) {
        Check(changes[0].name == "threads", "变化字段 threads");
        Check(changes[0].size == sizeof(int), "threads 字段大小");
        const auto* descriptor = go2cpp::reflect::FindField<PoolConfig>("threads");
        Check(descriptor != nullptr, "FieldDescriptor threads 存在");
        if (descriptor != nullptr) {
            Check(changes[0].offset == descriptor->offset,
                  "diff 偏移与 FieldDescriptor 一致");
        }
        Check(changes[1].name == "name", "变化字段 name");
    }
    Check(ConfigFieldDiff(before, before).empty(), "相同结构体无变化");
    Check(ConfigValuesEqual(before, before), "相同结构体相等");
    Check(!ConfigValuesEqual(before, after), "不同结构体不相等");

    // 嵌套结构体：子字段变化能检测到（顶层字段 pool 变化）。
    ServiceConfig service_before{};
    ServiceConfig service_after = service_before;
    service_after.pool.threads = 9;
    const auto nested = ConfigFieldDiff(service_before, service_after);
    Check(nested.size() == 1 && nested[0].name == "pool", "嵌套结构体字段 diff");
}

// ---------------------------------------------------------------------------
// 场景 8：注册表集成 + DumpIni 往返
// ---------------------------------------------------------------------------
void ScenarioRegistryRoundTrip() {
    std::printf("[场景8] 配置注册表与 DumpIni 往返\n");
    auto& config = Config::Instance();
    auto service = config.Lookup<ServiceConfig>("svc.main", ServiceConfig{}, "服务配置");
    Check(service != nullptr, "结构体配置注册");
    if (!service) {
        return;
    }
    ServiceConfig expected{};
    expected.host = "0.0.0.0";
    expected.port = 7070;
    expected.pool.threads = 12;
    expected.pool.name = "io\"pool";  // 故意含引号：验证 JSON/INI 双层转义
    expected.verbose = true;
    Check(service->SetValue(expected), "结构体配置设值");

    const std::string dump = config.DumpIni();
    Check(dump.find("[svc]") != std::string::npos, "DumpIni 包含 svc 段");
    Check(dump.find("main={") != std::string::npos, "DumpIni 输出 JSON 对象");

    IniFile reparsed;
    std::string error;
    Check(reparsed.Parse(dump, &error), "DumpIni 可被 IniFile 解析");
    Check(config.LoadFromIni(reparsed, &error), "读回 DumpIni 成功");
    Check(ConfigValuesEqual(service->GetValue(), expected), "DumpIni 往返值一致");

    // 旧格式标量配置项也走同一注册表。
    auto legacy = config.Lookup<int>("svc.legacy", 0, "旧格式项");
    IniFile legacy_ini;
    Check(legacy_ini.Parse("[svc]\nlegacy=4321\n", &error), "旧格式 INI 解析");
    Check(config.LoadFromIni(legacy_ini, &error), "旧格式 LoadFromIni");
    Check(legacy->GetValue() == 4321, "旧格式值生效");
}

// ---------------------------------------------------------------------------
// 场景 9：热重载（文件 + 监听器 + 字段级 diff）
// ---------------------------------------------------------------------------
void ScenarioHotReload() {
    std::printf("[场景9] 热重载与字段级 diff\n");
    auto& config = Config::Instance();
    auto pool = config.Lookup<PoolConfig>("reload.pool", PoolConfig{}, "热重载线程池");
    Check(pool != nullptr, "热重载配置注册");
    if (!pool) {
        return;
    }
    const std::string path = "/tmp/go2cpp_config_reflect_" +
                             std::to_string(static_cast<long>(::getpid())) + ".ini";
    Check(WriteFile(path, "[reload]\npool={\"threads\":3}\n"), "写初始配置");

    std::atomic<int> notified{0};
    PoolConfig last_old{};
    PoolConfig last_new{};
    const auto listener = pool->AddListener(
        [&](const PoolConfig& old_value, const PoolConfig& new_value) {
            last_old = old_value;
            last_new = new_value;
            notified.fetch_add(1, std::memory_order_relaxed);
        });

    Check(config.LoadFromFile(path), "初始加载");
    Check(pool->GetValue().threads == 3, "初始值生效");
    Check(notified.load() == 1, "初始加载触发监听");

    Check(WriteFile(path, "[reload]\npool={\"threads\":9,\"name\":\"fast\"}\n"),
          "写更新配置");
    Check(config.LoadFromFile(path), "热重载");
    Check(pool->GetValue().threads == 9, "热重载值生效");
    Check(pool->GetValue().name == "fast", "热重载新字段");
    Check(notified.load() == 2, "热重载触发监听");
    const auto changes = ConfigFieldDiff(last_old, last_new);
    Check(changes.size() == 2, "热重载字段级 diff 数量");
    if (changes.size() == 2) {
        Check(changes[0].name == "threads" && changes[1].name == "name",
              "热重载字段级 diff 名称");
    }

    // 同值再加载不应触发通知（字段级相等判断）。
    Check(config.LoadFromFile(path), "同值再加载");
    Check(notified.load() == 2, "同值不重复通知");

    pool->DelListener(listener);
    (void)std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 场景 10：SO 热更字段级 diff
// ---------------------------------------------------------------------------
void ScenarioSoArtifactDiff() {
    std::printf("[场景10] SO 热更字段级 diff\n");
    go2cpp::config::SoSpec spec;
    spec.name = "libtest";
    spec.group = "g";
    spec.path = "libs/libtest_v1.so";
    spec.sha256 = std::string(64, 'a');
    spec.version = "1";
    spec.symbol_prefix = "libtest_";
    spec.dependencies = {"core", "base"};

    Check(go2cpp::config::SameSoArtifact(spec, spec), "相同 spec 不变");
    Check(go2cpp::config::SoArtifactFieldChanges(spec, spec).empty(),
          "相同 spec 无字段变化");

    go2cpp::config::SoSpec next = spec;
    next.version = "2";
    next.path = "libs/libtest_v2.so";
    next.sha256 = std::string(64, 'b');
    Check(!go2cpp::config::SameSoArtifact(spec, next), "版本变化必须重载");
    const auto fields = go2cpp::config::SoArtifactFieldChanges(spec, next);
    Check(Contains(fields, "version"), "diff 包含 version");
    Check(Contains(fields, "path"), "diff 包含 path");
    Check(Contains(fields, "sha256"), "diff 包含 sha256");
    Check(!Contains(fields, "abi_version"), "abi 未变");

    // 依赖顺序不影响判定（集合语义）。
    go2cpp::config::SoSpec reordered = spec;
    reordered.dependencies = {"base", "core"};
    Check(go2cpp::config::SameSoArtifact(spec, reordered), "依赖顺序无关");

    // 只改 keep_versions 不触发重载。
    go2cpp::config::SoSpec keep = spec;
    keep.keep_versions = 9;
    Check(go2cpp::config::SameSoArtifact(spec, keep), "keep_versions 不触发重载");

    // 走一遍 SoReloader：changed_fields 应记录本次变化的字段。
    auto& config = Config::Instance();
    go2cpp::config::SoValidationOptions options;
    options.check_files = false;
    options.check_hashes = false;
    go2cpp::config::SoReloader reloader(
        config, [](const go2cpp::config::SoSpec&, std::string*) { return true; },
        {}, {}, options);

    go2cpp::config::SoManifest first;
    first.revision = 1;
    first.groups["g"] = {"libtest"};
    go2cpp::config::SoSpec reload_spec = spec;
    reload_spec.dependencies.clear();  // manifest 里没有 core/base，去掉外部依赖
    first.specs["libtest"] = reload_spec;
    const auto first_result = reloader.Reload(first);
    Check(first_result.ok, "首轮 SO 热更成功");
    const auto first_changed = first_result.changed_fields.find("libtest");
    Check(first_changed != first_result.changed_fields.end(), "首轮记录字段");
    Check(first_changed != first_result.changed_fields.end() &&
              first_changed->second.size() ==
                  go2cpp::config::SoArtifactFieldNames().size(),
          "新增 SO 标记全部字段");

    go2cpp::config::SoSpec reload_next = next;
    reload_next.dependencies.clear();
    go2cpp::config::SoManifest second = first;
    second.revision = 2;
    second.specs["libtest"] = reload_next;
    const auto second_result = reloader.Reload(second);
    Check(second_result.ok, "第二轮 SO 热更成功");
    const auto second_changed = second_result.changed_fields.find("libtest");
    Check(second_changed != second_result.changed_fields.end(), "第二轮记录字段");
    if (second_changed != second_result.changed_fields.end()) {
        const auto& changed = second_changed->second;
        Check(Contains(changed, "version"), "第二轮 diff 含 version");
        Check(Contains(changed, "path"), "第二轮 diff 含 path");
        Check(!Contains(changed, "symbol_prefix"), "第二轮 symbol_prefix 未变");
    }
}

// ---------------------------------------------------------------------------
// 场景 11：随机往返
// ---------------------------------------------------------------------------
void ScenarioRandomRoundTrip() {
    std::printf("[场景11] 随机往返（1000 组）\n");
    std::mt19937 engine(20261030);
    std::uniform_int_distribution<int> thread_dist(1, 32);
    std::uniform_int_distribution<int> queue_dist(1, 100000);
    std::uniform_int_distribution<int> char_dist(32, 126);  // 可打印 ASCII
    std::uniform_int_distribution<int> length_dist(0, 12);

    for (int round = 0; round < 1000; ++round) {
        PoolConfig original{};
        original.threads = thread_dist(engine);
        original.queue_limit = static_cast<std::size_t>(queue_dist(engine));
        original.enabled = (round % 2) == 0;
        std::string name;
        const int length = length_dist(engine);
        for (int index = 0; index < length; ++index) {
            name.push_back(static_cast<char>(char_dist(engine)));
        }
        if (!name.empty() && name.front() == '"') {
            name.insert(name.begin(), 'x');  // 覆盖 JSON 字符串转义路径
        }
        original.name = name;

        std::string encoded;
        std::string error;
        if (!ConfigEncode(original, &encoded, &error)) {
            CheckMessage(false, "随机编码失败", error);
            return;
        }
        PoolConfig decoded{};
        if (!ConfigDecode<PoolConfig>(encoded, &decoded, &error)) {
            CheckMessage(false, "随机解码失败", encoded + " " + error);
            return;
        }
        if (!ConfigValuesEqual(original, decoded)) {
            CheckMessage(false, "随机往返不一致", encoded);
            return;
        }

        // 嵌套结构体也做一次。
        ServiceConfig service{};
        service.host = name;
        service.port = thread_dist(engine) * 100;
        service.pool = original;
        service.verbose = (round % 3) == 0;
        std::string service_text;
        if (!ConfigEncode(service, &service_text, &error)) {
            CheckMessage(false, "嵌套随机编码失败", error);
            return;
        }
        ServiceConfig service_decoded{};
        if (!ConfigDecode<ServiceConfig>(service_text, &service_decoded, &error) ||
            !ConfigValuesEqual(service, service_decoded)) {
            CheckMessage(false, "嵌套随机往返不一致", error);
            return;
        }
    }
    Check(true, "随机往返完成");
}

// ---------------------------------------------------------------------------
// 场景 12：并发压测
// ---------------------------------------------------------------------------
void ScenarioConcurrency() {
    std::printf("[场景12] 并发压测（4 线程 x 500 轮）\n");
    ConfigVar<PoolConfig> pool("cfgreflect.concurrent", PoolConfig{}, "并发");
    std::atomic<int> bad{0};
    std::atomic<int> writes{0};
    constexpr int kThreads = 4;
    constexpr int kRounds = 500;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        workers.emplace_back([&pool, &bad, &writes, thread_index] {
            for (int round = 0; round < kRounds; ++round) {
                PoolConfig value{};
                value.threads = 1 + (thread_index * 7 + round) % 32;
                value.name = "t" + std::to_string(thread_index) + "-" +
                             std::to_string(round);
                std::string error;
                if (pool.FromString("{\"threads\":" +
                                        std::to_string(value.threads) +
                                        ",\"name\":\"" + value.name + "\"}",
                                    &error)) {
                    writes.fetch_add(1, std::memory_order_relaxed);
                }
                const PoolConfig current = pool.GetValue();
                if (current.threads < 1 || current.threads > 32) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    Check(bad.load() == 0, "并发读到的值始终合法");
    Check(writes.load() == kThreads * kRounds, "并发写入全部成功");
}

// ---------------------------------------------------------------------------
// 场景 13：编解码吞吐压测
// ---------------------------------------------------------------------------
void ScenarioThroughput() {
    std::printf("[场景13] 编解码吞吐（10 万次）\n");
    PoolConfig value{};
    value.threads = 8;
    value.name = "benchmark";
    std::string encoded;
    Check(ConfigEncode(value, &encoded), "压测编码准备");

    constexpr int kIterations = 100000;
    const auto started = std::chrono::steady_clock::now();
    std::size_t checksum = 0;
    for (int index = 0; index < kIterations; ++index) {
        PoolConfig decoded{};
        if (!ConfigDecode<PoolConfig>(encoded, &decoded)) {
            Check(false, "压测解码失败");
            return;
        }
        checksum += static_cast<std::size_t>(decoded.threads);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    const double ops = elapsed > 0
                           ? static_cast<double>(kIterations) * 1e6 /
                                 static_cast<double>(elapsed)
                           : 0.0;
    std::printf("[场景13] decode %d 次耗时 %lld us，吞吐 %.0f ops/s（checksum=%zu）\n",
                kIterations, static_cast<long long>(elapsed), ops, checksum);
    Check(checksum == static_cast<std::size_t>(8) * kIterations, "压测结果校验");
    Check(ops > 1000.0, "解码吞吐应高于 1000 ops/s");
}

}  // namespace

int main() {
    std::printf("=== Config 文本编解码（Reflect）测试 ===\n");
    ScenarioLegacyScalars();
    ScenarioVectors();
    ScenarioStructDefaults();
    ScenarioNestedRoundTrip();
    ScenarioErrors();
    ScenarioValidators();
    ScenarioFieldDiff();
    ScenarioRegistryRoundTrip();
    ScenarioHotReload();
    ScenarioSoArtifactDiff();
    ScenarioRandomRoundTrip();
    ScenarioConcurrency();
    ScenarioThroughput();

    if (g_failures == 0) {
        std::printf("\n[cfgreflect] failures=0\n");
        return 0;
    }
    std::printf("\n[cfgreflect] failures=%d\n", g_failures);
    return 1;
}
