# Bufio 模块 · 深入学习

> 面向「要接手改 `include/go2cpp/bufio/**` / `src/bufio/**` 的人」。
> 行号已复核；性能只给「本机怎么测」，不写文档里的数字。

---

## 第一部分 · 总：这个模块怎么跑起来

### 1.1 入口点清单

| 入口（类/函数） | 文件:行 | 谁调用它 | 触发什么 | 返回什么 |
|---|---|---|---|---|
| `Reader(source)` | `include/go2cpp/bufio/reader.hpp:206` | 业务/Scanner | 默认 4096 缓冲 | `Reader` |
| `Reader(source,size)` | `reader.hpp:213` | 业务 | 指定缓冲，`ClampBufferSize` 至少 16 | `Reader` |
| `Reader::Read(ByteSpan)` | `reader.hpp:232` | 上层 | 最多一次底层读；快路径直接读进目标 | `size_t`（0 需看 `Err`） |
| `Reader::ReadByte` | `reader.hpp:291` | 解析器 | 取 1 字节并允许 `UnreadByte` | `Result<uint8_t>` |
| `Reader::ReadRune` | `reader.hpp:311` | 文本解析 | UTF-8 解码，可 `UnreadRune` | `ReadRuneResult` |
| `Reader::UnreadByte/UnreadRune` | `reader.hpp:344/358` | 解析器 | 回退 1 字节/1 个 rune | `Result<void>` |
| `Reader::ReadLine` | `reader.hpp:390` | 逐行 | 去 `\n`/`\r`，超长 `ErrTooLong` | `Result<string>` |
| `Reader::ReadString(delim)` | `reader.hpp:461` | 逐记录 | 含分隔符、无界累积 | `Result<string>` |
| `Reader::ReadSlice(delim)` | `reader.hpp:506` | 零拷贝行 | 返回内部缓冲 span，满则 `ErrBufferFull` | `Result<ByteSpan>` |
| `Reader::Peek(n)` | `reader.hpp:568` | 协议头 | 零拷贝、不消耗；`n>Size()` 直接错 | `Result<ByteSpan>` |
| `Reader::Discard(n)` | `reader.hpp:592` | 跳过 | 消耗 n 字节 | `size_t` |
| `Reader::Buffered/Size/Err` | `reader.hpp:611/614/617` | 诊断 | 水位/容量/粘性错误 | `size_t`/`ErrorPtr` |
| `Writer(dest)` | `include/go2cpp/bufio/writer.hpp:159` | 业务 | 默认缓冲 | `Writer` |
| `Writer(dest,size)` | `writer.hpp:167` | 业务 | 指定缓冲 | `Writer` |
| `Writer::Write` | `writer.hpp:272` | 业务 | 缓冲/快路径直写/满自动 flush | `WriteResult` |
| `Writer::WriteByte/WriteString` | `writer.hpp:285/316` | 业务 | 同上（段合并） | `Result<void>` |
| `Writer::Flush` | `writer.hpp:338` | 业务 | 一次 `Write`（部分写循环） | `Result<void>` |
| `Writer::FlushBatch` | `writer.hpp:363` | 业务 | 有 `WriteBatch` 走批量，否则退化 | `Result<void>` |
| `Writer::SetBatchConfig` | `writer.hpp:389` | 业务 | 设置策略/段数/阈值/间隔 | `void` |
| `Writer::ReadFrom(src)` | `writer.hpp:429` | 拷贝 | 大块搬运 + 空读检测 | `Result<size_t>` |
| `Writer::Reset(dest)` | `writer.hpp:480` | 复用 | 换底层、丢缓冲、清错 | `void` |
| `Writer::Buffered/Available/Err` | `writer.hpp:490/493/499` | 诊断 | 未写字节/剩余空间/错误 | `size_t`/`ErrorPtr` |
| `Scanner(source)` | `include/go2cpp/bufio/scanner.hpp:155` | 业务 | 默认 4096 初始缓冲 | `Scanner` |
| `Scanner(source,size,max)` | `scanner.hpp:163` | 业务 | 初始/上限 | `Scanner` |
| `Scanner::Scan` | `scanner.hpp:189` | 业务 | 调 SplitFunc、扩容、终止 | `bool` |
| `Scanner::Bytes/Text` | `scanner.hpp:297/288` | 业务 | 零拷贝视图/拷贝串 | `ByteSpan`/`string` |
| `Scanner::Err` | `scanner.hpp:300` | 业务 | 区分 EOF 与错误 | `ErrorPtr` |
| `Scanner::Buffer/Split` | `scanner.hpp:309`（Buffer）/`Split` 在类内 | 业务 | 改上限/换切分函数 | `void` |
| `ByteSpan` | `include/go2cpp/bufio/byte_span.hpp:72` | 全模块 | 非拥有字节视图 | — |
| `ByteSpan::data/size/empty` | `byte_span.hpp:86/88/90` | 全模块 | 访问 | 指针/长度 |
| `ByteSpan::first/subspan` | `byte_span.hpp:100/113` | 解析 | 钳制/切片 | `ByteSpan` |
| `BatchConfig` | `include/go2cpp/bufio/batch.hpp:83` | `SetBatchConfig` | `policy/max_batch/threshold/interval` | 配置 |

