# Go2Cpp 理解力考试 · 缓冲 IO 卷（17 bufio）

> **考的是「为什么 / 坏在哪 / 边界在哪」**，不是背 API 名。
> **本卷数字均需自测，文档数字仅作历史对照。** 本项目迭代极快，`docs/` 里的吞吐数字随时可能过时；
> 需要数字时自己跑 `./go2cpp_bufio_test` / `./go2cpp_bufio_stress` / `./go2cpp_bufio_batch_bench`，
> 并写明缓冲容量、分块大小、是否任务集绑定。
> 追查位置只指向 `include/go2cpp/bufio/**`、`src/bufio/**`、`tests/bufio/**`。
>
> 覆盖：`Reader` 的零拷贝 Span 生命周期、`Writer` 的 flush 时机与粘性错误、`Scanner` 的 atEOF 契约、
> `Batch` 的批量语义，以及 limits.hpp 的边界；与 Go bufio 的差异都会点名。
>
> 难度分布：L3 = 1、L4 = 4。

---

### Q01 【L4 陷阱】`Peek` 返回的 Span 什么时候失效？为什么 `Peek(n > Size())` 不先预读？

- **痛点**：把 `Peek` 的返回值当 `std::string` 用、跨一次 `Read` 持有。Span 指向 `Reader` 的内部缓冲；下一次读操作会推进 `m_r`，`fill()` 还会 `memmove` 压缩前缀，指针立刻指向被覆盖/移动的数据。另一个坑是照搬 Go：Go 的 `Peek` 会先把缓冲填满再报 `ErrBufferFull` 并返回已有数据，本实现直接返回**纯错误**、不预读。
- **追查位置**：
  - `include/go2cpp/bufio/reader.hpp:559`（`@warning`：返回的 span 在下一次 `Read`/`Peek`/`ReadLine`/`Discard` 后失效）
  - `include/go2cpp/bufio/reader.hpp:568`（`Peek` 实现）
  - `include/go2cpp/bufio/reader.hpp:572`（`n > Size()` → 直接 `ErrBufferFull()`，**不预读**）
  - `include/go2cpp/bufio/reader.hpp:576`（只有 `n <= Size()` 才反复 `fill()` 直到够 n 或出现粘性错误）
  - `include/go2cpp/bufio/reader.hpp:585`（数据不足且已有错误时返回「已有部分 + 错误」）
  - `include/go2cpp/bufio/reader.hpp:658`（`fill()` 的 `memmove` 压缩：已消费前缀被搬走，旧 Span 失效）
  - `include/go2cpp/bufio/limits.hpp:45`（默认缓冲 4096；`Peek` 的上限就是它）
