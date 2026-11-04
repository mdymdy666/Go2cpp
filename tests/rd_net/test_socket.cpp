#include "go2cpp/rd_net/socket.hpp"

#include "go2cpp/bufio/batch.hpp"

/// @file test_socket.cpp
/// @brief REQ-006 验收：四态可分（kClosed / kReset / kTruncated / kWouldBlock）
///        + 1e4 无泄漏 + 100 socket 不串包 + 10 万次压测。
///
/// @note Windows 兼容（REQ-010）：本文件原来是纯 POSIX 写法（arpa/inet.h、poll.h、
///       unistd.h、dirent.h）。平台差异全部收口在下面的 `tsock` 命名空间里：
///       - `poll`            → `select`（Win7 SP1 可用；测试只等「可读」，不用
///                             WSAPoll——它对非阻塞 connect/UDP 的语义与 poll 有差异）；
///       - `close`           → `closesocket`；
///       - `errno`           → `WSAGetLastError()`（EINTR→WSAEINTR 等一一对应）；
///       - `/proc/self/fd`   → `GetProcessHandleCount`（口径是进程全部句柄，含非
///                             socket 的，比 Linux fd 计数宽，但仍能抓每轮泄漏）；
///       - `/proc/self/status` → `GetProcessMemoryInfo().WorkingSetSize`。
///       断言条数与 POSIX 分支完全一致：平台层只替换系统调用，不动任何 CHECK_*。

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#if defined(_WIN32)
// winsock2.h 必须先于 windows.h；WIN32_LEAN_AND_MEAN 阻止 windows.h 再拉 winsock.h。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef WIN32_EXTRA_LEAN
#define WIN32_EXTRA_LEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// NOMINMAX：windows.h 默认定义 min/max 宏，会破坏 std::min / std::max。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK_TRUE(cond)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                       \
    } while (0)

#define CHECK_STATUS(actual, expected)                                          \
    do {                                                                        \
        ++g_checks;                                                             \
        const rd::net::IoStatus a_ = (actual);                                  \
        const rd::net::IoStatus e_ = (expected);                                \
        if (!(a_ == e_)) {                                                      \
            ++g_failures;                                                       \
            std::printf("[FAIL] %s:%d: status=%s(%d) expected=%s(%d)\n",         \
                        __FILE__, __LINE__, rd::net::IoStatusName(a_),          \
                        static_cast<int>(a_), rd::net::IoStatusName(e_),        \
                        static_cast<int>(e_));                                  \
        }                                                                       \
    } while (0)

/// @brief 测试侧的平台抽象层：只做「POSIX 调用 → 等价 Winsock 调用」的一对一映射。
/// @note 每个函数的返回值语义与 POSIX 原调用保持一致，所以上层断言
///       （406 条 checks）一字不改。
namespace tsock {

/// @brief 跨平台 socket 句柄。
/// @note 存 `std::uintptr_t`：Windows 的 SOCKET 是 UINT_PTR（64 位），
///       塞进 int 会被截断并触发 /W4 C4244；POSIX 的 fd 也放得下。
using Handle = std::uintptr_t;
/// @brief 无效句柄：POSIX 的 -1 与 Windows 的 INVALID_SOCKET 转 uintptr_t 后都是全 1。
constexpr Handle kInvalidHandle = ~static_cast<Handle>(0);

/// @brief 句柄是否有效（POSIX: fd>=0；Windows: SOCKET != INVALID_SOCKET）。
inline bool Valid(Handle handle) { return handle != kInvalidHandle; }

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;
constexpr int kConnReset = WSAECONNRESET;
constexpr int kMsgSize = WSAEMSGSIZE;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNative = -1;
constexpr int kConnReset = ECONNRESET;
constexpr int kMsgSize = EMSGSIZE;
#endif

/// @brief 句柄 → 原生类型（POSIX: int；Windows: SOCKET）。
inline NativeSocket Native(Handle handle) { return static_cast<NativeSocket>(handle); }

/// @brief 原生类型 → 句柄。
inline Handle FromNative(NativeSocket native) { return static_cast<Handle>(native); }

/// @brief 最近一次 socket 系统调用的错误码（POSIX: errno；Windows: WSA*）。
inline int LastError() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

/// @brief 是否被中断（应重试）。POSIX 是 EINTR，Windows 是 WSAEINTR。
inline bool IsInterrupted(int error) {
#if defined(_WIN32)
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

/// @brief 初始化平台 socket 运行时（POSIX 空操作）。失败返回 false。
inline bool Init() {
#if defined(_WIN32)
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

/// @brief 新建 TCP socket；失败返回 kInvalidHandle。
inline Handle TcpSocket() {
    return FromNative(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
}

/// @brief bind 到给定地址；成功返回 0。
inline int Bind(Handle fd, const sockaddr_in& addr) {
#if defined(_WIN32)
    return ::bind(Native(fd), reinterpret_cast<const sockaddr*>(&addr),
                  static_cast<int>(sizeof(addr)));
#else
    return ::bind(Native(fd), reinterpret_cast<const sockaddr*>(&addr),
                  static_cast<socklen_t>(sizeof(addr)));
#endif
}

/// @brief listen；成功返回 0。
inline int Listen(Handle fd, int backlog) { return ::listen(Native(fd), backlog); }

/// @brief accept；被信号/中断打断时重试，失败返回 kInvalidHandle。
inline Handle Accept(Handle listen_fd) {
    for (;;) {
        const NativeSocket accepted = ::accept(Native(listen_fd), nullptr, nullptr);
        if (accepted != kInvalidNative) {
            return FromNative(accepted);
        }
        if (!IsInterrupted(LastError())) {
            return kInvalidHandle;
        }
    }
}

/// @brief 取本地绑定地址；成功返回 0。
inline int GetSockName(Handle fd, sockaddr_in& addr) {
#if defined(_WIN32)
    int len = static_cast<int>(sizeof(addr));
    return ::getsockname(Native(fd), reinterpret_cast<sockaddr*>(&addr), &len);
#else
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    return ::getsockname(Native(fd), reinterpret_cast<sockaddr*>(&addr), &len);
#endif
}

/// @brief setsockopt；成功返回 0。长度统一由本函数转成平台类型。
inline int SetOpt(Handle fd, int level, int name, const void* value, std::size_t len) {
#if defined(_WIN32)
    return ::setsockopt(Native(fd), level, name, static_cast<const char*>(value),
                        static_cast<int>(len));
#else
    return ::setsockopt(Native(fd), level, name, value, static_cast<socklen_t>(len));
#endif
}

/// @brief 关闭写方向（POSIX SHUT_WR / Windows SD_SEND）。
inline int ShutdownWrite(Handle fd) {
#if defined(_WIN32)
    return ::shutdown(Native(fd), SD_SEND);
#else
    return ::shutdown(Native(fd), SHUT_WR);
#endif
}

/// @brief 关闭句柄（POSIX close / Windows closesocket）。
inline int Close(Handle fd) {
#if defined(_WIN32)
    return ::closesocket(Native(fd));
#else
    return ::close(Native(fd));
#endif
}

/// @brief 等待句柄可读（有数据或 EOF）。返回 >0 可读、0 超时、<0 错误。
/// @note POSIX 用 poll(POLLIN)；Windows 用 select（等价「可读」语义，且不会像
///       WSAPoll 那样在连接重置/非阻塞 connect 上给出与 poll 不同的 revents）。
inline int WaitReadable(Handle fd, int timeout_ms) {
#if defined(_WIN32)
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(Native(fd), &read_set);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
        const int rc = ::select(0, &read_set, nullptr, nullptr, &timeout);
        if (rc < 0 && IsInterrupted(LastError())) {
            continue;
        }
        return rc;
    }
#else
    pollfd pfd{};
    pfd.fd = Native(fd);
    pfd.events = POLLIN;
    for (;;) {
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            return rc;
        }
        if (rc < 0 && IsInterrupted(LastError())) {
            continue;
        }
        return rc;
    }
#endif
}

/// @brief 非阻塞接收一段数据（不消费未就绪的数据）。
/// @return >0 字节数；0 对端关闭；-1 无数据可读或出错（LastError 给出原因）。
/// @note Windows 没有 MSG_DONTWAIT：先用零超时 select 探测，未就绪时按
///       WSAEWOULDBLOCK 返回，保持与 POSIX MSG_DONTWAIT 相同的调用约定。
///       注意这里**不**把 socket 改成非阻塞模式——同一条连接后面还要被阻塞读
///       （ReadSome）使用。
inline std::ptrdiff_t RecvNonBlocking(Handle fd, void* buf, std::size_t len) {
#if defined(_WIN32)
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(Native(fd), &read_set);
    timeval timeout{0, 0};
    const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
    if (ready == 0) {
        ::WSASetLastError(WSAEWOULDBLOCK);
        return -1;
    }
    if (ready < 0) {
        return -1;
    }
    return ::recv(Native(fd), static_cast<char*>(buf), static_cast<int>(len), 0);
#else
    return ::recv(Native(fd), buf, len, MSG_DONTWAIT);
#endif
}

/// @brief 阻塞读一段数据（TCP 字节流）。返回 >0 字节数；0 对端关闭；<0 错误。
inline std::ptrdiff_t ReadSome(Handle fd, void* buf, std::size_t len) {
#if defined(_WIN32)
    return ::recv(Native(fd), static_cast<char*>(buf), static_cast<int>(len), 0);
#else
    return ::read(Native(fd), buf, len);
#endif
}

/// @brief 休眠指定毫秒（POSIX: usleep；Windows: Sleep）。
inline void SleepMs(int ms) {
#if defined(_WIN32)
    ::Sleep(static_cast<DWORD>(ms));
#else
    ::usleep(static_cast<useconds_t>(ms) * 1000u);
#endif
}

/// @brief 当前进程打开的句柄数（只用于「前后不变」比较）。失败返回 -1。
/// @note Windows 没有 /proc/self/fd，改用 GetProcessHandleCount（Vista+，Win7 可用）。
///       口径是**进程全部句柄**（含非 socket），比 Linux 的 fd 计数宽；
///       对「每轮泄漏一个句柄」的检测能力一致。
inline int CountOpenHandles() {
#if defined(_WIN32)
    DWORD count = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &count) == 0) {
        return -1;
    }
    return static_cast<int>(count);
#else
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    ::closedir(dir);
    return count;
#endif
}

