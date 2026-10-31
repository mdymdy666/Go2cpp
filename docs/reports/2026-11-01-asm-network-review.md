# 网络层汇编级审查（只读）

- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL）
- 构建：`.build-bento-wsl`（Release `-O3`）
- 审查对象：`libgo2cpp_network.so.0`、`libgo2cpp_bento.a`、`libgo2cpp_bento_http.a`、`libgo2cpp_hash.so.0`、`libgo2cpp_app.a`、`libgo2cpp_p2p.so`
- 方法：`objdump -d --no-show-raw-insn` 反汇编 + 逐函数指令分类统计 + 源码对照 + 微基准（`/tmp` 下编译，不进工作树）
- 边界：**未修改工作树任何文件**；所有临时产物在 `/tmp`。

---

## 0. 摘要

本轮共定位 **20 个指令级浪费点**，按「每操作节省时间 × 触发频率」排序。最值钱的三个：

1. **WebSocket 客户端每帧构造一次 `std::random_device`**（掩码键）——每帧 6 次库调用 + 4 次 `getentropy`，实测约 **36 µs/帧**（WSL 环境；原生 Linux 估算 2–3 µs）。改成每连接一个 PRNG，可降到 ~20 ns/帧，**约 3 个数量级**。
2. **`http::ResponseWriter::write` 每写一块都构造 `ostringstream` 并分 3 次 `send`**——每 chunk 约 1.4–2 µs 固定开销，全是编译器无法内联的库调用。
3. **HTTP 请求解析每请求 14 次堆分配**（实测 300k 次采样），加上 `serve_connection` 每请求 8 KB 栈零初始化、每路由一次 `Request::path()` 分配。

---

## 1. 方法与环境

### 1.1 指令统计口径

用 `/tmp/asm_prof.py` 解析 `objdump -d --no-show-raw-insn -C` 输出，按函数分组，把指令分成
`call / jcc / jmp / alu / atomic / rep`，并统计调用目标 Top-N。

### 1.2 实测环境

- WSL2，Ubuntu 24.04，12 硬件线程
- g++ 13.3（头文件），libstdc++ 运行时 14.2.0
- 微基准：`/tmp/bench_http.cpp`、`/tmp/bench_random*.cpp`、`/tmp/mcount.c`（`LD_PRELOAD` malloc 计数）

### 1.3 已确认的编译选项缺口

`cmake/Go2CppModules.cmake:50-78` 有一份 `-fno-plt`/`-fno-semantic-interposition` 的「热库」选项，
但作用范围只覆盖 6 个并发库。`libgo2cpp_network.so.0`、`libgo2cpp_bento*.a` **不在其中**，
因此网络热路径上的外部调用仍走 `@plt`（`Parser::feed_impl` 共 119 个 `call`，其中 66 个目标是 `@plt`）。

---

## 2. 逐点分析（按收益排序）

### P1. WebSocket 客户端每帧构造 `std::random_device`（掩码键）

- **位置**：`src/network/websocket.cpp:565` `write_frame_unlocked`，反汇编 `libgo2cpp_network.so.0:0x45230`，
  其中 `0x4564d call std::random_device::_M_init@plt`、`0x45682 call _M_getval@plt`、
  `0x45699 call _M_fini@plt`、`0x457ab call operator new@plt`、`0x45923 call operator delete@plt`。
- **现状**：客户端角色（`m_role == Client`）每发一帧就 `std::random_device random;` 构造/析构一次，
  取 4 字节掩码键。`_M_init` 内部是 `std::string` 比较 + 资源初始化；`_M_getval` 每次走
  `getentropy` 路径（`LD_PRELOAD` 探针实测 2000 次 `rd()` → 2000 次 `getentropy`）。
- **实测**（`/tmp/bench_random2.cpp`，WSL）：
  - 构造一次：**1396 ns**
  - 构造 + 4 次取值：**34.9 µs**
  - 对照：裸 `getentropy` 437 ns、`getrandom` 395 ns
  - 该路径**几乎不产生系统调用**（strace 全程 64 次），是纯用户态 + glibc 熵源开销。
