# 17 bufio 答卷（5 题）

> 环境：WSL2 / 12 vCPU。本轮 `go2cpp_bufio_test` `checks=252696 failures=0`、`go2cpp_bufio_batch_test` `checks=67481 failures=0`；`1 MiB 分块` 实测 1853.9 MiB/s（512B 请求、Read 2048 次、底层 1415 次）。

## Q01

**答**：`Peek(n)` 返回的 `ByteSpan` 是**借用**：不拷贝、不拥有，生命周期到下一次会动缓冲的调用（`Read`/`Peek`/`ReadLine`/`Discard`）为止；要保留必须 `std::string(span.data(), span.size())`。`n > Size()` 直接返回 `ErrBufferFull`，**不调用底层、不填缓冲**——这是与 Go 的有意差异（Go 会先填满、返回部分数据 + 错误）。`n <= Size()` 时循环 `fill()`：够 n 就返回 `[m_r, m_r+n)`；底层 EOF/错误时返回「已有部分 + 错误」或纯错误，调用方必须同时看 Result 的值与错误。旧 Span 失效的根因是 `fill()` 的 `memmove`：把 `[m_r, m_w)` 搬到缓冲头并把 `m_r` 归零，不是「Peek 自己改缓冲」。`Peek` 不消耗数据，`Discard/Buffered/Read` 才推进。

**代码证据**：
```text
include/go2cpp/bufio/reader.hpp:559-560  * @warning 返回的 span 在下一次 `Read`/`Peek`/`ReadLine`/`Discard` 之后失效。不要改写它指向的内容。
include/go2cpp/bufio/reader.hpp:570-572  // step1: 超出缓冲容量，直接报错而不是先把源读掉一截 if (n > m_buffer.size()) { return Result<ByteSpan>(ErrBufferFull()); }
include/go2cpp/bufio/reader.hpp:574-575  // step2: 补数据，直到够 n 字节或遇到粘性错误 while (m_w - m_r < n && !m_err) {
include/go2cpp/bufio/reader.hpp:657-660  if (buffered > 0) { std::memmove(m_buffer.data(), m_buffer.data() + m_r, buffered); } m_w = buffered; m_r = 0;
include/go2cpp/bufio/limits.hpp:45  inline constexpr std::size_t kDefaultBufferSize = 4096;
```

**自证**：`go2cpp_bufio_test` 的 `场景2：HTTP 响应（Peek 零拷贝解析头 + Content-Length 精确读 body）` 通过（证明 Peek 不消耗数据）；`checks=252696 failures=0`。

**我没答出的**：没有在 Peek 后先 ReadByte 再打印旧 span 观察覆盖（题面建议的失效探针）。

## Q02

**答**：`Writer` 析构是 `= default`，**不碰底层**：忘记 `Flush()` 等于丢数据。理由三条：错误无法交还、可能写出半条记录、显式 Flush 可审计；与 Go 一致（Go 没有析构，必须显式 Flush）。`Write` 返回的字节数只表示「本对象已接受」，不代表已到达底层；`Buffered()` 才是还没写出的量。底层失败后 `m_err` 粘住：后续 `Write` 返回 `{0, m_err}`、`Flush` 返回该错误，不再尝试底层。`Reset()` 丢弃未 flush 的缓冲并清 `m_err`；若失败时缓冲里还有数据，这些数据**永久丢失**，调用方必须先处理错误。与 Go 的差异：Go 遇到短写直接 `ErrShortWrite`，本实现会循环写完。

**代码证据**：
```text
include/go2cpp/bufio/writer.hpp:36-39  ## 析构**不**自动 flush（重要）与 Go 一致（Go 也没有「析构」概念...必须显式 Flush）。
include/go2cpp/bufio/writer.hpp:185  ~Writer() = default;
include/go2cpp/bufio/writer.hpp:152-153  * - m_err 非空后所有写操作直接失败，不再调用底层（粘性）；* - m_err 非空时缓冲里可能仍有未写出的数据，Reset() 会丢弃它们。
include/go2cpp/bufio/writer.hpp:484-486  m_last_op_byte = false; m_last_flush = steady_clock::now(); m_err = {};
include/go2cpp/bufio/writer.hpp:576  m_err = streams::IoErrShortWrite();
```

**自证**：`go2cpp_bufio_test` 的 `场景3：写日志（Writer 批量写 1 万行 -> Flush -> 回读校验）` 通过（打印「未 flush 文件=0 字节」）；`场景6 慢速/短写对端` 覆盖短写（总字节 66490、短写 162 次、平均 410.4 B）。

**我没答出的**：没有单独验证「失败后 Reset 丢弃未写字节」的字节数。

## Q03

**答**：`ReadResult{count>0, error!=null}` 是 Go `io.Reader` 的合法返回（典型是最后一个包同时 EOF）；丢掉数据会静默截断。本实现把错误记进 `m_err`，**先把数据放进缓冲**，等缓冲耗尽后再交付错误：`fill()` 先查粘性错误、再 memmove 压缩、再读；`m_w += result.count` 数据先入缓冲；`result.error` 记进 `m_err`；`NoteIdleRead` 只处理 0 字节读。快路径（目标足够大且缓冲空）也遵守同一规则：数据立即返回、错误同时记住。连续 100 次「0 字节且无错误」→ `IoErrNoProgress`（Go 会一直转，这是差异）。`Read` 返回 0 不等于 EOF，必须看 `Err()`；`Read` 最多触发一次底层读。

