#!/usr/bin/env python3
"""断线续传（Resume）验收：补发区间、resume_gap、边界、随机、并发与限流回归。

用法（由 accept_resume.sh 调用）：
  python3 accept_resume.py --port 29303 --gap-port 29304 --rate-port 29305

三个服务实例的配置由 shell 脚本给出：
  --port       resume-buffer=1000, rate-limit=0   （补发 / 压测 / 并发重连）
  --gap-port   resume-buffer=4,    rate-limit=0   （gap + 区间边界）
  --rate-port  resume-buffer=1000, rate-limit=5   （限流回归）
"""

import argparse
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ws_min_client import WsClient, WsError, ws_connect  # noqa: E402

PASS = 0
FAIL = 0
FAILED_NAMES = []


def check(condition, name):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  PASS {name}", flush=True)
    else:
        FAIL += 1
        FAILED_NAMES.append(name)
        print(f"  FAIL {name}", flush=True)


def connect(port, video_id, last_seq=None, token=None):
    path = f"/ws/danmaku?video_id={video_id}"
    if last_seq is not None:
        path += f"&last_seq={last_seq}"
    if token is not None:
        path += f"&token={token}"
    sock, rest = ws_connect("127.0.0.1", port, path)
    return WsClient(sock, rest)


def recv(ws, timeout=2.0):
    try:
        return ws.recv_json(timeout)
    except (WsError, OSError):
        return None


def recv_type(ws, wanted, timeout=2.0, limit=4000):
    """读到指定 type 的消息；超时返回 None。"""
    deadline = time.time() + timeout
    for _ in range(limit):
        remaining = deadline - time.time()
        if remaining <= 0:
            return None
        message = recv(ws, max(0.01, remaining))
        if message is None:
            continue
        if message.get("type") == wanted:
            return message
    return None


def drain(ws, timeout=0.4):
    """读完当前已到达的消息（静默 timeout 后结束）。"""
    messages = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        message = recv(ws, max(0.01, deadline - time.time()))
        if message is None:
            break
        messages.append(message)
    return messages


def recv_n(ws, wanted_type, count, timeout=5.0):
    """按序读 count 条指定 type 的消息；其他 type 跳过。"""
    messages = []
    deadline = time.time() + timeout
    while len(messages) < count and time.time() < deadline:
        message = recv(ws, max(0.01, deadline - time.time()))
        if message is None:
            continue
        if message.get("type") == wanted_type:
            messages.append(message)
    return messages


def send_danmaku(ws, text):
    ws.send_json({"type": "send", "text": text, "time_ms": 1000,
                  "color": 16711680})


def seqs(messages):
    return [message.get("seq") for message in messages]


# ---------------------------------------------------------------------------
# 场景 1：A 收 5 条后断线，B 再发 10 条，A 用 last_seq=5 重连 → 补发 6..15。
# ---------------------------------------------------------------------------
def scenario_replay(port):
    print("\n[1] 断线重连补发 6..15（缓冲 1000）", flush=True)
    a = connect(port, 1)
    b = connect(port, 1)
    drain(a, 0.3)  # 清掉 online 帧（不占 seq）
    for index in range(1, 6):
        send_danmaku(b, f"m{index}")
    first = recv_n(a, "danmaku", 5)
    check(len(first) == 5, "A 收到 5 条弹幕")
    check(seqs(first) == [1, 2, 3, 4, 5], "前 5 条 seq=1..5")
    a.close()
    time.sleep(0.3)  # 等 Leave 完成；online 不占 seq，不影响序号

    for index in range(6, 16):
        send_danmaku(b, f"m{index}")
    time.sleep(0.3)

    a2 = connect(port, 1, last_seq=5)
    replayed = recv_n(a2, "danmaku", 10)
    check(len(replayed) == 10, "重连补发 10 条")
    check(seqs(replayed) == list(range(6, 16)), "补发 seq=6..15")
    check([m.get("text") for m in replayed] == [f"m{i}" for i in range(6, 16)],
          "补发内容 = m6..m15（无重复/无丢失）")
    online = recv_type(a2, "online", timeout=2.0)
    check(online is not None, "补发后仍收到 online 实时帧")
    check("seq" not in (online or {}), "online 是瞬时帧、不带 seq")

    # 边界：last_seq=latest → 补发为空
    a3 = connect(port, 1, last_seq=15)
    empty = drain(a3, 0.6)
    check(not [m for m in empty if m.get("type") == "danmaku"],
          "last_seq=15（等于 latest）不补发弹幕")
    a3.close()

    # 不带 last_seq 的新连接：只收实时，不回放历史
    fresh = connect(port, 1)
    history = drain(fresh, 0.6)
    check(not [m for m in history if m.get("type") == "danmaku"],
          "新连接不带 last_seq 不回放历史")
    fresh.close()
    a2.close()
    b.close()

    # 超出当前 latest：服务重启后序号回退 → gap
    future = connect(port, 1, last_seq=99)
    gap = recv_type(future, "resume_gap", timeout=2.0)
    check(gap is not None and gap.get("earliest") == 1,
          "last_seq > latest → resume_gap earliest=1")
    future.close()


