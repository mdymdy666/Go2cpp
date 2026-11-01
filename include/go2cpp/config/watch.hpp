#pragma once

/**
 * @file include/go2cpp/config/watch.hpp
 * @brief 配置热更新：文件监听（inotify / 轮询）+ 原子发布 + 无锁读快照。
 *
 * ## 模块定位
 *
 * `config.hpp` 里的 `Config::StartWatcher()` 是**进程级注册表**的轮询监听：
 * 它按固定周期 `stat` 文件、发现变化后走两阶段事务更新 `ConfigVar`。本文件
 * 是独立、可复用的**单文件热更新原语**，解决两个它没有覆盖的问题：
 *
 * 1. **监听延迟**：Linux 上优先用 inotify（`IN_CLOSE_WRITE`/`IN_MOVED_TO`），
 *    变更在毫秒级内可见；inotify 不可用时自动回退轮询；
 * 2. **读侧无锁**：`ConfigStore<T>` 用「引用计数槽 + 原子发布索引」的 RCU
 *    风格协议，读者只做两次原子读和一次 CAS，**不获取任何互斥锁**，也不会
 *    因为写者更新配置而阻塞。
 *
 * ## 本文件定义什么
 *
 * - `FileChange` / `FileWatchOptions` / `FileWatcher`：单文件变更监听，
 *   inotify 优先、轮询兜底，目录级监听因此兼容「写临时文件再 rename」的
 *   原子替换写法；
 * - `ConfigStore<T>`：无锁读的多版本配置存储，`Snapshot` 是 RAII 引用；
 * - `ConfigWatcher<T>`：把 `FileWatcher` + loader + `ConfigStore` 串起来，
 *   变更时重载、校验失败保留旧值、成功后原子发布并回调。
 *
 * ## 读侧为什么可以无锁
 *
 * `ConfigStore` 维护 `kSlotCount` 个槽，每个槽有：
 *
 * - `std::atomic<std::uint32_t> refs`：0 表示空闲，正数表示正在读的
 *   `Snapshot` 数量，最高位 `kLocked` 表示写者正在独占改写该槽；
 * - `std::shared_ptr<const T> value`：只在 refs==0 且写者已锁定该槽时替换。
 *
 * 读者协议（`ConfigStore::Load()`）：
 *
 * 1. `index = active.load(acquire)`；
 * 2. 对 `refs` 做 CAS（`old -> old + 1`）；若读到 `kLocked` 或 CAS 失败，
 *    回到第 1 步；
 * 3. 再次读 `active`，仍等于 `index` 才返回 `Snapshot`；否则释放引用重试。
 *
 * 写者协议（`ConfigStore::Store()`）：
 *
 * 1. 选一个 `refs == 0` 且不是当前 active 的槽，CAS（`0 -> kLocked`）锁定；
 * 2. 替换 `value`，`active.store(release)` 发布新索引，版本号 +1；
 * 3. `refs.store(0, release)` 解锁。
 *
 * 因此读者要么完整看到旧版本、要么完整看到新版本，永远不会读到「写了一半」
 * 的配置；写者更新时读者最多重试一次（读侧不加锁、不睡眠）。
 *
 * ## 边界与陷阱
 *
 * - 槽位固定 8 个：写者需要等旧槽的读者全部释放后才会复用它。**不要长期
 *   持有 `Snapshot`**（跨请求、塞进全局容器）——需要长期持有时调用
 *   `Snapshot::share()` 拷成 `shared_ptr<const T>`，它不受槽位数量限制；
 * - `ConfigStore` 析构时仍有 `Snapshot` 存活属于调用方错误（文档约束）；
 * - 配置对象 `T` 只需可移动/可拷贝（由 loader 构造），读侧从不拷贝 `T`；
 * - 写路径有互斥锁（`Store()` 串行、必要时等待空闲槽），读路径没有；
 * - `FileWatcher` 监听的是**父目录**而不是文件 inode：这样 `rename` 覆盖
 *   （编辑器/Vim/`mv` 的常见写法）不会让监听失效；删除后重建也能看到；
 * - inotify 分支**只上报稳定状态事件**（`IN_CLOSE_WRITE` / `IN_MOVED_TO` /
 *   `IN_DELETE` / `IN_MOVED_FROM` / `IN_ATTRIB`），刻意忽略 `IN_MODIFY` 与
 *   `IN_CREATE`：普通 `open/truncate/write/close` 在写入过程中会先触发
 *   MODIFY/CREATE，此时文件是半截的，读它只会得到一次无意义的失败重载。
 *   只处理 CLOSE_WRITE 可以让「写一半被读到」从源头消失；用 mmap 写文件
 *   不会产生 CLOSE_WRITE，这类写入方请改为原子替换或轮询模式；
 * - 轮询分支用 `inode + mtime(ns) + size` 做指纹：原地覆盖如果**同尺寸**
 *   且发生在同一个文件系统时间戳粒度内，可能漏检——轮询模式推荐配合
 *   「写临时文件 + rename」；inotify 模式没有这个限制；
 * - inotify 事件会合并：连续快速写入可能只产生一次回调。`ConfigWatcher`
 *   用 `debounce` 合并突发事件，每次重载都读取文件**最新内容**，不会读到
 *   中间态；对「每一次写入都必须对应一次重载」的强计数需求，请在写入方
 *   串行等待 `version()` 前进（测试即如此）；
 * - 重载失败（文件暂时不完整、校验失败、loader 抛异常）**保留旧配置**并
 *   计入 `failed_reloads()`，监听线程不会退出；
 * - 文件路径为空、loader 为空时 `Start()` 直接失败；
 * - `Stop()` 是幂等的；析构会 `Stop()`，可在任意线程调用（不能在 watcher
 *   自己的回调里调用，否则会自 join——`Stop()` 检测到同线程会 detach）。
 *
 * ## 阅读建议
 *
 * 先看 `ConfigStore::Snapshot` 的 RAII 语义，再看 `ConfigStore::Load()` 的
 * 三步重试协议与 `Store()` 的槽位锁定；最后看 `ConfigWatcher::ReloadLoop()`
 * 如何用 `debounce` 合并事件、`ReloadOnce()` 如何把 loader/validator/发布
 * 拆成「失败保留旧值」。
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#endif

namespace go2cpp::config {

/**
 * @brief 单文件监听到的变更类型。
 * @details 只表达「需要重载」这一层语义，不试图完整还原文件系统事件序列。
 */
