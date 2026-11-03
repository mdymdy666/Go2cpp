// JinShan REQ-005 验收示例：Lease 到期恰好一次 / Renew 延后 / Stop 后不再触发。
// 只验证行为，不做性能测量。构建运行见同目录 build_and_run.sh。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "go2cpp/relay/lease.hpp"

namespace r = go2cpp::relay;
using namespace std::chrono_literals;

static int g_fail = 0;

static void Report(bool ok, const char* what) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

int main() {
  // 1) 100 ms 租约：到期回调恰好一次，之后不再重复触发。
  {
    std::atomic<int> fired{0};
    bool once = false, no_more = false;
    {
      r::Lease lease(100ms, [&fired] { fired.fetch_add(1); });
      std::this_thread::sleep_for(250ms);  // 已到期约 150 ms
      once = fired.load() == 1;
      std::this_thread::sleep_for(250ms);  // 再等两个周期
      no_more = fired.load() == 1;
    }
    Report(once && no_more, "100ms 租约到期回调恰好一次（再等 250ms 不重复）");
  }

  // 2) 中途 Renew：截止时间重置为「现在 + 100 ms」，不能被原来的到期点提前触发。
  {
    std::atomic<int> fired{0};
    bool not_yet = false, after_renew = false;
    {
      r::Lease lease(100ms, [&fired] { fired.fetch_add(1); });
      std::this_thread::sleep_for(60ms);
      lease.Renew();  // 原到期点还有 40 ms，重置后还有 100 ms
      std::this_thread::sleep_for(60ms);
      not_yet = fired.load() == 0;   // 距 Renew 仅 60 ms
      std::this_thread::sleep_for(120ms);
      after_renew = fired.load() == 1;  // 距 Renew 180 ms，已触发一次
    }
    Report(not_yet && after_renew, "Renew() 把到期点延后（60ms 时未触发，180ms 时触发一次）");
  }

  // 3) Stop：取消后不再触发；Stop 幂等，析构也安全。
  {
    std::atomic<int> fired{0};
    bool never = false;
    {
      r::Lease lease(100ms, [&fired] { fired.fetch_add(1); });
      lease.Stop();
      lease.Stop();  // 幂等
      std::this_thread::sleep_for(250ms);
      never = fired.load() == 0;
    }
    Report(never, "Stop() 后 250ms 内回调未触发（Stop 幂等，不崩）");
  }

  std::printf("lease: %s (%d 个断言失败)\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  return g_fail == 0 ? 0 : 1;
}
