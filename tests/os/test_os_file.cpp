/**
 * @file test_os_file.cpp
 * @brief `go2cpp::os` 模块测试（开荒版）。
 *
 * ## 覆盖策略
 *
 * 项目要求「压测 + 随机 + ≥5 个真实场景」。本文件覆盖 10 个场景：
 *
 * 1. 小文件读写往返（含 `WriteAll` 部分写路径）
 * 2. 大文件（8 MiB）分块写 + `ReadFile` 整读
 * 3. 空文件（0 字节边界）
 * 4. 错误路径：不存在、只读、非法参数
 * 5. **`Seek` 边界**：`SEEK_SET/CUR/END`、负偏移、超 EOF、`Tell`
 * 6. **`Adopt` 拒绝 socket fd** —— 验证 file.hpp 1.4 的边界结论
 * 7. 并发 `pread`/`pwrite`（4 线程 × 各写各的区间，互不干扰）
 * 8. 环境变量（`Getenv`/`Setenv`/`Unsetenv`/`Environ`）
 * 9. `SetArgs`/`Args`/`ProgramName`
 * 10. 压测：10 万次小读写 + 随机偏移 `pread`
 *
 * ## 为什么特意测「拒绝 socket」
 *
 * `os::File` 的读写绕过了 Hook（见 file.hpp 1.2/1.3）。如果误把 socket fd 装进来，
 * `read`/`write` 走裸 `syscall` 不会被 Hook 接管，会把整个 M 线程阻塞在 socket 上、
 * 拖死协程调度器。所以 `Adopt` 必须显式拒绝 `S_ISSOCK` —— 这个测试守着这条红线。
 */
#include "go2cpp/config/config.hpp"
#include "go2cpp/os/os.hpp"

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace go2cpp::os;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[os] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 测试用的临时目录（ctest 在 build-quality 下跑，用 /tmp 更稳）。
///
/// **必须按 pid 隔离**：并发执行两份全量 ctest 时，同一个测试二进制会被两个
/// 进程同时运行；固定目录会让它们互相覆盖 large.bin / stress.bin，实测表现为
/// "random ReadAt returned wrong data"，与 os 模块的实现无关。
const std::string kTmpDir =
    std::string("/tmp/go2cpp_os_test_") + std::to_string(::getpid());

std::string Path(const std::string& name) { return kTmpDir + "/" + name; }

void EnsureTmpDir() {
    // 用 os 模块自己的能力建目录不可行（未实现 MkdDir），直接调 mkdir(2)。
    ::mkdir(kTmpDir.c_str(), 0755);
}

// ---------------------------------------------------------------------------
// 场景 1：小文件读写往返
// ---------------------------------------------------------------------------
void ScenarioSmallRoundTrip() {
    const std::string path = Path("small.txt");
    const std::string payload = "hello, go2cpp os module\nwith a second line\n";

    std::string error;
    Check(WriteFile(path, payload, &error).ok(), "WriteFile failed");

    std::string read_back;
    Check(ReadFile(path, &read_back, &error).ok(), "ReadFile failed");
    Check(read_back == payload, "small file round-trip mismatch");
    Check(read_back.size() == payload.size(), "small file size mismatch");

    // 单独验证 WriteAll 写全（大 payload 会触发多次 write）
    File file;
    Check(File::Create(Path("writeall.txt"), 0644, &file).ok(), "Create writeall failed");
    const std::string big(100000, 'W');
    const std::size_t written = file.WriteAll(big.data(), big.size());
    Check(written == big.size(), "WriteAll should write every byte");
    file.Close();

    std::string verify;
    Check(ReadFile(Path("writeall.txt"), &verify, &error).ok(), "ReadFile writeall failed");
    Check(verify == big, "WriteAll content mismatch");
}

