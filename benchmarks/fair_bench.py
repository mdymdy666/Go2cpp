"""公平压测驱动：稳态窗口采样服务端 CPU，多客户端随机负载。

用法（在仓库根目录，先构建 build-work 与压测程序）：
  python3 benchmarks/fair_bench.py --model gmp --workers 12 \
      --conns 200 --clients 2 --depth 1 --rate 0 --mixed \
      --body-min 64 --body-max 4096 --warmup-ms 2000 --duration-ms 10000
"""

import json
import os
import shlex
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build-gmp-vs-thread/gmp_vs_epoll_fair_benchmark"
CLK_TCK = os.sysconf("SC_CLK_TCK")

def arg(name, default, argv):
    for index, item in enumerate(argv):
        if item == name and index + 1 < len(argv):
            return argv[index + 1]
    return default


def has_flag(name, argv):
    return name in argv


def read_stat(pid):
    text = Path(f"/proc/{pid}/stat").read_text()
    fields = text.rsplit(") ", 1)[1].split()
    return int(fields[11]), int(fields[12])  # utime, stime (ticks)


def read_status(pid):
    values = {}
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith(("Threads:", "VmRSS:", "VmSize:")):
                key, _, value = line.partition(":")
                values[key] = value.strip()
    except OSError:
        pass
    return values


def main() -> int:
    argv = sys.argv[1:]
    model = arg("--model", "gmp", argv)
    workers = arg("--workers", "12", argv)
    conns = arg("--conns", "64", argv)
    clients = int(arg("--clients", "2", argv))
    depth = arg("--depth", "1", argv)
    rate = arg("--rate", "0", argv)
    body_min = arg("--body-min", "64", argv)
    body_max = arg("--body-max", "4096", argv)
    warmup_ms = arg("--warmup-ms", "2000", argv)
    duration_ms = arg("--duration-ms", "10000", argv)
    seed = arg("--seed", "1", argv)
    mixed = has_flag("--mixed", argv)

    server_bin = Path(arg("--server-bin", str(BIN), argv))
    client_bin = Path(arg("--client-bin", str(BIN), argv))
    server_cmd_override = arg("--server-cmd", "", argv)

    if not client_bin.is_file():
        print(f"缺少客户端二进制: {client_bin}")
        return 1
    if not server_cmd_override and not server_bin.is_file():
        print(f"缺少服务端二进制: {server_bin}")
        return 1

    if server_cmd_override:
        server_argv = shlex.split(server_cmd_override)
    else:
        server_argv = [str(server_bin), "server", "--model", model,
                       "--port", "0", "--workers", workers]
    server = subprocess.Popen(
        server_argv,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
    )
    port = 0
    server_pid = 0
    deadline = time.time() + 20
    while time.time() < deadline:
        line = server.stdout.readline()
        if not line:
            break
        if line.startswith("READY"):
            for token in line.split():
                if token.startswith("port="):
                    port = int(token.split("=", 1)[1])
                elif token.startswith("pid="):
                    server_pid = int(token.split("=", 1)[1])
            break
    if port == 0 or server_pid == 0:
        print("服务端未就绪")
        server.kill()
        return 1
    print(f"[server] model={model} workers={workers} pid={server_pid} port={port}", flush=True)

    events = [threading.Event() for _ in range(clients)]
    results = [[] for _ in range(clients)]
    procs = []

    def reader(index, proc):
        for line in proc.stdout:
            if line.startswith("MEASURE_START"):
                events[index].set()
            elif line.startswith("RESULT"):
                try:
                    results[index].append(json.loads(line[len("RESULT "):]))
                except json.JSONDecodeError:
                    pass

    client_cmd = [str(client_bin), "client", "--port", str(port), "--conns", conns,
                  "--depth", depth, "--rate", rate,
                  "--body-min", body_min, "--body-max", body_max,
                  "--warmup-ms", warmup_ms, "--duration-ms", duration_ms]
    if mixed:
        client_cmd.append("--mixed")

    threads = []
    for index in range(clients):
        proc = subprocess.Popen(
            client_cmd + ["--seed", str(int(seed) + index)],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
        )
        procs.append(proc)
        thread = threading.Thread(target=reader, args=(index, proc), daemon=True)
        thread.start()
        threads.append(thread)
        time.sleep(0.05)

    # 等所有客户端进入测量窗口，再开始采样服务端 CPU，避免把 warmup 摊进去。
    for event in events:
        event.wait(timeout=30)
    stat_start = read_stat(server_pid)
    time_start = time.time()

    for proc in procs:
        proc.wait(timeout=120)
    for thread in threads:
        thread.join(timeout=5)
    stat_end = read_stat(server_pid)
    time_end = time.time()
    status = read_status(server_pid)

    total_qps = sum(item["qps"] for group in results for item in group)
    total_completed = sum(item["completed"] for group in results for item in group)
    total_errors = sum(item["errors"] for group in results for item in group)
    p50s = sorted(item["p50_ns"] for group in results for item in group)
    p99s = sorted(item["p99_ns"] for group in results for item in group)
    p999s = sorted(item["p999_ns"] for group in results for item in group)
    if not p50s:
        print("客户端没有产出 RESULT")
        server.kill()
        return 1

    span = max(time_end - time_start, 1e-6)
    user_s = (stat_end[0] - stat_start[0]) / CLK_TCK
    sys_s = (stat_end[1] - stat_start[1]) / CLK_TCK
    user_us_per_req = user_s * 1e6 / max(total_completed, 1)
    sys_us_per_req = sys_s * 1e6 / max(total_completed, 1)
    cpu_pct = (user_s + sys_s) / span * 100.0

    for group in results:
        for item in group:
            print(f"[client] qps={item['qps']:.0f} p50={item['p50_ns']/1e6:.3f}ms "
                  f"p99={item['p99_ns']/1e6:.3f}ms p999={item['p999_ns']/1e6:.3f}ms "
                  f"completed={item['completed']} errors={item['errors']}", flush=True)
    print(
        f"[window] span={span:.2f}s completed={total_completed} errors={total_errors} "
        f"qps={total_completed/span:.0f} threads={status.get('Threads')} rss={status.get('VmRSS')}",
        flush=True,
    )
    print(
        f"[server] model={model} workers={workers} cpu={cpu_pct:.1f}% "
        f"user_us_per_req={user_us_per_req:.1f} sys_us_per_req={sys_us_per_req:.1f} "
        f"cpu_us_per_req={user_us_per_req + sys_us_per_req:.1f}",
        flush=True,
    )
    print(
        f"[latency] p50={p50s[len(p50s)//2]/1e6:.3f}ms p99={p99s[len(p99s)//2]/1e6:.3f}ms "
        f"p999={p999s[len(p999s)//2]/1e6:.3f}ms",
        flush=True,
    )

    server.send_signal(signal.SIGTERM)
    try:
        server.wait(timeout=20)
    except subprocess.TimeoutExpired:
        server.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
