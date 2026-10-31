#include "go2cpp/hook.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <iostream>

// io_hook 全面版：同一个组件的 7 个使用场景。
//   1  Hook 开关与作用域      IsEnabled / ScopedEnable 的进入与恢复
//   2  IOManager + hook recv  受管 Fiber 里用原生 recv，无数据时交还执行权
//   3  hook send              写端发送后唤醒读者
//   4  ★ 读 Fiber 不阻塞 M     P=1 下"读者先等、写者后发"仍能跑通
//   5  SO_RCVTIMEO 超时        系统级超时转换成 ETIMEDOUT
//   6  ScopedIOManagerBinding 作用域绑定/解绑 IOManager
//   7  connect_timeout        读写该超时配置
//
// 说明：全部场景使用 socketpair（AF_UNIX），**不监听任何端口、不依赖外部网络**。
//       本目标只在构建出 go2cpp_hook 时编译（与既有 io_hook_demo 一致）。

namespace {

using Ms = std::chrono::milliseconds;

// socketpair 的 RAII 包装，确保两条 fd 一定被关闭。
struct SocketPair {
    int fds[2]{-1, -1};
    bool open() {
        return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
    }
    ~SocketPair() {
        if (fds[0] >= 0) { ::close(fds[0]); }
        if (fds[1] >= 0) { ::close(fds[1]); }
    }
};

bool set_recv_timeout(int fd, int microseconds) {
    timeval timeout{0, microseconds};
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
}

go2cpp::IOManager make_manager() {
    go2cpp::SchedulerConfig config;
    config.processor_count = 1;
    return go2cpp::IOManager(config);
}

bool scenario_toggle() {
    std::clog << "[full][场景1] Hook 开关与作用域守卫\n";
    const bool before = go2cpp::hook::IsEnabled();
    bool inside = !before;
    {
        go2cpp::hook::ScopedEnable scope;        // 构造时启用，析构时恢复
        inside = go2cpp::hook::IsEnabled();
    }
    const bool restored = go2cpp::hook::IsEnabled() == before;
    std::cout << "  进入前=" << before << " 作用域内=" << inside
              << " 退出后恢复=" << restored << "（预期 作用域内=1，恢复=1）\n";
    return inside && restored;
}

bool scenario_recv(go2cpp::IOManager& manager) {
    std::clog << "[full][场景2] 受管 Fiber 里的 hook recv\n";
    SocketPair pair;
    if (!pair.open()) {
        return false;
    }
    std::atomic<bool> received{false};
    auto reader = manager.Go([&] {
        char value = 0;
        const auto got = ::recv(pair.fds[0], &value, 1, 0);
        received.store(got == 1 && value == 'R');
    });
    auto writer = manager.Go([&] {
        ::usleep(20000);
        const char value = 'R';
        (void)::send(pair.fds[1], &value, 1, 0);
    });
    const bool joined = reader->wait_for(std::chrono::seconds(3)) &&
                        writer->wait_for(std::chrono::seconds(3));
    std::cout << "  recv 成功=" << received.load() << " 都结束=" << joined
              << "（预期 1 1）\n";
    return received.load() && joined;
}

bool scenario_send(go2cpp::IOManager& manager) {
    std::clog << "[full][场景3] hook send 送达对端\n";
    SocketPair pair;
    if (!pair.open()) {
        return false;
    }
    std::atomic<int> sent{-1};
    std::atomic<bool> got_back{false};
    auto sender = manager.Go([&] {
        const char payload[4] = {'p', 'i', 'n', 'g'};
        sent.store(static_cast<int>(::send(pair.fds[1], payload, sizeof(payload), 0)));
    });
    (void)sender->wait_for(std::chrono::seconds(3));

    // 在主线程把数据读回来（非阻塞读，因为数据已就绪）
    char buffer[8] = {0};
    const auto got = ::recv(pair.fds[0], buffer, sizeof(buffer), 0);
    got_back.store(got == 4 && buffer[0] == 'p' && buffer[3] == 'g');
    std::cout << "  send 返回=" << sent.load() << " 对端收到=" << got_back.load()
              << "（预期 4 1）\n";
    return sent.load() == 4 && got_back.load();
}

bool scenario_reader_does_not_block_m(go2cpp::IOManager& manager) {
    std::clog << "[full][场景4] 读 Fiber 不阻塞 M（P=1 反证）\n";
    SocketPair pair;
    if (!pair.open()) {
        return false;
    }
    std::atomic<bool> reader_done{false};
    // 读者先启动并在无数据时等待；只有它把执行权交还，写者才可能运行。
    auto reader = manager.Go([&] {
        char value = 0;
        const auto got = ::recv(pair.fds[0], &value, 1, 0);
        reader_done.store(got == 1 && value == 'X');
    });
    auto writer = manager.Go([&] {
        ::usleep(30000);
        const char value = 'X';
        (void)::send(pair.fds[1], &value, 1, 0);
    });
    // ★ 先等 reader 结束：若 hook 未生效、recv 直接阻塞了唯一的 M，
    //   写者永远排不上，这里会超时失败。
    const bool reader_joined = reader->wait_for(std::chrono::seconds(3));
    const bool writer_joined = writer->wait_for(std::chrono::seconds(3));
    std::cout << "  读者完成=" << reader_done.load() << " 读者结束=" << reader_joined
              << " 写者结束=" << writer_joined << "（预期 1 1 1）\n";
    return reader_done.load() && reader_joined && writer_joined;
}

bool scenario_recv_timeout(go2cpp::IOManager& manager) {
    std::clog << "[full][场景5] SO_RCVTIMEO 转换为 ETIMEDOUT\n";
    SocketPair pair;
    if (!pair.open() || !set_recv_timeout(pair.fds[0], 100000)) {
        return false;
    }
    std::atomic<bool> timed_out{false};
    std::atomic<int> seen_errno{0};
    auto reader = manager.Go([&] {
        char value = 0;
        const auto result = ::recv(pair.fds[0], &value, 1, 0);
        seen_errno.store(errno);
        timed_out.store(result == -1 && errno == ETIMEDOUT);
    });
    (void)reader->wait_for(std::chrono::seconds(3));
    std::cout << "  超时=" << timed_out.load() << " errno=" << seen_errno.load()
              << "（预期超时=1，errno=" << ETIMEDOUT << "）\n";
    return timed_out.load();
}

bool scenario_binding_scope(go2cpp::IOManager& manager) {
    std::clog << "[full][场景6] ScopedIOManagerBinding 作用域绑定\n";
    SocketPair pair;
    if (!pair.open()) {
        return false;
    }
    std::atomic<bool> inside_ok{false};
    {
        go2cpp::hook::ScopedIOManagerBinding binding(manager);
        // 绑定生效期间，受管 Fiber 里的 syscall 由该 manager 接管
        auto task = manager.Go([&] {
            char value = 0;
            const auto got = ::recv(pair.fds[0], &value, 1, 0);
            inside_ok.store(got == 1 && value == 'B');
        });
        ::usleep(20000);
        const char value = 'B';
        (void)::send(pair.fds[1], &value, 1, 0);
        (void)task->wait_for(std::chrono::seconds(3));
    }
    // 解绑后不应影响后续流程
    const bool after = go2cpp::hook::IsEnabled();
    std::cout << "  作用域内接收成功=" << inside_ok.load()
              << " 解绑后 hook 状态=" << after << "（预期 1）\n";
    return inside_ok.load();
}

bool scenario_connect_timeout() {
    std::clog << "[full][场景7] connect_timeout 配置读写\n";
    const auto before = go2cpp::hook::connect_timeout();
    go2cpp::hook::set_connect_timeout(Ms(1234));
    const auto changed = go2cpp::hook::connect_timeout();
    go2cpp::hook::set_connect_timeout(before);        // 还原，避免影响其它用例
    const auto restored = go2cpp::hook::connect_timeout();
    const bool ok = changed == Ms(1234) && restored == before;
    std::cout << "  原值=" << before.count() << "ms 设为 1234ms 后="
              << changed.count() << "ms 还原后=" << restored.count()
              << "ms（预期 1234 与 原值）\n";
    return ok;
}

}  // namespace

/**
 * @brief io_hook 全面版：7 个场景，覆盖开关、收发、超时与绑定作用域。
 * @details 全部使用 socketpair，不监听端口、不依赖外部网络。
 * @return 全部场景通过返回 0，否则返回 1。
 */
int main() {
    auto manager = make_manager();
    if (!manager.Start()) {
        std::cerr << "IOManager 启动失败\n";
        return 1;
    }
    // 整个示例期间启用 hook（退出时自动恢复）
    go2cpp::hook::ScopedEnable hook_scope;

    bool ok = true;
    ok = scenario_toggle() && ok;
    ok = scenario_recv(manager) && ok;
    ok = scenario_send(manager) && ok;
    ok = scenario_reader_does_not_block_m(manager) && ok;
    ok = scenario_recv_timeout(manager) && ok;
    ok = scenario_binding_scope(manager) && ok;
    ok = scenario_connect_timeout() && ok;

    manager.Shutdown();
    std::clog << (ok ? "[full] 7 个场景全部通过\n" : "[full] 存在失败场景\n");
    return ok ? 0 : 1;
}
