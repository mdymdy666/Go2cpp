#pragma once

#include "go2cpp/fiber.hpp"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace go2cpp::fiber_local::detail {

using KeyId = std::uint64_t;

// Key allocation is process-local and monotonically increasing. A key is
// never reused during the lifetime of the process.
KeyId AllocateKey() noexcept;

std::shared_ptr<void> Get(Fiber* fiber, KeyId key) noexcept;
void Set(Fiber* fiber, KeyId key, std::shared_ptr<void> value);
void Reset(Fiber* fiber, KeyId key) noexcept;
void Cleanup(Fiber* fiber) noexcept;

using ThreadValueMap =
    std::unordered_map<KeyId, std::shared_ptr<void>>;

// Defined in the Fiber module so all translation units/DSOs use one TLS map
// per OS thread. A template-local static could silently create one map per DSO.
ThreadValueMap& ThreadValues() noexcept;

}  // namespace go2cpp::fiber_local::detail

namespace go2cpp {

/**
 * A named value slot with Fiber-local lifetime.
 *
 * Values obtained inside a Fiber belong to that G, survive migration between
 * M threads, and are destroyed when the Fiber trampoline finishes. Calls made
 * outside a Fiber use a per-thread fallback destroyed at thread exit.
 *
 * Access the slot only from the current execution context. The registry is
 * mutex-protected for migration/teardown, but a raw T* returned by TryGet()
 * must not be retained after the current Fiber calls Reset or completes.
 * Value destructors should be noexcept and must not suspend the Fiber.
 *
 * This stores values, not reusable Fiber stacks. Stack pooling is a separate
 * backend concern because a stack can finish on a different M and must retain
 * sanitizer/guard-page invariants.
 */
template <typename T>
class FiberLocalCache final {
    static_assert(!std::is_void_v<T>, "FiberLocalCache<void> is invalid");

public:
    FiberLocalCache() noexcept : m_key(fiber_local::detail::AllocateKey()) {}
    // Values intentionally outlive the key object and are reclaimed by the
    // Fiber trampoline (or thread-exit TLS teardown). This avoids destroying a
    // live Fiber's value merely because a cache wrapper went out of scope.
    // Callers should keep a cache object alive while they still access it.
    ~FiberLocalCache() = default;

    FiberLocalCache(const FiberLocalCache&) = delete;
    FiberLocalCache& operator=(const FiberLocalCache&) = delete;
    FiberLocalCache(FiberLocalCache&&) = delete;
    FiberLocalCache& operator=(FiberLocalCache&&) = delete;

    template <typename... Args>
    T& GetOrCreate(Args&&... args) {
        if (auto value = load()) {
            return *std::static_pointer_cast<T>(std::move(value));
        }
        auto value = std::make_shared<T>(std::forward<Args>(args)...);
        if (Fiber* const fiber = Fiber::Current()) {
            fiber_local::detail::Set(fiber, m_key, value);
        } else {
            fiber_local::detail::ThreadValues()[m_key] = value;
        }
        return *value;
    }

    template <typename... Args>
    T& get_or_create(Args&&... args) {
        return GetOrCreate(std::forward<Args>(args)...);
    }

    T* TryGet() const noexcept {
        if (Fiber* const fiber = Fiber::Current()) {
            auto value = fiber_local::detail::Get(fiber, m_key);
            return value ? std::static_pointer_cast<T>(value).get() : nullptr;
        }
        const auto& values = fiber_local::detail::ThreadValues();
        const auto found = values.find(m_key);
        return found == values.end()
                   ? nullptr
                   : std::static_pointer_cast<T>(found->second).get();
    }

    T* try_get() const noexcept { return TryGet(); }

    void Reset() noexcept {
        if (Fiber* const fiber = Fiber::Current()) {
            fiber_local::detail::Reset(fiber, m_key);
        } else {
            fiber_local::detail::ThreadValues().erase(m_key);
        }
    }

    void reset() noexcept { Reset(); }
    std::uint64_t key_id() const noexcept { return m_key; }

private:
    std::shared_ptr<void> load() const noexcept {
        if (Fiber* const fiber = Fiber::Current()) {
            return fiber_local::detail::Get(fiber, m_key);
        }
        const auto& values = fiber_local::detail::ThreadValues();
        const auto found = values.find(m_key);
        return found == values.end() ? std::shared_ptr<void>{}
                                     : found->second;
    }

    const fiber_local::detail::KeyId m_key;
};

template <typename T>
using FiberLocal = FiberLocalCache<T>;

}  // namespace go2cpp