/// @brief 当前进程常驻内存（KB）。无可用接口时返回 -1。
inline long ProcessRssKb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters,
                               static_cast<DWORD>(sizeof(counters))) == 0) {
        return -1;
    }
    return static_cast<long>(counters.WorkingSetSize / 1024u);
#else
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return -1;
    }
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            if (std::sscanf(line + 6, "%ld", &kb) != 1) {
                kb = -1;
            }
            break;
        }
    }
    std::fclose(file);
    return kb;
#endif
}

}  // namespace tsock

/// @brief 常驻内存（KB），名字沿用 Linux 口径；Windows 走 WorkingSetSize。
long ReadVmRssKb() { return tsock::ProcessRssKb(); }

/// @brief 打开句柄数，名字沿用 Linux 口径；Windows 走 GetProcessHandleCount。
int CountOpenFds() { return tsock::CountOpenHandles(); }

/// @brief 等句柄可读。原先是 poll(POLLIN)，Windows 分支用 select 等价实现。
int WaitEvents(tsock::Handle fd, int timeout_ms) {
    return tsock::WaitReadable(fd, timeout_ms);
}

/// @brief 建一个只监听 127.0.0.1 的 TCP listener（端口由内核分配）。失败返回无效句柄。
tsock::Handle MakeListener(std::uint16_t* port_out) {
    const tsock::Handle fd = tsock::TcpSocket();
    if (!tsock::Valid(fd)) {
        return tsock::kInvalidHandle;
    }
    const int one = 1;
    tsock::SetOpt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (tsock::Bind(fd, addr) != 0) {
        tsock::Close(fd);
        return tsock::kInvalidHandle;
    }
    if (tsock::Listen(fd, 4) != 0) {
        tsock::Close(fd);
        return tsock::kInvalidHandle;
    }
    if (tsock::GetSockName(fd, addr) != 0) {
        tsock::Close(fd);
        return tsock::kInvalidHandle;
    }
    *port_out = ntohs(addr.sin_port);
    return fd;
}

/// @brief accept 一个连接（EINTR 重试）。失败返回无效句柄。
tsock::Handle AcceptOne(tsock::Handle listen_fd) { return tsock::Accept(listen_fd); }

/// @brief 可校验的确定性字节模式，用于「不丢数据」比对。
void FillPattern(std::uint8_t* dst, std::size_t n, std::size_t offset) {
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = static_cast<std::uint8_t>(((offset + i) * 31u + 7u) & 0xFFu);
    }
}

bool VerifyPattern(const std::uint8_t* src, std::size_t n, std::size_t offset,
                   std::size_t* bad_offset) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t want = static_cast<std::uint8_t>(((offset + i) * 31u + 7u) & 0xFFu);
        if (src[i] != want) {
            if (bad_offset != nullptr) {
                *bad_offset = offset + i;
            }
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- 场景 1：FIN
void Scenario1Fin() {
    std::printf("\n== 场景 1: TCP 对端 shutdown(SHUT_WR) -> kClosed ==\n");
    std::uint16_t port = 0;
    const tsock::Handle listen_fd = MakeListener(&port);
    CHECK_TRUE(tsock::Valid(listen_fd));
    if (!tsock::Valid(listen_fd)) {
        return;
    }
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port});
    CHECK_TRUE(opened.ok());
    if (!opened.ok()) {
        tsock::Close(listen_fd);
        return;
    }
    rd::net::StreamSocket client = std::move(*opened);
    const tsock::Handle peer = AcceptOne(listen_fd);
    tsock::Close(listen_fd);
    CHECK_TRUE(tsock::Valid(peer));
    if (!tsock::Valid(peer)) {
        return;
    }

    // 对端有序关闭写方向：本端应收到 FIN，而不是 RST。
    CHECK_TRUE(tsock::ShutdownWrite(peer) == 0);
    const int revents = WaitEvents(client.native_handle(), 1000);
    std::printf("  poll revents=0x%x\n", static_cast<unsigned>(revents));
    CHECK_TRUE(revents > 0);

    std::uint8_t buf[64];
    std::size_t got = 0;
    const rd::net::IoStatus status =
        client.Recv(rd::net::MutableByteView{buf, sizeof(buf)}, &got);
    std::printf("  Recv -> %s, received=%zu, errno=%d\n",
                rd::net::IoStatusName(status), got, client.last_system_error());
    CHECK_STATUS(status, rd::net::IoStatus::kClosed);
    CHECK_TRUE(got == 0);
    tsock::Close(peer);
}

