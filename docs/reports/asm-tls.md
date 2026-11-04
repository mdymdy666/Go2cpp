> 来源：/tmp/tls_asm.md，2026-11-02 归档

# TLS 汇编级证据 + vs OpenSSL

- 日期：2026-11-02
- 工作树：`/UserData/CodexWorkSpace/Go2cpp`（WSL 树，非 `E:` 树）
- 独立构建：`/tmp/build-tls`（Release，GCC 13.3.0，OpenSSL 3.0.13，CMake 3.28.3）
- 机器：AMD Ryzen 9 8945HX（WSL2，12 vCPU，AVX2/AVX512）
- 编译选项（`compile_commands.json` 实测）：
  `-O3 -DNDEBUG -std=c++17 -fPIC -Wall -Wextra -Wpedantic -fno-plt -ftls-model=initial-exec`
- 边界：只改 `include/go2cpp/network/http2/tls_transport.hpp`（+3 行）与
  `src/network/http2/tls_transport.cpp`（+166/−12 行）；**未 commit**。

---

## 0. 结论速览

| 维度 | 结果（TLS 传输层 vs 裸 OpenSSL） | 判定 |
|---|---|---|
| 握手延迟（共享 `SSL_CTX`） | mean **1.070×**，p50 1.091×，p99 1.120× | ✅ <1.1× |
| 握手延迟（每次新建 `SSL_CTX`，同等语义） | mean **0.971×** | ✅ |
| 32KB 写吞吐（3 轮） | **0.933–1.008×**（均值 0.975×） | ✅ |
| 64B 小包写延迟（4 轮，优化后） | **0.946–1.064×**（优化前 1.29–1.35×） | ✅ |
| 32KB 读吞吐（多轮） | 0.953–1.085×（均值 ≈1.01×） | ✅ |

| 维度 | 结果（自研 ChaCha20-Poly1305 vs OpenSSL EVP） | 判定 |
|---|---|---|
| 64B 小包 | **0.52×**（自研更快，EVP init 开销占主导） | ✅ |
| 256B / 1KB / 16KB / 1MB | 1.41× / 4.10× / 11.93× / **13.41×** | ❌ 不达标 |

**一句话**：TLS 传输层（`TlsTransport` 对 OpenSSL 的封装）全维度 <1.1×（小包写优化后
0.95–1.06×）；自研 AEAD 仅在 ≤64B 领先，≥256B 起被 OpenSSL 的 AVX2/AVX512 实现
逐步拉开到 13.4×——纯标量代码的物理差距，无法靠常量级优化追平。

---

## 1. 范围澄清（重要，避免按错误前提审计）

1. **`8f0f522` 的 “TLS” 是 thread-local storage，不是 Transport Layer Security。**
   该提交给 10 个库加 `-ftls-model=initial-exec`，把 `__tls_get_addr` 261→0。
   与 TLS 握手/加解密无关。
2. **`include/go2cpp/network/tls*` / `src/network/tls*` 不存在。**
   实际文件是 `include/go2cpp/network/http2/tls_transport.hpp` 与
   `src/network/http2/tls_transport.cpp`（`docs/modules/tls.md` §1 已有同样的路径澄清）。
3. **TLS 模块没有自研密码学。** 握手（`SSL_connect`/`SSL_shutdown`）、加解密
   （AES-GCM / ChaCha20-Poly1305）、证书校验、ALPN 全部由 OpenSSL 提供；
   因此 TLS 路径上的“vs OpenSSL”本质是**封装层开销对比**。
4. **仓库内唯一的自研 AEAD 在 `src/bento/net/crypto.cpp`**（Bento PSK：
   ChaCha20-Poly1305 RFC 8439 + HChaCha20 + X25519），**不在 TLS 路径上**，
   也不在本次允许修改的边界内。本报告对它只做 objdump 与对标，不改代码。
5. **`tls_transport.cpp.o` 自身 0 个 `thread_local`**；唯一的 TLS 重定位是
   `error::detail::ThreadValues()::values` 的 6 条 `R_X86_64_GOTTPOFF`
   （initial-exec 已内联），`libgo2cpp_network.so` 的 `call __tls_get_addr` **= 0**。
   即：整 TU initial-exec 在含 TLS 模块的 network 库上确实生效。

---

## 2. 方法与复现口径

### 2.1 objdump

```bash
objdump -dr --no-show-raw-insn -C \
  /tmp/build-tls/CMakeFiles/go2cpp_network.dir/src/network/http2/tls_transport.cpp.o \
  > /tmp/tls_asm/tls_transport_dr.asm
objdump -dr --no-show-raw-insn -C \
  /tmp/build-tls/CMakeFiles/go2cpp_bento.dir/src/bento/net/crypto.cpp.o \
  > /tmp/tls_asm/crypto_dr.asm
```

统计脚本 `/tmp/tls_asm/analyze3.py`：解析函数边界、`call` 的 PLT/GOT 重定位目标、
`sub $imm,%rsp` 峰值栈帧、`v*` 向量指令、`j*/ret` 分支数。

### 2.2 vs OpenSSL

- 基线：`openssl speed -evp aes-256-gcm -seconds 2`、
  `openssl speed -evp chacha20-poly1305 -seconds 2`（同一台机器、同一个 libcrypto）。
- 自研 AEAD：`/tmp/tls_bench/bench_aead`，与 `EVP_EncryptInit_ex` /
  `EVP_EncryptUpdate` / `EVP_EncryptFinal_ex` + `GET_TAG` 完全同口径
  （每次完整 init、同 buffer、同循环），尺寸 64B–1MB。
