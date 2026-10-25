/**
 * @file supervisor.cpp
 * @brief 单线程进程监督器实现：ppoll 事件循环、超时阶梯、条件与配额。
 */
#include "go2cpp/os/supervisor.hpp"

#include "go2cpp/os/channel.hpp"
#include "raw_io.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

namespace go2cpp::os {

namespace {

std::string_view Tail(std::string_view text, std::size_t limit = 4096) {
    if (text.size() <= limit) {
        return text;
    }
    return text.substr(text.size() - limit);
}

class HeartbeatCondition final : public Condition {
public:
    HeartbeatCondition(std::chrono::milliseconds timeout,
                       std::chrono::milliseconds interval)
        : m_timeout(timeout), m_interval(interval) {}

    std::chrono::milliseconds Interval() const noexcept override { return m_interval; }

    bool Evaluate(const ConditionContext& context, std::string* reason) override {
        if (context.now - context.last_heartbeat > m_timeout) {
            if (reason != nullptr) {
                *reason = "心跳丢失超过 " + std::to_string(m_timeout.count()) + "ms";
            }
            return false;
        }
        return true;
    }

    std::string Describe() const override {
        return "heartbeat_lost(" + std::to_string(m_timeout.count()) + "ms)";
    }

private:
    std::chrono::milliseconds m_timeout;
    std::chrono::milliseconds m_interval;
};

class RssCondition final : public Condition {
public:
    RssCondition(std::uint64_t max_rss, std::chrono::milliseconds interval)
        : m_max_rss(max_rss), m_interval(interval) {}

    std::chrono::milliseconds Interval() const noexcept override { return m_interval; }

    bool Evaluate(const ConditionContext& context, std::string* reason) override {
        if (context.info != nullptr && context.info->rss_bytes > m_max_rss) {
            if (reason != nullptr) {
                *reason = "RSS 超过阈值: " + std::to_string(context.info->rss_bytes) + " > " +
                          std::to_string(m_max_rss);
            }
            return false;
        }
        return true;
    }

    std::string Describe() const override {
        return "rss_above(" + std::to_string(m_max_rss) + ")";
    }

private:
    std::uint64_t m_max_rss;
    std::chrono::milliseconds m_interval;
};

class ProbeFailureCondition final : public Condition {
public:
    ProbeFailureCondition(int failures,
                          std::function<bool(const ConditionContext&)> probe,
                          std::chrono::milliseconds interval)
        : m_failures(failures > 0 ? failures : 1),
          m_probe(std::move(probe)),
          m_interval(interval) {}

    std::chrono::milliseconds Interval() const noexcept override { return m_interval; }

    bool Evaluate(const ConditionContext& context, std::string* reason) override {
        if (!m_probe) {
            return true;
        }
        if (m_probe(context)) {
            m_consecutive_failures = 0;
            return true;
        }
        ++m_consecutive_failures;
        if (m_consecutive_failures >= m_failures) {
            if (reason != nullptr) {
                *reason = "连续探测失败 " + std::to_string(m_consecutive_failures) + " 次";
            }
            return false;
        }
        return true;
    }

    std::string Describe() const override {
        return "probe_failures(" + std::to_string(m_failures) + ")";
    }

private:
    int m_failures;
    int m_consecutive_failures{0};
    std::function<bool(const ConditionContext&)> m_probe;
    std::chrono::milliseconds m_interval;
};

class OutputMatchCondition final : public Condition {
public:
    OutputMatchCondition(std::string needle, bool stderr_only,
                         std::chrono::milliseconds interval)
        : m_needle(std::move(needle)),
          m_stderr_only(stderr_only),
          m_interval(interval) {}

    std::chrono::milliseconds Interval() const noexcept override { return m_interval; }

    bool Evaluate(const ConditionContext& context, std::string* reason) override {
        const bool matched =
            context.stderr_tail.find(m_needle) != std::string_view::npos ||
            (!m_stderr_only && context.stdout_tail.find(m_needle) != std::string_view::npos);
        if (matched) {
            if (reason != nullptr) {
                *reason = "输出匹配到错误模式: " + m_needle;
            }
            return false;
        }
        return true;
    }

