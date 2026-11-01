/**
 * @file test_windows_smoke.cpp
 * @brief Windows（MinGW 13.1）平台冒烟测试：验证 Linux-free 模块在 Windows 上
 *        **可编译、可链接、可运行**。
 *
 * 覆盖 6 个真实场景：
 *  1. `tools/time`：UTC 格式化（直接验证 `gmtime_r` → `gmtime_s` 的 Windows 适配）
 *     与 `Duration` 单位换算；
 *  2. `frame`：编码→解码往返、半包逐字节、粘包三帧、坏 CRC/坏 magic 拒绝；
 *  3. `cache`：LRU 命中与驱逐顺序；
 *  4. `error`：错误对象构造与消息；
 *  5. `reflect/json`：JsonWriter 生成 → JsonParser 解析往返；
 *  6. `tools/time::Sleep`：实际睡眠时长下界（验证 `nanosleep` 在 MinGW 可用）。
 *
 * 说明：`relay/lease` 不在此测试内 —— 其实现依赖 `go2cpp::Context`，链接会牵出
 * `scheduler`/`Fiber`（`context_x64.S` Linux 汇编），在 Windows 上只能编译不能链接。
 * 该结论由 `lease.cpp` 单独编译验证（见 /tmp/windows_verify.md）。
 *
 * 断言失败累计到 `g_failures`（不中断），`main` 汇总后返回非零。
 */

#include "go2cpp/cache/lru.hpp"
#include "go2cpp/error/error.hpp"
#include "go2cpp/frame/frame_codec.hpp"
#include "go2cpp/reflect/json.hpp"
#include "go2cpp/tools/time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
}

void CheckStr(const std::string& actual, const std::string& expected, int line) {
    ++g_checks;
    if (actual == expected) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "FAIL line %d: got '%s' want '%s'\n", line, actual.c_str(),
                 expected.c_str());
}

#define CHECK(cond) Check((cond), #cond, __LINE__)
#define CHECK_STR(actual, expected) CheckStr((actual), (expected), __LINE__)

void Announce(const char* name) {
    std::printf("[win-smoke] %s\n", name);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 场景 1：time 的 UTC 格式化与单位换算
// ---------------------------------------------------------------------------
void ScenarioTime() {
    Announce("场景1 time: UTC 格式化（gmtime_s 路径）+ Duration 换算");
    using go2cpp::time::Duration;
    using go2cpp::time::Time;

    CHECK_STR(Time::FromUnix(0).String(), "1970-01-01 00:00:00 +0000");
    // 1700000000 == 2023-11-14T22:13:20Z
    CHECK_STR(Time::FromUnix(1700000000).String(), "2023-11-14 22:13:20 +0000");
    // 2000-02-29T00:00:00Z（闰日）
    CHECK_STR(Time::FromUnix(951782400).String(), "2000-02-29 00:00:00 +0000");
    // 2024-01-01T00:00:00Z
    CHECK_STR(Time::FromUnix(1704067200).String(), "2024-01-01 00:00:00 +0000");
    // 带纳秒：小数秒去掉尾随零
    CHECK_STR(Time::FromUnix(1700000000, 123000000).String(),
              "2023-11-14 22:13:20.123 +0000");

    CHECK(Duration::FromSeconds(1.5).Milliseconds() == 1500);
    CHECK(Duration::FromNanoseconds(2500000).Microseconds() == 2500);
    CHECK(Duration::FromSeconds(-3.0).Seconds() == -3);
}

// ---------------------------------------------------------------------------
// 场景 2：frame 编解码
// ---------------------------------------------------------------------------
void ScenarioFrame() {
    Announce("场景2 frame: 往返 / 半包 / 粘包 / 坏 CRC");
    using go2cpp::frame::Error;
    using go2cpp::frame::ErrorCode;
    using go2cpp::frame::Frame;
    using go2cpp::frame::FrameCodec;
    using go2cpp::frame::MakeFrameSpec;
    using go2cpp::frame::MakeMagic;

    const go2cpp::frame::FrameSpec spec = MakeFrameSpec(MakeMagic("RDRL"));
    CHECK(spec.Valid());
    CHECK(spec.HasChecksum());

    FrameCodec codec(spec);

    Frame frame;
    frame.type = 7;
    frame.flags = 0x1234;
    frame.payload = {1, 2, 3, 4, 5};

    std::vector<std::uint8_t> bytes;
    Error err;
    CHECK(codec.Encode(frame, &bytes, &err));
    CHECK(err.Ok());
    // 12 字节头 + 5 字节载荷 + 4 字节 CRC
    CHECK(bytes.size() == 21);

    // 往返
    std::vector<Frame> frames;
    std::size_t consumed = 0;
    CHECK(codec.Feed(bytes.data(), bytes.size(), &frames, &consumed, &err));
    CHECK(frames.size() == 1);
    CHECK(consumed == bytes.size());
    if (frames.size() == 1) {
        CHECK(frames[0].type == 7);
        CHECK(frames[0].flags == 0x1234);
        CHECK(frames[0].payload == frame.payload);
    }

    // 半包：逐字节喂，前 n-1 次不得交付
    codec.Reset();
    frames.clear();
    bool premature = false;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        consumed = 0;
        codec.Feed(&bytes[i], 1, &frames, &consumed, &err);
        if (!frames.empty()) {
            premature = true;
        }
    }
    CHECK(!premature);
    CHECK(codec.BufferedBytes() == bytes.size() - 1);
    consumed = 0;
    codec.Feed(&bytes[bytes.size() - 1], 1, &frames, &consumed, &err);
    CHECK(frames.size() == 1);

    // 粘包：三帧一次喂
    codec.Reset();
    frames.clear();
    std::vector<std::uint8_t> three;
    for (int i = 0; i < 3; ++i) {
        three.insert(three.end(), bytes.begin(), bytes.end());
    }
    consumed = 0;
    CHECK(codec.Feed(three.data(), three.size(), &frames, &consumed, &err));
    CHECK(frames.size() == 3);
    CHECK(consumed == three.size());

    // 坏 CRC
    codec.Reset();
    frames.clear();
    std::vector<std::uint8_t> corrupt = bytes;
    corrupt.back() ^= 0xFF;
    consumed = 0;
    const bool accepted = codec.Feed(corrupt.data(), corrupt.size(), &frames, &consumed, &err);
    CHECK(!accepted);
    CHECK(err.code == ErrorCode::kChecksumMismatch);

    // 坏 magic
    codec.Reset();
    frames.clear();
    std::vector<std::uint8_t> bad_magic = bytes;
    bad_magic[0] ^= 0xFF;
    consumed = 0;
    CHECK(!codec.Feed(bad_magic.data(), bad_magic.size(), &frames, &consumed, &err));
    CHECK(err.code == ErrorCode::kBadMagic);
}