// ---------------------------------------------------------------- 场景 2：RST
void Scenario2Reset() {
    std::printf("\n== 场景 2: TCP 对端 SO_LINGER{1,0}+close -> kReset ==\n");
    std::uint16_t port = 0;
    const tsock::Handle listen_fd = MakeListener(&port);
    CHECK_TRUE(tsock::Valid(listen_fd));
    if (!tsock::Valid(listen_fd)) {
        return;
    }
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port});
    CHECK_TRUE(opened.ok());
    if (!opened.ok()) {
        tsock::Close(listen_fd);
        return;
    }
    rd::net::StreamSocket client = std::move(*opened);
    const tsock::Handle peer = AcceptOne(listen_fd);
    tsock::Close(listen_fd);
    CHECK_TRUE(tsock::Valid(peer));
    if (!tsock::Valid(peer)) {
        return;
    }

    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;  // 关闭时发 RST，而非 FIN
    CHECK_TRUE(tsock::SetOpt(peer, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
    tsock::Close(peer);

    std::uint8_t buf[64];
    std::size_t got = 0;
    rd::net::IoStatus status = rd::net::IoStatus::kWouldBlock;
    int revents = 0;
    for (int i = 0; i < 50; ++i) {
        revents = WaitEvents(client.native_handle(), 100);
        if (revents == 0) {
            continue;
        }
        got = 0;
        status = client.Recv(rd::net::MutableByteView{buf, sizeof(buf)}, &got);
        if (status != rd::net::IoStatus::kWouldBlock) {
            break;
        }
    }
    std::printf("  poll revents=0x%x\n", static_cast<unsigned>(revents));
    std::printf("  Recv -> %s, received=%zu, errno=%d\n",
                rd::net::IoStatusName(status), got, client.last_system_error());
    CHECK_STATUS(status, rd::net::IoStatus::kReset);
    CHECK_TRUE(status != rd::net::IoStatus::kClosed);
    CHECK_TRUE(client.last_system_error() == tsock::kConnReset);
}

// ---------------------------------------------------------------- 场景 3：UDP 截断
void Scenario3Truncated() {
    std::printf("\n== 场景 3: UDP 70000 字节 -> 1024 字节缓冲 -> kTruncated ==\n");
    auto recv_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto send_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(recv_opened.ok());
    CHECK_TRUE(send_opened.ok());
    if (!recv_opened.ok() || !send_opened.ok()) {
        return;
    }
    rd::net::DatagramSocket receiver = std::move(*recv_opened);
    rd::net::DatagramSocket sender = std::move(*send_opened);
    std::printf("  receiver=%s:%u sender=%s:%u\n",
                receiver.local_endpoint().host.c_str(), receiver.local_endpoint().port,
                sender.local_endpoint().host.c_str(), sender.local_endpoint().port);

    // IPv4 单个 UDP 数据报最大载荷 65507 字节；70000 字节根本发不出去。
    std::vector<std::uint8_t> oversized(70000, 0x5A);
    rd::net::SendResult sr{};
    rd::net::IoStatus status = sender.SendTo(
        rd::net::ByteView{oversized.data(), oversized.size()},
        receiver.local_endpoint(), &sr);
    std::printf("  SendTo(70000) -> %s, errno=%d (%s)\n",
                rd::net::IoStatusName(status), sender.last_system_error(),
                std::strerror(sender.last_system_error()));
    CHECK_STATUS(status, rd::net::IoStatus::kIoError);
    CHECK_TRUE(sender.last_system_error() == tsock::kMsgSize);

    // 用合法上限 65507 触发接收侧截断：缓冲 1024 < 报文 65507。
    std::vector<std::uint8_t> max_payload(65507);
    FillPattern(max_payload.data(), max_payload.size(), 0);
    status = sender.SendTo(
        rd::net::ByteView{max_payload.data(), max_payload.size()},
        receiver.local_endpoint(), &sr);
    std::printf("  SendTo(65507) -> %s, token=%llu\n",
                rd::net::IoStatusName(status),
                static_cast<unsigned long long>(sr.token));
    CHECK_STATUS(status, rd::net::IoStatus::kOk);

    std::uint8_t small[1024];
    std::size_t got = 0;
    rd::net::Endpoint from{};
    CHECK_TRUE(WaitEvents(receiver.native_handle(), 1000) > 0);
    status = receiver.RecvFrom(rd::net::MutableByteView{small, sizeof(small)}, &from,
                               &got);
    std::printf("  RecvFrom(1024) -> %s, actual=%zu (buffer=1024), from=%s:%u\n",
                rd::net::IoStatusName(status), got, from.host.c_str(), from.port);
    CHECK_STATUS(status, rd::net::IoStatus::kTruncated);
    CHECK_TRUE(status != rd::net::IoStatus::kOk);
#if defined(_WIN32)
    // 平台差异（REQ-010）：POSIX 的 recvmsg(MSG_TRUNC) 返回值是**真实报文长度**
    // 65507，所以 Linux 断言 got == 65507。Windows 的 WSARecvFrom 只报
    // WSAEMSGSIZE，内核不提供真实长度，实现只能把 received 记为缓冲大小
    // （见 src/rd_net/socket.cpp 的 RecvDatagramRaw 注释）。等价断言：截断时
    // received 必须恰好等于调用方缓冲长度——既不能谎报 0（丢失「有数据」信号），
    // 也不能谎报超过缓冲的字节数（会被误当成完整报文读）。
    CHECK_TRUE(got == sizeof(small));
#else
    CHECK_TRUE(got == 65507);
#endif
    CHECK_TRUE(small[0] == max_payload[0]);
    CHECK_TRUE(small[1023] == max_payload[1023]);

    // 截断后 socket 仍可正常收发小包。
    std::vector<std::uint8_t> tiny(100);
    FillPattern(tiny.data(), tiny.size(), 999);
    status = sender.SendTo(rd::net::ByteView{tiny.data(), tiny.size()},
                           receiver.local_endpoint(), &sr);
    CHECK_STATUS(status, rd::net::IoStatus::kOk);
    CHECK_TRUE(WaitEvents(receiver.native_handle(), 1000) > 0);
    got = 0;
    status = receiver.RecvFrom(rd::net::MutableByteView{small, sizeof(small)}, &from,
                               &got);
    std::printf("  follow-up RecvFrom -> %s, actual=%zu\n",
                rd::net::IoStatusName(status), got);
    CHECK_STATUS(status, rd::net::IoStatus::kOk);
    CHECK_TRUE(got == 100);
}

// ---------------------------------------------------------------- 场景 4：满缓冲
void Scenario4WouldBlock() {
    std::printf("\n== 场景 4: TCP 非阻塞发送塞满对端缓冲 -> kWouldBlock，重试不丢数据 ==\n");
    std::uint16_t port = 0;
    const tsock::Handle listen_fd = MakeListener(&port);
    CHECK_TRUE(tsock::Valid(listen_fd));
    if (!tsock::Valid(listen_fd)) {
        return;
    }
    int listen_rcvbuf = 8192;
    tsock::SetOpt(listen_fd, SOL_SOCKET, SO_RCVBUF, &listen_rcvbuf,
                 sizeof(listen_rcvbuf));
    rd::net::DialOptions dopts;
    dopts.send_buffer_bytes = 16384;
    dopts.connect_timeout = std::chrono::milliseconds(2000);
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port},
                                              dopts);
    CHECK_TRUE(opened.ok());
    if (!opened.ok()) {
        tsock::Close(listen_fd);
        return;
    }
    rd::net::StreamSocket client = std::move(*opened);
    const tsock::Handle peer = AcceptOne(listen_fd);
    tsock::Close(listen_fd);
    CHECK_TRUE(tsock::Valid(peer));
    if (!tsock::Valid(peer)) {
        return;
    }
    int peer_rcvbuf = 8192;
    tsock::SetOpt(peer, SOL_SOCKET, SO_RCVBUF, &peer_rcvbuf, sizeof(peer_rcvbuf));

    constexpr std::size_t kChunk = 16384;
    constexpr std::size_t kTotal = 512 * 1024;
    std::vector<std::uint8_t> chunk(kChunk);
    std::vector<std::uint8_t> rbuf(65536);
    std::size_t sent = 0;
    std::size_t received = 0;
    bool saw_would_block = false;
    bool data_ok = true;
    std::uint64_t last_token = 0;
    bool token_monotonic = true;

    auto drain_peer = [&]() {
        for (;;) {
            const std::ptrdiff_t n = tsock::RecvNonBlocking(peer, rbuf.data(), rbuf.size());
            if (n > 0) {
                std::size_t bad = 0;
                if (!VerifyPattern(rbuf.data(), static_cast<std::size_t>(n), received,
                                   &bad)) {
                    data_ok = false;
                    std::printf("  pattern mismatch at offset %zu\n", bad);
                }
                received += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && tsock::IsInterrupted(tsock::LastError())) {
                continue;
            }
            break;
        }
    };

    // 阶段 1：对端不读，连续非阻塞发送，直到本端发送缓冲满。
    while (sent < kTotal) {
        const std::size_t n = (std::min)(kChunk, kTotal - sent);
        FillPattern(chunk.data(), n, sent);
        rd::net::SendResult sr{};
        const rd::net::IoStatus status =
            client.Send(rd::net::ByteView{chunk.data(), n}, &sr);
        if (sr.token <= last_token) {
            token_monotonic = false;
        }
        last_token = sr.token;
        if (status == rd::net::IoStatus::kOk) {
            CHECK_TRUE(client.last_transferred() > 0);
            sent += client.last_transferred();
        } else if (status == rd::net::IoStatus::kWouldBlock) {
            saw_would_block = true;
            break;
        } else {
            std::printf("  unexpected Send status=%s errno=%d\n",
                        rd::net::IoStatusName(status), client.last_system_error());
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
    }
    std::printf("  phase1: sent=%zu, would_block=%s\n", sent,
                saw_would_block ? "yes" : "no");
    CHECK_TRUE(saw_would_block);

    // 阶段 2：一边读对端，一边把剩余数据发完。
    drain_peer();
    while (sent < kTotal) {
        const std::size_t n = (std::min)(kChunk, kTotal - sent);
        FillPattern(chunk.data(), n, sent);
        rd::net::SendResult sr{};
        const rd::net::IoStatus status =
            client.Send(rd::net::ByteView{chunk.data(), n}, &sr);
        if (status == rd::net::IoStatus::kOk) {
            sent += client.last_transferred();
        } else if (status == rd::net::IoStatus::kWouldBlock) {
            saw_would_block = true;
        } else {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        drain_peer();
    }

    // 阶段 3：把对端缓冲区里剩余的数据读干净。
    while (received < kTotal) {
        const int revents = WaitEvents(peer, 2000);
        if (revents <= 0) {
            break;
        }
        const std::size_t before = received;
        drain_peer();
        if (received == before) {
            break;
        }
    }
    std::printf("  sent=%zu received=%zu would_block_seen=%s data_ok=%s token_monotonic=%s\n",
                sent, received, saw_would_block ? "yes" : "no", data_ok ? "yes" : "no",
                token_monotonic ? "yes" : "no");
    CHECK_TRUE(sent == kTotal);
    CHECK_TRUE(received == kTotal);
    CHECK_TRUE(data_ok);
    CHECK_TRUE(token_monotonic);
    CHECK_TRUE(last_token > 0);
    tsock::Close(peer);
}

// ---------------------------------------------------------------- 场景 5a：1e4 echo
void Scenario5aEchoNoLeak() {
    std::printf("\n== 场景 5a: 1e4 次 UDP echo，RSS / fd 前后对比 ==\n");
    auto a_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto b_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(a_opened.ok());
    CHECK_TRUE(b_opened.ok());
    if (!a_opened.ok() || !b_opened.ok()) {
        return;
    }
    rd::net::DatagramSocket a = std::move(*a_opened);
    rd::net::DatagramSocket b = std::move(*b_opened);

    constexpr int kEcho = 10000;
    std::uint8_t payload[128];
    FillPattern(payload, sizeof(payload), 0);
    std::uint8_t rbuf[256];
    const long rss_before = ReadVmRssKb();
    const int fds_before = CountOpenFds();
    int ok_count = 0;
    for (int i = 0; i < kEcho; ++i) {
        rd::net::SendResult sr{};
        rd::net::IoStatus status = a.SendTo(rd::net::ByteView{payload, sizeof(payload)},
                                            b.local_endpoint(), &sr);
        if (status != rd::net::IoStatus::kOk) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        std::size_t got = 0;
        do {
            status = b.RecvFrom(rd::net::MutableByteView{rbuf, sizeof(rbuf)}, nullptr,
                                &got);
        } while (status == rd::net::IoStatus::kWouldBlock);
        if (status != rd::net::IoStatus::kOk || got != sizeof(payload)) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        status = b.SendTo(rd::net::ByteView{rbuf, got}, a.local_endpoint(), &sr);
        if (status != rd::net::IoStatus::kOk) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        got = 0;
        do {
            status = a.RecvFrom(rd::net::MutableByteView{rbuf, sizeof(rbuf)}, nullptr,
                                &got);
        } while (status == rd::net::IoStatus::kWouldBlock);
        if (status != rd::net::IoStatus::kOk || got != sizeof(payload)) {
            CHECK_TRUE(status == rd::net::IoStatus::kOk);
            break;
        }
        ++ok_count;
    }
    const long rss_after = ReadVmRssKb();
    const int fds_after = CountOpenFds();
    std::printf("  echo ok=%d/%d, VmRSS %ld kB -> %ld kB (delta %ld kB), fds %d -> %d\n",
                ok_count, kEcho, rss_before, rss_after, rss_after - rss_before,
                fds_before, fds_after);
    CHECK_TRUE(ok_count == kEcho);
    CHECK_TRUE(fds_after == fds_before);
    CHECK_TRUE((rss_after - rss_before) < 2048);
}

// ---------------------------------------------------------------- 场景 5b：100 socket
void Scenario5bHundredSockets() {
    std::printf("\n== 场景 5b: 100 个 socket 同时非阻塞收发，随机 payload 不串包 ==\n");
    constexpr int kCount = 100;
    std::vector<rd::net::DatagramSocket> socks;
    socks.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        auto opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
        if (!opened.ok()) {
            CHECK_TRUE(opened.ok());
            return;
        }
        socks.push_back(std::move(*opened));
    }
    // 每个目标期望收到的 (发送者, 完整 payload)。
    struct Expected {
        int from;
        std::vector<std::uint8_t> payload;
        bool seen{false};
    };
    std::vector<std::vector<Expected>> expect(kCount);
    std::mt19937_64 rng(20261103u);
    std::vector<std::vector<std::uint8_t>> payloads(kCount);
    for (int i = 0; i < kCount; ++i) {
        const int target = (i * 37 + 11) % kCount;
        std::vector<std::uint8_t> payload(64);
        const std::uint64_t r = rng();
        std::memcpy(payload.data(), &i, sizeof(i));
        std::memcpy(payload.data() + 4, &r, sizeof(r));
        for (std::size_t k = 12; k < payload.size(); ++k) {
            payload[k] = static_cast<std::uint8_t>((i + k) & 0xFF);
        }
        payloads[i] = payload;
        expect[static_cast<std::size_t>(target)].push_back(Expected{i, payload, false});
        rd::net::SendResult sr{};
        const rd::net::IoStatus status =
            socks[static_cast<std::size_t>(i)].SendTo(
                rd::net::ByteView{payload.data(), payload.size()},
                socks[static_cast<std::size_t>(target)].local_endpoint(), &sr);
        CHECK_STATUS(status, rd::net::IoStatus::kOk);
        CHECK_TRUE(sr.token == 1);  // 每个 socket 的首个 token 从 1 开始
    }

    int total_received = 0;
    int cross_talk = 0;
    std::vector<std::uint8_t> rbuf(256);
    for (int round = 0; round < 200 && total_received < kCount; ++round) {
        bool progressed = false;
        for (int i = 0; i < kCount; ++i) {
            for (;;) {
                std::size_t got = 0;
                const rd::net::IoStatus status = socks[static_cast<std::size_t>(i)]
                                                     .RecvFrom(rd::net::MutableByteView{
                                                                   rbuf.data(), rbuf.size()},
                                                               nullptr, &got);
                if (status == rd::net::IoStatus::kWouldBlock) {
                    break;
                }
                CHECK_STATUS(status, rd::net::IoStatus::kOk);
                if (status != rd::net::IoStatus::kOk) {
                    break;
                }
                ++total_received;
                progressed = true;
                bool matched = false;
                for (auto& e : expect[static_cast<std::size_t>(i)]) {
                    if (!e.seen && e.payload.size() == got &&
                        std::memcmp(e.payload.data(), rbuf.data(), got) == 0) {
                        e.seen = true;
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    ++cross_talk;
                }
            }
        }
        if (!progressed) {
            tsock::SleepMs(1);
        }
    }
    int unseen = 0;
    for (const auto& list : expect) {
        for (const auto& e : list) {
            if (!e.seen) {
                ++unseen;
            }
        }
    }
    std::printf("  sockets=%d received=%d/%d cross_talk=%d unseen=%d\n", kCount,
                total_received, kCount, cross_talk, unseen);
    CHECK_TRUE(total_received == kCount);
    CHECK_TRUE(cross_talk == 0);
    CHECK_TRUE(unseen == 0);
    CHECK_TRUE(rbuf.size() > 64);
    (void)payloads;
}

// ---------------------------------------------------------------- 压测
void Benchmark() {
    std::printf("\n== 压测: 10 万次 SendTo+RecvFrom ==\n");
    auto a_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto b_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(a_opened.ok());
    CHECK_TRUE(b_opened.ok());
    if (!a_opened.ok() || !b_opened.ok()) {
        return;
    }
    rd::net::DatagramSocket a = std::move(*a_opened);
    rd::net::DatagramSocket b = std::move(*b_opened);
    constexpr int kOps = 100000;
    std::uint8_t payload[128];
    FillPattern(payload, sizeof(payload), 0);
    std::uint8_t rbuf[256];
    const long rss_before = ReadVmRssKb();
    const int fds_before = CountOpenFds();
    int completed = 0;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOps; ++i) {
        rd::net::SendResult sr{};
        rd::net::IoStatus status = a.SendTo(rd::net::ByteView{payload, sizeof(payload)},
                                            b.local_endpoint(), &sr);
        if (status != rd::net::IoStatus::kOk) {
            break;
        }
        std::size_t got = 0;
        do {
            status = b.RecvFrom(rd::net::MutableByteView{rbuf, sizeof(rbuf)}, nullptr,
                                &got);
        } while (status == rd::net::IoStatus::kWouldBlock);
        if (status != rd::net::IoStatus::kOk) {
            break;
        }
        ++completed;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long rss_after = ReadVmRssKb();
    const int fds_after = CountOpenFds();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const double ns_per_op =
        completed > 0 ? static_cast<double>(elapsed_ns) / static_cast<double>(completed)
                      : 0.0;
    std::printf("  ops=%d/%d elapsed=%.3f ms ns/op=%.1f VmRSS %ld kB -> %ld kB "
                "(delta %ld kB) fds %d -> %d\n",
                completed, kOps, static_cast<double>(elapsed_ns) / 1.0e6, ns_per_op,
                rss_before, rss_after, rss_after - rss_before, fds_before, fds_after);
    CHECK_TRUE(completed == kOps);
    CHECK_TRUE(fds_after == fds_before);
}


// ---------------------------------------------------------------- 对比：默认 vs 批量
// 说明：本段**不**增加 g_checks/g_failures，只打印实测数据，保证原有
// 353 条断言口径不变；批量路径若失败会打印 [FAIL-BATCH] 以便人工判读。
struct UdpBenchSample {
    double ns_per_op{0};
    double mib_per_s{0};
    int ok{0};
};

UdpBenchSample RunUdpEchoBench(const rd::net::SocketOptions& options, int iters,
                               std::size_t payload_size) {
    UdpBenchSample sample;
    auto a_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    auto b_opened =
        rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, options);
    if (!a_opened.ok() || !b_opened.ok()) {
        return sample;
    }
    rd::net::DatagramSocket a = std::move(*a_opened);
    rd::net::DatagramSocket b = std::move(*b_opened);
    std::vector<std::uint8_t> payload(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFFu);
    }
    std::vector<std::uint8_t> rbuf(payload_size + 64);
    const auto start = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 0; i < iters; ++i) {
        rd::net::SendResult sr{};
        auto st = a.SendTo(rd::net::ByteView{payload.data(), payload.size()},
                           b.local_endpoint(), &sr);
        if (st != rd::net::IoStatus::kOk) {
            break;
        }
        std::size_t got = 0;
        do {
            st = b.RecvFrom(rd::net::MutableByteView{rbuf.data(), rbuf.size()}, nullptr,
                            &got);
        } while (st == rd::net::IoStatus::kWouldBlock);
        if (st != rd::net::IoStatus::kOk || got != payload.size()) {
            break;
        }
        st = b.SendTo(rd::net::ByteView{rbuf.data(), got}, a.local_endpoint(), &sr);
        if (st != rd::net::IoStatus::kOk) {
            break;
        }
        got = 0;
        do {
            st = a.RecvFrom(rd::net::MutableByteView{rbuf.data(), rbuf.size()}, nullptr,
                            &got);
        } while (st == rd::net::IoStatus::kWouldBlock);
        if (st != rd::net::IoStatus::kOk || got != payload.size()) {
            break;
        }
        ++ok;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    sample.ok = ok;
    if (ok > 0 && seconds > 0) {
        sample.ns_per_op = seconds * 1e9 / ok;
        // 单向有效载荷：每次 echo 有一份 payload 从 a 到 b。
        sample.mib_per_s =
            (static_cast<double>(ok) * static_cast<double>(payload_size)) / seconds /
            (1024.0 * 1024.0);
    }
    return sample;
}