    std::string Describe() const override { return "output_matches(" + m_needle + ")"; }

private:
    std::string m_needle;
    bool m_stderr_only;
    std::chrono::milliseconds m_interval;
};

}  // namespace

std::shared_ptr<Condition> MakeHeartbeatLostCondition(
    std::chrono::milliseconds timeout, std::chrono::milliseconds interval) {
    return std::make_shared<HeartbeatCondition>(timeout, interval);
}

std::shared_ptr<Condition> MakeRssCondition(std::uint64_t max_rss_bytes,
                                            std::chrono::milliseconds interval) {
    return std::make_shared<RssCondition>(max_rss_bytes, interval);
}

std::shared_ptr<Condition> MakeProbeFailureCondition(
    int failures, std::function<bool(const ConditionContext&)> probe,
    std::chrono::milliseconds interval) {
    return std::make_shared<ProbeFailureCondition>(failures, std::move(probe), interval);
}

std::shared_ptr<Condition> MakeOutputMatchCondition(std::string needle, bool stderr_only,
                                                    std::chrono::milliseconds interval) {
    return std::make_shared<OutputMatchCondition>(std::move(needle), stderr_only, interval);
}

// ===========================================================================
// Supervisor::Impl
// ===========================================================================

struct Supervisor::Impl final {
    struct Managed final {
        ManagedId id{0};
        Process process;
        ManagedSpec spec;
        LifecycleState state{LifecycleState::kRunning};
        std::chrono::steady_clock::time_point started{};
        std::chrono::steady_clock::time_point deadline{};
        bool has_deadline{false};
        std::chrono::steady_clock::time_point terminate_deadline{};
        std::chrono::steady_clock::time_point last_heartbeat{};
        std::chrono::steady_clock::time_point last_stdout{};
        std::chrono::steady_clock::time_point last_stderr{};
        std::string stdout_text;
        std::string stderr_text;
        std::string stderr_partial;
        std::uint64_t stdout_bytes{0};
        std::uint64_t stderr_bytes{0};
        std::uint64_t dropped_bytes{0};
        std::uint64_t peak_rss_bytes{0};
        ProcessInfo last_info;
        std::vector<std::shared_ptr<Condition>> conditions;
        std::vector<std::chrono::steady_clock::time_point> next_condition_times;
        QuotaLimits quotas;
        std::chrono::milliseconds grace{5000};
        bool terminate_requested{false};
        bool kill_sent{false};
        bool timed_out{false};
        bool condition_triggered{false};
        bool quota_triggered{false};
        std::string trigger_reason;
        std::optional<ExitStatus> exit_status;
        bool stdout_eof{false};
        bool stderr_eof{false};
    };

    explicit Impl(SupervisorOptions options);
    ~Impl();

    Result<ManagedId> Spawn(const ManagedSpec& spec);
    Result<ManagedId> Add(Process&& process, ManagedSpec spec);
    Result<void> NotifyHeartbeat(ManagedId id);
    void RequestStop(std::chrono::milliseconds grace);
    bool PollOnce(std::chrono::milliseconds max_wait);
    void Run();
    bool Active() const;
    std::size_t ActiveCount() const;
    std::optional<ManagedSnapshot> Snapshot(ManagedId id) const;
    std::string CollectedStdout(ManagedId id) const;
    std::string CollectedStderr(ManagedId id) const;
    Result<ExitStatus> Wait(ManagedId id, std::chrono::milliseconds timeout);
    SupervisorStats Stats() const;
    void SetEventSink(ProcEventSink sink);

    SupervisorOptions options;
    std::shared_ptr<Clock> clock;
    bool injected_clock{false};
    int event_fd{-1};
    std::atomic<bool> stop_requested{false};
    bool stop_cleanup_started{false};
    std::atomic<std::int64_t> stop_grace_ms{0};
    std::vector<Managed> managed;
    ManagedId next_id{1};
    SupervisorStats stats;
    std::atomic<std::size_t> active_count{0};
    ProcEventSink event_sink;
    std::mutex event_mutex;
    std::mutex heartbeat_mutex;
    std::vector<std::pair<ManagedId, std::chrono::steady_clock::time_point>> heartbeats;

