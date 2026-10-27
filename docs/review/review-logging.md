# logging 模块审查报告

- 审查对象：`src/logging/log.cpp`（1057 行）、`include/go2cpp/logging/log.hpp`（855 行）
- 基线：`a2fa0df`；本模块文件 md5 与审查快照一致（同期 Bento 工作区有并发改动，不影响本模块行号）
- 审查方式：只读静态审查，未运行编译/测试
- 统计：**P0×1 / P1×6 / P2×5，共 12 条**

## 1. 问题清单

| ID | 级别 | 位置 | 问题 | 证据 | 建议修复 |
|---|---|---|---|---|---|
| LOG-01 | P0 | `log.cpp:611-612,581-583,895-898` | 日志重入无任何防护：自定义 Sink/CallbackSink 里再打日志会沿同一 worker 无限递归直至栈溢出 | `LogItemWorker::Submit` 锁外 `snapshot->sink->Write(record, formatted)`（611-612）→ `CallbackSink::Write` 的 `m_callback(record, formatted)`（582）→ `GO2CPP_LOG_*` → `LogLine::~LogLine`（897）`m_logger->Log` → 同一 Submit，无深度/重入标志 | 加 thread_local 重入深度（>0 丢弃并可写 stderr/fallback）；文档禁止在 Sink/回调内打日志；补重入测试 |
| LOG-02 | P1 | `log.cpp:461-472` | 默认 FileSink 写失败后进程内永久静默丢日志：badbit 置位后 466 行直接 return，无计数、无告警、无自动 Reopen；LoggerManager 构造期打开失败（495）同样无声 | `if (!stream || !stream->good()) return;`（466）；`stream->write(...)`（467）后不检查 good()；`Reopen()` 失败仅 return false，全快照无调用点 | 写失败计数 + 首次失败 stderr/回调告警 + 退避 Reopen；启动时校验日志目录可写并上报 |
| LOG-03 | P1 | `log.cpp:808-811,845-850`；`log.hpp:478-498` | 每个 Logger（及每次 Configure）各自新建 FileSink 打开同一路径，`m_mutex` 是实例成员，跨实例无互斥：多线程经不同 Logger 写同一文件顺序交错；Logger 数无上限，fd 线性增长（Windows `app` 模式 seek+write 非原子，覆盖风险需人工确认平台） | `defaults.push_back(make_shared<LogItemWorker>(..., make_shared<FileSink>((path(m_directory)/m_file).string())))` 每 logger 一份；Configure 的 make_defaults 同样每 logger 一份 | 进程级按绝对路径共享 Sink（弱引用池）或单写线程；限制 Logger 数量；文档写明跨 Logger 不保证顺序 |
| LOG-04 | P1 | `log.cpp:529-554` | RotatingFileSink 覆写 Write 后绕过 FileSink 语义：不再对 Warn+ 立即 flush、不检查 good()、写失败仍累加 m_bytes；滚动路径 537 行 `std::filesystem::exists(source)` 无 error_code 可抛，此时 533 行已 close 且 545 行 reset 未执行，m_stream 变成已关闭流 | 对比 `FileSink::Write` 471 行 `if (record.level >= Warn) stream->flush();`，Rotating 的 550-553 只有 write + `m_bytes +=`；`if (std::filesystem::exists(source))` 未用 error_code | 滚动后按 record.level flush；exists 改 error_code；滚动失败保留可用流/重置 m_bytes 并上报；明确只按大小切分、不支持按日期 |
| LOG-05 | P1 | `log.cpp:471,976-977,984-987`；`log.hpp:31-34` | 头文件声称「Sink 在锁内只做 write/append，刷盘放在锁外」，实现却在锁内 flush：FileSink 持 m_mutex 对 Warn+ 调 stream->flush()，BufferedSink 持 m_io_mutex 调 Write+Flush；磁盘慢时把 IO 等待串进所有写日志线程 | `std::lock_guard<std::mutex> lock(m_mutex); ... if (record.level >= Warn) stream->flush();`；`std::lock_guard io_lock(m_io_mutex); m_inner->Write(...); m_inner->Flush();` | 锁内只置 need_flush 标志，出锁后由独立 flush 路径处理；或修正文档承认锁内 flush 并给出延迟预期 |
| LOG-06 | P1 | `log.hpp:404-462`；`log.cpp:781-785,871` | BufferedSink 无析构、LoggerManager 无析构 Flush：正常退出时 m_buffer 中未达阈值的日志随对象销毁；LoggerManager::Flush() 全快照无生产调用点；FlushBatch 已把 batch swap 出，若 m_inner->Write 抛异常则该批丢失 | 类定义无 `~BufferedSink()`；LoggerManager 无 `~LoggerManager()` 调 Flush；FlushBatch 无 try/catch | BufferedSink 析构 try { Flush(); } catch(...) {}；LoggerManager 析构/atexit 刷全部 Logger；文档说明 Flush 是正常退出必要条件 |
| LOG-07 | P1 | `log.cpp:781,124-137,736-738`；`log.hpp:846` | 静态初始化/析构顺序风险：LoggerManager、FormatRegistry 及其 mutex、SetParent 的父图锁都是函数内 static；其它 TU 静态对象在构造期打日志会触发 LoggerManager 构造并在默认 log/ 建文件，析构期打日志则 Instance() 返回已析构对象、FormatRegistry() 返回已析构 map → UB（需人工确认是否存在静态期日志） | `LoggerManager& Instance() { static LoggerManager manager; return manager; }`；`static std::unordered_map<...> registry;`；`GO2CPP_LOG_NAME` 每次 Instance().Get(name) | 改不析构 heap 单例（`static auto* = new LoggerManager()`）或显式 Shutdown()+文档声明；避免静态期打日志 |
| LOG-08 | P2 | `log.cpp:746-751` | Logger::SetParent 在持有 current->m_mutex 时执行 `current = current->m_parent.lock()`：若旧 current 是中间节点最后一个强引用，赋值会析构该 Logger 与其 mutex，随后 lock_guard 解锁已析构 mutex → UB（需人工确认触发条件） | `std::lock_guard<std::mutex> lock(current->m_mutex); current = current->m_parent.lock();` | 锁内只取 next 到局部变量，出锁后再 current = std::move(next)；或断言父节点被外部强持有 |
| LOG-09 | P2 | `log.cpp:1020-1038` | BufferedSink::Flush() 与并发 Write() 的批次会乱序：锁内 swap 后释放 m_mutex，在取 m_io_mutex 前另一线程可先写入自己的批次；显式 Flush() 返回时不能保证「调用前已提交的日志先落盘」 | `{ lock; batch.swap(m_buffer); }` → `if (!batch.empty()) FlushBatch(...)`，两段之间无顺序屏障 | 单写队列/批次序号；或文档明确 Flush 只保证最终写出、不保证跨线程顺序 |
| LOG-10 | P2 | `log.cpp:94-114,304-322` | 时间戳为本地时间且格式不含时区偏移；DateItem 用 char buffer[128]，用户 `%d{...}` 输出超过 127 字节时 strftime 返回 0，代码静默不输出时间字段（不是截断），localtime_r/localtime_s 返回值未检查；strftime 受进程 setlocale 全局状态影响，每条日志都做一次时区转换 | `if (std::strftime(buffer, sizeof(buffer), m_format.c_str(), &local_time) != 0) stream << buffer;`；`localtime_r(&seconds, &local_time)` 忽略返回值 | 检查返回值并输出占位符（`<time-overflow>`/`<invalid-time>`）；提供 UTC/固定偏移选项并把偏移写进格式；缓存时区转换 |
| LOG-11 | P2 | `log.cpp:918-929`；`log.hpp:425,584-596`；`log.cpp:436-442` | 内存无硬上限：kByTime 策略下 capacity_bytes 完全不参与判断（只看 time_due），缓冲最多可涨到整个 flush_interval 内的日志总量；bypass_threshold 默认 0 表示不绕过，单条超大记录直接 append；MemorySink 无容量上限；PatternFormatter::Format 对消息长度无限制 | `case FlushPolicy::kByTime: return time_due;` 未使用 m_buffer.size()；`m_config.bypass_threshold{0}`；`m_lines.emplace_back(formatted)` | kByTime 同时加 capacity_bytes 硬上限；给 bypass_threshold 合理默认值；MemorySink 加 max_bytes/max_lines；文档给出单条消息上限 |
| LOG-12 | P2 | `log.cpp:216-219,262-270`；`log.hpp:456,676,736,798,476` | 注释与实现/API 多处不一致：`{time}` 被硬编码映射到内部 key `"__time"`，用户 AddFormat("time", ...) 注册的项永远不生效且 HasFormat("time") 不符；未闭合 `{`（262-270）不置 *error，HasError() 漏报非法模式；BufferedSink/LogItemWorker/Logger 三处 m_mutex 注释都写成「保护 m_lines」；FileSink::Reopen 注释称失败后 m_stream==nullptr，但 is_open() 失败时直接返回未 reset | `if (token == "time") return "__time";`；`if (end != std::string::npos) { ... }` 无 else 置 error；`log.hpp:456 ///< 保护 m_lines；Write/Snapshot/Clear 使用`；log.hpp:476 vs log.cpp:494-495 | 把 time 注册为正式 key（删别名映射）；未闭合 `{` 置 error；修正注释；Reopen 两条失败路径统一语义 |

