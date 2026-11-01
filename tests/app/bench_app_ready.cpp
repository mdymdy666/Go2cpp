/**
 * @file bench_app_ready.cpp
 * @brief 流表 ready 堆与 1e8 帧压测（手动编译，不注册 ctest）。
 *
 * @details 同一份源码可分别链接改动前/后的 libgo2cpp_app.a 做前后对拍：
 *
 *   g++ -O3 -DNDEBUG -std=c++17 -I include tests/app/bench_app_ready.cpp \
 *       /tmp/libgo2cpp_app_new.a -o /tmp/bench_app_ready_new
 *
 *   运行：taskset -c 2 /tmp/bench_app_ready_new [1e8 帧数]
 *
 *          计时用 CLOCK_THREAD_CPUTIME_ID（线程 CPU 时间），排除其他 agent
 *          抢占；每项取多轮最小值。
 *
 * @author go2cpp contributors
 * @date 2026-11-01
 * @copyright MIT License
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <vector>

#include "go2cpp/app/frame.hpp"
#include "go2cpp/app/stream_table.hpp"

namespace app = go2cpp::app;

namespace {

double CpuNowNs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

std::uint64_t RssKb() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return 0;
    }
    unsigned long size = 0;
    unsigned long resident = 0;
    if (std::fscanf(f, "%lu %lu", &size, &resident) != 2) {
        resident = 0;
    }
    std::fclose(f);
    return static_cast<std::uint64_t>(resident) * 4;
}

class InfiniteProducer : public app::FrameProducer {
   public:
    explicit InfiniteProducer(std::uint32_t payload) : payload_(payload) {}
    bool Produce(std::uint32_t max_payload, app::Frame* out) override {
        std::uint32_t take = payload_;
        if (take > max_payload) {
            take = max_payload;
        }
        out->type = app::FrameType::kData;
        out->flags = 0;
        out->payload.resize(take);
        return true;
    }
    bool done() const override { return false; }
    std::uint64_t produced_bytes() const override { return 0; }

   private:
    std::uint32_t payload_ = 0;
};

struct Harness {
    app::StreamTable table;
    std::vector<app::Stream*> streams;
    std::uint32_t payload = 512;

    explicit Harness(std::uint32_t payload_bytes, std::uint32_t stream_count,
                     bool dependency_tree = false) {
        payload = payload_bytes;
        app::StreamTableConfig config;
        config.initial_stream_window = 0x7ffffff;
        config.connection_window = 0x7ffffff;
        config.max_frame_payload = 256 * 1024;
        config.max_concurrent_streams = 4096;
        table = app::StreamTable(config);
        std::vector<app::Stream*> parents;
        if (dependency_tree) {
            for (int i = 0; i < 8; ++i) {
                parents.push_back(table.CreateLocal(app::StreamKind::kControl, nullptr, 16, 0));
            }
        }
        for (std::uint32_t i = 0; i < stream_count; ++i) {
            auto producer = std::make_unique<InfiniteProducer>(payload);
            const std::uint32_t parent =
                dependency_tree ? parents[i % parents.size()]->id : 0u;
            streams.push_back(table.CreateLocal(app::StreamKind::kResource,
                                                std::move(producer), 16, parent));
        }
    }

    void TopUp(std::uint64_t frames) {
        const std::uint64_t want = frames * payload;
        const std::uint32_t inc =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(want, 0x40000000ull));
        table.OnWindowUpdate(0, inc);
        for (app::Stream* s : streams) {
            table.OnWindowUpdate(s->id, inc);
        }
    }

    std::uint64_t FlushFrames(std::uint64_t frames) {
        std::uint64_t done = 0;
        std::uint64_t since_topup = 0;
        while (done < frames) {
            app::Stream* s = table.PickNext(256 * 1024);
            if (s == nullptr) {
                break;
            }
            const std::size_t bytes = s->pending.Size();
            if (s->pending.type == app::FrameType::kData && bytes > 0) {
                table.ConsumeSend(*s, bytes);
            }
            table.ConsumePending(*s);
            table.SendData(*s, bytes, false);
            table.ChargeFinish(*s, bytes);
            ++done;
            if (++since_topup >= 100000) {
                since_topup = 0;
                TopUp(100000);
            }
        }
        return done;
    }
};

template <typename Fn>
double BestOf3(Fn fn) {
    double best = 1e18;
    for (int i = 0; i < 3; ++i) {
        best = std::min(best, fn());
    }
    return best;
}

void BenchFlush() {
    for (std::uint32_t streams : {1u, 8u, 256u}) {
        Harness h(512, streams);
        h.FlushFrames(200000);
        const double ns = BestOf3([&] {
            const double t0 = CpuNowNs();
            const std::uint64_t done = h.FlushFrames(3000000);
            return (CpuNowNs() - t0) / static_cast<double>(done);
        });
        std::printf("flush    streams=%4u        %8.2f ns/frame\n", streams, ns);
    }
    Harness dep(512, 256, true);
    dep.FlushFrames(100000);
    const double ns = BestOf3([&] {
        const double t0 = CpuNowNs();
        const std::uint64_t done = dep.FlushFrames(1000000);
        return (CpuNowNs() - t0) / static_cast<double>(done);
    });
    std::printf("flush    streams= 256 dep    %8.2f ns/frame\n", ns);
}

void BenchPickNext() {
    for (std::uint32_t streams : {1u, 256u}) {
        Harness h(512, streams);
        h.FlushFrames(1000);
        constexpr std::uint64_t kOps = 3000000;
        const double ns = BestOf3([&] {
            const double t0 = CpuNowNs();
            for (std::uint64_t i = 0; i < kOps; ++i) {
                (void)h.table.PickNext(256 * 1024);
            }
            return (CpuNowNs() - t0) / static_cast<double>(kOps);
        });
        std::printf("picknext streams=%4u        %8.2f ns/op\n", streams, ns);
    }
}

void BenchCharge() {
    Harness h(512, 256);
    h.FlushFrames(1000);
    constexpr std::uint64_t kOps = 2000000;
    const double ns = BestOf3([&] {
        const double t0 = CpuNowNs();
        for (std::uint64_t i = 0; i < kOps; ++i) {
            h.table.ChargeFinish(*h.streams[i % h.streams.size()], 512);
        }
        return (CpuNowNs() - t0) / static_cast<double>(kOps);
    });
    std::printf("charge   in-heap 256         %8.2f ns/op\n", ns);
}

void Bench1e8(std::uint64_t frames) {
    Harness h(512, 256);
    h.FlushFrames(1000000);
    const std::uint64_t rss_before = RssKb();
    const double t0 = CpuNowNs();
    const std::uint64_t done = h.FlushFrames(frames);
    const double ns = (CpuNowNs() - t0) / static_cast<double>(done == 0 ? 1 : done);
    const std::uint64_t rss_after = RssKb();
    std::printf("1e8      frames=%llu      %8.2f ns/frame  %.2f Mfps  rss=%llu..%llu KiB\n",
                static_cast<unsigned long long>(done), ns, 1e3 / ns,
                static_cast<unsigned long long>(rss_before),
                static_cast<unsigned long long>(rss_after));
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t frames =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000000ULL;
    BenchFlush();
    BenchPickNext();
    BenchCharge();
    Bench1e8(frames);
    return 0;
}