void BatchComparison() {
    std::printf("\n== 对比: UDP echo 默认(batch=0) vs 批量(sendmmsg) ==\n");
    rd::net::SocketOptions def{};
    rd::net::SocketOptions batch{};
    batch.batch_bytes = 4096;
    batch.batch_entries = 8;
    batch.enable_sendmmsg = true;

    constexpr int kIters = 5000;
    constexpr std::size_t kPayload = 8000;  // >= batch_bytes，确保走批量分支
    double def_ns[3] = {0, 0, 0};
    double def_mib[3] = {0, 0, 0};
    double bat_ns[3] = {0, 0, 0};
    double bat_mib[3] = {0, 0, 0};
    for (int round = 0; round < 3; ++round) {
        const UdpBenchSample d = RunUdpEchoBench(def, kIters, kPayload);
        const UdpBenchSample f = RunUdpEchoBench(batch, kIters, kPayload);
        def_ns[round] = d.ns_per_op;
        def_mib[round] = d.mib_per_s;
        bat_ns[round] = f.ns_per_op;
        bat_mib[round] = f.mib_per_s;
        std::printf(
            "  round %d: default ns/op=%.1f %.1f MiB/s (ok=%d/%d) | "
            "batch ns/op=%.1f %.1f MiB/s (ok=%d/%d)\n",
            round + 1, d.ns_per_op, d.mib_per_s, d.ok, kIters, f.ns_per_op,
            f.mib_per_s, f.ok, kIters);
        if (d.ok != kIters || f.ok != kIters) {
            std::printf("  [FAIL-BATCH] round %d 未跑满：default=%d batch=%d (期望 %d)\n",
                        round + 1, d.ok, f.ok, kIters);
        }
    }
    const auto avg = [](const double (&v)[3]) { return (v[0] + v[1] + v[2]) / 3.0; };
    std::printf("  avg: default ns/op=%.1f %.1f MiB/s | batch ns/op=%.1f %.1f MiB/s\n",
                avg(def_ns), avg(def_mib), avg(bat_ns), avg(bat_mib));

    // 批量配置下 kTruncated 语义不变：65507 字节报文 -> 1024 字节缓冲。
    auto rx = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, batch);
    auto tx = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, batch);
    if (rx.ok() && tx.ok()) {
        std::vector<std::uint8_t> big(65507);
        FillPattern(big.data(), big.size(), 7);
        std::uint8_t small[1024];
        rd::net::SendResult sr{};
        const auto send_st =
            tx->SendTo(rd::net::ByteView{big.data(), big.size()}, rx->local_endpoint(), &sr);
        std::size_t got = 0;
        const auto recv_st = rx->RecvFrom(rd::net::MutableByteView{small, sizeof(small)},
                                          nullptr, &got);
        std::printf("  batch-trunc: SendTo=%s RecvFrom=%s actual=%zu (期望 kOk/kTruncated/65507)\n",
                    rd::net::IoStatusName(send_st), rd::net::IoStatusName(recv_st), got);
        if (send_st != rd::net::IoStatus::kOk ||
            recv_st != rd::net::IoStatus::kTruncated || got != big.size()) {
            std::printf("  [FAIL-BATCH] 批量配置破坏了 kTruncated 语义\n");
        }
    } else {
        std::printf("  [FAIL-BATCH] 批量配置下 Open 失败\n");
    }

    // TCP 批量（writev）验证：128 KiB 单次 Send 按 batch_bytes=64 KiB 切成
    // 2 段，一次 writev 提交；对端逐块读回并校验内容。
    {
        const tsock::Handle lfd = tsock::TcpSocket();
        if (tsock::Valid(lfd)) {
            int one = 1;
            tsock::SetOpt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in la{};
            la.sin_family = AF_INET;
            la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            la.sin_port = 0;
            tsock::Bind(lfd, la);
            tsock::Listen(lfd, 4);
            tsock::GetSockName(lfd, la);
            rd::net::DialOptions dopts;
            dopts.batch_bytes = 65536;
            dopts.batch_entries = 8;
            dopts.enable_writev = true;
            auto cli = rd::net::StreamSocket::Open(
                rd::net::Endpoint{"127.0.0.1", ntohs(la.sin_port)}, dopts);
            if (cli.ok()) {
                const tsock::Handle peer = tsock::Accept(lfd);
                std::vector<std::uint8_t> block(128 * 1024);
                FillPattern(block.data(), block.size(), 3);
                rd::net::SendResult sr{};
                const auto st =
                    cli->Send(rd::net::ByteView{block.data(), block.size()}, &sr);
                const std::size_t sent = cli->last_transferred();
                std::vector<std::uint8_t> got(block.size());
                std::size_t off = 0;
                while (off < block.size()) {
                    const std::ptrdiff_t n = tsock::ReadSome(peer, got.data() + off, got.size() - off);
                    if (n <= 0) {
                        break;
                    }
                    off += static_cast<std::size_t>(n);
                }
                const bool same = off == block.size() &&
                                  std::memcmp(got.data(), block.data(), block.size()) == 0;
                std::printf("  tcp-batch: Send=%s transferred=%zu received=%zu match=%s\n",
                            rd::net::IoStatusName(st), sent, off, same ? "yes" : "no");
                if (st != rd::net::IoStatus::kOk || sent != block.size() ||
                    off != block.size() || !same) {
                    std::printf("  [FAIL-BATCH] TCP writev 批量路径异常\n");
                }
                tsock::Close(peer);
            } else {
                std::printf("  [FAIL-BATCH] TCP Open 失败 status=%s errno=%d\n",
                            rd::net::IoStatusName(cli.status()), cli.system_error());
            }
            tsock::Close(lfd);
        }
    }
}