- TLS 传输层：`/tmp/tls_bench/bench_tls`，本地回环、自建 OpenSSL server
  （`SSL_accept`），client 侧两套实现：
  - `TlsTransport`：非阻塞 fd + `poll` + `SSL_*`；
  - 裸 OpenSSL：同一个 `SSL_CTX`/`SSL_connect`/`SSL_write`/`SSL_read`，
    同样非阻塞 + `poll` 状态机（对齐等待策略）。
  握手 300 次取 mean/p50/p99；写/读各 256MB；小包 64B 交替 4 轮 × 5 万次。

---

## 3. objdump：TLS 传输层热函数

### 3.1 函数级统计（优化前 → 优化后）

| 函数 | 指令 | call | 栈帧 | 说明 |
|---|---:|---:|---:|---|
| `Handshake` | 314 → **344** | 24 → 25 | 568 → **600B** | 握手循环 + SIGPIPE 兜底 |
| `Write` | 146 → **172** | 15 → 16 | 568 → **600B** | 热路径：`SSL_write` |
| `Read` | **92** | 9 | **16B** | 热路径：`SSL_read` |
| `WaitFor` | **117** | 8 | **16B** | `poll` 100ms 分片 |
| `Close` | 149 → **182** | 17 → 18 | 544 → 576B | `SSL_shutdown` + SIGPIPE 兜底 |
| `CreateClient` | 209 → **267** | 27 → 38 | 72B | 含自定义 BIO 挂载 |
| `RecordSslError` | 289 | 21 | 96B | 错误路径（冷） |
| `Deadline` | 16 | 1 | 0B | `steady_clock::now` |
| `Closed` | **15** | 0 | 0B | 内联原子读 |
| `LastErrorString` | 74 | 8 | 24B | 诊断路径 |

> 静态指令数在优化后**增加**是预期的：快路径多了一个 `required` 分支与
> `m_nosignal_bio` 检查，而慢路径（错误处理）代码仍在。真正下降的是**热路径的
> 系统调用数**（见 §9）。

### 3.2 `Read` 快路径（优化前后不变）

```
9c4:  cmpq  $0x0,0x20(%rdi)      ; m_ssl == nullptr ?
a10:  call  Deadline()           ; steady_clock::now
a2d:  call  ERR_clear_error
a3d:  call  SSL_read
a43:  mov   %eax,%esi
a45:  test  %eax,%eax
a47:  jg    a93                  ; >0 → 直接返回，无 memcpy、无额外分支
```

一次成功读 = **3 个调用**（`Deadline` / `ERR_clear_error` / `SSL_read`），
栈帧 16B，数据从 `SSL_read` 直接写入调用方 buffer（零拷贝）。
其余 6 个 call（`SSL_get_error`、`WaitFor`、`RecordSslError`、`ERR_peek_error`）
全在错误/WANT 分支，不在热路径。

### 3.3 `Write` 热路径（优化后）

旧实现每次 `Write`（无论成功失败）固定执行 4 次信号系统调用：

```
构造: pthread_sigmask(SIG_BLOCK)      → rt_sigprocmask
写:   SSL_write
析构: sigpending                       → rt_sigpending
      sigismember (用户态) + sigtimedwait → rt_sigtimedwait
      pthread_sigmask(SIG_SETMASK)     → rt_sigprocmask
```

优化后（`/tmp/tls_asm/tls_transport_final_dr.asm`）：

```
ScopedSigpipeBlock(bool required):
    test required → false → 直接返回（已挂 MSG_NOSIGNAL BIO）
  否则 SigpipeGloballyIgnored() 命中 → 返回
  否则 pthread_sigmask(SIG_BLOCK)（兜底路径）
析构:
  仅 m_failed 才 sigpending/sigtimedwait；成功路径只恢复掩码
```

`strace -c` 实测 40 万次小包写（4 轮 × 5 万 × 两套实现，其中 TLS 20 万次）：

```
% time     seconds  usecs/call     calls    errors syscall
 97.14    0.000238          15        15           rt_sigprocmask
  2.86    0.000007           3         2           rt_sigaction
100.00    0.000245          14        17           total
```

`rt_sigpending` = 0，`rt_sigtimedwait` = 0。按旧实现口径，这 20 万次 TLS 写至少
应产生 20 万次 `rt_sigpending` + 20 万次 `rt_sigtimedwait` + 40 万次
`rt_sigprocmask`。**信号系统调用降了 4 个数量级。**

### 3.4 栈帧说明

- `Read` 16B：只有 deadline/buffer 指针。
- `Write` 600B：`sigset_t`（128B）×2 + 错误字符串构造的临时区 + 对齐；
  这是**慢路径/兜底路径**的预留，热路径只用其中少量槽位。
- `Handshake` 600B、`Close` 576B：同为兜底 + `SSL_*` 调用参数。

---

## 4. objdump：自研 ChaCha20-Poly1305（`src/bento/net/crypto.cpp`）

### 4.1 函数级统计

| 函数 | 指令 | call | 栈帧 | 备注 |
|---|---:|---:|---:|---|
| `ChaCha20Xor` | **1201** | 3 | **1144B** | `ChaCha20Permute4` 完全内联（4×16 words 栈数组） |
| `AeadSeal` | 626 | 10 | 296B | ChaCha20Block + ChaCha20Xor + Poly1305 |
| `AeadOpen` | 626 | 12 | 328B | 多一次 `ConstantTimeEqual16` |
| `Poly1305`（一次性） | 431 | 5 | 200B | |
| `ChaCha20Permute`（单块） | 254 | 1 | 136B | 20 轮 = 80 个 QuarterRound |
| `HChaCha20` | 205 | 1 | 200B | |
| **`Poly1305Block`** | **143** | **0** | **0B** | 每 16B 块 143 指令 ≈ **8.9 指令/字节** |
| `ChaCha20Block` | 113 | 3 | 144B | |
| `SealFrame` | 93 | 6 | 104B | 帧封装 |
| `ChaCha20InitState` | 42 | 0 | 0B | |