### 1.2 总架构图

```
调用方（协议解析 / 日志 / 测试桩）
   │
   ├── Reader<ReaderT> ── 拥有 m_buffer(≤Size) ── 借用底层 ReaderT::Read
   │        │  游标: m_r(已交付) / m_w(已读入) / m_buffer
   │        ├── fill(): memmove 压缩 + 一次底层读；错误延后交付
   │        ├── Peek/ReadSlice: 零拷贝 span（下一次读失效）
   │        └── ReadLine/ReadString: 拷贝成 string（可无界）
   │
   ├── Writer<WriterT> ── 拥有 m_buffer + m_segments ── 借用底层 WriterT::Write / WriteBatch
   │        ├── 快路径: 大块且缓冲空 → 直写底层
   │        ├── 满: 自动 flush（FlushLinear / FlushBatchImpl）
   │        └── 错误粘性: m_err 非空后不再碰底层
   │
   ├── Scanner<ReaderT> ── 自己的窗口缓冲（可翻倍到 max_token）
   │        └── SplitFunc(data, at_eof) → {advance, token, error}
   │
   └── ByteSpan / BatchConfig 是值类型，不拥有底层
```

线程/边界：三个模板都**不是**线程安全的；`Writer` 只引用底层、不拥有；`Reader`/`Scanner` 也按引用保存 source，source 必须比它们活得久。锁：无（batch 的定时检查在写线程内，无后台线程）。

### 1.3 复杂场景详解

#### 场景 A：一次 `ReadLine()`（12 步）

1. 调用 `ReadLine()`（`reader.hpp:390`）。
2. 先 `InvalidateUnread()`：任何读操作让上一次 `UnreadByte/UnreadRune` 失效。
3. 在 `[m_r, m_w)` 里找 `\n`（`memchr`）。
4. 找到：交付 `[m_r, pos)`，`m_r = pos+1`；若末尾是 `\r` 去掉（CRLF）。
5. 没找到且缓冲未满：`fill()`（`reader.hpp:649`）。
6. `fill()` 先查 `m_err`（`:651`）：有粘性错误直接返回，不再读底层。
7. 压缩已消费前缀：`memmove`（`:658`），`m_r=0`、`m_w=buffered`。
8. 一次底层 `Read`（`:668`）；`count>0` 入缓冲、`m_empty_reads=0`、错误记进 `m_err`（`:675`）。
9. 回到 ReadLine 继续找；直到找到、缓冲满或 EOF。
10. 缓冲满仍无 `\n`：`ErrTooLong`（行长超过缓冲容量），**不**扩容。
11. EOF 且无数据：返回 EOF 错误；EOF 但有残余：返回残余行（末行无换行）。
12. 失败路径：`m_err` 粘性；`ReadLine` 返回 `Result<string>`，错误与数据不共存。

#### 场景 B：`Writer` 写 1 万行后 `Flush`（12 步）