    void Emit(const ProcEvent& event);
    void EmitStderr(pid_t pid, pid_t pgid, const std::string& text, bool partial);
    void ApplyHeartbeats();
    void DrainOutput(Managed& managed, std::chrono::steady_clock::time_point now);
    void AppendOutput(Managed& managed, bool is_stderr, const std::byte* data,
                      std::size_t count, std::chrono::steady_clock::time_point now);
    void OnExit(Managed& managed, const ExitStatus& status);
    void TriggerTerminate(Managed& managed, ProcEventType type, std::string reason,
                          std::chrono::steady_clock::time_point now);
    void EvaluateConditions(Managed& managed, std::chrono::steady_clock::time_point now);
    void EvaluateQuota(Managed& managed, std::chrono::steady_clock::time_point now);
    ConditionContext BuildContext(Managed& managed,
                                  std::chrono::steady_clock::time_point now);
    void WaitForEvents(std::chrono::steady_clock::time_point now,
                       std::chrono::milliseconds budget);
    void DrainEventFd();
    std::chrono::milliseconds NextWait(std::chrono::steady_clock::time_point now,
                                       std::chrono::milliseconds budget) const;
    Managed* Find(ManagedId id);
    const Managed* Find(ManagedId id) const;
};

Supervisor::Impl::Impl(SupervisorOptions options_value)
    : options(std::move(options_value)),
      clock(options.clock ? options.clock : MakeRealClock()),
      injected_clock(options.clock != nullptr) {
    event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    stop_grace_ms.store(options.default_grace.count(), std::memory_order_relaxed);
}

Supervisor::Impl::~Impl() {
    for (auto& managed_process : managed) {
        if (managed_process.state != LifecycleState::kReaped &&
            managed_process.process.valid()) {
            const auto grace = std::min<std::chrono::milliseconds>(
                managed_process.grace, std::chrono::milliseconds(1000));
            (void)managed_process.process.Terminate(grace);
        }
    }
    if (event_fd >= 0) {
        (void)detail::RawClose(event_fd);
        event_fd = -1;
    }
}

Supervisor::Impl::Managed* Supervisor::Impl::Find(ManagedId id) {
    for (auto& item : managed) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

const Supervisor::Impl::Managed* Supervisor::Impl::Find(ManagedId id) const {
    for (const auto& item : managed) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

void Supervisor::Impl::Emit(const ProcEvent& event) {
    ProcEventSink sink;
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        sink = event_sink;
    }
    if (!sink) {
        return;
    }
    try {
        sink(event);
    } catch (...) {
    }
}

void Supervisor::Impl::EmitStderr(pid_t pid, pid_t pgid, const std::string& text,
                                  bool partial) {
    if (options.stderr_sink) {
        StderrLine line;
        line.pid = pid;
        line.pgid = pgid;
        line.text = text;
        line.partial = partial;
        try {
            options.stderr_sink(line);
        } catch (...) {
        }
        return;
    }
    if (!options.forward_stderr) {
        return;
    }
    std::fprintf(stderr, "[os.supervisor stderr pid=%d pgid=%d] %s%s\n",
                 static_cast<int>(pid), static_cast<int>(pgid), text.c_str(),
                 partial ? " <partial>" : "");
}

void Supervisor::Impl::ApplyHeartbeats() {
    std::vector<std::pair<ManagedId, std::chrono::steady_clock::time_point>> pending;
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex);
        pending.swap(heartbeats);
    }
    for (const auto& item : pending) {
        if (auto* managed_process = Find(item.first)) {
            managed_process->last_heartbeat = item.second;
        }
    }
}

void Supervisor::Impl::DrainEventFd() {
    if (event_fd < 0) {
        return;
    }
    std::uint64_t value = 0;
    while (detail::RawRead(event_fd, &value, sizeof(value)) > 0) {
    }
}

Result<ManagedId> Supervisor::Impl::Spawn(const ManagedSpec& spec) {
    Process process;
    auto backend = spec.backend ? spec.backend : options.backend;
    auto spawned = Process::Spawn(spec.options, &process, backend);
    if (!spawned.ok()) {
        return spawned.error();
    }
    return Add(std::move(process), spec);
}

