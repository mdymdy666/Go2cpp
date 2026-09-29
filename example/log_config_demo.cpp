#include "go2cpp/config.hpp"
#include "go2cpp/log.hpp"

#include <iostream>

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