// ---------------------------------------------------------------- 场景 6：UDP SendBatch
/// @brief 等待并逐条校验 count 条数据报；返回实际匹配条数。
int RecvUdpBatchExact(rd::net::DatagramSocket& rx,
                      const std::vector<std::vector<std::uint8_t>>& payloads,
                      std::size_t first, std::size_t count, std::size_t seg) {
    std::vector<std::uint8_t> buf(seg + 64);
    int matched = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (WaitEvents(rx.native_handle(), 2000) <= 0) {
            std::printf("  [recv] poll timeout at %zu/%zu\n", i, count);
            break;
        }
        std::size_t got = 0;
        rd::net::Endpoint from;
        const auto st = rx.RecvFrom(
            rd::net::MutableByteView{buf.data(), buf.size()}, &from, &got);
        if (st != rd::net::IoStatus::kOk || got != seg) {
            std::printf("  [recv] i=%zu status=%s got=%zu\n", i,
                        rd::net::IoStatusName(st), got);
            break;
        }
        if (std::memcmp(buf.data(), payloads[first + i].data(), seg) != 0) {
            std::printf("  [recv] i=%zu content mismatch\n", i);
            break;
        }
        ++matched;
    }
    return matched;
}

struct UdpBatchBench {
    double ns_per_datagram{0};
    double mib_per_s{0};
    int rounds{0};
};

