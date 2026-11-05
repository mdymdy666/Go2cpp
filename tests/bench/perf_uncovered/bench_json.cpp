// json 模块微基准：小对象序列化 / 反序列化 ops/s。
//
// 口径：一个 5 字段小对象（id/name/ok/score/tags），dump() 一次算一次序列化，
// ParseJson() 一次算一次反序列化（含深度栅栏迭代扫描）。

#include <cstddef>
#include <string>

#include "bench_common.hpp"
#include "go2cpp/json.hpp"

int main() {
    using go2cpp::Json;
    using go2cpp::ParseJson;

    const Json small = Json::object({{"id", 12345},
                                     {"name", "go2cpp-bench"},
                                     {"ok", true},
                                     {"score", 3.5},
                                     {"tags", Json::array({"a", "b", "c"})}});
    const std::string text = small.dump();

    {
        const auto parsed = ParseJson(text);
        if (!parsed.ok()) {
            std::printf("json_parse_error=1\n");
            return 1;
        }
    }

    bench::RunAdaptive("json_deserialize", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const auto parsed = ParseJson(text);
            acc += parsed.value.size();
        }
        bench::g_sink = acc;
    });

    bench::RunAdaptive("json_serialize", 20000, 150.0, [&](std::size_t n) {
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string out = small.dump();
            acc += out.size();
        }
        bench::g_sink = acc;
    });
    return 0;
}