1. 构造 `Writer(dest)`（`writer.hpp:159`），缓冲默认 4096。
2. 循环 `WriteString(line)`（`:316`）→ `Write(data,size)`（`:272`）。
3. `m_err` 非空 → 直接返回错误（`:274`），不再碰底层。
4. 缓冲空且 `size >= 缓冲容量` → 快路径直接 `dest.Write`（不拷贝）。
5. 否则能放下就 `memcpy` 进 `m_buffer`，`m_n += size`，登记段（一次 Write 一个段）。
6. 放不下：先 `Flush()` 腾空间，再放；仍放不下则直写。
7. `Flush()`（`:338`）→ `FlushLinear()`（`:568`）：一次 `dest.Write`。
8. 短写：循环写剩余（与 Go 不同，Go 直接 `ErrShortWrite`）；返回 0 且无错误 → `IoErrShortWrite`（`:576`）。
9. 底层错误：记 `m_err`（粘性），返回错误；缓冲里可能还有未写数据。
10. `Flush` 成功后 `m_n=0`、段清空、`m_last_flush=now`。
11. 析构（`:185` `= default`）不 flush：忘记 Flush 等于丢数据。
12. 失败路径：`Reset`（`:480`）会丢缓冲并清 `m_err`；`Buffered()` 仍显示未写字节（`:490`）。

#### 场景 C：`Scanner` 扫 10 万行（12 步）

1. 构造 `Scanner(source, 4096, 64KiB)`（`scanner.hpp:163`）。
2. `Split(ScanLines)`（默认）。
3. `Scan()`（`:189`）：窗口 `[m_start, m_end)` 交给 `SplitFunc`。
4. `ScanLines` 找 `\n`：找到 → `advance=pos+1`、`token=[0,pos)`（去 `\r`）。
5. 没找到且 `at_eof=false` → `{0,{},nullptr}`，`Scan` 调 `ReadMore`。
6. `ReadMore`（`:353` 起）：窗口占满 → 缓冲翻倍（`:371`），夹到 `m_max_token_size`。
7. 翻倍到上限仍放不下 → `ErrTooLong`（`:365`）。
8. `Scan` 拿到 token：`m_token=token`，`m_start += advance`，返回 true。
9. 用户 `Bytes()`（`:297`）零拷贝/`Text()`（`:288`）拷贝。
10. 下一次 `Scan` 覆盖 `m_token`，旧 `Bytes()` 视图失效。
11. EOF：`at_eof=true` 再调 SplitFunc；返回 `{0,{},nullptr}` → `Scan` false，`Err()` 为空。
12. `ErrFinalToken`：交付最后 token 后终止，`Err()` 仍为空；自定义 SplitFunc 越界 → `ErrInvalidAdvance`（`:213`），不 panic。

#### 场景 D：一次 `FlushBatch`（10 步）

1. `SetBatchConfig({kHybrid, 64, 4096, 100µs})`（`writer.hpp:389`）。
2. 多次 `Write` 累积段；每次写后 `MaybeAutoFlush` 检查策略。
3. `kOnFull`：缓冲到 `threshold` 或段数到 `max_batch` 时自动 flush。
4. `kInterval`/`kHybrid`：距上次 flush 超 `interval` 才触发；检查是惰性的（写返回前）。
5. 显式 `FlushBatch()`（`:363`）：先查 `m_err`，再 `if constexpr (HasWriteBatch)`。
6. 有 `WriteBatch` → `FlushBatchImpl()`（`:599`）：逐段构造 `BatchEntry`，一次调用。
7. 无 `WriteBatch` → `FlushLinear()`（`:568`）：段在缓冲里连续，一次 `Write`。
8. 部分写：两种路径都循环处理；返回 0 且无错误 → `IoErrShortWrite`。
9. 成功：清段、`m_last_op_byte=false`、更新 `m_last_flush`。
10. 失败路径：`m_err` 粘性；`FlushBatch` 不受策略限制（策略只管自动 flush）。

### 1.4 关键不变量

| # | 不变量 | 位置 | 破了会怎样 |
|---|---|---|---|
| 1 | `0 <= m_r <= m_w <= m_buffer.size()`；`Buffered()==m_w-m_r` | `reader.hpp:611` | 越界读/负数长度 |
| 2 | `m_err` 非空后 `fill()` 不再调用底层 | `reader.hpp:651` | 重复读、错误被覆盖 |
| 3 | `count>0` 且带 error 时先交付数据 | `reader.hpp:672`-`:675` | 静默截断最后一段 |
| 4 | `Peek/ReadSlice` 的 span 只到下一次读 | `reader.hpp:559` | 悬空视图（fill 的 memmove 覆盖） |
| 5 | `Writer` 析构不 flush | `writer.hpp:185` | 忘 Flush 丢数据 |
| 6 | `m_err` 粘性，`Reset` 才清 | `writer.hpp:152`/`:486` | 失败后继续写、数据错位 |
| 7 | `Write` 返回值只代表「已接受」 | `writer.hpp:191` | 误以为已到达对端 |
| 8 | `Scanner` 的 `atEOF` 与 `advance==0` 组合决定终止 | `scanner.hpp:199` | 死循环或提前结束 |
| 9 | `ErrFinalToken` 不是错误 | `scanner.hpp:202` | 正常结束被判失败 |
| 10 | `Bytes()` 视图下次 `Scan` 失效 | `scanner.hpp:297` | 读到被覆盖的 token |

