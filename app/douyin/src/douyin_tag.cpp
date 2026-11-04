/**
 * @file douyin_tag.cpp
 * @brief 话题标签提取实现：显式 `#tag` 优先，否则关键词映射。
 *
 * @author douyin demo
 * @date 2026-11-04
 * @copyright MIT License
 */

#include "douyin_tag.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace douyin {
namespace {

/// @brief 单个标签的最大字节数；超过就截断，防止异常长标题拖垮展示。
constexpr std::size_t kMaxTagBytes = 60;
/// @brief 显式 `#tag` 最多保留几个（多贴的标签不参与聚合）。
constexpr std::size_t kMaxExplicitTags = 5;
/// @brief 关键词推断最多保留几个（避免一条视频占满所有话题）。
constexpr std::size_t kMaxKeywordTags = 3;

/**
 * @brief 取 UTF-8 首字节对应的码点字节数。
 * @param lead [in] 首字节；非法首字节按 1 处理，保证扫描一定前进。
 * @return 1~4。
 */
std::size_t Utf8Length(unsigned char lead) {
    if ((lead & 0x80U) == 0) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

/// @brief 标签终止集合：空白、中英文标点、`#` 本身。
bool IsTagDelimiter(std::string_view cp) {
    static const std::string_view kDelims =
        " \t\r\n,，.。!！?？;；:：、#（）()【】[]{}<>《》\"'“”‘’/\\|+&=";
    return !cp.empty() && kDelims.find(cp) != std::string_view::npos;
}

/**
 * @brief 追加去重后的标签。
 * @param tag [in] 候选标签（非空、不含 `#`）。
 * @param tags [in,out] 结果集；已存在则忽略。
 * @param cap [in] 结果集上限；达到后不再追加。
 */
void PushUnique(const std::string& tag, std::vector<std::string>* tags,
                std::size_t cap) {
    if (tag.empty() || tags->size() >= cap) {
        return;
    }
    for (const std::string& existing : *tags) {
        if (existing == tag) {
            return;
        }
    }
    tags->push_back(tag);
}

/**
 * @brief 扫描显式 `#tag`。
 * @param title [in] 标题。
 * @return 去重后的话题（最多 kMaxExplicitTags 个）。
 */
std::vector<std::string> ExplicitTags(const std::string& title) {
    std::vector<std::string> tags;
    std::size_t i = 0;
    while (i < title.size() && tags.size() < kMaxExplicitTags) {
        const std::size_t len = Utf8Length(static_cast<unsigned char>(title[i]));
        const std::string_view cp(title.data() + i, len);
        if (cp != "#") {
            i += len;
            continue;
        }
        std::string tag;
        std::size_t j = i + 1;
        while (j < title.size()) {
            const std::size_t n = Utf8Length(static_cast<unsigned char>(title[j]));
            const std::string_view cur(title.data() + j, n);
            if (IsTagDelimiter(cur)) {
                break;
            }
            tag.append(cur);
            j += n;
            if (tag.size() >= kMaxTagBytes) {
                break;
            }
        }
        PushUnique(tag, &tags, kMaxExplicitTags);
        i = j;
    }
    return tags;
}

/// @brief 关键词 -> 话题；按表顺序匹配，长词在前减少歧义。
struct KeywordTag {
    const char* keyword;
    const char* tag;
};

constexpr KeywordTag kKeywordTags[] = {
    {"爬山", "爬山"},       {"云海", "爬山"},     {"木雕", "木雕"},
    {"手工", "手工"},       {"提拉米苏", "烘焙"}, {"面包", "烘焙"},
    {"咖啡", "咖啡"},       {"骑行", "骑行"},     {"大海", "旅行"},
    {"看海", "旅行"},       {"海边", "旅行"},     {"日落", "摄影"},
    {"延时", "摄影"},       {"大理", "旅行"},     {"山里", "生活"},
    {"石头", "生活"},       {"种菜", "生活"},     {"阳台", "生活"},
    {"便利店", "生活"},     {"窗台", "生活"},     {"流浪猫", "萌宠"},
    {"猫", "萌宠"},         {"狗", "萌宠"},       {"雪", "萌宠"},
    {"收音机", "修复"},     {"老钟", "修复"},     {"修复", "修复"},
    {"早餐", "美食"},       {"辣椒酱", "美食"},   {"烟火气", "美食"},
    {"美食", "美食"},       {"吉他", "音乐"},     {"旧书", "读书"},
    {"读书", "读书"},       {"旧书摊", "读书"},   {"城市", "城市"},
};

/**
 * @brief 关键词推断：标题子串命中即加对应话题。
 * @param title [in] 标题。
 * @return 去重后的话题（最多 kMaxKeywordTags 个）。
 */
std::vector<std::string> KeywordTags(const std::string& title) {
    std::vector<std::string> tags;
    for (const KeywordTag& entry : kKeywordTags) {
        if (title.find(entry.keyword) != std::string::npos) {
            PushUnique(entry.tag, &tags, kMaxKeywordTags);
        }
    }
    return tags;
}

}  // namespace

// step1: 显式 `#tag` 优先，发布者写什么就是什么；
// step2: 没有显式标签才用关键词表（历史 demo 视频没有 `#`）；
// step3: 两者都为空就返回空向量，由调用方展示「无话题」。
std::vector<std::string> ExtractTags(const std::string& title) {
    std::vector<std::string> tags = ExplicitTags(title);
    if (!tags.empty()) {
        return tags;
    }
    return KeywordTags(title);
}

}  // namespace douyin
