/**
 * @file error.cpp
 * @brief Bento 错误基础设施的实现：错误图遍历、文本展开与深链释放。
 *
 * @details 两个容易踩坑的点集中在这里：
 * - **深链析构**：10 万层 `Wrap` 直接递归析构会爆栈。`WrappedError` /
 *   `JoinError` 的析构函数把子节点移入线程局部释放队列，由最外层循环统一
 *   释放，析构调用栈深度保持常数。
 * - **环安全**：`Is`/`Has`/`As`/`WalkErrors`/`UnwrapAll(0)` 都用对象地址
 *   `visited` 去重，自定义错误即使返回自己也不会死循环。
 *
 * @author go2cpp contributors
 * @date 2026-10-28
 * @copyright MIT License
 */

#include "go2cpp/bento/error/error.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace go2cpp::bento::error {

namespace {

/**
 * @brief 线程局部释放队列。
 *
 * @details 用“先入队、后统一 reset”的办法把深链析构摊平成循环。`draining`
 *          标记当前线程是否已经在最外层循环里：内层析构只入队，不再递归
 *          展开，因此析构栈深度与错误链长度无关。
 */
struct ReleaseState {
    std::vector<ErrorPtr> pending;
    bool draining = false;

    /**
     * @brief 线程退出时排空剩余节点。
     *
     * @details 析构函数体先于成员析构执行，此时 `pending` 仍然可用；链式
     *          析构把下一批节点重新入队后会回到这里（`draining` 已置位，
     *          内层 `DrainReleaseQueue` 直接返回），由本循环统一释放，
     *          避免 thread_local 清理阶段访问已析构的队列。
     */
    ~ReleaseState() {
        draining = true;
        while (!pending.empty()) {
            ErrorPtr current = std::move(pending.back());
            pending.pop_back();
            current.reset();
        }
    }
};

thread_local ReleaseState g_release_state;

/// @brief 排空当前线程的释放队列；重入时直接返回，由外层循环负责。
void DrainReleaseQueue() noexcept {
    ReleaseState& state = g_release_state;
    if (state.draining) {
        return;
    }
    state.draining = true;
    while (!state.pending.empty()) {
        ErrorPtr current = std::move(state.pending.back());
        state.pending.pop_back();
        current.reset();
    }
    state.draining = false;
}

/// @brief 把一个原因交给释放队列（供 WrappedError 析构使用）。
void EnqueueRelease(ErrorPtr cause) noexcept {
    if (!cause) {
        return;
    }
    ReleaseState& state = g_release_state;
    try {
        state.pending.push_back(std::move(cause));
    } catch (...) {
        // 分配失败时退回直接释放；析构函数是 noexcept，这里不能抛出。
        cause.reset();
        return;
    }
    DrainReleaseQueue();
}

/// @brief 把一批原因交给释放队列（供 JoinError 析构使用）。
void ReleaseAll(std::vector<ErrorPtr>& causes) noexcept {
    ReleaseState& state = g_release_state;
    for (auto& cause : causes) {
        if (!cause) {
            continue;
        }
        try {
            state.pending.push_back(std::move(cause));
        } catch (...) {
            cause.reset();
        }
    }
    DrainReleaseQueue();
}

/**
 * @brief 展开全部后代（宽度优先、地址去重）。
 * @param roots 直接原因（空指针会被跳过）。
 * @return 前序排列的后代节点，不包含 roots 自身。
 */
std::vector<ErrorPtr> ExpandDescendants(const std::vector<ErrorPtr>& roots) {
    std::vector<ErrorPtr> result;
    std::vector<ErrorPtr> pending;
    pending.reserve(roots.size());
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        if (*it) {
            pending.push_back(*it);
        }
    }
    detail::VisitSet visited;
    while (!pending.empty()) {
        ErrorPtr current = std::move(pending.back());
        pending.pop_back();
        if (!current || !visited.Insert(current.get())) {
            continue;
        }
        result.push_back(current);
        if (!current->IsWrapping()) {
            continue;
        }
        const std::vector<ErrorPtr> children = current->UnwrapAll(1);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (*it) {
                pending.push_back(*it);
            }
        }
    }
    return result;
}

