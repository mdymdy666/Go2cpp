/**
 * @file config_app_ini.cpp
 * @brief 演示：静态注册配置 -> 加载 app.ini -> 运行时调整 -> 导出 app.ini。
 *
 * ## 用法
 *
 * 在**项目根目录**运行本程序，会在当前目录生成/更新 `app.ini`：
 *
 * ```sh
 * ./build-quality/go2cpp_config_app_ini
 * cat app.ini
 * ```
 *
 * 第二次运行时，程序会先读取已有的 `app.ini` 覆盖内置默认值，
 * 再应用本次的运行时调整，最后写回 —— 这就是「配置文件即状态」的闭环。
 *
 * ## 这个示例展示了什么
 *
 * 1. `GO2CPP_CONFIG_REGISTER` 在静态初始化期注册，`main` 里直接可用；
 * 2. `LoadFromFile` 读取磁盘配置覆盖默认值；
 * 3. `SetValue` 做运行时调整（校验器会拒绝非法值）；
 * 4. `SaveToFile` 原子导出（临时文件 + rename），生成带注释的 INI；
 * 5. `Groups()` 按 module 展示分组。
 */
#include "go2cpp/config/config.hpp"

#include <cstdio>
#include <fstream>
#include <string>

// 静态注册：在 main 之前完成。module 会与 key 拼成 "module.key"。
GO2CPP_CONFIG_REGISTER("app", int, port, 8080, "HTTP 监听端口");
GO2CPP_CONFIG_REGISTER("app", std::string, name, std::string("go2cpp-demo"),
                       "服务名字，会出现在日志里");
GO2CPP_CONFIG_REGISTER("app", bool, debug, false, "是否开启调试日志");
GO2CPP_CONFIG_REGISTER("log", std::string, level, std::string("warn"),
                       "日志级别: debug|info|warn|error");
GO2CPP_CONFIG_REGISTER("log", int, buffer_size, 4096,
                       "日志缓冲区字节数\n必须是 2 的幂");
GO2CPP_CONFIG_REGISTER("workers", int, count, 8, "工作线程/P 数量（需重启生效）");

int main() {
    auto& cfg = go2cpp::config::Config::Instance();
    const std::string path = "app.ini";

    // ---- 1. 如果已有配置文件，先读进来覆盖默认值 ----
    {
        std::ifstream probe(path);
        if (probe.good()) {
            probe.close();
            std::string error;
            if (cfg.LoadFromFile(path, &error)) {
                std::printf("[1] 已从 %s 加载既有配置\n", path.c_str());
            } else {
                std::printf("[1] 加载 %s 失败: %s（继续用默认值）\n",
                            path.c_str(), error.c_str());
            }
        } else {
            std::printf("[1] %s 不存在，使用内置默认值\n", path.c_str());
        }
    }

    // ---- 2. 打印当前值 ----
    std::printf("[2] 当前配置:\n");
    std::printf("    app.port        = %d\n",
                cfg.Lookup<int>("app.port", 0, "")->GetValue());
    std::printf("    app.name        = %s\n",
                cfg.Lookup<std::string>("app.name", "", "")->GetValue().c_str());
    std::printf("    app.debug       = %s\n",
                cfg.Lookup<bool>("app.debug", false, "")->GetValue() ? "true" : "false");
    std::printf("    log.level       = %s\n",
                cfg.Lookup<std::string>("log.level", "", "")->GetValue().c_str());
    std::printf("    log.buffer_size = %d\n",
                cfg.Lookup<int>("log.buffer_size", 0, "")->GetValue());
    std::printf("    workers.count   = %d\n",
                cfg.Lookup<int>("workers.count", 0, "")->GetValue());

    // ---- 3. 运行时调整（演示校验器拒绝非法值）----
    auto level = cfg.Lookup<std::string>("log.level", "", "");
    // 给 log.level 挂一个校验器：只接受四个合法级别
    level->SetValidator([](const std::string& value, std::string* error) {
        if (value == "debug" || value == "info" || value == "warn" ||
            value == "error") {
            return true;
        }
        if (error) {
            *error = "log.level 必须是 debug|info|warn|error，收到: " + value;
        }
        return false;
    });

    std::string error;
    if (!level->SetValue("verbose", &error)) {
        std::printf("[3] 非法值被拒绝（预期）: %s\n", error.c_str());
    }
    if (level->SetValue("info", &error)) {
        std::printf("[3] 运行时调整 log.level -> info 成功\n");
    }

    auto port = cfg.Lookup<int>("app.port", 0, "");
    if (port->SetValue(9090, &error)) {
        std::printf("[3] 运行时调整 app.port -> 9090 成功\n");
    }

    // ---- 4. 按 module 展示分组 ----
    std::printf("[4] 分组:\n");
    for (const auto& [module, vars] : cfg.Groups()) {
        std::printf("    [%s] %zu 项\n", module.c_str(), vars.size());
    }

    // ---- 5. 原子导出到 app.ini ----
    if (cfg.SaveToFile(path, &error)) {
        std::printf("[5] 已导出 -> %s\n", path.c_str());
    } else {
        std::printf("[5] 导出失败: %s\n", error.c_str());
        return 1;
    }

    // ---- 6. 打印生成的文件（前若干行）----
    std::printf("[6] 生成内容预览:\n");
    {
        std::ifstream in(path);
        std::string line;
        int shown = 0;
        while (std::getline(in, line) && shown < 24) {
            std::printf("    | %s\n", line.c_str());
            ++shown;
        }
    }
    return 0;
}
