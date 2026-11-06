/**
 * @file douyin_share_page.hpp
 * @brief 分享落地页 `/v/:id` 的服务端渲染模板（独立 HTML，不依赖首页 SPA）。
 *
 * @details 背景：`/v/:id` 原来由网关直接回首页单页（`kHomePageHtml`），
 *          分享到微信 / QQ / 复制链接后再打开，必须等整页 JS + feed 接口
 *          才能看到内容，且分享预览（OG）完全没有。本文件把「视频 → HTML」
 *          的渲染收在一个纯函数里：
 *          - 页面是移动端优先的单列布局（视频 → 标题 → 作者/关注 → 操作 →
 *            评论区），视觉沿用首页的深色 + 抖音红；
 *          - `<meta property="og:*">` 与 `twitter:card` 由服务端写死，爬虫
 *            无需执行 JS 即可拿到标题、描述、封面与视频地址；
 *          - 评论区第一页 SSR，后续页由页面内 JS 调 `api/comment/:id` 追加；
 *          - 动态文本一律经 `EscapeHtml`，避免标题里的 `<script>` 变成 XSS。
 *
 * @note 模板用 `R"HTML(...)HTML"` 原始字符串承载；页面内不得再出现 `)HTML`
 *       序列，否则会提前结束字面量。所有链接都带站点前缀 `base_path`
 *       （本机为 ""，公网 `/douyin` 前缀下为 "/douyin"）。
 *
 * @author douyin demo
 * @date 2026-11-06
 * @copyright MIT License
 */

#ifndef DOUYIN_SHARE_PAGE_HPP
#define DOUYIN_SHARE_PAGE_HPP

#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace douyin {

/// @brief 落地页上的一条顶层评论（已是可直接渲染的文本，未转义）。
struct ShareComment {
    std::int64_t id{0};            ///< 评论 id（点赞请求用）。
    std::int64_t user_id{0};       ///< 评论者 id（昵称缺失时显示「用户 #id」）。
    std::string author_name;       ///< 评论者昵称；空时退回「用户 #id」。
    std::string content;           ///< 正文（1~500 字，渲染前转义）。
    std::int64_t created_at{0};    ///< Unix 秒；显示「N 分钟前」。
    std::int64_t like_count{0};    ///< 点赞数。
    bool liked{false};             ///< 当前查看者是否已赞；匿名恒 false。
    bool pinned{false};            ///< 是否置顶（第 6 轮）；页面加「置顶」标签。
};

/// @brief 渲染落地页所需的全部数据（服务层一次查完，渲染层不碰数据库）。
struct SharePageData {
    std::int64_t video_id{0};      ///< 视频 id。
    std::string title;             ///< 标题（空时退回「视频 #id」）。
    std::int64_t author_id{0};     ///< 作者 id（关注接口用）。
    std::string author_name;       ///< 作者昵称（空时退回「用户 #id」）。
    std::string video_url;         ///< 已带站点前缀的播放地址（含 `/videos/...`）。
    std::string poster_url;        ///< 封面地址；空表示没有封面。
    int poster_width{0};           ///< 封面像素宽；0 表示未知，OG 不输出尺寸。
    int poster_height{0};          ///< 封面像素高；0 表示未知，OG 不输出尺寸。
    std::int64_t like_count{0};    ///< 点赞数（DB 同步维护）。
    std::int64_t comment_count{0}; ///< 评论数。
    std::int64_t created_at{0};    ///< 发布时间（Unix 秒）。
    bool liked{false};             ///< 当前查看者是否已点赞。
    bool following{false};         ///< 当前查看者是否已关注作者。
    std::vector<ShareComment> comments; ///< 第一页评论（最多 20 条）。
    std::string next_cursor;       ///< 还有下一页时的游标；空表示没有更多。
    std::string base_path;         ///< 站点前缀："" 或 "/douyin"（无尾斜杠）。
    std::string origin;            ///< "http://host[:port]"；空表示未知（用相对 URL）。
};

/// @brief HTML 文本转义（`& < > " '` 五个字符）；UTF-8 其它字节原样保留。
inline std::string EscapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}


/// @brief 取 UTF-8 字符串的完整首字符；空串返回 "#"。
///
/// @param text [in] 任意 UTF-8 文本；借用。
/// @return 首字符的完整字节序列（1~4 字节）。**不要**用 `substr(0, 1)`
///         代替：中文昵称会被切成半个码点，页面里出现非法 UTF-8（实测
///         浏览器显示乱码、Python 解码直接抛异常）。
inline std::string FirstUtf8Char(std::string_view text) {
    if (text.empty()) {
        return "#";
    }
    const unsigned char lead = static_cast<unsigned char>(text[0]);
    std::size_t width = 1;
    if ((lead & 0xE0) == 0xC0) {
        width = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        width = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        width = 4;
    }
    if (width > text.size()) {
        width = 1;
    }
    return std::string(text.substr(0, width));
}