- **浪费类型**：每帧一次「库对象构造/析构 + 熵源获取」，典型应摊到连接级。
- **可省指令**：每帧省 6 次库调用（3 个 PLT + new/delete）+ 4 次 `getentropy` 及其内部路径，
  估算 **每帧 150–300 条指令 + 2–36 µs**。
- **预期收益**：客户端掩码帧从 ~2 µs（原生）/ ~36 µs（本机）降到 ~20 ns；10 万帧/秒场景
  省 0.2–3.6 秒 CPU/秒。
- **难度**：低。把 `random_device`/ChaCha8 提升为 `WebSocket` 成员（或每连接一个）。
  参考 Go：`crypto/rand` 用 ChaCha8 生成器 + 32 字节缓冲，无每帧系统调用。

### P2. `ResponseWriter::write` 每 chunk 一个 `ostringstream` + 3 次 `send`

- **位置**：`src/network/http.cpp:685`，反汇编 `0x39e60`（165 条指令，17 call）。调用目标里出现
  `basic_ostringstream()`、`ostream::_M_insert<unsigned long>`、`locale::~locale`、`ios_base::~ios_base`、
  `_M_replace`、`_M_assign`、`__stack_chk_fail`——每次写 chunk 都付一遍 iostream 初始化/销毁。
- **现状**：
  ```cpp
  std::ostringstream prefix;
  prefix << std::hex << chunk.size() << "\r\n";
  const auto prefix_wire = prefix.str();      // 再分配 + 拷贝
  ... write_all(prefix) ... write_all(chunk) ... write_all("\r\n")   // 3 次系统调用
  ```
- **浪费类型**：流对象构造（locale/虚表/分配）+ `str()` 复制 + 3 次系统调用可合并成 1 次 `writev`。
- **可省指令**：`ostringstream` 与 `str()` 整段约 **300–400 条指令 + 2 次 malloc**；
  2 次系统调用约 **1.2 µs**。
- **预期收益**：每个 SSE/流式 chunk 省 **~1.4–2 µs**；SSE 每事件一次 chunk，收益线性。
- **难度**：低。`std::to_chars` 到栈上 20 字节 + `writev`（3 个 iovec）。
  同文件 `Response::to_string` 已经是这种写法，可直接照搬。

### P3. HTTP Parser 每请求 14 次堆分配

- **位置**：`src/network/http.cpp:341` `Parser::feed_impl`，反汇编 `0x3d080`（**1622 条指令**，**119 个 call**，
  其中 `string::basic_string(const char*)` 21 次、`_M_dispose` 20 次、`_M_construct` 7 次、
  `Headers::add(string,string)` 4 次）。
- **现状**：每个头字段执行 `header.add(std::string(name), value)`：
  - `trim()` 返回 `std::string`（一次分配，值 >15 字符时）；
  - `std::string(name)` 再分配一次；
  - `Headers::add` 的 `m_entries.emplace_back` 触发 vector 扩容（前几个头各一次）。
- **实测**（`/tmp/bench_http.cpp` + `LD_PRELOAD` 计数）：
  - 348 B 请求 / 10 个头：**14 次 malloc + 14 次 free**；
  - 解析耗时 **1007 ns/请求**（300k 次，`-O2` 客户端）；
  - 分配占比按 25 ns/次估算 ≈ 350 ns，即 **约 35% 的解析时间**。
- **浪费类型**：每头 1–2 次堆分配，可改为视图（`string_view` 指向 parser 内部 buffer，生命周期到下次
  `feed()`）或 arena。
- **可省指令**：每请求约 **250–400 条指令**（分配器路径 + string 构造）。
- **预期收益**：解析 **-300 ns/请求（约 30%）**；若配合去 `trim` 分配，可到 -400 ns。
- **难度**：中。`Headers::Entry` 改 `string_view` 会改变 API 生命周期语义，需要一次性梳理调用方
  （`Client`、`Server`、`WebSocket::ValidateUpgradeRequest`、`sse`）。