// ---------------------------------------------------------------------------
// 场景 2：大文件（8 MiB）
// ---------------------------------------------------------------------------
void ScenarioLargeFile() {
    constexpr std::size_t kTotal = 8u * 1024u * 1024u;
    constexpr std::size_t kChunk = 256u * 1024u;
    const std::string path = Path("large.bin");

    File file;
    Check(File::Create(path, 0644, &file).ok(), "create large failed");
    if (!file.valid()) {
        return;
    }
    // 写入可校验的模式（按块号填充）
    std::vector<char> chunk(kChunk);
    std::size_t total = 0;
    int block = 0;
    while (total < kTotal) {
        const std::size_t take = std::min(kChunk, kTotal - total);
        std::memset(chunk.data(), static_cast<char>(block & 0xFF), take);
        const std::size_t wrote = file.WriteAll(chunk.data(), take);
        Check(wrote == take, "large chunk write incomplete");
        total += wrote;
        ++block;
    }
    Check(file.Size() == static_cast<off_t>(kTotal), "large file size wrong");
    file.Close();

    std::string content;
    std::string error;
    Check(ReadFile(path, &content, &error).ok(), "read large failed");
    Check(content.size() == kTotal, "large file read size mismatch");
}

// ---------------------------------------------------------------------------
// 场景 3：空文件
// ---------------------------------------------------------------------------
void ScenarioEmptyFile() {
    const std::string path = Path("empty.txt");
    std::string error;
    Check(WriteFile(path, "", &error).ok(), "write empty failed");

    std::string content = "not empty";
    Check(ReadFile(path, &content, &error).ok(), "read empty failed");
    Check(content.empty(), "empty file should read back empty");

    File file;
    Check(File::Open(path, OpenFlag::kReadOnly, 0, &file).ok(), "open empty failed");
    Check(file.Size() == 0, "empty file size should be 0");
    char byte = 0;
    Check(file.Read(&byte, 1) == 0, "read at EOF should return 0");
}

// ---------------------------------------------------------------------------
// 场景 4：错误路径
// ---------------------------------------------------------------------------
void ScenarioErrors() {
    // 4.1 不存在的文件
    File missing;
    const auto missing_status = File::Open(Path("no-such-file-xyz"), OpenFlag::kReadOnly, 0,
                                           &missing);
    Check(!missing_status.ok(), "open nonexistent should fail");
    Check(missing_status.code == OsError::kOpenFailed, "should be kOpenFailed");
    Check(missing_status.system_error == ENOENT, "errno should be ENOENT");

    // 4.2 空路径
    File empty_path;
    Check(File::Open("", OpenFlag::kReadOnly, 0, &empty_path).code ==
              OsError::kInvalidArgument,
          "empty path should be kInvalidArgument");

    // 4.3 只读文件不可写
    const std::string ro = Path("readonly.txt");
    std::string error;
    Check(WriteFile(ro, "data", &error).ok(), "create readonly failed");
    ::chmod(ro.c_str(), 0444);
    File ro_file;
    const auto ro_status = File::Open(ro, OpenFlag::kWriteOnly, 0, &ro_file);
    Check(!ro_status.ok(), "opening read-only file for write should fail");
    Check(ro_status.system_error == EACCES, "errno should be EACCES");
    ::chmod(ro.c_str(), 0644);

    // 4.4 已关闭的句柄
    File closed;
    closed.Close();
    Check(!closed.valid(), "closed file should be invalid");
    Check(closed.Read(&error, 1) < 0, "read on closed file should fail");
    Check(closed.last_error() == EBADF, "closed read errno should be EBADF");
    Check(closed.Seek(0, SEEK_SET) == -1, "seek on closed file should fail");

    // 4.5 null 缓冲区
    File valid;
    Check(File::Create(Path("nullbuf.txt"), 0644, &valid).ok(), "create nullbuf failed");
    Check(valid.Read(nullptr, 10) < 0, "read with null buffer should fail");
    Check(valid.last_error() == EINVAL, "null buffer errno should be EINVAL");
}