- **验收标准**（答到这些算过）：
  1. `Peek(n)` 返回的 `ByteSpan` 是**借用**：不拷贝、不拥有，生命周期到下一次会动缓冲的调用为止；要保留必须 `std::string(span.data(), span.size())`。
  2. `n > Size()` 直接 `ErrBufferFull`，不调用底层、不填缓冲；这是与 Go 的有意差异（Go 会先填满、返回部分数据 + 错误）。
  3. `n <= Size()` 时循环 `fill()`：够 n 就返回 `[m_r, m_r+n)`；底层 EOF/错误时返回「已有部分 + 错误」或纯错误，调用方必须同时看 `Result` 的值与错误。
  4. `fill()` 的 `memmove` 会把 `[m_r, m_w)` 搬到缓冲头并把 `m_r` 归零——这正是旧 Span 失效的根因，不是「Peek 自己会改缓冲」。
  5. `Peek` 不消耗数据（`m_r` 不动），`Discard/Buffered/Read` 才推进。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_bufio_test` → `场景2：HTTP 响应（Peek 零拷贝解析头 + Content-Length 精确读 body）` 通过，并打印 `头长/Content-Length/读 body 后缓冲/底层调用`。它证明 Peek 不消耗数据；要观察失效，可在 Peek 后先 `ReadByte` 再打印旧 span 的内容，会看到字节已被覆盖/移动（ASAN 也能抓到越界读）。
- **常见错误答案**：「Span 是 string_view，只要不 Reset 就一直有效」「Peek 超过缓冲会扩容」「Peek 会顺便把数据读进缓冲，所以一定拿得到 n 字节」——零分。

### Q02 【L4 陷阱】`Writer` 析构为什么不自动 `Flush`？错误发生后再 `Reset` 会丢什么？

- **痛点**：按 RAII 直觉以为析构会兜底 flush。本实现的析构是 `= default`，**不碰底层**：忘记 `Flush()` 等于丢数据。另一个坑是错误粘性：底层失败后 `m_err` 非空，后续 `Write/Flush` 直接返回同一个错误，而缓冲里可能还压着没写出的字节；`Reset()` 会把它们丢掉并清错。
- **追查位置**：
  - `include/go2cpp/bufio/writer.hpp:36`（「析构**不**自动 flush（重要）」：3 条理由——错误无法交还、半条记录、显式可审计）
  - `include/go2cpp/bufio/writer.hpp:46`（「忘记 `Flush()` 等于丢数据」是有意保留的 Go 同款坑）
  - `include/go2cpp/bufio/writer.hpp:185`（`~Writer() = default`：只做默认清理）
  - `include/go2cpp/bufio/writer.hpp:152`（不变量：`m_err` 非空后所有写操作直接失败，不再调用底层）
  - `include/go2cpp/bufio/writer.hpp:153`（`m_err` 非空时缓冲里可能仍有未写出的数据，`Reset()` 会丢弃）
  - `include/go2cpp/bufio/writer.hpp:484`（`Reset(WriterT&)`：换底层、清缓冲）
  - `include/go2cpp/bufio/writer.hpp:486`（`m_err = {}`：错误也被清掉）
  - `include/go2cpp/bufio/writer.hpp:576`（短写且底层违约时置 `IoErrShortWrite`，错误粘性从这里开始）
- **验收标准**：
  1. `Writer` 析构不 flush；数据只在 `Flush()`/`FlushBatch()`/缓冲满自动 flush 时离开进程。
  2. `Write` 返回的字节数只表示「本对象已接受」，不代表已到达底层；`Buffered()` 才是还没写出的量。
  3. 底层失败后 `m_err` 粘住：后续 `Write` 返回 `WriteResult{0, m_err}`、`Flush` 返回该错误，不再尝试底层。
  4. `Reset()` 丢弃未 flush 的缓冲并清 `m_err`；如果失败时缓冲里还有数据，这些数据**永久丢失**，调用方必须先处理错误。
  5. 与 Go 的差异：Go 的 `bufio.Writer` 遇到短写直接 `ErrShortWrite`，本实现会循环写完；但「析构不 flush」与 Go 一致（Go 没有析构，必须显式 Flush）。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_bufio_test` → `场景3：写日志（Writer 批量写 1 万行 -> Flush -> 回读校验）` 打印 `未 flush 文件=0 字节`——这正是「不 Flush 就析构，数据没落地」的直接证据。再跑 `./go2cpp_bufio_stress` 看短写/错误的压力路径。
- **常见错误答案**：「析构会自动 flush，RAII 就该这样」「`Write` 返回成功就代表对端收到了」「`Reset` 只是换底层，缓冲会保留」——零分。

### Q03 【L4 陷阱】底层 `Read` 返回「数据 + 错误」时，`fill()` 为什么先把数据交付、错误留到后面？

- **痛点**：把 `ReadResult{count>0, error!=null}` 当成「有错就丢弃数据」。Go 的 `io.Reader` 契约允许 `n>0` 同时 `err!=nil`（典型是读到最后一个包同时 EOF）；丢掉数据会静默截断。本实现把错误记进 `m_err`，先把数据放进缓冲，等缓冲耗尽后再交付错误。
- **追查位置**：
  - `include/go2cpp/bufio/reader.hpp:649`（`fill()`：先查粘性错误、再压缩、再读）
  - `include/go2cpp/bufio/reader.hpp:651`（`if (m_err) return;`：有错误后不再碰底层）
  - `include/go2cpp/bufio/reader.hpp:672`（`m_w += result.count`：数据先入缓冲）
  - `include/go2cpp/bufio/reader.hpp:675`（注释：数据先留缓冲，错误记住，等缓冲耗尽再交付——Go 的 defer 语义）
  - `include/go2cpp/bufio/reader.hpp:681`（`NoteIdleRead(result.error)`：0 字节读才立刻处理错误/空读计数）
  - `include/go2cpp/bufio/reader.hpp:240`（`Read` 快路径：目标足够大时直接读进调用方缓冲，`count>0` 立刻返回，同时把 `error` 记进 `m_err`）
  - `include/go2cpp/bufio/limits.hpp:51`（`kMaxConsecutiveEmptyReads = 100`：连续 0 字节且无错误 → `IoErrNoProgress`）
