#!/bin/bash
# ============================================================================
# go2cpp_commit.sh —— 双仓库（WSL ↔ Windows）同步 + 提交 + 推送
#
# 为什么需要这个脚本：
#   /UserData/CodexWorkSpace/Go2Cpp（WSL）与 E:\CodexWorkspace\Go2Cpp（Windows）
#   是两个**独立的 git 仓库副本**。WSL 内无法访问 GitHub（DNS 解析成 ::1），
#   必须在 Windows 侧 push。但同步时又必须把 .git 一起同步过去，否则 Windows
#   推的是旧提交（表现为 "Everything up-to-date" 但远端 HEAD 没变 —— 最容易
#   误判为"推送成功"的假象）。
#
# 三个踩过的坑（本脚本已全部规避）：
#   1. rsync -a 会传权限位，导致 git 把几十个文件标成 M（内容其实零变化）
#      → 用 -rlD（不含 -p）
#   2. rsync 同步 .git 会覆盖 .git/config，core.fileMode 被重置回 true
#      → 同步 .git 之后必须重置
#   3. 为修坑 2 而排除 .git，反而让 WSL 的提交推不上去
#      → .git 必须同步，只是要在之后重置 fileMode
#
# 用法：
#   ./scripts/go2cpp_commit.sh <commit-msg-file> [file1 file2 ...]
#
#   <commit-msg-file>  提交消息文件（首行 subject，空行，然后 body）
#   其余参数           要 git add 的路径（相对仓库根）；不给则用 -u 只提交已跟踪文件的改动
#
# 退出码：0 表示提交+推送+远端核对全部成功。
# ============================================================================
set -uo pipefail

WSL_REPO=/UserData/CodexWorkSpace/Go2Cpp
WIN_REPO=/mnt/e/CodexWorkspace/Go2Cpp
BRANCH="${GO2CPP_BRANCH:-feature/gmp-debug-module}"

if [ $# -lt 1 ]; then
  echo "用法: $0 <commit-msg-file> [files...]" >&2
  exit 2
fi

MSG_FILE="$1"
shift

if [ ! -f "$MSG_FILE" ]; then
  echo "错误: 提交消息文件不存在: $MSG_FILE" >&2
  exit 2
fi

echo "=== [1/7] 在 WSL 侧提交 ==="
cd "$WSL_REPO" || { echo "错误: 无法进入 $WSL_REPO" >&2; exit 1; }

if [ $# -gt 0 ]; then
  git add -- "$@" || { echo "错误: git add 失败" >&2; exit 1; }
else
  git add -u || { echo "错误: git add -u 失败" >&2; exit 1; }
fi

git status --short
if ! git diff --cached --quiet; then
  git commit -F "$MSG_FILE" || { echo "错误: git commit 失败" >&2; exit 1; }
else
  echo "（没有待提交的改动，跳过 commit）"
fi
WSL_HEAD=$(git rev-parse --short HEAD)
echo "WSL HEAD: $WSL_HEAD"

echo
echo "=== [2/7] 同步工作树到 Windows（排除 .git）==="
rsync -rlD --delete \
  --exclude '.git/' \
  --exclude 'build-quality/' --exclude 'build-bench/' --exclude 'build-profile/' \
  --exclude '.dsh_tmp/' --exclude 'app.ini' \
  "$WSL_REPO"/ "$WIN_REPO"/ || { echo "错误: 工作树同步失败" >&2; exit 1; }

echo
echo "=== [3/7] 同步 .git（否则 Windows 不认识刚才的提交）==="
# 注意：这一步会覆盖 .git/config，所以下一步必须重置 fileMode。
rsync -rlD --delete "$WSL_REPO"/.git/ "$WIN_REPO"/.git/ \
  || { echo "错误: .git 同步失败" >&2; exit 1; }

echo
echo "=== [4/7] 重置 core.fileMode 并刷新索引 ==="
cd "$WIN_REPO" || { echo "错误: 无法进入 $WIN_REPO" >&2; exit 1; }
git config core.fileMode false
git update-index --refresh >/dev/null 2>&1 || true
WIN_HEAD=$(git rev-parse --short HEAD)
echo "Windows HEAD: $WIN_HEAD"
if [ "$WSL_HEAD" != "$WIN_HEAD" ]; then
  echo "错误: 两侧 HEAD 不一致（WSL=$WSL_HEAD Windows=$WIN_HEAD），停止推送" >&2
  exit 1
fi

echo
echo "=== [5/7] 推送 ==="
if ! git push origin "$BRANCH"; then
  echo "错误: git push 失败" >&2
  exit 1
fi

echo
echo "=== [6/7] 核对远端 HEAD（不要只看 push 的返回码）==="
REMOTE_HEAD=$(git rev-parse --short "origin/$BRANCH" 2>/dev/null || echo "unknown")
echo "远端 HEAD: $REMOTE_HEAD"
if [ "$REMOTE_HEAD" != "$WSL_HEAD" ]; then
  echo "错误: 远端 HEAD ($REMOTE_HEAD) 与本地 ($WSL_HEAD) 不一致 —— 推送没有真正生效" >&2
  exit 1
fi

echo
echo "=== [7/7] 工作树状态 ==="
git status --short || true
echo
echo "✅ 完成：$WSL_HEAD 已推送到 origin/$BRANCH"