// ---------------------------------------------------------------------------
// 场景 5：Seek 边界
// ---------------------------------------------------------------------------
void ScenarioSeek() {
    const std::string path = Path("seek.txt");
    const std::string payload = "0123456789";
    std::string error;
    Check(WriteFile(path, payload, &error).ok(), "write seek file failed");

    File file;
    Check(File::Open(path, OpenFlag::kReadOnly, 0, &file).ok(), "open seek file failed");
    if (!file.valid()) {
        return;
    }
    Check(file.Size() == 10, "seek file size should be 10");

    // SEEK_SET
    Check(file.Seek(3, SEEK_SET) == 3, "SEEK_SET 3 failed");
    Check(file.Tell() == 3, "Tell should be 3");
    char buf[8] = {};
    Check(file.Read(buf, 2) == 2 && buf[0] == '3' && buf[1] == '4', "read after SET wrong");
    Check(file.Tell() == 5, "Tell should be 5 after 2-byte read");

    // SEEK_CUR
    Check(file.Seek(1, SEEK_CUR) == 6, "SEEK_CUR 1 failed");
    Check(file.Read(buf, 1) == 1 && buf[0] == '6', "read after CUR wrong");

    // SEEK_END（负偏移）
    Check(file.Seek(-2, SEEK_END) == 8, "SEEK_END -2 failed");
    Check(file.Read(buf, 2) == 2 && buf[0] == '8' && buf[1] == '9', "read from end wrong");

    // SEEK_END（0）
    Check(file.Seek(0, SEEK_END) == 10, "SEEK_END 0 failed");
    Check(file.Read(buf, 1) == 0, "read at EOF should return 0");

    // 负偏移越界 -> EINVAL
    Check(file.Seek(-1, SEEK_SET) == -1, "negative SEEK_SET should fail");
    Check(file.last_error() == EINVAL, "negative seek errno should be EINVAL");

    // SEEK_END 正偏移（超过 EOF 是允许的）
    Check(file.Seek(5, SEEK_END) == 15, "SEEK_END +5 should be allowed");

    // ReadAt 不改变偏移
    Check(file.Seek(0, SEEK_SET) == 0, "reset seek failed");
    char at[4] = {};
    Check(file.ReadAt(at, 3, 4) == 3 && at[0] == '4' && at[1] == '5' && at[2] == '6',
          "ReadAt wrong");
    Check(file.Tell() == 0, "ReadAt must not change file offset");
}

// ---------------------------------------------------------------------------
// 场景 6：Adopt 必须拒绝 socket fd（file.hpp 1.4 的红线）
// ---------------------------------------------------------------------------
void ScenarioRejectSocket() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        Check(false, "socketpair failed (cannot run socket rejection test)");
        return;
    }
    File adopted;
    const auto status = File::Adopt(fds[0], false, &adopted);
    Check(!status.ok(), "Adopt on a socket fd must fail");
    Check(status.code == OsError::kIsSocket, "should report kIsSocket");
    Check(!adopted.valid(), "adopted file should stay invalid after rejection");

    ::close(fds[0]);
    ::close(fds[1]);

    // 反向：普通文件必须被接受
    std::string error;
    Check(WriteFile(Path("adopt.txt"), "abc", &error).ok(), "create adopt target failed");
    File plain;
    Check(File::Open(Path("adopt.txt"), OpenFlag::kReadOnly, 0, &plain).ok(),
          "open adopt target failed");
    if (plain.valid()) {
        File adopted_plain;
        const auto own_status = File::Adopt(plain.fd(), false, &adopted_plain);
        Check(own_status.ok(), "Adopt on a regular file should succeed");
        Check(adopted_plain.IsRegular(), "adopted plain file should be S_ISREG");
        // take_ownership=false，不要让 adopted_plain 关掉 plain 的 fd
        adopted_plain.Close();
    }
}

