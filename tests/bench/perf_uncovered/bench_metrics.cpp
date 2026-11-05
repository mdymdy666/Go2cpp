// metrics 模块微基准：counter / gauge 更新 ns/op。
//
// 口径：
//   - metrics_counter_inc：Counter::Inc() 一次（fetch_add(relaxed)）；
//   - metrics_gauge_add：Gauge::Add(1.0) 一次（CAS 循环，IEEE-754 位模式）；
//   - metrics_gauge_set：Gauge::Set(1.0) 一次（atomic store）。
// 均为无锁原子路径，不含 Registry 导出。

#include <cstddef>

#include "bench_common.hpp"
#include "go2cpp/metrics/metrics.hpp"

int main() {
    using go2cpp::metrics::Counter;
    using go2cpp::metrics::Gauge;

    Counter counter;
    Gauge gauge;

    counter.Inc();
    gauge.Set(1.0);
    if (counter.Value() != 1) {
        std::printf("metrics_probe_failed=1\n");
        return 1;
    }

    bench::RunAdaptive("metrics_counter_inc", 20000, 150.0, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            counter.Inc();
        }
        bench::g_sink = static_cast<std::size_t>(counter.Value());
    });

    bench::RunAdaptive("metrics_gauge_add", 20000, 150.0, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            gauge.Add(1.0);
        }
        bench::g_sink = static_cast<std::size_t>(gauge.Value());
    });

    bench::RunAdaptive("metrics_gauge_set", 20000, 150.0, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            gauge.Set(1.0);
        }
        bench::g_sink = static_cast<std::size_t>(gauge.Value());
    });
    return 0;
}