**代码证据**：
```text
include/go2cpp/bufio/reader.hpp:649-652  void fill() { // step1: 粘性错误后不再调用底层 if (m_err) { return;
include/go2cpp/bufio/reader.hpp:674-676  if (result.error) { m_err = result.error; } return;
include/go2cpp/bufio/reader.hpp:239-243  if (m_err) return 0; // step2: Go 的快路径——缓冲区空且目标足够大时直接读进目标
include/go2cpp/bufio/limits.hpp:51  inline constexpr std::size_t kMaxConsecutiveEmptyReads = 100;
```

**自证**：`go2cpp_bufio_test` 的随机 Read/ReadString 场景通过（`checks=252696 failures=0`），说明「数据+错误」没有丢字节；`go2cpp_bufio_stress` 未单跑。

**我没答出的**：没有写「先返回 {5, UnexpectedEOF} 再 Peek/Read」的桩验证 5 字节先交付。

## Q04

**答**：`advance==0 && token 为空` 且 `atEOF=false` → 数据不够，`ReadMore` 继续读；`atEOF=true` → 没有更多 token，`Scan()` 返回 false 且 `Err()` 为空。`ErrFinalToken` 是**哨兵**：交付它的 token 后正常终止，`Err()` 仍为空；普通错误才让 `Err()` 非空。自定义 `SplitFunc` 的 `advance` 超出窗口、或 token 落在窗口外 → `ErrInvalidAdvance`（不 panic）。token 超过 `max_token_size`（默认 64 KiB）→ `ErrTooLong`；`ReadMore` 在窗口占满时按翻倍扩容并夹到 `m_max_token_size`。`Bytes()` 视图在下一次 `Scan()` 后失效，`Text()` 才是拷贝；`Scan()` 返回 false 后**必须**看 `Err()`。与 Go 差异：`ScanWords` 只按 ASCII 空白切，自定义 SplitFunc 越界报错不 panic。

**代码证据**：
```text
include/go2cpp/bufio/scanner.hpp:199-205  ... if (result.error) { if (go2cpp::Is(result.error, ErrFinalToken())) { // SplitFunc 用 ErrFinalToken 声明「最后一个 token」...
include/go2cpp/bufio/scanner.hpp:212-214  ... SetErr(ErrInvalidAdvance()); Shutdown(); return false;
include/go2cpp/bufio/scanner.hpp:279-282  if (!ReadMore()) { // 缓冲已到上限仍切不出 token：ErrTooLong（或空闲读超限）。
include/go2cpp/bufio/scanner.hpp:365-367  if (m_buffer.size() >= m_max_token_size) { SetErr(ErrTooLong()); return false;
include/go2cpp/bufio/limits.hpp:53-54  /// Scanner 单个 token 的默认上限... inline constexpr std::size_t kMaxScanTokenSize = 64U * 1024U;
```

**自证**：`go2cpp_bufio_test` 的 `scanner: Buffer(max) / ErrFinalToken` 段与 `场景4 Scanner 扫 10 万行` 通过（`checks=252696 failures=0`）。

**我没答出的**：没有把 `max_token_size` 调到小于单行验证 `ErrTooLong`。

## Q05

**答**：`Flush()` 走 `FlushLinear()`：一次 `Write`，不依赖 `WriteBatch`；`FlushBatch()` 在底层有 `WriteBatch(const BatchEntry*, size_t)` 时逐段交出（1 次 `writev`/`sendmmsg`），没有时因为段在缓冲里连续，退化为 `FlushLinear`（1 次 `Write`）——系统调用次数同样是 1，差的是「逐段边界」信息。段 = 一次 `Write/WriteString`（连续 `WriteByte` 合并）；段只影响写入侧 iovec 数量，不改变字节顺序与内容。硬上限对所有策略生效：缓冲放不下、段数到 `max_batch` 必须 flush。`kInterval`/`kHybrid` 的定时检查发生在 `Write/WriteByte` 返回前，**没有后台线程**，空闲不会自动 flush。`max_batch` 默认 64、上限 1024（对齐 IOV_MAX）；`threshold` 会被缓冲容量钳制。空缓冲时 `FlushBatch` 直接成功，不触碰底层；错误仍粘性。

**代码证据**：
```text
include/go2cpp/bufio/writer.hpp:363-371  Result<void> FlushBatch() { if (m_err) return Result<void>(m_err); ... if constexpr (HasWriteBatch<WriterT>::value) { result = FlushBatchImpl(); } else { result = FlushLinear();
include/go2cpp/bufio/batch.hpp:64-68  kDefaultMaxBatch = 64; kMaxBatchEntries = 1024（对齐 Linux IOV_MAX）
include/go2cpp/bufio/batch.hpp:73-74  kDefaultFlushInterval{100}  // 100 µs，惰性检查
```

**自证**：`go2cpp_bufio_batch_test` `checks=67481 failures=0`（含「无批量接口的管道式 sink」「多线程共享 TCP 连接」两个退化/共享场景）。

**我没答出的**：没有跑 `go2cpp_bufio_batch_bench` 比较不同策略的 syscall/吞吐（题面也标注不注册 ctest）。
