#!/usr/bin/env bash
# 给抖音 demo 造演示数据（经网关 28080，可指定 GW 环境变量）。
set -uo pipefail
GW="${GW:-http://127.0.0.1:28080}"
API="$GW/api"

say() { printf '[seed] %s\n' "$*"; }

json_str() { sed -n "s/.*\"$1\":\"\\([^\"]*\\)\".*/\\1/p" | head -1; }
json_int() { sed -n "s/.*\"$1\":\\([0-9]*\\).*/\\1/p" | head -1; }

register() { # name email -> token
  local name="$1" email="$2"
  local body
  body=$(curl -s --max-time 5 -X POST "$API/user/register" \
    -H 'Content-Type: application/json' \
    -d "{\"name\":\"$name\",\"email\":\"$email\",\"password\":\"pass1234\"}")
  if echo "$body" | grep -q '"ok":true'; then
    echo "$body" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p' | head -1
  else
    curl -s --max-time 5 -X POST "$API/user/login" \
      -H 'Content-Type: application/json' \
      -d "{\"email\":\"$email\",\"password\":\"pass1234\"}" \
      | sed -n 's/.*"token":"\([^"]*\)".*/\1/p' | head -1
  fi
}

publish() { # token title url -> video_id
  local token="$1" title="$2" url="$3"
  curl -s --max-time 5 -X POST "$API/video" \
    -H "Authorization: Bearer $token" -H 'Content-Type: application/json' \
    -d "{\"title\":\"$title\",\"url\":\"$url\"}" \
    | sed -n 's/.*"video":{"id":\([0-9]*\).*/\1/p' | head -1
}

like() { # token video_id
  curl -s --max-time 5 -X POST "$API/like" \
    -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
    -d "{\"video_id\":$2}" >/dev/null
}

comment() { # token video_id content
  curl -s --max-time 5 -X POST "$API/comment" \
    -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
    -d "{\"video_id\":$2,\"content\":\"$3\"}" >/dev/null
}

say "注册 3 个用户"
T1=$(register "阿三" "asan@demo.local")
T2=$(register "小鱼" "xiaoyu@demo.local")
T3=$(register "老王" "laowang@demo.local")
[[ -z "$T1" || -z "$T2" || -z "$T3" ]] && { say "注册失败（检查 $API）"; exit 1; }
say "  token: ${T1:0:12}… ${T2:0:12}… ${T3:0:12}…"

publish_one() { # token title
  local token="$1" title="$2"
  local url="videos/$(( (RANDOM % 3) + 1 )).mp4"
  local id
  id=$(publish "$token" "$title" "$url")
  if [[ -n "$id" ]]; then
    say "  发布 #$id $title"
    echo "$id"
  fi
}

say "发布 6 条视频"
V1=$(publish_one "$T1" "周末去爬山，云海绝了")
V2=$(publish_one "$T1" "手工做的木雕小狐狸")
V3=$(publish_one "$T2" "凌晨四点的面包房")
V4=$(publish_one "$T2" "猫猫第一次见雪")
V5=$(publish_one "$T3" "老式收音机修复全过程")
V6=$(publish_one "$T3" "街角咖啡店的一天")

say "点赞与评论"
for pair in "$T2:$V1" "$T3:$V1" "$T1:$V3" "$T3:$V4" "$T1:$V5" "$T2:$V6"; do
  like "${pair%%:*}" "${pair##*:}"
done
comment "$T2" "$V1" "太美了，求路线"
comment "$T3" "$V1" "这云海我酸了"
comment "$T1" "$V3" "面包看着就香"
comment "$T2" "$V5" "手艺真好"

say "创建并开播一个直播间"
ROOM=$(curl -s --max-time 5 -X POST "$API/live/room" \
  -H "Authorization: Bearer $T1" -H 'Content-Type: application/json' \
  -d '{"title":"深夜电台：聊聊最近看的书"}' \
  | sed -n 's/.*"room":{"id":\([0-9]*\).*/\1/p' | head -1)
if [[ -n "$ROOM" ]]; then
  curl -s --max-time 5 -X POST "$API/live/room/$ROOM/start" \
    -H "Authorization: Bearer $T1" >/dev/null
  say "  直播间 #$ROOM 已开播"
fi

say "校验 feed"
curl -s --max-time 5 "$API/feed?limit=10" | head -c 300
echo
say "完成"
