#include "go2cpp/os/monitor/pusher.hpp"

#include <mutex>
#include <utility>

#include "go2cpp/os/monitor/aggregator.hpp"
#include "go2cpp/runtime.hpp"

namespace go2cpp::os::monitor {

struct Pusher::Impl {
    Impl(PusherConfig cfg, SnapshotSource& snapshot_source)
        : config(std::move(cfg)),
          source(snapshot_source),
          sink(HttpSinkConfig{config.manager_url, config.timeout}) {}

    ~Impl() { StopInternal(); }

    bool PushSnapshot(const Snapshot& snapshot) {
        return sink.Push(snapshot);
    }

    void StopInternal() {
        // 调用方必须持有 lifecycle_mutex。
        if (!running.load()) {
            return;
        }
        running = false;
        timer.Cancel();
        if (scheduler) {
            scheduler->shutdown();
            scheduler.reset();
        }
    }

    PusherConfig config;
    SnapshotSource& source;
    /// HTTP 细节下沉到 HttpSnapshotSink；Pusher 只做周期调度与统计转发。
    HttpSnapshotSink sink;
    std::unique_ptr<Scheduler> scheduler;
    core::Timer timer;
    std::atomic<bool> running{false};
    /// 串行化 Start/Stop，避免并发进入时 shutdown 正在启动的 scheduler。
    std::mutex lifecycle_mutex;
};

Pusher::Pusher(PusherConfig config, SnapshotSource& source)
    : impl_(std::make_unique<Impl>(std::move(config), source)),
      config_(impl_->config) {}

Pusher::Pusher(PusherConfig config, Aggregator& aggregator)
    : Pusher(std::move(config), static_cast<SnapshotSource&>(aggregator)) {}

Pusher::~Pusher() = default;

bool Pusher::Start() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    if (impl_->running.load()) {
        return false;
    }
    impl_->running = true;
    impl_->scheduler = std::make_unique<Scheduler>(2);
    impl_->scheduler->start();
    impl_->timer = impl_->scheduler->schedule_every(
        impl_->config.interval, [this]() { (void)PushOnce(); });
    return true;
}

void Pusher::Stop() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    impl_->StopInternal();
}

bool Pusher::running() const noexcept {
    return impl_->running.load();
}

bool Pusher::PushOnce() {
    return impl_->PushSnapshot(impl_->source.Latest());
}

bool Pusher::PushSnapshot(const Snapshot& snapshot) {
    return impl_->PushSnapshot(snapshot);
}

bool Pusher::Push(const Snapshot& snapshot) {
    return impl_->PushSnapshot(snapshot);
}

PusherStats Pusher::Stats() const {
    const SinkStats stats = impl_->sink.Stats();
    return PusherStats{stats.pushed, stats.failed, stats.last_error};
}

}  // namespace go2cpp::os::monitor