### P4. `Server::serve_connection` 每请求 8 KB 栈零初始化 + 每路由一次 `Request::path()`

- **位置**：`src/network/http.cpp:763`，反汇编 `0x3ed20`（441 条指令，48 call）。
  - 函数入口 `sub $0x1000,%rsp; orq $0x0,(%rsp)` 连续两次（栈探测 2 次，栈帧 0x2378 = 9080 B）；
  - `0x3eea4 rep stos %rax` —— 每次循环迭代把 8 KB `std::array<char, 8192> buffer{}` 清零；
  - `0x3f22f call Request::path()` 位于路由循环体内，每个路由都重新 `target.substr(0, '?')`，
    每次 **1 次 malloc + 拷贝**；
  - 每请求拷贝一次 `std::function`（`Handler selected = m_fallback`），可能触发分配。
- **浪费类型**：循环内反复清栈数组；路由匹配 O(路由数) 次字符串分配；`std::function` 按值拷贝。
- **可省指令**：8 KB 清零 ≈ **500–1000 条 store**（`rep stos` 按 8 B/条），路由 path 每次约 **40 条 + malloc**，
  `std::function` 拷贝约 **30–60 条**。合计 **每请求 600–1200 条**。
- **预期收益**：每请求 **0.5–1 µs**（8k ops/s 的单连接 server 约 0.5–1% CPU；高并发时随请求数线性）。
- **难度**：低。buffer 移到循环外并只清一次；预计算 `std::string_view path = request.path_view()`；
  `const Handler* selected` 指针代替拷贝。

### P5. `Request::to_string` 仍是 `ostringstream` + 多次 `method_name()`

- **位置**：`src/network/http.cpp:255`。`Client::do_request`（`0x3f8c0`，842 条指令，90 call）直接调用它。
- **现状**：
  ```cpp
  if (method_name(method).empty() || ...) return {};       // 第 1 次构造 string
  std::ostringstream stream; stream << method_name(method) << ...   // 第 2 次
  ... stream << body; return stream.str();                 // 再复制一次
  ```
  另外 `header.has("Connection")` / `header.has("Content-Length")` 各做一次线性扫描 + 逐字符 `tolower`。
- **浪费类型**：`method_name()` 返回 `std::string`（调用 3 次 = 3 次潜在分配）、`ostringstream` 构造、
  `str()` 复制、`has()` 重复扫描。
- **可省指令**：约 **300–500 条指令 + 2–3 次 malloc**。
- **预期收益**：每个出站请求 **~300–500 ns**。
- **难度**：低。`Response::to_string` 已经是 `reserve + to_chars + append` 的写法，照抄即可；
  `method_name` 改返回 `std::string_view`。

### P6. `Socket::sendv/recvv` 每包 4 次 `mutex` + `shared_from_this`

- **位置**：`src/network/socket.cpp:736` `sendv`（`0x2a4b0`，391 B）、`:710` `recv_impl`、`:689` `send_impl`。
- **现状**：一次 `send()` 依次经历
  1. `m_write_cancel_generation.load(acquire)`
  2. `begin_operation()`：`shared_from_this()`（原子引用计数）+ `lock_guard(m_mutex)` + `++m_inflight`
  3. 第二次 `load(acquire)`
  4. `if (effective == Duration::max()) { lock_guard(m_mutex); effective = m_send_timeout; }`
  5. `send_impl` → `is_closing()` → **又一次 `lock_guard(m_mutex)`**
  6. `send_impl` → `steady_clock::now()`（每次调用）
  7. `end_operation()` → **又一次 `lock_guard(m_mutex)`** + 可能 `shared_ptr` 析构
- **实测**：`bench_net_batch` 的「TCP 小消息逐条 send」为 **1350 ns/msg（64 B）**；批量 `writev` 为 104 ns/msg。
  4 次无竞争 mutex 约 60–80 ns，`shared_from_this` 约 10–20 ns。
