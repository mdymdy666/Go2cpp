/**
 * @file platform_probe.cpp
 * @brief Linux 专有 IO 能力探测（io_uring / O_DIRECT），只报告不改变系统。
 *
 * 为什么单独做探测：本原型的瓶颈在「编码 + CRC + page cache 回写」，
 * 不在 syscall 数量；接入 io_uring 的收益需要先确认内核支持与 ring 参数。
 * 该程序用原始 syscall 探测 `io_uring_setup(2)`，不依赖 liburing。
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct IoUringParams {
    std::uint32_t sq_entries;
    std::uint32_t cq_entries;
    std::uint32_t flags;
    std::uint32_t sq_thread_cpu;
    std::uint32_t sq_thread_idle;
    std::uint32_t features;
    std::uint32_t wq_fd;
    std::uint32_t resv[3];
    struct {
        std::uint32_t head;
        std::uint32_t tail;
        std::uint32_t ring_mask;
        std::uint32_t ring_entries;
        std::uint32_t flags;
        std::uint32_t dropped;
        std::uint32_t array;
        std::uint32_t resv1;
        std::uint64_t user_addr;
    } sq_off;
    struct {
        std::uint32_t head;
        std::uint32_t tail;
        std::uint32_t ring_mask;
        std::uint32_t ring_entries;
        std::uint32_t overflow;
        std::uint32_t cqes;
        std::uint32_t flags;
        std::uint32_t resv1;
        std::uint64_t user_addr;
    } cq_off;
};

void ProbeIoUring() {
#if defined(__linux__) && defined(SYS_io_uring_setup)
    IoUringParams params{};
    const long fd = ::syscall(SYS_io_uring_setup, 64U, &params);
    if (fd < 0) {
        std::printf("io_uring: unavailable (errno=%d %s)\n", errno,
                    std::strerror(errno));
        return;
    }
    std::printf(
        "io_uring: available fd=%ld sq_entries=%u cq_entries=%u features=0x%x\n",
        fd, params.sq_entries, params.cq_entries, params.features);
    ::close(static_cast<int>(fd));
#else
    std::printf("io_uring: not applicable (non-Linux build)\n");
#endif
}

void ProbeDirectIo() {
    const char* path = "/tmp/g2c-persistence-direct-probe.bin";
    void* buffer = nullptr;
    if (::posix_memalign(&buffer, 4096, 4096) != 0) {
        std::printf("O_DIRECT: probe allocation failed\n");
        return;
    }
    std::memset(buffer, 0x5A, 4096);
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) {
        std::printf("O_DIRECT: unavailable (errno=%d %s)\n", errno,
                    std::strerror(errno));
        std::free(buffer);
        return;
    }
    const ssize_t written = ::pwrite(fd, buffer, 4096, 0);
    std::printf("O_DIRECT: available write=%zd bytes (aligned 4096)\n", written);
    ::close(fd);
    ::unlink(path);
    std::free(buffer);
}

}  // namespace

int main() {
    std::printf("platform: linux=%d\n",
#if defined(__linux__)
                1
#else
                0
#endif
    );
    ProbeIoUring();
    ProbeDirectIo();
    return 0;
}
