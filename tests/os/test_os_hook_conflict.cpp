/**
 * @file test_os_hook_conflict.cpp
 * @brief `os` 模块与 `hook` 模块的冲突点实测。
 *
 * ## 这个测试守着什么红线
 *
 * `os::File` 的读写走**裸 `syscall(SYS_read/SYS_write)`**，绕过 Hook。这个设计
 * 成立的前提是：**只有普通文件才能进 `os::File`** —— 因为普通文件的 IO 在内核里
 * 同步完成、不会 `EAGAIN`；而 FIFO / pipe / eventfd / timerfd / signalfd / 字符设备
 * 上的 `read` **会真阻塞**，走裸 syscall 就等于**卡死整个 worker 线程**（本地 DoS）。
 *
 * 本测试把「哪些 fd 会被 Hook 接管」「哪些 fd 会被 os::File 拒绝」逐项实测出来。
 *
 * ## 实测得到的关键事实
 *
 * | fd 类型 | `getsockopt(SO_TYPE)` | `st_mode & S_IFMT` | Hook 接管 | 应该被 Adopt 拒绝 |
 * |---|---|---|---|---|
 * | 普通文件 | `ENOTSOCK` | `S_IFREG` | 否 | **否（唯一接受）** |
 * | FIFO / pipe | `ENOTSOCK` | `S_IFIFO` | 否 | 是 |
 * | 字符设备 | `ENOTSOCK` | `S_IFCHR` | 否 | 是 |
 * | **eventfd / timerfd / signalfd** | `ENOTSOCK` | **`0`（无类型位）** | 否 | 是 |
 * | socket | `SOCK_STREAM` 等 | `S_IFSOCK` | **是** | 是 |
 *
 * **`eventfd`/`timerfd`/`signalfd` 的类型位是 0** —— 这是 Linux `anon_inodefs`
 * 的特性。任何基于 `S_ISxxx` 的**黑名单**都会漏掉它们，而它们恰好都会阻塞。
 * 所以 `Adopt`/`Open` 必须用**白名单**（只接受 `S_ISREG`）。
 */
#include "go2cpp/os/file.hpp"
#include "go2cpp/network/hook.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace {

using go2cpp::os::File;
using go2cpp::os::FileStatus;
using go2cpp::os::OsError;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[os-hook] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 与 hook 的 inspect_socket 用同一个调用
bool IsSocketByGetsockopt(int fd) {
    int type = 0;
    socklen_t size = sizeof(type);
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0;
}

std::uint32_t ModeTypeBits(int fd) {
    struct ::stat info {};
    if (::syscall(SYS_fstat, fd, &info) != 0) {
        return 0xFFFFFFFFu;
    }
    return static_cast<std::uint32_t>(info.st_mode & S_IFMT);
}