// ---------------------------------------------------------------------------
// 场景 7：并发 pread/pwrite（各写各的区间）
// ---------------------------------------------------------------------------
void ScenarioConcurrentPread() {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    constexpr std::size_t kSlot = 64;
    const std::string path = Path("concurrent.bin");

    File file;
    Check(File::Create(path, 0644, &file).ok(), "create concurrent failed");
    if (!file.valid()) {
        return;
    }
    // 预分配空间
    const std::size_t total = static_cast<std::size_t>(kThreads) * kPerThread * kSlot;
    std::vector<char> zeros(total, 0);
    Check(file.WriteAll(zeros.data(), zeros.size()) == zeros.size(), "preallocate failed");

    std::atomic<int> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const off_t offset = static_cast<off_t>(
                    (static_cast<std::size_t>(t) * kPerThread + i) * kSlot);
                // 每个 slot 写入 "T<thread>-<i>" 填充标识
                char slot[kSlot];
                std::memset(slot, static_cast<char>('A' + t), sizeof(slot));
                const int n = std::snprintf(slot, sizeof(slot), "T%d-%d", t, i);
                std::memset(slot + n, static_cast<char>('A' + t), sizeof(slot) - n);
                if (file.WriteAt(slot, sizeof(slot), offset) !=
                    static_cast<ssize_t>(sizeof(slot))) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    Check(errors.load() == 0, "concurrent WriteAt had failures");

    // 校验：每个 slot 的内容必须与写入者一致
    int mismatches = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            const off_t offset = static_cast<off_t>(
                (static_cast<std::size_t>(t) * kPerThread + i) * kSlot);
            char slot[kSlot] = {};
            if (file.ReadAt(slot, sizeof(slot), offset) != static_cast<ssize_t>(sizeof(slot))) {
                ++mismatches;
                continue;
            }
            char expect[16] = {};
            const int n = std::snprintf(expect, sizeof(expect), "T%d-%d", t, i);
            if (std::memcmp(slot, expect, static_cast<std::size_t>(n)) != 0) {
                ++mismatches;
            }
        }
    }
    Check(mismatches == 0, "concurrent pread/pwrite cross-contaminated");
    std::printf("[os] 并发 pread/pwrite: %d 线程 x %d 次, 0 冲突\n", kThreads, kPerThread);
}

// ---------------------------------------------------------------------------
// 场景 8：环境变量
// ---------------------------------------------------------------------------
void ScenarioEnv() {
    // 8.1 设置并读取
    Check(Setenv("GO2CPP_OS_TEST_VAR", "hello-env").ok(), "Setenv failed");
    const auto value = Getenv("GO2CPP_OS_TEST_VAR");
    Check(value.has_value() && *value == "hello-env", "Getenv value mismatch");

    // 8.2 overwrite=false 不覆盖
    Check(Setenv("GO2CPP_OS_TEST_VAR", "second", false).ok(), "Setenv no-overwrite failed");
    const auto still = Getenv("GO2CPP_OS_TEST_VAR");
    Check(still.has_value() && *still == "hello-env", "overwrite=false must not change value");

    // 8.3 overwrite=true 覆盖
    Check(Setenv("GO2CPP_OS_TEST_VAR", "second", true).ok(), "Setenv overwrite failed");
    Check(Getenv("GO2CPP_OS_TEST_VAR").value_or("") == "second", "overwrite=true should change");

    // 8.4 不存在返回 nullopt
    Check(!Getenv("GO2CPP_OS_DEFINITELY_NOT_SET_12345").has_value(),
          "missing env should be nullopt");

    // 8.5 非法名字
    Check(Setenv("", "x").code == OsError::kInvalidArgument, "empty name should fail");
    Check(Setenv("A=B", "x").code == OsError::kInvalidArgument, "name with '=' should fail");

    // 8.6 Environ 包含刚设置的
    bool found = false;
    for (const auto& [k, v] : Environ()) {
        if (k == "GO2CPP_OS_TEST_VAR" && v == "second") {
            found = true;
            break;
        }
    }
    Check(found, "Environ should include the variable we set");

    // 8.7 unsetenv
    Check(Unsetenv("GO2CPP_OS_TEST_VAR").ok(), "Unsetenv failed");
    Check(!Getenv("GO2CPP_OS_TEST_VAR").has_value(), "after unset should be nullopt");
}

