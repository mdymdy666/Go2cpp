#include "go2cpp/config.hpp"

#include <iostream>
#include <string>

/**
 * @brief RuntimeConfig 简单版：从内存 INI 快照构造并校验运行时配置。
 * @details ① FromIni —— 由 IniFile 构造 RuntimeConfig（不读文件）；
 *          ② 读取 scheduler 与 log 两组字段；
 *          ③ Validate —— 边界校验，越界配置被拒。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp::config;
    bool ok = true;

    // ① 用一段内存文本构造 IniFile，再喂给 FromIni（不接触任何文件）
    std::clog << "[runtime-config][场景1] FromIni 从 INI 快照构造\n";
    IniFile ini;
    const bool parsed = ini.Parse(
        "[scheduler]\n"
        "processor_count = 4\n"
        "min_workers = 1\n"
        "max_workers = 8\n"
        "local_queue_limit = 512\n"
        "\n"
        "[log]\n"
        "level = info\n"
        "stdout = true\n"
        "directory = mylog\n");
    RuntimeConfig config;
    std::string error;
    const bool loaded = parsed && RuntimeConfig::FromIni(ini, &config, &error);
    std::cout << "  解析=" << parsed << " FromIni=" << loaded
              << " P=" << config.scheduler.processor_count
              << " min=" << config.scheduler.min_workers
              << " max=" << config.scheduler.max_workers
              << " error=\"" << error << "\"（预期 1 1 4 1 8）\n";
    ok = ok && parsed && loaded && config.scheduler.processor_count == 4 &&
         config.scheduler.min_workers == 1 && config.scheduler.max_workers == 8;

    // ② scheduler 与 log 两组字段都能读到
    std::clog << "[runtime-config][场景2] 读取 scheduler 与 log 字段\n";
    const bool fields_ok = config.scheduler.local_queue_limit == 512 &&
                           config.log_level == go2cpp::log::Level::Info &&
                           config.log_stdout && config.log_directory == "mylog";
    std::cout << "  queue=" << config.scheduler.local_queue_limit
              << " level=" << static_cast<int>(config.log_level)
              << " stdout=" << config.log_stdout
              << " dir=" << config.log_directory
              << "（预期 512 / info / 1 / mylog）\n";
    ok = ok && fields_ok;

    // ③ 合法快照通过 Validate，越界快照被拒
    std::clog << "[runtime-config][场景3] Validate 边界\n";
    std::string valid_error;
    const bool valid_ok = config.Validate(&valid_error);

    IniFile bad_ini;
    (void)bad_ini.Parse("[scheduler]\nprocessor_count = 99\n");   // 显式范围 1..32
    RuntimeConfig bad_config;
    std::string bad_error;
    const bool bad_loaded = RuntimeConfig::FromIni(bad_ini, &bad_config, &bad_error);
    const bool bad_valid = bad_loaded && bad_config.Validate(&bad_error);

    std::cout << "  合法配置通过=" << valid_ok << " 越界配置（P=99）通过=" << bad_valid
              << " 错误=\"" << bad_error << "\"（预期 1 0）\n";
    ok = ok && valid_ok && !bad_valid;

    std::clog << (ok ? "[runtime-config] 全部通过\n" : "[runtime-config] 有失败\n");
    return ok ? 0 : 1;
}