Result<ManagedId> Supervisor::Impl::Add(Process&& process, ManagedSpec spec) {
    if (!process.valid()) {
        return MakeError(ProcErrorCode::kInvalidArgument, EINVAL, "进程句柄无效",
                         "Supervisor::Add");
    }
    Managed managed_process;
    managed_process.id = next_id++;
    managed_process.process = std::move(process);
    managed_process.spec = std::move(spec);
    managed_process.state = LifecycleState::kRunning;
    managed_process.started = clock->Now();
    managed_process.last_heartbeat = managed_process.started;
    managed_process.last_stdout = managed_process.started;
    managed_process.last_stderr = managed_process.started;
    managed_process.grace = managed_process.spec.grace.count() > 0
                                ? managed_process.spec.grace
                                : options.default_grace;
    auto deadline = managed_process.spec.deadline.count() > 0
                        ? managed_process.spec.deadline
                        : managed_process.spec.options.timeout;
    if (deadline.count() > 0) {
        managed_process.has_deadline = true;
        managed_process.deadline = managed_process.started + deadline;
    }
    managed_process.conditions = managed_process.spec.conditions;
    managed_process.next_condition_times.reserve(managed_process.conditions.size());
    for (const auto& condition : managed_process.conditions) {
        managed_process.next_condition_times.push_back(
            managed_process.started + condition->Interval());
    }
    managed_process.quotas = managed_process.spec.quotas;
    const bool spec_has_quota =
        managed_process.quotas.max_rss_bytes != 0 ||
        managed_process.quotas.max_open_fds != 0 ||
        managed_process.quotas.max_output_bytes != 0 ||
        managed_process.quotas.max_cpu_ms != 0;
    if (!spec_has_quota) {
        managed_process.quotas = options.quotas;
    }
    auto info = managed_process.process.Info();
    if (info.ok()) {
        managed_process.last_info = info.value();
        managed_process.peak_rss_bytes = info.value().rss_bytes;
    }
    const ManagedId id = managed_process.id;
    managed.push_back(std::move(managed_process));
    ++stats.spawned;
    ++stats.active;
    stats.peak_active = std::max(stats.peak_active, stats.active);
    active_count.store(managed.size(), std::memory_order_release);
    return id;
}

Result<void> Supervisor::Impl::NotifyHeartbeat(ManagedId id) {
    std::lock_guard<std::mutex> lock(heartbeat_mutex);
    heartbeats.emplace_back(id, clock->Now());
    return OkResult();
}

void Supervisor::Impl::RequestStop(std::chrono::milliseconds grace) {
    if (grace.count() > 0) {
        stop_grace_ms.store(grace.count(), std::memory_order_relaxed);
    }
    stop_requested.store(true, std::memory_order_release);
    if (event_fd >= 0) {
        const std::uint64_t one = 1;
        (void)detail::RawWrite(event_fd, &one, sizeof(one));
    }
}

void Supervisor::Impl::AppendOutput(Managed& managed_process, bool is_stderr,
                                    const std::byte* data, std::size_t count,
                                    std::chrono::steady_clock::time_point now) {
    if (count == 0) {
        return;
    }
    if (is_stderr) {
        managed_process.stderr_bytes += count;
        managed_process.last_stderr = now;
    } else {
        managed_process.stdout_bytes += count;
        managed_process.last_stdout = now;
    }
    std::string* sink =
        is_stderr ? &managed_process.stderr_text : &managed_process.stdout_text;
    if (options.capture_output) {
        if (options.max_output_bytes == 0 || sink->size() < options.max_output_bytes) {
            const std::size_t room =
                options.max_output_bytes == 0
                    ? count
                    : std::min(count, options.max_output_bytes - sink->size());
            sink->append(reinterpret_cast<const char*>(data), room);
            if (room < count) {
                managed_process.dropped_bytes += count - room;
            }
        } else {
            managed_process.dropped_bytes += count;
        }
    }
    if (is_stderr) {
        for (std::size_t index = 0; index < count; ++index) {
            const char character = static_cast<char>(data[index]);
            if (character == '\n') {
                EmitStderr(managed_process.process.pid(), managed_process.process.pgid(),
                           managed_process.stderr_partial, false);
                managed_process.stderr_partial.clear();
            } else {
                managed_process.stderr_partial.push_back(character);
            }
        }
    }
}

void Supervisor::Impl::DrainOutput(Managed& managed_process,
                                   std::chrono::steady_clock::time_point now) {
    std::byte buffer[65536];
    const auto drain_channel = [&](MessageChannel& channel, bool is_stderr,
                                   bool* eof_flag) {
        if (*eof_flag) {
            return;
        }
        for (;;) {
            auto read = channel.Read(MutableByteSpan(buffer, sizeof(buffer)),
                                     std::chrono::milliseconds(0));
            if (!read.ok()) {
                if (read.error().code == ProcErrorCode::kClosed) {
                    *eof_flag = true;
                }
                break;
            }
            if (read.value().bytes == 0) {
                break;
            }
            AppendOutput(managed_process, is_stderr, buffer, read.value().bytes, now);
        }
    };
    auto& stdout_channel = managed_process.process.stdout_channel();
    auto& stderr_channel = managed_process.process.stderr_channel();
    drain_channel(stdout_channel, false, &managed_process.stdout_eof);
    if (&stderr_channel != &stdout_channel) {
        drain_channel(stderr_channel, true, &managed_process.stderr_eof);
    }
}

