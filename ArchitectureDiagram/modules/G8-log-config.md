# G8 · 横切设施：`log` / `config`

> 依赖：`log` 只依赖 C++17 标准库（可独立使用，也能通过 `CallbackSink` 接 spdlog/fmt）；
> `config` 依赖 `log`（`log::Level` 编解码、`BindLoggingConfig`）与 `scheduler`（`SchedulerConfig`、`RuntimeConfig`）。
> 规模（实测）：`logging/log.hpp` 506 + `src/logging/log.cpp` 1024；
> `config/config.hpp` 541 + `src/config/config.cpp` 1095。
> 两者是**横切设施**：几乎所有模块都会用到（`GO2CPP_LOG_*` 宏、`Config::Lookup`）。

---

## 0. 一句话定位

`log` 是「Filter × Formatter × Sink × Worker」四层组合的**同步**日志（无 worker 线程、无队列）；
`config` 是「INI + 强类型 `ConfigVar<T>` + 两阶段事务 + 文件监听」的配置中心，
用 `path + ".lock"` 编辑锁协议避免读到半文件。

---

## 1. `log`

### 1.1 级别与记录

```cpp
enum class Level : std::uint8_t {
  Trace = 0, Debug, Info, Notice, Warn, Error, Crit, Alert, Critical, Off, Fatal = Critical
};
const char* ToString(Level) noexcept;   bool ParseLevel(std::string_view, Level*) noexcept;

struct LogRecord {
  Level level{Level::Info};  std::string logger, message, file, function;
  std::uint64_t timestamp_ms{0}, thread_id{0}, fiber_id{0}, elapse_ms{0};
  std::string thread_name;   int line{0};
};
```

注意 `Fatal` 是 `Critical` 的**别名**（同值），另有 `Notice`/`Crit`/`Alert` 三级。

### 1.2 四层组合

| 层 | 接口 | 内置实现 |
|---|---|---|
| Filter | `LogFilter::Accept(const LogRecord&)` | `MinimumLevelFilter`（`atomic<Level>`，默认由 Logger 级别决定） |
| Formatter | `LogFormatter::Format(const LogRecord&) → std::string` | `PatternFormatter`（占位符 → `Item` 列表） |
| Sink | `LogSink::Write(record, formatted)` / `Flush()` / `Reopen()` | `FileSink`、`RotatingFileSink`、`StdoutSink`、`StderrSink`、`MemorySink`、`CallbackSink` |
| Worker | `LogItemWorker(Filter, Formatter, Sink)` + `Submit(record)` | 组合上面三层；配置以 `shared_ptr<const Snapshot>` **原子替换** |

`LogItemWorker` 的热路径只做**一次原子 `shared_ptr` 读取**（`m_snapshot`），
所以配置变更（`SetFilter/SetFormatter/SetSink`）不会让每条日志去争配置锁。

### 1.3 `Item` 插件化（本模块最灵活也最容易踩坑的地方）

```cpp
class LogFormatter {
  class Item {                       // 两个虚函数入口，覆写任意一个即可
    virtual void Format(std::ostream&, const LogRecord&) const { format(...); }
    virtual void format(std::ostream&, const LogRecord&) const {}
  };
  using ItemFactory = std::function<Item::ptr(const std::string& option)>;
  static bool AddFormat(std::string key, ItemFactory);                    // 带 option 的工厂
  static bool AddFormat(std::string key, std::function<Item::ptr()>);     // 简化工厂
  template <class ItemType> static bool AddFormat(std::string key);       // 直接用类型注册
  static bool RemoveFormat(const std::string& key);
  static bool HasFormat(const std::string& key);
  static std::vector<std::string> Formats();
};
```

**内置格式项键（实读 `src/logging/log.cpp` 的 `EnsureBuiltinFormats()`，`std::call_once` 注册）**：

| 键 | 含义 | 键 | 含义 |
|---|---|---|---|
| `m` | message | `t` | thread（**数字 id**） |
| `p` | level | `F` | fiber（**Fiber id**） |
| `N` | logger | `c` | thread_name |
| `e` / `E` | elapse（耗时） | `d` | 日期，可带 option：`%d{%Y-%m-%d}`（默认 `%Y-%m-%d %H:%M:%S`） |
| `f` | file | `%` | 字面 `%` |
| `l` | line | `T` | 制表符 |
| `n` | 换行 | `__time` | 时间（内部项） |