### 4.2 向量化程度：**0 条 SIMD 指令**

```bash
grep -cE '^\s+[0-9a-f]+:\s+v[a-z0-9]+' /tmp/tls_asm/crypto.asm   # → 0
```

- 全文件没有 `vpxor` / `vpaddd` / `vpshufb` / `vpermq` 等 AVX 指令；
- `ChaCha20Permute4` 是**C 层 4 路交错**（`x[4][16]` 数组 + 4 次循环），
  编译器没有做 SLP 向量化（ChaCha 的跨字依赖使自动向量化失效）；
- `Poly1305Block` 是 poly1305-donna 的 5×26-bit limb 标量实现（32×32→64 乘法）。

这就是 §6 中 13.4× 差距的物理根因：对手是 AVX512 手写汇编，我们是标量 C。

---

## 5. vs OpenSSL：`openssl speed` 基线（同机、同 libcrypto 3.0.13）

单位换算为 MB/s（原输出为 kB/s）：

| 算法 | 16B | 64B | 256B | 1KB | 8KB | 16KB |
|---|---:|---:|---:|---:|---:|---:|
| AES-256-GCM | 919.8 | 3237.5 | 7565.0 | 8933.9 | 13959.2 | **14252.1** |
| ChaCha20-Poly1305 | 380.0 | 723.3 | 2454.0 | 4836.0 | 5240.9 | **5298.4** |

AES-GCM 走 AES-NI + PCLMUL（14.2 GB/s）；ChaCha20-Poly1305 走 AVX512（5.3 GB/s）。

---

## 6. vs OpenSSL：自研 AEAD vs `EVP_chacha20_poly1305`（同进程）

命令：`/tmp/tls_bench/bench_aead`（每尺寸 0.35s × 200 次预热，完整 init 口径）

| 尺寸 | 自研 MB/s | OpenSSL EVP MB/s | EVP/自研 | 自研 ns/op | EVP ns/op |
|---:|---:|---:|---:|---:|---:|
| 64B | 220.6 | 114.3 | **0.52×** | 290.1 | 559.7 |
| 256B | 311.5 | 437.9 | 1.41× | 821.9 | 584.7 |
| 1KB | 361.6 | 1483.8 | 4.10× | 2832.0 | 690.1 |
| 4KB | 380.4 | 3232.8 | 8.50× | 10767.0 | 1267.0 |
| 16KB | 379.2 | 4522.2 | 11.93× | 43207.4 | 3623.0 |
| 64KB | 366.7 | 4915.0 | 13.40× | 178701.1 | 13333.9 |
| 1MB | 382.6 | 5128.8 | **13.41×** | 2740890.5 | 204450.1 |

解读：

- **≤64B 自研更快**：EVP 每次 `EncryptInit_ex` 要重建 cipher context（AES/ChaCha
  的 `EVP_CIPHER_CTX` 路径），固定开销 ~300–500ns；自研 `AeadSeal` 是纯函数，
  290ns 就完成。
- **≥256B 起被反超，且差距随尺寸单调拉大**：自研 ≈380 MB/s 封顶（ChaCha20 与
  Poly1305 串联：源码注释实测 ChaCha20 单测 830 MB/s，1/(1/830+1/700)≈380），
  OpenSSL 的 AVX512 实现 5.1 GB/s。
- 结论：**不达标**；而且不是“调参”能解决的差距，需要 AVX2/AVX512 向量化
  （ChaCha 4/8 路 + Poly1305 的 AVX2 乘法），预估可到 1.5–3 GB/s，仍难 <1.1×。

---

## 7. vs OpenSSL：TLS 传输层

> 比值口径（全节统一）：**延迟类**为 `TlsTransport / 裸 OpenSSL`（>1 = 封装更慢）；
> **吞吐类**为 `裸 OpenSSL / TlsTransport`（>1 = 封装吞吐更低，即更慢）。
> 每张表的列名已注明方向。

### 7.1 握手延迟（300 次，每次新建 TCP + TLS）

| 实现 | mean µs | p50 µs | p99 µs |
|---|---:|---:|---:|
| `TlsTransport`（每次新建 `SSL_CTX`） | 1044.9 | 1024.8 | 1743.4 |
| 裸 OpenSSL（共享 `SSL_CTX`） | 976.6 | 939.4 | 1557.3 |
| 裸 OpenSSL（每次新建 `SSL_CTX`） | 1076.4 | 1061.4 | 1621.4 |

- vs 共享 ctx：**1.070×**（p50 1.091×，p99 1.120×）→ <1.1×；
- vs 每次新建 ctx（与 `CreateClient` 同等语义）：**0.971×** → 更快。

差额几乎全部来自 `SSL_CTX_new` + 证书校验配置，不是封装逻辑。
`CreateClient` 的 267 条指令里没有数据拷贝，只有 OpenSSL 调用与参数校验。

### 7.2 写吞吐（32KB × 8192 = 256MB）

| 阶段 | `TlsTransport` MB/s | 裸 OpenSSL MB/s | 裸/封装（>1 = 封装更慢） |
|---|---:|---:|---:|
| 优化前 | 2057.3 | 2307.7 | 1.122× |
| 仅 `failed_` 快路径 | 2148.0 | 2334.5 | 1.087× |
| **最终（MSG_NOSIGNAL BIO）** | 2360 / 2309 / 2329 | 2320 / 2154 / 2349 | **0.983 / 0.933 / 1.008×** |

### 7.3 小包写延迟（64B）

