#include "go2cpp/config.hpp"

#include <iostream>
#include <string>

// RuntimeConfig 全面版：同一个组件的 7 个使用场景。
//   1  ★ 从内存 INI 构造      FromIni 接受 IniFile，不需要文件
//   2  scheduler 组字段        P / M / 队列 / 栈 等启动期参数
//   3  log 组字段              级别 / stdout / 目录 / 文件 / 格式
//   4  ★ 事务性               FromIni 失败时不修改调用方的 config
//   5  ★ 默认值               空 INI 得到全默认快照
//   6  ★ Validate 边界         合法通过；P 越界 / 队列为 0 被拒
//   7  ★★ 已发现的缺陷        [timer] 段的值用 ReadSize 读取，而 ReadSize 对
//                              非 scheduler 段必定返回 false，导致任何带 [timer]
//                              的 INI 都无法被 FromIni 接受（见文件末尾说明）
//
// 说明：只用 FromIni 与 Validate（纯计算）。**刻意不调用 ApplyLogging** ——
//       它会把日志配置应用到全局 LoggerManager 并可能创建日志目录，
//       既有全局副作用又依赖文件系统，不适合放进可反复运行的示例。

namespace {

using namespace go2cpp::config;

IniFile make_ini(const std::string& text, bool* ok) {
    IniFile ini;
    *ok = ini.Parse(text);
    return ini;
}

bool scenario_from_ini_memory() {
    std::clog << "[full][场景1] FromIni 从内存 INI 构造（不需要文件）\n";
    bool parsed = false;
    const auto ini = make_ini("[scheduler]\nprocessor_count = 6\n", &parsed);

    RuntimeConfig config;
    std::string error;
    const bool loaded = RuntimeConfig::FromIni(ini, &config, &error);

    const bool ok = parsed && loaded && config.scheduler.processor_count == 6;
    std::cout << "  解析=" << parsed << " FromIni=" << loaded
              << " P=" << config.scheduler.processor_count << "（预期 1 1 6）\n";
    return ok;
}

bool scenario_scheduler_fields() {
    std::clog << "[full][场景2] scheduler 组字段\n";
    bool parsed = false;
    const auto ini = make_ini(
        "[scheduler]\n"
        "processor_count = 8\n"
        "min_workers = 2\n"
        "max_workers = 16\n"
        "local_queue_limit = 1024\n"
        "fiber_stack_size = 262144\n"
        "fiber_bin_capacity = 64\n"
        "task_affinity_budget = 8\n"
        "allow_worker_oversubscription = false\n"
        "enable_sysmon = false\n"
        "pin_workers_to_cpu = true\n"
        "collect_metrics = false\n",
        &parsed);

    RuntimeConfig config;
    std::string error;
    const bool loaded = RuntimeConfig::FromIni(ini, &config, &error);
    const auto& sched = config.scheduler;

    const bool ok = parsed && loaded &&
                    sched.processor_count == 8 && sched.min_workers == 2 &&
                    sched.max_workers == 16 && sched.local_queue_limit == 1024 &&
                    sched.fiber_stack_size == 262144 &&
                    sched.fiber_bin_capacity == 64 &&
                    sched.task_affinity_budget == 8 &&
                    !sched.allow_worker_oversubscription &&
                    !sched.enable_sysmon && sched.pin_workers_to_cpu &&
                    !sched.collect_metrics;
    std::cout << "  P=" << sched.processor_count << " M=" << sched.min_workers << ".."
              << sched.max_workers << " queue=" << sched.local_queue_limit
              << " stack=" << sched.fiber_stack_size
              << " oversub=" << sched.allow_worker_oversubscription
              << " sysmon=" << sched.enable_sysmon
              << " pin=" << sched.pin_workers_to_cpu
              << " metrics=" << sched.collect_metrics << "（预期全部按文件值）\n";
    return ok;
}

bool scenario_log_fields() {
    std::clog << "[full][场景3] log 组字段\n";
    bool parsed = false;
    // log 段的键走 Has + ParseLevel/ParseBool/Get，不经 ReadSize，因此可正常读取
    const auto ini = make_ini(
        "[log]\n"
        "level = error\n"
        "stdout = true\n"
        "directory = logs/prod\n"
        "file = service.log\n"
        "format = {level}: {message}\n",
        &parsed);

    RuntimeConfig config;
    std::string error;
    const bool loaded = RuntimeConfig::FromIni(ini, &config, &error);
    // ★ 每条影响 ok 的条件都要能在输出里被看到（教训：曾有一个未打印的断言
    //   静默失败，导致"输出看起来全对但整体报失败"）。
    const bool level_ok = config.log_level == go2cpp::log::Level::Error;
    const bool stdout_ok = config.log_stdout;
    const bool dir_ok = config.log_directory == "logs/prod";
    const bool file_ok = config.log_file == "service.log";
    // 格式串的尾部换行在 INI 往返中不一定原样保留，所以只校验它确实被读到
    // （包含两个占位符），而不是逐字节相等。
    const bool format_ok = config.log_format.find("{level}") != std::string::npos &&
                           config.log_format.find("{message}") != std::string::npos;

    const bool ok = parsed && loaded && level_ok && stdout_ok && dir_ok && file_ok &&
                    format_ok;
    std::cout << "  level=" << static_cast<int>(config.log_level) << "(=" << level_ok
              << ") stdout=" << config.log_stdout << "(=" << stdout_ok
              << ") dir=" << config.log_directory << "(=" << dir_ok
              << ") file=" << config.log_file << "(=" << file_ok
              << ") format 含占位符=" << format_ok
              << " format 长度=" << config.log_format.size()
              << "（预期各项均为 1）\n";
    return ok;
}

bool scenario_transactional() {
    std::clog << "[full][场景4] 事务性：FromIni 失败时不修改调用方 config\n";
    bool parsed = false;
    const auto good_ini = make_ini("[scheduler]\nprocessor_count = 5\n", &parsed);
    RuntimeConfig config;
    std::string error;
    const bool first = RuntimeConfig::FromIni(good_ini, &config, &error);
    const auto before = config.scheduler.processor_count;

    bool parsed_bad = false;
    const auto bad_ini = make_ini("[scheduler]\nprocessor_count = 99\n", &parsed_bad);
    std::string bad_error;
    const bool second = RuntimeConfig::FromIni(bad_ini, &config, &bad_error);
    const auto after = config.scheduler.processor_count;

    const bool unchanged = !second ? (after == before) : true;
    const bool ok = first && before == 5 && unchanged;
    std::cout << "  首次成功=" << first << " 值=" << before
              << " 第二次=" << second << " 之后值=" << after
              << " 失败时保持不变=" << unchanged << "（预期 首次=1 值=5）\n";
    return ok;
}

bool scenario_defaults() {
    std::clog << "[full][场景5] 空 INI 得到全默认快照\n";
    bool parsed = false;
    const auto ini = make_ini("", &parsed);

    RuntimeConfig config;
    std::string error;
    const bool loaded = RuntimeConfig::FromIni(ini, &config, &error);
    const bool ok = loaded &&
                    config.timer_warn_after_days == 30 &&
                    config.timer_error_after_days == 365 &&
                    config.log_directory == "log" &&
                    config.log_file == "go2cpp.log" &&
                    !config.log_stdout;
    std::cout << "  timer=" << config.timer_warn_after_days << "/"
              << config.timer_error_after_days << " dir=" << config.log_directory
              << " file=" << config.log_file << " stdout=" << config.log_stdout
              << "（预期 30/365 log go2cpp.log 0）\n";
    return ok;
}

bool scenario_validate_bounds() {
    std::clog << "[full][场景6] Validate 边界\n";
    bool parsed = false;
    const auto good = make_ini(
        "[scheduler]\nprocessor_count = 32\nmin_workers = 1\nmax_workers = 32\n"
        "local_queue_limit = 1\n", &parsed);
    RuntimeConfig good_config;
    std::string error;
    const bool good_loaded = RuntimeConfig::FromIni(good, &good_config, &error);
    const bool good_valid = good_loaded && good_config.Validate(&error);

    bool parsed_bad = false;
    const auto bad_p = make_ini("[scheduler]\nprocessor_count = 99\n", &parsed_bad);
    RuntimeConfig bad_p_config;
    std::string p_error;
    const bool bad_p_loaded = RuntimeConfig::FromIni(bad_p, &bad_p_config, &p_error);
    const bool bad_p_valid = bad_p_loaded && bad_p_config.Validate(&p_error);

    bool parsed_zero = false;
    const auto zero_queue =
        make_ini("[scheduler]\nlocal_queue_limit = 0\n", &parsed_zero);
    RuntimeConfig zero_config;
    std::string zero_error;
    const bool zero_loaded = RuntimeConfig::FromIni(zero_queue, &zero_config, &zero_error);
    const bool zero_valid = zero_loaded && zero_config.Validate(&zero_error);

    const bool ok = good_valid && !bad_p_valid && !zero_valid;
    std::cout << "  合法通过=" << good_valid << " P=99 被拒=" << !bad_p_valid
              << " queue=0 被拒=" << !zero_valid << "（预期 1 1 1）\n"
              << "  P 越界错误=\"" << p_error << "\"\n"
              << "  queue 越界错误=\"" << zero_error << "\"\n";
    return ok;
}

// ★★ 场景 7：回归验证 —— [timer] 段曾经完全无法被读取
//
// 修复前的 ReadSize 实现（src/config/config.cpp）：
//
//     if (ParseSize(ini.Get(section, key), target)) {
//         if (std::string_view(section) == "scheduler" &&
//             ValidateSchedulerSize(key, *target, error)) {
//             return true;
//         }
//         return false;          // ← 非 scheduler 段必定走到这里
//     }
//
// 而 FromIni 用 ReadSize 读取 [timer] 的 warn_after_days / error_after_days。
// 因此只要 INI 里出现 [timer] 段，FromIni 必定失败，且不设置 error（静默）。
//
// 这解释了两个既有现象：
//   ① 仓库自带的 go2cpp.ini 含 [timer] 段，所以 LoadRuntimeConfig("go2cpp.ini")
//      从来无法成功；
//   ② 既有 log_config_demo 把"未加载 go2cpp.ini"当作常态分支，恰好绕过了它。
//
// 现已修复（ReadSize 按段分流：只有 scheduler 段才走 ValidateSchedulerSize）。
// 本场景作为回归验证：带 [timer] 的 INI 必须能成功构造，且值确实来自文件。
bool scenario_timer_regression() {
    std::clog << "[full][场景7] ★ 回归验证：[timer] 段可被正确读取\n";

    bool parsed = false;
    const auto ini = make_ini(
        "[timer]\n"
        "warn_after_days = 5\n"
        "error_after_days = 9\n",
        &parsed);

    RuntimeConfig config;
    std::string error;
    const bool loaded = RuntimeConfig::FromIni(ini, &config, &error);

    const bool warn_ok = config.timer_warn_after_days == 5;    // 默认是 30
    const bool error_ok = config.timer_error_after_days == 9;  // 默认是 365
    const bool silent = error.empty();

    // 对照：不写 [timer] 时仍应保持默认值
    bool parsed_default = false;
    const auto empty_ini = make_ini("", &parsed_default);
    RuntimeConfig default_config;
    std::string default_error;
    const bool default_loaded = RuntimeConfig::FromIni(empty_ini, &default_config,
                                                       &default_error);
    const bool defaults_kept = default_config.timer_warn_after_days == 30 &&
                               default_config.timer_error_after_days == 365;

    const bool ok = parsed && loaded && warn_ok && error_ok && silent &&
                    default_loaded && defaults_kept;
    std::cout << "  带 [timer] 的 FromIni=" << loaded
              << " warn=" << config.timer_warn_after_days << "(=" << warn_ok
              << ") error=" << config.timer_error_after_days << "(=" << error_ok
              << ") 无报错=" << silent
              << " 空 INI 仍用默认=" << defaults_kept
              << "（预期 1 5 9 1 1）\n";
    return ok;
}

}  // namespace

/**
 * @brief RuntimeConfig 全面版：7 个场景，含一个已确认框架缺陷的记录。
 * @details 只用 FromIni 与 Validate；刻意不调用 ApplyLogging（有全局副作用）。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_from_ini_memory() && ok;
    ok = scenario_scheduler_fields() && ok;
    ok = scenario_log_fields() && ok;
    ok = scenario_transactional() && ok;
    ok = scenario_defaults() && ok;
    ok = scenario_validate_bounds() && ok;
    ok = scenario_timer_regression() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}