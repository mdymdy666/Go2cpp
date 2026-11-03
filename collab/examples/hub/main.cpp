// JinShan REQ-003 验收示例：Hub 注册 / 查询 / 关闭 / 幂等注销。
// 只验证行为，不做性能测量。构建运行见同目录 build_and_run.sh。
#include <cstdio>
#include <memory>

#include "go2cpp/relay/hub.hpp"

namespace r = go2cpp::relay;

// Hub 的连接契约就是 streams::Closer（等价 Go 的 io.Closer）。
class Conn final : public r::Hub::Stream {
 public:
  go2cpp::ErrorPtr Close() override {
    ++closes;
    return nullptr;
  }
  int closes = 0;
};

static int g_fail = 0;

int main() {
  r::Hub hub;
  auto a1 = std::make_shared<Conn>(), a2 = std::make_shared<Conn>();
  auto b1 = std::make_shared<Conn>(), b2 = std::make_shared<Conn>();
  hub.Register("room:a", a1);
  hub.Register("room:a", a2);
  hub.Register("room:b", b1);
  hub.Register("room:b", b2);

  const std::size_t na = hub.Lookup("room:a").size();
  const std::size_t nb = hub.Lookup("room:b").size();
  const bool reg_ok = na == 2 && nb == 2 && hub.Keys().size() == 2;
  if (!reg_ok) ++g_fail;
  std::printf("[%s] 2 个 key 各 2 条连接: Lookup(room:a)=%zu, Lookup(room:b)=%zu, Keys=%zu\n",
              reg_ok ? "PASS" : "FAIL", na, nb, hub.Keys().size());

  // 同一对象在同一 key 下重复登记：返回 false，表不变。
  const bool dup = !hub.Register("room:a", a1) && hub.Lookup("room:a").size() == 2;
  if (!dup) ++g_fail;
  std::printf("[%s] 同一连接重复 Register 返回 false 且表不变\n", dup ? "PASS" : "FAIL");

  // Unregister 幂等：同一连接连调两次不崩，第二次是空操作。
  hub.Unregister("room:b", b1);
  hub.Unregister("room:b", b1);
  const std::size_t after_unreg = hub.Lookup("room:b").size();
  const bool unreg_ok = after_unreg == 1;
  if (!unreg_ok) ++g_fail;
  std::printf("[%s] Unregister 重复调用不崩, Lookup(room:b)=%zu\n",
              unreg_ok ? "PASS" : "FAIL", after_unreg);

  // CloseKey：先摘表再锁外 Close；返回 true，Lookup 立即为空，每条连接 Close 一次。
  const bool closed = hub.CloseKey("room:a");
  const bool lookup_empty = hub.Lookup("room:a").empty();
  const bool close_once = a1->closes == 1 && a2->closes == 1;
  const bool close_ok = closed && lookup_empty && close_once;
  if (!close_ok) ++g_fail;
  std::printf("[%s] CloseKey(room:a)=%s, Lookup 为空=%s, 两条连接各 Close 一次 (%d,%d)\n",
              close_ok ? "PASS" : "FAIL", closed ? "true" : "false",
              lookup_empty ? "true" : "false", a1->closes, a2->closes);

  // 对已关闭的 key 再 CloseKey 返回 false；清掉 room:b 后 Keys 为空。
  const bool again = !hub.CloseKey("room:a");
  hub.CloseKey("room:b");
  const bool fin = again && hub.Keys().empty() && b2->closes == 1;
  if (!fin) ++g_fail;
  std::printf("[%s] 重复 CloseKey 返回 false, 清空后 Keys=%zu\n",
              fin ? "PASS" : "FAIL", hub.Keys().size());
  return g_fail == 0 ? 0 : 1;
}