void Supervisor::Impl::OnExit(Managed& managed_process, const ExitStatus& status) {
    ExitStatus final_status = status;
    final_status.timed_out = final_status.timed_out || managed_process.timed_out;
    managed_process.exit_status = final_status;
    managed_process.state = LifecycleState::kReaped;
    if (managed_process.stderr_partial.size() > 0) {
        EmitStderr(managed_process.process.pid(), managed_process.process.pgid(),
                   managed_process.stderr_partial, true);
        managed_process.stderr_partial.clear();
    }
    const auto usage = managed_process.process.Usage();
    managed_process.peak_rss_bytes =
        std::max(managed_process.peak_rss_bytes, usage.peak_rss_bytes);

    ProcEvent event;
    event.type = ProcEventType::kExit;
    event.pid = managed_process.process.pid();
    event.pgid = managed_process.process.pgid();
    event.exit_code = final_status.exit_code;
    event.signal = final_status.signal;
    event.reason = managed_process.trigger_reason;
    event.when = std::chrono::system_clock::now();
    Emit(event);
    ProcEvent reap = event;
    reap.type = ProcEventType::kReap;
    Emit(reap);

    ++stats.completed;
    ++stats.reaped;
    if (final_status.timed_out) {
        ++stats.timed_out;
    }
    if (stats.active > 0) {
        --stats.active;
    }
    active_count.store(
        static_cast<std::size_t>(
            std::count_if(managed.begin(), managed.end(), [](const Managed& item) {
                return item.state != LifecycleState::kReaped;
            })),
        std::memory_order_release);
    if (managed_process.spec.on_exit) {
        try {
            managed_process.spec.on_exit(final_status, managed_process.trigger_reason);
        } catch (...) {
        }
    }
}

void Supervisor::Impl::TriggerTerminate(Managed& managed_process, ProcEventType type,
                                        std::string reason,
                                        std::chrono::steady_clock::time_point now) {
    if (managed_process.state != LifecycleState::kRunning) {
        return;
    }
    managed_process.state = LifecycleState::kTerminating;
    managed_process.terminate_requested = true;
    managed_process.kill_sent = false;
    managed_process.trigger_reason = std::move(reason);
    managed_process.terminate_deadline = now + managed_process.grace;
    if (type == ProcEventType::kTimeout) {
        managed_process.timed_out = true;
        ++stats.timed_out;
    } else if (type == ProcEventType::kCondition) {
        managed_process.condition_triggered = true;
        ++stats.condition_triggered;
    } else if (type == ProcEventType::kQuotaExceeded) {
        managed_process.quota_triggered = true;
        ++stats.quota_triggered;
    }
    ProcEvent event;
    event.type = type;
    event.pid = managed_process.process.pid();
    event.pgid = managed_process.process.pgid();
    event.reason = managed_process.trigger_reason;
    event.when = std::chrono::system_clock::now();
    Emit(event);
    const auto killed = managed_process.process.Kill(SIGTERM);
    if (!killed.ok() && killed.error().code != ProcErrorCode::kNotRunning) {
        managed_process.trigger_reason += " (SIGTERM 失败: " + killed.error().message + ")";
    }
}

ConditionContext Supervisor::Impl::BuildContext(
    Managed& managed_process, std::chrono::steady_clock::time_point now) {
    ConditionContext context;
    context.pid = managed_process.process.pid();
    auto info = managed_process.process.Info();
    if (info.ok()) {
        managed_process.last_info = info.value();
        context.info = &managed_process.last_info;
    }
    context.now = now;
    context.started = managed_process.started;
    context.last_heartbeat = managed_process.last_heartbeat;
    context.last_stdout = managed_process.last_stdout;
    context.last_stderr = managed_process.last_stderr;
    context.stdout_tail = Tail(managed_process.stdout_text);
    context.stderr_tail = Tail(managed_process.stderr_text);
    context.stdout_bytes = managed_process.stdout_bytes;
    context.stderr_bytes = managed_process.stderr_bytes;
    return context;
}