/// @brief 按 UTF-8 码点截断（不是按字节），超出时补 `…`。
inline std::string TruncateUtf8(std::string_view text, std::size_t max_chars) {
    std::size_t chars = 0;
    std::size_t pos = 0;
    while (pos < text.size() && chars < max_chars) {
        const unsigned char lead = static_cast<unsigned char>(text[pos]);
        std::size_t width = 1;
        if ((lead & 0xE0) == 0xC0) {
            width = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            width = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            width = 4;
        }
        if (pos + width > text.size()) {
            break;
        }
        pos += width;
        ++chars;
    }
    if (pos >= text.size()) {
        return std::string(text);
    }
    return std::string(text.substr(0, pos)) + "…";
}

/// @brief Unix 秒 → `YYYY-MM-DD`（本地时区）；非法值返回空串。
inline std::string FormatDate(std::int64_t timestamp) {
    if (timestamp <= 0) {
        return std::string();
    }
    const std::time_t seconds = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return std::string();
    }
    char buffer[16] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return std::string(buffer);
}

/// @brief Unix 秒 → 相对时间（刚刚 / N 分钟前 / N 小时前 / N 天前 / 日期）。
inline std::string TimeAgo(std::int64_t now, std::int64_t timestamp) {
    if (timestamp <= 0) {
        return std::string();
    }
    std::int64_t diff = now - timestamp;
    if (diff < 0) {
        diff = 0;
    }
    if (diff < 60) {
        return "刚刚";
    }
    if (diff < 3600) {
        return std::to_string(diff / 60) + " 分钟前";
    }
    if (diff < 86400) {
        return std::to_string(diff / 3600) + " 小时前";
    }
    if (diff < 30 * 86400) {
        return std::to_string(diff / 86400) + " 天前";
    }
    return FormatDate(timestamp);
}

/// @brief 相对地址拼绝对前缀；origin 为空时原样返回（用相对 URL 兜底）。
inline std::string AbsoluteUrl(std::string_view origin, std::string_view path) {
    if (origin.empty() || path.empty()) {
        return std::string(path);
    }
    return std::string(origin) + std::string(path);
}


/// @brief 读 JPEG 文件头里的真实像素尺寸（SOF0~SOF15 段）。
///
/// @param path [in] 本地文件路径（UTF-8，绝对或相对进程 CWD）；借用。
/// @param out_width [out] 成功时写入像素宽（>0）；失败时置 0。
/// @param out_height [out] 成功时写入像素高（>0）；失败时置 0。
/// @return true 表示两个 out 参数都写入了有效尺寸；false 表示文件打不开、
///         不是 JPEG 或找不到 SOF 段——调用方**必须**按「尺寸未知」处理
///         （不要输出 `og:image:width`），不能拿 0 当真实值。
///
/// ## 定位
/// 分享页 OG 尺寸的唯一来源。错了会怎样：微信/微博按错误的宽高裁切
/// 分享卡片，横屏封面被拉成竖屏（曾写死 720x1280）。
///
/// ## 并发/失败语义
/// - 纯函数，只读文件，无共享状态，多线程可并发调用；
/// - 任何异常读法都返回 false，不抛异常（EOF 一律走 false 分支）。
inline bool ReadJpegSize(const std::string& path, int* out_width,
                         int* out_height) {
    if (out_width == nullptr || out_height == nullptr) {
        return false;
    }
    *out_width = 0;
    *out_height = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    unsigned char soi[2] = {0, 0};
    in.read(reinterpret_cast<char*>(soi), 2);
    if (in.gcount() != 2 || soi[0] != 0xFF || soi[1] != 0xD8) {
        return false;
    }
    // 逐个 marker 前移；遇到 SOF 段读宽高，遇到无关段按其长度跳过。
    while (in) {
        const int marker_lead = in.get();
        if (marker_lead != 0xFF) {
            continue;
        }
        int marker = in.get();
        if (marker == EOF) {
            return false;
        }
        while (marker == 0xFF) {
            marker = in.get();
            if (marker == EOF) {
                return false;
            }
        }
        // 不带长度的独立 marker（SOI/EOI/RSTn/TEM）直接跳过。
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        const int len_hi = in.get();
        const int len_lo = in.get();
        if (len_hi == EOF || len_lo == EOF) {
            return false;
        }
        const int segment_len = (len_hi << 8) | len_lo;
        if (segment_len < 2) {
            return false;
        }
        const bool is_sof = (marker >= 0xC0 && marker <= 0xC3) ||
                            (marker >= 0xC5 && marker <= 0xC7) ||
                            (marker >= 0xC9 && marker <= 0xCB) ||
                            (marker >= 0xCD && marker <= 0xCF);
        if (is_sof) {
            in.get();  // 精度字节，固定 8，忽略。
            const int h_hi = in.get();
            const int h_lo = in.get();
            const int w_hi = in.get();
            const int w_lo = in.get();
            if (h_hi == EOF || h_lo == EOF || w_hi == EOF || w_lo == EOF) {
                return false;
            }
            const int height = (h_hi << 8) | h_lo;
            const int width = (w_hi << 8) | w_lo;
            if (width <= 0 || height <= 0) {
                return false;
            }
            *out_width = width;
            *out_height = height;
            return true;
        }
        in.seekg(segment_len - 2, std::ios::cur);
    }
    return false;
}

