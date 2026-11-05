// hook 模块微基准：被 LD_PRELOAD 拦截的 read(2) 包装开销 ns/op。
//
// 用法（对照必须成对跑）：
//   taskset -c 1 ./bench_hook                                          # 基线：libc read
//   LD_PRELOAD=<build>/libgo2cpp_hook.so taskset -c 1 ./bench_hook     # Hook 打开
//
// 口径：AF_UNIX socketpair 非阻塞；一轮 = 填满内核接收缓冲区 + 每次
// read(fd, &c, 1) 读到 EAGAIN。数据已在内核缓冲区，不会 park Fiber，
// 因此测得的是「read 系统调用 + Hook 包装（fd 元数据查找 + OpEnabled 判定）」
// 的净开销。单轮约 20 万次 read（≈150 ms），落在 100 ms–1 s 窗口内。

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::printf("hook_socketpair_error=%d\n", errno);
        return 1;
    }
    for (int i = 0; i < 2; ++i) {
        const int flags = ::fcntl(fds[i], F_GETFL, 0);
        (void)::fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }

    char chunk[4096];
    std::memset(chunk, 'x', sizeof(chunk));
    char byte = 0;

    std::size_t total_reads = 0;
    double total_ms = 0.0;
    int rounds = 0;
    while (total_ms < 150.0 && rounds < 8) {
        while (::write(fds[0], chunk, sizeof(chunk)) > 0) {
        }
        const Clock::time_point t0 = Clock::now();
        std::size_t reads = 0;
        while (::read(fds[1], &byte, 1) == 1) {
            ++reads;
        }
        total_ms += MsSince(t0);
        total_reads += reads;
        ++rounds;
    }

    const double ops = static_cast<double>(total_reads);
    std::printf("hook_read_1byte ops=%.0f total_ms=%.3f ops_per_sec=%.0f ns_per_op=%.3f\n", ops,
                total_ms, ops / (total_ms / 1000.0), total_ms * 1e6 / ops);
    std::fflush(stdout);

    ::close(fds[0]);
    ::close(fds[1]);
    return 0;
}
