#!/usr/bin/env bash
# 云端抖音 demo：清空数据 -> 启动（7 进程 + 隧道）-> 注入 30 用户 + 20 条真实视频 -> 验证公网
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
GW="http://127.0.0.1:28080"
PUB="http://82.157.147.127:8443/douyin"

say() { printf '[cloud] %s\n' "$*"; }
token_of() { sed -n 's/.*"token":"\([^"]*\)".*/\1/p' | head -1; }

say "1) 停旧服务（若有）"
bash scripts/stop_public.sh 2>/dev/null || true
sleep 1
pkill -x gateway 2>/dev/null || true
for s in user_service video_service feed_service interaction_service live_service search_service; do pkill -x "$s" 2>/dev/null || true; done
pkill -x tunnel-client 2>/dev/null || true
sleep 1

say "2) 清空数据（DB + 上传文件 + 旧日志）"
rm -f data/douyin.db data/douyin.db-wal data/douyin.db-shm
rm -f web/videos/upload_*.mp4
rm -f logs/serve_*.log
ls -la data/ web/videos/ 2>/dev/null | head -12

say "3) 启动 7 进程 + 隧道"
bash scripts/serve_public.sh 2>&1 | tail -14

say "4) 注册 30 个用户"
TOKENS=()
for i in $(seq 1 30); do
  name=$(printf 'user%04d' "$i")
  body=$(curl -s --max-time 5 -X POST "$GW/api/user/register" -H 'Content-Type: application/json' \
    -d "{\"name\":\"$name\",\"email\":\"$name@douyin.local\",\"password\":\"$name\"}")
  tok=$(echo "$body" | token_of)
  if [[ -n "$tok" ]]; then
    TOKENS+=("$tok")
  fi
done
say "  注册成功 ${#TOKENS[@]}/30"

# admin
ADMIN=$(curl -s --max-time 5 -X POST "$GW/api/user/register" -H 'Content-Type: application/json' \
  -d '{"name":"admin","email":"admin@douyin.local","password":"123456"}' | token_of)
say "  admin token: ${ADMIN:0:10}…"

say "5) 发布 20 条真实内容视频（demo1-5 轮换 + 真实标题）"
TITLES=(
  "周末去爬山，云海绝了" "手工木雕小狐狸，花了三天" "凌晨四点的面包房"
  "猫猫第一次见雪，反应太可爱" "老式收音机修复全过程" "街角咖啡店的一天"
  "在大理住了半个月" "第一次做提拉米苏" "骑行 200 公里去看海"
  "阳台种菜第 30 天" "给爷爷修好了老钟" "凌晨的便利店"
  "雨天的窗台和猫" "自制辣椒酱翻车现场" "山里捡到的石头"
  "学吉他第 100 天" "早餐摊的烟火气" "海边日落延时"
  "旧书摊淘到宝" "楼下的流浪猫生宝宝了"
)
pub=0
for i in $(seq 1 20); do
  idx=$(( (i * 7) % ${#TOKENS[@]} ))
  tok="${TOKENS[$idx]}"
  title="${TITLES[$((i - 1))]}"
  vnum=$(( (i % 5) + 1 ))
  body=$(curl -s --max-time 5 -X POST "$GW/api/video" \
    -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
    -d "{\"title\":\"$title\",\"url\":\"videos/demo$vnum.mp4\"}")
  echo "$body" | grep -q '"ok":true' && pub=$((pub + 1))
done
say "  发布成功 $pub/20"

say "6) 验证公网"
curl -s --max-time 10 "$PUB/api/health/all" | head -c 200; echo
curl -s --max-time 10 "$PUB/api/feed?limit=3" | head -c 400; echo
curl -s -o /dev/null -w "  公网首页=%{http_code} 视频=%{http_code}\n" --max-time 10 "$PUB/" "$PUB/videos/demo1.mp4"
say "完成：$PUB"
