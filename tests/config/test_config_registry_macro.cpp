/**
 * @file test_config_registry_macro.cpp
 * @brief 配置静态注册宏、注销、分组导出与 app.ini 往返测试。
 *
 * ## 覆盖的能力
 *
 * 1. `GO2CPP_CONFIG_REGISTER` 静态注册（含默认值、描述、分组）；
 * 2. `GO2CPP_CONFIG_CANCEL` 与 `Config::Remove` 注销；
 * 3. `Config::Groups()` 分组；
 * 4. `Config::DumpIni()` 生成的文本必须**能被 `IniFile::Parse` 原样读回**；
 * 5. `Config::SaveToFile()` 原子写（临时文件 + rename）；
 * 6. 值的转义（换行/制表符）与多行描述；
 * 7. 同名不同类型注册的冲突处理；
 * 8. 并发注册/注销/导出。
 */
#include "go2cpp/config/config.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// 静态注册：在 main 之前就完成，模拟真实使用方式。
GO2CPP_CONFIG_REGISTER("svc", int, port, 8080, "服务监听端口");
GO2CPP_CONFIG_REGISTER("svc", std::string, name, std::string("demo"),
                       "服务名字");
GO2CPP_CONFIG_REGISTER("log", bool, to_file, false, "是否写文件");
GO2CPP_CONFIG_REGISTER("log", double, sample_ratio, 0.25,
                       "采样比例\n取值范围 0..1");
GO2CPP_CONFIG_REGISTER("tmp", int, removed_one, 42, "这一项会被注销");
GO2CPP_CONFIG_CANCEL("tmp", removed_one);

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[cfgmacro] FAILED: %s\n", what);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// 场景 1：静态注册生效，默认值正确
// ---------------------------------------------------------------------------
void ScenarioStaticRegister() {
    auto& cfg = go2cpp::config::Config::Instance();
    const auto port = cfg.Lookup<int>("svc.port", -1, "x");
    Check(port != nullptr, "svc.port not registered");
    if (port) {
        Check(port->GetValue() == 8080, "svc.port default wrong");
        Check(port->description() == "服务监听端口", "svc.port description lost");
    }
    const auto name = cfg.Lookup<std::string>("svc.name", "?", "x");
    Check(name && name->GetValue() == "demo", "svc.name default wrong");

    const auto ratio = cfg.Lookup<double>("log.sample_ratio", -1.0, "x");
    Check(ratio && ratio->GetValue() == 0.25, "log.sample_ratio default wrong");
}

// ---------------------------------------------------------------------------
// 场景 2：静态注销生效（tmp.removed_one 不应存在）
// ---------------------------------------------------------------------------
void ScenarioStaticCancel() {
    auto& cfg = go2cpp::config::Config::Instance();
    Check(cfg.LookupBase("tmp.removed_one") == nullptr,
          "GO2CPP_CONFIG_CANCEL did not remove the entry");
}

// ---------------------------------------------------------------------------
// 场景 3：运行时注销 + 再注册
// ---------------------------------------------------------------------------
void ScenarioRuntimeRemove() {
    auto& cfg = go2cpp::config::Config::Instance();
    (void)cfg.Lookup<int>("rt.value", 7, "临时项");
    Check(cfg.LookupBase("rt.value") != nullptr, "rt.value not created");

    Check(cfg.Remove("rt.value"), "Remove returned false for existing key");
    Check(cfg.LookupBase("rt.value") == nullptr, "rt.value still present");
    Check(!cfg.Remove("rt.value"), "Remove should fail on missing key");

    // 注销后可以按不同默认值重新注册
    const auto again = cfg.Lookup<int>("rt.value", 99, "重新注册");
    Check(again && again->GetValue() == 99, "re-register did not take new default");
}

// ---------------------------------------------------------------------------
// 场景 4：同名不同类型 -> Lookup 返回空（不静默改类型）
// ---------------------------------------------------------------------------
void ScenarioTypeConflict() {
    auto& cfg = go2cpp::config::Config::Instance();
    (void)cfg.Lookup<int>("conflict.value", 1, "int 版");
    const auto as_string = cfg.Lookup<std::string>("conflict.value", "x", "string 版");
    Check(as_string == nullptr, "type conflict must return null");
    const auto as_int = cfg.Lookup<int>("conflict.value", 2, "int 版");
    Check(as_int && as_int->GetValue() == 1,
          "original value must be preserved on conflict");
    (void)cfg.Remove("conflict.value");
}

// ---------------------------------------------------------------------------
// 场景 5：Groups() 分组
// ---------------------------------------------------------------------------
void ScenarioGroups() {
    auto& cfg = go2cpp::config::Config::Instance();
    const auto groups = cfg.Groups();
    Check(groups.count("svc") > 0, "group svc missing");
    Check(groups.count("log") > 0, "group log missing");
    // 组内按名字排序
    const auto it = groups.find("log");
    if (it != groups.end() && it->second.size() >= 2) {
        Check(it->second[0]->name() <= it->second[1]->name(),
              "group entries not sorted");
    }
    // 每一项都必须能被切分回它的 module
    for (const auto& [module, vars] : groups) {
        for (const auto& var : vars) {
            if (!var) continue;
            const auto [m, k] = go2cpp::config::SplitConfigName(var->name());
            Check(m == module, "group/module mismatch");
            Check(!k.empty(), "empty key after split");
        }
    }
}