/// @brief 每轮：发送 entries 条（只对发送计时）→ 逐条收干净并校验。
UdpBatchBench RunUdpBatchBench(bool batched, int rounds, std::size_t entries,
                               std::size_t seg) {
    UdpBatchBench sample;
    auto rx_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto tx_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    if (!rx_opened.ok() || !tx_opened.ok()) {
        return sample;
    }
    rd::net::DatagramSocket rx = std::move(*rx_opened);
    rd::net::DatagramSocket tx = std::move(*tx_opened);
    if (tx.SetBatchTarget(rx.local_endpoint()) != rd::net::IoStatus::kOk) {
        return sample;
    }
    std::vector<std::vector<std::uint8_t>> payloads(entries,
                                                    std::vector<std::uint8_t>(seg));
    std::vector<go2cpp::bufio::BatchEntry> batch(entries);
    for (std::size_t i = 0; i < entries; ++i) {
        FillPattern(payloads[i].data(), seg, i * 13);
        batch[i].data = reinterpret_cast<const char*>(payloads[i].data());
        batch[i].size = seg;
    }
    long long send_ns = 0;
    int ok = 0;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        bool sent = true;
        if (batched) {
            rd::net::SendResult sr{};
            if (tx.SendBatch(batch.data(), batch.size(), &sr) != rd::net::IoStatus::kOk ||
                tx.last_batch_submitted() != entries) {
                sent = false;
            }
        } else {
            for (std::size_t i = 0; i < entries; ++i) {
                rd::net::SendResult sr{};
                const rd::net::ByteView view{
                    reinterpret_cast<const std::uint8_t*>(batch[i].data), batch[i].size};
                if (tx.SendTo(view, rx.local_endpoint(), &sr) != rd::net::IoStatus::kOk) {
                    sent = false;
                    break;
                }
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (!sent) {
            break;
        }
        send_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (RecvUdpBatchExact(rx, payloads, 0, entries, seg) !=
            static_cast<int>(entries)) {
            break;
        }
        ++ok;
    }
    sample.rounds = ok;
    if (ok > 0 && send_ns > 0) {
        const double bytes = static_cast<double>(ok) * static_cast<double>(entries) *
                             static_cast<double>(seg);
        sample.ns_per_datagram =
            static_cast<double>(send_ns) / (static_cast<double>(ok) * entries);
        sample.mib_per_s = bytes / (static_cast<double>(send_ns) / 1.0e9) /
                           (1024.0 * 1024.0);
    }
    return sample;
}

void Scenario6UdpSendBatch() {
    std::printf("\n== 场景 6: UDP SendBatch 一次 sendmmsg 提交 64 条 ==\n");
    constexpr std::size_t kEntries = 64;
    constexpr std::size_t kSeg = 1024;

    auto rx_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    auto tx_opened = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
    CHECK_TRUE(rx_opened.ok());
    CHECK_TRUE(tx_opened.ok());
    if (!rx_opened.ok() || !tx_opened.ok()) {
        return;
    }
    rd::net::DatagramSocket rx = std::move(*rx_opened);
    rd::net::DatagramSocket tx = std::move(*tx_opened);

    std::vector<std::vector<std::uint8_t>> payloads(kEntries,
                                                    std::vector<std::uint8_t>(kSeg));
    std::vector<go2cpp::bufio::BatchEntry> entries(kEntries);
    for (std::size_t i = 0; i < kEntries; ++i) {
        FillPattern(payloads[i].data(), kSeg, i * 37);
        entries[i].data = reinterpret_cast<const char*>(payloads[i].data());
        entries[i].size = kSeg;
    }

    // 未设置目标：SendBatch 必须返回 kInvalid。
    {
        auto alone_opened =
            rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0});
        CHECK_TRUE(alone_opened.ok());
        if (alone_opened.ok()) {
            rd::net::DatagramSocket alone = std::move(*alone_opened);
            rd::net::SendResult sr{};
            CHECK_STATUS(alone.SendBatch(entries.data(), 1, &sr),
                         rd::net::IoStatus::kInvalid);
        }
    }

    CHECK_STATUS(tx.SetBatchTarget(rx.local_endpoint()), rd::net::IoStatus::kOk);

    // count=0：kOk no-op，对端收不到数据。
    {
        rd::net::SendResult sr{};
        CHECK_STATUS(tx.SendBatch(entries.data(), 0, &sr), rd::net::IoStatus::kOk);
        CHECK_TRUE(tx.last_batch_submitted() == 0);
        std::uint8_t probe[16];
        std::size_t got = 0;
        CHECK_STATUS(rx.RecvFrom(rd::net::MutableByteView{probe, sizeof(probe)},
                                 nullptr, &got),
                     rd::net::IoStatus::kWouldBlock);
    }

    // count=1。
    {
        rd::net::SendResult sr{};
        CHECK_STATUS(tx.SendBatch(entries.data(), 1, &sr), rd::net::IoStatus::kOk);
        CHECK_TRUE(tx.last_batch_submitted() == 1);
        CHECK_TRUE(RecvUdpBatchExact(rx, payloads, 0, 1, kSeg) == 1);
    }

    // 一次 64 条 1KB：对端逐条收全、顺序一致。
    {
        rd::net::SendResult sr{};
        CHECK_STATUS(tx.SendBatch(entries.data(), entries.size(), &sr),
                     rd::net::IoStatus::kOk);
        CHECK_TRUE(tx.last_batch_submitted() == entries.size());
        const int matched = RecvUdpBatchExact(rx, payloads, 0, kEntries, kSeg);
        std::printf("  udp-batch: status=%s submitted=%zu received=%d/%zu\n",
                    rd::net::IoStatusName(sr.status), tx.last_batch_submitted(),
                    matched, kEntries);
        CHECK_TRUE(matched == static_cast<int>(kEntries));
    }

    // 超过上限：1500 段 × 32B -> 只提交前 1024 段，剩余 476 段下一次提交。
    {
        constexpr std::size_t kSmallSeg = 32;
        constexpr std::size_t kBig = 1500;
        std::vector<std::vector<std::uint8_t>> small(
            kBig, std::vector<std::uint8_t>(kSmallSeg));
        std::vector<go2cpp::bufio::BatchEntry> big_entries(kBig);
        for (std::size_t i = 0; i < kBig; ++i) {
            FillPattern(small[i].data(), kSmallSeg, i * 7);
            big_entries[i].data = reinterpret_cast<const char*>(small[i].data());
            big_entries[i].size = kSmallSeg;
        }
        rd::net::SendResult sr{};
        CHECK_STATUS(tx.SendBatch(big_entries.data(), kBig, &sr),
                     rd::net::IoStatus::kOk);
        CHECK_TRUE(tx.last_batch_submitted() == rd::net::kMaxBatchEntries);
        // 接收端 rcvbuf 约 208KB，1024 条小报文会在内核接收队列溢出（UDP 语义），
        // 这里只验证提交计数；顺序与内容由前面 64 条 × 1KB 用例覆盖。
        const std::size_t rest = kBig - rd::net::kMaxBatchEntries;
        CHECK_STATUS(tx.SendBatch(big_entries.data() + rd::net::kMaxBatchEntries,
                                  rest, &sr),
                     rd::net::IoStatus::kOk);
        CHECK_TRUE(tx.last_batch_submitted() == rest);
        std::printf("  udp-limit: count=1500 committed=%zu + %zu (cap=%zu)\n",
                    rd::net::kMaxBatchEntries, rest, rd::net::kMaxBatchEntries);
    }

    // UDP 部分提交尝试：小 rcvbuf/sndbuf + 对端不读（loopback 可能直接丢包，
    // 不返回 EAGAIN；如实打印结果，不作硬断言）。
    {
        rd::net::SocketOptions ro;
        ro.recv_buffer_bytes = 2048;
        rd::net::SocketOptions so;
        so.send_buffer_bytes = 4096;
        auto r2o = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, ro);
        auto t2o = rd::net::DatagramSocket::Open(rd::net::Endpoint{"127.0.0.1", 0}, so);
        if (r2o.ok() && t2o.ok()) {
            constexpr std::size_t kPartial = 200;
            constexpr std::size_t kPartialSeg = 512;
            std::vector<std::vector<std::uint8_t>> pp(
                kPartial, std::vector<std::uint8_t>(kPartialSeg));
            std::vector<go2cpp::bufio::BatchEntry> pe(kPartial);
            for (std::size_t i = 0; i < kPartial; ++i) {
                FillPattern(pp[i].data(), kPartialSeg, i * 3);
                pe[i].data = reinterpret_cast<const char*>(pp[i].data());
                pe[i].size = kPartialSeg;
            }
            CHECK_STATUS(t2o->SetBatchTarget(r2o->local_endpoint()),
                         rd::net::IoStatus::kOk);
            rd::net::SendResult sr{};
            const auto st = t2o->SendBatch(pe.data(), pe.size(), &sr);
            std::printf("  udp-partial-attempt: status=%s submitted=%zu/%zu "
                        "(rcvbuf=2048 sndbuf=4096, 对端不读)\n",
                        rd::net::IoStatusName(st), t2o->last_batch_submitted(),
                        pe.size());
            if (st == rd::net::IoStatus::kWouldBlock) {
                CHECK_TRUE(t2o->last_batch_submitted() > 0);
                CHECK_TRUE(t2o->last_batch_submitted() < pe.size());
            }
        }
    }

    // 3 轮对比：批量 64 条 vs 64 次单条 SendTo（只计发送阶段）。
    constexpr int kRounds = 100;
    double batched_ns[3] = {0, 0, 0};
    double batched_mib[3] = {0, 0, 0};
    double single_ns[3] = {0, 0, 0};
    double single_mib[3] = {0, 0, 0};
    for (int round = 0; round < 3; ++round) {
        const UdpBatchBench b = RunUdpBatchBench(true, kRounds, kEntries, kSeg);
        const UdpBatchBench s = RunUdpBatchBench(false, kRounds, kEntries, kSeg);
        batched_ns[round] = b.ns_per_datagram;
        batched_mib[round] = b.mib_per_s;
        single_ns[round] = s.ns_per_datagram;
        single_mib[round] = s.mib_per_s;
        std::printf(
            "  round %d: batch ns/dgram=%.1f %.1f MiB/s (rounds=%d) | "
            "single ns/dgram=%.1f %.1f MiB/s (rounds=%d)\n",
            round + 1, b.ns_per_datagram, b.mib_per_s, b.rounds, s.ns_per_datagram,
            s.mib_per_s, s.rounds);
        if (b.rounds != kRounds || s.rounds != kRounds) {
            std::printf("  [FAIL-BATCH] UDP bench 未跑满：batch=%d single=%d\n",
                        b.rounds, s.rounds);
        }
    }
    const auto avg = [](const double (&v)[3]) { return (v[0] + v[1] + v[2]) / 3.0; };
    std::printf("  avg: batch ns/dgram=%.1f %.1f MiB/s | single ns/dgram=%.1f "
                "%.1f MiB/s | batch_faster=%s\n",
                avg(batched_ns), avg(batched_mib), avg(single_ns), avg(single_mib),
                avg(batched_ns) < avg(single_ns) ? "yes" : "no");
}

