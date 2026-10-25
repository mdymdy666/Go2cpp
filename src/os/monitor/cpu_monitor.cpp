#include "go2cpp/os/monitor/cpu_monitor.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace go2cpp::os::monitor {
namespace {

/// 读取 /proc/loadavg 的三个负载值、可运行任务数与线程总数。
void ReadLoadAvg(const std::string& path, CpuLoad& load) {
    std::ifstream in(path);
    if (!in) {
        return;
    }
    std::string line;
    if (!std::getline(in, line)) {
        return;
    }
    // 格式：0.52 0.58 0.59 1/1234 5678
    std::istringstream iss(line);
    double l1 = 0.0;
    double l5 = 0.0;
    double l15 = 0.0;
    std::string runnable;
    std::uint64_t last_pid = 0;
    if (iss >> l1 >> l5 >> l15 >> runnable >> last_pid) {
        load.load1 = l1;
        load.load5 = l5;
        load.load15 = l15;
        const auto slash = runnable.find('/');
        if (slash != std::string::npos) {
            load.runnable = static_cast<std::uint64_t>(
                std::strtoull(runnable.substr(0, slash).c_str(), nullptr, 10));
            load.total_threads = static_cast<std::uint64_t>(
                std::strtoull(runnable.substr(slash + 1).c_str(), nullptr, 10));
        }
    }
}

}  // namespace

CpuMonitor::CpuMonitor(std::string stat_path, std::string loadavg_path)
    : stat_path_(std::move(stat_path)),
      loadavg_path_(std::move(loadavg_path)) {}

Status CpuMonitor::UpdateOnce(Snapshot& snapshot) {
    std::ifstream in(stat_path_);
    if (!in) {
        return Status::kError;
    }

    std::vector<CpuCore> cores;
    std::map<std::string, RawTimes> current;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu", 0) != 0) {
            continue;
        }
        std::istringstream iss(line);
        std::string name;
        RawTimes raw;
        if (!(iss >> name >> raw.user >> raw.nice >> raw.system >> raw.idle)) {
            continue;
        }
        // 内核早期版本可能缺 tail 字段，按 0 处理。
        (void)(iss >> raw.iowait >> raw.irq >> raw.softirq >> raw.steal >>
               raw.guest >> raw.guest_nice);

        CpuCore core;
        core.name = name;
        core.user = raw.user;
        core.nice = raw.nice;
        core.system = raw.system;
        core.idle = raw.idle;
        core.iowait = raw.iowait;
        core.irq = raw.irq;
        core.softirq = raw.softirq;
        core.steal = raw.steal;
        core.guest = raw.guest;
        core.guest_nice = raw.guest_nice;

        const auto prev = last_.find(name);
        // 首帧（含 Stop 后重新开始、以及热插拔后新出现的 cpuN）没有基线，
        // 各占比保持结构体默认的 0；累计值仍照常输出。
        if (prev != last_.end()) {
            const RawTimes& old = prev->second;
            // 计数器只增不减；先做饱和差再求和，避免 u64 回绕（例如容器
            // 迁移/热插拔后读到一个更小的值）时下溢成巨大的值。
            const auto diff = [](std::uint64_t now_value,
                                 std::uint64_t before) -> double {
                return now_value >= before
                           ? static_cast<double>(now_value - before)
                           : 0.0;
            };
            const double user = diff(raw.user, old.user);
            const double nice = diff(raw.nice, old.nice);
            const double system = diff(raw.system, old.system);
            const double idle = diff(raw.idle, old.idle);
            const double iowait = diff(raw.iowait, old.iowait);
            const double irq = diff(raw.irq, old.irq);
            const double softirq = diff(raw.softirq, old.softirq);
            const double steal = diff(raw.steal, old.steal);
            // 与参考实现一致的公式：guest 已包含在 user 内，不重复累加。
            const double total_delta = user + nice + system + idle + iowait +
                                       irq + softirq + steal;
            const double busy_delta =
                user + nice + system + irq + softirq + steal;
            if (total_delta > 0.0) {
                const auto pct = [total_delta](double delta) {
                    const double value = delta / total_delta * 100.0;
                    // 分项累计值在两次采样之间理论上不会超过总增量；这里
                    // 仍然夹紧，防止异常值把占比放大到 >100。
                    return value > 100.0 ? 100.0 : value;
                };
                core.usage_percent = pct(busy_delta);
                core.user_percent = pct(user);
                core.system_percent = pct(system);
                core.nice_percent = pct(nice);
                core.idle_percent = pct(idle);
                core.iowait_percent = pct(iowait);
                core.irq_percent = pct(irq);
                core.softirq_percent = pct(softirq);
            }
        }
        cores.push_back(std::move(core));
        current[name] = raw;
    }

    if (cores.empty()) {
        return Status::kError;
    }

    snapshot.cpus = std::move(cores);
    ReadLoadAvg(loadavg_path_, snapshot.load);
    last_ = std::move(current);
    ++sample_count_;
    return Status::kOk;
}

void CpuMonitor::Stop() {
    last_.clear();
}

}  // namespace go2cpp::os::monitor
