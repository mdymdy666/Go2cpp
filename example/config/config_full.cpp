#include "go2cpp/config.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

// config 全面版：同一个组件的 7 个使用场景。
//   1  INI 分段解析         多个 [section] 与注释行
//   2  INI 默认值           缺失键返回调用方给的默认值
//   3  ConfigVar 读写       名字/描述/取值/设值
//   4  ★ 字符串往返         ToString 与 FromString 互为逆操作
//   5  多种值类型           int / bool / string / vector<T>
//   6  ★ 校验器             SetValidator 可拒绝非法值，且拒绝后值不变
//   7  ★ 变更监听           AddListener 在值变化时收到 (old, new)，可注销
//
// 说明：本示例只使用 IniFile / ConfigVar<T> / ValueCodec<T>，
//       不涉及 RuntimeConfig 的待定(pending)与回滚路径。

namespace {

using go2cpp::config::ConfigVar;
using go2cpp::config::IniFile;

bool scenario_ini_sections() {
    std::clog << "[full][场景1] INI 分段解析\n";
    const std::string text =
        "# 井号注释\n"
        "[a]\n"
        "one = 1\n"
        "two = 2\n"
        "[b]\n"
        "one = 11\n";
    IniFile ini;
    const bool parsed = ini.Parse(text);
    const bool ok = parsed &&
                    ini.Get("a", "one", "") == "1" &&
                    ini.Get("a", "two", "") == "2" &&
                    ini.Get("b", "one", "") == "11";
    std::cout << "  段 a.one=" << ini.Get("a", "one", "?")
              << " a.two=" << ini.Get("a", "two", "?")
              << " b.one=" << ini.Get("b", "one", "?")
              << "（预期 1 2 11，同名键按段隔离）\n";
    return ok;
}

bool scenario_ini_default() {
    std::clog << "[full][场景2] 缺失键返回默认值\n";
    IniFile ini;
    (void)ini.Parse("[s]\npresent = yes\n");
    const bool ok = ini.Get("s", "present", "D") == "yes" &&
                    ini.Get("s", "absent", "D") == "D" &&
                    ini.Get("nosection", "present", "D2") == "D2";
    std::cout << "  存在键=" << ini.Get("s", "present", "D")
              << " 缺失键=" << ini.Get("s", "absent", "D")
              << " 缺失段=" << ini.Get("nosection", "present", "D2")
              << "（预期 yes D D2）\n";
    return ok;
}

bool scenario_config_var_basic() {
    std::clog << "[full][场景3] ConfigVar 元信息与读写\n";
    ConfigVar<std::string> name("app-name", "demo", "应用名");
    const bool initial = name.GetValue() == "demo";
    const bool set_ok = name.SetValue("service");
    const bool ok = initial && set_ok && name.GetValue() == "service" &&
                    name.name() == "app-name" &&
                    name.description() == "应用名";
    std::cout << "  名字=" << name.name() << " 描述=" << name.description()
              << " 值=" << name.GetValue() << "（预期 app-name 应用名 service）\n";
    return ok;
}

bool scenario_string_round_trip() {
    std::clog << "[full][场景4] ToString 与 FromString 互为逆操作\n";
    ConfigVar<int> number("n", 0, "整数");
    ConfigVar<bool> flag("b", false, "开关");
    ConfigVar<std::string> text("t", "", "文本");

    (void)number.SetValue(-42);
    (void)flag.SetValue(true);
    (void)text.SetValue("hello world");

    const auto n_enc = number.ToString();
    const auto b_enc = flag.ToString();
    const auto t_enc = text.ToString();

    // 用另一个实例解码，验证编码是自洽的
    ConfigVar<int> n2("n2", 0, "");
    ConfigVar<bool> b2("b2", false, "");
    ConfigVar<std::string> t2("t2", "", "");
    const bool n_ok = n2.FromString(n_enc) && n2.GetValue() == -42;
    const bool b_ok = b2.FromString(b_enc) && b2.GetValue();
    const bool t_ok = t2.FromString(t_enc) && t2.GetValue() == "hello world";

    std::cout << "  int 编码=\"" << n_enc << "\" bool 编码=\"" << b_enc
              << "\" string 编码=\"" << t_enc << "\" 往返=" << (n_ok && b_ok && t_ok)
              << "（预期 -42 true hello world，往返 1）\n";
    return n_ok && b_ok && t_ok;
}

bool scenario_value_types() {
    std::clog << "[full][场景5] 多种值类型（含 vector）\n";
    ConfigVar<std::vector<int>> list("list", {}, "整数列表");
    const bool set_ok = list.SetValue({1, 2, 3, 4});
    const auto encoded = list.ToString();

    ConfigVar<std::vector<int>> decoded("list2", {}, "");
    const bool decode_ok = decoded.FromString(encoded);
    const auto value = decoded.GetValue();

    const bool ok = set_ok && decode_ok && value.size() == 4 &&
                    value[0] == 1 && value[3] == 4;
    std::cout << "  编码=\"" << encoded << "\" 解码后元素数=" << value.size()
              << " 首=" << (value.empty() ? -1 : value[0])
              << " 末=" << (value.empty() ? -1 : value.back())
              << "（预期 4 个元素，1 和 4）\n";
    return ok;
}

bool scenario_validator() {
    std::clog << "[full][场景6] 校验器拒绝非法值，且拒绝后值不变\n";
    ConfigVar<int> port("port", 8080, "端口");
    port.SetValidator([](const int& value, std::string* error) {
        if (value < 1 || value > 65535) {
            if (error) {
                *error = "端口必须在 1..65535";
            }
            return false;
        }
        return true;
    });

    std::string error;
    const bool good = port.SetValue(9090, &error);
    const int after_good = port.GetValue();

    error.clear();
    const bool bad = port.SetValue(70000, &error);
    const int after_bad = port.GetValue();

    const bool ok = good && after_good == 9090 && !bad && after_bad == 9090 &&
                    !error.empty();
    std::cout << "  合法设置=" << good << " 值=" << after_good
              << " 非法设置=" << bad << " 值仍为=" << after_bad
              << " 错误=\"" << error << "\"（预期 1 9090 0 9090）\n";
    return ok;
}

bool scenario_listener() {
    std::clog << "[full][场景7] 变更监听与注销\n";
    ConfigVar<int> value("v", 1, "值");
    int calls = 0;
    int last_old = 0;
    int last_new = 0;
    const auto id = value.AddListener([&](const int& old_value, const int& new_value) {
        ++calls;
        last_old = old_value;
        last_new = new_value;
    });

    (void)value.SetValue(2);
    (void)value.SetValue(3);
    const int after_two_sets = calls;
    const int seen_old = last_old;
    const int seen_new = last_new;

    value.DelListener(id);
    (void)value.SetValue(4);
    const int after_del = calls;

    const bool ok = after_two_sets == 2 && after_del == 2 &&
                    seen_old == 2 && seen_new == 3 && value.GetValue() == 4;
    std::cout << "  两次设值回调次数=" << after_two_sets
              << " 最后一次 (old,new)=(" << seen_old << "," << seen_new << ")"
              << " 注销后再设值次数=" << after_del
              << " 当前值=" << value.GetValue()
              << "（预期 2 (2,3) 2 4）\n";
    return ok;
}

}  // namespace

/**
 * @brief config 全面版：7 个场景，覆盖 INI 解析、强类型项、校验与监听。
 * @details 每个场景一个函数，独立可读；全部通过才返回 0。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_ini_sections() && ok;
    ok = scenario_ini_default() && ok;
    ok = scenario_config_var_basic() && ok;
    ok = scenario_string_round_trip() && ok;
    ok = scenario_value_types() && ok;
    ok = scenario_validator() && ok;
    ok = scenario_listener() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}