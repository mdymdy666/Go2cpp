#include "go2cpp/context.hpp"
#include "go2cpp/error.hpp"
#include "go2cpp/log.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {

/**
 * @brief 检查示例断言并记录失败原因。
 * @param[in] logger 示例日志记录器。
 * @param[in] condition 需要验证的条件。
 * @param[in] message 失败时输出的说明。
 * @return 条件满足返回 true，否则记录错误并返回 false。
 */
bool check(const go2cpp::Logger::ptr& logger, bool condition,
           const char* message) {
    if (condition) {
        return true;
    }
    GO2CPP_LOG_ERROR(logger) << "ContextRollback 示例验证失败: " << message;
    return false;
}

/**
 * @brief 记录一个需要补偿的资源步骤。
 * @details 该函数把“申请资源”和“撤销申请”绑定起来，演示真实业务中
 *          应由 undo 回调释放临时状态，而不是试图恢复任意外部副作用。
 * @param[in,out] scope 当前回滚作用域。
 * @param[in,out] live_resources 当前仍持有的资源数量。
 * @param[in] name 当前步骤名称，仅用于日志。
 * @param[in] logger 示例日志记录器。
 * @return 登记 undo 成功返回 true；作用域已经提交或回滚时返回 false。
 */
bool reserve_resource(go2cpp::ContextRollback& scope, int& live_resources,
                      const char* name,
                      const go2cpp::Logger::ptr& logger) {
    ++live_resources;
    if (!scope.record_undo([&live_resources]() noexcept { --live_resources; })) {
        --live_resources;
        GO2CPP_LOG_ERROR(logger) << "步骤 " << name
                                 << " 未能登记补偿动作，资源立即撤销";
        return false;
    }
    GO2CPP_LOG_INFO(logger) << "步骤 " << name << " 已完成，资源数="
                            << live_resources;
    return true;
}

}  // namespace

/**
 * @brief 演示 ContextRollback 的成功、局部回滚、取消传播和错误路径。
 * @details 本示例刻意把每条路径的“动作、补偿、状态和结果”都打印出来：
 *          1. 成功路径提交后不再执行 undo；
 *          2. 多步骤流程用 savepoint 撤销后半段，再显式回滚剩余步骤；
 *          3. 父 Context 取消会驱动 child 自动执行补偿；
 *          4. deadline 到期会自动取消 child 并执行补偿；
 *          5. undo 抛出异常时继续执行其余补偿并把事务标记为失败；
 *          6. 失效 savepoint 会被拒绝，避免 ABA 式的错误回滚。
 * @return 所有验证通过返回 0，否则返回 1。
 */
