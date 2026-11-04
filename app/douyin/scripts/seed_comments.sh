#!/usr/bin/env bash
# 给云端抖音 20 条视频生成评论（30 用户里挑 15 个当评论者）。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
GW="http://127.0.0.1:28080"
PUB="http://82.157.147.127:8443/douyin"

say() { printf '[comments] %s\n' "$*"; }
token_of() { sed -n 's/.*"token":"\([^"]*\)".*/\1/p' | head -1; }

say "1) 登录 15 个评论者"
TOKENS=()
for i in $(seq 1 15); do
  name=$(printf 'user%04d' "$i")
  tok=$(curl -s --max-time 5 -X POST "$GW/api/user/login" -H 'Content-Type: application/json' \
    -d "{\"email\":\"$name@douyin.local\",\"password\":\"$name\"}" | token_of)
  [[ -n "$tok" ]] && TOKENS+=("$tok")
done
say "  登录成功 ${#TOKENS[@]}/15"

say "2) 拉取视频列表"
FEED=$(curl -s --max-time 10 "$GW/api/feed?limit=100")
VIDS=$(echo "$FEED" | grep -o '"video":{"id":[0-9]*' | grep -o '[0-9]*$')
say "  视频数：$(echo "$VIDS" | wc -l)"

POOL=(
  "太美了，收藏了" "求详细攻略！" "这画面质感真好" "在哪儿拍的呀"
  "背景音乐叫什么" "看完心情都好了" "第一次见这种" "支持一下"
  "up 主更新太勤了" "这个我可以学" "已转发给朋友" "细节满分"
  "看完饿了" "太治愈了" "什么时候出下期" "这手艺绝了"
  "路过点个赞" "真实的生活气" "镜头很稳" "配色舒服"
)

say "3) 给每条视频发 3-5 条评论"
total=0
for vid in $VIDS; do
  n=$(( (vid % 3) + 3 ))
  for k in $(seq 1 "$n"); do
    ti=$(( (vid * 7 + k * 11) % ${#TOKENS[@]} ))
    pi=$(( (vid * 3 + k * 5) % ${#POOL[@]} ))
    tok="${TOKENS[$ti]}"
    text="${POOL[$pi]}"
    body=$(curl -s --max-time 5 -X POST "$GW/api/comment" \
      -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
      -d "{\"video_id\":$vid,\"content\":\"$text\"}")
    echo "$body" | grep -q '"ok":true' && total=$((total + 1))
  done
done
say "  评论成功 $total 条"

say "4) 验证（取一条视频的评论）"
FIRST=$(echo "$VIDS" | head -1)
curl -s --max-time 10 "$GW/api/comment/$FIRST?limit=10" | head -c 500; echo
say "  公网 feed 首条评论数："
curl -s --max-time 10 "$PUB/api/feed?limit=1" | grep -o '"comment_count":[0-9]*' | head -1
say "完成"