- **浪费类型**：快路径上的锁与原子操作，可用原子计数 + 只读快照替代。
- **可省指令**：每包约 **80–120 条指令**（`lock cmpxchg` 前后 + 分支 + 内存屏障）。
- **预期收益**：每包 **~100 ns**（对 1350 ns 的 64 B 消息是 **7%**）。
- **难度**：中。需要保证 `close()` 与在途操作的并发语义（`m_inflight` + `m_fd` 一起发布）。

### P7. `RelayClient::SendTo` 每包 2 次 vector 分配 + 2 次 payload 拷贝

- **位置**：`src/p2p/turn_client.cpp:232`，反汇编 `libgo2cpp_p2p.so:0x34450`（271 条指令，20 call：
  2× `operator new`、5× `operator delete`、3× `memmove`、2× `memcpy`、`pthread_mutex_lock/unlock`）。
- **现状**：
  ```cpp
  std::vector<std::uint8_t> body; body.reserve(3 + 16 + length);   // 分配 1
  AppendEndpointBody(&body, peer); body.insert(..., bytes, bytes+length);  // 拷贝 1
  const std::vector<std::uint8_t> wire = SerializeRelayMessage(message);   // 分配 2 + 拷贝 2
  ```
- **浪费类型**：每包两次堆分配、payload 两次字节拷贝、一次 mutex。
- **可省指令**：约 **150–250 条指令 + 1 次 malloc + 1 次 memcpy**。
- **预期收益**：每包 **~150–250 ns**（P2P 中继路径）。
- **难度**：低。成员 scratch + `SerializeRelayMessageInto(span)`；`body` 用 `std::span` 指向 scratch 尾部。

### P8. CRC32 每 8 字节约 20 条指令（1 KB 帧 ≈ 2500 条）

- **位置**：`src/hash/crc32.cpp:35` `Crc32::Update`（`libgo2cpp_hash.so.0:0x3790`，94 条指令，0 call）。
  已是 slicing-by-8（8 张 256 项表，每轮 4 次移位 + 8 次 load + 7 次 xor）。
- **现状**：Bento 帧的 `ComputeFrameChecksum`（`frame.cpp:128`）对**每帧完整 payload** 做一次 CRC；
  1 KB 帧约 128 轮 × ~20 条 ≈ **2500 条指令**，估算 500–900 ns。
- **浪费类型**：标量查表；x86 有更快的等价手段。
- **可省指令**：PCLMULQDQ 折叠（IEEE 多项式）可到 ~0.1–0.2 cycles/byte，1 KB 约 **150–300 条指令**，
  省 **~2200 条/帧**。
- **预期收益**：每 1 KB 帧 **~500–800 ns**（帧越小收益越小）。
- **难度**：中高。需要 CPU 特性分派（`__builtin_cpu_supports("pclmul")`）+ 位精确实现 + 与 Go
  `hash/crc32` 的 IEEE 结果对齐测试；或协议层改用 CRC32C（硬件指令 `crc32q`），但那会改线格式。

### P9. `WebSocket::read_frame` 每帧 `payload.resize` + 逐字节掩码 XOR

- **位置**：`src/network/websocket.cpp:493`，反汇编 `0x466e0`（467 条指令，39 call）。
  掩码循环在 `0x469a9–0x469bb`：`xor %eax,%eax; and $0x3,%ecx; xor %cl,(%rdx,%rax,1)`——每字节 3–4 条指令。
- **现状**：服务端每帧 `frame.payload.resize(length)`（可能分配）+ 逐字节 XOR（1 KB = ~3000 条指令）；
  另有 8 处 `protocol_error("<字面量>")` 的错误路径会构造 `std::string`（I-cache 压力）。
- **浪费类型**：每帧一次分配；XOR 未向量化。
- **可省指令**：4 字节展开可省 3/4；SSE2（16 B）可省 15/16，1 KB 帧从 ~3000 条降到 ~250 条。
- **预期收益**：每 1 KB 帧 **~500–800 ns**；每帧分配另省 ~50 ns。
- **难度**：低（先做 4/8 字节展开）到中（SSE2 + 尾处理）。

