// JinShan REQ-002 验收示例：RelayPipe 双向搬运 8 MiB，两端 SHA-256 一致。
// 只验证行为，不做性能测量。构建运行见同目录 build_and_run.sh。
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <thread>
#include <vector>

#include "go2cpp/hash/sha256.hpp"
#include "go2cpp/relay/relay_pipe.hpp"

namespace h = go2cpp::hash;
namespace r = go2cpp::relay;
namespace s = go2cpp::streams;
using Digest = std::array<std::uint8_t, h::Sha256::kSize>;
constexpr std::size_t kTotal = 8u * 1024u * 1024u;

// socketpair fd -> RelayPipe 的 Stream；Close 幂等并打断阻塞 I/O。
class FdStream final : public r::Stream {
 public:
  explicit FdStream(int fd) : m_fd(fd) {}
  ~FdStream() override { (void)Close(); }
  s::ReadResult Read(char* p, std::size_t n) override {
    for (;;) {
      const ssize_t k = ::read(m_fd.load(), p, n);
      if (k >= 0) return {static_cast<std::size_t>(k), k == 0 ? s::IoEof() : nullptr};
      if (errno != EINTR) return {0, go2cpp::NewError("read failed")};
    }
  }
  s::WriteResult Write(const char* p, std::size_t n) override {
    const ssize_t k = ::write(m_fd.load(), p, n);  // 短写由 RelayPipe 续写
    if (k >= 0) return {static_cast<std::size_t>(k), nullptr};
    return {0, go2cpp::NewError("write failed")};
  }
  go2cpp::ErrorPtr Close() override {
    const int fd = m_fd.exchange(-1);
    if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
    return nullptr;
  }

 private:
  std::atomic<int> m_fd;
};

// 全部写出后 shutdown(SHUT_WR)：让 relay 读到干净 EOF，而不是等超时。
static bool SendAll(int fd, const char* d, std::size_t n) {
  std::size_t done = 0;
  while (done < n) {
    const ssize_t k = ::write(fd, d + done, n - done);
    if (k > 0) done += static_cast<std::size_t>(k);
    else if (errno != EINTR) return false;
  }
  return ::shutdown(fd, SHUT_WR) == 0;
}

// 固定读满 n 字节并流式算 SHA-256（不依赖 EOF，避免两端关闭顺序耦合）。
static Digest RecvHash(int fd, std::size_t n, bool* ok) {
  h::Sha256 sha;
  std::vector<char> buf(64u * 1024u);
  std::size_t got = 0;
  while (got < n) {
    const ssize_t k = ::read(fd, buf.data(), buf.size());
    if (k > 0) { sha.Write(h::AsBytes(buf.data(), static_cast<std::size_t>(k))); got += static_cast<std::size_t>(k); }
    else if (k < 0 && errno == EINTR) continue;
    else break;
  }
  *ok = got == n;
  Digest out{};
  sha.Sum(h::AsBytes(out.data(), out.size()));
  return out;
}

int main() {
  int a[2] = {-1, -1}, b[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, a) != 0 || ::socketpair(AF_UNIX, SOCK_STREAM, 0, b) != 0) {
    std::printf("relay_pipe: socketpair 失败\n");
    return 1;
  }
  FdStream relay_a(a[1]), relay_b(b[0]);  // relay 侧；a[0]/b[1] 留给测试端
  r::PipeConfig cfg;
  cfg.copy_buf = 64u * 1024u;
  cfg.queue_cap = 1024u * 1024u;
  r::RelayPipe pipe(&relay_a, &relay_b, cfg);

  std::vector<char> a_to_b(kTotal), b_to_a(kTotal);
  for (std::size_t i = 0; i < kTotal; ++i) {
    a_to_b[i] = static_cast<char>(0x11 + (i * 31u + 7u) % 251u);
    b_to_a[i] = static_cast<char>(0x77 + (i * 17u + 3u) % 251u);
  }
  bool sa = false, sb = false, rca = false, rcb = false;
  Digest seen_a{}, seen_b{};
  std::thread ta([&] { sa = SendAll(a[0], a_to_b.data(), kTotal); });
  std::thread tb([&] { sb = SendAll(b[1], b_to_a.data(), kTotal); });
  std::thread ua([&] { seen_a = RecvHash(a[0], kTotal, &rca); });
  std::thread ub([&] { seen_b = RecvHash(b[1], kTotal, &rcb); });

  go2cpp::ErrorPtr err;
  const bool run_ok = pipe.Run(&err);
  ta.join(); tb.join(); ua.join(); ub.join();
  const r::PipeStats st = pipe.Stats();
  ::close(a[0]); ::close(b[1]);

  const Digest want_ab = h::Sha256::Digest(h::AsBytes(a_to_b.data(), kTotal));
  const Digest want_ba = h::Sha256::Digest(h::AsBytes(b_to_a.data(), kTotal));
  const bool ab_bytes = st.a_to_b == kTotal, ba_bytes = st.b_to_a == kTotal;
  const bool ab_hash = seen_b == want_ab, ba_hash = seen_a == want_ba;
  const bool pass = run_ok && sa && sb && rca && rcb && ab_bytes && ba_bytes && ab_hash && ba_hash;
  std::printf("relay_pipe: Run()=%s, a_to_b=%llu, b_to_a=%llu, dropped=%llu, timeouts=%llu\n",
              run_ok ? "true" : "false", (unsigned long long)st.a_to_b,
              (unsigned long long)st.b_to_a, (unsigned long long)st.dropped,
              (unsigned long long)st.timeouts);
  std::printf("[%s] A->B 字节数 %llu == %zu, 两端 SHA-256 一致=%s\n",
              (ab_bytes && ab_hash) ? "PASS" : "FAIL", (unsigned long long)st.a_to_b,
              kTotal, ab_hash ? "true" : "false");
  std::printf("[%s] B->A 字节数 %llu == %zu, 两端 SHA-256 一致=%s\n",
              (ba_bytes && ba_hash) ? "PASS" : "FAIL", (unsigned long long)st.b_to_a,
              kTotal, ba_hash ? "true" : "false");
  if (!run_ok && err) std::printf("relay_pipe: Run 错误: %s\n", err->Message().c_str());
  return pass ? 0 : 1;
}