**命名别名**（同样已注册）：`logger`、`level`、`message`、`thread`、`fiber`、`elapse`、
`thread_name`、`file`、`line`、`function`。

**两套语法共用同一注册表**：
- `{name}` 花括号语法（`ResolveBraceKey` 把 `time` 映射到 `__time`）；
- `%X` / `%X{option}` Sylar 风格语法。

**★ 三个关键行为**：
1. **未知键不会报错**：`ParsePattern` 把无法解析的原样保留为文本，并把 `PatternFormatter::HasError()` 置 true
   —— 所以「格式没生效」时先查 `HasError()`，不要以为是被静默吞掉。
2. **`AddFormat` 重名返回 `false`**（key 为空/工厂为空/key 已存在）。单字符键位基本被内置占满，
   用户想注册短键很容易失败而**静默失效** —— 必须先 `RemoveFormat` 再注册。
3. **工厂在注册表锁外执行**（`FindFormatItem` 先取工厂副本、再调用），
   避免用户在工厂里再次注册格式项造成死锁。

### 1.4 `Logger` / `LoggerManager` / 宏

```cpp
class Logger {                       // 默认级别 Warn
  void SetLevel(Level) noexcept;  Level level() const noexcept;  bool ShouldLog(Level) const noexcept;
  void AddWorker(LogItemWorker::ptr);  void RemoveWorker(const LogItemWorker::ptr&);   // 按 shared_ptr 身份匹配
  void AddDefaultWorker(LogItemWorker::ptr);  void ConfigureDefaults(std::vector<LogItemWorker::ptr>);
  void ClearWorkers();  void SetParent(Logger::ptr);  void SetPropagate(bool) noexcept;
  void Flush() const;   void Log(LogRecord record) const;
};
class LoggerManager {
  static LoggerManager& Instance();
  Logger::ptr Get(const std::string& name);   Logger::ptr Root();
  void Configure(Level, bool stdout_enabled, const std::string& directory,
                 const std::string& file, const std::string& pattern = {});
  void ConfigureLogger(const std::string& name, Level, bool propagate, std::vector<LogItemWorker::ptr> = {});
  void Remove(const std::string& name);       // 根 Logger 不可删
  std::vector<Logger::ptr> List() const;  void Flush();
};
class LogLine {                      // 流式临时对象，析构时提交
  LogLine(Logger::ptr, Level, const char* file, int line, const char* function);
  ~LogLine() noexcept;               // 内部 ostringstream → LogRecord → Logger::Log
  std::ostream& stream() noexcept;
};
#define GO2CPP_LOG_NAME(name)   ::go2cpp::log::LoggerManager::Instance().Get(name)
#define GO2CPP_LOG_LEVEL(logger, level)  ...LogLine(...).stream()
#define GO2CPP_LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL/FATAL(logger)
```

- `Logger::m_worker_snapshot` 是原子快照：日志线程**无锁**读取 worker 列表。
- `Logger::SetLevel` 会**同步改写所有默认 worker 的最低级别过滤器**（所以改级别对已存在的默认 worker 立刻生效）。
- 默认配置：`level = Warn`、`directory = log`、`file = go2cpp.log`、
  `pattern = "{time} [{level}] {logger} ({file}:{line}) {message}\n"`。

### 1.5 坑点

1. **完全同步、没有 worker 线程/队列**：`FileSink::Write` 持 mutex 写 `ofstream`，
   仅 `level >= Warn` 时每条 flush —— 高频日志会在调用线程上付出落盘成本。
2. **默认 `Warn`**：`Info`/`Debug` 默认**丢弃**（示例里大量用 `std::clog` 而非日志宏，部分原因在此）。
3. `RotatingFileSink` 构造时把已有文件大小计入 `m_bytes`；超过 `max_bytes` 时 rename 成 `.1..N`
   再重建流，**没有 fsync**。
4. `CallbackSink` 的回调构造后不可变，`Write` **不加内部锁** —— 回调若访问共享状态需自行同步。
5. `LogLine` 的析构是 `noexcept` 且内部 try/catch（用户 Sink 抛异常不会 terminate）。
6. 日志模块**不依赖任何 Go2Cpp 运行时**，可在纯线程程序里单独使用。