- **验收标准**：
  1. `count > 0 && error != null` 时，先交付这 `count` 字节；错误进入 `m_err`，等下一次缓冲为空时由 `Err()` 暴露。
  2. `m_err` 是粘性的：一旦非空，`fill()` 直接返回，不再调用底层；所有读方法最终以该错误收尾（包括 EOF）。
  3. 快路径（`p.size() >= Size()` 且缓冲为空）也遵守同一规则：数据立即返回，错误同时记住——不会因为「直接读进调用方」而丢掉错误。
  4. 连续 100 次「0 字节且无错误」→ `IoErrNoProgress`，避免在违约的底层上死循环；这是与 Go `bufio` 的差异（Go 会一直转）。
  5. `Read` 返回 0 不等于 EOF：必须看 `Err()`；`Read` 最多触发一次底层读，要读满得自己循环。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_bufio_test` → `随机：Read（随机长度/随机请求尺寸/验证原始字节）` 与 `随机：ReadString（随机分隔符/随机分块）` 通过，说明「数据+错误」没有丢字节；`./go2cpp_bufio_stress` 用随机分块/短读压这条路径。要精确观察 EOF 同包，可写一个桩：先返回 `{5, IoErrUnexpectedEOF}`，再 `Peek/Read` 应拿到 5 字节后才见错误。
- **常见错误答案**：「有错误就返回 0，数据丢掉」「读到 EOF 才算读完，`Read` 返回 0 就是 EOF」「粘性错误只影响当前这一次读」——零分。

### Q04 【L4 陷阱】`SplitFunc` 的 `advance==0 && token 为空` 在 `atEOF` 前后含义为什么不同？超长 token 走哪条路？

- **痛点**：把「0 进展」一律当成「继续读」。扫描到 EOF 时，`atEOF=true` 的 `0,empty` 表示「没有更多 token」，继续读会死循环。另一个坑是 `ErrFinalToken`：它是「最后一个 token」的哨兵，不是错误，`Err()` 必须为空；把它当错误会让正常结束变成失败。
- **追查位置**：
  - `include/go2cpp/bufio/scanner.hpp:189`（`Scan()`：五步主循环）
  - `include/go2cpp/bufio/scanner.hpp:199`（把 `(window, m_err != nullptr)` 交给 `SplitFunc`：错误态也要再切一次 atEOF）
  - `include/go2cpp/bufio/scanner.hpp:202`（`ErrFinalToken`：交付 token 后正常终止，`Err()` 为空）
  - `include/go2cpp/bufio/scanner.hpp:213`（`advance` 越界/未覆盖 token → `ErrInvalidAdvance`，不是 panic）
  - `include/go2cpp/bufio/scanner.hpp:280`（缓冲到上限仍切不出 token → `ErrTooLong` 或空闲读超限）
  - `include/go2cpp/bufio/scanner.hpp:365`（`ReadMore`：窗口占满时按翻倍扩容，夹到 `m_max_token_size`）
  - `include/go2cpp/bufio/scanner.hpp:409`（`m_max_token_size` 默认 `kMaxScanTokenSize`；构造/Buffer() 可改）
  - `include/go2cpp/bufio/limits.hpp:54`（`kMaxScanTokenSize = 64 KiB`，只约束 Scanner，不约束 Reader::ReadString）
- **验收标准**：
  1. `advance==0 && token 空` 且 `atEOF=false` → 数据不够，`ReadMore` 继续读；`atEOF=true` → 没有更多 token，`Scan()` 返回 false 且 `Err()` 为空。
  2. `ErrFinalToken` 是哨兵：交付它的 token 后终止，`Err()` 仍为空；普通错误才让 `Err()` 非空。
  3. 自定义 `SplitFunc` 的 `advance` 超出窗口、或 `token` 落在窗口外 → `ErrInvalidAdvance`，不能 panic（C++ 侧不能让用户回调把进程带走）。
  4. token 超过 `max_token_size` → `ErrTooLong`；`ReadMore` 只在这里失败，扩容是翻倍且夹到上限。
  5. `Bytes()` 返回的视图在下一次 `Scan()` 后失效；`Text()` 才是拷贝。`Scan()` 返回 false 后**必须**看 `Err()` 区分正常 EOF 与真实错误。
  6. 与 Go 的差异：`ScanWords` 只按 ASCII 空白切分（Go 用 `unicode.IsSpace` 走 UTF-8）；自定义 SplitFunc 越界不 panic 而是报错。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_bufio_test` → `scanner: Buffer(max) / ErrFinalToken` 段通过；`场景4：Scanner 扫 10 万行` 打印行数与底层调用数。要观察 `ErrTooLong`，把 `max_token_size` 调到小于单行长度再 `Scan`，`Err()` 应为 `ErrTooLong`。