### P10. Bento 帧 `Encode`/`Decode` 每帧各一次堆分配 + 一次 payload 拷贝

- **位置**：`src/bento/net/frame.cpp:144` `Encode`（`libgo2cpp_bento.a`，106 条指令，4 call）、
  `:185` `Decode`（210 条指令，7 call）、`:235` `FrameDecoder::Next`（197 条）、`:221` `Append`（191 条）。
- **现状**：
  - `Encode` 返回 `std::vector<std::uint8_t>`（`vector out(total)` 分配 + `memcpy` payload）；
  - `ParseOne` 里 `out->payload.assign(...)`（又一次分配 + 拷贝）；
  - `FrameDecoder::Append` 先 `m_buffer.erase(begin, begin+offset)` 再 `insert`（191 条指令，含搬移）。
- **浪费类型**：编解码所有权模型强制拷贝；解码可零拷贝（视图）。
- **可省指令**：每帧约 **120–200 条指令 + 1 次 malloc + 1 次 memcpy**（若调用方提供输出缓冲）。
- **预期收益**：每帧 **~100–150 ns** + 降低 allocator 压力；1 GB/s 帧流量下约 1–2% CPU。
- **难度**：中。需要 `EncodeInto(span)` 与 `FrameView`（payload 指向 decoder buffer）两套 API，
  `session.cpp:981/1003` 是主要调用方。

### P11. `bufio::Writer::Write` 每次写都取一次时钟 + 一次原子加

- **位置**：`include/go2cpp/bufio/writer.hpp:213` `Write`（`libgo2cpp_bento_http.a`，345 条指令，26 call，
  含 `lock addl $0x1,0x8(%r12)`）；`MaybeAutoFlush`（:648）在 `PolicyUsesInterval()` 时调用
  `std::chrono::steady_clock::now()`（该 `.a` 里 `R_X86_64_PLT32 steady_clock::now` 两处）；
  `FlushBatch`（:380）与 `Flush`（:363）各再取一次时钟。
- **现状**：每次 `Write` 返回前 `MaybeAutoFlush()`，定时策略下 `now()` 一次（实测 18.4 ns）；
  `m_err`（`shared_ptr`）在若干路径上触发引用计数原子操作。
- **浪费类型**：小写入场景下每次写付一次 vDSO 时钟 + 原子。
- **可省指令**：每次写 **~20–30 条指令 + 18 ns**（若改为每 N 次或每 N 字节检查一次）。
- **预期收益**：小写入（日志行、HTTP 头）**2–5%**。
- **难度**：低。

### P12. `bufio::FlushBatchImpl` 每次 flush 重建 `BatchEntry` 数组

- **位置**：`writer.hpp:619`，`libgo2cpp_bento_http.a` 中 784 条指令、105 个条件跳转、37 call、4 个 `lock`。
- **现状**：每次 flush 都 `m_entries.clear(); for (segment : m_segments) m_entries.push_back(...)`，
  再调 `WriteBatch`；段表本来就是按顺序铺满的连续区间，可以零拷贝转成 `iovec`。
- **可省指令**：每次 flush **~200–400 条指令**（循环 + BatchEntry 构造 + 边界检查）。
- **预期收益**：每次批量 flush **~100–200 ns**；高写入率下 2–4%。
- **难度**：低。写入时直接维护 `iovec` 数组，`FlushBatch` 只做一次指针传递。

### P13. HTTP Parser 的冗余校验（每头两次全扫）

- **位置**：`http.cpp:341`，头循环内：
  - `if (name.find_first_of("\r\n") != npos) return Error;` —— `name` 来自 `\r\n` 切分区间，
    `colon < end` 已排除跨行，该检查**恒为假**；
  - `for (const unsigned char c : value) if (c < 0x20U && c != '\t') return Error;` —— 每头把 value 再扫一遍。
