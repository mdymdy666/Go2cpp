#include "go2cpp/log.hpp"

#include <iostream>
#include <sstream>
#include <string>

/**
 * @brief logging 简单版：级别解析、格式化、内存收集。
 * @details 全部在内存里完成，不写任何文件：
 *          ① ParseLevel —— 文本级别名转 Level；
 *          ② PatternFormatter —— 按模式把 LogRecord 格式化成一行；
 *          ③ MemorySink —— 收集格式化结果，Snapshot 取回。
 * @return 全部校验通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp::log;
    bool ok = true;

    // ① 级别解析
    std::clog << "[log][场景1] ParseLevel\n";
    Level level = Level::Off;
    const bool parsed = ParseLevel("warn", &level);
    Level unknown = Level::Info;
    const bool rejected = !ParseLevel("not-a-level", &unknown);
    std::cout << "  解析 warn 成功=" << parsed << " 结果=" << static_cast<int>(level)
              << " 非法名被拒=" << rejected << "（预期 1 4 1）\n";
    ok = ok && parsed && level == Level::Warn && rejected;

    // ② 格式化
    std::clog << "[log][场景2] PatternFormatter\n";
    PatternFormatter formatter("%m%n");
    LogRecord record;
    record.level = Level::Error;
    record.logger = "Demo";
    record.message = "something failed";
    const std::string formatted = formatter.Format(record);
    std::cout << "  格式=\"" << formatted << "\"（预期 something failed 换行）\n";
    ok = ok && formatted == "something failed\n" && formatter.pattern() == "%m%n";

    // ③ 内存收集
    std::clog << "[log][场景3] MemorySink\n";
    MemorySink sink;
    sink.Write(record, formatted);
    LogRecord second;
    second.message = "第二条";
    sink.Write(second, formatter.Format(second));
    const auto snapshot = sink.Snapshot();
    std::cout << "  收集条数=" << snapshot.size() << " 首条=\"" 
              << (snapshot.empty() ? "" : snapshot[0]) << "\"（预期 2）\n";
    ok = ok && snapshot.size() == 2 && snapshot[0] == "something failed\n";

    std::clog << (ok ? "[log] 全部通过\n" : "[log] 有失败\n");
    return ok ? 0 : 1;
}