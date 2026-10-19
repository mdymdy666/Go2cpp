// Fiber（受管 G）场景下 DoneCase / AfterCase 的专项测试。
// 补上 T1~T11 未覆盖的 park 路径：Select 在 Fiber 里 park，被另一个 Fiber 唤醒。

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "go2cpp/concurrency/channel.hpp"
#include "go2cpp/concurrency/context.hpp"
#include "go2cpp/runtime.hpp"

using namespace go2cpp;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", name);
  if (!ok) {
    ++g_failures;
  }
}

}  // namespace

int main() {
  Scheduler scheduler(4);
  scheduler.start();

  // ---------- F1：Fiber 里 Select(RecvCase+DoneCase)，由另一个 Fiber cancel ----------
  {
    auto pair = Context::WithCancel(Context::Background());
    auto ctx = pair.first;
    auto cancel = pair.second;
    auto ch = MakeChannel<int>(1);

    auto result = std::make_shared<int>(-1);
    auto status = std::make_shared<ChannelStatus>(ChannelStatus::kInvalid);

    auto waiter = go(scheduler, [ch, ctx, result, status] {
      std::vector<SelectCase> cases{RecvCase(ch), DoneCase(ctx)};
      const auto r = Select(cases, ctx, std::optional<ContextDuration>(3s));
      *result = static_cast<int>(r.index);
      *status = r.status;
    });
    go(scheduler, [cancel] {
      std::this_thread::sleep_for(50ms);
      cancel();
    });
    waiter->wait();
    Check(*result == 1 && *status == ChannelStatus::kCancelled,
          "F1 Fiber 内 DoneCase 被另一个 Fiber 取消唤醒");
  }

  // ---------- F2：Fiber 里 Select(RecvCase+DoneCase)，由另一个 Fiber send ----------
  {
    auto pair = Context::WithCancel(Context::Background());
    auto ctx = pair.first;
    auto ch = MakeChannel<int>(1);

    auto result = std::make_shared<int>(-1);
    auto value = std::make_shared<int>(0);

    auto waiter = go(scheduler, [ch, ctx, result, value] {
      std::vector<SelectCase> cases{RecvCase(ch), DoneCase(ctx)};
      const auto r = Select(cases, ctx, std::optional<ContextDuration>(3s));
      *result = static_cast<int>(r.index);
      if (const auto v = r.Value<int>()) {
        *value = *v;
      }
    });
    go(scheduler, [ch] {
      std::this_thread::sleep_for(50ms);
      (void)ch->Send(1234);
    });
    waiter->wait();
    Check(*result == 0 && *value == 1234,
          "F2 Fiber 内 RecvCase 被另一个 Fiber 的 send 唤醒");
  }

  // ---------- F3：Fiber 里 Select(AfterCase)，按时到期 ----------
  {
    auto result = std::make_shared<int>(-1);
    const auto start = std::chrono::steady_clock::now();
    auto waiter = go(scheduler, [result] {
      std::vector<SelectCase> cases{AfterCase(60ms)};
      const auto r = Select(cases, {}, std::optional<ContextDuration>(3s));
      *result = static_cast<int>(r.index);
    });
    waiter->wait();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    Check(*result == 0 && ms >= 50 && ms < 2000,
          "F3 Fiber 内 AfterCase 按时到期");
  }

  // ---------- F4：Fiber 里 Select(RecvCase+DoneCase)，数据与取消同时就绪 ----------
  {
    auto pair = Context::WithCancel(Context::Background());
    auto ctx = pair.first;
    auto ch = MakeChannel<int>(1);

    // 先发数据再 cancel：无论哪个赢，都必须恰好返回一个合法 index
    (void)ch->Send(7);
    pair.second();

    auto result = std::make_shared<int>(-1);
    auto waiter = go(scheduler, [ch, ctx, result] {
      std::vector<SelectCase> cases{RecvCase(ch), DoneCase(ctx)};
      const auto r = Select(cases, ctx, std::optional<ContextDuration>(3s));
      *result = static_cast<int>(r.index);
    });
    waiter->wait();
    Check(*result == 0 || *result == 1,
          "F4 Fiber 内数据与取消同时就绪时不崩且选中其一");
  }

  // ---------- F5：Fiber 里 1000 次 Select(default) 不卡住调度器 ----------
  {
    auto counter = std::make_shared<int>(0);
    auto ch = MakeChannel<int>(1);
    auto waiter = go(scheduler, [ch, counter] {
      for (int i = 0; i < 1000; ++i) {
        std::vector<SelectCase> cases{RecvCase(ch), DefaultCase()};
        const auto r = Select(cases, {}, std::nullopt);
        if (r.index == 1) {
          ++*counter;
        }
      }
    });
    waiter->wait();
    Check(*counter == 1000, "F5 Fiber 内 1000 次 default 命中");
  }

  std::printf("\n[fiber_select] failures=%d\n", g_failures);
  scheduler.shutdown();
  return g_failures == 0 ? 0 : 1;
}