/// @brief 渲染一条 SSR 评论行（`article.cmt`）。
inline std::string RenderCommentRow(const ShareComment& comment,
                                    std::int64_t now) {
    // 优先显示昵称：原先一律「用户 #id」，同页里作者有昵称、评论者没有。
    const std::string who =
        !comment.author_name.empty()
            ? comment.author_name
            : (comment.user_id > 0
                   ? ("用户 #" + std::to_string(comment.user_id))
                   : std::string("匿名用户"));
    std::string html;
    html.reserve(512);
    html += "<article class=\"cmt\" data-cid=\"" +
            std::to_string(comment.id) + "\">";
    // 头像占位固定 "#"：与 JS 追加的评论行保持一致，也避免按字节取首字符
    // 把「用户 #1」切成半个 UTF-8 码点。
    html += "<div class=\"cavatar\" aria-hidden=\"true\">#";
    html += "</div><div class=\"cbody\"><div class=\"chead\"><b>";
    html += EscapeHtml(who);
    html += "</b>";
    if (comment.pinned) {
        html += "<span class=\"cpin\">置顶</span>";
    }
    html += "<time>";
    html += EscapeHtml(TimeAgo(now, comment.created_at));
    html += "</time></div><p>";
    html += EscapeHtml(comment.content);
    html += "</p><button type=\"button\" class=\"clike";
    html += comment.liked ? " on" : "";
    html += "\" data-cid=\"" + std::to_string(comment.id) +
            "\" data-liked=\"" + (comment.liked ? "1" : "0") + "\">";
    html += "<span aria-hidden=\"true\">♥</span><span class=\"cn\">" +
            std::to_string(comment.like_count) + "</span></button>";
    html += "</div></article>";
    return html;
}

/**
 * @brief 渲染 `/v/:id` 的分享落地页（完整 HTML 字符串）。
 *
 * @param data [in] 页面数据——借用；函数内只读，不持有任何引用。
 *        约定：`video_url` / `poster_url` 已带站点前缀，`base_path` 无尾斜杠，
 *        `origin` 可为空（此时 OG 用相对地址，预览效果依赖平台是否补全）。
 * @return 可直接作为 200 响应体的 HTML；调用方**必须**把
 *         `Content-Type` 设为 `text/html; charset=utf-8`。
 *
 * ## 定位
 * 落地页的唯一渲染入口。错了会怎样：分享预览拿不到标题/封面；标题里的
 * `"` 或 `<` 若未转义会破坏属性甚至形成 XSS；`base_path` 漏拼会让公网
 * 前缀下的视频/接口 404。
 *
 * ## 步骤
 * step1. 归一化显示文本（空标题/空昵称兜底）与绝对 URL；
 * step2. 写 head：title、description、OG、Twitter Card、canonical；
 * step3. 写 body 骨架：播放器 → 标题 → 作者/关注 → 操作栏 → 评论区；
 * step4. SSR 第一页评论（无评论时显示空态）；
 * step5. 追加内联 JS（点赞/关注/发评论/翻页/分享），最后闭合文档。
 *
 * ## 并发/失败语义
 * - 无共享状态，多线程可并发调用；
 * - 不抛异常路径：所有 std::string 操作在内存不足时抛 bad_alloc，由调用方
 *   的兜底 500 处理；
 * - 不变量：返回的 HTML 以 `</html>` 结尾，且所有动态文本已转义。
 */
