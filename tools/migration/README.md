# tools/migration —— 可迁移协程 demo（v3：safepoint id + 句柄 + 签名）

验证 Go2Cpp 的协程上下文能否序列化后**跨进程恢复**。v3 在 v2「地址符号化」
之上补了四件事：

1. **safepoint id 取代指令偏移**：用户函数里的挂起点用编译期编号表达，恢复时
   按 id 查当前镜像的锚点 —— 函数内指令布局变化不影响恢复；
2. **lambda / 静态函数注册**：`MIGRATION_SYMBOL_FN(fn)` 用 `dladdr` + ELF
   `.symtab` + `__cxa_demangle` 生成可读的稳定名；
3. **可迁移堆对象**：显式登记的可序列化对象（内置 `std::string`，自定义需
   `MigrationSerialize/Deserialize`）转成句柄，随栈迁移；
4. **强制签名**：HMAC-SHA256，密钥来自环境变量或文件；未签名/签名错误拒绝。

完整分析与 adopt API 设计见 [docs/fiber-migration.md](../../docs/fiber-migration.md)。

## 构建

独立 CMake 工程，不并入主构建（主工程 `ctest` 不受影响）：

```bash
cmake -S tools/migration -B build-migration
cmake --build build-migration -j
```

关键编译选项（`CMakeLists.txt` 已设置）：

| 选项 | 原因 |
|------|------|
| `-fPIE -pie` | 允许 ASLR —— 这正是要验证的场景 |
| `-rdynamic` | 主程序符号进入动态符号表，供 `dladdr`/`dlsym` 兜底解析 |
| `-fno-omit-frame-pointer` | rbp 链完整；`ChannelRecvAt` 用 `__builtin_frame_address(0)+8` 记录返回地址槽 |
| `-fno-optimize-sibling-calls` | 避免尾调用把 `ChannelRecvAt` 的帧消掉 |
| `-fno-stack-protector` | 挂起帧的 canary 来自生成进程，跨进程必然不匹配 |
| `-fno-ipa-cp/-sra/-icf`（lambda demo） | 禁止 `.constprop/.isra` 克隆，否则栈上的实际函数与注册地址不一致 |

## 运行（两个进程，同一个二进制）

先生成签名密钥（**必需**，否则 dump 直接拒绝）：

```bash
export MIGRATION_SIGNING_KEY="hex:$(head -c 32 /dev/urandom | xxd -p -c 64)"
# 或：export MIGRATION_SIGNING_KEY_FILE=/etc/go2cpp/migration.key
```

```bash
# 进程 A：启动 Fiber，阻塞在通道接收上，把栈 + 符号化上下文 + safepoint id 写盘
./build-migration/migrate dump /tmp/fiber.mig

# 进程 B：校验签名/符号/挂起点，重建栈与控制块，发送两条消息
./build-migration/migrate restore /tmp/fiber.mig

# 只读校验（不执行）
./build-migration/migrate verify /tmp/fiber.mig

# 密码学自检（SHA-256 / HMAC 测试向量）
./build-migration/migrate selftest
```

### safepoint：布局变化后恢复

`demo_body_sp_v1.cpp` / `demo_body_sp_v2.cpp` 语义相同，但 v2 在挂起点前后
插入了不同长度的无关代码。两版的指令偏移不同、栈帧布局（槽位）相同：

```bash
MIGRATION_DEMO_SO=$PWD/build-migration/libmigration_demo_sp_v1.so \
  ./build-migration/migrate dump /tmp/fiber_sp.mig

MIGRATION_DEMO_SO=$PWD/build-migration/libmigration_demo_sp_v2.so \
  ./build-migration/migrate restore /tmp/fiber_sp.mig
# [B] 挂起点 id=0x20001：旧 migration_demo_body+0x19a → 新 migration_demo_body+0x1e1
# [B] fiber#1 [sp-v2] 栈 payload 校验通过（8355840），句柄校验通过：label="跨进程标签" payload=heap-payload
```

### lambda 主体

```bash
MIGRATION_DEMO_SO=$PWD/build-migration/libmigration_demo_lambda.so \
  ./build-migration/migrate dump /tmp/fiber_lambda.mig
MIGRATION_DEMO_SO=$PWD/build-migration/libmigration_demo_lambda.so \
  ./build-migration/migrate restore /tmp/fiber_lambda.mig
```

`verify` 会显示 demangled 的 lambda 符号（`kind=3`）：

```
sym[1] (anonymous namespace)::g_lambda_body::{lambda(FiberCtl*)#1}::operator()(...) kind=3
```

### 句柄（堆对象）

demo 主体挂起前登记两个对象：

```cpp
auto* label = new std::string("跨进程标签");
auto* heap  = new DemoPayload{...};
MIGRATION_MIGRATE_OBJECT(label, "std::string");
MIGRATION_MIGRATE_OBJECT(heap, "DemoPayload");
```

恢复后两个对象被重建并逐字段校验；**未登记**的堆指针仍直接拒绝：

```
栈上存在未登记的堆指针 0x...：v3 只迁移显式登记的可序列化对象
```

### 验证 SO 版本替换（v2 场景保留）

`MIGRATION_DEMO_SO` 指定 Fiber 主体所在的 SO（导出 `migration_demo_body`）。
dump 与 restore 可以指向不同版本：

