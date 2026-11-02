#include "go2cpp/config.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// config_center 全面版：Config 中心（Lookup + 热重载）的 7 个使用场景。
//   1  Lookup 注册默认值      程序内置默认值，先注册后覆盖
//   2  ★ Lookup 幂等           同名再次调用返回同一实例，不会重置已有值
//   3  查询与枚举              LookupBase 按名查询、List 列出全部
//   4  ★ INI 覆盖默认值        LoadFromIni 就是热重载内部走的同一条路径
//   5  ★ 提交监听              AddCommitListener 收到本次变更的变量列表
//   6  ★ 事务校验              AddTransactionValidator 可以在提交前拒绝整批
//   7  ★ 文件热重载            StartWatcher / Watching / StopWatcher
//
// 说明：Config 是进程级单例，本示例注册的键带 "demo." 前缀以避免与其它示例冲突。
//       场景 4 与场景 7 都体现同一条主线：**先有程序内置默认值，配置文件存在则覆盖它**。

namespace {

using namespace go2cpp::config;
using Ms = std::chrono::milliseconds;

bool scenario_register_defaults() {
    std::clog << "[full][场景1] Lookup 注册内置默认值\n";
    auto& config = Config::Instance();
    auto threads = config.Lookup<int>("demo.threads", 4, "工作线程数");
    auto name = config.Lookup<std::string>("demo.name", "default-service", "服务名");
    auto enabled = config.Lookup<bool>("demo.enabled", false, "是否启用");

    const bool ok = threads->GetValue() == 4 && name->GetValue() == "default-service" &&
                    !enabled->GetValue();
    std::cout << "  threads=" << threads->GetValue() << " name=" << name->GetValue()
              << " enabled=" << enabled->GetValue() << "（预期 4 default-service 0）\n";
    return ok;
}

bool scenario_lookup_idempotent() {
    std::clog << "[full][场景2] Lookup 幂等：同名返回同一实例\n";
    auto& config = Config::Instance();
    const auto first = config.Lookup<int>("demo.threads", 4, "工作线程数");
    (void)first->SetValue(16);

    // 第二次用不同的默认值调用：既不该新建实例，也不该把值改回默认
    const auto second = config.Lookup<int>("demo.threads", 999, "不同的描述");

    const bool same = (first == second);
    const bool kept = second->GetValue() == 16;
    // 名字与描述来自首次注册
    const bool meta_kept = second->description() == "工作线程数";
    const bool ok = same && kept && meta_kept;
    std::cout << "  同一实例=" << same << " 值保持 16=" << kept
              << " 描述沿用首次注册=" << meta_kept << "（预期 1 1 1）\n";
    return ok;
}

bool scenario_lookup_base_and_list() {
    std::clog << "[full][场景3] LookupBase 查询与 List 枚举\n";
    auto& config = Config::Instance();
    const auto found = config.LookupBase("demo.name");
    const auto missing = config.LookupBase("demo.absent");

    const auto all = config.List();
    bool listed_name = false;
    bool listed_threads = false;
    for (const auto& entry : all) {
        if (entry && entry->name() == "demo.name") {
            listed_name = true;
        }
        if (entry && entry->name() == "demo.threads") {
            listed_threads = true;
        }
    }
    const bool ok = found != nullptr && missing == nullptr && listed_name &&
                    listed_threads && all.size() >= 3;
    std::cout << "  查到 demo.name=" << (found != nullptr)
              << " 查不到 demo.absent=" << (missing == nullptr)
              << " List 含 demo.name=" << listed_name
              << " 含 demo.threads=" << listed_threads
              << " 总数=" << all.size() << "（预期 1 1 1 1 >=3）\n";
    return ok;
}

bool scenario_ini_overrides_defaults() {
    std::clog << "[full][场景4] INI 覆盖内置默认值（热重载的核心语义）\n";
    auto& config = Config::Instance();
    auto threads = config.Lookup<int>("demo.threads", 4, "工作线程数");
    auto name = config.Lookup<std::string>("demo.name", "default-service", "服务名");

    const int before_threads = threads->GetValue();
    const std::string before_name = name->GetValue();

    IniFile ini;
    const bool parsed = ini.Parse(
        "[demo]\n"
        "threads = 32\n"
        "name = from-ini\n");
    std::string error;
    const bool loaded = config.LoadFromIni(ini, &error);

    const bool ok = parsed && loaded && threads->GetValue() == 32 &&
                    name->GetValue() == "from-ini";
    std::cout << "  加载前=" << before_threads << "/" << before_name
              << " 加载后=" << threads->GetValue() << "/" << name->GetValue()
              << " error=\"" << error << "\"（预期 加载后 32/from-ini）\n";
    return ok;
}

bool scenario_commit_listener() {
    std::clog << "[full][场景5] 提交监听：收到本次变更的变量列表\n";
    auto& config = Config::Instance();
    auto threads = config.Lookup<int>("demo.threads", 4, "工作线程数");
    auto name = config.Lookup<std::string>("demo.name", "default-service", "服务名");

    std::vector<std::string> changed_names;
    const auto id = config.AddCommitListener(
        [&changed_names](const std::vector<ConfigVarBase::ptr>& changed) {
            changed_names.clear();
            for (const auto& item : changed) {
                if (item) {
                    changed_names.push_back(item->name());
                }
            }
        });

    IniFile ini;
    (void)ini.Parse("[demo]\nthreads = 7\nname = listener-test\n");
    std::string error;
    const bool loaded = config.LoadFromIni(ini, &error);
    config.DelCommitListener(id);

    // 监听器只应看到真正发生变化的项
    bool saw_threads = false;
    bool saw_name = false;
    for (const auto& entry : changed_names) {
        if (entry == "demo.threads") {
            saw_threads = true;
        }
        if (entry == "demo.name") {
            saw_name = true;
        }
    }
    // 注销之后不应再收到通知
    const std::size_t after_count = changed_names.size();
    IniFile again;
    (void)again.Parse("[demo]\nthreads = 8\n");
    (void)config.LoadFromIni(again, &error);
    const bool detached = changed_names.size() == after_count;

    const bool ok = loaded && saw_threads && saw_name && detached;
    std::cout << "  收到变更数=" << after_count << " 含 demo.threads=" << saw_threads
              << " 含 demo.name=" << saw_name << " 注销后不再通知=" << detached
              << "（预期 含两项且注销后不通知）\n";
    return ok;
}

bool scenario_transaction_validator() {
    std::clog << "[full][场景6] 事务校验：提交前可拒绝整批\n";
    auto& config = Config::Instance();
    auto threads = config.Lookup<int>("demo.threads", 4, "工作线程数");
    (void)threads->SetValue(10);

    // 拒绝所有 threads > 16 的候选批次
    const auto reject_id = config.AddTransactionValidator(
        [](const std::map<std::string, std::string>& candidates, std::string* error) {
            const auto found = candidates.find("demo.threads");
            if (found != candidates.end()) {
                try {
                    if (std::stoi(found->second) > 16) {
                        if (error) {
                            *error = "demo.threads 不能超过 16";
                        }
                        return false;
                    }
                } catch (...) {
                    if (error) {
                        *error = "demo.threads 不是整数";
                    }
                    return false;
                }
            }
            return true;
        });

    IniFile too_many;
    (void)too_many.Parse("[demo]\nthreads = 99\n");
    std::string error;
    const bool rejected = !config.LoadFromIni(too_many, &error);
    const bool value_kept = threads->GetValue() == 10;   // 拒绝后旧值不变
    const std::string reject_error = error;

    // 合法批次应当通过
    IniFile ok_ini;
    (void)ok_ini.Parse("[demo]\nthreads = 12\n");
    error.clear();
    const bool accepted = config.LoadFromIni(ok_ini, &error);
    const bool applied = threads->GetValue() == 12;

    config.DelTransactionValidator(reject_id);

    const bool ok = rejected && value_kept && accepted && applied;
    std::cout << "  越界批次被拒=" << rejected << " 旧值保持 10=" << value_kept
              << " 错误=\"" << reject_error << "\"\n"
              << "  合法批次通过=" << accepted << " 已应用 12=" << applied
              << "（预期 1 1 1 1）\n";
    return ok;
}

bool scenario_file_watcher() {
    std::clog << "[full][场景7] 文件热重载：StartWatcher / Watching / StopWatcher\n";
    auto& config = Config::Instance();
    auto threads = config.Lookup<int>("demo.threads", 4, "工作线程数");

    const std::string path = "/tmp/go2cpp-config-center-demo.ini";

    // 写一个初始配置文件
    {
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            std::cout << "  无法写入临时配置（跳过）\n";
            return false;
        }
        out << "[demo]\nthreads = 64\n";
    }

