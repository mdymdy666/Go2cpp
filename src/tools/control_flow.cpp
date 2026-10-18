/**
 * @file control_flow.cpp
 * @brief `tools/control_flow.hpp` 的实现：defer 执行、panic 状态与 recover 边界。
 *
 * ## 模块定位
 *
 * 控制流原语的实现层：`defer` 的回调执行与异常记录、`panic` 的共享状态读写、
 * `recover` 在 defer 回调内的消费逻辑。被 Fiber 任务与调度器入口间接使用。
 *
 * ## 本文件定义什么
 *
 * - `detail::PanicState`：带 mutex 的 panic 状态；
 * - 线程本地的 defer 深度/异常记录与 `DeferCallbackScope`；
 * - `defer::run_now`、`panic::call/clear`、`recover::take/operator()/peek`。
 *
 * ## 阅读建议
 *
 * 先读 `defer::run_now` 的「先清 active 再执行」，再看 `recover::take` 的
 * defer 边界检查；`PanicState` 的锁保护的是 `info`/`active`/`recovered` 三者。
 *
 * ## 边界与陷阱
 *
 * `take()` 只在当前线程的 defer 回调内成功；`panic::call` 不跳栈；defer
 * 回调里的异常只记录到 `LastDeferException()`，不会继续传播。
 */
#include "go2cpp/control_flow.hpp"

#include <mutex>

namespace go2cpp {
namespace detail {

struct PanicState {
    mutable std::mutex mutex; ///< 保护 info/active/recovered；panic 与 recover 的所有读写都持锁
    PanicInfo info;           ///< 当前 panic 快照；call_info() 写，take()/peek() 读
    bool active{false};       ///< 是否处于已触发未恢复；call_info() 置真，take()/operator()/clear() 置假
    bool recovered{false};    ///< 是否已被 recover 处理；take()/operator() 置真，call_info()/clear() 置假
};

thread_local unsigned int s_defer_depth = 0; ///< 当前线程 defer 回调嵌套深度；DeferCallbackScope 增减
thread_local std::exception_ptr s_defer_exception; ///< 最近一次 defer 回调异常；RecordDeferException 写，LastDeferException 读

class DeferCallbackScope final {
public:
    DeferCallbackScope() noexcept : m_previous(s_defer_depth) {
        ++s_defer_depth;
    }

