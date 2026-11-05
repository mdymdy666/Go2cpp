// log 模块微基准：写 1e6 行 ops/s（过滤 + 格式化 + Sink 分发）。
//
// 口径：LogItemWorker::Submit() 一次 = 1 行。链路含 MinimumLevelFilter 判定、
// PatternFormatter("{message}\n") 渲染、Sink::Write 计数。Sink 用原子计数，
// 不落盘 —— 测的是日志管线本身，不含磁盘 IO（磁盘写会淹没被测开销）。

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include "bench_common.hpp"
#include "go2cpp/logging/log.hpp"

namespace {

class CountingSink final : public go2cpp::log::LogSink {
public:
    void Write(const go2cpp::log::LogRecord& record, std::string_view formatted) override {
        (void)record;
        m_lines.fetch_add(1, std::memory_order_relaxed);
        m_bytes.fetch_add(formatted.size(), std::memory_order_relaxed);
    }

    std::uint64_t Lines() const { return m_lines.load(std::memory_order_relaxed); }
    std::uint64_t Bytes() const { return m_bytes.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> m_lines{0};
    std::atomic<std::uint64_t> m_bytes{0};
};

}  // namespace

int main() {
    using namespace go2cpp::log;

    auto sink = std::make_shared<CountingSink>();
    auto worker = std::make_shared<LogItemWorker>(
        std::make_shared<MinimumLevelFilter>(Level::Info),
        std::make_shared<PatternFormatter>("{message}\n"), sink);

    LogRecord record;
    record.level = Level::Info;
    record.logger = "bench";
    record.message = "perf-uncovered log line payload";
    record.file_literal = "bench_log.cpp";
    record.line = 1;

    if (!worker->Submit(record)) {
        std::printf("log_probe_failed=1\n");
        return 1;
    }

    bench::RunAdaptive("log_submit_line", 20000, 150.0, [&](std::size_t n) {
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < n; ++i) {
            accepted += worker->Submit(record) ? 1u : 0u;
        }
        bench::g_sink = accepted;
    });

    std::printf("log_lines=%llu log_bytes=%llu\n",
                static_cast<unsigned long long>(sink->Lines()),
                static_cast<unsigned long long>(sink->Bytes()));
    return 0;
}