enum class FileChange : std::uint8_t {
    kModified = 0,  ///< 内容或属性变化（inotify MODIFY/CLOSE_WRITE/ATTRIB）。
    kReplaced,      ///< 被 rename 覆盖或新建（MOVED_TO/CREATE）。
    kRemoved,       ///< 被删除或移走（DELETE/MOVED_FROM）。
    kCreated,       ///< 轮询模式下从「不存在」变为「存在」。
};

/**
 * @brief `FileWatcher` 的构造参数。
 * @details 构造后只读；修改需要重建 watcher。
 */
struct FileWatchOptions {
    /// 轮询模式的检查周期（inotify 模式下仅作为 `poll` 超时上限）。
    std::chrono::milliseconds poll_interval{100};
    /// 是否优先尝试 inotify；false 强制轮询（便于对照测试）。
    bool prefer_inotify{true};
};

/**
 * @brief 监听单个配置文件的变更（inotify 优先，轮询兜底）。
 *
 * ## 定位
 * `ConfigWatcher<T>` 的底层事件源；也可以单独用于「配置变更后重建日志
 * 后端」这类不需要快照存储的场景。
 *
 * ## 实现选择
 * - Linux 且 `prefer_inotify`：监听**父目录**，按 basename 过滤事件；
 *   这样原子替换（写 `.tmp` 再 `rename`）与删除重建都能被观察到；
 * - inotify 初始化 / `inotify_add_watch` 失败：记录 `last_error()`，自动
 *   回退到轮询，`Start()` 仍返回 true；
 * - 轮询：比较 `stat` 的 inode/mtime(ns)/size；文件被删除后继续等待重建。
 *
 * ## 线程模型
 * - `Start()` 创建一个后台线程；`Stop()` 通知并 join；
 * - 回调在后台线程执行，**必须快速返回**；本类不负责去抖（由上层做）；
 * - `SetCallback()`/`running()`/`change_count()` 可与其他线程并发调用。
 */
class FileWatcher final {
public:
    /// 变更回调；在 watcher 后台线程执行。
    using Callback = std::function<void(FileChange)>;

    /**
     * @brief 绑定要监听的文件路径。
     * @param path 文件路径（相对路径按进程当前目录解析）。
     * @param options 监听参数。
     */
    explicit FileWatcher(std::string path, FileWatchOptions options = {});
    /// 停止监听并回收线程。
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) = delete;
    FileWatcher& operator=(FileWatcher&&) = delete;

    /**
     * @brief 启动监听线程。
     * @param error 失败原因（可为空）。
     * @return 启动成功返回 true；已运行时返回 false。
     * @note inotify 不可用时回退轮询，仍返回 true，原因见 `last_error()`。
     */
    bool Start(std::string* error = nullptr);

    /// 停止监听并 join（幂等）。
    void Stop();

    /// 后台线程是否在运行。
    bool running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }
    /// 实际使用的是否为 inotify（`Start()` 之前恒为 false）。
    bool uses_inotify() const noexcept {
        return m_uses_inotify.load(std::memory_order_acquire);
    }
    /// 监听到的变更次数（含被上层合并的重复事件）。
    std::uint64_t change_count() const noexcept {
        return m_change_count.load(std::memory_order_relaxed);
    }
    /// 监听路径。
    const std::string& path() const noexcept { return m_path; }

    /// 设置变更回调（线程安全；回调在执行期间被复制）。
    void SetCallback(Callback callback);

    /// 最近一次错误；无错误时为空。
    std::string last_error() const;