- **可省指令**：每头 **~20–40 条**（10 头 ≈ **200–400 条/请求**，约 60–120 ns）。
- **预期收益**：解析 **~5–10%**。
- **难度**：低（删除恒假检查；value 校验可合并到 trim 的同一次扫描）。

### P14. `equal_ci` 逐字符调用 `std::tolower`（PLT）

- **位置**：`http.cpp:25`；反汇编 `feed_impl` 内 `tolower@plt` 6 次、`isalnum@plt` 7 次、`isspace@plt` 4 次。
- **现状**：每次比较 2 次 `tolower` 函数调用（经 PLT），且 `Headers::has/get/erase/values`
  每次都线性扫描 + 逐字符 `tolower`。
- **可省指令**：单字符从「2 次 PLT + 2 条比较」降到「1 次 `or $0x20` + 1 条比较」；
  典型请求省 **50–150 条指令**。
- **预期收益**：**~30–80 ns/请求**。
- **难度**：低（`(a | 0x20) == (b | 0x20)`，仅对 ASCII 头名安全，可在注释里写明）。
  同时把 `Headers` 的比较改成先 size 再 memcmp 的快速路径。

### P15. `network.so` / `bento*.a` 未启用 `-fno-plt`

- **位置**：`cmake/Go2CppModules.cmake:58-78`（选项已存在，仅覆盖 6 个并发库）。
- **现状**：`feed_impl` 共 119 个 `call`，其中 **66 个目标是 `@plt`**；`ResponseWriter::write` 17 个调用
  全部 `@plt`；`write_frame_unlocked` 34 个调用全部 `@plt`。
- **可省指令**：每 PLT 调用省 1 次 `jmp` + 1 次 GOT 载入（约 2 条），66 次 ≈ **130 条/请求**；
  同时消除 lazy-binding 分支，I-cache 更友好。
- **预期收益**：**~30–80 ns/请求**（1–5%），全库代码尺寸略降。
- **难度**：低（CMake 目标列表加两行）。

### P16. `write_frame_unlocked` 每帧栈探测 + 栈保护

- **位置**：`0x4523e sub $0x1000,%rsp; 0x45245 orq $0x0,(%rsp)`，栈帧 **0x1428 = 5160 B**；
  `0x45251 mov %fs:0x28` / 返回前比较 / `__stack_chk_fail@plt`。
- **现状**：栈帧 5 KB 主要来自 `std::array<std::uint8_t, 14 + 512> buffer{}`（小帧合并发送用），
  超过 4 KB 保护页 → 每次调用多一条触碰写 + 保护检查；且 `buffer{}` 会先零初始化 526 B。
- **可省指令**：每帧 **~10–20 条**（栈探测 + canary 读写）外加 526 B 清零。
- **预期收益**：**~10–20 ns/帧**。
- **难度**：低（把 inline buffer 移到成员/线程局部；或把函数拆成「小帧/大帧」两条路径）。

### P17. `WebSocket::read_exact` 每帧 4 次调用 + 每次取时钟

- **位置**：`websocket.cpp:415`，`0x447c0`（148 条指令，9 call）；`read_frame` 里分别读 2 B 头、
  2/8 B 扩展、4 B 掩码、payload，共 4 次。
- **现状**：每次 `read_exact` 入口 `steady_clock::now()`，并检查 prefetch 区；
  有 16 KB prefetch 缓冲，但同一帧内仍反复进出。
- **可省指令**：每帧 **~50–100 条**（3 次函数调用 + 3 次 `now()`）。
- **预期收益**：**~60–100 ns/帧**。
- **难度**：低（用一次 `read_exact(header, 14)` 把基础头+扩展+掩码一次读完，再一次性读 payload）。

### P18. CoAP 报文处理每包 517 条指令

- **位置**：`src/bento/net/coap.cpp`，`CoapClient::HandleDatagram`（`libgo2cpp_bento.a:0x63a0`，**517 条指令，
  42 call，76 个条件跳转**）；`SendRequest`（0x5e00，234 条，11 call）。
