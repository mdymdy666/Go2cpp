// error 模块微基准：构造 + 格式化 ops/s。
//
// 口径：NewError() 构造一个叶子错误（shared_ptr<const Error>），随后调用
// Message() 拿格式化文本（返回 std::string 拷贝），两步合计算 1 次 op。
// 另测 Wrap() 单层包装的构造开销（错误链是热路径上的常见形态）。

#include <cstddef>
#include <string>

#include "bench_common.hpp"
#include "go2cpp/error.hpp"

int main() {
    {
        const auto probe = go2cpp::NewError("bench: probe");
        if (probe->Message().empty()) {
            std::printf("error_probe_failed=1\n");
            return 1;
        }
    }

    bench::RunAdaptive("error_construct_format", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto err = go2cpp::NewError("bench: request failed, code=42");
            acc += err->Message().size();
        }
        bench::g_sink = acc;
    });

    bench::RunAdaptive("error_wrap", 20000, 150.0, [&](std::size_t n) {
        const auto cause = go2cpp::NewError("bench: cause");
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto wrapped = go2cpp::Wrap(cause, "bench: wrapper");
            acc += wrapped->Message().size();
        }
        bench::g_sink = acc;
    });
    return 0;
}