    ~DeferCallbackScope() noexcept { s_defer_depth = m_previous; }

private:
    unsigned int m_previous; ///< 进入作用域前的深度；析构时恢复，支持嵌套 defer
};

bool InDeferCallback() noexcept { return s_defer_depth != 0; }

void RecordDeferException(std::exception_ptr exception) noexcept {
    s_defer_exception = std::move(exception);
}

std::exception_ptr LastDeferException() noexcept {
    return s_defer_exception;
}

void ClearDeferException() noexcept { s_defer_exception = {}; }

}  // namespace detail

defer::~defer() noexcept { run_now(); }

/**
 * @brief 立即执行尚未取消的回调。
 *
 * ## 步骤
 * step1. `m_active` 为假直接返回（已执行/已取消）；
 * step2. 先清 `m_active`，保证回调里再次触发也不会重入；
 * step3. 进入 `DeferCallbackScope`，让 `recover::take()` 识别当前处于 defer 边界；
 * step4. 执行回调；回调异常由 `CallableModel::run()` 捕获并记录。
 */
void defer::run_now() noexcept {
    // step1: 已执行或已 dismiss
    if (!m_active) {
        return;
    }
    // step2: 先置为不活动，避免回调内重入
    m_active = false;
    if (!m_callable) {
        return;
    }
    // step3: 标记 defer 回调作用域，recover 只在此边界内生效
    detail::DeferCallbackScope scope;
    // step4: 执行；异常在 CallableModel 内被吞掉并记录
    m_callable->run();
}

panic::panic() : m_state(std::make_shared<detail::PanicState>()) {}

panic::~panic() = default;

void panic::call(std::string message, int code) {
    PanicInfo info;
    info.raised = true;
    info.message = std::move(message);
    info.code = code;
    call_info(std::move(info));
}

void panic::call(const char* message, int code) {
    call(message == nullptr ? std::string{} : std::string(message), code);
}

/**
 * @brief 发布一次 panic 状态（所有 `call` 重载的汇合点）。
 *
 * ## 步骤
 * step1. 防御空状态（拷贝后状态可能被清空）；
 * step2. 持锁覆盖 `info`；
 * step3. 置 `active=true`、`recovered=false`，允许覆盖之前的 panic。
 */
void panic::call_info(PanicInfo info) {
    // step1: 兜底创建共享状态
    if (!m_state) {
        m_state = std::make_shared<detail::PanicState>();
    }
    // step2/step3: 持锁发布并复位 recovered
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->info = std::move(info);
    m_state->active = true;
    m_state->recovered = false;
}

bool panic::active() const noexcept {
    if (!m_state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->active;
}

bool panic::recovered() const noexcept {
    if (!m_state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->recovered;
}

PanicInfo panic::info() const {
    if (!m_state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->info;
}

void panic::clear() noexcept {
    if (!m_state) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->info.payload.reset();
    m_state->info.message.clear();
    m_state->info.code = 0;
    m_state->info.raised = false;
    m_state->active = false;
    m_state->recovered = false;
}

bool recover::bind(panic& source) noexcept {
    m_state = source.m_state;
    return !m_state.expired();
}

bool recover::active() const noexcept {
    const auto state = m_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->active;
}

/**
 * @brief 在 defer 回调内取走并确认 panic。
 *
 * ## 步骤
 * step1. 非 defer 回调作用域直接失败（保留恢复边界）；
 * step2. weak_ptr 升级失败（panic 已销毁）返回空；
 * step3. 持锁检查 `active`，未触发/已恢复返回空；
 * step4. 移走 `info` 并清空，置 `active=false`、`recovered=true`；
 * step5. 返回快照。
 *
 * ## 并发语义
 * 成功只发生一次：两个并发 `take()` 只有一个能拿到非空结果。
 */
std::optional<PanicInfo> recover::take() const {
    // step1: 只允许在 defer 回调内恢复
    if (!detail::InDeferCallback()) {
        return std::nullopt;
    }
    // step2: 绑定的 panic 可能已销毁
    const auto state = m_state.lock();
    if (!state) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    // step3: 没有活动 panic
    if (!state->active) {
        return std::nullopt;
    }
    // step4: 取走并标记 recovered
    PanicInfo result = std::move(state->info);
    state->info = {};
    state->active = false;
    state->recovered = true;
    // step5: 返回快照
    return result;
}

/**
 * @brief 恢复并丢弃 panic 内容，只返回是否成功。
 *
 * ## 步骤
 * step1. 非 defer 回调作用域返回 false；
 * step2. 升级 weak_ptr 并持锁；
 * step3. 没有活动 panic 返回 false；
 * step4. 清空 payload/文本/错误码，置 recovered 并返回 true。
 */
bool recover::operator()() const noexcept {
    // step1: 恢复边界检查
    if (!detail::InDeferCallback()) {
        return false;
    }
    // step2: 绑定状态已失效
    const auto state = m_state.lock();
    if (!state) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    // step3: 没有可恢复的 panic
    if (!state->active) {
        return false;
    }
    // step4: 丢弃内容并标记已恢复
    state->info.payload.reset();
    state->info.message.clear();
    state->info.code = 0;
    state->info.raised = false;
    state->active = false;
    state->recovered = true;
    return true;
}

PanicInfo recover::peek() const {
    const auto state = m_state.lock();
    if (!state) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->info;
}

std::exception_ptr LastDeferException() noexcept {
    return detail::LastDeferException();
}

void ClearDeferException() noexcept {
    detail::ClearDeferException();
}

}  // namespace go2cpp
