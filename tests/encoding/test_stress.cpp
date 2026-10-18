/**
 * @file test_stress.cpp
 * @brief `encoding` 模块压测：1 MiB 数据的 base64 / hex 编解码吞吐。
 *
 * @details 方法：固定 1 MiB 随机数据，预热后重复 15 轮，取**最快一轮**换算
 *          MiB/s（取最快而不是平均，是为了减少 WSL 调度抖动的影响；每轮结果
 *          都打印，便于看出离群值）。压测条件：Release/-O2、单线程。
 *
 *          这不是微基准框架，不承诺与其它机器可比；数字用于确认实现没有
 *          意外的逐字节退化（例如每字节一次字符串分配）。
 */

#include "encoding_test_support.hpp"

#include "go2cpp/encoding/base64.hpp"
#include "go2cpp/encoding/hex.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>

namespace e = go2cpp::encoding;

namespace {

constexpr std::size_t kMiB = 1024 * 1024;

/// 对 `fn` 重复计时，打印每一轮并返回最快耗时（秒）。
template <typename Fn>
double TimeBest(const char* label, std::size_t bytes, int rounds, Fn&& fn) {
    double best = std::numeric_limits<double>::infinity();
    std::printf("[encoding] %s (%d rounds)\n", label, rounds);
    for (int round = 0; round < rounds; ++round) {
        const auto start = go2cpp_encoding_test::Clock::now();
        fn();
        const double seconds = go2cpp_encoding_test::SecondsSince(start);
        best = std::min(best, seconds);
        go2cpp_encoding_test::ReportThroughput(label, bytes, seconds);
    }
    go2cpp_encoding_test::ReportThroughput(label, bytes, best);
    return best;
}

}  // namespace

void RunStressTests() {
    go2cpp_encoding_test::Announce("stress: 1 MiB base64 / hex throughput");

    std::mt19937_64 rng(0x572e55);
    const std::string data = go2cpp_encoding_test::RandomBytes(rng, kMiB);
    std::printf(
        "[encoding] environment: 1 MiB = %zu bytes, single thread, Release/-O2\n",
        kMiB);

    volatile std::size_t sink = 0;

    // ---- base64 编码 ----
    {
        const std::string encoded = e::StdEncoding.EncodeToString(data);
        ENCODING_CHECK(encoded.size() == e::StdEncoding.EncodedLen(data.size()));
        TimeBest("base64 StdEncoding encode", data.size(), 15, [&] {
            std::string out;
            out.reserve(encoded.size());
            e::StdEncoding.Encode(out, data);
            sink += out.size();
        });
        TimeBest("base64 URLEncoding encode", data.size(), 15, [&] {
            std::string out;
            out.reserve(encoded.size());
            e::URLEncoding.Encode(out, data);
            sink += out.size();
        });

        // ---- base64 解码 ----
        TimeBest("base64 StdEncoding decode", encoded.size(), 15, [&] {
            std::string out;
            out.reserve(data.size());
            const auto error = e::StdEncoding.Decode(out, encoded);
            ENCODING_CHECK(error == nullptr);
            sink += out.size();
        });
        const std::string url_encoded = e::URLEncoding.EncodeToString(data);
        TimeBest("base64 URLEncoding decode", url_encoded.size(), 15, [&] {
            std::string out;
            out.reserve(data.size());
            const auto error = e::URLEncoding.Decode(out, url_encoded);
            ENCODING_CHECK(error == nullptr);
            sink += out.size();
        });
    }

    // ---- base64 Raw（无填充）----
    {
        const std::string raw = e::RawStdEncoding.EncodeToString(data);
        ENCODING_CHECK(raw.size() == e::RawStdEncoding.EncodedLen(data.size()));
        TimeBest("base64 RawStdEncoding encode", data.size(), 15, [&] {
            std::string out;
            out.reserve(raw.size());
            e::RawStdEncoding.Encode(out, data);
            sink += out.size();
        });
        TimeBest("base64 RawStdEncoding decode", raw.size(), 15, [&] {
            std::string out;
            out.reserve(data.size());
            const auto error = e::RawStdEncoding.Decode(out, raw);
            ENCODING_CHECK(error == nullptr);
            sink += out.size();
        });
    }

    // ---- hex ----
    {
        const std::string encoded = e::Hex::EncodeToString(data);
        ENCODING_CHECK(encoded.size() == kMiB * 2);
        TimeBest("hex encode", data.size(), 15, [&] {
            std::string out;
            out.reserve(encoded.size());
            e::Hex::Encode(out, data);
            sink += out.size();
        });
        TimeBest("hex decode", encoded.size(), 15, [&] {
            std::string out;
            out.reserve(data.size());
            const auto error = e::Hex::Decode(out, encoded);
            ENCODING_CHECK(error == nullptr);
            sink += out.size();
        });
        // 大写输入性能应与小写一致（同一查表路径）。
        std::string upper = encoded;
        for (char& c : upper) {
            if (c >= 'a' && c <= 'f') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        TimeBest("hex decode (uppercase)", upper.size(), 15, [&] {
            std::string out;
            out.reserve(data.size());
            const auto error = e::Hex::Decode(out, upper);
            ENCODING_CHECK(error == nullptr);
            sink += out.size();
        });
    }

    // 防止优化器把上面的计算全部删掉；同时输出一个可核对的校验值。
    std::printf("[encoding] stress sink (bytes processed, mod 2^32) = %zu\n",
                static_cast<std::size_t>(sink & 0xffffffffu));
}
