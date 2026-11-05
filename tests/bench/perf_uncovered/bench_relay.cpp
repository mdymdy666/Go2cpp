// relay 模块微基准：Hub 注册/查找/注销吞吐 ns/op。
//
// 口径（与 tests/relay/bench_relay.cpp 的单线程 Hub 段同口径，但收敛到
// 公共脚手架）：一次 op = Register(key, conn) + Lookup(key) + Unregister。
// key 在 1024 个房间间轮转，conn 为 shared_ptr<NullStream>（仅承载引用计数）。

#include <cstddef>
#include <memory>
#include <string>

#include "bench_common.hpp"
#include "go2cpp/relay/hub.hpp"

namespace {

class NullStream final : public go2cpp::relay::Hub::Stream {
public:
    ::go2cpp::ErrorPtr Close() override { return {}; }
};

}  // namespace

int main() {
    go2cpp::relay::Hub hub;

    bench::RunAdaptive("relay_hub_rw", 2000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string key = "room:" + std::to_string(i % 1024);
            auto conn = std::make_shared<NullStream>();
            acc += hub.Register(key, conn) ? 1u : 0u;
            const auto snapshot = hub.Lookup(key);
            acc += snapshot.size();
            hub.Unregister(key, conn);
        }
        bench::g_sink = acc;
    });
    return 0;
}