---

## 第二部分 · 分：逐块细讲

### 2.1 `Reader` 的游标与状态机（`reader.hpp`）

**职责**：把「一次底层读」与「多次小读」解耦；状态只有 `m_r/m_w/m_buffer/m_err/m_empty_reads/m_can_unread_byte/m_last_rune_size`。

**状态机**：

| 状态 | 事件 | 迁移 |
|---|---|---|
| 空（m_r==m_w） | `Read(p)` 且 p≥Size | 快路径直读，不经缓冲 |
| 空 | `Read(p)` 且 p<Size | `fill()` → 有数据则拷贝 |
| 有数据 | `Read/ReadByte/ReadRune` | 推进 m_r |
| 有数据 | `Peek` | 不动 m_r |
| 任意 | `UnreadByte` | m_r--（仅上一次是 ReadByte） |
| 任意 | `fill()` | memmove 压缩 + 底层读 |
| 粘性错误 | 任何读 | 直接返回错误 |

**失败路径**：`fill()` 连续 100 次 0 字节无错误 → `IoErrNoProgress`（`reader.hpp:628` 的 `NoteIdleRead`）；计数到 `kMaxConsecutiveEmptyReads` 即置错。

**典型误用**：`Read` 返回 0 就以为 EOF；跨读持有 `Peek` 的 span。

### 2.2 `fill()` 与「错误延后交付」

`fill()`（`reader.hpp:649`）四步：查错误 → memmove 压缩 → 一次底层读 → `NoteIdleRead`。关键在第三步：`count>0` 时先 `m_w += count`，若同时有 error 就存进 `m_err`（`:675`），等缓冲耗尽再交给调用方。这样「最后一个包 + EOF」不会丢包。

**典型误用**：在 `fill()` 里「有 error 就清空缓冲」——会截断；把 `m_err` 当一次性（它是粘性的）。

### 2.3 `ReadLine` / `ReadSlice` / `ReadString` 的边界差异

| 方法 | 超长时 | 是否含分隔符 | 是否拷贝 | 行号 |
|---|---|---|---|---|
| `ReadLine` | `ErrTooLong`（缓冲容量上限） | 不含，去 `\r\n` | 是 | `reader.hpp:390` |
| `ReadSlice` | 返回已有全部 + `ErrBufferFull` | 含 | 否（span） | `reader.hpp:506` |
| `ReadString` | 无界（可跨多次 fill 累积） | 含 | 是 | `reader.hpp:461` |

**为什么不一样**：`ReadLine` 的签名是 `Result<string>`，没有 `isPrefix` 位，只能报错；`ReadString` 用 `std::string` 累积，天然无界；`ReadSlice` 是零拷贝，缓冲满只能交付已有一截 + 错误（Go 同）。

**典型误用**：用 `ReadLine` 处理任意长行（应用 `ReadString('\n')`）；把 `ReadSlice` 的 `ErrBufferFull` 当致命错误（它仍返回数据）。

### 2.4 `Peek` / `Discard` / Unread

- `Peek(n)`（`:568`）：`n>Size()` 直接 `ErrBufferFull`，**不预读**（与 Go 差异）；`n<=Size()` 时循环 `fill` 到够或错误；不消耗。
- `Discard(n)`（`:592`）：最多消耗 `Buffered`，返回实际消耗；不够时 `fill` 继续。
- `UnreadByte`（`:344`）/`UnreadRune`（`:358`）：只能撤销最近一次对应操作；任何其它读使它们失效。
- 失效根因：`fill` 的 `memmove`（`:658`）会搬动缓冲，旧 span 指针失效。

**典型误用**：`Peek` 后 `UnreadByte`（顺序错，Unread 只针对 ReadByte）；`Peek` 期望扩容。

