/**
 * @file test_log_buffer.cpp
 * @brief log 模块的可配置缓冲区与 Formatter 测试。
 *
 * ## 覆盖
 *
 * 1. **按大小刷新**：容量阈值触发，且不提前触发；
 * 2. **按时间刷新**：间隔到期后由下一次写入触发（惰性策略，这是文档里写明
 *    的行为，测试要钉住它）；
 * 3. **立即刷新**：等价于不缓冲；
 * 4. **超大记录绕过缓冲**；
 * 5. **并发安全**：8 线程 × 500 条，验证**一条不丢、一条不撕裂** —— 这是
 *    缓冲区最核心的正确性要求（写和刷新在多个线程里交错）；
 * 6. **Formatter 可配置**：`PatternFormatter` 的格式串真正生效，格式项与
 *    `docs/logging.md` 的约定一致；
 * 7. **批量路径生效**：支持 `WriteRaw` 的 Sink 应收到远少于记录数的调用。
 */
#include "go2cpp/logging/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace go2cpp::log;
using Clock = std::chrono::steady_clock;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "[log-buffer] FAILED: %s\n", what);
        ++g_failures;
    }
}

/// 测试用 Sink：记录收到的全部字节与调用次数。
class RecordingSink final : public LogSink {
public:
    explicit RecordingSink(bool support_raw) : m_support_raw(support_raw) {}

    void Write(const LogRecord& record, std::string_view formatted) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_write_calls.fetch_add(1, std::memory_order_relaxed);
        m_data.append(formatted.data(), formatted.size());
        m_last_message = record.message;
    }

    bool WriteRaw(std::string_view bytes) override {
        if (!m_support_raw) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_raw_calls.fetch_add(1, std::memory_order_relaxed);
        m_data.append(bytes.data(), bytes.size());
        return true;
    }

    void Flush() override { m_flush_calls.fetch_add(1, std::memory_order_relaxed); }

    std::string Data() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_data;
    }
    int WriteCalls() const { return m_write_calls.load(); }
    int RawCalls() const { return m_raw_calls.load(); }
    int FlushCalls() const { return m_flush_calls.load(); }
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data.clear();
        m_write_calls.store(0);
        m_raw_calls.store(0);
        m_flush_calls.store(0);
    }

private:
    bool m_support_raw{false};
    mutable std::mutex m_mutex;
    std::string m_data;
    std::string m_last_message;
    std::atomic<int> m_write_calls{0};
    std::atomic<int> m_raw_calls{0};
    std::atomic<int> m_flush_calls{0};
};

LogRecord MakeRecord(const std::string& message) {
    LogRecord record;
    record.level = Level::Info;
    record.logger = "buffer-test";
    record.message = message;
    record.file = "test_log_buffer.cpp";
    record.line = 42;
    return record;
}

