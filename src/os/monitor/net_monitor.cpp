#include "go2cpp/os/monitor/net_monitor.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace go2cpp::os::monitor {
namespace {

/// 差分用的时间戳：必须是单调时钟（原因见 disk_monitor.cpp）。
std::int64_t NowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
        .count();
}

/// 去掉 "eth0:" 末尾的冒号。
std::string NormalizeName(std::string name) {
    if (!name.empty() && name.back() == ':') {
        name.pop_back();
    }
    return name;
}

}  // namespace

NetMonitor::NetMonitor(std::string dev_path)
    : dev_path_(std::move(dev_path)) {}

Status NetMonitor::UpdateOnce(Snapshot& snapshot) {
    std::ifstream in(dev_path_);
    if (!in) {
        return Status::kError;
    }

    const std::int64_t now_ns = NowNs();
    const double dt = has_prev_
                          ? static_cast<double>(now_ns - last_sample_ns_) / 1e9
                          : 0.0;

    std::vector<NetDevStat> nets;
    std::map<std::string, RawCounters> current;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        // 前两行是标题。
        if (++line_no <= 2) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string name = NormalizeName(line.substr(0, colon));
        // 去掉前导空格。
        const auto first = name.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        name = name.substr(first);

        std::istringstream iss(line.substr(colon + 1));
        RawCounters raw;
        if (!(iss >> raw.rcv_bytes >> raw.rcv_packets >> raw.rcv_errs >>
              raw.rcv_drops)) {
            continue;
        }
        // 接收方向的 fifo / frame / compressed / multicast 暂不使用，但必须
        // 消费掉才能读到发送方向。用四个独立变量而不是重复读取同一个，
        // 避免「同一对象在一个表达式里被多次修改」的求值顺序争议。
        std::uint64_t fifo = 0;
        std::uint64_t frame = 0;
        std::uint64_t compressed = 0;
        std::uint64_t multicast = 0;
        if (!(iss >> fifo >> frame >> compressed >> multicast)) {
            continue;  // /proc/net/dev 固定 16 字段，短行说明格式异常
        }
        (void)fifo;
        (void)frame;
        (void)compressed;
        (void)multicast;
        if (!(iss >> raw.snd_bytes >> raw.snd_packets >> raw.snd_errs >>
              raw.snd_drops)) {
            continue;
        }

        NetDevStat stat;
        stat.name = name;
        stat.rcv_bytes = raw.rcv_bytes;
        stat.rcv_packets = raw.rcv_packets;
        stat.rcv_errs = raw.rcv_errs;
        stat.rcv_drops = raw.rcv_drops;
        stat.snd_bytes = raw.snd_bytes;
        stat.snd_packets = raw.snd_packets;
        stat.snd_errs = raw.snd_errs;
        stat.snd_drops = raw.snd_drops;

        const auto prev = last_.find(name);
        // 首帧、接口新出现（docker0/veth 热插拔）时没有基线，速率保持 0；
        // 接口消失后再出现同样按首帧处理。
        if (prev != last_.end() && dt > 0.0) {
            const RawCounters& old = prev->second;
            const auto delta = [](std::uint64_t now, std::uint64_t before) {
                return now >= before ? now - before : now;
            };
            stat.rcv_rate =
                static_cast<double>(delta(raw.rcv_bytes, old.rcv_bytes)) / dt;
            stat.snd_rate =
                static_cast<double>(delta(raw.snd_bytes, old.snd_bytes)) / dt;
            stat.rcv_packets_rate =
                static_cast<double>(delta(raw.rcv_packets, old.rcv_packets)) / dt;
            stat.snd_packets_rate =
                static_cast<double>(delta(raw.snd_packets, old.snd_packets)) / dt;
        }
        nets.push_back(std::move(stat));
        current[name] = raw;
    }

    if (nets.empty()) {
        return Status::kError;
    }

    snapshot.nets = std::move(nets);
    last_ = std::move(current);
    last_sample_ns_ = now_ns;
    has_prev_ = true;
    ++sample_count_;
    return Status::kOk;
}

void NetMonitor::Stop() {
    last_.clear();
    last_sample_ns_ = 0;
    has_prev_ = false;
}

}  // namespace go2cpp::os::monitor
