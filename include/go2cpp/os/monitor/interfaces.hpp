#pragma once

// 监控模块的内部接口：采集结果从 «采集器 → 聚合器 → 接收端» 的每一段都只
// 依赖接口，便于替换后端或注入测试替身：
//
//   SnapshotSource  ← Aggregator 实现；Pusher 只依赖它读最新快照。
//   SnapshotSink    ← 接收 Snapshot：HttpSnapshotSink（Pusher 内部使用）、
//                      MemorySnapshotSink（测试/内存）、FileSnapshotSink（落盘）。
//   SnapshotStore   ← Manager 的存储：MemorySnapshotStore（默认实现）。
//
// 所有接口都不抛异常：失败用 false / 错误计数 + last_error 表达，与模块其余
// 部分（Status 返回值）一致。实现类自身线程安全，可被多个 Fiber 并发调用。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "go2cpp/os/monitor/monitor.hpp"

namespace go2cpp::os::monitor {

/** @brief 快照接收端（sink）：成功返回 true，失败返回 false 并记录统计。 */
class SnapshotSink {
public:
    SnapshotSink() = default;
    virtual ~SnapshotSink() = default;

    SnapshotSink(const SnapshotSink&) = delete;
    SnapshotSink& operator=(const SnapshotSink&) = delete;
    SnapshotSink(SnapshotSink&&) = delete;
    SnapshotSink& operator=(SnapshotSink&&) = delete;

    /** @brief 接收一条快照；实现必须线程安全。 */
    virtual bool Push(const Snapshot& snapshot) = 0;

    /** @brief 实现类型名，用于日志与诊断（固定字符串，不要动态分配）。 */
    virtual const char* kind() const noexcept { return "sink"; }
};

/** @brief 快照来源：Aggregator 实现它，Pusher 依赖该接口而非具体聚合器。 */
class SnapshotSource {
public:
    SnapshotSource() = default;
    virtual ~SnapshotSource() = default;

    SnapshotSource(const SnapshotSource&) = delete;
    SnapshotSource& operator=(const SnapshotSource&) = delete;
    SnapshotSource(SnapshotSource&&) = delete;
    SnapshotSource& operator=(SnapshotSource&&) = delete;

    /** @brief 最近一次聚合出的快照；无数据时返回各字段为默认值的快照。 */
    virtual Snapshot Latest() const = 0;
};

/** @brief 快照存储：Manager 只依赖该接口，默认用 MemorySnapshotStore。 */
class SnapshotStore {
public:
    SnapshotStore() = default;
    virtual ~SnapshotStore() = default;

    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;
    SnapshotStore(SnapshotStore&&) = delete;
    SnapshotStore& operator=(SnapshotStore&&) = delete;

    /** @brief 追加一条快照；超过容量时由实现决定淘汰策略。 */
    virtual bool Append(const Snapshot& snapshot) = 0;

    /** @brief 最近 n 条快照（按时间正序，尾部最新）；n 大于总数时返回全部。 */
    virtual std::vector<Snapshot> Recent(std::size_t n) const = 0;

    /** @brief 当前存储条数。 */
    virtual std::size_t size() const = 0;

    /** @brief 清空存储；持久化实现可覆盖为删除历史。 */
    virtual void Clear() {}
};

/** @brief sink 的推送统计。 */
struct SinkStats {
    std::uint64_t pushed{0};
    std::uint64_t failed{0};
    std::string last_error;
};

// ---------------------------------------------------------------------------
// 内存实现：测试替身 / 无持久化需求的部署。
// ---------------------------------------------------------------------------

/**
 * @brief 内存快照接收端，只保留最近 capacity 条。
 *
 * 测试里可当作 MockSink：`Push` 记录次数，`Snapshots()` 取回内容；容量为 0
 * 时表示不限制条数。
 */
class MemorySnapshotSink final : public SnapshotSink {
public:
    explicit MemorySnapshotSink(std::size_t capacity = 256);

    bool Push(const Snapshot& snapshot) override;
    const char* kind() const noexcept override { return "memory"; }

    /** @brief 当前保存的快照（时间正序，尾部最新）。 */
    std::vector<Snapshot> Snapshots() const;
    /** @brief 最近一条；无数据时返回默认快照。 */
    Snapshot Last() const;
    /** @brief 当前保存条数。 */
    std::size_t size() const;
    /** @brief 累计成功 Push 次数（含已被容量淘汰的）。 */
    std::uint64_t pushed() const;
    /** @brief 容量；0 表示不限制。 */
    std::size_t capacity() const;
    void Clear();

private:
    mutable std::mutex mutex_;
    std::deque<Snapshot> snapshots_;
    std::size_t capacity_;
    std::uint64_t pushed_{0};
};

/**
 * @brief 文件快照接收端：每行一个紧凑 JSON（JSON Lines），追加写并 flush。
 *
 * 适合本地留档 / 交给外部采集器（Filebeat 等）。构造时打开文件失败不会抛
 * 异常：第一次 Push 返回 false 并写入 last_error，便于部署时降级。
 */
class FileSnapshotSink final : public SnapshotSink {
public:
    /** @param truncate true 表示启动时清空文件，false 追加。 */
    explicit FileSnapshotSink(std::string path, bool truncate = false);
    ~FileSnapshotSink() override;

    bool Push(const Snapshot& snapshot) override;
    const char* kind() const noexcept override { return "file"; }

    const std::string& path() const noexcept;
    SinkStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
};

// ---------------------------------------------------------------------------
// HTTP 实现：Pusher 内部默认使用它（也可以单独注入）。
// ---------------------------------------------------------------------------

/** @brief HTTP sink 配置。 */
struct HttpSinkConfig {
    /// Manager 的推送地址（完整 URL，默认与 Pusher 一致）。
    std::string url{"http://127.0.0.1:9101/api/push"};
    /// 单次 POST 超时。
    std::chrono::milliseconds timeout{5000};
};

/** @brief HTTP POST JSON 到 Manager；失败只累计统计，不抛异常。 */
class HttpSnapshotSink final : public SnapshotSink {
public:
    explicit HttpSnapshotSink(HttpSinkConfig config = {});
    ~HttpSnapshotSink() override;

    bool Push(const Snapshot& snapshot) override;
    const char* kind() const noexcept override { return "http"; }

    SinkStats Stats() const;
    const HttpSinkConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    HttpSinkConfig config_;
};

// ---------------------------------------------------------------------------
// 存储实现：Manager 默认使用。
// ---------------------------------------------------------------------------

/** @brief 内存环形缓冲（容量满后淘汰最旧一条）。 */
class MemorySnapshotStore final : public SnapshotStore {
public:
    explicit MemorySnapshotStore(std::size_t capacity = 256);

    bool Append(const Snapshot& snapshot) override;
    std::vector<Snapshot> Recent(std::size_t n) const override;
    std::size_t size() const override;
    void Clear() override;

    /** @brief 容量；0 表示不限制条数。 */
    std::size_t capacity() const;

private:
    mutable std::mutex mutex_;
    std::deque<Snapshot> snapshots_;
    std::size_t capacity_;
};

}  // namespace go2cpp::os::monitor