// ---------------------------------------------------------------------------
// 场景 9：Args
// ---------------------------------------------------------------------------
void ScenarioArgs() {
    const char* argv[] = {"prog", "a", "b", "c", nullptr};
    SetArgs(4, const_cast<char**>(argv));
    Check(Args().size() == 4, "Args size wrong");
    Check(ProgramName() == "prog", "ProgramName wrong");
    Check(Args()[1] == "a" && Args()[3] == "c", "Args content wrong");

    // 重复调用应覆盖
    const char* argv2[] = {"other", nullptr};
    SetArgs(1, const_cast<char**>(argv2));
    Check(Args().size() == 1 && ProgramName() == "other", "SetArgs should overwrite");

    // 非法参数不崩溃
    SetArgs(0, nullptr);
    Check(Args().empty(), "SetArgs(0,nullptr) should clear");
}

// ---------------------------------------------------------------------------
// 场景 10：压测 + 随机
// ---------------------------------------------------------------------------
void ScenarioStress() {
    const std::string path = Path("stress.bin");
    File file;
    Check(File::Create(path, 0644, &file).ok(), "create stress failed");
    if (!file.valid()) {
        return;
    }

    // 10.1 10 万次小写（顺序）
    constexpr int kWrites = 100000;
    const char byte = 'x';
    for (int i = 0; i < kWrites; ++i) {
        if (file.Write(&byte, 1) != 1) {
            Check(false, "stress sequential write failed");
            break;
        }
    }
    Check(file.Size() == kWrites, "stress file size wrong");

    // 10.2 随机偏移读（固定种子，可复现）
    std::mt19937 rng(20261017);
    std::uniform_int_distribution<int> dist(0, kWrites - 1);
    int wrong = 0;
    for (int i = 0; i < 5000; ++i) {
        const int offset = dist(rng);
        char got = 0;
        if (file.ReadAt(&got, 1, offset) != 1 || got != 'x') {
            ++wrong;
        }
    }
    Check(wrong == 0, "random ReadAt returned wrong data");

    std::printf("[os] 压测: %d 次顺序写 + 5000 次随机 ReadAt 完成\n", kWrites);
}


// ---------------------------------------------------------------------------
// 场景 11：config 集成（os.* 注册 + app.ini 生成 + ReadFile 读配置）
// ---------------------------------------------------------------------------
void ScenarioConfigIntegration() {
    auto& cfg = go2cpp::config::Config::Instance();

    // 11.1 两个 os.* 键都已在静态初始化期注册
    Check(cfg.LookupBase("os.default_read_buffer_size") != nullptr,
          "os.default_read_buffer_size should be registered");
    Check(cfg.LookupBase("os.max_file_size") != nullptr,
          "os.max_file_size should be registered");

    // 11.2 ReadBufferSize 读配置，并做安全钳制
    auto buffer_var = cfg.Lookup<int>("os.default_read_buffer_size", 65536, "");
    Check(buffer_var != nullptr, "lookup buffer var failed");
    if (buffer_var) {
        const int saved = buffer_var->GetValue();
        buffer_var->SetValue(12345);
        Check(ReadBufferSize() == 12345, "ReadBufferSize should follow config");
        buffer_var->SetValue(1);   // 低于下限
        Check(ReadBufferSize() == 4096, "ReadBufferSize should clamp to 4096");
        buffer_var->SetValue(64 * 1024 * 1024);   // 高于上限
        Check(ReadBufferSize() == 16u * 1024u * 1024u, "ReadBufferSize should clamp to 16MiB");
        buffer_var->SetValue(saved);
    }

    // 11.3 MaxFileSize 读配置
    auto max_var = cfg.Lookup<int>("os.max_file_size", 268435456, "");
    Check(max_var != nullptr, "lookup max_file_size failed");
    if (max_var) {
        const int saved = max_var->GetValue();
        max_var->SetValue(1024);
        Check(MaxFileSize() == 1024, "MaxFileSize should follow config");
        // 写一个 2KB 文件，ReadFile 应被上限拒绝
        const std::size_t path_size = 2048;
        std::string error;
        Check(WriteFile(Path("overlimit.bin"), std::string(path_size, 'z'), &error).ok(),
              "write overlimit failed");
        std::string content;
        const auto status = ReadFile(Path("overlimit.bin"), &content, &error);
        Check(!status.ok(), "ReadFile should reject file over os.max_file_size");
        Check(status.system_error == EFBIG, "errno should be EFBIG");
        Check(content.empty(), "rejected read should leave output empty");
        max_var->SetValue(saved);
    }

    // 11.4 SaveDefaultConfig 生成 app.ini，且含 [os] 段
    const std::string ini_path = Path("app.ini");
    std::string error;
    Check(SaveDefaultConfig(ini_path, &error), "SaveDefaultConfig failed");
    std::string ini_content;
    Check(ReadFile(ini_path, &ini_content, &error).ok(), "read generated app.ini failed");
    Check(ini_content.find("[os]") != std::string::npos,
          "generated app.ini should contain [os] section");
    Check(ini_content.find("default_read_buffer_size") != std::string::npos,
          "generated app.ini should contain default_read_buffer_size");
    Check(ini_content.find("max_file_size") != std::string::npos,
          "generated app.ini should contain max_file_size");
    std::printf("[os] 生成的 app.ini 长度=%zu 字节\n", ini_content.size());
}


