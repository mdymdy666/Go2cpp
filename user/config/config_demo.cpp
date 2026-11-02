#include "go2cpp/config.hpp"

#include <iostream>
#include <string>

/**
 * @brief config 简单版：解析 INI、读取配置项、用 ConfigVar 读写强类型值。
 * @details ① IniFile::Parse —— 从文本解析 [section] key=value；
 *          ② IniFile::Get / Has —— 按键读取，支持默认值；
 *          ③ ConfigVar<T> —— 带名字与校验的强类型配置项，可转成字符串往返。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp::config;
    bool ok = true;

    // ① 解析一段 INI 文本
    std::clog << "[config][场景1] 解析 INI\n";
    const std::string text =
        "; 注释行\n"
        "[server]\n"
        "port = 8080\n"
        "host = 127.0.0.1\n"
        "\n"
        "[log]\n"
        "level = info\n";
    IniFile ini;
    std::string error;
    const bool parsed = ini.Parse(text, &error);
    std::cout << "  解析成功=" << parsed << "（预期 1）\n";
    ok = ok && parsed;

    // ② 按键读取；不存在的键返回默认值
    std::clog << "[config][场景2] 读取与默认值\n";
    const auto port = ini.Get("server", "port", "0");
    const auto host = ini.Get("server", "host", "localhost");
    const auto missing = ini.Get("server", "absent", "fallback");
    const bool has_port = ini.Has("server", "port");
    const bool has_absent = ini.Has("server", "absent");
    std::cout << "  port=" << port << " host=" << host
              << " 缺失键默认值=" << missing
              << " Has(port)=" << has_port << " Has(absent)=" << has_absent
              << "（预期 8080 127.0.0.1 fallback 1 0）\n";
    ok = ok && port == "8080" && host == "127.0.0.1" && missing == "fallback" &&
         has_port && !has_absent;

    // ③ ConfigVar 强类型读写与字符串往返
    std::clog << "[config][场景3] ConfigVar 读写与往返\n";
    ConfigVar<int> workers("workers", 4, "工作线程数");
    const bool set_ok = workers.SetValue(8);
    const auto encoded = workers.ToString();
    // ★ 注意求值顺序：必须在调用 FromString 之前先把值取出来。
    //   若把 GetValue() 与 FromString() 写在同一个 << 链里，
    //   打印出来的会是修改之后的值（本示例首版就踩了这个坑）。
    const int after_set = workers.GetValue();
    const bool from_string = workers.FromString("16");
    const int after_from_string = workers.GetValue();
    std::cout << "  名字=" << workers.name() << " 设置后=" << after_set
              << " 编码=" << encoded << " 从字符串设为=" << after_from_string
              << "（预期 workers 8 8 16）\n";
    ok = ok && set_ok && workers.name() == "workers" && encoded == "8" &&
         from_string && after_set == 8 && after_from_string == 16;

    std::clog << (ok ? "[config] 全部通过\n" : "[config] 有失败\n");
    return ok ? 0 : 1;
}