```bash
MIGRATION_DEMO_SO=$PWD/build-migration/libmigration_demo_v1.so \
  ./build-migration/migrate dump /tmp/fiber_so.mig
MIGRATION_DEMO_SO=$PWD/build-migration/libmigration_demo_v2.so \
  ./build-migration/migrate restore /tmp/fiber_so.mig
```

### 环境变量

| 变量 | 用途 |
|------|------|
| `MIGRATION_SIGNING_KEY` | 签名密钥（`hex:` 前缀或原始字节） |
| `MIGRATION_SIGNING_KEY_FILE` | 密钥文件（去掉行尾换行；64 位十六进制按 hex 解） |
| `MIGRATION_INSECURE_NO_SIGNATURE=1` | **仅测试**：强制生成未签名快照 |
| `MIGRATION_DEMO_SO` | Fiber 主体 SO |
| `MIGRATION_DEMO_UNREGISTERED_HEAP=1` | 负例：故意不登记堆对象 |
| `MIGRATION_DEMO_EXTRA_SYMBOL=1` | 负例：恢复侧多注册一个符号，演示符号表差异 |
| `MIGRATION_MODE` / `MIGRATION_FILE` / `MIGRATION_MSG1` / `MIGRATION_MSG2` | CLI 包装 |

## 库接口

```cpp
#include "migration/runtime.hpp"
#include "migration/handles.hpp"
#include "migration/safepoints.hpp"
#include "migration/symbol_table.hpp"

migration::SetProcessRole("A");
migration::DumpDemoToFile("/tmp/fiber.mig", &error);

migration::SetProcessRole("B");
migration::RestoreDemoFromFile("/tmp/fiber.mig", 4242, 7, &error, &trace);
migration::VerifySnapshotFile("/tmp/fiber.mig", &error, &report);
```

挂起点与句柄 API：

```cpp
std::int64_t value = 0;
MIGRATION_RECV(kSpRecv1, value, channel, self);   // = ChannelRecvAt + MIGRATION_SAFEPOINT
MIGRATION_PARK(kSpPark, self);                    // park，等待 ResumeParkedFiber

auto* p = new std::string("...");
MIGRATION_MIGRATE_OBJECT(p, "std::string");       // 显式登记才可迁移
```

自定义类型：

```cpp
bool MigrationSerialize(const DemoPayload&, std::vector<std::uint8_t>*);
bool MigrationDeserialize(DemoPayload*, const std::uint8_t*, std::size_t);
MIGRATION_SERIALIZABLE("DemoPayload", DemoPayload);
```

## 测试

```bash
bash tools/migration/tests/run_tests.sh build-migration
```

53 项：跨进程恢复、PIE/ASLR、safepoint 布局变化、lambda、句柄、签名正负路径、
符号冲突/缺失/表差异、build-id、CRC/截断/magic、随机消息、20 次随机位翻转、
10 轮压力。

## 目录

```
tools/migration/
  include/migration/format.hpp        快照格式 v3（符号/重定位/safepoint/句柄/签名/清单）
  include/migration/runtime.hpp       可迁移 Fiber/Channel 控制块与 API
  include/migration/symbol_table.hpp  符号注册表（MIGRATION_SYMBOL / MIGRATION_SYMBOL_FN）
  include/migration/safepoints.hpp    挂起点锚点宏与 id 查表
  include/migration/handles.hpp       句柄表与可序列化类型注册
  include/migration/signature.hpp     HMAC-SHA256 与密钥装载
  src/format.cpp                      序列化、CRC、签名、校验
  src/runtime.cpp                     上下文切换、safepoint/句柄重定位、恢复
  src/symbol_table.cpp                符号表实现（demangle/冲突/哈希）
  src/safepoints.cpp                  扫描已加载模块的 migration_safepoints 段
  src/handles.cpp                     类型工厂、对象登记、句柄表
  src/sha256.cpp / src/signature.cpp  SHA-256/HMAC 与密钥管理
  demo_lib/demo_body_v1.cpp           v2 场景：同布局、不同加载地址
  demo_lib/demo_body_v2.cpp
  demo_lib/demo_body_sp_v1.cpp        v3 safepoint + 句柄
  demo_lib/demo_body_sp_v2.cpp        同语义、不同指令布局
  demo_lib/demo_body_lambda.cpp       noinline lambda 主体
  demo_lib/demo_body_unregistered.cpp 负例：匿名静态函数未注册
  tests/symbol_conflict.cpp           负例：同名符号冲突
  migrate_main.cpp                    dump/restore/verify/selftest 入口
  tests/run_tests.sh                  53 项场景测试
```

## 限制

- **栈帧布局必须一致**：safepoint id 解决的是同函数内**指令偏移**；帧大小、
  局部变量槽位变化会让恢复后的栈与代码预期不符。跨版本迁移要求可迁移函数的
  栈帧布局稳定（ABI/编译选项锁定）。
- **禁止 IPA 克隆**：`.constprop/.isra` 会让栈上的实际函数与注册地址不一致，
  可迁移 TU 需要 `-fno-ipa-cp/-sra/-icf`（lambda demo 已设置）。
- 堆对象必须**显式登记**且类型提供序列化；对象内部再有堆指针需自行展开。
- canary 通过 `-fno-stack-protector` 规避，尚未实现序列化 guard 并写回。
- 演示为单线程、无缓冲通道；生产版 `go2cpp::Fiber`/`Channel` 的 adopt API
  仍是设计文档（`docs/fiber-migration.md` §11），未实现。