// ---------------------------------------------------------------------------
// 场景 3：cache LRU
// ---------------------------------------------------------------------------
void ScenarioCache() {
    Announce("场景3 cache: LRU 命中 / 驱逐顺序");
    using go2cpp::cache::CacheOptions;
    using go2cpp::cache::LruCache;

    CacheOptions options;
    options.capacity = 2;
    LruCache<int, int> cache(options);

    cache.Put(1, 100);
    cache.Put(2, 200);
    CHECK(cache.Size() == 2);

    int value = 0;
    CHECK(cache.Get(1, &value));
    CHECK(value == 100);

    // 插入 3 应驱逐最久未用的 2（1 刚被 Get 命中）
    cache.Put(3, 300);
    CHECK(cache.Size() == 2);
    CHECK(!cache.Get(2, &value));
    CHECK(cache.Get(1, &value));
    CHECK(cache.Get(3, &value));

    CHECK(cache.Erase(1));
    CHECK(cache.Size() == 1);
    cache.Clear();
    CHECK(cache.Size() == 0);
}

// ---------------------------------------------------------------------------
// 场景 4：error 对象
// ---------------------------------------------------------------------------
void ScenarioError() {
    Announce("场景4 error: 构造 / 消息");
    using go2cpp::error::StringError;

    StringError e(std::string("boom"));
    CHECK(!e.Message().empty());
    CHECK(e.Message() == "boom");
}

// ---------------------------------------------------------------------------
// 场景 5：reflect/json 往返
// ---------------------------------------------------------------------------
void ScenarioJson() {
    Announce("场景5 reflect/json: JsonWriter → JsonParser 往返");
    using go2cpp::reflect::JsonParser;
    using go2cpp::reflect::JsonValue;
    using go2cpp::reflect::JsonWriter;

    std::string text;
    JsonWriter writer(text);
    writer.BeginObject();
    writer.Key("a");
    writer.Int(1);
    writer.Key("b");
    writer.BeginArray();
    writer.Bool(true);
    writer.String("x");
    writer.EndArray();
    writer.EndObject();
    CHECK(writer.ok());
    CHECK(!text.empty());
    std::printf("          写入: %s\n", text.c_str());

    JsonValue value;
    CHECK(JsonParser::Parse(text, value));
    CHECK(value.kind == JsonValue::Kind::kObject);
    const JsonValue* a = value.Find("a");
    CHECK(a != nullptr);
    if (a != nullptr) {
        CHECK(a->text == "1");
    }
    const JsonValue* b = value.Find("b");
    CHECK(b != nullptr);
    if (b != nullptr) {
        CHECK(b->kind == JsonValue::Kind::kArray);
        CHECK(b->elements.size() == 2);
        if (b->elements.size() == 2) {
            CHECK(b->elements[0].kind == JsonValue::Kind::kBool);
            CHECK(b->elements[0].boolean);
            CHECK(b->elements[1].text == "x");
        }
    }
}

// ---------------------------------------------------------------------------
// 场景 6：Sleep 实际睡眠
// ---------------------------------------------------------------------------
void ScenarioSleep() {
    Announce("场景6 time/Sleep: 实际睡眠时长下界（MinGW nanosleep）");
    using go2cpp::time::Duration;
    using go2cpp::time::Sleep;

    const auto start = std::chrono::steady_clock::now();
    Sleep(Duration::FromNanoseconds(30LL * 1000 * 1000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    std::printf("          实测睡眠 %lld ms\n", static_cast<long long>(elapsed));
    CHECK(elapsed >= 25);
    CHECK(elapsed < 2000);

    // 非正时长立即返回
    const auto start2 = std::chrono::steady_clock::now();
    Sleep(Duration::FromNanoseconds(0));
    Sleep(Duration::FromNanoseconds(-1000));
    const auto elapsed2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start2)
                              .count();
    CHECK(elapsed2 < 50);
}

}  // namespace

int main() {
    std::printf("=== Go2Cpp Windows 平台冒烟测试 (MinGW 13.1, C++17) ===\n");
    ScenarioTime();
    ScenarioFrame();
    ScenarioCache();
    ScenarioError();
    ScenarioJson();
    ScenarioSleep();
    std::printf("\n=== 断言 %d 条，失败 %d 条 ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