- **常见错误答案**：「0 进展永远表示继续读」「`ErrFinalToken` 是错误，应该让 `Err()` 非空」「token 超长会自动扩容到内存够为止」——零分。

### Q05 【L3 陷阱】`FlushBatch` 和 `Flush` 到底差在哪？没有 `WriteBatch` 的底层会不会退化？

- **痛点**：把 `FlushBatch` 当成「更强的 Flush」。它只是把「当前所有段」一次交给底层：底层有 `WriteBatch(const BatchEntry*, size_t)` 时是 1 次 `writev/sendmmsg`，没有时因为段在缓冲里连续，退化成 1 次 `Write`——系统调用次数同样是 1，差的是「逐段边界」信息。另外定时策略是惰性的（`Write` 返回前检查），没有后台线程，写完就停必须显式收尾。
- **追查位置**：
  - `include/go2cpp/bufio/writer.hpp:24`（批量 flush 的定位：`SetBatchConfig` 选策略，`FlushBatch` 显式立即写）
  - `include/go2cpp/bufio/writer.hpp:31`（`Flush()` 仍是单次 `Write`，是「立刻落地」的最强保证）
  - `include/go2cpp/bufio/writer.hpp:33`（定时检查是惰性的，没有后台线程；写完即停必须显式 flush）
  - `include/go2cpp/bufio/writer.hpp:363`（`FlushBatch`：先查 `m_err`，再按 `HasWriteBatch` 分派）
  - `include/go2cpp/bufio/writer.hpp:368`（`if constexpr (HasWriteBatch<WriterT>::value)` → `FlushBatchImpl()`，否则 `FlushLinear()`）
  - `include/go2cpp/bufio/writer.hpp:599`（`FlushBatchImpl`：逐段调 `WriteBatch`，处理部分写与粘性错误）
  - `include/go2cpp/bufio/batch.hpp:53`（`FlushPolicy`：kOnFull/kInterval/kOnExplicit/kHybrid）
  - `include/go2cpp/bufio/batch.hpp:65`（`kDefaultMaxBatch = 64`）
  - `include/go2cpp/bufio/batch.hpp:68`（`kMaxBatchEntries = 1024`，与 IOV_MAX 对齐）
  - `include/go2cpp/bufio/batch.hpp:74`（`kDefaultFlushInterval = 100µs`，惰性检查）
- **验收标准**：
  1. `Flush()` 走 `FlushLinear()`：一次 `Write`，不依赖 `WriteBatch`；`FlushBatch()` 在有 `WriteBatch` 时逐段交出，无则退化为 `FlushLinear`（字节流不变）。
  2. 段 = 一次 `Write/WriteString`（连续 `WriteByte` 合并）；段只影响写入侧 iovec 数量，不改变字节顺序与内容。
  3. 硬上限对所有策略生效：缓冲放不下、段数到 `max_batch` 时必须 flush，否则 `Write` 无法继续接受。
  4. `kInterval`/`kHybrid` 的定时检查发生在 `Write/WriteByte` 返回前；没有后台线程，空闲时不会自动 flush，写完即停要显式 `Flush/FlushBatch`。
  5. `max_batch` 默认 64、上限 1024（避免一次 `writev` 超 IOV_MAX）；`threshold` 会被缓冲容量钳制。
  6. 空缓冲时 `FlushBatch` 直接成功，不触碰底层；错误仍保持粘性。
- **怎么自证**：`cd .build-bento-wsl && ./go2cpp_bufio_batch_test` → 5 个场景（日志行、RPC 响应、传感器窗口、管道 sink、共享 TCP 连接）全过；`./go2cpp_bufio_batch_bench` 打印不同策略下的系统调用次数/吞吐（具体数自己跑，别背）。要验证退化，用一个只有 `Write` 的桩底层跑同一测试，`FlushBatch` 仍然正确但不会出现 `WriteBatch` 调用。
- **常见错误答案**：「`FlushBatch` 一定比 `Flush` 少 syscall」「没有 `WriteBatch` 时 `FlushBatch` 会报错」「定时策略有后台线程，空闲也会自动 flush」——零分。

---

## 命题备注（给自己人看）

- Q01/Q02/Q03/Q04 都能用 `go2cpp_bufio_test` 的现成场景自证；Q05 用 `go2cpp_bufio_batch_test`。
- Q02 的「未 flush 文件=0 字节」是测试输出里的可观察量，不是性能数字；Q05 的 64/1024/100µs 是代码常量，不是实测调优结论。