## 2. 重点问题说明

### LOG-01（P0）重入导致栈溢出
模块正确地避免「持锁调用用户代码」，但没有防递归：`LogItemWorker::Submit` 在锁外调用 `Sink::Write`，`CallbackSink` 再调用用户回调，用户在回调里用 `GO2CPP_LOG_*` 写日志就会再次进入同一个 worker。由于 `LogLine` 在析构时才提交，递归深度与日志语句嵌套一致，最终栈溢出。这是日志库最常见也最致命的集成问题，建议用 `thread_local` 深度计数，深度 >0 时直接丢弃或写 stderr。

### LOG-02/LOG-04（P1）写失败静默
FileSink 的 `Write` 只检查入口的 `good()`，写入后不检查 `badbit`；RotatingFileSink 在滚动时先 `close` 再 `std::filesystem::exists`（可抛异常），异常路径下流已关闭且 `m_bytes` 未重置。两者叠加的后果是：磁盘满、权限变化、文件被删除时日志会「看起来正常但实际丢失」，且没有计数/告警可用于运维发现。

### LOG-05（P1）锁内 flush 与文档矛盾
`log.hpp:31-34` 的注释明确说「刷盘放在锁外」，但 `FileSink::Write` 在持有 `m_mutex` 时对 Warn+ 调 `stream->flush()`；`BufferedSink` 在 `m_io_mutex` 内同时 Write+Flush。Warn/Error 是故障时最常写的级别，磁盘抖动会把所有写日志线程串行阻塞，正好放大故障。