private:
    /// 轮询模式下的文件指纹。
    struct Stamp {
        bool exists{false};       ///< 文件当前是否存在。
        std::uint64_t inode{0};   ///< inode 号；rename 替换会变化。
        std::int64_t mtime_ns{0}; ///< 修改时间（纳秒）。
        std::uint64_t size{0};    ///< 文件字节数。
    };

    /// inotify 初始化；失败返回 false（由 Start 回退轮询）。
    bool StartInotify();
    /// 启动轮询分支。
    void StartPolling();
    /// inotify 事件循环（后台线程）。
    void InotifyLoop();
    /// 轮询循环（后台线程）；`previous` 是 `Start()` 时同步记录的基线。
    void PollLoop(Stamp previous, bool present);
    /// 读取文件指纹；不存在返回 false 且 `stamp->exists=false`。
    bool ReadStamp(Stamp* stamp) const;
    /// 分发一次变更（复制回调后锁外调用）。
    void Notify(FileChange change);
    /// 记录错误文本（加锁）。
    void RecordError(std::string message);
    /// 关闭 inotify 资源（线程退出后调用）。
    void CloseInotify();

    /// 监听路径。
    std::string m_path;
    /// 父目录（inotify watch 目标）。
    std::string m_directory;
    /// 文件名（事件过滤依据）。
    std::string m_basename;
    /// 监听参数。
    FileWatchOptions m_options;

    /// 生命周期串行化（Start/Stop）。
    std::mutex m_lifecycle_mutex;
    /// 保护回调与错误文本。
    mutable std::mutex m_callback_mutex;
    /// 回调。
    Callback m_callback;
    /// 最近错误。
    std::string m_last_error;
    /// 轮询等待条件变量（停止时立即唤醒）。
    std::condition_variable m_wait_cv;
    /// 轮询等待互斥。
    std::mutex m_wait_mutex;

    /// 后台线程。
    std::thread m_thread;
    /// 运行标志。
    std::atomic<bool> m_running{false};
    /// 是否使用 inotify。
    std::atomic<bool> m_uses_inotify{false};
    /// 变更计数。
    std::atomic<std::uint64_t> m_change_count{0};

    /// inotify 文件描述符；-1 表示未使用。
    int m_inotify_fd{-1};
    /// inotify watch 描述符；-1 表示未使用。
    int m_watch_descriptor{-1};
};

/**
 * @brief 多版本配置存储：读者无锁、写者原子发布。
 *
 * ## 定位
 * 热更新的核心数据结构：把「当前配置」抽象成一个不可变快照，写者构造新
 * 快照并原子换指针，读者拿到引用计数保护的 `Snapshot` 后无锁访问。
 *
 * ## 不变量
 * - 任意时刻 `active` 指向的槽的 `value` 非空（除非从未发布过）；
 * - 槽的 `value` 只在 `refs == kLocked` 时被写者替换；
 * - 读者成功返回的 `Snapshot` 在析构前保证 `value` 不会被替换/释放；
 * - 版本号单调递增，每次 `Store` +1。
 *
 * ## 为什么不用 `std::atomic<std::shared_ptr<T>>`
 * 那是 C++20 接口；C++17 下只能用 `std::atomic_load(&shared_ptr)`，而
 * libstdc++ 的实现内部是**自旋锁池**，读者在锁被占用时会自旋等待，不满足
 * 「无锁读」。本实现用槽位 + 引用计数 CAS，读侧不进入任何互斥锁。
 *
 * ## 槽位耗尽
 * 8 个槽中最多 7 个可以被长期持有；写者在全部非 active 槽都被读者占用时
 * 会等待（条件变量），最多 7 个 `Snapshot` 同时存活。需要长期持有时用
 * `Snapshot::share()`。
 */
template <typename T>
class ConfigStore final {
public:
    /// 槽位数量：8 个足够覆盖「一个写者 + 少量并发快照」的场景。
    static constexpr std::size_t kSlotCount = 8;
    /// 写者独占锁定位（最高位）。
    static constexpr std::uint32_t kLocked = 0x80000000u;

    /**
     * @brief 配置快照：RAII 引用，析构时归还槽位。
     *
     * ## 定位
     * `ConfigStore::Load()` 的返回值；`operator->`/`operator*` 直接访问
     * 不可变配置对象，**不加锁、不拷贝 T**。
     *
     * ## 生命周期
     * 必须早于所属 `ConfigStore` 析构；需要跨组件长期持有时用 `share()`。
     */
    class Snapshot final {
    public:
        /// 空快照（`get() == nullptr`）。
        Snapshot() noexcept = default;

        /// 拷贝：对同一槽增加引用（不会阻塞写者）。
        Snapshot(const Snapshot& other) noexcept
            : m_owner(other.m_owner), m_slot(other.m_slot) {
            if (m_owner != nullptr) {
                m_owner->AddRef(m_slot);
            }
        }

        /// 移动：转移引用，不改变计数。
        Snapshot(Snapshot&& other) noexcept
            : m_owner(other.m_owner), m_slot(other.m_slot) {
            other.m_owner = nullptr;
            other.m_slot = 0;
        }

        /// 拷贝赋值：先释放旧引用，再获取新引用。
        Snapshot& operator=(const Snapshot& other) noexcept {
            if (this == &other) {
                return *this;
            }
            Reset();
            m_owner = other.m_owner;
            m_slot = other.m_slot;
            if (m_owner != nullptr) {
                m_owner->AddRef(m_slot);
            }
            return *this;
        }

        /// 移动赋值：转移引用。
        Snapshot& operator=(Snapshot&& other) noexcept {
            if (this == &other) {
                return *this;
            }
            Reset();
            m_owner = other.m_owner;
            m_slot = other.m_slot;
            other.m_owner = nullptr;
            other.m_slot = 0;
            return *this;
        }

        /// 归还引用；写者在等待空槽时会被唤醒。
        ~Snapshot() { Reset(); }

        /// 当前配置对象指针；空快照返回 nullptr。
        const T* get() const noexcept {
            if (m_owner == nullptr) {
                return nullptr;
            }
            return m_owner->SlotValue(m_slot);
        }

        /// 解引用；空快照是调用方错误。
        const T& operator*() const { return *get(); }
        /// 成员访问；空快照是调用方错误。
        const T* operator->() const noexcept { return get(); }
        /// 是否持有有效配置。
        explicit operator bool() const noexcept { return get() != nullptr; }

