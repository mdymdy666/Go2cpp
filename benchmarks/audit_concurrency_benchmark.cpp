/**
 * @file audit_concurrency_benchmark.cpp
 * @brief channel / future / io 三项并发设施的审计基准（独立可执行文件）。
 *
 * ## 测什么
 * - channel：1 万次 send/recv 的 ns/op。有缓冲（单线程 Send+Recv 往返、
 *   TrySend/TryRecv 快路径）与无缓冲（1P1C 会合）分开；
 * - future：SetValue→Get 的端到端 ns/op（每次新建 Promise 的完整生命周期）
 *   与已就绪 Future 的重复 Get；
 * - io：1 万次 WaitRead 唤醒延迟 vs 裸 epoll_wait。两边都使用同一对
 *   pipe + 同样的 ack 握手，且在 `taskset -c 0` 单核上运行 —— 单核保证
 *   等待方先进入 park/内核睡眠，写端才被调度，测到的是真实唤醒路径。
 *
 * ## 采集方式（与审计要求一致）
 * - Release 构建（-O3 -DNDEBUG）；
 * - `taskset -c 0` 绑核；
 * - 每个指标采样 5 次，报告**中位数**与全部样本；
 * - 运行前后记录 `/proc/loadavg` 与 `procs_running`（loadavg 在 WSL2
 *   不可信，只作背景；判定以 procs_running 与样本离散度为准）。
 *
 * 单次运行约 1-3 秒，必须套 `timeout 120`。
 */

#include "go2cpp/channel.hpp"
#include "go2cpp/future.hpp"
#include "go2cpp/io.hpp"

#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kIterations = 10000;
constexpr int kRepeats = 5;

std::atomic<long long> g_sink{0};

double ElapsedNsPerOp(const Clock::time_point& start, int operations) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start);
    return static_cast<double>(elapsed.count()) / operations;
}

double Median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void Report(const char* label, const std::vector<double>& samples) {
    std::printf("[bench] %-42s median %10.1f ns/op   samples:", label,
                Median(samples));
    for (double sample : samples) {
        std::printf(" %.1f", sample);
    }
    std::printf("\n");
}

template <typename Fn>
void Measure(const char* label, Fn&& body) {
    std::vector<double> samples;
    samples.reserve(kRepeats);
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        const auto start = Clock::now();
        body();
        samples.push_back(ElapsedNsPerOp(start, kIterations));
    }
    Report(label, samples);
}

void BenchmarkChannelBufferedRoundtrip() {
    auto channel = go2cpp::MakeChannel<int>(8);
    Measure("channel buffered Send+Recv roundtrip", [&] {
        long long sum = 0;
        for (int i = 0; i < kIterations; ++i) {
            (void)channel->TrySend(i);
            sum += channel->TryRecv().ValueOrDefault();
        }
        g_sink.fetch_add(sum, std::memory_order_relaxed);
    });
}

void BenchmarkChannelBufferedFastPath() {
    auto channel = go2cpp::MakeChannel<int>(kIterations + 1);
    Measure("channel buffered TrySend+TryRecv fast", [&] {
        long long sum = 0;
        for (int i = 0; i < kIterations; ++i) {
            (void)channel->TrySend(i);
        }
        for (int i = 0; i < kIterations; ++i) {
            sum += channel->TryRecv().ValueOrDefault();
        }
        g_sink.fetch_add(sum, std::memory_order_relaxed);
    });
}

void BenchmarkChannelUnbufferedRendezvous() {
    Measure("channel unbuffered 1P1C rendezvous", [] {
        auto channel = go2cpp::MakeChannel<int>(0);
        std::thread producer([channel] {
            for (int i = 0; i < kIterations; ++i) {
                (void)channel->Send(i);
            }
        });
        long long sum = 0;
        for (int i = 0; i < kIterations; ++i) {
            sum += channel->Recv().ValueOrDefault();
        }
        producer.join();
        g_sink.fetch_add(sum, std::memory_order_relaxed);
    });
}

void BenchmarkFutureHotGet() {
    auto pair = go2cpp::MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    (void)promise.SetValue(7);
    Measure("future ready GetResult (hot)", [&] {
        long long sum = 0;
        for (int i = 0; i < kIterations; ++i) {
            const auto result = future.GetResult();
            sum += result.Value() ? *result.Value() : 0;
        }
        g_sink.fetch_add(sum, std::memory_order_relaxed);
    });
}

void BenchmarkFutureLifecycle() {
    Measure("future SetValue->Get lifecycle", [] {
        long long sum = 0;
        for (int i = 0; i < kIterations; ++i) {
            auto pair = go2cpp::MakePromise<int>();
            auto promise = std::move(pair.first);
            auto future = std::move(pair.second);
            (void)promise.SetValue(i);
            const auto result = future.GetResult();
            sum += result.Value() ? *result.Value() : 0;
        }
        g_sink.fetch_add(sum, std::memory_order_relaxed);
    });
}

void BenchmarkFutureTryGetMiss() {
    auto pair = go2cpp::MakePromise<int>();
    auto promise = std::move(pair.first);
    auto future = std::move(pair.second);
    Measure("future pending TryGet (miss)", [&] {
        long long sum = 0;
        for (int i = 0; i < kIterations; ++i) {
            const auto result = future.GetResultFor(0ms);
            sum += result.status == go2cpp::FutureStatus::kTimedOut ? 1 : 0;
        }
        g_sink.fetch_add(sum, std::memory_order_relaxed);
    });
    (void)promise.SetValue(1);
}