void Supervisor::Impl::EvaluateConditions(
    Managed& managed_process, std::chrono::steady_clock::time_point now) {
    if (managed_process.conditions.empty()) {
        return;
    }
    for (std::size_t index = 0; index < managed_process.conditions.size(); ++index) {
        if (now < managed_process.next_condition_times[index]) {
            continue;
        }
        auto& condition = managed_process.conditions[index];
        managed_process.next_condition_times[index] = now + condition->Interval();
        ConditionContext context = BuildContext(managed_process, now);
        std::string reason;
        bool healthy = true;
        try {
            healthy = condition->Evaluate(context, &reason);
        } catch (...) {
            // 扩展点隔离：谓词异常视为「本次不触发」，并记录事件。
            ProcEvent event;
            event.type = ProcEventType::kIoError;
            event.pid = managed_process.process.pid();
            event.pgid = managed_process.process.pgid();
            event.reason = "条件求值抛出异常: " + condition->Describe();
            event.when = std::chrono::system_clock::now();
            Emit(event);
            healthy = true;
        }
        if (!healthy) {
            TriggerTerminate(managed_process, ProcEventType::kCondition,
                             reason.empty() ? condition->Describe() : reason, now);
            return;
        }
    }
}

void Supervisor::Impl::EvaluateQuota(Managed& managed_process,
                                     std::chrono::steady_clock::time_point now) {
    if (managed_process.quotas.action == QuotaAction::kIgnore) {
        return;
    }
    if (managed_process.state != LifecycleState::kRunning) {
        return;
    }
    const auto usage = managed_process.process.Usage();
    managed_process.peak_rss_bytes =
        std::max(managed_process.peak_rss_bytes, usage.peak_rss_bytes);
    std::string reason;
    if (managed_process.quotas.max_rss_bytes != 0 &&
        usage.peak_rss_bytes > managed_process.quotas.max_rss_bytes) {
        reason = "RSS 配额超限";
    } else if (managed_process.quotas.max_open_fds != 0 &&
               usage.open_fds > managed_process.quotas.max_open_fds) {
        reason = "fd 配额超限";
    } else if (managed_process.quotas.max_cpu_ms != 0 &&
               usage.cpu_user_ms + usage.cpu_system_ms >
                   managed_process.quotas.max_cpu_ms) {
        reason = "CPU 配额超限";
    } else if (managed_process.quotas.max_output_bytes != 0 &&
               managed_process.stdout_bytes + managed_process.stderr_bytes >
                   managed_process.quotas.max_output_bytes) {
        reason = "输出字节配额超限";
    }
    if (reason.empty()) {
        return;
    }
    ProcEvent event;
    event.type = ProcEventType::kQuotaExceeded;
    event.pid = managed_process.process.pid();
    event.pgid = managed_process.process.pgid();
    event.reason = reason;
    event.when = std::chrono::system_clock::now();
    Emit(event);
    if (managed_process.quotas.action == QuotaAction::kKill) {
        TriggerTerminate(managed_process, ProcEventType::kQuotaExceeded, reason, now);
    }
}

std::chrono::milliseconds Supervisor::Impl::NextWait(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds budget) const {
    auto next = now + budget;
    for (const auto& managed_process : managed) {
        if (managed_process.state == LifecycleState::kReaped) {
            continue;
        }
        if (managed_process.has_deadline && !managed_process.terminate_requested) {
            next = std::min(next, managed_process.deadline);
        }
        if (managed_process.terminate_requested && !managed_process.kill_sent) {
            next = std::min(next, managed_process.terminate_deadline);
        }
        for (const auto& condition_time : managed_process.next_condition_times) {
            next = std::min(next, condition_time);
        }
        if (managed_process.quotas.action != QuotaAction::kIgnore) {
            next = std::min(next, now + options.tick);
        }
    }
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next - now);
    if (wait.count() < 0) {
        wait = std::chrono::milliseconds(0);
    }
    return wait;
}

void Supervisor::Impl::WaitForEvents(std::chrono::steady_clock::time_point now,
                                     std::chrono::milliseconds budget) {
    auto wait = NextWait(now, budget);
    if (injected_clock) {
        // 注入时钟用于假后端/状态机测试：用 SleepFor 推进时间，避免真实等待。
        clock->SleepFor(wait);
        return;
    }
    std::vector<struct pollfd> descriptors;
    descriptors.reserve(1 + managed.size() * 2);
    if (event_fd >= 0) {
        struct pollfd descriptor {};
        descriptor.fd = event_fd;
        descriptor.events = POLLIN;
        descriptors.push_back(descriptor);
    }
    for (const auto& managed_process : managed) {
        if (managed_process.state == LifecycleState::kReaped) {
            continue;
        }
        const int stdout_fd = managed_process.process.native_fd(StdioStream::kStdout);
        const int stderr_fd = managed_process.process.native_fd(StdioStream::kStderr);
        if (stdout_fd >= 0 && !managed_process.stdout_eof) {
            struct pollfd descriptor {};
            descriptor.fd = stdout_fd;
            descriptor.events = POLLIN;
            descriptors.push_back(descriptor);
        }
        if (stderr_fd >= 0 && stderr_fd != stdout_fd && !managed_process.stderr_eof) {
            struct pollfd descriptor {};
            descriptor.fd = stderr_fd;
            descriptor.events = POLLIN;
            descriptors.push_back(descriptor);
        }
    }
    if (descriptors.empty()) {
        clock->SleepFor(wait);
        return;
    }
    struct timespec timeout {};
    timeout.tv_sec = static_cast<time_t>(wait.count() / 1000);
    timeout.tv_nsec = static_cast<long>((wait.count() % 1000) * 1000000);
    const int ready = detail::RawPpoll(descriptors.data(), descriptors.size(), &timeout,
                                       nullptr);
    if (ready < 0 && errno != EINTR) {
        // 等待失败不应终止监督循环；下一轮用最小 tick 重试。
        clock->SleepFor(std::min<std::chrono::milliseconds>(wait, options.tick));
    }
}

