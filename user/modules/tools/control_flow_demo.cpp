#include "go2cpp/control_flow.hpp"
#include "go2cpp/channel.hpp"
#include "go2cpp/log.hpp"

#include <iostream>
#include <memory>
#include <vector>

namespace {

/**
 * @brief 记录控制流示例的检查结果。
 * @param[in] logger 示例日志记录器。
 * @param[in] condition 需要验证的条件。
 * @param[in] message 场景说明；失败时会写入错误日志。
 * @return 条件满足返回 true，否则返回 false。
 */
bool check(const go2cpp::Logger::ptr& logger, bool condition,
           const char* message) {
    if (condition) {
        GO2CPP_LOG_INFO(logger) << "通过: " << message;
        return true;
    }
    GO2CPP_LOG_ERROR(logger) << "失败: " << message;
    return false;
}

}  // namespace

/**
 * @brief 演示 defer、panic 和 recover 的状态式兼容接口。
 * @details defer 在作用域离开时读取 panic 状态并调用 recover；本项目用显式
 *          状态传递实现控制流，不依赖 C++ 异常或 longjmp。
 * @return panic 已被 recover 时返回 0，否则返回 1。
 */
int main() {
    go2cpp::LoggerManager::Instance().Configure(
        go2cpp::LogLevel::Info, true, "log", "control_flow_demo.log");
    const auto logger = GO2CPP_LOG_NAME("ControlFlowDemo");
    logger->SetLevel(go2cpp::LogLevel::Info);

    // 场景一：参数在 defer 构造时保存，离开作用域按 LIFO 执行。这个例子
    // 用值参数而不是引用参数，便于新手观察“注册时求值”的语义。
    GO2CPP_LOG_INFO(logger) << "场景一：defer 参数求值和 LIFO 顺序";
    std::vector<int> order;
    int evaluated = 3;
    go2cpp::panic failure;
    go2cpp::recover recovery(failure);
    {
        go2cpp::defer first([&] { order.push_back(1); });
        go2cpp::defer second([&] { order.push_back(2); });
        go2cpp::defer captured([&](int value) { order.push_back(value); },
                                evaluated);
        evaluated = 9;
        go2cpp::defer on_scope_exit([&] {
            const auto result = recovery.take();
            if (result.has_value()) {
                std::cout << "recovered: " << result->message
                          << " code=" << result->code << '\n';
            }
        });
        failure.call("demo failure", 7);
        // panic 是显式状态发布；C++ 不会自动跳转，业务代码应在这里
        // return、分支或把状态交给上层。
    }
    // on_scope_exit、captured、second、first 的析构顺序对应逆序注册。
    const bool defer_ok = check(
        logger, order.size() == 3 && order[0] == 3 && order[1] == 2 &&
                    order[2] == 1,
        "defer 已按注册逆序执行，参数仍为注册时的 3");

    // 场景二：panic/recover 只能在 defer 回调内消费，普通代码提前调用
    // take() 会失败；这保留了受控恢复边界，不会伪造 longjmp。
    GO2CPP_LOG_INFO(logger) << "场景二：defer 边界内 recover，边界外拒绝";
    const bool recover_ok = check(
        logger, failure.recovered() && !failure.active(),
        "panic 已被 defer 捕获，恢复后状态不再 active");

    // 场景三：错误路径。move-only 值可以通过 SelectValue 传递，但放入内建
    // Channel Select 时会明确返回 kInvalid；调用方应改用普通 Send/Recv。
    GO2CPP_LOG_INFO(logger) << "场景三：move-only Select 错误路径";
    auto move_channel = go2cpp::MakeChannel<std::unique_ptr<int>>(0);
    const auto invalid_select = go2cpp::Select(
        {go2cpp::RecvCase(move_channel)});
    const bool invalid_ok = check(
        logger, invalid_select.status == go2cpp::ChannelStatus::kInvalid,
        "不可复制元素的内建 Select 被拒绝，避免注册失败后永久等待");

    std::cout << "active=" << std::boolalpha << failure.active()
              << " defer=" << defer_ok << " recover=" << recover_ok
              << " invalid_select=" << invalid_ok << '\n';
    go2cpp::LoggerManager::Instance().Flush();
    return defer_ok && recover_ok && invalid_ok ? 0 : 1;
}
