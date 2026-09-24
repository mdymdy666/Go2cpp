#include "go2cpp/io.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>

int main() {
    using namespace std::chrono_literals;

    int first[2]{-1, -1};
    int second[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, first) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, second) != 0) {
        return 1;
    }

    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    go2cpp::IOManager manager(config);
    if (!manager.Start()) {
        ::close(first[0]);
        ::close(first[1]);
        ::close(second[0]);
        ::close(second[1]);
        return 1;
    }

    // 先写入两个端点，WaitMany 会在同一次等待中观察两个就绪请求。
    const char first_value = 'A';
    const char second_value = 'B';
    const bool sent = ::send(first[1], &first_value, 1, 0) == 1 &&
                      ::send(second[1], &second_value, 1, 0) == 1;
    std::atomic<bool> result{false};

    auto waiter = manager.Go([&] {
        const std::vector<go2cpp::IOWaitRequest> requests{
            go2cpp::IOWaitRequest{first[0], go2cpp::IOEvent::Read, {}},
            go2cpp::IOWaitRequest{second[0], go2cpp::IOEvent::Read, {}},
        };
        const auto many = manager.WaitManyFor(requests, 2s);
        bool saw_first = false;
        bool saw_second = false;
        if (many.ready()) {
            for (const auto index : many.ready_indices) {
                if (index == 0) {
                    saw_first = true;
                } else if (index == 1) {
                    saw_second = true;
                }
            }
        }

        char value = 0;
        if (saw_first) {
            (void)::recv(first[0], &value, 1, 0);
        }
        if (saw_second) {
            (void)::recv(second[0], &value, 1, 0);
        }

        // 两个字节都已取走，第二次等待展示 timeout 结果。
        const auto timeout = manager.WaitAnyFor(
            {{first[0], go2cpp::IOEvent::Read, {}}}, 20ms);
        const bool timeout_ok =
            timeout.status == go2cpp::io::WaitStatus::kTimeout;
        result.store(sent && saw_first && saw_second && timeout_ok,
                     std::memory_order_release);
    });

    const bool completed = waiter->wait_for(3s);
    const bool final_result =
        completed && result.load(std::memory_order_acquire);
    manager.Shutdown();
    ::close(first[0]);
    ::close(first[1]);
    ::close(second[0]);
    ::close(second[1]);

    std::cout << "wait-many=" << std::boolalpha << final_result << '\n';
    return final_result ? 0 : 1;
}