### 1.6 与 Go 的差异

| Go | 本项目 | 差异 |
|---|---|---|
| `log/slog`（Handler + Attr + Level） | Filter/Formatter/Sink/Worker 四层 | 无结构化 Attr 体系；pattern 驱动文本 |
| `slog` 的 `With`/`WithGroup` | `Logger::SetParent` + `SetPropagate` | 父链传播而非字段继承 |
| `log.Logger` 输出到 `io.Writer` | Sink 接口（文件/标准流/内存/回调） | 多出轮转与格式化项插件 |
| 无内置轮转（需 lumberjack 等） | `RotatingFileSink` | 简易滚动，无压缩/无 fsync |

---

## 2. `config`

### 2.1 INI 与类型转换

```cpp
class IniFile final {              // 只处理 section / key=value / 注释
  bool Load(const std::string& path, std::string* error = nullptr);
  bool Parse(const std::string& text, std::string* error = nullptr);
  std::string Get(section, key, fallback = {}) const;   bool Has(section, key) const;
};
template <typename T> struct ValueCodec {              // 通用：流式 >>，且要求消费完整
  static std::string Encode(const T&);   static bool Decode(const std::string&, T*);
};
// 特化：std::string / bool（1/true/yes/on 与 0/false/no/off，大小写不敏感）
//       log::Level（走 ParseLevel）/ std::vector<T>（逗号分隔）
```

注意通用 `ValueCodec<T>::Decode` 在 `>>` 之后还会 `>> std::ws` 并要求 `eof()`，
即 **`"12abc"` 这类带尾随垃圾的输入会被判为非法**。

### 2.2 `ConfigVar<T>`：强类型 + 无锁读 + 事务四阶段

```cpp
class ConfigVarBase {              // 非模板接口，供 Config 统一枚举/校验/写入
  virtual std::string ToString() const;
  virtual bool ValidateString(const std::string&, std::string* error = nullptr) const;
  virtual bool FromString(const std::string&, std::string* error = nullptr);
  virtual bool ApplyString(const std::string&, std::string* error = nullptr);   // 事务第一阶段
  virtual bool HasPending() const noexcept;
  virtual void NotifyPending() noexcept;                                        // 发布通知
  virtual void RollbackPending() noexcept;                                      // 回滚未发布的值
};
template <typename T> class ConfigVar final : public ConfigVarBase {
  T GetValue() const;                                   // atomic_load(shared_ptr<const T>)：无锁读
  bool SetValue(const T&, std::string* error = nullptr);
  void SetValidator(Validator);        void SetDirectValidator(Validator);
  std::uint64_t AddListener(Listener); void DelListener(std::uint64_t id);
  // Listener = function<void(const T& old_value, const T& new_value)>
};
```

关键设计（逐行读证）：

- **读路径无锁**：`m_value` 是 `shared_ptr<const T>`，用 `std::atomic_load/store_explicit` 发布/读取；
  写路径用 `m_mutex` 串行化监听器表与 `m_pending`。
- `SetValue` 顺序：取校验器（锁内）→ **锁外**跑校验器 → 锁内比较值是否相同（相同直接返回 true）
  → `atomic_store` 新值 → 拷贝监听器表 → **锁外**按注册顺序逐个调用（每个独立 try/catch）。
- `ApplyString`（事务第一阶段）：**先 `make_shared<const T>(parsed)`**（把可能抛 `bad_alloc` 的动作放在写入前），
  再在锁内写值并置 `PendingChange{old_value(旧快照指针), new_value}`。
  保存**旧快照指针**而不是旧值副本，是为了让回滚在 `bad_alloc` 下也能保持 `noexcept` 确定性。
- **`m_pending` 存在时拒绝新写入**（报「配置变量正在等待上一事务通知」）：
  一次事务里同一变量只发布最终值，避免中间值触发模块重配置。
- `NotifyPending()`：先把 `m_pending` 与监听器表**搬出**锁，再锁外调用；
  ★ 但它的 `catch (...)` 分支里**再次构造 `std::lock_guard`**（台账 X-25 记录的
  「noexcept 里加锁」危险形态之一），与 `HasPending()` 同属该族。

