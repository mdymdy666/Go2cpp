// trace 模块微基准：span 创建 / 派生 / 传播编码 ns/op。
//
// 口径：
//   - trace_root：TraceContext::CreateRoot()（128 位 trace + 64 位 span 生成）；
//   - trace_child：有效上下文上 ChildSpan()（派生下游 span，热路径主操作）；
//   - trace_header：ToHeader() 定长 72 字节文本编码（跨节点传播）。

#include <cstddef>
#include <string>

#include "bench_common.hpp"
#include "go2cpp/distributed/trace.hpp"

int main() {
    using go2cpp::distributed::TraceContext;

    const TraceContext root = TraceContext::CreateRoot();
    if (!root.IsValid()) {
        std::printf("trace_probe_failed=1\n");
        return 1;
    }

    bench::RunAdaptive("trace_root", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += TraceContext::CreateRoot().IsValid() ? 1u : 0u;
        }
        bench::g_sink = acc;
    });

    bench::RunAdaptive("trace_child", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += root.ChildSpan().IsValid() ? 1u : 0u;
        }
        bench::g_sink = acc;
    });

    bench::RunAdaptive("trace_header", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += root.ToHeader().size();
        }
        bench::g_sink = acc;
    });
    return 0;
}
