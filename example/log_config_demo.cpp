#include "go2cpp/config.hpp"
#include "go2cpp/log.hpp"

#include <iostream>

/**
 * @brief 演示从 INI 文件加载运行时日志配置并创建命名 Logger。
 * @details 配置文件决定输出目录、级别和格式；示例只记录警告与错误，避免
 *          发布模式默认输出大量 Info 日志。
 * @return 配置加载并完成日志演示后返回 0。
 */
int main() {
    go2cpp::config::RuntimeConfig config;
    std::string error;
    if (go2cpp::config::LoadRuntimeConfig("go2cpp.ini", &config, &error)) config.ApplyLogging(&error);
    static GO2CPP::Logger::ptr logger = GO2CPP_LOG_NAME("LogConfigDemo");
    GO2CPP_LOG_WARN(logger) << "警告会写入配置的日志文件";
    GO2CPP_LOG_ERROR(logger) << "错误也会写入配置的日志文件";
    std::cout << "日志示例完成；默认 stdout 不输出 info，配置 log.stdout=true 后才开启。\n";
    return 0;
}
