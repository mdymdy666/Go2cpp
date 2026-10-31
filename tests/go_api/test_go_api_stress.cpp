/**
 * @file test_go_api_stress.cpp
 * @brief Go 接口对接层的并发 / 随机 / 长链路压力测试。
 *
 * 与 `test_go_net.cpp`、`test_go_concurrency.cpp` 的「场景正确性」互补：
 * 这里追求**并发压力 + 随机化 + 真实链路**，覆盖：
 *   1. TCP 并发 Dial/Listen：8 客户端 × 30 轮，随机长度、随机内容、长度前缀协议；
 *   2. Chan 压测：4 生产者 × 20000 条 + 2 消费者；
 *   3. Mutex 争用：8 线程 × 20000 次 ++；
 *   4. Context 取消/超时风暴：500 个随机 deadline；
 *   5. time.After/Ticker 并发：200 个随机定时器 + 10 个 ticker；
 *   6. errors 图遍历压测：5000 次 Wrap/Join/Is/As。
 *
 * 全部在普通线程上运行（不依赖 Scheduler 启动），可直接作为 CI 冒烟。
 */

#include "go2cpp/go/go.hpp"
#include "go_api_check.hpp"
#include "go_api_util.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/** 写满 size 字节（处理部分写）。 */
bool WriteAll(go::net::Conn& conn, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const auto written = conn.Write(bytes + offset, size - offset);
        if (written.err || written.n == 0) {
            return false;
        }
        offset += written.n;
    }
    return true;
}

/** 场景 1：TCP 并发回环，随机长度 + 长度前缀。 */
void StressTcpConcurrent() {
    GO_ANNOUNCE("stress: tcp 8 clients x 30 rounds, random payload");
    auto listen = go_api_test::ListenLoopbackTCP();
    GO_REQUIRE(listen.Ok());
    const auto port = go_api_test::ParsePort(listen.listener->Addr().String());
    GO_REQUIRE(port != 0);

    std::atomic<bool> stop{false};
    std::atomic<int> served{0};
    std::atomic<int> server_failures{0};
    std::thread server([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto accepted = listen.listener->Accept();
            if (!accepted.first) {
                break;
            }
            auto conn = accepted.first;
            std::uint32_t length = 0;
            if (!go_api_test::ReadExact(*conn, &length, sizeof(length))) {
                server_failures.fetch_add(1);
                (void)conn->Close();
                continue;
            }
            std::string payload(length, '\0');
            if (!go_api_test::ReadExact(*conn, payload.data(), payload.size())) {
                server_failures.fetch_add(1);
                (void)conn->Close();
                continue;
            }
            if (!WriteAll(*conn, &length, sizeof(length)) ||
                !WriteAll(*conn, payload.data(), payload.size())) {
                server_failures.fetch_add(1);
            }
            (void)conn->Close();
            served.fetch_add(1);
        }
    });

    constexpr int kClients = 8;
    constexpr int kRounds = 30;
    std::atomic<int> client_failures{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int client = 0; client < kClients; ++client) {
        clients.emplace_back([&, client] {
            std::mt19937 generator(static_cast<std::uint32_t>(client) * 7919u + 17u);
            std::uniform_int_distribution<std::uint32_t> size_dist(1, 4096);
            for (int round = 0; round < kRounds; ++round) {
                auto dialed =
                    go::net::Dial("tcp", "127.0.0.1:" + std::to_string(port));
                if (!dialed.Ok()) {
                    client_failures.fetch_add(1);
                    continue;
                }
                auto conn = dialed.conn;
                const std::uint32_t length = size_dist(generator);
                std::string payload(length, '\0');
                for (std::uint32_t index = 0; index < length; ++index) {
                    payload[index] = static_cast<char>(generator() & 0xff);
                }
                if (!WriteAll(*conn, &length, sizeof(length)) ||
                    !WriteAll(*conn, payload.data(), payload.size())) {
                    client_failures.fetch_add(1);
                    (void)conn->Close();
                    continue;
                }
                std::uint32_t echoed_length = 0;
                if (!go_api_test::ReadExact(*conn, &echoed_length,
                                            sizeof(echoed_length)) ||
                    echoed_length != length) {
                    client_failures.fetch_add(1);
                    (void)conn->Close();
                    continue;
                }
                std::string echoed(echoed_length, '\0');
                if (!go_api_test::ReadExact(*conn, echoed.data(), echoed.size()) ||
                    echoed != payload) {
                    client_failures.fetch_add(1);
                }
                (void)conn->Close();
            }
        });
    }
    for (auto& client : clients) {
        client.join();
    }
    stop.store(true);
    (void)listen.listener->Close();
    server.join();

    GO_CHECK(client_failures.load() == 0);
    GO_CHECK(server_failures.load() == 0);
    GO_CHECK(served.load() == kClients * kRounds);
}