bool Supervisor::Impl::PollOnce(std::chrono::milliseconds max_wait) {
    DrainEventFd();
    ApplyHeartbeats();
    const auto now = clock->Now();

    for (auto& managed_process : managed) {
        if (managed_process.state != LifecycleState::kReaped) {
            DrainOutput(managed_process, now);
        }
    }

    if (stop_requested.load(std::memory_order_acquire) && !stop_cleanup_started) {
        stop_cleanup_started = true;
        for (auto& managed_process : managed) {
            if (managed_process.state == LifecycleState::kRunning) {
                const auto override_grace = std::chrono::milliseconds(
                    stop_grace_ms.load(std::memory_order_relaxed));
                if (override_grace.count() > 0) {
                    managed_process.grace = override_grace;
                }
                TriggerTerminate(managed_process, ProcEventType::kKill, "Supervisor Stop 请求",
                                 now);
            }
        }
    }

    bool active = false;
    for (auto& managed_process : managed) {
        if (managed_process.state == LifecycleState::kReaped) {
            continue;
        }
        ExitStatus status;
        ProcError wait_error;
        if (managed_process.process.TryWait(&status, &wait_error)) {
            DrainOutput(managed_process, now);
            OnExit(managed_process, status);
            continue;
        }
        if (!wait_error.ok()) {
            ++managed_process.dropped_bytes;
            OnExit(managed_process, ExitStatus{});
            continue;
        }
        if (managed_process.has_deadline && !managed_process.terminate_requested &&
            now >= managed_process.deadline) {
            TriggerTerminate(managed_process, ProcEventType::kTimeout, "deadline 到期", now);
        }
        if (managed_process.terminate_requested && !managed_process.kill_sent &&
            now >= managed_process.terminate_deadline) {
            managed_process.kill_sent = true;
            (void)managed_process.process.Kill(SIGKILL);
        }
        if (managed_process.state == LifecycleState::kRunning) {
            EvaluateConditions(managed_process, now);
            EvaluateQuota(managed_process, now);
        }
        active = true;
    }

    if (!active) {
        return false;
    }
    WaitForEvents(now, max_wait);
    return true;
}

void Supervisor::Impl::Run() {
    while (PollOnce(std::chrono::milliseconds(100))) {
    }
}

bool Supervisor::Impl::Active() const {
    return active_count.load(std::memory_order_acquire) > 0;
}

std::size_t Supervisor::Impl::ActiveCount() const {
    return active_count.load(std::memory_order_acquire);
}

std::optional<ManagedSnapshot> Supervisor::Impl::Snapshot(ManagedId id) const {
    const auto* managed_process = Find(id);
    if (managed_process == nullptr) {
        return std::nullopt;
    }
    ManagedSnapshot snapshot;
    snapshot.id = managed_process->id;
    snapshot.pid = managed_process->process.pid();
    snapshot.pgid = managed_process->process.pgid();
    snapshot.state = managed_process->state;
    snapshot.terminated_by_condition = managed_process->condition_triggered;
    snapshot.terminated_by_quota = managed_process->quota_triggered;
    snapshot.timed_out = managed_process->timed_out;
    snapshot.trigger_reason = managed_process->trigger_reason;
    snapshot.exit_status = managed_process->exit_status;
    snapshot.usage.open_fds = managed_process->last_info.alive
                                  ? managed_process->process.Usage().open_fds
                                  : 0;
    snapshot.usage.peak_rss_bytes = managed_process->peak_rss_bytes;
    snapshot.usage.stdout_bytes = managed_process->stdout_bytes;
    snapshot.usage.stderr_bytes = managed_process->stderr_bytes;
    snapshot.usage.dropped_output_bytes = managed_process->dropped_bytes;
    if (managed_process->last_info.alive) {
        snapshot.usage.cpu_user_ms = managed_process->last_info.cpu_user_ms;
        snapshot.usage.cpu_system_ms = managed_process->last_info.cpu_system_ms;
    }
    snapshot.started = managed_process->started;
    snapshot.deadline = managed_process->has_deadline
                            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                  managed_process->deadline - managed_process->started)
                            : std::chrono::milliseconds(0);
    return snapshot;
}