# ---------------------------------------------------------------------------
# 场景 2：缓冲 4，last_seq=0 太旧 → resume_gap；边界区间仍可补发。
# ---------------------------------------------------------------------------
def scenario_gap(gap_port):
    print("\n[2] 缓冲 4 的 gap 判定（last_seq=0 太旧）", flush=True)
    c = connect(gap_port, 2)
    for index in range(1, 9):
        send_danmaku(c, f"g{index}")
    time.sleep(0.4)
    drain(c, 0.2)
    c.close()
    time.sleep(0.2)

    d = connect(gap_port, 2, last_seq=0)
    gap = recv_type(d, "resume_gap", timeout=2.0)
    check(gap is not None, "last_seq=0 太旧 → 收到 resume_gap")
    check((gap or {}).get("earliest") == 5, "resume_gap.earliest=5（缓冲保留 5..8）")
    check((gap or {}).get("latest") == 8, "resume_gap.latest=8")
    d.close()

    e = connect(gap_port, 2, last_seq=4)
    replayed = recv_n(e, "danmaku", 4)
    check(seqs(replayed) == [5, 6, 7, 8], "last_seq=4 补发 5..8（窗口边界）")
    e.close()

    f = connect(gap_port, 2, last_seq=8)
    check(not [m for m in drain(f, 0.5) if m.get("type") == "danmaku"],
          "last_seq=8 无补发")
    f.close()


# ---------------------------------------------------------------------------
# 场景 3：随机 last_seq（0..12，含太旧/恰在窗口/等于/超前）。
# ---------------------------------------------------------------------------
def scenario_random(gap_port):
    print("\n[3] 随机 last_seq × 区间不变式（缓冲 4，房间已静默）", flush=True)
    rng = random.Random(20261104)
    candidates = list(range(0, 13))
    rng.shuffle(candidates)
    for last_seq in candidates:
        client = connect(gap_port, 2, last_seq=last_seq)
        expected_gap = last_seq < 4 or last_seq > 8
        if expected_gap:
            gap = recv_type(client, "resume_gap", timeout=1.5)
            check(gap is not None and gap.get("earliest") == 5,
                  f"last_seq={last_seq} → gap earliest=5")
        else:
            expected = list(range(max(last_seq + 1, 5), 9))
            replayed = recv_n(client, "danmaku", len(expected), timeout=1.5)
            check(seqs(replayed) == expected,
                  f"last_seq={last_seq} 补发 {expected}")
            check(recv_type(client, "online", timeout=1.0) is not None,
                  f"last_seq={last_seq} 补发后在线数帧正常")
        client.close()
        time.sleep(0.05)


# ---------------------------------------------------------------------------
# 场景 4：20 客户端并发压测 + 并发随机重连。
# ---------------------------------------------------------------------------
def scenario_concurrency(port):
    print("\n[4] 并发压测：20 客户端 × 200 条 + 并发随机重连", flush=True)
    sender = connect(port, 7)
    clients = [connect(port, 7) for _ in range(20)]
    time.sleep(0.3)
    for client in clients:
        drain(client, 0.05)

    total = 200
    begin = time.perf_counter()
    for index in range(1, total + 1):
        send_danmaku(sender, f"p{index}")
    received = 0
    expected = list(range(1, total + 1))
    for index, client in enumerate(clients):
        messages = recv_n(client, "danmaku", total, timeout=15.0)
        received += len(messages)
        if seqs(messages) != expected:
            check(False, f"客户端 {index} 收齐 1..200（实际 {len(messages)} 条）")
    elapsed = time.perf_counter() - begin
    check(received == total * len(clients),
          f"20 客户端共收 {received}/{total * len(clients)} 条且序号连续")
    print(f"  [压测] {received} 条 / {elapsed:.3f}s = {received / elapsed:.0f} 条/秒",
          flush=True)

    rng = random.Random(7)
    replayed_clients = []
    reconnect_begin = time.perf_counter()
    for client in clients[:10]:
        client.close()
    time.sleep(0.2)
    for index, client in enumerate(clients[:10]):
        last_seq = rng.randint(0, total + 3)
        resumed = connect(port, 7, last_seq=last_seq)
        replayed_clients.append((last_seq, resumed))
    for last_seq, resumed in replayed_clients:
        if last_seq > total:
            gap = recv_type(resumed, "resume_gap", timeout=2.0)
            check(gap is not None and gap.get("earliest") == 1,
                  f"并发重连 last_seq={last_seq} > latest → gap")
        else:
            expected_seq = list(range(last_seq + 1, total + 1))
            messages = recv_n(resumed, "danmaku", len(expected_seq), timeout=3.0)
            check(seqs(messages) == expected_seq,
                  f"并发重连 last_seq={last_seq} 补发 {len(expected_seq)} 条且连续")
        resumed.close()
    reconnect_elapsed = time.perf_counter() - reconnect_begin
    print(f"  [并发重连] 10 个随机 last_seq 校验耗时 {reconnect_elapsed:.3f}s",
          flush=True)
    for client in clients[10:]:
        client.close()
    sender.close()


# ---------------------------------------------------------------------------
# 场景 5：限流回归（5 条/10s）：第 6 条应为 429 error，与 resume 无关。
# ---------------------------------------------------------------------------
def scenario_rate_limit(rate_port):
    print("\n[5] 限流回归：第 6 条 429", flush=True)
    client = connect(rate_port, 9)
    drain(client, 0.2)
    for index in range(1, 7):
        send_danmaku(client, f"r{index}")
        time.sleep(0.05)
    error = recv_type(client, "error", timeout=2.0)
    check(error is not None and error.get("code") == 429,
          "第 6 条收到 error code=429")
    client.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--gap-port", type=int, required=True)
    parser.add_argument("--rate-port", type=int, required=True)
    args = parser.parse_args()

    print("== 断线续传（Resume）验收 ==", flush=True)
    scenario_replay(args.port)
    scenario_gap(args.gap_port)
    scenario_random(args.gap_port)
    scenario_concurrency(args.port)
    scenario_rate_limit(args.rate_port)

    print(f"\n== 结果：{PASS} PASS / {FAIL} FAIL ==", flush=True)
    if FAILED_NAMES:
        for name in FAILED_NAMES:
            print(f"  - {name}", flush=True)
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