inline std::string BuildSharePage(const SharePageData& data) {
    // step1: 文本与 URL 归一化。
    const std::string title =
        data.title.empty() ? ("视频 #" + std::to_string(data.video_id))
                           : data.title;
    const std::string author =
        data.author_name.empty()
            ? ("用户 #" + std::to_string(data.author_id))
            : data.author_name;
    const std::string prefix = data.base_path + "/";
    const std::string page_path =
        prefix + "v/" + std::to_string(data.video_id);
    const std::string page_url = AbsoluteUrl(data.origin, page_path);
    const std::string cover_url = AbsoluteUrl(data.origin, data.poster_url);
    const std::string play_url = AbsoluteUrl(data.origin, data.video_url);
    const std::string home_url = AbsoluteUrl(data.origin, prefix);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string description =
        TruncateUtf8(title, 70) + " · @" + author + " · ♥ " +
        std::to_string(data.like_count) + " · 💬 " +
        std::to_string(data.comment_count);

    std::string html;
    html.reserve(32 * 1024);
    // step2: head + 移动端 meta + OG / Twitter 预览。
    html += "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n";
    html += "<meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,"
            "viewport-fit=cover\">\n";
    html += "<meta name=\"color-scheme\" content=\"dark\">\n";
    html += "<meta name=\"theme-color\" content=\"#08080d\">\n";
    html += "<title>" + EscapeHtml(title) + " - Go2Cpp 抖音</title>\n";
    html += "<meta name=\"description\" content=\"" + EscapeHtml(description) +
            "\">\n";
    html += "<meta property=\"og:type\" content=\"video.other\">\n";
    html += "<meta property=\"og:site_name\" content=\"Go2Cpp 抖音\">\n";
    html += "<meta property=\"og:title\" content=\"" + EscapeHtml(title) +
            "\">\n";
    html += "<meta property=\"og:description\" content=\"" +
            EscapeHtml(description) + "\">\n";
    html += "<meta property=\"og:url\" content=\"" + EscapeHtml(page_url) +
            "\">\n";
    if (!cover_url.empty()) {
        html += "<meta property=\"og:image\" content=\"" +
                EscapeHtml(cover_url) + "\">\n";
        // 尺寸取封面真实像素：写死 720x1280 会让微信/微博按竖屏比例
        // 裁切横屏封面，分享卡片里的画面被拉变形。
        if (data.poster_width > 0 && data.poster_height > 0) {
            html += "<meta property=\"og:image:width\" content=\"" +
                    std::to_string(data.poster_width) + "\">\n";
            html += "<meta property=\"og:image:height\" content=\"" +
                    std::to_string(data.poster_height) + "\">\n";
        }
    }
    html += "<meta property=\"og:video\" content=\"" + EscapeHtml(play_url) +
            "\">\n";
    html += "<meta property=\"og:video:type\" content=\"video/mp4\">\n";
    html += "<meta name=\"twitter:card\" content=\"summary_large_image\">\n";
    html += "<meta name=\"twitter:title\" content=\"" + EscapeHtml(title) +
            "\">\n";
    html += "<meta name=\"twitter:description\" content=\"" +
            EscapeHtml(description) + "\">\n";
    if (!cover_url.empty()) {
        html += "<meta name=\"twitter:image\" content=\"" +
                EscapeHtml(cover_url) + "\">\n";
    }
    html += "<link rel=\"canonical\" href=\"" + EscapeHtml(page_url) + "\">\n";
    html += R"HTML(<style>
:root{--pink:#fe2c55;--cyan:#25f4ee;--bg:#08080d;--panel:#14141c;--line:rgba(255,255,255,.1);--text:#f4f5fa;--muted:#9a9dad}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{background:var(--bg);color:var(--text);font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Hiragino Sans GB","Microsoft YaHei",sans-serif;-webkit-font-smoothing:antialiased}
a{color:inherit;text-decoration:none}
button{font:inherit;cursor:pointer;border:0;background:none;color:inherit}
.wrap{max-width:720px;margin:0 auto;padding-bottom:36px}
.top{position:sticky;top:0;z-index:20;display:flex;align-items:center;gap:10px;padding:11px 16px;background:rgba(8,8,13,.94);border-bottom:1px solid var(--line);backdrop-filter:blur(8px)}
.brand{font-size:17px;font-weight:800;letter-spacing:.2px;background:linear-gradient(92deg,var(--cyan),#fff 45%,var(--pink));-webkit-background-clip:text;background-clip:text;color:transparent;white-space:nowrap}
.top .spacer{margin-left:auto}
.ghost{padding:6px 12px;font-size:13px;color:var(--muted);border:1px solid var(--line);border-radius:9px;white-space:nowrap}
.player{background:#000;border-bottom:1px solid var(--line)}
.player video{display:block;width:100%;max-height:72vh;aspect-ratio:9/16;object-fit:contain;background:#000}
@media (min-width:560px){.player video{aspect-ratio:auto;height:72vh}}
.meta{padding:14px 16px 6px}
.meta h1{margin:0 0 6px;font-size:18px;line-height:1.45;font-weight:700;word-break:break-word}
.stats{display:flex;gap:14px;flex-wrap:wrap;font-size:13px;color:var(--muted)}
.author{display:flex;align-items:center;gap:12px;padding:12px 16px;border-bottom:1px solid var(--line)}
.avatar{width:44px;height:44px;flex:0 0 44px;border-radius:50%;display:grid;place-items:center;font-size:18px;font-weight:800;color:#08080d;background:linear-gradient(135deg,var(--cyan),var(--pink))}
.author .who{display:flex;flex-direction:column;min-width:0}
.author .who b{font-size:15px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.author .who small{font-size:12px;color:var(--muted)}
.follow{margin-left:auto;flex:0 0 auto;padding:8px 18px;border-radius:999px;background:var(--pink);color:#fff;font-size:13px;font-weight:700;transition:filter .16s}
.follow:hover{filter:brightness(1.08)}
.follow.on{background:transparent;border:1px solid var(--line);color:var(--muted);font-weight:500}
.ops{display:flex;gap:8px;padding:12px 16px;border-bottom:1px solid var(--line)}
.op{flex:1;display:flex;flex-direction:column;align-items:center;gap:3px;padding:9px 4px;border-radius:12px;background:var(--panel);font-size:12px;color:var(--text);transition:background .16s,color .16s}
.op:hover{background:#1b1b26}
.op i{font-style:normal;font-size:19px;line-height:1.1}
.op.on{color:var(--pink)}
.op.on i{transform:scale(1.12)}
.comments{padding:14px 16px 0}
.comments h2{display:flex;align-items:center;gap:8px;margin:0 0 10px;font-size:16px}
.comments h2 small{font-size:12px;font-weight:400;color:var(--muted)}
.composer{display:flex;gap:8px;margin-bottom:12px}
.composer input{flex:1;min-width:0;padding:9px 12px;border-radius:10px;border:1px solid var(--line);background:var(--panel);color:var(--text);font:inherit;font-size:14px;outline:none}
.composer input:focus{border-color:rgba(37,244,238,.55);box-shadow:0 0 0 3px rgba(37,244,238,.12)}
.composer button{padding:0 16px;border-radius:10px;background:linear-gradient(92deg,var(--pink),#ff5c7a);color:#fff;font-size:13px;font-weight:700}
.composer button:disabled{opacity:.55;cursor:default}
.clist{max-height:62vh;overflow-y:auto;overscroll-behavior:contain;border-top:1px solid var(--line)}
.cmt{display:flex;gap:10px;padding:11px 0;border-bottom:1px solid var(--line)}
.cavatar{width:34px;height:34px;flex:0 0 34px;border-radius:50%;display:grid;place-items:center;font-size:14px;font-weight:700;color:#08080d;background:linear-gradient(135deg,#8ea1ff,var(--cyan))}
.cbody{flex:1;min-width:0}
.chead{display:flex;align-items:baseline;gap:8px}
.chead b{font-size:13px;font-weight:600}
.chead time{font-size:11px;color:var(--muted)}
.cpin{padding:1px 6px;border-radius:6px;font-size:10.5px;font-weight:700;color:#fff;background:linear-gradient(92deg,var(--pink),#ff5c7a)}
.cbody p{margin:2px 0 4px;font-size:14px;word-break:break-word;white-space:pre-wrap}
.clike{display:inline-flex;align-items:center;gap:4px;padding:2px 7px;border-radius:8px;font-size:12px;color:var(--muted)}
.clike:hover{background:rgba(255,255,255,.05)}
.clike.on{color:var(--pink)}
.more{display:block;width:100%;margin:12px 0 0;padding:10px;border-radius:10px;border:1px solid var(--line);font-size:13px;color:var(--muted)}
.more:hover{color:var(--text);border-color:rgba(255,255,255,.24)}
.empty{margin:0;padding:14px 0;font-size:13px;color:var(--muted)}
.toast{position:fixed;left:50%;bottom:26px;z-index:99;max-width:86vw;transform:translateX(-50%);padding:9px 16px;border-radius:999px;background:rgba(20,20,28,.97);border:1px solid var(--line);font-size:13px;box-shadow:0 8px 30px rgba(0,0,0,.55)}
.toast.ok{color:#8ef0d2;border-color:rgba(61,220,151,.4)}
.toast.err{color:#ffb3c1;border-color:rgba(254,44,85,.4)}
footer{padding:22px 16px 0;text-align:center;font-size:12px;color:var(--muted)}
</style>
</head>
)HTML";
    // step3: body 骨架（播放器 / 标题 / 作者 / 操作 / 评论区）。
    html += "<body data-base=\"" + EscapeHtml(prefix) + "\" data-video-id=\"" +
            std::to_string(data.video_id) + "\" data-author-id=\"" +
            std::to_string(data.author_id) + "\" data-next-cursor=\"" +
            EscapeHtml(data.next_cursor) + "\">\n";
    html += "<div id=\"toast\" class=\"toast\" role=\"status\" hidden></div>\n";
    html += "<div class=\"wrap\">\n<header class=\"top\"><a class=\"brand\" href=\"" +
            EscapeHtml(prefix) + "\">Go2Cpp 抖音</a><span class=\"spacer\"></span>" +
            "<a class=\"ghost\" href=\"" + EscapeHtml(prefix) +
            "\">打开首页</a></header>\n";
    html += "<section class=\"player\">\n<video id=\"player\" controls playsinline "
            "preload=\"metadata\" poster=\"" + EscapeHtml(cover_url) +
            "\" src=\"" + EscapeHtml(play_url) + "\"></video>\n</section>\n";
    html += "<section class=\"meta\"><h1>" + EscapeHtml(title) +
            "</h1><div class=\"stats\"><span>♥ <b id=\"likeStat\">" +
            std::to_string(data.like_count) + "</b> 赞</span><span>💬 <b id=\"commentStat\">" +
            std::to_string(data.comment_count) + "</b> 评论</span><span>" +
            EscapeHtml(FormatDate(data.created_at)) + "</span></div></section>\n";
    html += "<section class=\"author\"><div class=\"avatar\" aria-hidden=\"true\">";
    html += EscapeHtml(FirstUtf8Char(author));
    html += "</div><div class=\"who\"><b>" + EscapeHtml(author) +
            "</b><small>@" + std::to_string(data.author_id) + "</small></div>";
    html += "<button id=\"followBtn\" type=\"button\" class=\"follow";
    html += data.following ? " on" : "";
    html += "\" aria-pressed=\"";
    html += data.following ? "true" : "false";
    html += "\">" + std::string(data.following ? "已关注" : "关注") +
            "</button></section>\n";
    html += "<section class=\"ops\">";
    html += "<button id=\"likeBtn\" type=\"button\" class=\"op";
    html += data.liked ? " on" : "";
    html += "\" aria-pressed=\"";
    html += data.liked ? "true" : "false";
    html += "\"><i aria-hidden=\"true\">♥</i><span>点赞</span></button>";
    html += "<a class=\"op\" id=\"downloadBtn\" href=\"" +
            EscapeHtml(play_url) +
            "\" download><i aria-hidden=\"true\">⬇</i><span>下载</span></a>";
    html += "<button id=\"shareBtn\" type=\"button\" class=\"op\">" +
            std::string("<i aria-hidden=\"true\">↗</i><span>分享</span></button>");
    html += "</section>\n";
    html += "<section class=\"comments\"><h2>评论 <small id=\"commentTotal\">" +
            std::to_string(data.comment_count) + "</small></h2>";
    html += "<div class=\"composer\"><input id=\"commentInput\" maxlength=\"500\" "
            "placeholder=\"说点什么…\" aria-label=\"评论内容\"><button id=\"sendBtn\" "
            "type=\"button\">发送</button></div>";
    html += "<div id=\"commentList\" class=\"clist\">";
    // step4: SSR 第一页评论（无评论时给空态）。
    if (data.comments.empty()) {
        html += "<p class=\"empty\" id=\"emptyHint\">还没有评论，来抢沙发～</p>";
    } else {
        for (const auto& comment : data.comments) {
            html += RenderCommentRow(comment, now);
        }
    }
    html += "</div><button id=\"moreBtn\" type=\"button\" class=\"more\"";
    if (data.next_cursor.empty()) {
        html += " hidden";
    }
    html += ">加载更多评论</button></section>\n";
    // step5: 内联 JS（无外部依赖；所有请求都带站点前缀）。
    html += R"HTML(<footer>Go2Cpp 抖音 Demo · 移动端分享落地页</footer>
</div>
<script>
(function () {
  var B = document.body.getAttribute('data-base') || '/';
  var ID = Number(document.body.getAttribute('data-video-id')) || 0;
  var AUTHOR = Number(document.body.getAttribute('data-author-id')) || 0;
  var TOKEN = '';
  try { TOKEN = localStorage.getItem('douyin_token') || ''; } catch (e) { TOKEN = ''; }
  var $ = function (id) { return document.getElementById(id); };
  var likeBtn = $('likeBtn'), likeStat = $('likeStat'), followBtn = $('followBtn');
  var input = $('commentInput'), sendBtn = $('sendBtn'), list = $('commentList');
  var moreBtn = $('moreBtn'), emptyHint = $('emptyHint');
  var cursor = document.body.getAttribute('data-next-cursor') || '';
  var commentTotal = $('commentTotal'), commentStat = $('commentStat');
  var toastTimer = 0;

  function toast(msg, kind) {
    var el = $('toast');
    el.textContent = msg;
    el.className = 'toast' + (kind ? ' ' + kind : '');
    el.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.hidden = true; }, 2600);
  }

  function api(path, method, body) {
    var opt = { method: method || 'GET', headers: {} };
    if (body !== undefined) {
      opt.headers['Content-Type'] = 'application/json';
      opt.body = JSON.stringify(body);
    }
    if (TOKEN) opt.headers['Authorization'] = 'Bearer ' + TOKEN;
    return fetch(B + String(path).replace(/^\//, ''), opt).then(function (res) {
      return res.text().then(function (text) {
        var data = null;
        try { data = JSON.parse(text); } catch (e) { data = null; }
        if (!res.ok || !data || data.ok === false) {
          var err = (data && data.error) || ('HTTP ' + res.status);
          throw new Error(err);
        }
        return data;
      });
    });
  }

  function needLogin() {
    if (TOKEN) return false;
    toast('请先回首页登录，再点赞 / 关注 / 评论', 'err');
    return true;
  }

  function bumpComments(delta) {
    var total = Number(commentTotal.textContent) + delta;
    if (total < 0) total = 0;
    commentTotal.textContent = String(total);
    commentStat.textContent = String(total);
  }

  likeBtn.addEventListener('click', function () {
    if (needLogin()) return;
    var liked = likeBtn.classList.contains('on');
    api('/api/like', liked ? 'DELETE' : 'POST', { video_id: ID }).then(function (d) {
      likeBtn.classList.toggle('on', !liked);
      likeBtn.setAttribute('aria-pressed', liked ? 'false' : 'true');
      likeStat.textContent = String(d.like_count);
      toast(liked ? '已取消点赞' : '已点赞', 'ok');
    }).catch(function (e) { toast('点赞失败：' + e.message, 'err'); });
  });

  followBtn.addEventListener('click', function () {
    if (needLogin()) return;
    var following = followBtn.classList.contains('on');
    api('/api/user/' + AUTHOR + '/follow', following ? 'DELETE' : 'POST').then(function () {
      followBtn.classList.toggle('on', !following);
      followBtn.textContent = following ? '关注' : '已关注';
      followBtn.setAttribute('aria-pressed', following ? 'false' : 'true');
      toast(following ? '已取消关注' : '已关注', 'ok');
    }).catch(function (e) { toast('操作失败：' + e.message, 'err'); });
  });

  function commentRow(c) {
    var row = document.createElement('article');
    row.className = 'cmt';
    row.setAttribute('data-cid', String(c.id));
    var avatar = document.createElement('div');
    avatar.className = 'cavatar';
    avatar.setAttribute('aria-hidden', 'true');
    avatar.textContent = '#';
    var body = document.createElement('div');
    body.className = 'cbody';
    var head = document.createElement('div');
    head.className = 'chead';
    var who = document.createElement('b');
    who.textContent = c.author_name || ('用户 #' + (c.user_id || '—'));
    var when = document.createElement('time');
    when.textContent = '刚刚';
    head.appendChild(who);
    head.appendChild(when);
    var text = document.createElement('p');
    text.textContent = c.content || '';
    var like = document.createElement('button');
    like.type = 'button';
    like.className = 'clike' + (c.liked ? ' on' : '');
    like.setAttribute('data-cid', String(c.id));
    like.setAttribute('data-liked', c.liked ? '1' : '0');
    like.innerHTML = '<span aria-hidden="true">♥</span><span class="cn">' +
      String(c.like_count || 0) + '</span>';
    body.appendChild(head);
    body.appendChild(text);
    body.appendChild(like);
    row.appendChild(avatar);
    row.appendChild(body);
    return row;
  }

  list.addEventListener('click', function (ev) {
    var btn = ev.target && ev.target.closest ? ev.target.closest('.clike') : null;
    if (!btn) return;
    if (needLogin()) return;
    var cid = btn.getAttribute('data-cid');
    var liked = btn.getAttribute('data-liked') === '1';
    api('/api/comment/' + cid + '/like', liked ? 'DELETE' : 'POST').then(function (d) {
      btn.setAttribute('data-liked', d.liked ? '1' : '0');
      btn.classList.toggle('on', !!d.liked);
      var num = btn.querySelector('.cn');
      if (num) num.textContent = String(d.like_count);
    }).catch(function (e) { toast('点赞失败：' + e.message, 'err'); });
  });

  sendBtn.addEventListener('click', function () {
    if (needLogin()) return;
    var text = (input.value || '').trim();
    if (!text) { toast('评论内容不能为空', 'err'); return; }
    sendBtn.disabled = true;
    api('/api/comment', 'POST', { video_id: ID, content: text }).then(function (d) {
      input.value = '';
      if (emptyHint && emptyHint.parentNode) emptyHint.parentNode.removeChild(emptyHint);
      list.insertBefore(commentRow(d.comment || {}), list.firstChild);
      bumpComments(1);
      toast('评论已发布', 'ok');
    }).catch(function (e) {
      toast('发送失败：' + e.message, 'err');
    }).then(function () { sendBtn.disabled = false; });
  });

  moreBtn.addEventListener('click', function () {
    if (!cursor) return;
    moreBtn.disabled = true;
    api('/api/comment/' + ID + '?limit=20&cursor=' + encodeURIComponent(cursor))
      .then(function (d) {
        (d.items || []).forEach(function (item) {
          if (item && item.comment) list.appendChild(commentRow(item.comment));
        });
        cursor = d.next_cursor || '';
        moreBtn.hidden = !cursor;
        moreBtn.disabled = false;
      })
      .catch(function (e) {
        moreBtn.disabled = false;
        toast('加载失败：' + e.message, 'err');
      });
  });

  $('shareBtn').addEventListener('click', function () {
    var url = location.href;
    if (navigator.share) {
      navigator.share({ title: document.title, url: url }).catch(function () {});
      return;
    }
    var copy = function (text) {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () {
          toast('链接已复制', 'ok');
        }, function () { toast(text, 'ok'); });
        return;
      }
      var area = document.createElement('textarea');
      area.value = text;
      area.style.position = 'fixed';
      area.style.opacity = '0';
      document.body.appendChild(area);
      area.select();
      try { document.execCommand('copy'); toast('链接已复制', 'ok'); }
      catch (e) { toast(text, 'ok'); }
      document.body.removeChild(area);
    };
    copy(url);
  });
})();
</script>
</body>
</html>
)HTML";
    return html;
}

/**
 * @brief 渲染「视频不存在 / 已删除」的 404 落地页。
 *
 * @param base_path [in] 站点前缀（`""` 或 `/douyin`）；借用，函数内只读。
 * @param origin [in] 站点源（`http://host`），可为空；仅用于回首页的绝对链接，
 *        为空时用相对路径。
 * @return 完整 HTML；调用方**必须**用 404 状态码返回，避免爬虫把失效链接
 *         当成有效分享。
 */
inline std::string BuildShareNotFound(std::string_view base_path,
                                      std::string_view origin) {
    const std::string prefix = std::string(base_path) + "/";
    const std::string home = AbsoluteUrl(origin, prefix);
    std::string html;
    html.reserve(4096);
    html += "<!DOCTYPE html>\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html += "<meta name=\"color-scheme\" content=\"dark\">";
    html += "<meta name=\"robots\" content=\"noindex\">";
    html += "<title>视频不存在 - Go2Cpp 抖音</title><style>";
    html += R"CSS(:root{--pink:#fe2c55;--cyan:#25f4ee;--bg:#08080d;--line:rgba(255,255,255,.1);--text:#f4f5fa;--muted:#9a9dad}
*{box-sizing:border-box}html,body{margin:0;height:100%}
body{background:var(--bg);color:var(--text);font:15px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif;display:grid;place-items:center;padding:24px}
.card{max-width:420px;text-align:center}
.code{font-size:64px;font-weight:800;line-height:1;background:linear-gradient(92deg,var(--cyan),#fff 45%,var(--pink));-webkit-background-clip:text;background-clip:text;color:transparent}
h1{margin:10px 0 6px;font-size:19px}
p{margin:0 0 22px;color:var(--muted);font-size:14px}
a{display:inline-block;padding:9px 22px;border-radius:999px;background:var(--pink);color:#fff;text-decoration:none;font-weight:700;font-size:14px}
)CSS";
    html += "</style></head><body><div class=\"card\"><div class=\"code\">404</div>";
    html += "<h1>视频不存在或已被删除</h1>";
    html += "<p>链接可能已失效，也可能作者已经删除了这条视频。</p>";
    html += "<a href=\"" + EscapeHtml(home) + "\">回首页看更多</a>";
    html += "</div></body></html>";
    return html;
}

}  // namespace douyin

#endif  // DOUYIN_SHARE_PAGE_HPP
