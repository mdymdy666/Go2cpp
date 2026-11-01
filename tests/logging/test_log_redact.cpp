/**
 * @file test_log_redact.cpp
 * @brief 日志脱敏（JinShan MOD-01）验证：`key_id` / `tok_***` 与 1e4 条日志明文扫描。
 *
 * ## 覆盖（真实场景，不是冒烟测试）
 *
 * 1. **NIST 已知向量**：`KeyId("abc")`、`KeyId("")`、`Redact("")` 的边界值；
 * 2. **与仓库实现的交叉验证**：2000 条随机长度输入，内嵌 SHA-256 与
 *    `go2cpp::hash::Sha256` 前 8 hex 必须完全一致（防止两份实现漂移）；
 * 3. **token 占位符**：任意长度（含极长）都输出 `tok_***`，不泄露长度；
 * 4. **1e4 条日志全量扫描**：每条日志带随机 key/token 各一个，扫描全部
 *    格式化文本，明文出现次数必须为 **0**；同时 `key_id=` 与 `tok_***`
 *    各出现 1e4 次（证明脱敏真的生效，而不是日志没写出去）；
 * 5. **正面对照**：故意写一条未脱敏日志，扫描必须命中——证明扫描逻辑
 *    本身有效（否则「0 次」可能只是因为没扫到东西）；
 * 6. **流安全**：`ostringstream << Redactable` 与 `<< RedactedField` 的输出
 *    只含脱敏文本；
 * 7. **并发**：8 线程 × 1000 次对同一 key 脱敏，结果一致（纯值类型无共享态）；
 * 8. **性能**：1e5 次 `Redact()` 的 ns/次（报告给 JinShan 做开销评估）。
 *
 * 构建运行：
 *   tests/logging/build_and_run_redact.sh
 */
#include "go2cpp/logging/log.hpp"
#include "go2cpp/logging/redact.hpp"

#include "go2cpp/hash/sha256.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using go2cpp::log::IsSensitiveFieldName;
using go2cpp::log::KeyId;
using go2cpp::log::Level;
using go2cpp::log::LogItemWorker;
using go2cpp::log::LogRedacted;
using go2cpp::log::LogRedactedToken;
using go2cpp::log::Logger;
using go2cpp::log::MemorySink;
using go2cpp::log::MinimumLevelFilter;
using go2cpp::log::PatternFormatter;
using go2cpp::log::Redact;
using go2cpp::log::Redactable;
using go2cpp::log::RedactToken;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "[FAIL] %s\n", what.c_str());
        ++g_failures;
    }
}

void BeginTest(const char* name) { std::printf("== %s\n", name); }

/// 随机小写 hex（模拟 API key）。
std::string RandomHex(std::mt19937_64& rng, std::size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(length, '0');
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = kHex[static_cast<std::size_t>(rng() & 0x0fu)];
    }
    return out;
}

/// 随机 base64url 风格串（模拟 session token）。
std::string RandomToken(std::mt19937_64& rng, std::size_t length) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out(length, 'A');
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = kAlphabet[static_cast<std::size_t>(rng() % 64u)];
    }
    return out;
}