// ---------------------------------------------------------------------------
// 场景 12：审计回归（NUL / ReadFile 上限 / O_APPEND WriteAt / EINVAL / fd 标志）
// ---------------------------------------------------------------------------
void ScenarioAuditRegressions() {
    // 12.1 路径内嵌 NUL：必须 EINVAL，且不得操作截断后的路径
    const std::string real = Path("nul_base.txt");
    Check(WriteFile(real, "hello").ok(), "nul: prepare file");
    std::string evil = real;
    evil.push_back('\0');
    evil += "-suffix";
    File file;
    auto status = File::Open(evil, OpenFlag::kReadOnly, 0, &file);
    Check(!status.ok() && status.code == OsError::kInvalidArgument &&
              status.system_error == EINVAL && !file.valid(),
          "nul: Open must fail with kInvalidArgument/EINVAL");
    std::string content = "dirty";
    Check(!ReadFile(evil, &content).ok() && content.empty(),
          "nul: ReadFile must fail and clear output");
    Check(!WriteFile(evil, "overwritten").ok(), "nul: WriteFile must fail");
    std::string check;
    Check(ReadFile(real, &check).ok() && check == "hello",
          "nul: truncated real file must stay untouched");

    // 12.2 环境变量内嵌 NUL
    Check(Setenv("OS_NUL_NAME", "v").ok(), "nul env: prepare");
    std::string name = "OS_NUL_NAME";
    name.push_back('\0');
    name += "-suffix";
    Check(!Getenv(name).has_value(), "nul env: Getenv must return nullopt");
    status = Setenv(name, "x");
    Check(!status.ok() && status.code == OsError::kInvalidArgument &&
              status.system_error == EINVAL,
          "nul env: Setenv(name) must be EINVAL");
    std::string value = "v";
    value.push_back('\0');
    value += "x";
    status = Setenv("OS_NUL_NAME", value);
    Check(!status.ok() && status.code == OsError::kInvalidArgument &&
              status.system_error == EINVAL,
          "nul env: Setenv(value) must be EINVAL");
    status = Unsetenv(name);
    Check(!status.ok() && status.code == OsError::kInvalidArgument &&
              status.system_error == EINVAL,
          "nul env: Unsetenv must be EINVAL");
    Check(Getenv("OS_NUL_NAME").value_or("") == "v",
          "nul env: rejected calls must not touch the real variable");
    Check(Unsetenv("OS_NUL_NAME").ok(), "nul env: cleanup");

    // 12.3 ReadFile 上限对 st_size==0 的 procfs 也生效
    auto max_var = go2cpp::config::Config::Instance().Lookup<int>(
        "os.max_file_size", 268435456, "");
    Check(max_var != nullptr, "cap: lookup max_file_size");
    if (max_var && ::access("/proc/self/status", R_OK) == 0) {
        const int saved = max_var->GetValue();
        max_var->SetValue(64);
        std::string proc;
        const auto cap_status = ReadFile("/proc/self/status", &proc);
        Check(!cap_status.ok() && cap_status.system_error == EFBIG && proc.empty(),
              "cap: 64B limit must reject st_size==0 pseudofile");
        max_var->SetValue(saved);
    }

    // 12.4 WriteAt + O_APPEND：显式拒绝，不静默追加
    const std::string append_path = Path("append_regression.txt");
    Check(WriteFile(append_path, "AAA").ok(), "append: prepare");
    File append_file;
    Check(File::Open(append_path, OpenFlag::kWriteOnly | OpenFlag::kAppend, 0,
                     &append_file).ok(),
          "append: open O_APPEND");
    Check(append_file.WriteAt("Z", 1, 0) == -1 &&
              append_file.last_error() == EINVAL,
          "append: WriteAt must fail with EINVAL");
    std::string append_content;
    Check(ReadFile(append_path, &append_content).ok() && append_content == "AAA",
          "append: rejected WriteAt must not modify file");
    Check(append_file.Write("B", 1) == 1, "append: normal Write still appends");

    // 12.5 out==nullptr 统一 EINVAL
    status = File::Open(real, OpenFlag::kReadOnly, 0, nullptr);
    Check(status.code == OsError::kInvalidArgument && status.system_error == EINVAL,
          "EINVAL: Open(out=nullptr)");
    status = File::Adopt(-1, false, nullptr);
    Check(status.code == OsError::kInvalidArgument && status.system_error == EINVAL,
          "EINVAL: Adopt(out=nullptr)");
    status = ReadFile(real, nullptr);
    Check(status.code == OsError::kInvalidArgument && status.system_error == EINVAL,
          "EINVAL: ReadFile(out=nullptr)");

    // 12.6 Open 返回的 fd 带 O_NONBLOCK|O_CLOEXEC（file.hpp 的文档承诺）
    File flags_file;
    Check(File::Open(real, OpenFlag::kReadOnly, 0, &flags_file).ok(),
          "fd flags: open");
    const int flags = ::fcntl(flags_file.fd(), F_GETFL);
    const int fd_flags = ::fcntl(flags_file.fd(), F_GETFD);
    Check(flags >= 0 && (flags & O_NONBLOCK) != 0, "fd flags: O_NONBLOCK");
    Check(fd_flags >= 0 && (fd_flags & FD_CLOEXEC) != 0, "fd flags: FD_CLOEXEC");
}

}  // namespace

