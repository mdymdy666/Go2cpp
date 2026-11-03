#!/usr/bin/env bash
# 公网穿透对照实验：公网 200 -> 停 client -> 公网 502 + 本机 200 -> 起 client -> 公网 200
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"      # app/douyin
REPO="$(cd "$ROOT/../.." && pwd)"                             # 仓库根
VPS="${VPS:-82.157.147.127}"
PUB="http://$VPS:8443/douyin/api/feed?limit=1"
LOC="http://127.0.0.1:28080/api/feed?limit=1"
CLIENT="${TUNNEL_BIN:-$REPO/.build-bento-wsl/tunnel-client}"
[[ -x "$CLIENT" ]] || CLIENT="/tmp/build-tunnel/tunnel-client"

say() { printf '[demo] %s\n' "$*"; }
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$1"; }

say "1) 当前隧道状态"
curl -s --max-time 8 "http://$VPS:8443/__tunnel__/status" | head -c 300; echo
echo "   公网 = $(code "$PUB")   本机 = $(code "$LOC")"

say "2) 停掉本机 tunnel-client"
pid=$(pgrep -x tunnel-client | head -1 || true)
if [[ -n "${pid:-}" ]]; then kill "$pid" 2>/dev/null; fi
sleep 3
echo "   公网 = $(code "$PUB")   ← 应为 502/000（隧道断）"
echo "   本机 = $(code "$LOC")   ← 应为 200（本地服务正常）"

say "3) 重启 tunnel-client（$CLIENT）"
if [[ ! -x "$CLIENT" ]]; then
  say "找不到 tunnel-client：$CLIENT"
  exit 1
fi
setsid nohup "$CLIENT" --server "$VPS:7443" --local 28080 \
  --subdomain douyin --token MyShop2026 >/tmp/tunnel_douyin_demo.log 2>&1 < /dev/null &
sleep 6
echo "   公网 = $(code "$PUB")   ← 应恢复 200"
curl -s --max-time 8 "http://$VPS:8443/__tunnel__/status" | head -c 300; echo
say "完成：公网断 / 本机通 / 公网恢复 三步对照即为公网穿透证据"
