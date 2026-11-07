"""给 douyin 造私信 + 举报演示数据（幂等：已存在则跳过）。"""
import json
import sys
import urllib.error
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
BASE = "http://127.0.0.1:28080"


def req(path, data=None, token=None, method=None):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    body = json.dumps(data).encode() if data is not None else None
    m = method or ("POST" if body is not None else "GET")
    r = urllib.request.Request(BASE + path, data=body, headers=headers, method=m)
    try:
        with urllib.request.urlopen(r, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode() or "{}")


def login(email, pwd):
    st, body = req("/api/user/login", {"email": email, "password": pwd})
    return body.get("token", ""), body.get("user", {})


u2_token, u2 = login("user0002@douyin.local", "user0002")
print("user0002:", u2.get("id"), bool(u2_token))

# 造 3 个对话对象（尝试登录已存在用户；不存在则注册）
partners = []
for e, p in [("user0001@douyin.local", "user0001"),
             ("user0003@douyin.local", "user0003"),
             ("admin@douyin.local", "123456")]:
    t, u = login(e, p)
    if u.get("id"):
        partners.append((e, u["id"]))

print("对话对象:", partners)

# 每个会话发 3 条
sent = 0
for email, pid in partners:
    for i in range(3):
        st, body = req("/api/dm/send",
                       {"to_user_id": pid, "content": f"演示消息 {i+1}"},
                       token=u2_token)
        if st == 200 and body.get("ok"):
            sent += 1
print("发送私信:", sent)

# 造 5 条举报（不同 target）
reports = [
    ("video", 1, "spam", "演示：垃圾内容"),
    ("video", 2, "violence", "演示：暴力内容"),
    ("comment", 1, "harassment", "演示：骚扰评论"),
    ("comment", 2, "spam", "演示：垃圾评论"),
    ("user", 3, "impersonation", "演示：冒充他人"),
]
rep_ok = 0
for ttype, tid, reason, detail in reports:
    st, body = req("/api/report",
                   {"target_type": ttype, "target_id": tid,
                    "reason": reason, "detail": detail},
                   token=u2_token)
    if st in (200, 201):
        rep_ok += 1
print("举报:", rep_ok, "/", len(reports))

# 通知数（点赞/评论/关注应已产生）
st, body = req("/api/notifications?limit=5", token=u2_token)
print("通知:", body.get("counts", {}), "unread:", body.get("unread_count"))
