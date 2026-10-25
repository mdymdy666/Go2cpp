/**
 * @file weather.cpp
 * @brief 天气工具：调用 open-meteo 公开接口，聚合当日温度曲线。
 *
 * @details
 * 参考项目直接 `httplib::Client` + 手工截取小时数组，且假设数组一定有 24 项
 * （越界就崩）。这里：
 * - 用 Go2Cpp 的 `http::Get`（共享 Socket/解析器实现，超时可控）；
 * - 对 HTTP 状态、JSON 结构、数组长度都做检查，不满足时返回真实错误；
 * - 分段平均值只对实际存在的小时计算，不写死下标。
 */

#include "mcp/tools/builtin_tools.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "go2cpp/network/http.hpp"

namespace go2cpp::mcp::tools {

namespace {

/// 读取字符串参数，缺失时返回空串。
std::string StringArgument(const json& arguments, const char* name) {
    const auto it = arguments.find(name);
    if (it == arguments.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

/// 把纬度/经度限制在合法范围，避免拿任意字符串拼 URL。
bool ValidCoordinate(const std::string& text, double low, double high) {
    if (text.empty()) {
        return false;
    }
    try {
        const double value = std::stod(text);
        return std::isfinite(value) && value >= low && value <= high;
    } catch (const std::exception&) {
        return false;
    }
}

/// 求 [begin, end) 区间平均值；区间为空时返回 NaN。
double Average(const std::vector<double>& values, std::size_t begin, std::size_t end) {
    if (begin >= end || end > values.size()) {
        return std::nan("");
    }
    double sum = 0.0;
    for (std::size_t index = begin; index < end; ++index) {
        sum += values[index];
    }
    return sum / static_cast<double>(end - begin);
}

std::string FormatTemperature(double value) {
    if (std::isnan(value)) {
        return "n/a";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

}  // namespace

void RegisterWeatherTool(Registry& registry) {
    Tool tool;
    tool.name = "get_weather";
    tool.description = "查询城市当日逐小时温度预报（数据源 open-meteo）";
    tool.input_schema = json{{"type", "object"},
                             {"properties", {{"city", {{"type", "string"}}},
                                             {"latitude", {{"type", "string"}}},
                                             {"longitude", {{"type", "string"}}}}},
                             {"required", json::array({"city", "latitude", "longitude"})}};
    tool.handler = [](const ToolContext& context) {
        const std::string city = StringArgument(context.arguments, "city");
        const std::string latitude = StringArgument(context.arguments, "latitude");
        const std::string longitude = StringArgument(context.arguments, "longitude");

        if (city.empty()) {
            return ToolResult::Text("缺少字符串参数 city", true);
        }
        if (!ValidCoordinate(latitude, -90.0, 90.0)) {
            return ToolResult::Text("latitude 必须是 [-90, 90] 内的数字字符串", true);
        }
        if (!ValidCoordinate(longitude, -180.0, 180.0)) {
            return ToolResult::Text("longitude 必须是 [-180, 180] 内的数字字符串", true);
        }

        const std::string url = "http://api.open-meteo.com/v1/forecast?latitude=" + latitude +
                                "&longitude=" + longitude +
                                "&hourly=temperature_2m&forecast_days=1";
        const auto response = go2cpp::http::Get(url, std::chrono::seconds(15));
        if (!response.ok()) {
            return ToolResult::Text("天气服务请求失败: " + response.message, true);
        }
        if (response.response->status != go2cpp::http::Status::OK) {
            return ToolResult::Text("天气服务返回 HTTP " +
                                        std::to_string(static_cast<int>(response.response->status)),
                                    true);
        }

        json document;
        try {
            document = json::parse(response.response->body);
        } catch (const json::exception& error) {
            return ToolResult::Text(std::string("天气服务返回非法 JSON: ") + error.what(), true);
        }

        const auto hourly = document.find("hourly");
        if (hourly == document.end()) {
            return ToolResult::Text("天气服务响应缺少 hourly 字段", true);
        }
        const auto times_it = hourly->find("time");
        const auto temps_it = hourly->find("temperature_2m");
        if (times_it == hourly->end() || temps_it == hourly->end() || !times_it->is_array() ||
            !temps_it->is_array() || times_it->size() != temps_it->size()) {
            return ToolResult::Text("天气服务响应缺少对齐的 hourly.time / hourly.temperature_2m", true);
        }

        std::vector<double> temperatures;
        temperatures.reserve(temps_it->size());
        for (const auto& item : *temps_it) {
            if (!item.is_number()) {
                return ToolResult::Text("天气服务温度字段包含非数字项", true);
            }
            temperatures.push_back(item.get<double>());
        }
        if (temperatures.size() < 24) {
            return ToolResult::Text("天气服务只返回了 " + std::to_string(temperatures.size()) +
                                        " 个小时点，不足 24 个",
                                    true);
        }

        // 早 / 午 / 晚三个 6 小时区间
        const double morning = Average(temperatures, 6, 12);
        const double afternoon = Average(temperatures, 12, 18);
        const double evening = Average(temperatures, 18, 24);

        double highest = temperatures.front();
        double lowest = temperatures.front();
        std::string highest_time;
        std::string lowest_time;
        for (std::size_t index = 0; index < temperatures.size(); ++index) {
            const double value = temperatures[index];
            if (value > highest) {
                highest = value;
                highest_time = times_it->at(index).get<std::string>();
            }
            if (value < lowest) {
                lowest = value;
                lowest_time = times_it->at(index).get<std::string>();
            }
        }

        std::string text = "Weather Forecast for " + city + ":\n\n";
        text += "Morning: " + FormatTemperature(morning) + " C\n";
        text += "Afternoon: " + FormatTemperature(afternoon) + " C\n";
        text += "Evening: " + FormatTemperature(evening) + " C\n\n";
        text += "Highest: " + FormatTemperature(highest) + " C at " +
                (highest_time.empty() ? std::string("n/a") : highest_time) + "\n";
        text += "Lowest: " + FormatTemperature(lowest) + " C at " +
                (lowest_time.empty() ? std::string("n/a") : lowest_time) + "\n";

        return ToolResult::Text(text);
    };
    registry.RegisterTool(std::move(tool));
}

}  // namespace go2cpp::mcp::tools
