#include "go2cpp/hook.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <iostream>

/**
 * @brief 演示 Hook 后的 socket 接收、发送和 SO_RCVTIMEO 超时行为。
 * @details 读 Fiber 在没有数据时交还执行权，写 Fiber 发送数据后唤醒读者；
 *          第二次读取验证系统级超时仍会转换为 ETIMEDOUT。
 * @return 数据接收和超时检查均通过时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return 1;
    }
    timeval timeout{0, 100000};
    if (::setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) != 0) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return 1;
    }
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    go2cpp::IOManager manager(config);
    std::atomic<bool> received{false};
    std::atomic<bool> timed_out{false};
    if (!manager.Start()) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return 1;
    }
    auto reader = manager.Go([&] {
        char value = 0;
        received.store(::recv(sockets[0], &value, 1, 0) == 1 && value == 'G');
        const int result = static_cast<int>(::recv(sockets[0], &value, 1, 0));
        timed_out.store(result == -1 && errno == ETIMEDOUT);
    });
    auto writer = manager.Go([&] {
        ::usleep(20000);
        const char value = 'G';
        (void)::send(sockets[1], &value, 1, 0);
    });
    const bool joined = reader->wait_for(3s) && writer->wait_for(3s);
    manager.Shutdown();
    ::close(sockets[0]);
    ::close(sockets[1]);
    std::cout << "hook-enabled=" << go2cpp::hook::IsEnabled()
              << " P=1 recv=" << received.load()
              << " socket-timeout=" << timed_out.load() << '\n';
    return joined && received.load() && timed_out.load() ? 0 : 1;
}
