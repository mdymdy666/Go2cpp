#include "go2cpp/channel.hpp"
#include "go2cpp/context.hpp"
#include "go2cpp/io.hpp"
#include "go2cpp/log.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * @brief socket 等待 Fiber 投递给 Select 的事件。
 * @details socket 的 epoll 等待和 channel 的 Select 是两个独立层次：
 *          IOManager 只负责 fd 就绪，事件桥接 Fiber 把结果转成可选择的
 *          channel 值。这样同一个 Select 可以统一处理 IO 和业务消息。
 */
struct SocketEvent {
    int fd{-1};
    char value{'\0'};
    go2cpp::io::WaitStatus status{go2cpp::io::WaitStatus::kError};
};

/**
 * @brief 记录示例关键路径，并保证日志默认能在终端看到。
 * @param[in] logger 示例使用的 Logger。
 * @param[in] message 要记录的阶段信息。
 */
void LogInfo(const go2cpp::Logger::ptr& logger, const std::string& message) {
    GO2CPP_LOG_INFO(logger) << message;
}

}  // namespace

/**
 * @brief 演示 Fiber IO、Channel Select、超时、取消和错误路径。
 * @details 示例分为四个可以独立理解的场景：
 *          1. WaitManyFor 一次收集两个 socket 的就绪索引；
 *          2. socket 等待 Fiber 把 fd 事件桥接到 channel，Select 同时等待
 *             socket 事件和业务 channel 消息；
 *          3. DefaultCase 在没有事件时立即返回，避免把非阻塞探测写成阻塞；
 *          4. 空 channel 在 Context 取消后返回 Cancelled，展示失败路径的
 *             收敛方式。所有阻塞等待都有有限超时，任务结束后才关闭 fd。
 * @return 所有成功、超时和取消断言均通过时返回 0。
 */
