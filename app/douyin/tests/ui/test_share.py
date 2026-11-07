"""分享落地页 `/v/:id`：视频可播 / OG 标签 / 404。"""
from playwright.sync_api import Page, expect

from conftest import BASE


def test_share_video_playable(page: Page):
    """/v/1 视频真的能播（readyState >= 2），标题是视频标题。"""
    page.goto(BASE + "/v/1", wait_until="networkidle")
    page.wait_for_timeout(1200)
    video = page.locator("video").first
    expect(video).to_be_visible()
    state = video.evaluate(
        "el => ({readyState: el.readyState, error: el.error ? el.error.code : 0,"
        " src: (el.currentSrc || el.src || '').slice(-40)})")
    assert state["error"] == 0, f"分享页视频加载失败: {state}"
    assert state["readyState"] >= 2, f"分享页视频不可播: {state}"
    title = page.title()
    assert "Go2Cpp 抖音" in title and title != "Go2Cpp 抖音", f"标题异常: {title}"
    assert "评论" in page.inner_text("body"), "分享页没有评论区"


def test_share_og_tags(page: Page):
    """OG/Twitter 标签齐全，og:image 指向的图真的能加载。"""
    page.goto(BASE + "/v/1", wait_until="networkidle")
    meta = page.evaluate(
        """() => {
            const out = {};
            document.querySelectorAll('meta[property^="og:"], meta[name^="twitter:"]')
                .forEach(m => { out[m.getAttribute('property') || m.getAttribute('name')]
                    = m.getAttribute('content'); });
            return out;
        }"""
    )
    for key in ["og:title", "og:description", "og:url", "og:image", "og:video"]:
        assert key in meta and meta[key], f"缺少 {key}"
    assert "twitter:card" in meta, "缺少 twitter:card"

    loaded = page.evaluate(
        """async (url) => {
            return await new Promise((resolve) => {
                const img = new Image();
                img.onload = () => resolve({ok: true, w: img.naturalWidth});
                img.onerror = () => resolve({ok: false, w: 0});
                img.src = url;
                setTimeout(() => resolve({ok: img.naturalWidth > 0, w: img.naturalWidth}), 5000);
            });
        }""",
        meta["og:image"],
    )
    assert loaded["ok"] and loaded["w"] > 0, f"og:image 加载失败: {meta['og:image']} {loaded}"


def test_share_missing_video_shows_404(page: Page):
    """/v/<不存在 id>：显示 404 文案、没有播放器、HTTP 404。"""
    resp = page.goto(BASE + "/v/99999999", wait_until="networkidle")
    assert resp is not None and resp.status == 404, f"HTTP 状态不是 404: {resp and resp.status}"
    body = page.inner_text("body")
    assert "视频不存在" in body or "已被删除" in body, f"404 文案缺失: {body[:150]}"
    assert page.locator("video").count() == 0, "404 页不应出现播放器"


def test_share_download_and_comments(page: Page):
    """分享页的点赞/评论数据与评论列表可见（有数据时）。"""
    page.goto(BASE + "/v/1", wait_until="networkidle")
    page.wait_for_timeout(800)
    body = page.inner_text("body")
    assert "赞" in body and "评论" in body, f"分享页缺少互动数据: {body[:200]}"
    comments = page.evaluate(
        """() => {
            const items = document.querySelectorAll('#commentList .cmt');
            return {count: items.length,
                    emptyVisible: !!(document.getElementById('emptyHint') &&
                        document.getElementById('emptyHint').offsetParent !== null)};
        }"""
    )
    assert comments["count"] >= 1 or comments["emptyVisible"], (
        f"评论区既没有内容也没有空态: {comments}")
