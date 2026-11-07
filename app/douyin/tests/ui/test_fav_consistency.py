"""收藏一致性：收藏页 vs 主页「收藏」tab 必须显示同一批。"""
import pytest
from playwright.sync_api import Page

BASE = "http://127.0.0.1:28080"


def _login(page: Page, email="user0002@douyin.local", pwd="user0002"):
    page.goto(BASE + "/", wait_until="networkidle")
    page.evaluate(
        """async (c) => {
            const r = await fetch('/api/user/login', {
                method:'POST', headers:{'Content-Type':'application/json'},
                body: JSON.stringify(c)
            });
            const j = await r.json();
            if (j.token) localStorage.setItem('douyin_token', j.token);
            if (j.user) localStorage.setItem('douyin_user', JSON.stringify(j.user));
            return j;
        }""",
        {"email": email, "password": pwd},
    )
    page.wait_for_timeout(800)


def test_favorites_visible_on_profile(page: Page):
    _login(page)
    page.goto(BASE + "/", wait_until="networkidle")
    page.wait_for_timeout(1000)

    # 幂等：先查当前收藏，只对「未收藏」的 id 调 POST（该接口是切换语义）
    out = page.evaluate(
        """async () => {
            const t = localStorage.getItem('douyin_token');
            const before = await (await fetch('/api/video/favorites?limit=100', {
                headers:{'Authorization':'Bearer '+t}
            })).json();
            const have = new Set((before.items || []).map(x => x.id));
            for (const id of [1, 2]) {
                if (!have.has(id)) {
                    await fetch('/api/video/' + id + '/favorite', {
                        method:'POST',
                        headers:{'Content-Type':'application/json','Authorization':'Bearer '+t},
                        body: '{}'
                    });
                }
            }
            const after = await (await fetch('/api/video/favorites?limit=100', {
                headers:{'Authorization':'Bearer '+t}
            })).json();
            return {before: have.size, after: (after.items||[]).length};
        }"""
    )
    print("收藏数（前/后）:", out)

    prof = page.evaluate(
        """async () => {
            const t = localStorage.getItem('douyin_token');
            const u = JSON.parse(localStorage.getItem('douyin_user') || 'null');
            const r = await fetch('/api/user/' + u.id + '/favorites?limit=100', {
                headers:{'Authorization':'Bearer '+t}
            });
            return {status: r.status, body: (await r.text()).slice(0, 300)};
        }"""
    )
    print("主页收藏接口:", prof)

    assert out["after"] >= 1, f"收藏页为空: {out}"
    assert prof["status"] == 200, f"主页接口异常: {prof}"
    assert '"items":[]' not in prof["body"], f"主页收藏为空但收藏页有 {out['after']} 条: {prof['body']}"
