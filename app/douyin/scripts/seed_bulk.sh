#!/usr/bin/env bash
# 批量造数：1000 用户（user0001..user1000，密码=用户名）+ admin/123456 + 1000 视频。
# 串行写（SQLite 单写者），约 20–40 秒。
set -uo pipefail
GW="${GW:-http://127.0.0.1:28080}"
API="$GW/api"
USERS="${USERS:-1000}"
VIDEOS="${VIDEOS:-1000}"

say() { printf '[bulk] %s\n' "$*"; }

post() { # path token json
  local path="$1" token="$2" body="$3"
  if [[ -n "$token" ]]; then
    curl -s --max-time 5 -X POST "$API$path" \
      -H "Authorization: Bearer $token" -H 'Content-Type: application/json' -d "$body"
  else
    curl -s --max-time 5 -X POST "$API$path" \
      -H 'Content-Type: application/json' -d "$body"
  fi
}

token_of() { sed -n 's/.*"token":"\([^"]*\)".*/\1/p' | head -1; }

say "注册 admin（密码 123456）"
ADMIN_TOKEN=$(post "/user/register" "" '{"name":"admin","email":"admin@demo.local","password":"123456"}' | token_of)
if [[ -z "$ADMIN_TOKEN" ]]; then
  ADMIN_TOKEN=$(post "/user/login" "" '{"email":"admin@demo.local","password":"123456"}' | token_of)
fi
say "  admin token: ${ADMIN_TOKEN:0:12}…"

say "注册 $USERS 个用户（user0001..user$(printf '%04d' "$USERS")，密码=用户名）"
ok=0; skip=0; fail=0
for i in $(seq 1 "$USERS"); do
  name=$(printf 'user%04d' "$i")
  body=$(post "/user/register" "" "{\"name\":\"$name\",\"email\":\"$name@demo.local\",\"password\":\"$name\"}")
  if echo "$body" | grep -q '"ok":true'; then ok=$((ok+1));
  elif echo "$body" | grep -q '"code":409'; then skip=$((skip+1));
  else fail=$((fail+1)); fi
  if (( i % 200 == 0 )); then say "  进度 $i/$USERS（ok=$ok 已存在=$skip 失败=$fail）"; fi
done
say "  用户完成：新增 $ok，已存在 $skip，失败 $fail"

say "登录并发布 $VIDEOS 条视频（随机作者）"
tokens=()
for i in $(seq 1 50); do
  name=$(printf 'user%04d' "$(( (i * 19) % USERS + 1 ))")
  t=$(post "/user/login" "" "{\"email\":\"$name@demo.local\",\"password\":\"$name\"}" | token_of)
  [[ -n "$t" ]] && tokens+=("$t")
done
if [[ ${#tokens[@]} -eq 0 ]]; then
  say "没有可用 token，退出"; exit 1
fi
say "  可用作者 token：${#tokens[@]} 个"

pub=0
for i in $(seq 1 "$VIDEOS"); do
  t="${tokens[$(( (i * 7) % ${#tokens[@]} ))]}"
  title=$(printf '视频 #%04d' "$i")
  url="videos/$(( (i % 3) + 1 )).mp4"
  body=$(post "/video" "$t" "{\"title\":\"$title\",\"url\":\"$url\"}")
  if echo "$body" | grep -q '"ok":true'; then pub=$((pub+1)); fi
  if (( i % 200 == 0 )); then say "  视频进度 $i/$VIDEOS（已发布 $pub）"; fi
done
say "  视频完成：发布 $pub/$VIDEOS"

say "校验"
curl -s --max-time 5 "$API/feed?limit=2" | head -c 200; echo
say "完成"