### 2.5 `Writer` 的缓冲、快路径与错误粘性（`writer.hpp`）

**数据结构**：`m_destination`、`m_buffer`、`m_n`、`m_segments`、`m_err`、`m_last_op_byte`、`m_last_flush`、batch 配置。

**关键函数**：`Write`（`:272`）大块直写/缓冲；`FlushLinear`（`:568`）单次 Write + 短写循环；`FlushBatchImpl`（`:599`）逐段 `WriteBatch`；`Reset`（`:480`）丢缓冲清错；`ReadFrom`（`:429`）大块搬运。

**状态机**：正常 →（底层失败）`m_err` 粘性 → 只能 `Reset` 恢复；`Flush` 成功清 `m_n`/段，失败保留缓冲。

**典型误用**：失败后继续 `Write`（全部返回同一错误）；`Reset` 前不处理缓冲（丢数据）。

### 2.6 批量 flush（`batch.hpp` + `writer.hpp`）

- `FlushPolicy`（`batch.hpp:53`）：`kOnFull`（阈值/段数）、`kInterval`（时间）、`kOnExplicit`（只显式）、`kHybrid`（默认，满或超时）。
- 硬上限：缓冲放不下、段数到 `max_batch`（默认 64、上限 1024）必须 flush。
- `FlushBatch`（`writer.hpp:363`）：有 `WriteBatch` 一次交出，无则 `FlushLinear`；显式调用不受策略限制。
- 段 = 一次 Write/WriteString（连续 WriteByte 合并）；段只影响 iovec 数量，不改变字节流。
- 定时检查惰性（`Write` 返回前），无后台线程；写完即停必须显式 flush。

**典型误用**：以为 `FlushBatch` 一定比 `Flush` 少 syscall（无 `WriteBatch` 时一样）；依赖定时器在空闲时自动 flush。

### 2.7 `Scanner` 与 `SplitFunc`（`scanner.hpp`）

**契约**：`SplitResult{advance, token, error}`；`advance==0 && token 空` 表示「继续读」，`at_eof=true` 时表示「结束」；token 必须指向 `data` 内部（零拷贝）。

**状态机**：

| 输入 | at_eof | SplitFunc 返回 | Scanner 动作 |
|---|---|---|---|
| 有分隔符 | false | advance>0, token | 交付 token，推进 start |
| 无分隔符 | false | 0, empty | ReadMore（扩容） |
| 无分隔符 | true | 0, empty | 结束，Err 为空 |
| 任意 | 任意 | ErrFinalToken | 交付 token 后结束，Err 为空 |
| 任意 | 任意 | 其它 error | 结束，Err 非空 |
| 任意 | 任意 | advance 越界 | ErrInvalidAdvance |

**失败路径**：`ErrTooLong`（`:365`/`:380`）、`ErrInvalidAdvance`（`:213` 等）、空闲读超限（`:280`）。

**典型误用**：SplitFunc 返回的 token 指向临时串；忽略 `at_eof` 导致死循环。

### 2.8 `ByteSpan` 生命周期（`byte_span.hpp`）

- 非拥有视图：`{m_data, m_size}`（`:120`/`:121`），默认构造为空且 `data()==nullptr`（`:79`）。
- `first(n)` 要求 `n<=size()`（`:100`）；`subspan` 自行钳制（`:113`/`:116`）。
- `AsStringView`/`ToString`：前者不分配，后者拷贝（`:29`）。
- 来源决定寿命：Reader 内部缓冲（下次读失效）、Scanner 窗口（下次 Scan 失效）、调用方自己的数组（调用方负责）。

**典型误用**：把 `ByteSpan` 存进容器跨读使用；对空 span 解引用 `data()`。

### 2.9 `limits.hpp` 的边界

| 常量 | 值 | 作用域 | 位置 |
|---|---|---|---|
| `kDefaultBufferSize` | 4096 | Reader/Writer/Scanner 默认 | `limits.hpp:45` |
| `kMinBufferSize` | 16 | 小于则提升 | `limits.hpp:48` |
| `kMaxConsecutiveEmptyReads` | 100 | Reader::fill/Scanner::ReadMore/Writer::Flush/ReadFrom | `limits.hpp:51` |
| `kMaxScanTokenSize` | 64 KiB | 只约束 Scanner；ReadString 不受限 | `limits.hpp:54` |