/**
 * @brief 在错误图中做前序搜索，任意节点满足谓词即返回 true。
 * @note 环安全；自定义 `UnwrapAll()` 抛异常时返回 false。
 */
template <typename Predicate>
bool TraverseAny(const ErrorPtr& root, Predicate predicate) noexcept {
    try {
        if (!root) {
            return false;
        }
        std::vector<ErrorPtr> pending;
        pending.push_back(root);
        detail::VisitSet visited;
        while (!pending.empty()) {
            ErrorPtr current = std::move(pending.back());
            pending.pop_back();
            if (!current || !visited.Insert(current.get())) {
                continue;
            }
            if (predicate(*current)) {
                return true;
            }
            if (!current->IsWrapping()) {
                continue;
            }
            const std::vector<ErrorPtr> children = current->UnwrapAll();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (*it) {
                    pending.push_back(*it);
                }
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

/// @brief 追加一段非空文本，自动补 `": "` 分隔符。
void AppendMessagePart(std::string& result, const std::string& part) {
    if (part.empty()) {
        return;
    }
    if (!result.empty()) {
        result += ": ";
    }
    result += part;
}

} // namespace

std::string_view ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return "OK";
        case ErrorCode::kUnknown:
            return "UNKNOWN";
        case ErrorCode::kInvalidArgument:
            return "INVALID_ARGUMENT";
        case ErrorCode::kNotFound:
            return "NOT_FOUND";
        case ErrorCode::kAlreadyExists:
            return "ALREADY_EXISTS";
        case ErrorCode::kPermissionDenied:
            return "PERMISSION_DENIED";
        case ErrorCode::kResourceExhausted:
            return "RESOURCE_EXHAUSTED";
        case ErrorCode::kFailedPrecondition:
            return "FAILED_PRECONDITION";
        case ErrorCode::kOutOfRange:
            return "OUT_OF_RANGE";
        case ErrorCode::kUnimplemented:
            return "UNIMPLEMENTED";
        case ErrorCode::kInternal:
            return "INTERNAL";
        case ErrorCode::kUnavailable:
            return "UNAVAILABLE";
        case ErrorCode::kDataLoss:
            return "DATA_LOSS";
        case ErrorCode::kCancelled:
            return "CANCELLED";
        case ErrorCode::kDeadlineExceeded:
            return "DEADLINE_EXCEEDED";
        case ErrorCode::kWrappedError:
            return "WRAPPED_ERROR";
    }
    return "UNKNOWN";
}

std::vector<ErrorPtr> Error::UnwrapAll(std::size_t /*level*/) const {
    return {};
}

WrappedError::WrappedError(std::string message, ErrorPtr cause)
    : m_message(std::move(message)), m_cause(std::move(cause)) {}

WrappedError::~WrappedError() {
    EnqueueRelease(std::move(m_cause));
}

std::string WrappedError::Message() const {
    std::string result;
    // 沿 m_cause 链展开：每层对象的 m_cause 成员在对象存活期间保活下一层，
    // 而 `this` 由调用方保活，因此整条链在调用期间有效，无需逐层复制
    // shared_ptr（10 万层链可省约 20 万次引用计数原子操作）。
    const WrappedError* current = this;
    for (;;) {
        AppendMessagePart(result, current->m_message);
        const ErrorPtr& cause = current->m_cause;
        if (!cause) {
            break;
        }
        const auto* nested = dynamic_cast<const WrappedError*>(cause.get());
        if (nested == nullptr) {
            AppendMessagePart(result, cause->Message());
            break;
        }
        current = nested;
    }
    return result;
}

std::vector<ErrorPtr> WrappedError::UnwrapAll(std::size_t level) const {
    std::vector<ErrorPtr> direct;
    if (m_cause) {
        direct.push_back(m_cause);
    }
    if (level != 0) {
        return direct;
    }
    return ExpandDescendants(direct);
}

