/**
 * @file test_json_unescape.cpp
 * @brief `\uXXXX` 解码回归测试：BMP 码点、UTF-16 代理对与非法序列。
 *
 * @details 覆盖的是 P0 用户可见 bug 的直接根因：`douyin::JsonStringField`
 *          旧实现把 `\u` 里的 `u` 当普通字符追加，`"\u6f14"` 落库成字面量
 *          `u6f14`（举报 detail 中文全毁）。本测试只打解析层，不起服务：
 *          - BMP 中文 `\u4f60\u597d` → `你好`；
 *          - 代理对 `\uD83D\uDE00` → U+1F600（UTF-8 F0 9F 98 80）；
 *          - 边界码点 `\u0000` / `\u00e9` / `\uFFFF`；
 *          - 常规转义（`\"` / `\\` / `\n`）不回归；
 *          - 非法序列（残缺、非 hex、孤立代理）必须返回 false，不静默吞掉。
 *
 * 用法：`bash app/douyin/scripts/run_json_unescape_test.sh`
 * 退出码：0 = 全过；1 = 有失败（打印失败用例）。
 */

#include "douyin_http.hpp"

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

/// @brief 断言两个字节串完全相同；失败时打印期望/实际的 hex 与可读文本。
void ExpectBytes(std::string_view actual, std::string_view expected,
                 std::string_view name) {
    ++g_checks;
    if (actual == expected) {
        return;
    }
    ++g_failures;
    std::printf("[FAIL] %.*s\n", static_cast<int>(name.size()), name.data());
    std::printf("   expect(%zu):", expected.size());
    for (const char c : expected) {
        std::printf(" %02x", static_cast<unsigned char>(c));
    }
    std::printf("\n   actual(%zu):", actual.size());
    for (const char c : actual) {
        std::printf(" %02x", static_cast<unsigned char>(c));
    }
    std::printf("\n");
}

/// @brief 解析 `payload` 里 `detail` 字段；返回是否解析成功。
bool ParseDetail(std::string_view payload, std::string* out) {
    std::string value;
    const bool ok = douyin::JsonStringField(payload, "detail", &value);
    if (ok) {
        *out = std::move(value);
    }
    return ok;
}

/// @brief 断言解析成功且结果等于期望字节串。
void ExpectParsed(std::string_view payload, std::string_view expected,
                  std::string_view name) {
    std::string value;
    ++g_checks;
    if (!ParseDetail(payload, &value)) {
        ++g_failures;
        std::printf("[FAIL] %.*s（解析失败，期望 %zu 字节）\n",
                    static_cast<int>(name.size()), name.data(), expected.size());
        return;
    }
    --g_checks;
    ExpectBytes(value, expected, name);
}

/// @brief 断言解析失败（非法转义不得静默放行）。
void ExpectRejected(std::string_view payload, std::string_view name) {
    std::string value = "unchanged";
    ++g_checks;
    if (ParseDetail(payload, &value)) {
        ++g_failures;
        std::printf("[FAIL] %.*s（本应被拒绝，却解析出 %zu 字节）\n",
                    static_cast<int>(name.size()), name.data(), value.size());
        return;
    }
    --g_checks;
    ExpectBytes(value, "unchanged", name);
}

void RunCase(const char* payload, const std::string& expected, const char* name) {
    ExpectParsed(payload, expected, name);
}

}  // namespace

int main() {
    // 1. BMP 中文：`\u4f60\u597d` → `你好`（bug 现场）
    RunCase(R"({"detail":"\u4f60\u597d"})", "\xe4\xbd\xa0\xe5\xa5\xbd",
            "bmp 你好");
    // 2. 举报 detail 的真实载荷：`\u6f14\u793a\u5185\u5bb9`
    RunCase(R"({"detail":"\u6f14\u793a\u5185\u5bb9"})",
            "\xe6\xbc\x94\xe7\xa4\xba\xe5\x86\x85\xe5\xae\xb9",
            "report detail 中文");
    // 3. 代理对：`\uD83D\uDE00` → U+1F600
    RunCase(R"({"detail":"\uD83D\uDE00"})", "\xf0\x9f\x98\x80",
            "surrogate pair 大写");
    // 4. 小写 hex 的代理对：同样必须是 😀
    RunCase(R"({"detail":"\ud83d\ude00"})", "\xf0\x9f\x98\x80",
            "surrogate pair 小写");
    // 5. 中文 + emoji 混排
    RunCase(R"({"detail":"\u4e2d\u6587\ud83d\ude00"})",
            "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80", "中文+emoji");
    // 6. 边界码点：`\u00e9`（2 字节 UTF-8）、`\uFFFF`（3 字节）
    RunCase(R"({"detail":"\u00e9"})", "\xc3\xa9", "bmp é");
    RunCase(R"({"detail":"\uFFFF"})", "\xef\xbf\xbf", "bmp U+FFFF");
    // 7. `\u0000` 是合法的 NUL 字节（JSON 允许），必须解出 0x00
    RunCase(R"({"detail":"\u0000"})", std::string(1, '\0'), "bmp U+0000");
    // 8. 常规转义不回归：`\"`、`\\`、`\n`、`\/`
    RunCase(R"({"detail":"a\"b\\c\nd\/e"})", "a\"b\\c\nd/e", "常规转义");
    // 9. `\u` 与后续字段并存：解析不能把 pos 推进错位
    RunCase(R"({"detail":"\u4f60","reason":"x"})", "\xe4\xbd\xa0",
            "\\u 后紧跟其它字段");
    // 10. 字面量（无反斜杠）保持原样：`u6f14` 不是转义
    RunCase(R"({"detail":"u6f14"})", "u6f14", "字面量 u6f14 不变");

    // 11. 非法序列必须拒绝，不得静默吞掉
    ExpectRejected(R"({"detail":"\u12"})", "残缺 hex");
    ExpectRejected(R"({"detail":"\u12g4"})", "非 hex 字符");
    ExpectRejected(R"({"detail":"\uD83D"})", "孤立高代理");
    ExpectRejected(R"({"detail":"\uDE00"})", "孤立低代理");
    ExpectRejected(R"({"detail":"\uD83Dxy"})", "高代理后无低代理");

    // 12. 只有合法时才写 out：失败路径保持调用方原值
    {
        std::string value = "keep";
        const bool ok = douyin::JsonStringField(
            R"({"detail":"\uZZZZ"})", "detail", &value);
        ++g_checks;
        if (ok || value != "keep") {
            ++g_failures;
            std::printf("[FAIL] 失败路径不应改写 out（ok=%d value=%s）\n",
                        ok ? 1 : 0, value.c_str());
        }
    }

    if (g_failures == 0) {
        std::printf("[PASS] %d checks\n", g_checks);
        return 0;
    }
    std::printf("[FAIL] %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