// ---------------------------------------------------------------- 场景 7：TCP SendBatch
std::size_t ReadFullBytes(tsock::Handle fd, std::uint8_t* dst, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        const std::ptrdiff_t got = tsock::ReadSome(fd, dst + off, n - off);
        if (got > 0) {
            off += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && tsock::IsInterrupted(tsock::LastError())) {
            continue;
        }
        break;
    }
    return off;
}

struct TcpBatchBench {
    double ns_per_segment{0};
    double mib_per_s{0};
    int rounds{0};
};

TcpBatchBench RunTcpBatchBench(bool batched, int rounds, std::size_t entries,
                               std::size_t seg) {
    TcpBatchBench sample;
    std::uint16_t port = 0;
    const tsock::Handle lfd = MakeListener(&port);
    if (!tsock::Valid(lfd)) {
        return sample;
    }
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port});
    if (!opened.ok()) {
        tsock::Close(lfd);
        return sample;
    }
    rd::net::StreamSocket cli = std::move(*opened);
    const tsock::Handle peer = tsock::Accept(lfd);
    tsock::Close(lfd);
    if (!tsock::Valid(peer)) {
        return sample;
    }
    std::vector<std::uint8_t> block(entries * seg);
    std::vector<go2cpp::bufio::BatchEntry> batch(entries);
    for (std::size_t i = 0; i < entries; ++i) {
        FillPattern(block.data() + i * seg, seg, i * 19);
        batch[i].data = reinterpret_cast<const char*>(block.data() + i * seg);
        batch[i].size = seg;
    }
    std::vector<std::uint8_t> got(entries * seg);
    long long send_ns = 0;
    int ok = 0;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        bool sent = true;
        if (batched) {
            rd::net::SendResult sr{};
            if (cli.SendBatch(batch.data(), batch.size(), &sr) !=
                    rd::net::IoStatus::kOk ||
                cli.last_batch_submitted() != entries) {
                sent = false;
            }
        } else {
            for (std::size_t i = 0; i < entries; ++i) {
                rd::net::SendResult sr{};
                const rd::net::ByteView view{
                    reinterpret_cast<const std::uint8_t*>(batch[i].data), batch[i].size};
                if (cli.Send(view, &sr) != rd::net::IoStatus::kOk ||
                    cli.last_transferred() != seg) {
                    sent = false;
                    break;
                }
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (!sent) {
            break;
        }
        send_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (ReadFullBytes(peer, got.data(), got.size()) != got.size() ||
            std::memcmp(got.data(), block.data(), block.size()) != 0) {
            break;
        }
        ++ok;
    }
    tsock::Close(peer);
    sample.rounds = ok;
    if (ok > 0 && send_ns > 0) {
        const double bytes = static_cast<double>(ok) * static_cast<double>(entries) *
                             static_cast<double>(seg);
        sample.ns_per_segment =
            static_cast<double>(send_ns) / (static_cast<double>(ok) * entries);
        sample.mib_per_s = bytes / (static_cast<double>(send_ns) / 1.0e9) /
                           (1024.0 * 1024.0);
    }
    return sample;
}