    const bool initially_watching = config.Watching();
    std::string error;
    const bool started = config.StartWatcher(path, Ms(50), &error);
    const bool watching = config.Watching();

    // 等热重载把文件里的值应用进来（轮询，带超时）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool applied = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (threads->GetValue() == 64) {
            applied = true;
            break;
        }
        std::this_thread::sleep_for(Ms(20));
    }

    // 改文件，验证会再次热重载
    bool reloaded = false;
    if (applied) {
        {
            std::ofstream out(path, std::ios::trunc);
            out << "[demo]\nthreads = 128\n";
        }
        while (std::chrono::steady_clock::now() < deadline) {
            if (threads->GetValue() == 128) {
                reloaded = true;
                break;
            }
            std::this_thread::sleep_for(Ms(20));
        }
    }

    config.StopWatcher();
    const bool stopped = !config.Watching();
    std::remove(path.c_str());

    const bool ok = !initially_watching && started && watching && applied && reloaded &&
                    stopped;
    std::cout << "  启动前 Watching=" << initially_watching << " 启动成功=" << started
              << " 启动后 Watching=" << watching << " 首次加载 64=" << applied
              << " 改动后重载 128=" << reloaded << " 停止后 Watching=" << stopped
              << " error=\"" << error << "\"（预期 0 1 1 1 1 1）\n";
    return ok;
}

}  // namespace

/**
 * @brief config_center 全面版：Config 中心的 7 个场景（含真实文件热重载）。
 * @details 主线是「程序内置默认值 → 配置文件存在则覆盖」。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    bool ok = true;
    ok = scenario_register_defaults() && ok;
    ok = scenario_lookup_idempotent() && ok;
    ok = scenario_lookup_base_and_list() && ok;
    ok = scenario_ini_overrides_defaults() && ok;
    ok = scenario_commit_listener() && ok;
    ok = scenario_transaction_validator() && ok;
    ok = scenario_file_watcher() && ok;

    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