std::string Supervisor::Impl::CollectedStdout(ManagedId id) const {
    const auto* managed_process = Find(id);
    return managed_process == nullptr ? std::string() : managed_process->stdout_text;
}

std::string Supervisor::Impl::CollectedStderr(ManagedId id) const {
    const auto* managed_process = Find(id);
    return managed_process == nullptr ? std::string() : managed_process->stderr_text;
}

Result<ExitStatus> Supervisor::Impl::Wait(ManagedId id,
                                          std::chrono::milliseconds timeout) {
    const auto start = clock->Now();
    for (;;) {
        const auto* managed_process = Find(id);
        if (managed_process == nullptr) {
            return MakeError(ProcErrorCode::kInvalidArgument, ENOENT, "未找到受管进程",
                             "Supervisor::Wait");
        }
        if (managed_process->state == LifecycleState::kReaped &&
            managed_process->exit_status.has_value()) {
            return *managed_process->exit_status;
        }
        if (timeout.count() >= 0 && clock->Now() - start >= timeout) {
            return MakeError(ProcErrorCode::kTimeout, ETIMEDOUT, "等待受管进程超时",
                             "Supervisor::Wait");
        }
        if (!PollOnce(std::chrono::milliseconds(50))) {
            const auto* again = Find(id);
            if (again != nullptr && again->exit_status.has_value()) {
                return *again->exit_status;
            }
            return MakeError(ProcErrorCode::kNotRunning, ESRCH, "监督器已没有活跃进程",
                             "Supervisor::Wait");
        }
    }
}

SupervisorStats Supervisor::Impl::Stats() const {
    SupervisorStats snapshot = stats;
    snapshot.active = active_count.load(std::memory_order_acquire);
    return snapshot;
}

void Supervisor::Impl::SetEventSink(ProcEventSink sink) {
    std::lock_guard<std::mutex> lock(event_mutex);
    event_sink = std::move(sink);
}

// ===========================================================================
// Supervisor 公共接口
// ===========================================================================

Supervisor::Supervisor(SupervisorOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options))) {}

Supervisor::~Supervisor() = default;

Supervisor::Supervisor(Supervisor&& other) noexcept : m_impl(std::move(other.m_impl)) {}

Supervisor& Supervisor::operator=(Supervisor&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Result<ManagedId> Supervisor::Spawn(const ManagedSpec& spec) {
    return m_impl->Spawn(spec);
}

Result<ManagedId> Supervisor::Add(Process&& process, ManagedSpec spec) {
    return m_impl->Add(std::move(process), std::move(spec));
}

Result<void> Supervisor::NotifyHeartbeat(ManagedId id) {
    return m_impl->NotifyHeartbeat(id);
}

void Supervisor::Stop(std::chrono::milliseconds grace) {
    m_impl->RequestStop(grace);
}

bool Supervisor::PollOnce(std::chrono::milliseconds max_wait) {
    return m_impl->PollOnce(max_wait);
}

void Supervisor::Run() { m_impl->Run(); }

bool Supervisor::active() const { return m_impl->Active(); }
std::size_t Supervisor::active_count() const { return m_impl->ActiveCount(); }

std::optional<ManagedSnapshot> Supervisor::Snapshot(ManagedId id) const {
    return m_impl->Snapshot(id);
}

std::string Supervisor::CollectedStdout(ManagedId id) const {
    return m_impl->CollectedStdout(id);
}

std::string Supervisor::CollectedStderr(ManagedId id) const {
    return m_impl->CollectedStderr(id);
}

Result<ExitStatus> Supervisor::Wait(ManagedId id, std::chrono::milliseconds timeout) {
    return m_impl->Wait(id, timeout);
}

SupervisorStats Supervisor::stats() const { return m_impl->Stats(); }

void Supervisor::SetEventSink(ProcEventSink sink) {
    m_impl->SetEventSink(std::move(sink));
}

}  // namespace go2cpp::os