/// 统计子串出现次数（空子串返回 0）。
std::size_t CountOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// 1. NIST 已知向量 + 边界
// ---------------------------------------------------------------------------
void TestKnownVectors() {
    BeginTest("NIST 已知向量与空值边界");
    // SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223 ...
    Check(KeyId("abc") == "ba7816bf", "KeyId(abc) 必须等于 ba7816bf");
    // SHA-256("") = e3b0c442 98fc1c14 9afbf4c8 996fb924 ...
    Check(KeyId("") == "", "空密钥的 KeyId 必须为空串");
    Check(Redact("abc").redacted() == "key_id=ba7816bf",
          "Redact(abc) 必须输出 key_id=ba7816bf");
    Check(Redact("").redacted() == "key_id=empty", "空密钥必须输出 key_id=empty");
    Check(RedactToken("").redacted() == "tok_empty", "空 token 必须输出 tok_empty");
    Check(RedactToken("abc").redacted() == "tok_***", "token 必须输出 tok_***");
    // 8 位、小写、纯 hex。
    const std::string id = KeyId("sk-live-0123456789");
    Check(id.size() == 8, "key_id 必须恰好 8 位");
    for (const char ch : id) {
        Check((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'),
              "key_id 必须是小写 hex");
    }
    // 字段形式。
    Check(LogRedacted("api_key", "abc").ToString() == "api_key=key_id=ba7816bf",
          "LogRedacted 必须输出 field=key_id=...");
    Check(LogRedactedToken("session_token", "abc").ToString() ==
              "session_token=tok_***",
          "LogRedactedToken 必须输出 field=tok_***");
}

// ---------------------------------------------------------------------------
// 2. 与 go2cpp::hash::Sha256 交叉验证
// ---------------------------------------------------------------------------
void TestAgainstRepoHash() {
    BeginTest("内嵌 SHA-256 与 go2cpp::hash::Sha256 对拍（2000 条）");
    std::mt19937_64 rng(0x5eed1234u);
    int mismatches = 0;
    std::string first_mismatch;
    for (int i = 0; i < 2000; ++i) {
        // 覆盖 1/55/56/63/64/65/119/120/127/128/129 等填充边界附近。
        const std::size_t length = 1u + static_cast<std::size_t>(rng() % 300u);
        const std::string input = RandomHex(rng, length);
        go2cpp::hash::Sha256 hasher;
        hasher.Write(input);
        const std::vector<std::uint8_t> digest = hasher.Sum();
        static constexpr char kHex[] = "0123456789abcdef";
        std::string expected(8, '0');
        for (std::size_t j = 0; j < 4; ++j) {
            expected[j * 2] = kHex[digest[j] >> 4];
            expected[j * 2 + 1] = kHex[digest[j] & 0x0fu];
        }
        if (KeyId(input) != expected) {
            if (mismatches == 0) {
                first_mismatch = "长度=" + std::to_string(length) +
                                 " 内嵌=" + KeyId(input) + " 仓库=" + expected;
            }
            ++mismatches;
        }
    }
    Check(mismatches == 0,
          "2000 条随机输入必须与仓库 Sha256 完全一致" +
              (first_mismatch.empty() ? std::string() : "（首个：" + first_mismatch + "）"));
}

// ---------------------------------------------------------------------------
// 3. token 占位符不泄露长度
// ---------------------------------------------------------------------------
void TestTokenMask() {
    BeginTest("token 占位符与长度无关");
    std::mt19937_64 rng(0xabcdu);
    const std::vector<std::size_t> lengths = {1, 2, 7, 16, 32, 64, 127, 128,
                                              256, 1024, 65536};
    for (const std::size_t length : lengths) {
        const std::string token = RandomToken(rng, length);
        const Redactable redacted = RedactToken(token);
        Check(redacted.redacted() == "tok_***", "任意长度 token 都必须是 tok_***");
        Check(redacted.redacted().find(token) == std::string::npos,
              "脱敏文本不得包含 token 明文");
        Check(redacted.secret() == token, "secret() 必须保留明文供业务使用");
    }
}

// ---------------------------------------------------------------------------
// 4. 1e4 条日志全量扫描（核心验收）
// ---------------------------------------------------------------------------
struct ScanResult {
    int leaked_keys = 0;
    int leaked_tokens = 0;
    std::size_t key_id_count = 0;
    std::size_t token_mask_count = 0;
    std::size_t log_bytes = 0;
    double scan_ms = 0.0;
};

ScanResult ScanLogs(const std::string& all, const std::vector<std::string>& keys,
                    const std::vector<std::string>& tokens) {
    ScanResult result;
    result.log_bytes = all.size();
    const auto started = std::chrono::steady_clock::now();
    for (const std::string& key : keys) {
        if (all.find(key) != std::string::npos) {
            ++result.leaked_keys;
        }
    }
    for (const std::string& token : tokens) {
        if (all.find(token) != std::string::npos) {
            ++result.leaked_tokens;
        }
    }
    result.scan_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    result.key_id_count = CountOccurrences(all, "key_id=");
    result.token_mask_count = CountOccurrences(all, "tok_***");
    return result;
}

void TestTenThousandLogs() {
    BeginTest("1e4 条日志明文扫描（key/token 各 1e4 个）");
    constexpr int kLogCount = 10000;
    std::mt19937_64 rng(0x1234abcdu);

    std::vector<std::string> keys;
    std::vector<std::string> tokens;
    keys.reserve(kLogCount);
    tokens.reserve(kLogCount);
    for (int i = 0; i < kLogCount; ++i) {
        keys.push_back(RandomHex(rng, 32));       // 128 位随机 API key
        tokens.push_back(RandomToken(rng, 48));   // 288 位随机 session token
    }

    auto sink = std::make_shared<MemorySink>();
    auto worker = std::make_shared<LogItemWorker>(
        std::make_shared<MinimumLevelFilter>(Level::Trace),
        std::make_shared<PatternFormatter>(), sink);
    auto logger = std::make_shared<Logger>("redact-test");
    logger->SetLevel(Level::Trace);
    logger->SetPropagate(false);
    logger->AddWorker(worker);

    // 正面对照：故意写一条未脱敏日志，证明扫描有效。
    {
        go2cpp::log::LogRecord leak;
        leak.level = Level::Info;
        leak.logger = "redact-test";
        leak.message = "unsafe old code key=" + keys[0];
        logger->Log(leak);
        const std::vector<std::string> lines = sink->Snapshot();
        std::string joined;
        for (const std::string& line : lines) {
            joined.append(line);
            joined.push_back('\n');
        }
        Check(joined.find(keys[0]) != std::string::npos,
              "正面对照：未脱敏日志必须能被扫描命中");
        Check(joined.find("key_id=") == std::string::npos,
              "正面对照：未脱敏日志不应出现 key_id");
        sink->Clear();
    }

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < kLogCount; ++i) {
        GO2CPP_LOG_INFO(logger) << "login attempt i=" << i << " "
                                << LogRedacted("api_key", keys[static_cast<std::size_t>(i)])
                                << " "
                                << LogRedactedToken("session_token",
                                                    tokens[static_cast<std::size_t>(i)])
                                << " endpoint=/v1/login result=ok";
    }
    const double write_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();

    const std::vector<std::string> lines = sink->Snapshot();
    Check(lines.size() == static_cast<std::size_t>(kLogCount),
          "1e4 条日志必须全部落进 MemorySink");
    std::string all;
    all.reserve(static_cast<std::size_t>(kLogCount) * 160);
    for (const std::string& line : lines) {
        all.append(line);
        all.push_back('\n');
    }
    const ScanResult result = ScanLogs(all, keys, tokens);

    std::printf("   日志条数=%zu 文本=%zu B 写入=%.1f ms 扫描=%.1f ms\n",
                lines.size(), result.log_bytes, write_ms, result.scan_ms);
    std::printf("   明文命中: key=%d token=%d; key_id=%zu token_mask=%zu\n",
                result.leaked_keys, result.leaked_tokens, result.key_id_count,
                result.token_mask_count);
    Check(result.leaked_keys == 0, "1e4 个 key 的明文命中数必须为 0");
    Check(result.leaked_tokens == 0, "1e4 个 token 的明文命中数必须为 0");
    Check(result.key_id_count == static_cast<std::size_t>(kLogCount),
          "key_id= 必须出现 1e4 次");
    Check(result.token_mask_count == static_cast<std::size_t>(kLogCount),
          "tok_*** 必须出现 1e4 次");

    // 同一个 key 在两条日志里得到同一个 id（可关联）。
    GO2CPP_LOG_INFO(logger) << "repeat " << Redact(keys[1]);
    const std::vector<std::string> after = sink->Snapshot();
    Check(after.size() == static_cast<std::size_t>(kLogCount) + 1,
          "追加一条日志后条数正确");
    Check(after.back().find(Redact(keys[1]).redacted()) != std::string::npos,
          "同一个 key 必须输出同一个 key_id");
}