| 阶段 | `TlsTransport` ns/op | 裸 OpenSSL ns/op | 封装/裸（>1 = 封装更慢） |
|---|---:|---:|---:|
| 优化前（单轮） | 2886.8 | 2120.8 | 1.361× |
| 优化前（交替 4 轮） | 2276–2396 | 1732–1855 | 1.288–1.347× |
| 仅 `failed_` 快路径（4 轮） | 1838–1923（SIG_IGN） | 1700–1792 | 1.035–1.131× |
| **最终（4 轮）** | 1719–2162 | 1756–2198 | **0.946–1.064×** |

### 7.4 读吞吐（32KB × 8192 = 256MB）

| 轮次 | `TlsTransport` MB/s | 裸 OpenSSL MB/s | 裸/封装（>1 = 封装更慢） |
|---|---:|---:|---:|
| 1 | 2727.0 | 2597.8 | 0.953× |
| 2 | 2296.8 | 2490.9 | 1.085× |
| 3 | 2591.6 | 2490.4 | 0.961× |
| 4 | 2317.1 | 2432.4 | 1.050× |

均值 ≈1.01×；波动来自本地回环 + 单 worker 调度，读数本身没有额外拷贝。

---

## 8. 正确性

### 8.1 自研 AEAD（`/tmp/tls_bench/bench_aead`）

```
[rfc8439] tag_match=1
[rfc8439] open_ok=1 plain_match=1
[1e6x64B] seal_mismatch=0 tag_mismatch=0 own_open_fail=0 evp_open_own_ct_fail=0
[1000x16KB] mismatch=0
```

- RFC 8439 §2.8.2 官方向量：tag 逐字节一致；
- **1,000,000 次 × 64B**：自研 `AeadSeal` 与 OpenSSL `EVP_EncryptUpdate` 的
  密文 + tag 逐字节一致（0 差异）；自研 `AeadOpen` 回读明文一致；
  OpenSSL 解自研密文也一致（双向交叉验证）；
- 1000 次 × 16KB 同样零差异（大包路径）。

### 8.2 TLS 握手 / ALPN / 应用数据

`BUILD_DIR=/tmp/build-tls scripts/verify_tls_alpn.sh`（`openssl s_server` 为对端）：

```
[tls-alpn] TLS 版本: TLSv1.3
[tls-alpn] ALPN 协商结果: 'h2'
[tls-alpn] TLS 上读到 512 字节
[tls-alpn] failures=0
```

覆盖：新建连接 → TLS 1.3 握手 → ALPN 选 `h2` → TLS 上写 HTTP 请求 → 读响应。
另外 bench 的 source 模式覆盖了“server 先 `SSL_shutdown`+close、client 再
`Close()` 发 close_notify”的异常路径，进程未触发 SIGPIPE（这正是旧实现最危险的
路径）。

---

## 9. 优化与证据链（边界内，只动 `tls_transport.{hpp,cpp}`）

### 9.1 问题定位（objdump 先发现）

`Write` 的 15 个 call 里 7 个是信号相关（`pthread_sigmask`×2、`sigemptyset`、
`sigaddset`、`sigpending`、`sigismember`、`sigtimedwait`），且**成功路径也全部执行**。
64B 小包总耗时 ~2.9µs，其中约 0.7–0.9µs 是这 4 次信号系统调用（WSL2 下
`rt_sigprocmask` ≈190ns/次）。

### 9.2 三步优化

1. **成功路径跳过 pending 检查**：只有 `SSL_*` 返回失败才可能留下 pending
   SIGPIPE（成功的 `write` 不产生信号），加 `MarkFailure()`，成功路径省
   `sigpending` + `sigtimedwait` 2 次系统调用。
2. **进程已 `SIG_IGN` SIGPIPE 时整体跳过**：`sigaction` 查询一次并缓存；
   内核不会为失败的 write 生成信号，屏蔽与消费都是纯开销。
3. **自定义 socket BIO（根治）**：写走 `send(fd, buf, len, MSG_NOSIGNAL)`，
   读走 `recv`，实现 `BIO_meth_*` 的 write/read/ctrl/create/destroy；
   `SSL_set_bio` 挂载。失败自动回退 `SSL_set_fd`（仍由 1、2 兜底）。
   这样 **SSL 写路径从源头不产生 SIGPIPE**，错误语义不变
   （`EPIPE` → `SSL_ERROR_SYSCALL`）。

### 9.3 收益

| 指标 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| 64B 写 ns/op | 2886.8 | 1719–2162 | **−25% ~ −40%** |
| 64B 写 vs 裸 OpenSSL | 1.29–1.36× | 0.95–1.06× | 达标 |
| 32KB 写 vs 裸 OpenSSL | 1.122× | 0.93–1.01× | 达标 |
| TLS 写信号 syscall（20 万次） | ≥80 万（理论） | 15（实测） | **−4 个数量级** |

### 9.4 风险与回归

- `MSG_NOSIGNAL` 是 Linux 专有；`tls_transport.cpp` 本身已用 `pthread_sigmask`/
  `poll`/`fcntl`，已是 Linux-only，无新增平台约束。
- `BIO_CTRL_GET_CLOSE` 返回 0：fd 仍由 `Socket` 拥有，BIO 不关闭，所有权不变。
- 回退路径保留：`AttachNoSignalBio` 失败（`BIO_meth_new`/`BIO_new` 返回 null）时
  走原 `SSL_set_fd`，行为与旧实现完全一致。
- 回归：`verify_tls_alpn.sh` 全绿；bench 的握手/读/写/source 模式全通过；
  进程在“对端先关闭”路径未触发 SIGPIPE。

---

## 10. 是否 <1.1× 的判定

