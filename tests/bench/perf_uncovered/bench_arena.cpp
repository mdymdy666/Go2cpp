// memory/arena 模块微基准：Arena::Allocate 分配 ns/op。
//
// 口径：Arena(64 KiB) 上连续 Allocate(64)，一次分配算 1 op。块用尽时走
// AllocateNewBlock（64 KiB/块），所以 1e6 次约 1e6*64B = 64 MiB 线性内存，
// 单轮在 200 MB 预算内。每次测量新建 Arena，退出即释放。

#include <cstddef>

#include "bench_common.hpp"
#include "src/storage/arena.hpp"

int main() {
    {
        go2cpp::storage::Arena probe;
        char* p = probe.Allocate(64);
        if (p == nullptr) {
            std::printf("arena_probe_failed=1\n");
            return 1;
        }
    }

    bench::RunAdaptive("arena_allocate64", 20000, 150.0, [&](std::size_t n) {
        go2cpp::storage::Arena arena;
        std::size_t acc = 0;
        for (std::size_t i = 0; i < n; ++i) {
            char* p = arena.Allocate(64);
            p[0] = static_cast<char>(i);
            acc += static_cast<std::size_t>(p[0]);
        }
        bench::g_sink = acc;
    });
    return 0;
}
