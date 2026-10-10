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
    std::clog << "[log-config][场景1] 注册自定义格式项并输出 Warn/Error\n";
    go2cpp::config::RuntimeConfig config;
    std::string error;
    // 必须先注册格式项，再创建 PatternFormatter；Formatter 构造时会保存 Item 快照。
    go2cpp::log::LogFormatter::AddFormat(
        "g", [](const std::string&) { return std::make_shared<DemoFormatItem>(); });
    if (go2cpp::config::LoadRuntimeConfig("go2cpp.ini", &config, &error)) {
        std::clog << "[log-config][场景1] go2cpp.ini 加载成功\n";
        if (!config.ApplyLogging(&error)) {
            std::clog << "[log-config][场景1] 应用配置失败: " << error << '\n';
        }
    } else {
        // 示例目录不一定有 go2cpp.ini；缺少可选配置时仍使用 LoggerManager
        // 的默认安全值，不能把“文件不存在”误当成日志系统崩溃。
        std::clog << "[log-config][场景1] 未加载 go2cpp.ini，使用默认日志配置: "
                  << error << '\n';
    }
    static GO2CPP::Logger::ptr logger = GO2CPP_LOG_NAME("LogConfigDemo");
    GO2CPP_LOG_WARN(logger) << "警告会写入配置的日志文件";
    GO2CPP_LOG_ERROR(logger) << "错误也会写入配置的日志文件";
    go2cpp::log::PatternFormatter custom_formatter("%g %m%n");
    go2cpp::log::LogRecord custom_record;
    custom_record.logger = "LogConfigDemo";
    custom_record.message = "自定义格式项已经生效";
    std::cout << custom_formatter.Format(custom_record);
    go2cpp::log::LogFormatter::RemoveFormat("g");

    std::clog << "[log-config][场景2] INI 合法文本可以被读取\n";
    go2cpp::config::IniFile ini;
    std::string parse_error;
    const bool valid_ini = ini.Parse("[log]\nlevel=info\nstdout=true\n",
                                     &parse_error);
    const bool valid_value = valid_ini && ini.Get("log", "level") == "info";
    std::clog << "[log-config][场景2] valid=" << valid_ini
              << " level=" << ini.Get("log", "level") << '\n';

    std::clog << "[log-config][场景3] 错误路径：非法节名不会覆盖旧配置\n";
    parse_error.clear();
    const bool invalid_ini = ini.Parse("[log\nlevel=debug\n", &parse_error);
    const bool transaction_kept_old = !invalid_ini &&
                                      ini.Get("log", "level") == "info";
    std::clog << "[log-config][场景3] rejected=" << !invalid_ini
              << " old_level_kept=" << transaction_kept_old
              << " error=" << parse_error << '\n';

    std::cout << "日志示例完成；默认 stdout 不输出 info，配置 log.stdout=true 后才开启。\n";
    go2cpp::LoggerManager::Instance().Flush();
    return valid_value && transaction_kept_old ? 0 : 1;
}
