#include "go2cpp/config.hpp"

#include <iostream>
#include <string>

/**
 * @brief config 简单版：注册默认值，再用 INI 覆盖它。
 * @details 这就是"程序内部自己加载"的主线：
 *          ① Config::Lookup 注册一个带默认值的配置项；
 *          ② 同名再次 Lookup 返回同一个实例，不会把值重置回默认；
 *          ③ LoadFromIni（热重载内部走的同一条路径）覆盖已注册的值。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp::config;
    bool ok = true;
    auto& config = Config::Instance();

    // ① 注册默认值
    std::clog << "[config-center][场景1] Lookup 注册默认值\n";
    auto port = config.Lookup<int>("demo.port", 8080, "监听端口");
    auto host = config.Lookup<std::string>("demo.host", "127.0.0.1", "监听地址");
    std::cout << "  port=" << port->GetValue() << " host=" << host->GetValue()
              << "（预期 8080 127.0.0.1）\n";
    ok = ok && port->GetValue() == 8080 && host->GetValue() == "127.0.0.1";

    // ② 同名再次 Lookup 返回同一实例
    std::clog << "[config-center][场景2] Lookup 幂等\n";
    (void)port->SetValue(9000);
    auto again = config.Lookup<int>("demo.port", 1111, "另一段描述");
    const bool same_object = (again == port);
    const bool value_kept = again->GetValue() == 9000;
    std::cout << "  同一实例=" << same_object << " 值未被重置=" << value_kept
              << "（预期 1 1，第二次 Lookup 的默认值 1111 不生效）\n";
    ok = ok && same_object && value_kept;

    // ③ 用 INI 覆盖已注册的值（热重载内部走同一条路径）
    std::clog << "[config-center][场景3] LoadFromIni 覆盖已注册值\n";
    IniFile ini;
    (void)ini.Parse("[demo]\nport = 7070\nhost = 0.0.0.0\n");
    std::string error;
    const bool loaded = config.LoadFromIni(ini, &error);
    std::cout << "  加载=" << loaded << " port=" << port->GetValue()
              << " host=" << host->GetValue() << " error=\"" << error
              << "\"（预期 1 7070 0.0.0.0）\n";
    ok = ok && loaded && port->GetValue() == 7070 && host->GetValue() == "0.0.0.0";

    std::clog << (ok ? "[config-center] 全部通过\n" : "[config-center] 有失败\n");
    return ok ? 0 : 1;
}