        /**
         * @brief 拷贝成 `shared_ptr<const T>`，用于长期持有。
         * @return 独立于槽位的共享指针；空快照返回空指针。
         */
        std::shared_ptr<const T> share() const {
            if (m_owner == nullptr) {
                return nullptr;
            }
            return m_owner->SlotShared(m_slot);
        }

    private:
        friend class ConfigStore;

        /// 由 `ConfigStore::Load()` 构造；`slot` 已被本快照持有引用。
        Snapshot(ConfigStore* owner, std::size_t slot) noexcept
            : m_owner(owner), m_slot(slot) {}

        /// 释放当前引用。
        void Reset() noexcept {
            if (m_owner != nullptr) {
                m_owner->Release(m_slot);
                m_owner = nullptr;
            }
        }

        /// 所属 store；空快照为 nullptr。
        ConfigStore* m_owner{nullptr};
        /// 槽位下标。
        std::size_t m_slot{0};
    };

    /// 构造空 store（`Load()` 返回空快照）。
    ConfigStore() noexcept = default;

    /// 用初始值构造；等价于构造后立即 `Store(initial)`。
    explicit ConfigStore(std::shared_ptr<const T> initial) {
        if (initial) {
            Store(std::move(initial));
        }
    }

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;
    ConfigStore(ConfigStore&&) = delete;
    ConfigStore& operator=(ConfigStore&&) = delete;

    ~ConfigStore() = default;