**典型误用**：以为 `ReadLine` 受 `kMaxScanTokenSize` 限制（它受缓冲容量限制）；把 100 次空读当「100 次失败」（是「无进展」）。

### 2.10 与 Go bufio 的差异总表

| 主题 | Go | 本实现 | 位置 |
|---|---|---|---|
| `Peek(n>Size)` | 先填满，返回部分+错误 | 直接 `ErrBufferFull`，不预读 | `reader.hpp:38` |
| `ReadLine` 超长 | `isPrefix=true` 分段 | `ErrTooLong`，建议 ReadString | `reader.hpp:42` |
| `Writer` 短写 | `ErrShortWrite` 一次 | 循环写完 | `writer.hpp:13` |
| 连续空读 | 无限循环 | 100 次 → `IoErrNoProgress` | `limits.hpp:51` |
| 析构 flush | 无析构概念 | 明确不 flush | `writer.hpp:36` |
| `ScanWords` | Unicode 空白 | 仅 ASCII 空白 | `scanner.hpp:32` |
| 自定义 Split 越界 | panic | `ErrInvalidAdvance` | `scanner.hpp:37` |

### 2.11 测试与基准

```bash
cd .build-bento-wsl
./go2cpp_bufio_test          # 单元 + 随机 + 6 个场景（含「未 flush 文件=0 字节」）
./go2cpp_bufio_stress        # 随机分块/短读/大缓冲
./go2cpp_bufio_batch_test    # 5 个批量场景
./go2cpp_bufio_batch_bench   # 批量策略对比
```
观测点：`场景3` 的「未 flush 文件=0 字节」、`场景4` 的行数/底层调用、`场景6` 的短写次数。性能数字必须现场跑并记录缓冲/分块/是否绑核，不抄文档。

### 2.12 场景 E：`ReadRune` 与 `UnreadRune` 的 UTF-8 边界（12 步）

1. `ReadRune()`（`reader.hpp:311`）先确保至少有 1 字节（空缓冲就 `fill`）。
2. 用 `detail::DecodeUtf8` 解码首字节：ASCII 直接返回；多字节读 continuation。
3. 缓冲里不完整（例如只剩 2 字节但需要 3 字节）：`fill()` 继续读（会 memmove）。
4. 仍不完整且 EOF：返回 `RuneError` + `size=1`（Go 的 `utf8.RuneError` 语义）。
5. 非法首字节/continuation 错：返回 `RuneError` + 消耗 1 字节，保证前进。
6. 成功：`m_last_rune_size = size`，`m_r += size`，返回 `{rune,size,nullptr}`。
7. `UnreadRune()`（`:358`）检查 `m_last_rune_size>0` 且 `m_r>=size`，否则 `ErrInvalidUnreadRune`。
8. 回退：`m_r -= size`，`m_last_rune_size=-1`。
9. 任何其它读（`Read/Peek/ReadLine/ReadByte`）都会 `InvalidateUnread()`，之后 `UnreadRune` 失败。
10. `UnreadByte`（`:344`）只对「上一次是 ReadByte」有效；ReadRune 后不能用 UnreadByte 回退整段。
11. 失败路径：`m_err` 粘性；解码错误不是粘性错误，只影响本次返回。
12. 典型误用：把 `RuneError,size=1` 当「读到 EOF」；连续 `UnreadRune` 两次。

### 2.13 `ReadFrom`：大块搬运与空读检测

`Writer::ReadFrom(src)`（`writer.hpp:429`）用于把任意 Reader 拷进 Writer：
1. 若缓冲为空且源可直接读大块，走快路径（减少一次拷贝）；
2. 否则 `fill` 缓冲后 `Flush`；
3. 源返回 0 且无错误 → 累计空读；到 `kMaxConsecutiveEmptyReads` 判 `IoErrNoProgress`；
4. 源返回 `n>0` 且带 error → 先写数据，再返回该错误；
5. 目标 `m_err` 粘性，写失败立即停；
6. 返回值是「已写字节」，不是「已读字节」；
7. 典型误用：把 `ReadFrom` 当 `copy_file_range`（它仍走用户态缓冲）。

### 2.14 Writer 的「段」到底是什么