### 2.3 `Config`：两阶段事务 + 编辑锁 + 热重载

```cpp
class Config final {               // private 构造/析构：进程级单例
  static Config& Instance();
  template <typename T> std::shared_ptr<ConfigVar<T>> Lookup(name, default_value, description = {});  // 类型冲突返回空
  ConfigVarBase::ptr LookupBase(const std::string& name) const;
  bool LoadFromIni(const IniFile&, std::string* error = nullptr);
  bool LoadFromFile(const std::string& path, std::string* error = nullptr);
  std::vector<ConfigVarBase::ptr> List() const;
  std::uint64_t AddCommitListener(CommitListener);      void DelCommitListener(std::uint64_t);
  std::uint64_t AddTransactionValidator(TransactionValidator);  void DelTransactionValidator(std::uint64_t);
  bool StartWatcher(const std::string& path, std::chrono::milliseconds interval = 1000ms, std::string* error = nullptr);
  void StopWatcher();   bool Watching() const noexcept;
};
using CommitListener = std::function<void(const std::vector<ConfigVarBase::ptr>& changed)>;
using TransactionValidator = std::function<bool(const std::map<std::string,std::string>& candidates, std::string* error)>;
```

**事务流程（`LoadFromIni`，`m_transaction_mutex` 串行化）**：

```text
① 预检：对本次 INI 中出现的每个键做 ValidateString；
        再用 TransactionValidator 检查**完整候选快照**（未出现的键用当前值）—— 用于跨字段约束
② 提交第一阶段：对每个变量 ApplyString（写值 + 置 pending；失败即中止）
③ 发布：全部成功后统一 NotifyPending（回调在**事务锁外**执行）
④ 失败：对已 ApplyString 的变量 RollbackPending 恢复旧值（不调监听器）
```

**热重载（`StartWatcher`）**：

1. 先检查 `path + ".lock"` —— **锁文件非空则拒绝启动**（编辑锁是**约定**，不是强制）；
2. 读 `FileStamp{modified, size}` 并做**首次** `LoadFromFile`；
3. 起后台线程，`m_watcher_cv.wait_for(interval)` 轮询；
4. 判定变化用 `FileStamp` 比较 + `retry_pending`（保证「锁由非空变空」即使 stamp/size 不变也会强制重试一次）；
5. `ReadStableFileContents`：**stamp + 全文双快照 + 锁三次检查（8 次 × 2 ms）** → 半文件永不提交；
6. 失败只 `std::cerr`（不退出、不动已注册变量）。
7. `StopWatcher` 支持**在监听线程自身调用**（自我 `detach()`，台账 X-20：这是项目第四种
   「在自身线程内停止服务」策略，与 timer 的 `terminate`、io/context 的跳过 join 不同）。

### 2.4 绑定与运行时快照

```cpp
bool BindLoggingConfig(Config&, std::string* error = nullptr);         // log.level/stdout/directory/file/format
bool BindRuntimeConfig(Config&, RuntimeConfig* target, std::string* error = nullptr);  // scheduler.* + log.*
bool LoadAndWatch(Config&, path, interval = 1000ms, std::string* error = nullptr);     // 只绑日志

struct RuntimeConfig {
  scheduler::SchedulerConfig scheduler{};
  log::Level log_level{log::Level::Warn};  bool log_stdout{false};
  std::string log_directory{"log"}, log_file{"go2cpp.log"},
              log_format{"{time} [{level}] {logger} ({file}:{line}) {message}\n"};
  std::size_t timer_warn_after_days{30}, timer_error_after_days{365};
  static bool FromIni(const IniFile&, RuntimeConfig*, std::string* error = nullptr);
  bool Validate(std::string* error = nullptr) const;
  bool ApplyLogging(std::string* error = nullptr) const;
};
bool LoadRuntimeConfig(const std::string& path, RuntimeConfig*, std::string* error = nullptr);
```

★ **`LoadAndWatch` 只绑日志**：`scheduler.*` 是**启动期结构参数**（`processor_count`/`min_workers`/
`max_workers`/`local_queue_limit`/`fiber_stack_size`），必须先 `BindRuntimeConfig` 再创建 Scheduler，
不能靠热重载逐个改。