JoinError::JoinError(std::vector<ErrorPtr> causes) {
    // 快路径：Join() 已经过滤空指针，直接接管 vector，避免大 Join 再分配
    // 一次并逐元素搬移（10 万原因时可省一次 800 KB 级分配）。
    const bool has_null = std::any_of(causes.begin(), causes.end(),
                                      [](const ErrorPtr& cause) { return !cause; });
    if (!has_null) {
        m_causes = std::move(causes);
        return;
    }
    m_causes.reserve(causes.size());
    for (auto& cause : causes) {
        if (cause) {
            m_causes.push_back(std::move(cause));
        }
    }
}

JoinError::~JoinError() {
    ReleaseAll(m_causes);
}

std::string JoinError::Message() const {
    std::string result;
    std::vector<ErrorPtr> pending;
    pending.reserve(m_causes.size());
    for (auto it = m_causes.rbegin(); it != m_causes.rend(); ++it) {
        if (*it) {
            pending.push_back(*it);
        }
    }
    bool first = true;
    while (!pending.empty()) {
        ErrorPtr current = std::move(pending.back());
        pending.pop_back();
        if (!current) {
            continue;
        }
        if (const auto* nested = dynamic_cast<const JoinError*>(current.get())) {
            for (auto it = nested->m_causes.rbegin(); it != nested->m_causes.rend(); ++it) {
                if (*it) {
                    pending.push_back(*it);
                }
            }
            continue;
        }
        const std::string part = current->Message();
        if (part.empty()) {
            continue;
        }
        if (!first) {
            result += '\n';
        }
        first = false;
        result += part;
    }
    return result;
}

std::vector<ErrorPtr> JoinError::UnwrapAll(std::size_t level) const {
    if (level != 0) {
        return m_causes;
    }
    return ExpandDescendants(m_causes);
}

ErrorPtr NewError(std::string message, ErrorCode code) {
    return std::make_shared<StringError>(std::move(message), code);
}

ErrorPtr Wrap(ErrorPtr cause, std::string message) {
    if (!cause) {
        return {};
    }
    return std::make_shared<WrappedError>(std::move(message), std::move(cause));
}

ErrorPtr Join(std::vector<ErrorPtr> causes) {
    causes.erase(std::remove_if(causes.begin(), causes.end(),
                                [](const ErrorPtr& cause) { return !cause; }),
                 causes.end());
    if (causes.empty()) {
        return {};
    }
    return std::make_shared<JoinError>(std::move(causes));
}

ErrorPtr Join(std::initializer_list<ErrorPtr> causes) {
    return Join(std::vector<ErrorPtr>(causes));
}

std::vector<ErrorPtr> UnwrapAll(const ErrorPtr& error, std::size_t level) {
    return error ? error->UnwrapAll(level) : std::vector<ErrorPtr>{};
}

std::string ErrorMessage(const ErrorPtr& error) {
    return error ? error->Message() : std::string{};
}

WalkResult WalkErrors(const ErrorPtr& error, const ErrorVisitor& visitor) noexcept {
    WalkResult result;
    if (!visitor) {
        result.ok = false;
        return result;
    }
    try {
        std::vector<ErrorPtr> pending;
        if (error) {
            pending.push_back(error);
        }
        detail::VisitSet visited;
        while (!pending.empty()) {
            ErrorPtr current = std::move(pending.back());
            pending.pop_back();
            if (!current || !visited.Insert(current.get())) {
                continue;
            }
            result.errors.push_back(current);
            const ErrorVisitResult decision = visitor(current);
            if (decision == ErrorVisitResult::kStop) {
                result.status = ErrorVisitResult::kStop;
                return result;
            }
            if (decision == ErrorVisitResult::kSkipChildren || !current->IsWrapping()) {
                continue;
            }
            const std::vector<ErrorPtr> children = current->UnwrapAll();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (*it) {
                    pending.push_back(*it);
                }
            }
        }
    } catch (...) {
        result.ok = false;
    }
    return result;
}

bool Is(const ErrorPtr& error, const ErrorPtr& target) noexcept {
    if (!target) {
        return !error;
    }
    if (!error) {
        return false;
    }
    return TraverseAny(error, [&target](const Error& node) { return node.Is(*target); });
}

bool Has(const ErrorPtr& error, ErrorCode code) noexcept {
    return TraverseAny(error, [code](const Error& node) { return node.Code() == code; });
}

} // namespace go2cpp::bento::error
