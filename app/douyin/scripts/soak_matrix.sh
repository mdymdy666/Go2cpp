#!/usr/bin/env bash
# 精简回归矩阵：断线重连 / 点赞风暴 / slowloris
set -uo pipefail
cd /UserData/CodexWorkSpace/Go2cpp/app/douyin || exit 1

GW=http://127.0.0.1:28080
PUB=http://82.157.147.127:8443/douyin
PIDFILE=logs/serve_public.pids

tok() {
  curl -s -X POST "$GW/api/user/login" -H 'Content-Type: application/json' \
    -d "{\"email\":\"$1@douyin.local\",\"password\":\"$1\"}" \
    | python3 -c 'import sys,json; print(json.load(sys.stdin)["token"])'
}

echo "===== 场景 1：断线重连（kill -9 feed_service）====="
feed_pid=$(awk '$1=="feed_service"{print $2}' "$PIDFILE")
echo "feed_service pid=$feed_pid"
before=$(curl -s --max-time 5 "$GW/api/feed?limit=100" | grep -o '"video":{"id":' | wc -l)
echo "before down: feed=$before"
kill -9 "$feed_pid" 2>/dev/null
sleep 1
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$GW/api/feed?limit=1")
echo "after kill: code=$code (预期 502)"
start=$(date +%s%N)
DOUYIN_DB=data/douyin.db setsid nohup build/feed_service 29103 >>logs/serve_feed_service.log 2>&1 < /dev/null &
newpid=$!
for _ in $(seq 1 100); do
  curl -fsS --max-time 1 http://127.0.0.1:29103/health >/dev/null 2>&1 && break
  sleep 0.05
done
end=$(date +%s%N)
after=$(curl -s --max-time 5 "$GW/api/feed?limit=100" | grep -o '"video":{"id":' | wc -l)
echo "recovered in $(( (end - start) / 1000000 ))ms, feed=$after"
awk -v p="$newpid" '$1=="feed_service"{printf "feed_service %s 29103\n", p; next} {print}' "$PIDFILE" > "$PIDFILE.tmp" && mv "$PIDFILE.tmp" "$PIDFILE"

echo "===== 场景 2：点赞风暴（10 并发 x 20）====="
python3 - "$GW" <<'PY'
import json, sys, time, urllib.request

gw = sys.argv[1]

def login(name):
    req = urllib.request.Request(gw + "/api/user/login",
        data=json.dumps({"email": f"{name}@douyin.local", "password": name}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.load(r)["token"]

tokens = [login(f"user{i:04d}") for i in range(1, 11)]
vids = list(range(1, 21))
import threading
codes = []
lock = threading.Lock()

def worker(tok):
    for k in range(20):
        vid = vids[(hash(tok) + k) % len(vids)]
        req = urllib.request.Request(gw + "/api/like",
            data=json.dumps({"video_id": vid}).encode(),
            headers={"Content-Type": "application/json", "Authorization": "Bearer " + tok})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                c = r.status
        except urllib.error.HTTPError as e:
            c = e.code
        except Exception:
            c = 0
        with lock:
            codes.append(c)

ts = [threading.Thread(target=worker, args=(t,)) for t in tokens]
t0 = time.time()
for t in ts: t.start()
for t in ts: t.join()
el = time.time() - t0
ok = codes.count(200); conflict = codes.count(409); other = len(codes) - ok - conflict
print(f"likes: total={len(codes)} 200={ok} 409={conflict} other={other} elapsed={el*1000:.0f}ms qps={len(codes)/el:.0f}")
PY

echo "===== 场景 3：slowloris（4 条半开 + 1 个正常请求）====="
python3 - "$GW" <<'PY'
import socket, sys, time, urllib.request

gw = sys.argv[1]
host, port = gw.replace("http://", "").split(":")
port = int(port)
socks = []
for _ in range(4):
    s = socket.create_connection((host, port), timeout=3)
    s.sendall(b"GET /api/feed HTTP/1.1\r\nHost: x\r\n")  # 半个请求头
    socks.append(s)
t0 = time.time()
try:
    with urllib.request.urlopen(gw + "/health", timeout=15) as r:
        code = r.status
except Exception as e:
    code = type(e).__name__
el = (time.time() - t0) * 1000
print(f"slowloris: normal request code={code} elapsed={el:.0f}ms (预期 <=10000ms)")
for s in socks:
    try: s.close()
    except Exception: pass
PY

echo "===== 终态 ====="
curl -s --max-time 8 "$PUB/api/health/all" | head -c 160; echo