- **现状**：解析 option 时逐个构造对象/字符串，报文对象按值传递；每数据报 42 次调用是主要成本。
- **可省指令**：约 **150–250 条/报**（option 解析改视图 + 栈数组）。
- **预期收益**：**~200–400 ns/报**。
- **难度**：中（需要改 CoAP option 存储模型）。

### P19. `Headers::get` / `values` 返回 `std::string` 拷贝

- **位置**：`http.cpp:248-249`。每次 `get()` 返回 `std::string`（一次 malloc + 拷贝），
  `values()` 返回 `vector<string>`（每个匹配一次拷贝）。
- **现状**：`has_connection_token`（`http.cpp:77`）每请求调用 `values("Connection")`；
  `Request::add_cookie` 调用 3 次 `get`；`WebSocket::ValidateUpgradeRequest` 多次 `get`。
- **可省指令**：每次调用 **~30–50 条 + 1 次 malloc**。
- **预期收益**：每请求 **~50–150 ns**。
- **难度**：低-中（新增 `get_view`/`values_view`，保留旧 API 兼容）。

### P20. `StreamTable::FillPending` 每条数据一次状态机 + 每流统计

- **位置**：`libgo2cpp_app.a:0x11d0`（130 条指令，6 call）、`ReceiveData`（38 条）、`ConsumeSend`（17 条）。
- **现状**：`FillPending` 每次发送窗口更新都遍历 `pending` 队列并刷新状态（18 个条件跳转）。
- **可省指令**：约 **30–60 条/次**（把「窗口更新」与「队列遍历」解耦，用脏标记延迟刷新）。
- **预期收益**：**~20–40 ns/次**；HTTP/2 高并发流场景收益放大。
- **难度**：中。

---

## 3. 实测数据汇总

| 指标 | 数值 | 来源 |
|---|---|---|
| HTTP 解析（348 B / 10 头） | **1007 ns/请求** | `/tmp/bench_http`（300k 次） |
| HTTP 解析堆分配 | **14 次 malloc + 14 次 free / 请求** | `LD_PRELOAD` malloc 计数 |
| Go `net/http.ReadRequest`（同请求，reader 复用） | 2430 ns/请求 | `/tmp/bench_go_http2` |
| Go `net/http.ReadRequest`（每次新建 reader） | 4311 ns/请求 | `/tmp/bench_go_http` |
| `std::random_device` 构造 | 1396 ns | `/tmp/bench_random2` |
| `std::random_device` 构造 + 4 次取值 | **34.9 µs** | `/tmp/bench_random2` |
| 裸 `getentropy` / `getrandom` | 437 ns / 395 ns | `/tmp/bench_sys` |
| `steady_clock::now()` | 18.4 ns | `/tmp/bench_sys` |
| TCP 小消息逐条 `send`（64 B） | 1350 ns/msg | `bench_net_batch` |
| TCP 小消息 `writev` 批量（64 B） | 104 ns/msg | `bench_net_batch` |
| UDP 单包 vs batch-64 | 5477 → 3615 ns/包 | `bench_net_batch` |

**关于 Go 对比的结论**：直觉上「C++ 应比 Go 快」，实测确实如此（1007 vs 2430 ns），
但这不代表没有浪费——C++ 侧 35% 的时间在 14 次 `malloc`/`free` 上，而 Go 的
`textproto` 用 `[]byte` 扫描 + map 一次成型。**两边的绝对差距掩盖了同一类问题：每头一次分配。**
把 P3 做掉后，C++ 侧可到 ~600 ns，与 Go 拉开 4 倍。

## 4. 逐函数指令统计（完整）