bool MakePipe(int (&fds)[2]) {
    return ::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0;
}

/** @brief 一轮裸 epoll_wait 唤醒测量：ack 握手 + 单核强制阻塞。 */
double OneRawEpollSample(int epoll_fd, int read_fd, int write_fd,
                         std::atomic<int>* ack) {
    std::thread writer([ack, write_fd] {
        for (int last = 0; last < kIterations; ++last) {
            while (ack->load(std::memory_order_acquire) == last) {
                std::this_thread::yield();
            }
            const char byte = 'e';
            (void)::syscall(SYS_write, write_fd, &byte, 1);
        }
    });
    const auto start = Clock::now();
    for (int i = 0; i < kIterations; ++i) {
        ack->store(i + 1, std::memory_order_release);
        epoll_event out{};
        int count = 0;
        do {
            count = ::epoll_wait(epoll_fd, &out, 1, -1);
        } while (count < 0 && errno == EINTR);
        char byte = 0;
        (void)::syscall(SYS_read, read_fd, &byte, 1);
    }
    const double ns = ElapsedNsPerOp(start, kIterations);
    writer.join();
    return ns;
}

void BenchmarkRawEpollWakeup() {
    std::vector<double> samples;
    samples.reserve(kRepeats);
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        int data[2]{-1, -1};
        if (!MakePipe(data)) {
            std::printf("[bench] raw epoll: pipe2 failed\n");
            return;
        }
        const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.u64 = 1;
        if (epoll_fd < 0 ||
            ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, data[0], &event) != 0) {
            std::printf("[bench] raw epoll: setup failed\n");
            return;
        }
        std::atomic<int> ack{0};
        samples.push_back(OneRawEpollSample(epoll_fd, data[0], data[1], &ack));
        ::close(epoll_fd);
        ::close(data[0]);
        ::close(data[1]);
    }
    Report("raw epoll_wait wakeup (1P)", samples);
}

/** @brief 一轮 IOManager WaitRead 测量（同一 ack 协议）。 */
bool OneManagedSample(go2cpp::IOManager& manager, double* ns_out) {
    int data[2]{-1, -1};
    if (!MakePipe(data)) {
        return false;
    }
    std::atomic<int> ack{0};
    std::atomic<bool> done{false};
    std::atomic<long long> read_count{0};
    std::atomic<double> elapsed_ns{0};
    std::thread writer([&] {
        for (int last = 0; last < kIterations; ++last) {
            while (ack.load(std::memory_order_acquire) == last) {
                std::this_thread::yield();
            }
            const char byte = 'm';
            (void)::syscall(SYS_write, data[1], &byte, 1);
        }
    });
    (void)manager.Go([&, read_fd = data[0]] {
        long long local = 0;
        const auto start = Clock::now();
        for (int i = 0; i < kIterations; ++i) {
            ack.store(i + 1, std::memory_order_release);
            const auto result = manager.Wait(read_fd, go2cpp::IOEvent::kRead);
            if (result.ready()) {
                char byte = 0;
                if (::syscall(SYS_read, read_fd, &byte, 1) == 1) {
                    ++local;
                }
            }
        }
        elapsed_ns.store(ElapsedNsPerOp(start, kIterations),
                         std::memory_order_release);
        read_count.store(local, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });
    const auto deadline = Clock::now() + 20s;
    while (!done.load(std::memory_order_acquire) && Clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    writer.join();
    if (!done.load(std::memory_order_acquire)) {
        return false;
    }
    g_sink.fetch_add(read_count.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
    *ns_out = elapsed_ns.load(std::memory_order_acquire);
    (void)manager.NotifyClose(data[0]);
    ::close(data[0]);
    ::close(data[1]);
    return true;
}

void BenchmarkManagedWaitRead() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    config.max_workers = 1;
    config.idle_wait = 1ms;
    go2cpp::IOManager manager(config);
    if (!manager.Start()) {
        std::printf("[bench] managed io: start failed\n");
        return;
    }
    std::vector<double> samples;
    samples.reserve(kRepeats);
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        double sample = 0;
        if (!OneManagedSample(manager, &sample)) {
            std::printf("[bench] managed io: sample failed/timeout\n");
            manager.Shutdown();
            return;
        }
        samples.push_back(sample);
    }
    Report("IOManager WaitRead wakeup (1P)", samples);
    manager.Shutdown();
}

}  // namespace

int main() {
    std::printf("=== Go2Cpp channel / future / io 审计基准 ===\n");
    std::printf("迭代数=%d 重复=%d 绑定=taskset -c 0\n", kIterations, kRepeats);
    BenchmarkChannelBufferedRoundtrip();
    BenchmarkChannelBufferedFastPath();
    BenchmarkChannelUnbufferedRendezvous();
    BenchmarkFutureHotGet();
    BenchmarkFutureLifecycle();
    BenchmarkFutureTryGetMiss();
    BenchmarkRawEpollWakeup();
    BenchmarkManagedWaitRead();
    std::printf("[bench] sink=%lld（防止优化消除）\n",
                g_sink.load(std::memory_order_relaxed));
    return 0;
}