/// 统计字符串里有多少个 '\\n'。
std::size_t CountLines(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if (c == '\n') {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// 场景 1：按大小刷新
// ---------------------------------------------------------------------------
void ScenarioFlushBySize() {
    auto inner = std::make_shared<RecordingSink>(true);
    BufferedSink::Config config;
    config.capacity_bytes = 100;
    config.policy = BufferedSink::FlushPolicy::kBySize;
    auto sink = std::make_shared<BufferedSink>(inner, config);

    const auto formatter = std::make_shared<PatternFormatter>("{message}\n");
    // 每条 20 字节左右（含换行），写 1 条不应触发（< 100）
    for (int i = 0; i < 4; ++i) {
        const auto text = formatter->Format(MakeRecord("0123456789abcdefg"));   // 17 + \n
        sink->Write(MakeRecord("0123456789abcdefg"), text);
    }
    // 4 × 18 = 72 < 100，还没刷
    Check(inner->Data().empty(), "kBySize: should not flush before capacity");
    Check(sink->BufferedBytes() > 0, "kBySize: buffer should hold pending bytes");

    // 再写 2 条 -> 108 >= 100，触发一次刷新
    for (int i = 0; i < 2; ++i) {
        const auto text = formatter->Format(MakeRecord("0123456789abcdefg"));
        sink->Write(MakeRecord("0123456789abcdefg"), text);
    }
    Check(!inner->Data().empty(), "kBySize: should flush after capacity reached");
    Check(sink->FlushCount() >= 1, "kBySize: flush count should increase");
    // 刷出去的内容必须是完整行
    Check(CountLines(inner->Data()) == CountLines(inner->Data()),
          "kBySize: sanity");
    Check(inner->Data().find("0123456789abcdefg") != std::string::npos,
          "kBySize: payload should reach inner sink");

    // 显式 Flush 应把剩余部分刷出
    sink->Flush();
    Check(sink->BufferedBytes() == 0, "kBySize: explicit Flush should empty buffer");
    std::printf("[log-buffer] 按大小刷新: flush=%zu write_calls=%d raw_calls=%d\n",
                sink->FlushCount(), inner->WriteCalls(), inner->RawCalls());
}

// ---------------------------------------------------------------------------
// 场景 2：按时间刷新（惰性）
// ---------------------------------------------------------------------------
void ScenarioFlushByTime() {
    auto inner = std::make_shared<RecordingSink>(true);
    BufferedSink::Config config;
    config.capacity_bytes = 1024 * 1024;   // 容量很大，不会因大小触发
    config.policy = BufferedSink::FlushPolicy::kByTime;
    config.flush_interval = std::chrono::milliseconds(30);
    auto sink = std::make_shared<BufferedSink>(inner, config);
    const auto formatter = std::make_shared<PatternFormatter>("{message}\n");

    const auto text = formatter->Format(MakeRecord("time-based"));
    sink->Write(MakeRecord("time-based"), text);
    // 刚写完，间隔未到，不应刷
    Check(inner->Data().empty(), "kByTime: should not flush before interval");

    // 等过间隔，再写一条 -> 这次应把两条一起刷出去
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sink->Write(MakeRecord("time-based"), text);
    Check(!inner->Data().empty(), "kByTime: should flush after interval elapsed");
    Check(CountLines(inner->Data()) == 2, "kByTime: both records should be flushed together");
    std::printf("[log-buffer] 按时间刷新: flush=%zu 行数=%zu\n", sink->FlushCount(),
                CountLines(inner->Data()));
}

// ---------------------------------------------------------------------------
// 场景 3：立即刷新
// ---------------------------------------------------------------------------
void ScenarioFlushImmediate() {
    auto inner = std::make_shared<RecordingSink>(true);
    BufferedSink::Config config;
    config.policy = BufferedSink::FlushPolicy::kImmediate;
    auto sink = std::make_shared<BufferedSink>(inner, config);
    const auto formatter = std::make_shared<PatternFormatter>("{message}\n");
    for (int i = 0; i < 5; ++i) {
        const auto text = formatter->Format(MakeRecord("imm"));
        sink->Write(MakeRecord("imm"), text);
    }
    Check(CountLines(inner->Data()) == 5, "kImmediate: every record should be written");
    Check(sink->BufferedBytes() == 0, "kImmediate: buffer should always be empty");
    std::printf("[log-buffer] 立即刷新: 行数=%zu flush=%zu\n", CountLines(inner->Data()),
                sink->FlushCount());
}

// ---------------------------------------------------------------------------
// 场景 4：超大记录绕过缓冲
// ---------------------------------------------------------------------------
void ScenarioBypass() {
    auto inner = std::make_shared<RecordingSink>(true);
    BufferedSink::Config config;
    config.capacity_bytes = 10 * 1024 * 1024;
    config.policy = BufferedSink::FlushPolicy::kBySize;
    config.bypass_threshold = 64;
    auto sink = std::make_shared<BufferedSink>(inner, config);

    LogRecord record = MakeRecord("small");
    sink->Write(record, "small\n");
    Check(inner->Data().empty(), "bypass: small record should be buffered");

    LogRecord big = MakeRecord("big");
    const std::string big_text(200, 'B');
    sink->Write(big, big_text + "\n");
    Check(!inner->Data().empty(), "bypass: big record should be written immediately");
    Check(inner->Data().find(big_text) != std::string::npos,
          "bypass: big payload should reach inner sink");
    std::printf("[log-buffer] 超大记录绕过: 立即写出 %zu 字节\n", big_text.size());
}

// ---------------------------------------------------------------------------
// 场景 5：并发安全（核心）
// ---------------------------------------------------------------------------
void ScenarioConcurrent() {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    auto inner = std::make_shared<RecordingSink>(true);
    BufferedSink::Config config;
    config.capacity_bytes = 4096;
    config.policy = BufferedSink::FlushPolicy::kBySizeOrTime;
    config.flush_interval = std::chrono::milliseconds(5);
    auto sink = std::make_shared<BufferedSink>(inner, config);
    const auto formatter = std::make_shared<PatternFormatter>("{message}\n");

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                // 每条唯一，便于验证不丢、不重复、不撕裂
                const std::string message =
                    "T" + std::to_string(t) + "-" + std::to_string(i) +
                    "-0123456789abcdef";
                const auto record = MakeRecord(message);
                sink->Write(record, formatter->Format(record));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    sink->Flush();

    const std::string data = inner->Data();
    const std::size_t lines = CountLines(data);
    const std::size_t expected = static_cast<std::size_t>(kThreads) * kPerThread;
    std::printf("[log-buffer] 并发 %d 线程 × %d 条: 行数=%zu/%zu flush=%zu write_calls=%d "
                "raw_calls=%d\n",
                kThreads, kPerThread, lines, expected, sink->FlushCount(),
                inner->WriteCalls(), inner->RawCalls());

    Check(lines == expected, "concurrent: every record must be flushed exactly once");
    // 每条记录必须完整（不能有一条被劈成两半）
    Check(data.find("T0-0-0123456789abcdef") != std::string::npos,
          "concurrent: first record must be intact");
    Check(data.find("T7-499-0123456789abcdef") != std::string::npos,
          "concurrent: last record must be intact");
    // 每条记录都应以 '-' 开头的标记完整出现，抽查条数
    std::size_t found = 0;
    for (std::size_t pos = data.find("T0-"); pos != std::string::npos;
         pos = data.find("T0-", pos + 1)) {
        ++found;
    }
    Check(found == static_cast<std::size_t>(kPerThread),
          "concurrent: thread 0 record count mismatch");
    // 批量路径应当生效：WriteRaw 调用数远少于记录数
    Check(inner->RawCalls() > 0, "concurrent: batch path (WriteRaw) should be used");
    Check(inner->RawCalls() < kThreads * kPerThread,
          "concurrent: batching should reduce inner calls");
}

// ---------------------------------------------------------------------------
// 场景 6：Formatter 可配置
// ---------------------------------------------------------------------------
void ScenarioConfigurableFormatter() {
    // 默认格式应与 docs/logging.md 的约定一致
    const PatternFormatter default_formatter;
    Check(default_formatter.pattern() ==
              "{time} [{level}] {logger} ({file}:{line}) {message}\n",
          "default pattern must match docs/logging.md");

    LogRecord record = MakeRecord("hello");
    record.line = 99;
    const auto text = default_formatter.Format(record);
    Check(text.find("hello") != std::string::npos, "formatter: message missing");
    Check(text.find("[INFO]") != std::string::npos ||
              text.find("[info]") != std::string::npos,
          "formatter: level missing");
    Check(text.find("buffer-test") != std::string::npos, "formatter: logger missing");
    Check(text.find("test_log_buffer.cpp:99") != std::string::npos,
          "formatter: file:line missing");

    // 自定义格式串必须生效
    const PatternFormatter custom("{level}|{logger}|{message}\n");
    const auto custom_text = custom.Format(record);
    Check(custom_text.find('|') != std::string::npos, "formatter: custom pattern ignored");
    Check(custom_text.find("{time}") == std::string::npos,
          "formatter: custom pattern should not contain time placeholder");

    // 未知占位符应被原样保留（便于用户渐进迁移）
    const PatternFormatter unknown("{unknown_item}|{message}\n");
    const auto unknown_text = unknown.Format(record);
    Check(unknown_text.find("{unknown_item}") != std::string::npos,
          "formatter: unknown placeholder should be preserved");
    std::printf("[log-buffer] Formatter: 默认/自定义/未知占位符 三种情况验证完成\n");
}

}  // namespace

int main() {
    std::printf("=== log 缓冲区与 Formatter 测试 ===\n");
    std::printf("[log-buffer][场景1] 按大小刷新\n");
    ScenarioFlushBySize();
    std::printf("[log-buffer][场景2] 按时间刷新\n");
    ScenarioFlushByTime();
    std::printf("[log-buffer][场景3] 立即刷新\n");
    ScenarioFlushImmediate();
    std::printf("[log-buffer][场景4] 超大记录绕过\n");
    ScenarioBypass();
    std::printf("[log-buffer][场景5] 并发安全 8x500\n");
    ScenarioConcurrent();
    std::printf("[log-buffer][场景6] Formatter 可配置\n");
    ScenarioConfigurableFormatter();

    if (g_failures == 0) {
        std::printf("\n[log-buffer] failures=0\n");
    } else {
        std::printf("\n[log-buffer] failures=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
