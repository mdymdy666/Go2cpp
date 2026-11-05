// config 模块微基准：INI 加载（1e4 键）与键查找。
//
// 口径：
//   - config_parse_10k：IniFile::Parse() 解析一份 100 段 × 100 键 = 1e4 键的
//     文本，一次 op = 整份文件加载（报告里换算 keys/s = ops_per_sec * 1e4）；
//   - config_get：IniFile::Get(section, key) 一次查找（返回 std::string）。
// 说明：Config::LoadFromIni 只接受已静态注册的配置项，无法凭空加载 1e4 个
// 未注册键；这里测的是它唯一的语法层前置 —— IniFile 解析，口径等价于
// 「配置加载的解析吞吐」。

#include <cstddef>
#include <string>

#include "bench_common.hpp"
#include "go2cpp/config.hpp"

int main() {
    constexpr int kSections = 100;
    constexpr int kKeysPerSection = 100;
    constexpr std::size_t kKeys = static_cast<std::size_t>(kSections) * kKeysPerSection;

    std::string text;
    text.reserve(kKeys * 24);
    for (int s = 0; s < kSections; ++s) {
        text += "[section";
        text += std::to_string(s);
        text += "]\n";
        for (int k = 0; k < kKeysPerSection; ++k) {
            text += "key";
            text += std::to_string(k);
            text += " = value";
            text += std::to_string(s * kKeysPerSection + k);
            text += "\n";
        }
    }

    {
        go2cpp::config::IniFile probe;
        std::string error;
        if (!probe.Parse(text, &error)) {
            std::printf("config_parse_error=%s\n", error.c_str());
            return 1;
        }
        if (probe.Get("section0", "key0").empty()) {
            std::printf("config_probe_failed=1\n");
            return 1;
        }
    }

    bench::RunAdaptive("config_parse_10k", 2, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            go2cpp::config::IniFile ini;
            if (!ini.Parse(text)) {
                acc += 1;
            }
            acc += ini.Get("section42", "key77").size();
        }
        bench::g_sink = acc;
    });

    go2cpp::config::IniFile ini;
    (void)ini.Parse(text);
    bench::RunAdaptive("config_get", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += ini.Get("section42", "key77").size();
        }
        bench::g_sink = acc;
    });
    return 0;
}