| 对象 | 是否 <1.1× | 依据 |
|---|---|---|
| TLS 握手（共享 ctx） | ✅ 1.070× | §7.1 |
| TLS 握手（同等语义，新 ctx） | ✅ 0.971× | §7.1 |
| TLS 32KB 写 | ✅ 0.933–1.008× | §7.2 |
| TLS 64B 写 | ✅ 0.946–1.064× | §7.3 |
| TLS 32KB 读 | ✅ 0.953–1.085× | §7.4 |
| 自研 ChaCha20-Poly1305（≥256B） | ❌ 1.41–13.41× | §6 |
| 自研 ChaCha20-Poly1305（≤64B） | ✅ 0.52× | §6 |

**TLS 模块本身（对 OpenSSL 的传输层封装）全部 <1.1×**；
自研 AEAD 的大包吞吐不达标，且属于 `src/bento/net/crypto.cpp`（边界外，本次未改）。

---

## 11. 剩余差距与建议

1. **自研 ChaCha20-Poly1305**：唯一出路是向量化。
   - ChaCha20：AVX2 下 4 路 × 8 blocks 交错（寄存器 16×`__m256i`），
     预期 1.5–2.5 GB/s；
   - Poly1305：AVX2 的 26-bit limb 并行乘法（2/4 路），预期 2–4 GB/s；
   - 即便如此仍难 <1.1×（OpenSSL 是 AVX512 + 手写汇编），除非只在小包场景比。
2. **TLS 握手**：若要进一步降，可在 `CreateClient` 复用进程级 `SSL_CTX`
   （会话复用/证书缓存），但这会改变“每次连接独立上下文”的安全假设，需产品决策。
3. **`Write` 栈帧 600B**：主要来自兜底的 `sigset_t`。若确认 `MSG_NOSIGNAL` BIO
   永不失败，可把兜底路径拆成独立函数（`noinline`），把热函数栈帧降到 ~200B；
   当前收益仅为栈占用，未做。
4. **`ERR_clear_error()` 每次读/写**：OpenSSL 无错时开销 ~2–5ns，保留（有错时
   防止错误队列污染，是正确性需要）。

---

## 12. 复现命令

```bash
# 构建
cmake -S /UserData/CodexWorkSpace/Go2cpp -B /tmp/build-tls \
  -DCMAKE_BUILD_TYPE=Release -DGO2CPP_WITH_BENTO=ON -DGO2CPP_BUILD_TESTS=ON
cmake --build /tmp/build-tls --target go2cpp_network go2cpp_bento go2cpp_tls_alpn_test -j12

# objdump
objdump -dr --no-show-raw-insn -C \
  /tmp/build-tls/CMakeFiles/go2cpp_network.dir/src/network/http2/tls_transport.cpp.o
python3 /tmp/tls_asm/analyze3.py /tmp/tls_asm/tls_transport_final_dr.asm TlsTransport 30

# OpenSSL 基线
openssl speed -evp aes-256-gcm -seconds 2
openssl speed -evp chacha20-poly1305 -seconds 2

# 自研 AEAD 对拍/吞吐
g++ -O3 -DNDEBUG -std=c++17 -fno-plt -I/UserData/CodexWorkSpace/Go2cpp/include \
  /tmp/tls_bench/bench_aead.cpp \
  /UserData/CodexWorkSpace/Go2cpp/src/bento/net/crypto.cpp \
  /UserData/CodexWorkSpace/Go2cpp/src/bento/net/protocol.cpp -lcrypto -o /tmp/tls_bench/bench_aead
/tmp/tls_bench/bench_aead

# TLS 传输层对标（需先链接 bench_tls，见 /tmp/tls_bench）
/tmp/tls_bench/bench_tls default   # 握手/写/读/小包
/tmp/tls_bench/bench_tls small     # 小包 4 轮
/tmp/tls_bench/bench_tls write     # 32KB 写 3 轮

# 系统调用计数
strace -c -f -e trace=rt_sigprocmask,rt_sigpending,rt_sigtimedwait,rt_sigaction \
  -o /tmp/tls_asm/strace_small.txt /tmp/tls_bench/bench_tls small

# 正确性
BUILD_DIR=/tmp/build-tls bash scripts/verify_tls_alpn.sh 18443
```

---

## 13. 产物索引

| 内容 | 路径 |
|---|---|
| 优化前反汇编 | `/tmp/tls_asm/tls_transport_dr.asm` |
| 最终反汇编 | `/tmp/tls_asm/tls_transport_final_dr.asm` |
| crypto 反汇编 | `/tmp/tls_asm/crypto_dr.asm` |
| 统计脚本 | `/tmp/tls_asm/analyze3.py` |
| AEAD 对拍源码/结果 | `/tmp/tls_bench/bench_aead.cpp` / `bench_aead` |
| TLS 对标源码/结果 | `/tmp/tls_bench/bench_tls.cpp` / `tls_final_default.txt`、`tls_default.txt`、`tls_ign.txt` |
| strace 结果 | `/tmp/tls_asm/strace_small.txt` |
| 原文件备份 | `/tmp/tls_transport.cpp.orig` |
| 独立构建 | `/tmp/build-tls` |

### 改动清单（未 commit）

```
include/go2cpp/network/http2/tls_transport.hpp |   3 +
src/network/http2/tls_transport.cpp            | 175 ++++++++++++++++--
```

- 头文件：新增私有成员 `bool m_nosignal_bio{false};`
- 实现：`NoSignalBioWrite/Read/Ctrl/Create/Destroy/Method`、`AttachNoSignalBio`、
  `SigpipeGloballyIgnored`、`ScopedSigpipeBlock(bool required)` + `MarkFailure()`、
  `CreateClient` 挂 BIO（失败回退 `SSL_set_fd`）、三处 `ScopedSigpipeBlock`
  传 `!m_nosignal_bio`。