    /**
     * @brief 无锁获取当前配置快照。
     *
     * ## 步骤
     * step1. 读 `active`；从未发布过则返回空快照；
     * step2. 对槽位 `refs` 做 CAS 递增（跳过写者锁定中的槽）；
     * step3. 再读 `active` 确认没有换槽；不一致则释放并重试。
     *
     * @return 引用计数保护的快照；不阻塞、不获取互斥锁。
     * @note 极端竞争下可能重试若干次，但不会无限自旋：写者每次发布都会让
     *       旧槽保持可读，读者下一次一定能拿到某个有效版本。
     */
    Snapshot Load() const noexcept {
        for (;;) {
            const std::uint32_t index =
                m_active.load(std::memory_order_acquire);
            if (index >= kSlotCount) {
                return Snapshot();
            }
            std::uint32_t refs =
                m_slots[index].refs.load(std::memory_order_acquire);
            if (refs == kLocked) {
                continue;
            }
            if (!m_slots[index].refs.compare_exchange_weak(
                    refs, refs + 1, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                continue;
            }
            if (m_active.load(std::memory_order_acquire) == index) {
                return Snapshot(const_cast<ConfigStore*>(this), index);
            }
            Release(index);
        }
    }

    /// 无锁读的便捷形式：直接返回 `shared_ptr` 快照。
    std::shared_ptr<const T> LoadShared() const {
        return Load().share();
    }

    /**
     * @brief 发布新配置（写侧串行）。
     *
     * ## 步骤
     * step1. 取写锁（多个写者串行）；
     * step2. 选一个 `refs == 0` 的非 active 槽；没有则等待读者释放；
     * step3. CAS 锁定该槽，替换 `value`，发布 `active`，版本 +1，解锁槽；
     * step4. 唤醒可能等待空槽的写者（当前实现由条件变量谓词保证）。
     *
     * @param value 新配置；为空时忽略（不发布空配置）。
     * @note 写路径可能因为「7 个槽都被长期 Snapshot 占用」而等待；这是
     *       有意的背压，不覆盖读者正在读的数据。
     */
    void Store(std::shared_ptr<const T> value) {
        if (!value) {
            return;
        }
        std::lock_guard<std::mutex> write_lock(m_write_mutex);
        std::shared_ptr<const T> pending = std::move(value);
        for (;;) {
            std::size_t slot = 0;
            {
                std::unique_lock<std::mutex> wait_lock(m_wait_mutex);
                m_writer_waiting.fetch_add(1, std::memory_order_relaxed);
                m_wait_cv.wait(wait_lock, [this, &slot]() {
                    return FindAvailableSlotLocked(&slot);
                });
                m_writer_waiting.fetch_sub(1, std::memory_order_relaxed);
            }
            std::uint32_t expected = 0;
            if (!m_slots[slot].refs.compare_exchange_strong(
                    expected, kLocked, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                // 读者在「选中槽」与「锁定槽」之间抢先进入；重新选槽。
                continue;
            }
            m_slots[slot].value = std::move(pending);
            m_active.store(static_cast<std::uint32_t>(slot),
                           std::memory_order_release);
            m_version.fetch_add(1, std::memory_order_acq_rel);
            m_slots[slot].refs.store(0, std::memory_order_release);
            return;
        }
    }

    /// 当前版本号（每次 `Store` +1；无锁读）。
    std::uint64_t version() const noexcept {
        return m_version.load(std::memory_order_acquire);
    }

    /// 是否从未发布过配置。
    bool empty() const noexcept {
        return m_active.load(std::memory_order_acquire) >= kSlotCount;
    }

    /// 当前持有快照的读者数（诊断用，非精确时刻值）。
    std::size_t reader_count() const noexcept {
        std::size_t total = 0;
        for (std::size_t index = 0; index < kSlotCount; ++index) {
            const std::uint32_t refs =
                m_slots[index].refs.load(std::memory_order_relaxed);
            if (refs != kLocked && refs > 0) {
                total += refs;
            }
        }
        return total;
    }

private:
    /// 单个配置槽。
    struct Slot {
        /// 0=空闲；正数=读者数；kLocked=写者独占。
        std::atomic<std::uint32_t> refs{0};
        /// 配置对象；只在锁定状态下替换。
        std::shared_ptr<const T> value;
    };

    /// 增加引用（源快照已持有，写者不可能锁定该槽）。
    void AddRef(std::size_t slot) const noexcept {
        m_slots[slot].refs.fetch_add(1, std::memory_order_acquire);
    }

    /// 释放引用；最后一个读者离开时唤醒等待空槽的写者。
    void Release(std::size_t slot) const noexcept {
        if (m_slots[slot].refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (m_writer_waiting.load(std::memory_order_relaxed) != 0) {
                std::lock_guard<std::mutex> lock(m_wait_mutex);
                m_wait_cv.notify_one();
            }
        }
    }

    /// 槽位里的配置指针（读者持有引用期间稳定）。
    const T* SlotValue(std::size_t slot) const noexcept {
        return m_slots[slot].value.get();
    }

    /// 槽位里的 shared_ptr 拷贝（读者持有引用期间稳定）。
    std::shared_ptr<const T> SlotShared(std::size_t slot) const {
        return m_slots[slot].value;
    }

    /// 在等待锁内选择一个空闲且非 active 的槽（写者串行，读 m_next_slot 安全）。
    bool FindAvailableSlotLocked(std::size_t* slot) {
        const std::uint32_t active =
            m_active.load(std::memory_order_relaxed);
        for (std::size_t offset = 0; offset < kSlotCount; ++offset) {
            const std::size_t index = (m_next_slot + offset) % kSlotCount;
            if (index == active) {
                continue;
            }
            if (m_slots[index].refs.load(std::memory_order_relaxed) == 0) {
                *slot = index;
                m_next_slot = (index + 1) % kSlotCount;
                return true;
            }
        }
        return false;
    }

    /// 8 个槽；mutable 允许 const 的 Load 增加引用计数。
    mutable Slot m_slots[kSlotCount];
    /// 当前发布槽索引；kSlotCount 表示尚未发布。
    mutable std::atomic<std::uint32_t> m_active{
        static_cast<std::uint32_t>(kSlotCount)};
    /// 版本号。
    mutable std::atomic<std::uint64_t> m_version{0};
    /// 写者串行锁。
    mutable std::mutex m_write_mutex;
    /// 等待空槽用的互斥与条件变量。
    mutable std::mutex m_wait_mutex;
    mutable std::condition_variable m_wait_cv;
    /// 正在等待空槽的写者数（读者释放时据此决定是否 notify）。
    mutable std::atomic<std::uint32_t> m_writer_waiting{0};
    /// 下一个候选槽（写者串行，无需原子）。
    std::size_t m_next_slot{0};
};

/**
 * @brief 配置文件热更新器：监听 → 重载 → 校验 → 原子发布 → 回调。
 *
 * ## 定位
 * 把 `FileWatcher` 与 `ConfigStore<T>` 组合成开箱即用的热更新循环。调用方
 * 只提供一个把文件路径变成配置对象的 loader（可抛异常），其余由本类负责。
 *
 * ## 不变量
 * - 只有在 loader 成功且 validator 通过后才 `Store()`；失败保留旧快照；
 * - 发布与回调串行（`m_reload_mutex`），不会出现两个线程同时发布；
 * - `Load()` 完全转发给 `ConfigStore`，无锁；
 * - watcher 线程只置脏标记，重载在独立线程完成，慢解析不会阻塞事件读取。
 *
 * ## 失败语义
 * - `Start()`：初始加载失败则不启动（与 `Config::StartWatcher` 一致），返回
 *   具体错误；文件不存在也属于失败；
 * - 运行期重载失败：记录 `last_error()`、`failed_reloads()` +1、调用错误
 *   回调，旧配置继续服务；
 * - loader 抛异常按失败处理，绝不逃出监听线程。
 *
 * ## 线程模型
 * - `Start()`/`Stop()` 由调用方线程调用；
 * - 内部有两条线程：`FileWatcher` 的事件线程 + 本类的重载线程；
 * - `Load()`/`version()`/统计接口可在任意线程调用。
 */
template <typename T>
class ConfigWatcher final {
public:
    /// 从文件路径加载配置；失败返回空指针并写 `error`。
    using Loader = std::function<std::shared_ptr<const T>(
        const std::string& path, std::string* error)>;
    /// 校验新配置；返回 false 时写 `error`，旧配置保留。
    using Validator =
        std::function<bool(const T& value, std::string* error)>;
    /// 发布成功回调（在重载线程/手动调用线程执行，参数是刚发布的快照）。
    using ReloadCallback =
        std::function<void(const T& value, std::uint64_t version)>;
    /// 失败回调。
    using ErrorCallback = std::function<void(const std::string& message)>;

    /// 快照类型别名。
    using Snapshot = typename ConfigStore<T>::Snapshot;

    /// 热更新参数。
    struct Options {
        /// 底层文件监听参数。
        FileWatchOptions watch{};
        /// 事件去抖窗口：把突发多次写合并成一次重载。
        std::chrono::milliseconds debounce{10};
        /// 启动时是否先同步加载一次（默认 true；false 时允许空配置启动）。
        bool reload_on_start{true};
    };

    /**
     * @brief 绑定路径、loader 与参数；不启动线程。
     * @param path 配置文件路径。
     * @param loader 解析函数；空函数会让 `Start()` 失败。
     * @param options 热更新参数。
     */
    ConfigWatcher(std::string path, Loader loader, Options options = {})
        : m_path(std::move(path)),
          m_loader(std::move(loader)),
          m_options(std::move(options)),
          m_watcher(m_path, m_options.watch) {}

    /// 停止监听并回收线程。
    ~ConfigWatcher() { Stop(); }

    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;
    ConfigWatcher(ConfigWatcher&&) = delete;
    ConfigWatcher& operator=(ConfigWatcher&&) = delete;

    /**
     * @brief 启动热更新：初始加载 + 启动文件监听 + 启动重载线程。
     * @param error 失败原因（可为空）。
     * @return 成功返回 true；loader 为空、初始加载失败或监听启动失败返回 false。
     */
    bool Start(std::string* error = nullptr) {
        std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
        if (m_running.load(std::memory_order_acquire)) {
            if (error != nullptr) {
                *error = "配置热更新已在运行";
            }
            return false;
        }
        if (!m_loader) {
            if (error != nullptr) {
                *error = "配置 loader 为空";
            }
            return false;
        }
        if (m_path.empty()) {
            if (error != nullptr) {
                *error = "配置文件路径为空";
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = false;
        }
        m_running.store(true, std::memory_order_release);
        if (m_options.reload_on_start) {
            std::string load_error;
            if (!ReloadOnce(&load_error)) {
                m_running.store(false, std::memory_order_release);
                if (error != nullptr) {
                    *error = load_error;
                }
                return false;
            }
        }
        m_watcher.SetCallback([this](FileChange /*change*/) { MarkDirty(); });
        std::string watch_error;
        if (!m_watcher.Start(&watch_error)) {
            m_running.store(false, std::memory_order_release);
            if (error != nullptr) {
                *error = watch_error.empty() ? "文件监听启动失败" : watch_error;
            }
            return false;
        }
        m_thread = std::thread([this]() { ReloadLoop(); });
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    /// 停止监听与重载线程（幂等；可在任意线程调用）。
    void Stop() {
        std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
        if (!m_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_watcher.Stop();
        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id()) {
                m_thread.detach();
            } else {
                m_thread.join();
            }
        }
    }

    /// 热更新是否在运行。
    bool running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }

    /// 无锁获取当前配置快照。
    Snapshot Load() const noexcept { return m_store.Load(); }

    /// 无锁获取当前配置的 `shared_ptr`（可长期持有）。
    std::shared_ptr<const T> LoadShared() const {
        return m_store.LoadShared();
    }

    /// 当前配置版本（每次成功发布 +1）。
    std::uint64_t version() const noexcept { return m_store.version(); }
    /// 成功重载次数。
    std::uint64_t reloads() const noexcept {
        return m_reloads.load(std::memory_order_relaxed);
    }
    /// 失败重载次数（loader/validator 失败）。
    std::uint64_t failed_reloads() const noexcept {
        return m_failed_reloads.load(std::memory_order_relaxed);
    }
    /// 最近一次失败原因；成功后清空。
    std::string last_error() const {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        return m_last_error;
    }
    /// 底层监听是否使用 inotify。
    bool uses_inotify() const noexcept { return m_watcher.uses_inotify(); }
    /// 监听到的原始变更事件数。
    std::uint64_t change_events() const noexcept {
        return m_watcher.change_count();
    }
    /// 配置路径。
    const std::string& path() const noexcept { return m_path; }

    /// 设置校验器（建议在 `Start()` 之前调用；运行期替换是线程安全的）。
    void SetValidator(Validator validator) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_validator = std::move(validator);
    }

    /// 设置发布成功回调（线程安全）。
    void OnReload(ReloadCallback callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_on_reload = std::move(callback);
    }

    /// 设置失败回调（线程安全）。
    void OnError(ErrorCallback callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_on_error = std::move(callback);
    }

    /**
     * @brief 同步执行一次重载（不依赖监听事件）。
     * @param error 失败原因（可为空）。
     * @return 成功发布返回 true；失败时旧配置保留。
     * @note 与后台重载线程互斥（`m_reload_mutex`），可安全并发调用。
     */
    bool ReloadNow(std::string* error = nullptr) {
        return ReloadOnce(error);
    }

private:
    /// 重载线程循环：等脏标记 → 去抖 → 重载。
    void ReloadLoop() {
        for (;;) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_stop || m_dirty; });
            if (m_stop) {
                return;
            }
            m_dirty = false;
            lock.unlock();
            if (m_options.debounce.count() > 0) {
                std::unique_lock<std::mutex> debounce_lock(m_mutex);
                m_cv.wait_for(debounce_lock, m_options.debounce,
                              [this]() { return m_stop; });
                if (m_stop) {
                    return;
                }
                // 合并窗口内的重复事件：本轮统一读最新文件内容。
                m_dirty = false;
            }
            std::string error;
            (void)ReloadOnce(&error);
        }
    }

    /// 监听回调：只置脏标记并唤醒重载线程。
    void MarkDirty() {
        if (!m_running.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_dirty = true;
        }
        m_cv.notify_one();
    }

    /// 执行一次「读取 → 校验 → 发布 → 回调」；失败保留旧配置。
    bool ReloadOnce(std::string* error) {
        std::lock_guard<std::mutex> reload_lock(m_reload_mutex);
        std::string local_error;
        std::shared_ptr<const T> value;
        try {
            value = m_loader(m_path, &local_error);
        } catch (const std::exception& exception) {
            local_error = std::string("loader 异常: ") + exception.what();
        } catch (...) {
            local_error = "loader 抛出未知异常";
        }
        if (!value) {
            return Fail(local_error.empty() ? "配置加载失败" : local_error,
                        error);
        }
        Validator validator;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            validator = m_validator;
        }
        if (validator) {
            std::string validate_error;
            bool valid = false;
            try {
                valid = validator(*value, &validate_error);
            } catch (const std::exception& exception) {
                validate_error =
                    std::string("validator 异常: ") + exception.what();
            } catch (...) {
                validate_error = "validator 抛出未知异常";
            }
            if (!valid) {
                return Fail(validate_error.empty() ? "配置校验失败"
                                                   : validate_error,
                            error);
            }
        }
        m_store.Store(value);
        const std::uint64_t published = m_store.version();
        m_reloads.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_error_mutex);
            m_last_error.clear();
        }
        ReloadCallback callback;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            callback = m_on_reload;
        }
        if (callback) {
            try {
                callback(*value, published);
            } catch (...) {
                // 用户回调异常不能终止热更新循环。
            }
        }
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    /// 记录失败并调用错误回调；旧配置保持不变。
    bool Fail(const std::string& message, std::string* error) {
        {
            std::lock_guard<std::mutex> lock(m_error_mutex);
            m_last_error = message;
        }
        m_failed_reloads.fetch_add(1, std::memory_order_relaxed);
        if (error != nullptr) {
            *error = message;
        }
        ErrorCallback callback;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            callback = m_on_error;
        }
        if (callback) {
            try {
                callback(message);
            } catch (...) {
                // 同上：错误回调异常不影响监听。
            }
        }
        return false;
    }

    /// 配置文件路径。
    std::string m_path;
    /// 解析函数。
    Loader m_loader;
    /// 参数。
    Options m_options;
    /// 底层文件监听。
    FileWatcher m_watcher;
    /// 配置存储（无锁读）。
    ConfigStore<T> m_store;

    /// 生命周期锁（Start/Stop）。
    std::mutex m_lifecycle_mutex;
    /// 重载线程等待的互斥与条件变量。
    std::mutex m_mutex;
    std::condition_variable m_cv;
    /// 停止标志。
    bool m_stop{false};
    /// 脏标记。
    bool m_dirty{false};
    /// 运行标志。
    std::atomic<bool> m_running{false};
    /// 重载串行锁（后台线程与 `ReloadNow()` 互斥）。
    std::mutex m_reload_mutex;
    /// 保护回调与校验器。
    std::mutex m_callback_mutex;
    /// 发布成功回调。
    ReloadCallback m_on_reload;
    /// 失败回调。
    ErrorCallback m_on_error;
    /// 校验器。
    Validator m_validator;
    /// 保护错误文本。
    mutable std::mutex m_error_mutex;
    /// 最近错误。
    std::string m_last_error;
    /// 重载线程。
    std::thread m_thread;
    /// 成功重载计数。
    std::atomic<std::uint64_t> m_reloads{0};
    /// 失败重载计数。
    std::atomic<std::uint64_t> m_failed_reloads{0};
};