// ---------------------------------------------------------------------------
// 场景 1：fd 类型光谱 —— gotssockopt + 类型位 + Adopt 三方对照
// ---------------------------------------------------------------------------
void ScenarioFdSpectrum() {
    struct Entry {
        const char* name;
        int fd;
        bool expect_hook_takes;   // Hook 是否接管
        bool expect_adopt_ok;     // Adopt 是否接受
        OsError expect_code;      // expect_adopt_ok == false 时的错误码
    };

    std::vector<Entry> entries;
    std::vector<int> owned;

    const char* reg = "/tmp/os_hook_regular.txt";
    int fd = ::open(reg, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { ::write(fd, "x", 1); ::lseek(fd, 0, SEEK_SET); }
    owned.push_back(fd);
    entries.push_back({"普通文件", fd, false, true, OsError::kOk});

    const char* fifo = "/tmp/os_hook_fifo";
    ::unlink(fifo);
    ::mkfifo(fifo, 0644);
    fd = ::open(fifo, O_RDWR | O_NONBLOCK);   // O_RDWR 避免 open 阻塞
    owned.push_back(fd);
    entries.push_back({"FIFO", fd, false, false, OsError::kNotRegularFile});

    int pfd[2] = {-1, -1};
    (void)::pipe(pfd);
    owned.push_back(pfd[0]);
    owned.push_back(pfd[1]);
    entries.push_back({"pipe 读端", pfd[0], false, false, OsError::kNotRegularFile});

    fd = ::open("/dev/urandom", O_RDONLY);
    owned.push_back(fd);
    entries.push_back({"/dev/urandom", fd, false, false, OsError::kNotRegularFile});

    fd = ::eventfd(0, EFD_NONBLOCK);
    owned.push_back(fd);
    entries.push_back({"eventfd", fd, false, false, OsError::kNotRegularFile});

    fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    owned.push_back(fd);
    entries.push_back({"timerfd", fd, false, false, OsError::kNotRegularFile});

    sigset_t mask;
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGUSR1);
    fd = ::signalfd(-1, &mask, SFD_NONBLOCK);
    owned.push_back(fd);
    entries.push_back({"signalfd", fd, false, false, OsError::kNotRegularFile});

    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    owned.push_back(fd);
    entries.push_back({"AF_UNIX socket", fd, true, false, OsError::kIsSocket});

    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    owned.push_back(fd);
    entries.push_back({"AF_INET socket", fd, true, false, OsError::kIsSocket});

    std::printf("%-16s %-10s %-12s %-8s %-10s %s\n",
                "fd 类型", "S_IFMT", "getsockopt", "Hook", "Adopt", "错误码");
    std::printf("%s\n", std::string(84, '-').c_str());

    for (const auto& e : entries) {
        if (e.fd < 0) {
            std::fprintf(stderr, "[os-hook] %s 创建失败\n", e.name);
            ++g_failures;
            continue;
        }
        const bool hook_takes = IsSocketByGetsockopt(e.fd);
        const std::uint32_t bits = ModeTypeBits(e.fd);

        File file;
        const FileStatus st = File::Adopt(e.fd, false, &file);

        std::printf("%-16s 0%-10o %-12s %-8s %-10s %s\n",
                    e.name, bits,
                    hook_takes ? "SOCK" : "ENOTSOCK",
                    hook_takes ? "是" : "否",
                    st.ok() ? "接受" : "拒绝",
                    st.ok() ? "-" : go2cpp::os::ToString(st.code));

        if (hook_takes != e.expect_hook_takes) {
            std::fprintf(stderr, "[os-hook] %s: Hook 接管预期不符\n", e.name);
            ++g_failures;
        }
        if (st.ok() != e.expect_adopt_ok) {
            std::fprintf(stderr, "[os-hook] %s: Adopt 结果预期不符（实际 %s）\n",
                         e.name, st.ok() ? "接受" : "拒绝");
            ++g_failures;
        }
        if (!e.expect_adopt_ok && st.code != e.expect_code) {
            std::fprintf(stderr, "[os-hook] %s: 错误码预期不符（实际 %s）\n",
                         e.name, go2cpp::os::ToString(st.code));
            ++g_failures;
        }
        // 接受了的话，File 会持有 fd（take_ownership=false，所以不会 close）
    }

    for (int f : owned) { if (f >= 0) ::close(f); }
    ::unlink(reg);
    ::unlink(fifo);
}

// ---------------------------------------------------------------------------
// 场景 2：Open 一个 FIFO 路径 —— 必须被拒绝，且不能阻塞在 open 本身
// ---------------------------------------------------------------------------
void ScenarioOpenFifo() {
    const char* fifo = "/tmp/os_hook_open_fifo";
    ::unlink(fifo);
    Check(::mkfifo(fifo, 0644) == 0, "mkfifo failed");

    File file;
    const auto start = std::chrono::steady_clock::now();
    const FileStatus st = File::Open(fifo, go2cpp::os::OpenFlag::kReadOnly, 0, &file);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();

    std::printf("[os-hook] Open(FIFO) -> %s, 耗时 %lld ms\n",
                st.ok() ? "接受" : go2cpp::os::ToString(st.code),
                static_cast<long long>(elapsed));

    Check(!st.ok(), "Open on a FIFO path must be rejected");
    Check(st.code == OsError::kNotRegularFile, "Open on FIFO should report kNotRegularFile");
    // 关键：不能阻塞在 open 本身（RawOpen 加了 O_NONBLOCK）
    Check(elapsed < 2000, "Open on FIFO must not block (O_NONBLOCK 必须生效)");

    ::unlink(fifo);
}

