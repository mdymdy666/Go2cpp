/**
 * @file fake_monitor_manager.cpp
 * @brief e2e 辅助进程：启动真实 `Manager`，把端口写到 stdout，等 stdin EOF 退出。
 *
 * 用法：`fake_monitor_manager [port]`（端口 0 = 内核分配）。
 * 监控数据不在这里伪造：e2e 通过真实 `POST /api/push` 推送快照，这样
 * MCP 工具 → Manager 查询 → 存储回读 的整条链路都是真实实现。
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "go2cpp/os/monitor/manager.hpp"

int main(int argc, char** argv) {
    std::uint16_t port = 0;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    }

    go2cpp::os::monitor::ManagerConfig config;
    config.listen_address = "127.0.0.1";
    config.port = port;
    go2cpp::os::monitor::Manager manager(config);
    if (!manager.Start()) {
        std::fprintf(stderr,
                     "fake_monitor_manager: 启动失败（端口 %u 可能被占用）\n",
                     static_cast<unsigned>(port));
        return 1;
    }

    std::printf("PORT=%u\n", static_cast<unsigned>(manager.port()));
    std::fflush(stdout);

    // 阻塞等父进程关闭 stdin（EOF）后优雅停止；期间 Manager 的 Fiber 处理请求。
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
    }
    manager.Stop();
    return 0;
}
