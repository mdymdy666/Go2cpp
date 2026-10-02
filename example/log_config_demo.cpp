#include "go2cpp/config.hpp"
#include "go2cpp/log.hpp"

#include <iostream>

/**
 * @brief 示例用的自定义格式项。
 * @details 只负责从 LogRecord 读取一个业务字段；过滤、输出目标和生命周期
 *          仍由 LogItemWorker、LogSink 以及 LoggerManager 管理。
 */
class DemoFormatItem final : public go2cpp::log::LogFormatter::Item {
public:
    /**
     * @brief 将 Logger 名称追加到输出流。
     * @param[out] stream 格式器正在构造的输出流。
     * @param[in] record 当前日志的结构化记录。
     * @return 无；结果直接写入 stream。
     */
    void format(std::ostream& stream,
                const go2cpp::log::LogRecord& record) const override {
        stream << "module=" << record.logger;
    }
};

/**
 * @brief 演示从 INI 文件加载运行时日志配置并创建命名 Logger。
 * @details 配置文件决定输出目录、级别和格式；示例只记录警告与错误，避免
 *          发布模式默认输出大量 Info 日志。
 * @return 配置加载并完成日志演示后返回 0。
 */
int main() {
    go2cpp::config::RuntimeConfig config;
    std::string error;
    // 必须先注册格式项，再创建 PatternFormatter；Formatter 构造时会保存 Item 快照。
    go2cpp::log::LogFormatter::AddFormat(
        "g", [](const std::string&) { return std::make_shared<DemoFormatItem>(); });
    if (go2cpp::config::LoadRuntimeConfig("go2cpp.ini", &config, &error)) config.ApplyLogging(&error);
    static GO2CPP::Logger::ptr logger = GO2CPP_LOG_NAME("LogConfigDemo");
    GO2CPP_LOG_WARN(logger) << "警告会写入配置的日志文件";
    GO2CPP_LOG_ERROR(logger) << "错误也会写入配置的日志文件";
    go2cpp::log::PatternFormatter custom_formatter("%g %m%n");
    go2cpp::log::LogRecord custom_record;
    custom_record.logger = "LogConfigDemo";
    custom_record.message = "自定义格式项已经生效";
    std::cout << custom_formatter.Format(custom_record);
    go2cpp::log::LogFormatter::RemoveFormat("g");
    std::cout << "日志示例完成；默认 stdout 不输出 info，配置 log.stdout=true 后才开启。\n";
    return 0;
}
