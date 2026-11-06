#!/usr/bin/env bash
# 媒体链接自检：库里每条视频 / 封面指向的文件是否真实存在。
#
# 为什么需要它（2026-11-06 用户视角审查）：
#   seed_cloud.sh 会把 20 条视频轮换指向 demo1-5.mp4，而 demo2/demo3 的转码
#   产物曾整批缺失。当时所有 accept_*.sh 都 PASS——因为 curl 只校验 JSON 与
#   状态码，没人打开首页看「视频加载失败」。这个脚本把「记录指向的文件」变成
#   可断言的验收项。
#
# 用法：scripts/check_media_links.sh [-q]
#   退出码 0=全部存在；1=有缺失（逐条打印 id/url）；2=数据库不存在。
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${DOUYIN_DB:-$ROOT/data/douyin.db}"
WEB="${DOUYIN_WEB_DIR:-$ROOT/web}"
QUIET=0
[[ "${1:-}" == "-q" ]] && QUIET=1

if [[ ! -f "$DB" ]]; then
  echo "[check_media_links] 数据库不存在：$DB" >&2
  exit 2
fi

python3 - "$DB" "$WEB" "$QUIET" <<'PY'
import os
import sqlite3
import sys

db_path, web_dir, quiet = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
rows = list(
    con.execute(
        "select id, url, coalesce(cover_url, ''), deleted_at from videos order by id"
    )
)

missing = []
for video_id, url, cover_url, deleted_at in rows:
    # 软删记录也检查：恢复时不该把死链一起恢复。
    for label, rel in (("url", url), ("cover", cover_url)):
        if not rel or rel.startswith("http://") or rel.startswith("https://"):
            continue
        path = os.path.join(web_dir, rel)
        if not os.path.exists(path):
            missing.append((video_id, label, rel, deleted_at))

total = len(rows)
print(f"[check_media_links] 视频记录 {total} 条，缺失 {len(missing)} 条")
if missing:
    for video_id, label, rel, deleted_at in missing:
        flag = "（已软删）" if deleted_at else ""
        print(f"  [MISS] id={video_id} {label}={rel}{flag}")
    if not quiet:
        print("提示：补齐文件后可用以下 SQL 恢复误删记录：")
        print("  update videos set deleted_at = null where id in (...);")
    sys.exit(1)
print("[check_media_links] OK：所有媒体文件都存在")
sys.exit(0)
PY
