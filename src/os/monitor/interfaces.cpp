#include "go2cpp/os/monitor/interfaces.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

#include "go2cpp/network/http.hpp"

namespace go2cpp::os::monitor {

// ---------------------------------------------------------------------------
// MemorySnapshotSink
// ---------------------------------------------------------------------------

MemorySnapshotSink::MemorySnapshotSink(std::size_t capacity)
    : capacity_(capacity) {}

bool MemorySnapshotSink::Push(const Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.push_back(snapshot);
    if (capacity_ > 0) {
        while (snapshots_.size() > capacity_) {
            snapshots_.pop_front();
        }
    }
    ++pushed_;
    return true;
}

std::vector<Snapshot> MemorySnapshotSink::Snapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Snapshot>(snapshots_.begin(), snapshots_.end());
}

Snapshot MemorySnapshotSink::Last() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_.empty() ? Snapshot{} : snapshots_.back();
}

std::size_t MemorySnapshotSink::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_.size();
}

std::uint64_t MemorySnapshotSink::pushed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pushed_;
}

std::size_t MemorySnapshotSink::capacity() const {
    return capacity_;
}

void MemorySnapshotSink::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.clear();
}

// ---------------------------------------------------------------------------
// FileSnapshotSink（JSON Lines）
// ---------------------------------------------------------------------------

struct FileSnapshotSink::Impl {
    Impl(std::string file_path, bool truncate) : path(std::move(file_path)) {
        std::ios::openmode mode = std::ios::out;
        mode |= truncate ? std::ios::trunc : std::ios::app;
        stream.open(path, mode);
        if (!stream.is_open()) {
            stats.last_error = "无法打开文件: " + path;
        }
    }

    bool Push(const Snapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stream.is_open()) {
            ++stats.failed;
            if (stats.last_error.empty()) {
                stats.last_error = "文件未打开: " + path;
            }
            return false;
        }
        const std::string line = SnapshotToJson(snapshot);
        stream << line << '\n';
        stream.flush();
        if (!stream.good()) {
            ++stats.failed;
            stats.last_error = "写入失败: " + path;
            return false;
        }
        ++stats.pushed;
        return true;
    }

    SinkStats Stats() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stats;
    }

    std::string path;
    std::ofstream stream;
    mutable std::mutex mutex;
    SinkStats stats;
};

FileSnapshotSink::FileSnapshotSink(std::string path, bool truncate)
    : impl_(std::make_unique<Impl>(std::move(path), truncate)),
      path_(impl_->path) {}

FileSnapshotSink::~FileSnapshotSink() = default;

bool FileSnapshotSink::Push(const Snapshot& snapshot) {
    return impl_->Push(snapshot);
}

const std::string& FileSnapshotSink::path() const noexcept {
    return path_;
}

SinkStats FileSnapshotSink::Stats() const {
    return impl_->Stats();
}

// ---------------------------------------------------------------------------
// HttpSnapshotSink
// ---------------------------------------------------------------------------

struct HttpSnapshotSink::Impl {
    explicit Impl(HttpSinkConfig cfg) : config(std::move(cfg)) {}

    bool Push(const Snapshot& snapshot) {
        const std::string body = SnapshotToJson(snapshot);
        http::Client client(config.timeout);
        const http::Result result =
            client.post(config.url, body, "application/json");
        std::lock_guard<std::mutex> lock(mutex);
        if (result.ok()) {
            ++stats.pushed;
            return true;
        }
        ++stats.failed;
        stats.last_error =
            result.message.empty() ? "http post failed" : result.message;
        return false;
    }

    SinkStats Stats() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stats;
    }

    HttpSinkConfig config;
    mutable std::mutex mutex;
    SinkStats stats;
};

HttpSnapshotSink::HttpSnapshotSink(HttpSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))),
      config_(impl_->config) {}

HttpSnapshotSink::~HttpSnapshotSink() = default;

bool HttpSnapshotSink::Push(const Snapshot& snapshot) {
    return impl_->Push(snapshot);
}

SinkStats HttpSnapshotSink::Stats() const {
    return impl_->Stats();
}

const HttpSinkConfig& HttpSnapshotSink::config() const noexcept {
    return config_;
}

// ---------------------------------------------------------------------------
// MemorySnapshotStore
// ---------------------------------------------------------------------------

MemorySnapshotStore::MemorySnapshotStore(std::size_t capacity)
    : capacity_(capacity) {}

bool MemorySnapshotStore::Append(const Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.push_back(snapshot);
    if (capacity_ > 0) {
        while (snapshots_.size() > capacity_) {
            snapshots_.pop_front();
        }
    }
    return true;
}

std::vector<Snapshot> MemorySnapshotStore::Recent(std::size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (n == 0 || snapshots_.empty()) {
        return {};
    }
    const std::size_t count = std::min(n, snapshots_.size());
    std::vector<Snapshot> result;
    result.reserve(count);
    // deque 尾部是最新的；按时间正序返回。
    for (std::size_t i = snapshots_.size() - count; i < snapshots_.size();
         ++i) {
        result.push_back(snapshots_[i]);
    }
    return result;
}

std::size_t MemorySnapshotStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_.size();
}

void MemorySnapshotStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshots_.clear();
}

std::size_t MemorySnapshotStore::capacity() const {
    return capacity_;
}

}  // namespace go2cpp::os::monitor