---

## 13. 续（2026-11-04）：ChaCha20 AVX2/AVX-512 + Poly1305 radix-2^44

> 日期：2026-11-04；工作树 `/UserData/CodexWorkSpace/Go2cpp`；机器同 §0（Ryzen 9 8945HX，
> AVX2/AVX-512F/VL/BW/DQ 均可用）。只改 `src/bento/net/crypto.cpp`（+约 500 行，含
> 回退分支），**未 commit**。

### 13.1 13.41× 的确认

原 §6 的 13.41× 是 **1MB** 处 `EVP/自研` 的比值，64KB 处为 13.40×，测量方法为同进程
`EVP_chacha20_poly1305` 完整 init 对拍（§2.2）。本次复测沿用同一口径，结论一致：
**纯标量时自研 AEAD 约 0.4 GB/s 封顶，OpenSSL 5.1–5.4 GB/s，量级差距成立。**

### 13.2 根因（逐条落实）

| # | 根因 | 证据 |
|---|---|---|
| 1 | ChaCha20 未向量化 | 原 `crypto_dr.asm` 0 条 `v*` 指令；`ChaCha20Permute4` 是 C 层 4 路交错，编译器无法做 SLP |
| 2 | Poly1305 是 5×26 位 limb 标量 | `Poly1305Block` 143 指令/16B 块，约 8.9 指令/字节，无延迟隐藏 |
| 3 | 两者串行、吞吐相乘 | 单测 ChaCha 0.83 GB/s、Poly 0.7 GB/s → 1/(1/0.83+1/0.7)=0.38 GB/s，与实测吻合 |
| 4 | 无运行期分派 | 编译期无 `target` 属性，整 TU 不能用 AVX2/AVX-512 |

AES-GCM/AES-NI 与本题无关：仓库内 AEAD 只有 ChaCha20-Poly1305（`src/bento/net/crypto.cpp`），
TLS 握手/记录层由 OpenSSL 提供，没有自研 AES-GCM 路径可加（§1.3/§1.4 已澄清）。

### 13.3 改动

1. **ChaCha20 垂直向量化 + 运行期分派**（`ChaCha20XorAvx512` 16 lane / `ChaCha20XorAvx2` 8 lane）：
   - 布局 vertical（每个 `__m512i` 装 16 个 block 的同一 state word），20 轮全整向量；
   - AVX-512 用 `vprold` 单指令轮转，尾部 16×16 32-bit 转置 64 条；
   - AVX2 用 `vpshufb`（16/8 位）+ shift/or（12/7 位），8×8 转置 24 条；
   - `__attribute__((target("avx512f,avx512dq,avx512bw,avx512vl")))` /
     `target("avx2")`，`__builtin_cpu_supports` 探测 + magic static 缓存；
   - `BENTO_CRYPTO_SIMD=scalar|avx2|avx512` 可强制降级（只下调、不越过硬件）。
2. **Poly1305 换成 radix-2^44 的 3 limb**（`__SIZEOF_INT128__` 时启用，MSVC 回退原
   5×26 实现）：每块 9 次 64×64→128 乘法，进位链 3 段；跨 limb 折回项乘 20
   （2^132 ≡ 20 mod 2^130-5）。`Poly1305Block` 143 → **103 指令**（-28%），
   一次性 `Poly1305` 431 → **368 指令**（-15%）。

### 13.4 前后数据（同机同口径，每尺寸 0.2s，`bench_aead`）

自研 AEAD seal 吞吐（MB/s）与 `EVP/自研` 倍率：

| 尺寸 | 原（全标量） | 新 scalar | 新 AVX2 | 新 AVX-512 | 原倍率 | 新倍率(AVX-512) |
|---:|---:|---:|---:|---:|---:|---:|
| 4 KiB | 380.4 | 416.7 | 1485.5 | **1803.2** | 8.50× | **1.80×** |
| 16 KiB | 379.2 | 407.5 | 1631.2 | **2008.1** | 11.93× | **2.40×** |
| 64 KiB | 366.7 | 425.4 | 1653.0 | **1973.6** | 13.40× | **2.65×** |
| 1 MiB | 382.6 | 414.9 | 1499.6 | **1901.7** | 13.41× | **2.85×** |

- 大包（64KiB）**366.7 → 1973.6 MB/s，5.38×**；vs EVP 倍率 **13.40× → 2.65×**。
- ChaCha20 单测：0.83 → **7.88 GB/s**（AVX-512，64KiB）。
- Poly1305 单测：约 2.4 → **3.0 GB/s**（64KiB，radix-2^44）。
- ≤64B 仍领先 EVP（0.48×，即自研更快）。

### 13.5 为什么仍未达 1.1×

当前合成上限：`1/(1/7.88 + 1/3.0) = 2.17 GB/s`，实测 1.97 GB/s（91%）。要 <1.1× 需
≥ 4.7 GB/s，即 Poly1305 需 ≥ 12.6 GB/s（每 16B 块 ≤ 5 周期）。

- 标量 Poly1305 每块 9 次 `mulx` + 3 段进位，实测 21 周期/块，已是单块串行下限；
  **单块串行在任何标量实现上都难突破 ~3–4 GB/s**。
- 唯一出路是 **多块并行**：AVX2 4 路 / AVX-512 8 路 26-bit limb（`vpmuludq`，
  OpenSSL 路线），4 路理论 12 GB/s、8 路 24 GB/s，可把整体推到 4.5–6 GB/s。
  工作量：约 400–600 行 intrinsics + 延迟归约 + 尾部处理，且需独立向量对拍；
  本次 40 分钟时间盒内无法完成并验证，故按铁律给书面瓶颈。