// ---------------------------------------------------------------------------
// 场景 3：Hook 开关不影响 os::File（os 走裸 syscall）
// ---------------------------------------------------------------------------
void ScenarioHookSwitchDoesNotAffectOs() {
    const char* path = "/tmp/os_hook_switch.txt";
    {
        File create;
        const auto st = File::Create(path, 0644, &create);
        Check(st.ok(), "create temp file failed");
        if (st.ok()) {
            const char payload[] = "hello-os";
            Check(create.Write(payload, sizeof(payload) - 1) == sizeof(payload) - 1,
                  "write failed");
        }
    }

    // 关闭 kRead / kClose 的 hook（kClose 会被硬拒绝，这里只验证返回）
    const bool read_set = go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kRead, false);
    const bool close_set = go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kClose, false);
    std::printf("[os-hook] SetOpEnabled(kRead,false)=%d  SetOpEnabled(kClose,false)=%d\n",
                read_set ? 1 : 0, close_set ? 1 : 0);
    Check(read_set, "kRead should be disablable");
    Check(!close_set, "kClose must be hard-refused");

    // os::File 走裸 syscall，应该完全不受影响
    File file;
    const auto open_status = File::Open(path, go2cpp::os::OpenFlag::kReadOnly, 0, &file);
    Check(open_status.ok(), "os::File::Open should work while kRead hook is disabled");

    char buffer[16] = {};
    const auto got = file.Read(buffer, sizeof(buffer));
    Check(got == 8, "os::File::Read should still work while kRead hook is disabled");
    Check(std::string(buffer, got > 0 ? static_cast<std::size_t>(got) : 0) == "hello-os",
          "os::File::Read content mismatch");

    // 恢复，避免影响其它测试
    (void)go2cpp::hook::SetOpEnabled(go2cpp::hook::HookOp::kRead, true);
    ::unlink(path);
}


// ---------------------------------------------------------------------------
// 场景 4：Create 一个已存在的 FIFO 路径 —— 必须拒绝且不阻塞
// ---------------------------------------------------------------------------
void ScenarioCreateExistingFifo() {
    const char* fifo = "/tmp/os_hook_create_fifo";
    ::unlink(fifo);
    Check(::mkfifo(fifo, 0644) == 0, "mkfifo for Create test failed");

    // 已存在的 FIFO 路径 + O_CREAT|O_TRUNC：open 会成功打开它（O_TRUNC 对 FIFO 无意义），
    // 所以必须靠 fstat 类型校验拦住；而 RawOpen 的 O_NONBLOCK 保证不会卡在 open 本身。
    File file;
    const auto start = std::chrono::steady_clock::now();
    const FileStatus st = File::Create(fifo, 0644, &file);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();

    std::printf("[os-hook] Create(existing FIFO) -> %s, 耗时 %lld ms\n",
                st.ok() ? "接受" : go2cpp::os::ToString(st.code),
                static_cast<long long>(elapsed));

    Check(!st.ok(), "Create on an existing FIFO path must be rejected");
    Check(st.code == OsError::kNotRegularFile,
          "Create on FIFO should report kNotRegularFile");
    Check(elapsed < 2000, "Create on FIFO must not block (O_NONBLOCK 必须生效)");

    // 同一路径下 Create 一个普通文件仍应成功（确认没误伤正常路径）
    const char* reg = "/tmp/os_hook_create_reg.txt";
    ::unlink(reg);
    File reg_file;
    const FileStatus reg_st = File::Create(reg, 0644, &reg_file);
    Check(reg_st.ok(), "Create on a regular path should still succeed");

    ::unlink(fifo);
    ::unlink(reg);
}

}  // namespace

int main() {
    std::printf("=== os 与 hook 冲突点排查 ===\n");
    std::printf("[os-hook][场景1] fd 类型光谱（11 种）\n");
    ScenarioFdSpectrum();
    std::printf("[os-hook][场景2] Open(FIFO) 必须拒绝且不阻塞\n");
    ScenarioOpenFifo();
    std::printf("[os-hook][场景3] Hook 开关不影响 os::File\n");
    ScenarioHookSwitchDoesNotAffectOs();
    std::printf("[os-hook][场景4] Create(已存在 FIFO) 必须拒绝且不阻塞\n");
    ScenarioCreateExistingFifo();

    if (g_failures == 0) {
        std::printf("\n[os-hook] failures=0\n");
    } else {
        std::printf("\n[os-hook] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