int main() {
    using namespace go2cpp;
    using namespace std::chrono_literals;

    // 示例默认只输出 Warn；这里显式开启 Info，方便新手看到每个状态变化。
    LoggerManager::Instance().Configure(LogLevel::Info, true, "log",
                                        "context_rollback_demo.log");
    const auto logger = GO2CPP_LOG_NAME("ContextRollbackDemo");
    logger->SetLevel(LogLevel::Info);

    const auto root = Background();
    int live_resources = 0;

    // 场景一：成功路径。commit() 丢弃 undo，但不会取消 child Context。
    GO2CPP_LOG_INFO(logger) << "场景一：成功提交，补偿动作不应执行";
    {
        ContextRollback scope(root);
        if (!check(logger, reserve_resource(scope, live_resources, "连接池槽位",
                                            logger),
                   "成功步骤必须登记 undo") ||
            !check(logger, scope.commit(), "活动事务应当可以提交") ||
            !check(logger, scope.rollback_done().WaitFor(1s),
                   "commit 后 RollbackDone 必须完成") ||
            !check(logger, live_resources == 1,
                   "提交后资源仍由业务持有，不应被回滚")) {
            return 1;
        }
        GO2CPP_LOG_INFO(logger) << "场景一通过: status=committed, resource_count="
                                << live_resources;
    }

    // 场景二：多步骤流程。savepoint 只撤销其后的动作，之后再完整回滚。
    GO2CPP_LOG_INFO(logger) << "场景二：savepoint 局部回滚，再回滚剩余步骤";
    {
        ContextRollback scope(root);
        if (!check(logger,
                   reserve_resource(scope, live_resources, "临时文件", logger),
                   "临时文件步骤登记失败")) {
            return 1;
        }
        const auto mark = scope.savepoint();
        if (!check(logger,
                   reserve_resource(scope, live_resources, "socket 注册", logger),
                   "socket 注册步骤登记失败") ||
            !check(logger, scope.rollback_to(mark),
                   "有效 savepoint 应能撤销后续步骤") ||
            !check(logger, live_resources == 2,
                   "局部回滚应只撤销 socket 注册") ||
            !check(logger, !mark.valid(),
                   "局部回滚后旧 savepoint 必须失效，防止 ABA")) {
            return 1;
        }
        GO2CPP_LOG_INFO(logger)
            << "savepoint 已完成，临时文件仍在，继续执行最终校验";
        if (!check(logger, scope.rollback(), "剩余步骤应能完整回滚") ||
            !check(logger, scope.rollback_done().WaitFor(1s),
                   "完整回滚必须发布 RollbackDone") ||
            !check(logger, live_resources == 1,
                   "场景二只撤销本场景资源，已提交场景一保持不变")) {
            return 1;
        }
        GO2CPP_LOG_INFO(logger) << "场景二通过: 局部回滚和 LIFO 补偿完成";
    }

    // 场景三：父 Context 取消。取消只驱动当前 scope 的 undo，不恢复父状态。
    GO2CPP_LOG_INFO(logger) << "场景三：父 Context 取消，自动执行补偿";
    {
        const auto parent_cancel = WithCancel(root);
        ContextRollback scope(parent_cancel.first);
        if (!check(logger,
                   reserve_resource(scope, live_resources, "请求租约", logger),
                   "请求租约步骤登记失败")) {
            return 1;
        }
        parent_cancel.second();
        if (!check(logger, scope.rollback_done().WaitFor(1s),
                   "父取消后 undo 必须完成") ||
            !check(logger, scope.rolled_back(),
                   "父取消后的事务状态应为 rolled_back") ||
            !check(logger, live_resources == 1,
                   "父取消应释放请求租约，但不影响场景一资源") ||
            !check(logger, Is(scope.context()->Err(), CanceledError()),
                   "child Context 错误应为 Canceled")) {
            return 1;
        }
        GO2CPP_LOG_INFO(logger) << "场景三通过: parent cancel -> child Done -> undo";
    }

    // 场景四：截止时间路径。TimerService 到期后会取消 child 并自动回滚。
    GO2CPP_LOG_INFO(logger) << "场景四：deadline 到期，自动取消并执行补偿";
    {
        const auto timeout_parent = WithTimeout(root, 25ms);
        ContextRollback scope(timeout_parent.first);
        if (!check(logger,
                   reserve_resource(scope, live_resources, "短期令牌", logger),
                   "短期令牌步骤登记失败") ||
            !check(logger, scope.rollback_done().WaitFor(1s),
                   "deadline 到期后 undo 必须完成") ||
            !check(logger, scope.rolled_back(),
                   "deadline 到期后的事务状态应为 rolled_back") ||
            !check(logger, live_resources == 1,
                   "deadline 回滚应释放短期令牌") ||
            !check(logger, Is(scope.context()->Err(), DeadlineExceededError()),
                   "deadline 到期的 child 错误应为 DeadlineExceeded")) {
            return 1;
        }
        GO2CPP_LOG_INFO(logger) << "场景四通过: deadline -> cancel -> undo";
    }

    // 场景五：错误路径。undo 抛异常不会阻止剩余动作执行，事务进入 failed。
    GO2CPP_LOG_INFO(logger) << "场景五：补偿动作异常，验证失败状态和资源清理";
    {
        ContextRollback scope(root);
        ++live_resources;
        if (!check(logger,
                   scope.record_undo([&live_resources]() {
                       --live_resources;
                       throw std::runtime_error("模拟释放失败");
                   }),
                   "异常 undo 应先成功登记") ||
            !check(logger, scope.rollback(NewError("业务步骤失败")),
                   "显式 rollback 应领取异常 undo") ||
            !check(logger, scope.rollback_done().WaitFor(1s),
                   "异常 undo 也必须完成 RollbackDone") ||
            !check(logger, scope.had_failure(),
                   "undo 异常应记录 had_failure") ||
            !check(logger, scope.status() == ContextRollback::Status::kFailed,
                   "undo 异常后的状态应为 failed") ||
            !check(logger, live_resources == 1,
                   "即使 undo 抛异常，动作已经执行，资源不能泄漏")) {
            return 1;
        }
        GO2CPP_LOG_WARN(logger)
            << "场景五按预期失败: 业务必须检查 had_failure，不能只看 rollback 返回值";
    }

    // 场景六：错误的旧令牌。不能重复使用已经消费的 savepoint。
    GO2CPP_LOG_INFO(logger) << "场景六：重复使用失效 savepoint，验证拒绝行为";
    {
        ContextRollback scope(root);
        if (!check(logger,
                   reserve_resource(scope, live_resources, "一次性句柄", logger),
                   "句柄步骤登记失败")) {
            return 1;
        }
        const auto mark = scope.savepoint();
        if (!check(logger, scope.rollback_to(mark), "第一次 rollback_to 应成功") ||
            !check(logger, !scope.rollback_to(mark),
                   "第二次使用旧 token 必须失败") ||
            !check(logger, scope.rollback(), "清理剩余资源应成功") ||
            !check(logger, live_resources == 1,
                   "失效 token 不得重复执行补偿")) {
            return 1;
        }
        GO2CPP_LOG_WARN(logger)
            << "场景六按预期拒绝失效 savepoint；调用方应重新调用 savepoint()";
    }

    if (!check(logger, live_resources == 1,
               "所有临时事务结束后只能保留已提交的资源")) {
        return 1;
    }
    GO2CPP_LOG_INFO(logger) << "ContextRollback 示例全部通过，最终资源数="
                            << live_resources;
    std::cout << "context rollback demo passed: 6 scenarios\n";
    LoggerManager::Instance().Flush();
    return 0;
}