/** 场景 2：Chan 4 生产者 × 20000 + 2 消费者。 */
void StressChan() {
    GO_ANNOUNCE("stress: chan 4 producers x 20000 + 2 consumers");
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 20000;
    constexpr int kConsumers = 2;
    auto channel = go::Chan<int>::Make(64);

    std::atomic<std::int64_t> received{0};
    std::atomic<std::int64_t> sum{0};
    std::vector<std::thread> consumers;
    for (int index = 0; index < kConsumers; ++index) {
        consumers.emplace_back([&] {
            // 阻塞接收；生产者全部结束后由主线程 Close 唤醒并退出。
            for (;;) {
                int value = 0;
                if (!channel.Recv(value)) {
                    break;
                }
                received.fetch_add(1, std::memory_order_relaxed);
                sum.fetch_add(value, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < kPerProducer; ++index) {
                if (!channel.Send(producer * kPerProducer + index)) {
                    break;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    // 生产结束；关闭通道让消费者排空缓冲后退出（Go 的 close(ch) + range 语义）。
    channel.Close();
    for (auto& consumer : consumers) {
        consumer.join();
    }

    GO_CHECK(received.load() == kProducers * kPerProducer);
    const std::int64_t expected =
        static_cast<std::int64_t>(kProducers) * kPerProducer *
        (kProducers * kPerProducer - 1) / 2;
    GO_CHECK(sum.load() == expected);
}

/** 场景 3：Mutex 8 线程 × 20000 次。 */
void StressMutex() {
    GO_ANNOUNCE("stress: mutex 8 threads x 20000");
    go::Mutex mutex;
    std::int64_t counter = 0;
    constexpr int kThreads = 8;
    constexpr int kIterations = 20000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                mutex.Lock();
                ++counter;
                mutex.Unlock();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GO_CHECK(counter == static_cast<std::int64_t>(kThreads) * kIterations);
}

/** 场景 4：Context 取消/超时风暴。 */
void StressContext() {
    GO_ANNOUNCE("stress: context 500 randomized deadlines");
    std::atomic<int> expired{0};
    std::atomic<int> not_expired{0};
    constexpr int kTotal = 500;
    std::vector<std::thread> threads;
    constexpr int kWorkers = 8;
    threads.reserve(kWorkers);
    for (int worker = 0; worker < kWorkers; ++worker) {
        threads.emplace_back([&, worker] {
            std::mt19937 generator(static_cast<std::uint32_t>(worker) * 104729u + 3u);
            std::uniform_int_distribution<int> delay_dist(0, 3);
            for (int index = 0; index < kTotal / kWorkers; ++index) {
                const auto delay =
                    go::Duration::FromNanoseconds(delay_dist(generator) * 1000000LL);
                auto timed = go::WithTimeout(go::Background(), delay);
                go::WaitDone(timed.context, 200 * go::Millisecond);
                if (go::Done(timed.context).IsDone()) {
                    expired.fetch_add(1);
                } else {
                    not_expired.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    GO_CHECK(expired.load() + not_expired.load() ==
             (kTotal / kWorkers) * kWorkers);
    GO_CHECK(not_expired.load() == 0);
}

/** 场景 5：200 个 After + 10 个 Ticker。 */
void StressTime() {
    GO_ANNOUNCE("stress: 200 After + 10 Tickers");
    constexpr int kTimers = 200;
    std::vector<go::Chan<go::Time>> timers;
    timers.reserve(kTimers);
    std::mt19937 generator(2024);
    std::uniform_int_distribution<int> delay_dist(1, 20);
    for (int index = 0; index < kTimers; ++index) {
        timers.push_back(go::AfterChan(
            go::Duration::FromNanoseconds(delay_dist(generator) * 1000000LL)));
    }
    int fired = 0;
    for (auto& timer : timers) {
        auto value = go_api_test::RecvWithin(timer, 2 * go::Second);
        if (value.has_value()) {
            ++fired;
        }
    }
    GO_CHECK(fired == kTimers);

    constexpr int kTickers = 10;
    std::vector<go::Ticker::Ptr> tickers;
    tickers.reserve(kTickers);
    for (int index = 0; index < kTickers; ++index) {
        tickers.push_back(go::NewTicker(5 * go::Millisecond));
    }
    int ticks = 0;
    for (auto& ticker : tickers) {
        for (int round = 0; round < 5; ++round) {
            auto value = go_api_test::RecvWithin(ticker->C(), 2 * go::Second);
            if (value.has_value()) {
                ++ticks;
            }
        }
        ticker->Stop();
    }
    GO_CHECK(ticks == kTickers * 5);
}

/** 场景 6：errors 图遍历压测。 */
void StressErrors() {
    GO_ANNOUNCE("stress: errors 1000 wraps/joins");
    auto root = go::New("root");
    go::error current = root;
    for (int index = 0; index < 1000; ++index) {
        current = go::Wrap(current, "layer-" + std::to_string(index));
        if (!go::Is(current, root)) {
            GO_CHECK_MSG(false, "Is() lost root at layer " + std::to_string(index));
            break;
        }
    }
    auto joined = go::Join({current, root, go::New("extra")});
    GO_CHECK(go::Is(joined, root));
}

}  // namespace

int main() {
    StressTcpConcurrent();
    StressChan();
    StressMutex();
    StressContext();
    StressTime();
    StressErrors();
    return go_api_test::ExitCode();
}
