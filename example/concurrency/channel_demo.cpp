#include "go2cpp/runtime.hpp"

#include <iostream>

/**
 * @brief Channel 简单版：无缓冲交接、有缓冲队列、关闭后退出。
 * @details 三个互相独立的最小片段：
 *          ① 无缓冲 Channel —— 发送方要等到接收方取走才算完成（同步交接）；
 *          ② 有缓冲 Channel —— 容量未满时发送立即返回；
 *          ③ Close 之后继续 Recv —— 返回 Closed()，循环按 Go 的 range 语义退出。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;

    Scheduler scheduler(2);
    scheduler.start();
    bool ok = true;

    // ① 无缓冲：发送与接收必须"对上"才算完成
    std::clog << "[channel][场景1] 无缓冲 Channel 的同步交接\n";
    auto unbuffered = MakeChannel<int>(0);
    auto sender = go(scheduler, [unbuffered] { (void)unbuffered->Send(42); });
    if (const auto got = unbuffered->Recv()) {
        std::cout << "  收到: " << *got.value << '\n';
        ok = ok && (*got.value == 42);
    } else {
        ok = false;
    }
    sender->wait();

    // ② 有缓冲：容量 3，前三次发送都不阻塞
    std::clog << "[channel][场景2] 有缓冲 Channel\n";
    auto buffered = MakeChannel<int>(3);
    for (int i = 1; i <= 3; ++i) {
        ok = ok && static_cast<bool>(buffered->Send(i));
    }
    for (int i = 1; i <= 3; ++i) {
        if (const auto got = buffered->Recv()) {
            std::cout << "  取回: " << *got.value << '\n';
            ok = ok && (*got.value == i);
        } else {
            ok = false;
        }
    }

    // ③ 关闭：Recv 返回 Closed()，用 Go 的 range 语义退出循环
    std::clog << "[channel][场景3] 关闭后按 range 语义退出\n";
    auto closing = MakeChannel<int>(2);
    (void)closing->Send(7);
    (void)closing->Send(8);
    closing->Close();
    int sum = 0;
    while (const auto got = closing->Recv()) {
        sum += *got.value;
    }
    std::cout << "  关闭前累计: " << sum << "（预期 15）\n";
    ok = ok && (sum == 15);

    scheduler.shutdown();
    std::clog << (ok ? "[channel] 全部通过\n" : "[channel] 有失败\n");
    return ok ? 0 : 1;
}