void Scenario7TcpSendBatch() {
    std::printf("\n== 场景 7: TCP SendBatch 一次 writev 提交 64 段 ==\n");
    constexpr std::size_t kEntries = 64;
    constexpr std::size_t kSeg = 1024;

    std::uint16_t port = 0;
    const tsock::Handle lfd = MakeListener(&port);
    CHECK_TRUE(tsock::Valid(lfd));
    if (!tsock::Valid(lfd)) {
        return;
    }
    auto opened = rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", port});
    CHECK_TRUE(opened.ok());
    if (!opened.ok()) {
        tsock::Close(lfd);
        return;
    }
    rd::net::StreamSocket cli = std::move(*opened);
    const tsock::Handle peer = tsock::Accept(lfd);
    tsock::Close(lfd);
    CHECK_TRUE(tsock::Valid(peer));
    if (!tsock::Valid(peer)) {
        return;
    }

    std::vector<std::uint8_t> block(kEntries * kSeg);
    std::vector<go2cpp::bufio::BatchEntry> entries(kEntries);
    for (std::size_t i = 0; i < kEntries; ++i) {
        FillPattern(block.data() + i * kSeg, kSeg, i * 53);
        entries[i].data = reinterpret_cast<const char*>(block.data() + i * kSeg);
        entries[i].size = kSeg;
    }

    // 一次 64 段：字节流顺序与内容必须一致。
    {
        rd::net::SendResult sr{};
        const auto st = cli.SendBatch(entries.data(), entries.size(), &sr);
        CHECK_STATUS(st, rd::net::IoStatus::kOk);
        CHECK_TRUE(cli.last_batch_submitted() == entries.size());
        CHECK_TRUE(cli.last_transferred() == block.size());
        std::vector<std::uint8_t> got(block.size());
        const std::size_t off = ReadFullBytes(peer, got.data(), got.size());
        const bool same = off == block.size() &&
                          std::memcmp(got.data(), block.data(), block.size()) == 0;
        std::printf("  tcp-batch: status=%s submitted=%zu transferred=%zu "
                    "received=%zu match=%s\n",
                    rd::net::IoStatusName(st), cli.last_batch_submitted(),
                    cli.last_transferred(), off, same ? "yes" : "no");
        CHECK_TRUE(off == block.size());
        CHECK_TRUE(same);
    }

    // count=0：kOk no-op。
    {
        rd::net::SendResult sr{};
        CHECK_STATUS(cli.SendBatch(entries.data(), 0, &sr), rd::net::IoStatus::kOk);
        CHECK_TRUE(cli.last_batch_submitted() == 0);
        CHECK_TRUE(cli.last_transferred() == 0);
    }

    // count=1。
    {
        rd::net::SendResult sr{};
        CHECK_STATUS(cli.SendBatch(entries.data(), 1, &sr), rd::net::IoStatus::kOk);
        CHECK_TRUE(cli.last_batch_submitted() == 1);
        CHECK_TRUE(cli.last_transferred() == kSeg);
        std::vector<std::uint8_t> got(kSeg);
        CHECK_TRUE(ReadFullBytes(peer, got.data(), got.size()) == kSeg);
        CHECK_TRUE(std::memcmp(got.data(), block.data(), kSeg) == 0);
    }

    // 超过上限：1500 段 × 16B -> 1024 段 + 476 段两次提交。
    {
        constexpr std::size_t kSmallSeg = 16;
        constexpr std::size_t kBig = 1500;
        std::vector<std::uint8_t> small(kBig * kSmallSeg);
        std::vector<go2cpp::bufio::BatchEntry> be(kBig);
        for (std::size_t i = 0; i < kBig; ++i) {
            FillPattern(small.data() + i * kSmallSeg, kSmallSeg, i * 5);
            be[i].data = reinterpret_cast<const char*>(small.data() + i * kSmallSeg);
            be[i].size = kSmallSeg;
        }
        rd::net::SendResult sr{};
        CHECK_STATUS(cli.SendBatch(be.data(), kBig, &sr), rd::net::IoStatus::kOk);
        CHECK_TRUE(cli.last_batch_submitted() == rd::net::kMaxBatchEntries);
        std::vector<std::uint8_t> got(rd::net::kMaxBatchEntries * kSmallSeg);
        CHECK_TRUE(ReadFullBytes(peer, got.data(), got.size()) == got.size());
        CHECK_TRUE(std::memcmp(got.data(), small.data(), got.size()) == 0);
        const std::size_t rest = kBig - rd::net::kMaxBatchEntries;
        CHECK_STATUS(cli.SendBatch(be.data() + rd::net::kMaxBatchEntries, rest, &sr),
                     rd::net::IoStatus::kOk);
        CHECK_TRUE(cli.last_batch_submitted() == rest);
        std::vector<std::uint8_t> got2(rest * kSmallSeg);
        CHECK_TRUE(ReadFullBytes(peer, got2.data(), got2.size()) == got2.size());
        CHECK_TRUE(std::memcmp(got2.data(), small.data() +
                                                 rd::net::kMaxBatchEntries * kSmallSeg,
                               got2.size()) == 0);
        std::printf("  tcp-limit: count=1500 committed=%zu + %zu\n",
                    rd::net::kMaxBatchEntries, rest);
    }
    tsock::Close(peer);

    // 部分提交（对端慢读）：SO_SNDBUF=4096，64 段 × 1KB 一次提交。
    {
        std::uint16_t p2 = 0;
        const tsock::Handle l2 = MakeListener(&p2);
        CHECK_TRUE(tsock::Valid(l2));
        if (tsock::Valid(l2)) {
            rd::net::DialOptions opts;
            opts.send_buffer_bytes = 4096;
            opts.tcp_no_delay = true;
            auto c2_opened =
                rd::net::StreamSocket::Open(rd::net::Endpoint{"127.0.0.1", p2}, opts);
            CHECK_TRUE(c2_opened.ok());
            if (c2_opened.ok()) {
                rd::net::StreamSocket c2 = std::move(*c2_opened);
                const tsock::Handle peer2 = tsock::Accept(l2);
                CHECK_TRUE(tsock::Valid(peer2));
                if (tsock::Valid(peer2)) {
                    rd::net::SendResult sr{};
                    const auto st = c2.SendBatch(entries.data(), entries.size(), &sr);
                    std::printf("  tcp-partial: status=%s submitted=%zu/%zu "
                                "transferred=%zu (SO_SNDBUF=4096, 对端不读)\n",
                                rd::net::IoStatusName(st), c2.last_batch_submitted(),
                                entries.size(), c2.last_transferred());
#if defined(_WIN32)
                    // 平台差异（REQ-010）：Linux 的 sendmsg 一次提交受 SO_SNDBUF
                    // 硬约束，对端不读时必然是部分提交；Windows 的 WSASend 把
                    // SO_SNDBUF 当提示，AFD 可整体接下一次 64KB 的 gather 写，
                    // 返回 kOk 是合法结果。两种结果都必须满足前缀语义：
                    //   kOk        → 64 段全部提交，字节数精确等于 block.size()；
                    //   kWouldBlock → 前 K 段（1<=K<64）提交，transferred 落在
                    //                 第 K 段内（部分段不计入 last_batch_submitted）。
                    // 无论哪种，peer 读到的字节都必须与 block 前缀逐字节相同。
                    CHECK_TRUE(st == rd::net::IoStatus::kOk ||
                               st == rd::net::IoStatus::kWouldBlock);
                    CHECK_TRUE(c2.last_batch_submitted() > 0);
                    if (st == rd::net::IoStatus::kOk) {
                        CHECK_TRUE(c2.last_batch_submitted() == entries.size());
                        CHECK_TRUE(c2.last_transferred() == block.size());
                    } else {
                        CHECK_TRUE(c2.last_batch_submitted() < entries.size());
                        CHECK_TRUE(c2.last_transferred() >=
                                   c2.last_batch_submitted() * kSeg);
                        CHECK_TRUE(c2.last_transferred() <
                                   (c2.last_batch_submitted() + 1) * kSeg);
                    }
                    std::vector<std::uint8_t> got(c2.last_transferred());
                    CHECK_TRUE(ReadFullBytes(peer2, got.data(), got.size()) ==
                               got.size());
                    CHECK_TRUE(std::memcmp(got.data(), block.data(), got.size()) == 0);
#else
                    CHECK_STATUS(st, rd::net::IoStatus::kWouldBlock);
                    CHECK_TRUE(c2.last_batch_submitted() > 0);
                    CHECK_TRUE(c2.last_batch_submitted() < entries.size());
                    CHECK_TRUE(c2.last_transferred() >=
                               c2.last_batch_submitted() * kSeg);
                    CHECK_TRUE(c2.last_transferred() <
                               (c2.last_batch_submitted() + 1) * kSeg);
                    std::vector<std::uint8_t> got(c2.last_transferred());
                    CHECK_TRUE(ReadFullBytes(peer2, got.data(), got.size()) ==
                               got.size());
                    CHECK_TRUE(std::memcmp(got.data(), block.data(), got.size()) == 0);
#endif
                    tsock::Close(peer2);
                }
                c2.Close();
            }
            tsock::Close(l2);
        }
    }

    // 3 轮对比：批量 64 段 vs 64 次单条 Send（只计发送阶段）。
    constexpr int kRounds = 100;
    double batched_ns[3] = {0, 0, 0};
    double batched_mib[3] = {0, 0, 0};
    double single_ns[3] = {0, 0, 0};
    double single_mib[3] = {0, 0, 0};
    for (int round = 0; round < 3; ++round) {
        const TcpBatchBench b = RunTcpBatchBench(true, kRounds, kEntries, kSeg);
        const TcpBatchBench s = RunTcpBatchBench(false, kRounds, kEntries, kSeg);
        batched_ns[round] = b.ns_per_segment;
        batched_mib[round] = b.mib_per_s;
        single_ns[round] = s.ns_per_segment;
        single_mib[round] = s.mib_per_s;
        std::printf(
            "  round %d: batch ns/seg=%.1f %.1f MiB/s (rounds=%d) | "
            "single ns/seg=%.1f %.1f MiB/s (rounds=%d)\n",
            round + 1, b.ns_per_segment, b.mib_per_s, b.rounds, s.ns_per_segment,
            s.mib_per_s, s.rounds);
        if (b.rounds != kRounds || s.rounds != kRounds) {
            std::printf("  [FAIL-BATCH] TCP bench 未跑满：batch=%d single=%d\n",
                        b.rounds, s.rounds);
        }
    }
    const auto avg = [](const double (&v)[3]) { return (v[0] + v[1] + v[2]) / 3.0; };
    std::printf("  avg: batch ns/seg=%.1f %.1f MiB/s | single ns/seg=%.1f "
                "%.1f MiB/s | batch_faster=%s\n",
                avg(batched_ns), avg(batched_mib), avg(single_ns), avg(single_mib),
                avg(batched_ns) < avg(single_ns) ? "yes" : "no");
}

}  // namespace

int main() {
    std::printf("REQ-006 四态 Socket 抽象验收（%s）\n", __DATE__);
    if (!tsock::Init()) {
        std::printf("[FAIL] Winsock 初始化失败\n");
        return 1;
    }
    Scenario1Fin();
    Scenario2Reset();
    Scenario3Truncated();
    Scenario4WouldBlock();
    Scenario5aEchoNoLeak();
    Scenario5bHundredSockets();
    Benchmark();
    BatchComparison();
    Scenario6UdpSendBatch();
    Scenario7TcpSendBatch();
    std::printf("\n== 汇总: checks=%d failures=%d ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
