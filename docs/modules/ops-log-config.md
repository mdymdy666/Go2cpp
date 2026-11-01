# 运维能力：日志轮转 + 配置热更新

> 模块：`go2cpp::log`（`include/go2cpp/logging/rotate.hpp`）、
> `go2cpp::config`（`include/go2cpp/config/watch.hpp`）
> 状态：已实现，未 commit。报告：`/tmp/ops_log_config.md`

两个 header-only 模块，分别解决生产运维里最常见、也最容易做错的两件事：
**日志不能丢**、**配置热更新要快且读侧不能加锁**。它们与既有模块是组合关系，
不修改 `logging/log.hpp` 与 `config/config.hpp`。

---

## 1. 日志轮转（`logging/rotate.hpp`）

### 1.1 定位与能力

`log.hpp` 自带的 `RotatingFileSink` 只做「按大小把文件重命名成 `.1/.2`」：
不压缩、不按时间周期、写路径内同步重命名、失败后无重试。`rotate.hpp` 是独立
叠加的完整实现：

| 能力 | 实现 |
| --- | --- |
| 按大小轮转 | `RotationOptions::max_bytes`（0 表示关闭） |
| 按时间轮转 | `RotationPeriod::kHourly` / `kDaily`（本地自然小时/自然日） |
| 保留 N 份 | `max_files`（归档份数，不含当前文件）；后台按文件名删除最旧 |
| gzip 压缩 | 自研 `compress::Gzip::Writer`；先写 `.gz.tmp` 再原子 rename |
| 不丢日志 | 所有写入先进内存缓冲，`fwrite` 完整成功后才移除；短写保留剩余 |
| 轮转期写入缓冲 | 轮转临界区只做 flush/fsync/rename/fopen，压缩与删除全部异步 |
| 失败可重试 | `fopen`/短写/rename 失败都保留数据；`pending_bytes()` 可观测 |
| 日志流水线接入 | `RotatingLogSink`（`LogSink` 适配器，支持 `WriteRaw`） |

### 1.2 关键类型

```cpp
namespace go2cpp::log {

enum class RotationPeriod : std::uint8_t { kNone, kHourly, kDaily };

struct RotationOptions {
    std::size_t max_bytes{10u * 1024u * 1024u};  // 单文件上限；0=关闭
    std::size_t max_files{5};                    // 归档份数；0=不保留
    RotationPeriod period{RotationPeriod::kNone};
    bool compress{true};
    int compression_level{Gzip::DefaultCompression};
    std::size_t write_buffer_bytes{64u * 1024u};
    std::size_t max_pending_bytes{64u * 1024u * 1024u};  // 磁盘故障安全阀
    bool fsync_on_rotate{true};
    bool create_dirs{true};
    std::function<std::chrono::system_clock::time_point()> clock;  // 可注入
};

class RotatingLogFile {
public:
    RotatingLogFile(std::string path, RotationOptions options = {});
    std::size_t Write(std::string_view data);  // 返回接受的字节
    bool Flush();                              // fflush
    bool Sync();                               // fflush + fsync
    bool Reopen();                             // 外部 logrotate 后重绑
    bool RotateNow();                          // SIGHUP 等手动轮转
    void Close();                              // 等压缩队列清空
    std::uint64_t rotations() const;
    std::uint64_t compressed_files() const;
    std::uint64_t pruned_files() const;
    std::uint64_t compress_errors() const;
    std::uint64_t dropped_bytes() const;       // 正常应恒为 0
    std::size_t pending_bytes() const;
    std::string last_error() const;
};

class RotatingLogSink final : public LogSink { /* Write / WriteRaw / Flush / Reopen */ };

}  // namespace go2cpp::log
```

### 1.3 轮转协议（不丢日志的原因）

```
Write(data)
  ├─ 已 Close → 返回 0
  ├─ 文件未打开 → 惰性 fopen（失败：data 继续留缓冲）
  ├─ 周期检查（仅 kHourly/kDaily）→ 跨周期先 RotateLocked
  ├─ 大小检查（m_bytes + pending + data > max_bytes）→ RotateLocked
  ├─ pending.append(data)
  └─ pending >= write_buffer_bytes → fwrite + fflush
```

`RotateLocked()` 的顺序是固定的：

1. `FlushPendingLocked()`：把缓冲完整写入**旧文件**；失败则放弃本次轮转
   （数据仍在缓冲，不丢）；
2. `fsync`（可关）→ `fclose`；
3. `rename(current → current.YYYYMMDD-HHMMSS-NNNNN)`；失败则重新打开原文件
   继续追加，下一轮再试；