| 库 | 函数 | 指令数 | call | jcc | jmp | 原子 |
|---|---|---|---|---|---|---|
| network.so | `http::Parser::feed_impl` | 1622 | 119 | 172 | 87 | 0 |
| network.so | `http::Client::do_request` | 842 | 90 | 55 | 50 | 0 |
| network.so | `http::Server::serve_connection` | 441 | 48 | 31 | 16 | 0 |
| network.so | `WebSocket::read_message` | 510 | 56 | 45 | 34 | 0 |
| network.so | `WebSocket::write_frame_unlocked` | 496 | 34 | 41 | 21 | 0 |
| network.so | `WebSocket::read_frame` | 467 | 39 | 39 | 30 | 0 |
| network.so | `Socket::recv_batch` | 246 | 13 | 25 | 9 | 0 |
| network.so | `Socket::send_batch` | 234 | 13 | 24 | 10 | 0 |
| network.so | `http::ResponseWriter::write` | 165 | 17 | 9 | 7 | 0 |
| network.so | `WebSocket::read_exact` | 148 | 9 | 13 | 6 | 0 |
| network.so | `http::ResponseWriter::start` | 127 | 14 | 8 | 9 | 0 |
| network.so | `Socket::send_impl` / `recv_impl` | 107 / 105 | 6 / 6 | 8 / 8 | 0 | 0 |
| bento.a | `Decode`（FrameFormat 版） | 210 | 7 | 22 | 11 | 0 |
| bento.a | `FrameDecoder::Next` | 197 | 8 | 22 | 14 | 0 |
| bento.a | `FrameDecoder::Append` | 191 | 9 | 25 | 10 | 0 |
| bento.a | `CoapClient::HandleDatagram` | 517 | 42 | 76 | 27 | 2 |
| bento.a | `Encode`（FrameFormat 版） | 106 | 4 | 4 | 1 | 0 |
| bento_http.a | `bufio::Writer::FlushBatchImpl` | 784 | 37 | 105 | 33 | 4 |
| bento_http.a | `bufio::Writer::Write` | 345 | 26 | 47 | 22 | 3 |
| hash.so | `Crc32::Update` | 94 | 0 | 9 | 1 | 0 |
| p2p.so | `RelayClient::SendTo` | 271 | 20 | 26 | 13 | 1 |
| p2p.so | `RelayClient::RecvFrom` | 183 | 16 | 21 | 6 | 0 |
| app.a | `StreamTable::FillPending` | 130 | 6 | 18 | 5 | 0 |

> 注：表中「原子」列只统计真实原子指令（`lock` 前缀 / `xchg` 内存）。反汇编里大量出现的
> `xchg %ax,%ax` 是 2 字节 NOP（对齐填充），不是原子操作，未计入。

## 5. 复现命令

> 说明：微基准源码是本次审查的临时文件，审查结束后已按「临时文件即用即删」规范清理；
> 下面保留命令作为复现参考，重新生成同名源码即可（源码结构见 §3 对应条目）。

```bash
cd /UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl
# 单函数指令统计
python3 /tmp/asm_prof.py libgo2cpp_network.so.0 'go2cpp::http::Parser::feed_impl'
# 反汇编 write_frame_unlocked
objdump -d --no-show-raw-insn -C libgo2cpp_network.so.0 --start-address=0x45230 --stop-address=0x45a90
# 微基准（需先编译）
g++ -O2 -std=c++17 /tmp/bench_http.cpp -I/UserData/CodexWorkSpace/Go2cpp/include \
    -L/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl -lgo2cpp_network \
    -Wl,-rpath,/UserData/CodexWorkSpace/Go2cpp/.build-bento-wsl -o /tmp/bench_http
LD_PRELOAD=/tmp/mcount.so /tmp/bench_http
```

## 6. 未覆盖 / 后续

- **HTTP/2 与 HTTP/3**（`src/network/http2`、`quic`）未纳入本轮：函数量级更大，建议单独一轮。
- **MQTT**（`src/bento/net/mqtt.cpp`）只做了符号定位，未逐函数统计。
- P1 的 34.9 µs 是 WSL 环境实测，原生 Linux 上 `std::random_device` 应显著更低；
  但「每帧构造一个熵源对象」这一点与平台无关，仍是结构性浪费。
- 所有估算的「可省指令数」来自反汇编静态计数；「预期收益」中标注实测的部分来自微基准，
  未标注的是按 IPC≈2、3 GHz 的保守换算。