// ---------------------------------------------------------------------------
// 5. 流安全 / 敏感字段名 / 超长输入
// ---------------------------------------------------------------------------
void TestStreamAndEdgeCases() {
    BeginTest("流安全、字段名判断与超长输入");
    std::ostringstream os;
    os << "k=" << Redact("secret-key") << " t=" << RedactToken("tok-123")
       << " f=" << LogRedacted("password", "hunter2");
    const std::string text = os.str();
    Check(text.find("secret-key") == std::string::npos, "流输出不得含密钥明文");
    Check(text.find("tok-123") == std::string::npos, "流输出不得含 token 明文");
    Check(text.find("hunter2") == std::string::npos, "流输出不得含密码明文");
    Check(text.find("k=key_id=") != std::string::npos, "流输出必须含 key_id 前缀");
    Check(text.find("f=password=key_id=") != std::string::npos,
          "LogRedacted 必须保留字段名");

    Check(IsSensitiveFieldName("API_KEY"), "API_KEY 必须判为敏感");
    Check(IsSensitiveFieldName("session_token"), "session_token 必须判为敏感");
    Check(IsSensitiveFieldName("user_password"), "user_password 必须判为敏感");
    Check(!IsSensitiveFieldName("endpoint"), "endpoint 不应判为敏感");
    Check(!IsSensitiveFieldName("size"), "size 不应判为敏感");

    // 超长密钥（1 MiB）：不能崩溃、不能把明文带出来。
    const std::string huge(1024 * 1024, 'x');
    const Redactable redacted = Redact(huge);
    Check(redacted.redacted().size() == std::string("key_id=").size() + 8,
          "超长密钥仍输出固定长度 key_id");
    Check(redacted.redacted().find(huge) == std::string::npos,
          "超长密钥明文不得出现在脱敏文本中");
    // 含 NUL / 非 ASCII 的密钥按字节处理。
    std::string binary;
    binary.push_back('\0');
    binary.append("中文密钥");
    binary.push_back('\xff');
    Check(Redact(binary).redacted().size() == std::string("key_id=").size() + 8,
          "二进制密钥必须按字节脱敏");
}