// ============================================================================
// FileWatcher 实现
// ============================================================================

inline FileWatcher::FileWatcher(std::string path, FileWatchOptions options)
    : m_path(std::move(path)), m_options(std::move(options)) {
    const std::filesystem::path file_path(m_path);
    const std::filesystem::path directory =
        file_path.parent_path().empty() ? std::filesystem::path(".")
                                        : file_path.parent_path();
    m_directory = directory.string();
    m_basename = file_path.filename().string();
}

inline FileWatcher::~FileWatcher() { Stop(); }

inline void FileWatcher::SetCallback(Callback callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_callback = std::move(callback);
}

inline std::string FileWatcher::last_error() const {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    return m_last_error;
}

inline void FileWatcher::RecordError(std::string message) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_last_error = std::move(message);
}

inline void FileWatcher::Notify(FileChange change) {
    m_change_count.fetch_add(1, std::memory_order_relaxed);
    Callback callback;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        callback = m_callback;
    }
    if (callback) {
        try {
            callback(change);
        } catch (...) {
            // 监听回调异常不影响事件循环。
        }
    }
}

inline bool FileWatcher::StartInotify() {
#if defined(__linux__)
    if (!m_options.prefer_inotify) {
        return false;
    }
    m_inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_inotify_fd < 0) {
        RecordError("inotify_init1 失败，回退轮询");
        return false;
    }
    // 只订阅「稳定状态」事件：CLOSE_WRITE 表示一次写入已结束，MOVED_TO
    // 表示原子替换完成。IN_MODIFY/IN_CREATE 会在写入中途触发，读到的
    // 是半截文件，因此刻意不订阅。
    const std::uint32_t mask = IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_TO |
                               IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF |
                               IN_MOVE_SELF;
    m_watch_descriptor =
        ::inotify_add_watch(m_inotify_fd, m_directory.c_str(), mask);
    if (m_watch_descriptor < 0) {
        ::close(m_inotify_fd);
        m_inotify_fd = -1;
        RecordError("inotify_add_watch 失败，回退轮询");
        return false;
    }
    RecordError("");
    return true;