4. `fopen` 新文件；失败时后续 `Write` 继续缓冲，`Flush` 重试；
5. 把归档压缩任务与清理任务入队到**单个后台线程**（FIFO，串行）。

任意时刻每条记录只可能处于两种状态之一：在 `m_pending`，或已完整写入文件。
不存在「从缓冲删除但尚未写盘」的窗口。磁盘长期不可写时，`max_pending_bytes`
是唯一会拒收新数据的地方（计入 `dropped_bytes`，正常恒为 0）。

### 1.4 文件命名与保留

- 当前文件：构造时路径（如 `logs/app.log`），`fopen(..., "ab")` 追加；
- 归档：`logs/app.log.20261101-110012-00001`，时间戳为本地时间，序号固定
  5 位，同一秒内多次轮转也不会冲突；字典序即轮转顺序；
- gzip：完整归档 → 同名 `.gz`（先 `.gz.tmp` 再 rename，失败保留原文件）；
- 保留：每次轮转入队一个清理任务，按文件名排序删除超过 `max_files` 的最旧
  归档；压缩与清理在同一后台线程串行，不会删到正在压缩的文件；
- `max_files == 0`：轮转后删除全部归档（不是「不清理」）。

### 1.5 性能

- 写路径：`std::string::append` + 每 64 KiB 一次 `fwrite`/`fflush`；按大小轮转
  的常规路径不读时钟，只有轮转时 `system_clock::now()` 一次；
- 按时间轮转：每次 `Write` 读一次时钟（vDSO，约 20 ns），追求极致吞吐时
  只开大小轮转；
- 压缩、清理在后台线程；`Close()` 会等队列清空（完整性优先）。

实测（Release，WSL ext4，`/tmp`）：

| 指标 | 数值 |
| --- | --- |
| 1e6 条、约 39.9 MB、12 次轮转 | 写入 0.160 s（约 238 MB/s） |
| 关闭（压缩 12 个归档） | 0.157 s |
| 端到端 | 0.317 s，约 120 MB/s |
| 丢行 / 乱序 / 压缩失败 | 0 / 0 / 0 |

> 数据是单次运行结果；测试机同时有其他任务，绝对吞吐会波动，正确性断言
> 不依赖时间。

---

## 2. 配置热更新（`config/watch.hpp`）

### 2.1 定位

`config.hpp` 的 `Config::StartWatcher()` 是进程级注册表的轮询监听。本模块是
独立的单文件热更新原语，提供：

- `FileWatcher`：inotify 优先、轮询兜底；
- `ConfigStore<T>`：RCU 风格的多版本存储，**读侧无锁**；
- `ConfigWatcher<T>`：监听 → 重载 → 校验 → 原子发布 → 回调。

### 2.2 监听语义

| 事件 | inotify 分支 | 轮询分支 |
| --- | --- | --- |
| 普通写入（open/truncate/write/close） | 只听 `IN_CLOSE_WRITE`，写一半不会被读到 | 比较 inode/mtime(ns)/size |
| 原子替换（写 `.tmp` + rename） | `IN_MOVED_TO` → `kReplaced` | inode 变化 → 检出 |
| 删除 / 移走 | `IN_DELETE` / `IN_MOVED_FROM` → `kRemoved` | 存在→不存在 |
| 重建 | `IN_CLOSE_WRITE`（`IN_CREATE` 忽略） | 不存在→存在 |

inotify 监听**父目录**而不是文件 inode：`rename` 覆盖不会让监听失效。
`IN_MODIFY`/`IN_CREATE` 刻意不订阅——它们在写入中途触发，读到的必然是半截
文件；只处理「写已闭合」可以把这类无意义的失败重载从源头消除。用 mmap
写文件不产生 `CLOSE_WRITE`，这类写入方应改为原子替换或轮询。

轮询的固有边界：原地覆盖如果**同尺寸**且发生在同一个文件系统时间戳粒度
（常见 1–4 ms）内，可能漏检一次；推荐配合原子替换。inotify 模式没有这个
限制。

### 2.3 `ConfigStore<T>`：无锁读协议

8 个槽位，每槽：

```cpp
struct Slot {
    std::atomic<std::uint32_t> refs{0};  // 0=空闲；正数=读者；最高位=写者锁定
    std::shared_ptr<const T> value;      // 只在 refs==kLocked 时替换
};
```

读者（`Load()`，无任何互斥锁）：

1. `index = active.load(acquire)`；
2. 对 `refs` 做 CAS 递增；读到 `kLocked` 或 CAS 失败则回到第 1 步；
3. 再读 `active` 确认仍是 `index`，是则返回 `Snapshot`，否则释放并重试。