// ---------------------------------------------------------------------------
// 6. 并发一致性
// ---------------------------------------------------------------------------
void TestConcurrency() {
    BeginTest("8 线程 × 1000 次并发脱敏");
    constexpr int kThreads = 8;
    constexpr int kIterations = 1000;
    const std::string key = "sk-concurrent-0123456789abcdef";
    const std::string expected = Redact(key).redacted();
    std::vector<std::thread> threads;
    std::vector<int> mismatches(kThreads, 0);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &key, &expected, &mismatches] {
            for (int i = 0; i < kIterations; ++i) {
                if (Redact(key).redacted() != expected) {
                    ++mismatches[static_cast<std::size_t>(t)];
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    int total = 0;
    for (const int value : mismatches) {
        total += value;
    }
    Check(total == 0, "并发下同一个 key 必须得到同一个 key_id");
}

// ---------------------------------------------------------------------------
// 7. 性能
// ---------------------------------------------------------------------------
void TestPerformance() {
    BeginTest("1e5 次 Redact 的开销");
    std::mt19937_64 rng(7);
    const std::string key = RandomHex(rng, 32);
    constexpr int kIterations = 100000;
    std::size_t sink = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        sink += Redact(key).redacted().size();
    }
    const double elapsed_ns = std::chrono::duration<double, std::nano>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    std::printf("   %d 次 Redact: 总 %.1f ms, 单次 %.0f ns（sink=%zu）\n",
                kIterations, elapsed_ns / 1e6, elapsed_ns / kIterations, sink);
    Check(sink == static_cast<std::size_t>(kIterations) * 15u,
          "每次 Redact 的输出长度必须固定（key_id= + 8 hex）");
}

}  // namespace

int main() {
    std::printf("[redact] 日志脱敏验证开始（JinShan MOD-01）\n");
    TestKnownVectors();
    TestAgainstRepoHash();
    TestTokenMask();
    TestTenThousandLogs();
    TestStreamAndEdgeCases();
    TestConcurrency();
    TestPerformance();
    if (g_failures == 0) {
        std::printf("[redact] 全部通过\n");
        return 0;
    }
    std::printf("[redact] 失败项：%d\n", g_failures);
    return 1;
}
