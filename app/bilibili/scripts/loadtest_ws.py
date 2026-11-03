#!/usr/bin/env python3
"""弹幕广播压测：N 条连接同房间，连接 0 发 M 条，其余连接统计端到端延迟。

每条弹幕文本尾部带 `#<perf_counter>`，接收端用 `now - 时间戳` 得到「发送→收到」
毫秒延迟（含客户端排队，50 连接时受 Python GIL 影响，报告里如实标注）。

用法：python3 scripts/loadtest_ws.py --connections 50 --messages 100
"""

import argparse
import json
import statistics
import threading
import time

from ws_min_client import WsClient, WsError, ws_connect


def connect(port, video_id):
    sock, rest = ws_connect("127.0.0.1", port, f"/ws/danmaku?video_id={video_id}")
    return WsClient(sock, rest)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=29203)
    parser.add_argument("--video-id", type=int, default=1)
    parser.add_argument("--connections", type=int, default=50)
    parser.add_argument("--messages", type=int, default=100)
    parser.add_argument("--gap-ms", type=float, default=5.0)
    args = parser.parse_args()

    total = args.connections
    if total < 2:
        raise SystemExit("connections 至少 2")

    clients = [connect(args.port, args.video_id) for _ in range(total)]
    print(f"已连接 {total} 个 WS 连接（video_id={args.video_id}）", flush=True)
    # publisher 的接收队列会收到每次 join 的 online 广播，读到 count==total 即全部就绪。
    deadline = time.time() + 10
    while time.time() < deadline:
        message = clients[0].recv_json(0.5)
        if message and message.get("type") == "online" and message.get("count") == total:
            break
    print("房间 online 已到 " + str(total), flush=True)

    latencies = [[] for _ in range(total)]
    errors = []
    error_lock = threading.Lock()

    def subscriber(index):
        try:
            received = 0
            while received < args.messages:
                message = clients[index].recv_json(15.0)
                if message is None:
                    with error_lock:
                        errors.append(f"conn{index} 超时，只收到 {received}/{args.messages}")
                    return
                if message.get("type") != "danmaku":
                    continue
                text = message.get("text", "")
                if "#" not in text:
                    continue
                try:
                    sent_at = float(text.rsplit("#", 1)[1])
                except ValueError:
                    continue
                latencies[index].append((time.perf_counter() - sent_at) * 1000.0)
                received += 1
        except (WsError, OSError) as exc:
            with error_lock:
                errors.append(f"conn{index}: {exc}")

    threads = [threading.Thread(target=subscriber, args=(index,), daemon=True)
               for index in range(1, total)]
    for thread in threads:
        thread.start()
    time.sleep(0.3)

    publisher = clients[0]
    started = time.perf_counter()
    for sequence in range(args.messages):
        text = f"load-{sequence}#{time.perf_counter():.6f}"
        publisher.send_json({"type": "send", "text": text,
                             "time_ms": sequence * 1000})
        time.sleep(args.gap_ms / 1000.0)
    print(f"发布完成：{args.messages} 条，用时 {time.perf_counter() - started:.2f}s",
          flush=True)

    for thread in threads:
        thread.join(timeout=20)

    all_latencies = [value for per_connection in latencies for value in per_connection]
    if errors:
        print("错误:", flush=True)
        for error in errors[:10]:
            print("  " + error, flush=True)
    if not all_latencies:
        print("没有收到任何广播", flush=True)
        return 1

    all_latencies.sort()
    count = len(all_latencies)
    p50 = all_latencies[int(count * 0.50)]
    p99 = all_latencies[min(count - 1, int(count * 0.99))]
    p999 = all_latencies[min(count - 1, int(count * 0.999))]
    print(json.dumps({
        "connections": total,
        "messages_per_publisher": args.messages,
        "received_samples": count,
        "errors": len(errors),
        "min_ms": round(all_latencies[0], 3),
        "p50_ms": round(p50, 3),
        "p99_ms": round(p99, 3),
        "p999_ms": round(p999, 3),
        "max_ms": round(all_latencies[-1], 3),
        "mean_ms": round(statistics.fmean(all_latencies), 3),
    }, ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