#else
    return false;
#endif
}

inline void FileWatcher::CloseInotify() {
#if defined(__linux__)
    if (m_inotify_fd >= 0) {
        if (m_watch_descriptor >= 0) {
            (void)::inotify_rm_watch(m_inotify_fd, m_watch_descriptor);
        }
        (void)::close(m_inotify_fd);
    }
#endif
    m_inotify_fd = -1;
    m_watch_descriptor = -1;
}

inline void FileWatcher::InotifyLoop() {
#if defined(__linux__)
    while (m_running.load(std::memory_order_acquire)) {
        struct pollfd descriptor {};
        descriptor.fd = m_inotify_fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 50);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            RecordError("inotify poll 失败");
            break;
        }
        if (ready == 0) {
            continue;
        }
        if ((descriptor.revents & POLLIN) == 0) {
            continue;
        }
        char buffer[4096];
        const ssize_t count = ::read(m_inotify_fd, buffer, sizeof(buffer));
        if (count <= 0) {
            if (errno == EAGAIN) {
                continue;
            }
            RecordError("inotify read 失败");
            break;
        }
        bool modified = false;
        bool replaced = false;
        bool removed = false;
        std::size_t offset = 0;
        while (offset + sizeof(struct inotify_event) <=
               static_cast<std::size_t>(count)) {
            const auto* event = reinterpret_cast<const struct inotify_event*>(
                buffer + offset);
            const bool self_event =
                (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0;
            const bool name_matches =
                event->len > 0 && m_basename == event->name;
            if (self_event || name_matches) {
                if ((event->mask &
                     (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF)) != 0) {
                    removed = true;
                }
                if ((event->mask & IN_MOVED_TO) != 0) {
                    replaced = true;
                }
                // 只认「写已闭合」与属性变化；IN_MODIFY/IN_CREATE 不在
                // 订阅掩码里，这里再显式排除以防御未来放宽掩码。
                if ((event->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) != 0 &&
                    (event->mask & (IN_MODIFY | IN_CREATE)) == 0) {
                    modified = true;
                }
            }
            offset += sizeof(struct inotify_event) + event->len;
        }
        // 同一批事件只上报一次，避免一次写入触发多次重载；优先级：
        // 删除 > 替换 > 修改。
        if (removed) {
            Notify(FileChange::kRemoved);
        } else if (replaced) {
            Notify(FileChange::kReplaced);
        } else if (modified) {
            Notify(FileChange::kModified);
        }
    }