- 一次 `Write`/`WriteString` 在缓冲里对应一个段（`m_segments` 记录 offset/length）。
- 连续 `WriteByte` 合并成一个段（`m_last_op_byte` 标记）。
- `FlushBatch` 把每个段变成一个 `BatchEntry`；底层 `WriteBatch` 用它构造 iovec。
- 段只影响写入侧一次 syscall 的 iovec 数量；对 TCP 字节流，接收端看到的仍是顺序拼接的字节。
- 段数受 `max_batch`（默认 64、上限 1024）约束；超了必须 flush。
- 典型误用：以为段边界会保留到对端（不会）；用段数估算消息数（连续 WriteByte 会合并）。

### 2.15 改这个模块前的检查清单

1. 动 Reader：先保证 `0<=m_r<=m_w<=size` 与 `Buffered()==m_w-m_r`（`reader.hpp:611`）；任何 `fill` 改动都要考虑 `memmove` 对 span 的影响。
2. 动错误路径：`count>0` 且带 error 必须「先交付数据、后交付错误」；`m_err` 必须粘性。
3. 动 `Peek/ReadSlice`：明确「返回 span 还是拷贝」，并在注释/测试里写清失效点。
4. 动 `ReadLine`：超长只能 `ErrTooLong`（签名没有 isPrefix）；需要任意长行引导用户用 `ReadString`。
5. 动 Writer：析构不 flush 的约定不能改；`Write` 返回值只代表「已接受」。
6. 动 flush：短写必须循环；返回 0 且无错误 → `IoErrShortWrite`；失败后 `m_err` 粘性，只有 `Reset` 清。
7. 动批量：`FlushBatch` 必须在无 `WriteBatch` 时退化为 `FlushLinear`；定时检查保持惰性，不引入后台线程。
8. 动 Scanner：保持 `atEOF`/`advance==0` 的三态语义；自定义 SplitFunc 越界必须报错不 panic。
9. 动 ByteSpan：不要加隐式 `std::string` 转换（会悄悄拷贝）；`first/subspan` 的越界语义保持「first 要求、subspan 钳制」。
10. 性能结论必须附命令/环境（2.11），不抄文档数字。

### 2.16 建议阅读顺序

1. `limits.hpp`（四个常量）→ 2. `byte_span.hpp`（视图语义）→ 3. `reader.hpp` 的 `Read`/`fill`/`Peek`；
2. `writer.hpp` 的 `Write`/`FlushLinear`/`FlushBatch` → `batch.hpp`；
3. `scanner.hpp` 的 `Scan`/`ReadMore` + `src/bufio/bufio.cpp` 的三个预置 SplitFunc；
4. 最后跑 `tests/bufio/test_bufio.cpp` 的随机场景，把「超长/EOF/短写」三种边界与代码对上。

### 2.17 错误类型与来源速查

| 错误 | 触发 | 来源 | 调用方应做什么 |
|---|---|---|---|
| `IoEof()` | 底层 EOF 且缓冲空 | `reader.hpp` fill/Read | 正常结束，停止循环 |
| `ErrBufferFull()` | `Peek(n>Size)` / `ReadSlice` 缓冲满 | `reader.hpp:572` / `:528` | Peek 改小 n 或先 Read；ReadSlice 消费已给数据 |
| `ErrTooLong()` | `ReadLine` 超缓冲 / Scanner 超 max_token | `reader.hpp:390` / `scanner.hpp:365` | 改用 `ReadString` 或调大 `Buffer(max)` |
| `ErrInvalidUnreadByte/Rune` | 上一次不是对应读操作 | `reader.hpp:344/358` | 不要连续 Unread；检查调用序列 |
| `ErrInvalidAdvance` | 自定义 SplitFunc 越界/未覆盖 token | `scanner.hpp:213` 等 | 修 SplitFunc，不是重试 |
| `ErrFinalToken` | SplitFunc 声明最后一个 token | `scanner.hpp:202` | 当正常结束，`Err()` 为空 |
| `IoErrShortWrite` | 底层写返回 0 且无错误 | `writer.hpp:576` | 视为错误，`Reset` 或上报 |
| `IoErrNoProgress` | 连续 100 次 0 字节读 | `limits.hpp:51` | 底层违约，停止并诊断 |
| `ENOMEM` | 批量慢路径 `bad_alloc` | `file.cpp`（os） | 返回已写字节，调用方决定重试 |

注意：`ErrFinalToken` 是哨兵不是失败；`ErrBufferFull` 往往仍带数据；`IoEof` 与「Read 返回 0」要分开看。


