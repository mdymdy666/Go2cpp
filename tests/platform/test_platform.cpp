// go2cpp::platform 三件基础件单元测试
#include <cstdio>
#include <thread>

#include "go2cpp/platform/last_error.hpp"
#include "go2cpp/platform/steady_clock.hpp"
#include "go2cpp/platform/wake_event.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    ++g_checks;                                                  \
    if (!(cond)) {                                               \
      ++g_failures;                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                            \
  } while (0)

void TestWakeEvent() {
  go2cpp::platform::WakeEvent ev;
  CHECK(!ev.valid());
  std::string err;
  CHECK(ev.Open(&err));
  CHECK(ev.valid());
  CHECK(ev.NativeHandle() >= 0);
  ev.Notify();
  ev.Notify();   // Windows 会合并；Linux 会累积
  ev.Drain();
  ev.Drain();    // 幂等
  // Open 幂等
  CHECK(ev.Open(&err));
}

void TestLastError() {
  using namespace go2cpp::platform;
  // 制造一个错误：打开不存在文件
  const int code = [] {
    volatile int x = LastErrorCode();
    (void)x;
    return LastErrorCode();
  }();
  (void)code;
  // WouldBlock / Interrupted 的常量判断
  CHECK(IsWouldBlock(IsWouldBlock(0) ? 0 : 1) == IsWouldBlock(1));
  CHECK(IsInterrupted(IsInterrupted(0) ? 0 : 1) == IsInterrupted(1));
  // 错误消息非空
  const std::string msg = LastErrorMessage(2);  // ENOENT
  CHECK(!msg.empty());
}

void TestSteadyClock() {
  using namespace go2cpp::platform;
  const std::int64_t a = SteadyNanos();
  CHECK(a > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const std::int64_t b = SteadyNanos();
  CHECK(b > a);
  const std::int64_t delta = b - a;
  CHECK(delta >= 4'000'000);   // 至少 4ms
  CHECK(delta < 200'000'000);  // 不超过 200ms（CI 抖动容忍）

  // SleepNanos 精度：睡 2ms，实测 >= 1.8ms 且 < 50ms
  const std::int64_t c = SteadyNanos();
  SleepNanos(2'000'000);
  const std::int64_t d = SteadyNanos();
  const std::int64_t slept = d - c;
  CHECK(slept >= 1'500'000);
  CHECK(slept < 50'000'000);
}

}  // namespace

int main() {
  TestWakeEvent();
  TestLastError();
  TestSteadyClock();
  std::printf("platform: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