#endif
}

inline bool FileWatcher::ReadStamp(Stamp* stamp) const {
    if (stamp == nullptr) {
        return false;
    }
    struct stat info {};
    if (::stat(m_path.c_str(), &info) != 0) {
        stamp->exists = false;
        stamp->inode = 0;
        stamp->mtime_ns = 0;
        stamp->size = 0;
        return false;
    }
    stamp->exists = true;
    stamp->inode = static_cast<std::uint64_t>(info.st_ino);
    stamp->size = static_cast<std::uint64_t>(info.st_size);
#if defined(__APPLE__)
    stamp->mtime_ns = static_cast<std::int64_t>(info.st_mtimespec.tv_sec) *
                          1000000000LL +
                      static_cast<std::int64_t>(info.st_mtimespec.tv_nsec);
#elif defined(__linux__)
    stamp->mtime_ns = static_cast<std::int64_t>(info.st_mtim.tv_sec) *
                          1000000000LL +
                      static_cast<std::int64_t>(info.st_mtim.tv_nsec);
#else
    stamp->mtime_ns = static_cast<std::int64_t>(info.st_mtime) * 1000000000LL;
#endif
    return true;
}

inline void FileWatcher::PollLoop(Stamp previous, bool present) {
    while (m_running.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(m_wait_mutex);
            m_wait_cv.wait_for(lock, m_options.poll_interval, [this]() {
                return !m_running.load(std::memory_order_acquire);
            });
        }
        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }
        Stamp current;
        const bool exists = ReadStamp(&current);
        bool changed = false;
        FileChange change = FileChange::kModified;
        if (!present && exists) {
            change = FileChange::kCreated;
            changed = true;
        } else if (present && !exists) {
            change = FileChange::kRemoved;
            changed = true;
        } else if (present && exists &&
                   (current.inode != previous.inode ||
                    current.mtime_ns != previous.mtime_ns ||
                    current.size != previous.size)) {
            change = FileChange::kModified;
            changed = true;
        }
        previous = current;
        present = exists;
        if (changed) {
            Notify(change);
        }
    }
}

inline bool FileWatcher::Start(std::string* error) {
    std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
    if (m_running.load(std::memory_order_acquire)) {
        if (error != nullptr) {
            *error = "文件监听已在运行";
        }
        return false;
    }
    if (m_path.empty()) {
        if (error != nullptr) {
            *error = "监听路径为空";
        }
        return false;
    }
    const bool inotify_ready = StartInotify();
    m_uses_inotify.store(inotify_ready, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    if (inotify_ready) {
        m_thread = std::thread([this]() { InotifyLoop(); });
    } else {
        // 基线必须在 Start() 返回前记录：若放到线程里读，调用方在 Start()
        // 之后立刻写文件时，轮询线程可能把「已改过的文件」当基线，导致这次
        // 变更永久漏检。这里同步读一次，Start() 之后的变更一定能被比较出来。
        Stamp initial;
        const bool present = ReadStamp(&initial);
        m_thread = std::thread(
            [this, initial, present]() { PollLoop(initial, present); });
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

inline void FileWatcher::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lifecycle(m_lifecycle_mutex);
        if (!m_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        m_wait_cv.notify_all();
        worker = std::move(m_thread);
    }
    if (worker.joinable()) {
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
        } else {
            worker.join();
        }
    }
    CloseInotify();
}

}  // namespace go2cpp::config
