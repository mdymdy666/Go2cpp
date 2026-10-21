#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/scheduler.hpp"
#include "go2cpp/tools/go.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

double NsPerOp(std::chrono::steady_clock::duration d, long ops) {
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) /
           static_cast<double>(ops);
}

}  // namespace

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 100000;

    // ---- Fiber：两个协程通过无缓冲 channel 来回交接 ----
    {
        go2cpp::Scheduler sched(2);
        auto a = go2cpp::MakeChannel<int>(0);
        auto b = go2cpp::MakeChannel<int>(0);
        const auto start = std::chrono::steady_clock::now();
        auto t1 = go2cpp::go(sched, [a, b, n] {
            for (int i = 0; i < n; ++i) {
                (void)a->Recv();
                (void)b->Send(1);
            }
        });
        auto t2 = go2cpp::go(sched, [a, b, n] {
            for (int i = 0; i < n; ++i) {
                (void)a->Send(1);
                (void)b->Recv();
            }
        });
        t1->wait();
        t2->wait();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double ns = NsPerOp(elapsed, static_cast<long>(n) * 2);
        std::printf("fiber ping-pong: n=%d switches=%ld ns/switch=%.1f\n", n,
                    static_cast<long>(n) * 2, ns);
    }

    // ---- 线程：两个 pthread 通过 mutex+CV 来回交接 ----
    {
        std::mutex m;
        std::condition_variable cv;
        int turn = 0;
        const auto start = std::chrono::steady_clock::now();
        std::thread t1([&] {
            for (int i = 0; i < n; ++i) {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return turn == 0; });
                turn = 1;
                cv.notify_one();
            }
        });
        std::thread t2([&] {
            for (int i = 0; i < n; ++i) {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return turn == 1; });
                turn = 0;
                cv.notify_one();
            }
        });
        t1.join();
        t2.join();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double ns = NsPerOp(elapsed, static_cast<long>(n) * 2);
        std::printf("thread ping-pong: n=%d switches=%ld ns/switch=%.1f\n", n,
                    static_cast<long>(n) * 2, ns);
    }

    return 0;
}