### 13.6 正确性与复现

```bash
# 构建（-Wall -Wextra -Wpedantic，零告警）
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic -fno-plt -Iinclude \
  /mnt/e/tmp/tls_bench2/bench_aead.cpp \
  src/bento/net/crypto.cpp src/bento/net/protocol.cpp -lcrypto -o /tmp/tls_bench2/bench_aead

# 项目单测（RFC 8439 §2.4.2/§2.5.2/§2.8.2 + 帧/重放窗口）
cmake --build build-review --target bento_test_crypto -j12 && ./build-review/bento_test_crypto
# → [PASS] test_crypto (5482 checks)

# 独立对拍：RFC 向量 + 0..2049 全字节长度往返 + 逐字节篡改拒绝
g++ -O2 -std=c++17 -Iinclude /mnt/e/tmp/tls_bench2/check_vectors.cpp \
  src/bento/net/crypto.cpp src/bento/net/protocol.cpp -o /tmp/tls_bench2/check_vectors
/tmp/tls_bench2/check_vectors   # → ALL_VECTORS_OK
```

- `bento_test_crypto`：5482 checks 全过，含 RFC 向量与 AEAD 往返。
- 独立对拍：Poly1305/AEAD 的 RFC 向量、0–2049 字节每个长度的 seal→open 往返、
  每个长度翻一位密文的拒绝，全部通过。
- 无 AVX-512 机器时 `BENTO_CRYPTO_SIMD=avx2` 或 `scalar`，实测仍分别有 3.14×/12.03×
  （64KiB）——分派回退路径已实测，不是纸面分支。

### 13.7 改动清单（未 commit）

```
src/bento/net/crypto.cpp | +约 500 行（SIMD 后端 + radix-2^44 + 分派）
```

## 多路 SIMD Poly1305（2026-11-04）

> 工作树 `/UserData/CodexWorkSpace/Go2cpp`；机器同 §0（Ryzen 9 8945HX，Zen 4，
> AVX2 / AVX-512F/DQ/BW/VL/IFMA 均可用）。只改 `src/bento/net/crypto.cpp`
> （**+981 行 / −7 行**，全部在匿名命名空间内），**未 commit**。

### 1. 结论（先说数）

| 指标 | 改动前（§13 终态） | 改动后 | 变化 |
|---|---:|---:|---:|
| Poly1305 单测 64 KiB | 3.07 GB/s | **9.28 GB/s** | 3.02× |
| Poly1305 单测 1 MiB | 3.12 GB/s | **9.36 GB/s** | 3.00× |
| AEAD seal 64 KiB | 1976.6 MB/s | **4211.2 MB/s** | 2.13× |
| AEAD seal 1 MiB | 1951.1 MB/s | **4376.6 MB/s** | 2.24× |
| vs EVP 倍率 64 KiB | 2.58× | **1.28×** | −50% |
| vs EVP 倍率 1 MiB | 2.83× | **1.29×** | −54% |

- 相对 §13 的起点（366.7 MB/s / 13.40×），大包累计 **11.48×**，倍率 13.40× → **1.28×**。
- **未达 1.1×**：要 ≤1.1× 需总吞吐 ≥4.7 GB/s，即 Poly ≥12.6 GB/s；实测 9.3 GB/s，
  差 1.35×。瓶颈与下一步见 §5。

### 2. 算法（为什么多路能把 3 GB/s 变成 9 GB/s）

串行的 `h ← (h + m)·r` 是强依赖链：标量 radix-2^44 已压到 21 周期/块，再抠指令
也突破不了单块串行链（§13.5）。多路化用的是展开恒等式

```
((h+m0)·r + m1)·r + … + m_{N-1})·r
  = (h+m0)·r^N + m1·r^{N-1} + … + m_{N-1}·r^1
```

右端 N 个部分积**互相独立**：lane k 算 `(h·[k==0] + m_k)·r^{N-k}`，N 个乘法在各自
lane 内并行累加，最后跨 lane 求和并做**一次** 26 位进位链。每 N 块只剩一条串行
进位链，瓶颈从依赖链变成吞吐。

表示换成 **radix 2^26 × 5 limb**（标量路径是 radix 2^44 × 3 limb）：`vpmuludq` 是
32×32→64 的无符号乘，44 位 limb 放不进 32 位操作数；26 位 limb 下 5 项累加上界
`5×5×2^52 < 2^57`，64 位累加器不溢出。两条路径只在批次边界互换 44/26 位表示，
不足 N 块的尾部仍走原来的标量 `Poly1305Block`。

### 3. 改动

1. **`Poly1305BlocksAvx2`（4 路，每批 64B）**：2 条 `vmovdqu` + 2 条 `vpunpck` +
   2 条 `vpermq` 把 4 个块的 lo/hi 转置成 lane 主序；5 个 26 位 limb 用 shift/and/or
   提取；**25 条 `vpmuludq` + 20 条 `vpaddq`** 完成 5×5 卷积（4 个 lane 完全并行）；
   每批一次跨 lane 求和（5 条向量指令/向量）与 5 段 26 位进位链。
2. **`Poly1305BlocksAvx512`（8 路，每批 128B）**：`vpermutex2var_epi64` 两条完成
   8×2 转置；同样 25 条 `vpmuludq` 覆盖 8 个 lane（每块摊销从 6.25 条降到 3.1 条）；
   `_mm512_maskz_set1_epi64` 把累积值只放进 lane 0。
