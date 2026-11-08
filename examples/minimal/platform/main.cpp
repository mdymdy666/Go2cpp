// minimal_platform —— 证明 `go2cpp_platform` 是**零依赖**（只用标准库）。
//
// 验证：
//   ldd /tmp/mp/minimal_platform | grep -c 'libgo2cpp_'   # 必须是 0（或只有它自己）
#include <cstdio>
#include <string>

#include "go2cpp/platform/atomic_file.hpp"
#include "go2cpp/platform/last_error.hpp"
#include "go2cpp/platform/steady_clock.hpp"
#include "go2cpp/platform/wake_event.hpp"

int main() {
  using namespace go2cpp::platform;

  // 1) WakeEvent：跨平台「唤醒事件」
  WakeEvent wake;
  std::string err;
  if (!wake.Open(&err)) {
    std::fprintf(stderr, "WakeEvent::Open failed: %s\n", err.c_str());
    return 1;
  }
  wake.Notify();
  wake.Drain();
  std::printf("WakeEvent: ok (handle=%lld)\n",
              static_cast<long long>(wake.NativeHandle()));

  // 2) SteadyClock：单调时钟 + 睡眠
  const std::int64_t t0 = SteadyNanos();
  SleepNanos(1'000'000);  // 1ms
  const std::int64_t t1 = SteadyNanos();
  std::printf("SteadyClock: slept %.3f ms\n",
              static_cast<double>(t1 - t0) / 1e6);

  // 3) LastError：跨平台错误码
  const int code = LastErrorCode();
  std::printf("LastError: code=%d is_would_block=%d is_interrupted=%d\n", code,
              IsWouldBlock(code) ? 1 : 0, IsInterrupted(code) ? 1 : 0);

  // 4) AtomicRename：原子 rename（Windows 用 MoveFileExW 覆盖）
  {
    const std::string a = "/tmp/minimal_platform_a.tmp";
    const std::string b = "/tmp/minimal_platform_b.tmp";
    std::FILE* f = std::fopen(a.c_str(), "wb");
    if (f != nullptr) {
      std::fputs("hello", f);
      std::fclose(f);
    }
    int rerr = 0;
    const bool ok = AtomicRename(a, b, &rerr);
    std::printf("AtomicRename: %s (err=%d)\n", ok ? "ok" : "failed", rerr);
    (void)DeleteFileIfExists(b);
  }

  // 5) 零依赖：这个程序只链接 go2cpp_platform
  return 0;
}