写者（`Store()`，串行）：

1. 选一个 `refs == 0` 且不是 active 的槽（没有就等读者释放）；
2. CAS `0 → kLocked` 锁定；
3. 替换 `value`，`active.store(release)`，版本 +1；
4. `refs.store(0, release)` 解锁。

因此读者要么完整看到旧版本、要么完整看到新版本，不会读到写了一半的配置；
写者更新不会阻塞读，读者也不会阻塞写（除写者自己等待空槽）。

为什么不用 `std::atomic<std::shared_ptr<T>>`：那是 C++20；C++17 的
`std::atomic_load(&shared_ptr)` 在 libstdc++ 内部是自旋锁池，读者在锁被占用
时会自旋等待，不满足「无锁读」。本实现读侧不进入任何互斥锁。

**槽位约束**：8 个槽中最多 7 个可被长期持有；写者在全部非 active 槽都被
`Snapshot` 占用时会等待。不要跨请求长期持有 `Snapshot`，需要时用
`Snapshot::share()` 拷成 `shared_ptr<const T>`（不受槽位限制）。

### 2.4 `ConfigWatcher<T>`

```cpp
template <typename T>
class ConfigWatcher {
public:
    using Loader = std::function<std::shared_ptr<const T>(
        const std::string& path, std::string* error)>;
    struct Options {
        FileWatchOptions watch{};              // poll_interval / prefer_inotify
        std::chrono::milliseconds debounce{10};
        bool reload_on_start{true};
    };
    ConfigWatcher(std::string path, Loader loader, Options options = {});
    bool Start(std::string* error = nullptr);  // 初始加载 + 监听 + 线程
    void Stop();
    Snapshot Load() const noexcept;            // 无锁
    std::uint64_t version() const noexcept;
    std::uint64_t reloads() const noexcept;
    std::uint64_t failed_reloads() const noexcept;
    bool ReloadNow(std::string* error = nullptr);
    void SetValidator(Validator validator);
    void OnReload(ReloadCallback callback);
    void OnError(ErrorCallback callback);
};
```

线程模型：

```
写方 ──> 文件
          │ inotify/轮询
          ▼
   FileWatcher 线程 ──(置脏标记)──> ConfigWatcher 重载线程
                                      ├─ debounce 合并
                                      ├─ loader + validator
                                      ├─ 失败：保留旧快照，failed+1
                                      └─ 成功：ConfigStore::Store 原子发布 → 回调
```

- 监听线程只置脏标记，慢解析不会阻塞内核事件读取；
- `debounce` 把突发多次写合并成一次重载，每次读文件**最新内容**；
- 失败（文件暂时不完整、校验失败、loader 异常）保留旧配置，监听线程不退出；
- `Start()` 的初始加载失败则不启动（与 `Config::StartWatcher` 一致）。

### 2.5 实测数据（WSL，Release）

| 指标 | 数值 |
| --- | --- |
| 1e4 次变更（inotify，每次等待版本前进） | 2.066 s，206.6 µs/次，0 失败 |
| 1e4 次变更事件数 | 10000（每次 CLOSE_WRITE 一次回调） |
| 4 读者 × 2000 次发布 | 9.57 M 次 `Load()`，0 撕裂 |
| `ConfigStore::Load()`（单线程） | 3.58 ns/次 |
| `ConfigStore::Store()`（1 写者 + 4 读者） | 1.0 µs/次，0 撕裂 |
| 轮询回退（50 次变更，1 ms 周期） | 全部生效，0 失败 |

---

## 3. 构建与测试

### 3.1 文件

```
include/go2cpp/logging/rotate.hpp          新增（header-only）
include/go2cpp/config/watch.hpp            新增（header-only）
tests/logging/test_log_rotate.cpp          新增（自带 main）
tests/config/test_config_watch.cpp         新增（自带 main）
cmake/Go2CppOpsLogConfig.cmake             新增（测试目标注册）
CMakeLists.txt                             末尾追加一行 include(...)
docs/modules/ops-log-config.md             本文档
```

### 3.2 构建与运行

```sh
cd /UserData/CodexWorkSpace/Go2cpp
cmake -S . -B .build-bento-wsl -DCMAKE_BUILD_TYPE=Release
cmake --build .build-bento-wsl --target go2cpp_log_rotate_test go2cpp_config_watch_test -j
ctest --test-dir .build-bento-wsl -R 'go2cpp_(log_rotate|config_watch)_test' --output-on-failure
```