## 3. 亮点

- 热路径用原子 `shared_ptr` 快照切换 worker/sink，避免每条日志都加注册表锁；`LogItemWorker` 内部不在锁内调用用户代码，方向正确。
- 格式化缓冲边界基本安全：`FormatTime` 的 `date[64]`/`millis[5]` 满足最大输出，`snprintf` 参数强转到 `unsigned long long`，无 printf 可变参数。
- 级别过滤、Formatter/FormatRegistry、Sink 分层清晰，`WriteRaw` 保留默认实现维持 API 兼容。
- `log.hpp` 对锁职责、Flush 语义、Sink 接口有较完整的注释（虽有个别漂移）。

## 4. 维度结论

| 维度 | 结论 |
|---|---|
| 正确性 | 缓冲边界安全；`strftime` 失败静默、时间戳无时区、`{time}` 硬编码、未闭合 `{` 不置错；仅按大小滚动，无按日期 |
| 并发 | 原子快照正确；重入无防护（P0）；锁内 flush 与文档矛盾；多 Logger 多 FileSink 无跨实例互斥；静态单例/注册表存在初始化/析构顺序风险；SetParent 锁内释放 last ref 有 UB 窗口 |
| 资源与错误 | 写失败静默且 Sink 永久失效；Rotating 可抛异常中断滚动；每 Logger 一个 fd 且 Get 无上限；BufferedSink/MemorySink/pattern 无内存硬上限 |
| 风格与可维护性 | API 稳定、注释总体质量高；`Configure` 空 pattern 会重置自定义 pattern、`ConfigureLogger` 无法清空 worker、默认 pattern 两处重复、多处以 m_lines 复制粘贴注释 |

## 5. 未覆盖与限制

- 未运行编译/测试；Windows `app` 模式 seek+write 的原子性、静态期是否存在日志调用、`SetParent` 的 last-ref 触发条件均标注「需人工确认」。
- 低信号项（`SetLevel/ParseLevel` 的 noexcept 内可能抛、`ShouldLog` 内存序不一致、`Remove` 后旧 Logger 继续写同文件、`Get` 持锁建目录）未纳入 12 条，供后续清理参考。