int main() {
    std::printf("=== go2cpp::os 模块测试 ===\n");
    EnsureTmpDir();

    std::printf("[os][场景1] 小文件读写往返\n");
    ScenarioSmallRoundTrip();
    std::printf("[os][场景2] 大文件 8MiB\n");
    ScenarioLargeFile();
    std::printf("[os][场景3] 空文件\n");
    ScenarioEmptyFile();
    std::printf("[os][场景4] 错误路径\n");
    ScenarioErrors();
    std::printf("[os][场景5] Seek 边界\n");
    ScenarioSeek();
    std::printf("[os][场景6] Adopt 拒绝 socket fd\n");
    ScenarioRejectSocket();
    std::printf("[os][场景7] 并发 pread/pwrite\n");
    ScenarioConcurrentPread();
    std::printf("[os][场景8] 环境变量\n");
    ScenarioEnv();
    std::printf("[os][场景9] Args\n");
    ScenarioArgs();
    std::printf("[os][场景10] 压测与随机\n");
    ScenarioStress();
    std::printf("[os][场景11] config 集成\n");
    ScenarioConfigIntegration();
    std::printf("[os][场景12] 审计回归\n");
    ScenarioAuditRegressions();

    if (g_failures == 0) {
        std::printf("\n[os] failures=0\n");
    } else {
        std::printf("\n[os] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