关闭：`-DGO2CPP_WITH_OPS_LOG_CONFIG=OFF`。两个目标都是测试可执行文件，
不新增库、不进入 `install(EXPORT)`（头文件随顶层 `install(DIRECTORY include/)`）。

### 3.3 测试覆盖

`test_log_rotate.cpp`（8 个场景，全部通过）：

1. 按大小轮转 + gzip 归档 + 完整序列（4000 行，≥10 次轮转）；
2. 按天轮转（注入时钟）：同日不轮转、跨日轮转；
3. 保留 N 份：20 次轮转后恰好剩 `max_files` 个归档，最旧被删；
4. 不压缩 + `RotatingLogSink`（`Write`/`WriteRaw`/`Reopen`）；
5. 8 线程 × 5000 条并发写 + 高频轮转：id 唯一、行完整；
6. 失败恢复：目标先是目录，数据留缓冲；障碍移除后完整落盘；
7. 1e6 条 / 12 次轮转：逐行验证 0..999999 连续；
8. 随机混合：随机记录长度、上限、保留数，3 轮各 2 万条。

`test_config_watch.cpp`（9 个场景，全部通过）：

1. 单次修改生效 + 快照拷贝/移动语义；
2. 1e4 次连续变更（原子替换写入），0 失败；
3. 原子替换（`IN_MOVED_TO`）20 次；
4. 非法配置（loader 失败 + validator 失败）保留旧值，恢复后继续；
5. 删除 / 重建；
6. 4 读者并发 + 2000 次发布：`a+b==sum` 不变量 0 撕裂；
7. 轮询回退 50 次变更；
8. 读侧基准：1e6 次无锁 `Load()` + 4 读 1 写并发；
9. `Stop()` 后不再自动重载，`ReloadNow()` 仍可手动刷新。

---

## 4. 边界与已知限制

### 日志轮转

- `RotatingLogFile` 不做格式化、不带级别；接日志流水线用 `RotatingLogSink`；
- `Write` 只保证「数据已接受并在内存/文件之一」，要保证落盘需显式 `Flush()`
  或 `Sync()`；进程被 `SIGKILL` 时未刷出的缓冲会丢（所有用户态日志共性）；
- `Close()` 会等待压缩队列清空，归档很大时关闭可能变慢（完整性优先）；
- 单条记录大于 `max_bytes` 时仍整条写入，不拆行；
- 磁盘长期不可写超过 `max_pending_bytes` 时拒收新数据并计入 `dropped_bytes`；
- 归档名使用本地时间 + 进程内序号；多进程同时写同一路径不受支持；
- `Reopen()` 只重开当前文件，不触发轮转，适合外部 logrotate 移走文件后的重绑。

### 配置热更新

- 写路径有互斥锁；读路径无锁但需要 `Snapshot` 生命周期正确（不跨 `ConfigStore`
  析构、不长期占用槽位）；
- 槽位固定 8 个，7 个长期快照会让写者等待（背压，不覆盖数据）；
- inotify 事件合并：连续快速写可能只触发一次重载；`debounce` 进一步合并。
  需要「每次写都对应一次重载」时，写入方串行等待 `version()` 前进；
- 轮询的 mtime 粒度漏检风险（同尺寸、原地覆盖）见 2.2；
- 不递归监听目录、不监听多文件；一个 `ConfigWatcher` 对应一个文件；
- 不支持 Windows（inotify 分支编译为空、轮询分支可用，但未在 Windows 上验证）。

---

## 5. 设计取舍（与既有模块的关系）

| 决策 | 原因 |
| --- | --- |
| 不改 `log.hpp` / `config.hpp` | 既有 `RotatingFileSink`/`Config::StartWatcher` 的行为被其他测试与示例依赖；新能力以组合方式叠加 |
| 独立 `rotate.hpp` 而不是扩展 `RotatingFileSink` | 轮转器需要「缓冲 + 异步压缩 + 失败重试」一整套状态机，塞进 Sink 会让 `log.hpp` 的锁模型变复杂 |
| 独立 `watch.hpp` 而不是扩展 `Config` | `Config` 是进程级注册表（全局单例、事务、`ConfigVar`），热更新原语需要能承载任意配置类型 |
| 选用自研 gzip | 仓库已有 `compress` 模块且不再依赖系统 zlib；测试用独立解压路径（`Gzip::Reader` 校验 CRC/ISIZE）证明格式正确 |
| `ConfigStore` 不用 `shared_ptr` 引用计数做读路径 | 需要「读者零锁」；槽位 CAS 的读开销比 `atomic_load(shared_ptr)` 低且无内部自旋锁 |