3. **运行期分派**：`Poly1305BlocksSimd` 复用 ChaCha20 已有的 `SimdBackend`
   （`__builtin_cpu_supports` + magic static 缓存）；AVX-512 先按 8 块批吃，
   剩下 4–7 块交 4 路批收尾，再剩下的走标量。`BENTO_CRYPTO_SIMD=scalar|avx2|avx512`
   可强制降级（**已实测**三条路径，不是纸面分支）。
4. **零改动保证**：`Poly1305Ctx`/`Poly1305Finish`/`Poly1305Block` 一行未改；
   多路只在批次边界读写同一个 `h[3]`/`r[3]`，所以 `Poly1305Finish` 的抗恒等
   与 tag 归约逻辑仍然只有一份。
5. `SimdBackend` 枚举与 `ActiveSimdBackend()` 前置声明上移到文件头，供 Poly1305
   早于 ChaCha 段引用；MSVC（无 `__SIZEOF_INT128__` 或非 GCC/Clang）保持 5×26
   标量回退，`BENTO_POLY_MULTI_SIMD` 为 0 时整块代码不参与编译。
6. 零告警：`-O3 -Wall -Wextra -Wpedantic` 下无输出。

### 4. 正确性

```bash
# 项目单测（含 RFC 8439 §2.5.2/§2.8.2 与帧/重放窗口）
cmake --build build-review --target bento_test_crypto -j12 && ./build-review/bento_test_crypto
# → [PASS] test_crypto (5482 checks)；[INFO] aead throughput: seal=3953.8MB/s

# 独立对拍：RFC 向量 + 0..2049 全长度往返 + 逐字节篡改拒绝
/tmp/tls_bench3/cv2              # AVX-512 8 路 → ALL_VECTORS_OK
BENTO_CRYPTO_SIMD=avx2  ...      # AVX2 4 路  → ALL_VECTORS_OK
BENTO_CRYPTO_SIMD=scalar ...     # 标量回退   → ALL_VECTORS_OK
```

对拍过程中抓到并修掉一个真 bug：AVX-512 的水平求和少做最后一步 128 位块内
64 位合并，导致只累加了偶数 lane（160B 的篡改被接受）；补上后三后端全过。

### 5. 为什么停在 9.3 GB/s（书面瓶颈）

- 8 路每批 128B 的理论指令量：约 115 条 512 位指令（其中 25 条乘法）→ Zen 4
  双泵下每批约 29–35 周期 → 每块 3.6–4.4 周期，理论上限约 14–18 GB/s。
- 实测 9.3 GB/s ≈ 每块 6.9 周期（4 GHz 折算），即每批约 55 周期，比指令吞吐
  预算多 20 周期。已定位的两块开销：
  1. **跨 lane 求和是每批的串行尾巴**：5 个 `d` 各要做 3 级折叠（8→4→2→1），
     每级 shuffle 延迟 3–4 周期，5 个并行后仍占约 30 条指令与 12 周期延迟；
  2. **批间 `h` 往返**：折叠结果 → 标量进位链 → `vpbroadcastq` 回 lane 0，
     每批约 10–20 周期纯延迟。
- 已试过把 `h` 与进位链整体留在向量域（`Poly1305Fold8x64` 返回 zmm、批间只做
  `maskz_mov`），结果**更慢**（8.19 GB/s，−12%）：512 位 `vpsrlq/vpandq` 在 Zen 4
  上是双泵、延迟 2–3 周期，6 步串行进位链的延迟反而超过省下的标量往返。
  该版本已回退，不在工作树里。
- 再往上只有两条路（本次时间盒内未做）：
  1. **AVX-512 IFMA（`vpmadd52luq/huq`）做 radix-2^44 的 8 路**：每块 9 对乘加，
     8 路每批只需 18 条乘加指令（对比现在 25 条 mul + 20 条 add），且 44 位 limb
     下进位链只有 3 段。预估 18–24 GB/s。工作量约 300–400 行。
  2. **16 路（2 组 zmm）**：每批 256B 摊薄固定开销，理论 +17%，但寄存器压力
     到 28/32 个 zmm，收益不如 IFMA。
- 另一条与 Poly1305 无关的余地：ChaCha20 当前 7.85 GB/s，若与 Poly 同量级提升
  （例如 16 路 + 更省的转置），合成上限还能再抬 20%；但 1.1× 的门槛卡在
  Poly 这一侧。

### 6. 复现

```bash
# 基准（同进程 EVP_chacha20_poly1305 对拍，每尺寸 0.25s，taskset 绑核）
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic -fno-plt -Iinclude \
  /mnt/e/tmp/tls_bench2/bench_aead.cpp src/bento/net/crypto.cpp \
  src/bento/net/protocol.cpp -lcrypto -o /tmp/tls_bench3/bench_v1
taskset -c 3 /tmp/tls_bench3/bench_v1 0.25
BENTO_CRYPTO_SIMD=avx2 taskset -c 4 /tmp/tls_bench3/bench_v1 0.25
```

4 路 vs 8 路单测（同一二进制，仅改环境变量；注意 `BENTO_CRYPTO_SIMD=avx2`
会同时把 ChaCha20 降到 8 lane，所以总吞吐那张表不能直接当 Poly 4 路的收益）：

| 后端 | Poly 64 KiB | Poly 1 MiB | ChaCha 64 KiB |
|---|---:|---:|---:|
| AVX-512 8 路 | **9.28 GB/s** | **9.36 GB/s** | 7.85 GB/s |
| AVX2 4 路 | 7.64 GB/s | 7.23 GB/s | 4.07 GB/s |

### 7. 改动清单（未 commit）

```
src/bento/net/crypto.cpp | +981 −7 行（多路 Poly1305 + 分派 + 前置声明）
docs/reports/asm-tls.md  | 本节
```