### 2.5 坑点

1. **`ReadSize`/`ParseSize` 的边界**：用 `std::from_chars` —— **不支持 `1K`/`1M` 单位后缀**、不支持负号、
   超 `size_t` 即失败；且**只有 `section == "scheduler"` 才走 `ValidateSchedulerSize` 范围校验**，
   其他段只要解析成功就通过（源码注释保留了历史缺陷：非 scheduler 段曾掉到 `return false` 且不设 error，
   导致 `[timer]` 键静默不可读；commit `b215579` 已修）。
2. `ConfigVar<std::size_t>` 走 `istringstream >>`，非法输入 → `FromString` 返回 false
   → **整份 INI 预检失败、整批回滚**（这是"全有或全无"的语义，不是逐键跳过）。
3. 台账 X-19：`RuntimeConfig::Validate` 里曾有一个**恒不成立**的 `min_workers != 0 && min_workers < 1`
   （无符号类型），已删除。
4. 台账 X-25：`ConfigVar<T>::HasPending()` 与 `NotifyPending()` 的 catch 分支都属于
   「`noexcept` 却加锁」族（共 13 处的其中 2 处）。
5. 配置独立性规范（`docs/configuration.md`）：**每个键必须能独立解析与校验**，
   不能假设同文件其他键会同时写完；跨键约束只能放 `TransactionValidator`。
6. 编辑锁是**纯约定**：watcher 只拒绝读取，无法阻止别人写入。
7. `Config` 是**不可析构的单例**（`Config()`/`~Config()` 均私有，`Instance()` 是函数局部 static），
   所以 X-20 描述的 UAF 路径当前不可达。

### 2.6 与 Go 的差异

| Go | 本项目 | 差异 |
|---|---|---|
| `flag` / `os.Getenv` / `viper` | `Config::Lookup` + `ConfigVar<T>` + `ConfigVar::AddListener` | 类型化 + 监听器 + 两阶段事务 |
| `viper.WatchConfig` | `StartWatcher` + `path.lock` 编辑锁 | 用锁文件与双快照避免半文件；失败只写 stderr |
| `encoding/json`/`yaml` 配置源 | **只有 INI** | 有意避免重量依赖（`config.hpp` 头注释） |
| 无内置事务 | `ApplyString`/`NotifyPending`/`RollbackPending` + `TransactionValidator` | 「全有或全无」的配置批次 |

---

## 3. 对架构图的贡献

**节点**

- `Log`（`Level`/`LogRecord` → `Filter` × `Formatter`(`Item` 注册表) × `Sink` → `LogItemWorker` → `Logger` → `LoggerManager`）
- `Config`（`IniFile` + `ValueCodec<T>` + `ConfigVar<T>`(pending 事务) + `Config`(编辑锁/监听线程)）
- `RuntimeConfig`（scheduler + log + timer 告警阈值的统一快照）
- `GO2CPP_LOG_*` 宏（`LogLine` 析构提交）

**连线**

- `全模块 → Log`：「`GO2CPP_LOG_NAME/LEVEL` 宏；`Logger` 默认级别 Warn」
- `Config → ConfigVar`：「预检 → ApplyString(pending) → NotifyPending / RollbackPending」
- `Config → 文件系统`：「`path.lock` 编辑锁 + FileStamp/双快照稳定性检查」
- `Config → Scheduler/Log`：「BindRuntimeConfig / BindLoggingConfig（启动期结构参数不可热改）」
- `Config → RuntimeConfig`：「LoadRuntimeConfig 一次性构造 + Validate」

**图内文字**

> 横切层：日志是**同步**的四层组合（Filter → Formatter → Sink → Worker），
> 格式项注册表以单字符键为主（`m/p/N/e/E/f/l/t/F/c/d` + 命名别名），
> 未知键**原样保留**并把 `HasError()` 置真，重名 `AddFormat` 返回 false（易静默失效）。
> 配置是「INI + 强类型变量 + 两阶段事务 + 锁文件热重载」：
> 先整体预检（含跨字段 `TransactionValidator`），再逐变量置 pending，最后统一通知；
> 失败则整批回滚。读取无锁（原子 shared_ptr），监听器一律在锁外执行。
> `scheduler.*` 是启动期结构参数，**不能热改**。
