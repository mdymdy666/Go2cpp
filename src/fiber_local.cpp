#include "go2cpp/fiber_local.hpp"

#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace go2cpp::fiber_local::detail {
namespace {

using ValueMap = std::unordered_map<KeyId, std::shared_ptr<void>>;

thread_local ValueMap s_thread_values;

struct Registry final {
    std::mutex m_mutex;
    std::unordered_map<Fiber*, ValueMap> m_values;
};

Registry& registry() noexcept {
    // Keep the registry until process exit: Fiber objects may be destroyed
    // during static teardown, when ordinary static destruction order is not
    // sufficient to guarantee that a mutex still exists.
    static Registry* s_registry = new Registry();
    return *s_registry;
}

std::atomic<KeyId> s_next_key{1};

}  // namespace

ThreadValueMap& ThreadValues() noexcept { return s_thread_values; }

KeyId AllocateKey() noexcept {
    // Zero is reserved as the invalid/sentinel key.  A fetch_add based
    // allocator would wrap to an earlier live key after UINT64_MAX and could
    // alias values belonging to another FiberLocalCache.  Publish zero as an
    // exhausted sentinel instead; the practically unreachable next allocation
    // terminates rather than silently violating isolation.
    KeyId candidate = s_next_key.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == 0) {
            std::terminate();
        }
        const KeyId next =
            candidate == std::numeric_limits<KeyId>::max() ? 0 : candidate + 1;
        if (s_next_key.compare_exchange_weak(candidate, next,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            return candidate;
        }
    }
}

std::shared_ptr<void> Get(Fiber* fiber, KeyId key) noexcept {
    if (!fiber || key == 0) {
        return {};
    }
    Registry& store = registry();
    std::lock_guard<std::mutex> lock(store.m_mutex);
    const auto fiber_found = store.m_values.find(fiber);
    if (fiber_found == store.m_values.end()) {
        return {};
    }
    const auto value_found = fiber_found->second.find(key);
    return value_found == fiber_found->second.end() ? std::shared_ptr<void>{}
                                                     : value_found->second;
}

void Set(Fiber* fiber, KeyId key, std::shared_ptr<void> value) {
    if (!fiber || key == 0) {
        return;
    }
    Registry& store = registry();
    std::lock_guard<std::mutex> lock(store.m_mutex);
    store.m_values[fiber][key] = std::move(value);
}

void Reset(Fiber* fiber, KeyId key) noexcept {
    if (!fiber || key == 0) {
        return;
    }
    Registry& store = registry();
    for (;;) {
        std::shared_ptr<void> value;
        {
            std::lock_guard<std::mutex> lock(store.m_mutex);
            const auto fiber_found = store.m_values.find(fiber);
            if (fiber_found == store.m_values.end()) {
                return;
            }
            const auto value_found = fiber_found->second.find(key);
            if (value_found == fiber_found->second.end()) {
                return;
            }
            value = std::move(value_found->second);
            fiber_found->second.erase(value_found);
            if (fiber_found->second.empty()) {
                store.m_values.erase(fiber_found);
            }
        }
        // Never invoke user destructors while holding the registry mutex.
        value.reset();
    }
}

void Cleanup(Fiber* fiber) noexcept {
    if (!fiber) {
        return;
    }
    Registry& store = registry();
    for (;;) {
        ValueMap values;
        {
            std::lock_guard<std::mutex> lock(store.m_mutex);
            const auto found = store.m_values.find(fiber);
            if (found == store.m_values.end()) {
                return;
            }
            values = std::move(found->second);
            store.m_values.erase(found);
        }
        // A value destructor may create another local; clean that wave too.
        // Destructors execute without the registry lock and must not suspend
        // the Fiber; doing so would hold this cleanup continuation on its stack.
        values.clear();
    }
}

}  // namespace go2cpp::fiber_local::detail