int main() {
    using namespace std::chrono_literals;
    using go2cpp::ChannelStatus;
    using go2cpp::Context;
    using go2cpp::ContextPtr;
    using go2cpp::IOEvent;
    using go2cpp::IOManager;
    using go2cpp::IOWaitManyResult;
    using go2cpp::IOWaitRequest;
    using go2cpp::IOWaitStatus;
    using go2cpp::MakeChannel;
    using go2cpp::RecvCase;
    using go2cpp::Select;

    // 示例把 Info 同时输出到 stdout 和 log/select_io_channel_demo.log，方便
    // 新手观察“注册 -> 触发 -> 消费 -> 清理”的完整链路。
    go2cpp::LoggerManager::Instance().Configure(
        go2cpp::LogLevel::Info, true, "log", "select_io_channel_demo.log");
    const auto logger = GO2CPP_LOG_NAME("SelectIOChannelDemo");
    LogInfo(logger, "开始：初始化 IOManager、socketpair 和业务 channel");

    int first[2]{-1, -1};
    int second[2]{-1, -1};
    int bridge[2]{-1, -1};
    const auto close_pair = [](int pair[2]) {
        for (int index = 0; index < 2; ++index) {
            int& fd = pair[index];
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, first) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, second) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, bridge) != 0) {
        std::cerr << "socketpair 创建失败\n";
        close_pair(first);
        close_pair(second);
        close_pair(bridge);
        return 1;
    }

    go2cpp::SchedulerConfig config;
    config.processor_count = 2;
    config.max_workers = 2;
    IOManager manager(config);
    if (!manager.Start()) {
        std::cerr << "IOManager 启动失败\n";
        close_pair(first);
        close_pair(second);
        close_pair(bridge);
        return 1;
    }

    bool all_ok = true;

    // 场景一：WaitManyFor 不是“等待所有任务完成”的屏障，而是一次返回
    // 当前已经就绪的请求索引。两个 socket 同时写入后，结果应同时包含 0/1。
    const char first_value = 'A';
    const char second_value = 'B';
    // 必须先让 WaitMany 完成全部注册，再写入两个 fd。否则第一个节点
    // 注册后就可能被 epoll 观察到，而第二个节点尚未发布，示例会把
    // 合法的注册时序竞态误判为多路等待丢事件。
    bool sent_many = false;
    std::atomic<bool> many_ok{false};
    auto many_task = manager.Go([&] {
        const std::vector<IOWaitRequest> requests{
            {first[0], IOEvent::Read, {}},
            {second[0], IOEvent::Read, {}},
        };
        const IOWaitManyResult result = manager.WaitManyFor(requests, 2s);
        bool saw_first = false;
        bool saw_second = false;
        if (result.ready()) {
            for (const auto index : result.ready_indices) {
                saw_first = saw_first || index == 0;
                saw_second = saw_second || index == 1;
            }
        }
        char value = '\0';
        if (saw_first) {
            (void)::recv(first[0], &value, 1, 0);
        }
        if (saw_second) {
            (void)::recv(second[0], &value, 1, 0);
        }
        const bool ok = sent_many && saw_first && saw_second;
        many_ok.store(ok, std::memory_order_release);
        GO2CPP_LOG_INFO(logger) << "场景一 WaitManyFor: status="
                                << static_cast<int>(result.status)
                                << " ready_count=" << result.ready_indices.size()
                                << " passed=" << std::boolalpha << ok;
    });

    const auto registration_deadline = std::chrono::steady_clock::now() + 1s;
    while (many_task->state() != go2cpp::GState::kWaiting &&
           std::chrono::steady_clock::now() < registration_deadline) {
        std::this_thread::yield();
    }
    sent_many = ::send(first[1], &first_value, 1, 0) == 1 &&
                ::send(second[1], &second_value, 1, 0) == 1;

    const bool many_finished = many_task->wait_for(3s);
    all_ok = all_ok && many_finished && many_ok.load(std::memory_order_acquire);

    // 场景二：Select 同时监听“业务消息”和“socket 就绪事件”。Select 只
    // 管理 channel；socket_waiter 是桥接 Fiber，在 IOManager 上等待 epoll，
    // 触发后把 SocketEvent 发送到 socket_events。业务发送者使用同一 Select
    // 进入竞争，因此两类阻塞源都会被同一套选择逻辑处理。
    const auto root = Context::Background();
    const auto context_pair = Context::WithTimeout(root, 2s);
    const ContextPtr context = context_pair.first;
    const auto business_events = MakeChannel<int>(0);
    const auto socket_events = MakeChannel<SocketEvent>(0);
    std::atomic<bool> socket_bridge_ok{false};
    std::atomic<bool> business_ok{false};

    auto socket_waiter = manager.Go([&] {
        const auto wait_result = manager.WaitFor(bridge[0], IOEvent::Read,
                                                 1500ms, context);
        if (!wait_result.ready()) {
            GO2CPP_LOG_WARN(logger) << "场景二 socket bridge 未就绪，status="
                                    << static_cast<int>(wait_result.status);
            (void)socket_events->Send(
                SocketEvent{bridge[0], '\0', wait_result.status}, context);
            return;
        }
        SocketEvent event{bridge[0], '\0', wait_result.status};
        const bool read_ok = ::recv(bridge[0], &event.value, 1, 0) == 1;
        socket_bridge_ok.store(read_ok, std::memory_order_release);
        GO2CPP_LOG_INFO(logger) << "场景二 socket 就绪，桥接 value=" << event.value;
        (void)socket_events->Send(event, context);
    });

    auto business_sender = manager.Go([&] {
        // 无缓冲 channel 的 Send 会等待 Select 注册接收者；这正是 Fiber
        // 级阻塞的示范，不能用“先 TrySend，失败就丢弃”的方式替代。
        const auto result = business_events->Send(2026, context);
        const bool ok = result.Ok();
        business_ok.store(ok, std::memory_order_release);
        GO2CPP_LOG_INFO(logger) << "场景二 business channel Send status="
                                << static_cast<int>(result.status)
                                << " passed=" << std::boolalpha << ok;
    });

    // 先验证 DefaultCase：此时第三个空 channel 没有值，应立即返回 Ready，
    // 而不是占住 Fiber 等待。后续的两个 Select 不带 default，才会阻塞。
    const auto empty_channel = MakeChannel<int>(0);
    const auto default_result = Select(
        {RecvCase(empty_channel), go2cpp::DefaultCase()}, context, 100ms);
    const bool default_ok = default_result.selected &&
                            default_result.status == ChannelStatus::kReady &&
                            default_result.index == 1;
    GO2CPP_LOG_INFO(logger) << "场景三 DefaultCase selected="
                            << default_result.index << " passed="
                            << std::boolalpha << default_ok;

    bool saw_business = false;
    bool saw_socket = false;
    // 普通线程写入 socket 的时间不确定，Select 的两轮等待分别收集业务
    // channel 和 socket bridge，任何一次超时都会在日志中看到原因。
    const char bridge_value = 'S';
    const bool bridge_sent = ::send(bridge[1], &bridge_value, 1, 0) == 1;
    for (int round = 0; round < 2; ++round) {
        const auto selected = Select(
            {RecvCase(business_events), RecvCase(socket_events)}, context, 1500ms);
        if (!selected.selected) {
            GO2CPP_LOG_ERROR(logger) << "场景二 Select 未选中事件，status="
                                     << static_cast<int>(selected.status);
            all_ok = false;
            break;
        }
        if (selected.index == 0) {
            saw_business = selected.Value<int>().value_or(0) == 2026;
            GO2CPP_LOG_INFO(logger) << "场景二 Select 收到 business channel，value="
                                    << selected.Value<int>().value_or(-1);
        } else if (selected.index == 1) {
            const auto event = selected.Value<SocketEvent>();
            saw_socket = event.has_value() && event->value == 'S' &&
                         event->status == IOWaitStatus::kReady;
            GO2CPP_LOG_INFO(logger) << "场景二 Select 收到 socket channel，value="
                                    << (event.has_value() ? event->value : '?');
        }
    }
    const bool socket_finished = socket_waiter->wait_for(3s);
    const bool business_finished = business_sender->wait_for(3s);
    context_pair.second();
    all_ok = all_ok && bridge_sent && socket_finished && business_finished &&
             socket_bridge_ok.load(std::memory_order_acquire) &&
             business_ok.load(std::memory_order_acquire) && saw_business &&
             saw_socket && default_ok;

    // 场景四：错误路径。取消 Context 后，对一个永远没有发送者的 channel
    // 执行 Select，返回必须是 Cancelled；示例明确记录该结果，而不是把取消
    // 当作“超时成功”。
    const auto cancel_pair = Context::WithCancel(root);
    const auto never_channel = MakeChannel<int>(0);
    std::atomic<bool> cancel_ok{false};
    auto cancel_task = manager.Go([&] {
        const auto result = Select({RecvCase(never_channel)}, cancel_pair.first,
                                   2s);
        const bool cancelled = !result.selected &&
                               result.status == ChannelStatus::kCancelled;
        cancel_ok.store(cancelled, std::memory_order_release);
        GO2CPP_LOG_INFO(logger) << "场景四 Context 取消 Select status="
                                << static_cast<int>(result.status)
                                << " passed=" << std::boolalpha << cancelled;
    });
    std::this_thread::sleep_for(20ms);
    cancel_pair.second();
    const bool cancel_finished = cancel_task->wait_for(3s);
    all_ok = all_ok && cancel_finished && cancel_ok.load(std::memory_order_acquire);

    manager.Shutdown();
    close_pair(first);
    close_pair(second);
    close_pair(bridge);
    GO2CPP_LOG_INFO(logger) << "示例结束：all_ok=" << std::boolalpha << all_ok;
    go2cpp::LoggerManager::Instance().Flush();
    std::cout << "select-io-channel-demo=" << std::boolalpha << all_ok << '\n';
    return all_ok ? 0 : 1;
}