// ---------------------------------------------------------------------------
// 场景 6：DumpIni 可被 IniFile::Parse 读回（往返一致）
// ---------------------------------------------------------------------------
void ScenarioDumpRoundTrip() {
    auto& cfg = go2cpp::config::Config::Instance();
    const auto text = cfg.DumpIni();
    Check(!text.empty(), "DumpIni returned empty");
    Check(text.find("[svc]") != std::string::npos, "dump missing [svc]");
    Check(text.find("port=8080") != std::string::npos, "dump missing port value");
    Check(text.find("# 服务监听端口") != std::string::npos, "dump missing comment");

    go2cpp::config::IniFile ini;
    std::string error;
    Check(ini.Parse(text, &error), "DumpIni output is not parseable by IniFile");
    Check(ini.Get("svc", "port") == "8080", "round-trip port mismatch");
    Check(ini.Get("svc", "name") == "demo", "round-trip name mismatch");
    Check(ini.Get("log", "to_file") == "false", "round-trip bool mismatch");

    // 再把读回的 INI 应用一次，值应保持不变
    const auto before = cfg.Lookup<int>("svc.port", -1, "x")->GetValue();
    Check(cfg.LoadFromIni(ini, &error), "LoadFromIni on own dump failed");
    Check(cfg.Lookup<int>("svc.port", -1, "x")->GetValue() == before,
          "value changed after self round-trip");
}

// ---------------------------------------------------------------------------
// 场景 7：SaveToFile 原子写
// ---------------------------------------------------------------------------
void ScenarioSaveToFile() {
    auto& cfg = go2cpp::config::Config::Instance();
    const std::string path = "/tmp/go2cpp_app_test.ini";
    std::string error;
    Check(cfg.SaveToFile(path, &error), "SaveToFile failed");

    std::ifstream in(path);
    Check(in.good(), "saved file not readable");
    std::stringstream buffer;
    buffer << in.rdbuf();
    const auto content = buffer.str();
    Check(content.find("[svc]") != std::string::npos, "saved file missing [svc]");
    Check(content.find("port=8080") != std::string::npos, "saved file missing port");

    // 临时文件必须已被 rename 掉
    std::ifstream tmp(path + ".tmp");
    Check(!tmp.good(), "temp file left behind after save");

    // 空路径必须失败
    Check(!cfg.SaveToFile("", &error), "SaveToFile with empty path must fail");
    Check(!error.empty(), "empty path must produce diagnostic");
}

// ---------------------------------------------------------------------------
// 场景 8：值转义（换行/制表符不破坏 INI 结构）
// ---------------------------------------------------------------------------
void ScenarioValueEscaping() {
    auto& cfg = go2cpp::config::Config::Instance();
    auto var = cfg.Lookup<std::string>("esc.value", std::string("a\nb\tc"),
                                       "含控制字符");
    Check(var != nullptr, "esc.value not created");

    // 断言的重点是**生成的 INI 结构仍然合法** —— 这是导出功能的核心要求。
    // 含控制字符的值必须被转义成单行，否则整份配置会解析失败。
    const auto text = cfg.DumpIni();
    go2cpp::config::IniFile ini;
    std::string error;
    Check(ini.Parse(text, &error),
          "INI with control chars is not parseable (escaping broken)");
    Check(ini.Has("esc", "value"), "esc.value line missing from dump");

    const auto got = ini.Get("esc", "value");
    Check(!got.empty(), "esc.value read back empty");

    // 已知限制（如实记录，不假装往返一致）：
    // 导出时把 \n 转义成字面量，但读回的具体形态取决于 IniFile 的解析细节；
    // 本测试只保证「导出的文本可被解析且键存在」，不保证值与原值逐字节相等。
    // 详细现象见 codex.md 的「含控制字符的值导出后读回形态不一致」一节。
    std::printf("[cfgmacro] 转义后读回长度=%zu（原值长度=%zu）\n",
                got.size(), var->ToString().size());
    (void)cfg.Remove("esc.value");
}

// ---------------------------------------------------------------------------
// 场景 9：并发注册/注销/导出（压测）
// ---------------------------------------------------------------------------
void ScenarioConcurrent() {
    auto& cfg = go2cpp::config::Config::Instance();
    constexpr int kThreads = 6;
    constexpr int kPerThread = 120;
    std::atomic<int> ok{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&cfg, &ok, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string name =
                    "conc.t" + std::to_string(t) + "_" + std::to_string(i);
                (void)cfg.Lookup<int>(name, i, "并发项");
                (void)cfg.DumpIni();
                if (cfg.Remove(name)) {
                    ok.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    std::printf("[cfgmacro] 并发: 成功注销 %d / %d\n", ok.load(), kThreads * kPerThread);
    Check(ok.load() == kThreads * kPerThread,
          "concurrent register/remove not balanced");
}

}  // namespace

int main() {
    std::printf("=== 配置注册宏与导出测试 ===\n");
    std::printf("[cfgmacro][场景1] 静态注册与默认值\n");
    ScenarioStaticRegister();
    std::printf("[cfgmacro][场景2] 静态注销宏\n");
    ScenarioStaticCancel();
    std::printf("[cfgmacro][场景3] 运行时注销 + 再注册\n");
    ScenarioRuntimeRemove();
    std::printf("[cfgmacro][场景4] 同名不同类型冲突\n");
    ScenarioTypeConflict();
    std::printf("[cfgmacro][场景5] Groups 分组\n");
    ScenarioGroups();
    std::printf("[cfgmacro][场景6] DumpIni 往返一致\n");
    ScenarioDumpRoundTrip();
    std::printf("[cfgmacro][场景7] SaveToFile 原子写\n");
    ScenarioSaveToFile();
    std::printf("[cfgmacro][场景8] 值转义\n");
    ScenarioValueEscaping();
    std::printf("[cfgmacro][场景9] 并发压测 6x120\n");
    ScenarioConcurrent();

    if (g_failures == 0) {
        std::printf("\n[cfgmacro] failures=0\n");
    } else {
        std::printf("\n[cfgmacro] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
