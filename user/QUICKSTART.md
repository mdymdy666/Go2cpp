# 快速入门：从零到跑通第一个例子

> 目标：**10 分钟内**装好依赖、构建完成、跑通示例，并能自己改一个例子。
> 全流程在 Ubuntu 24.04 + GCC 13 上实测：`-DGO2CPP_WITH_BENTO=ON` 下全量
> 947 个构建目标，退出码 0。

## 0. 前置要求

| 项 | 要求 | 检查命令 |
| --- | --- | --- |
| 系统 | **Linux**（IO 层用 epoll；WSL2 可用） | `uname -s` |
| 编译器 | GCC ≥ 13 或 Clang ≥ 16（C++17） | `g++ --version` |
| CMake | ≥ 3.20 | `cmake --version` |
| 构建器 | Ninja（推荐）或 Make | `ninja --version` |

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build
```

不需要装 Boost/abseil/spdlog —— 运行时零第三方依赖（`third_party/` 里只有按需
内置的头文件库，如 nlohmann/json）。

## 1. 构建

在**仓库根目录**执行：

```sh
cmake -S . -B build -G Ninja -DGO2CPP_WITH_BENTO=ON
cmake --build build -j"$(nproc)"
```

- `-DGO2CPP_WITH_BENTO=ON` 打开自研 UDP 传输协议，`user/modules/app/` 里的 web/chat
  服务示例依赖它；
- 示例默认就会构建（`GO2CPP_BUILD_EXAMPLES=ON`），测试也默认构建
  （`GO2CPP_BUILD_TESTS=ON`）；
- 想加速只构建示例：见下面的「3. 常用命令」。

首次全量构建几分钟（本机 12 核实测 < 5 分钟；核数越少越久）。

## 2. 跑第一个例子

```sh
./build/go2cpp_beginner_demo
```

预期输出（节选）：

```
[beginner][场景1] 显式 Scheduler + go() + Channel
[beginner][场景1] Fiber 已在 Scheduler 中执行
收到: 2026
[beginner][场景2] Select 超时：没有发送者时应及时返回
...
```

**退出码为 0 表示示例自身的断言全部通过**（`echo $?` 检查）。

接下来按 [README.md](README.md#学习路径) 的学习路径继续；或者直接跑本模块的一组：

```sh
ctest --test-dir build -R 'go2cpp_(beginner|channel|context|sync)_' --output-on-failure
```

## 3. 改一个例子

以 `user/modules/concurrency/beginner_demo.cpp` 为例：

1. 打开文件，把第 30 行的输出改掉：

   ```cpp
   std::cout << "收到: " << *value << '\n';
   // 改成
   std::cout << "收到: " << *value << "（我改过了）\n";
   ```

2. 只重编译这一个目标（秒级）：

   ```sh
   cmake --build build --target go2cpp_beginner_demo -j
   ```

3. 再跑：

   ```sh
   ./build/go2cpp_beginner_demo
   ```

4. 用 ctest 单跑它（等价于「示例即测试」）：

   ```sh
   ctest --test-dir build -R go2cpp_beginner_demo --output-on-failure
   ```

## 4. 常用命令

```sh
# 只构建某个示例（产物固定为 build/go2cpp_<文件名>）
cmake --build build --target go2cpp_http_demo -j

# 列出全部示例测试
ctest --test-dir build -N | grep go2cpp_

# 只跑示例类测试（示例名都以 go2cpp_ 开头）
ctest --test-dir build -R '^go2cpp_' --output-on-failure

# 跑全部测试（较慢）
ctest --test-dir build -j"$(nproc)" --output-on-failure

# 跑 app 的服务型示例（不注册 ctest，需要自己管端口）
./build/user/modules/app/go2cpp_cache_demo     # 无端口，直接跑
./build/user/modules/app/go2cpp_stream_demo    # 需要 -DGO2CPP_WITH_APP=ON
./build/user/modules/app/go2cpp_web_server_simple 28080
curl -s http://127.0.0.1:28080/health  # {"status":"ok"}

# 换一个干净的构建目录（推荐用于验证「从零构建」）
cmake -S . -B /tmp/build-check -G Ninja -DGO2CPP_WITH_BENTO=ON
```

## 5. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `bind(127.0.0.1:8080) 失败` | 端口被占用。给示例传别的端口：`./go2cpp_web_server_simple 28080` |
| `go2cpp_stream_demo` 不存在 | 它是 app 层示例，需要 `-DGO2CPP_WITH_APP=ON` 后重新配置构建 |
| `user/app: web_server 示例跳过` | Bento 没开。加 `-DGO2CPP_WITH_BENTO=ON` |
| 示例跑起来卡住 | 大多数示例都有看门狗超时；服务型示例要发信号退出（`SIGTERM`） |
| 日志看不到输出 | 默认只把 warning/error 写 `log/go2cpp.log`，stdout 要显式打开 |
| 改了头文件后编译很慢 | `tools/`、`channel.hpp` 是 header-only，会触发大量重编译；用 `-j` 和 `ccache` |
| WSL 里 `find`/`ls` 行为怪异 | PATH 里混进了 Windows 工具，用绝对路径或 `command find` |

## 6. 下一步

- 想理解某个模块：读对应目录的 `README.md`（从 [README.md](README.md) 的目录地图进入）；
- 想自己加一个示例：在 `user/modules/<模块>/` 下加 `xxx_demo.cpp`，在
  `cmake/Go2CppTests.cmake` 的 `GO2CPP_EXAMPLES` 里加一行 `<模块>/xxx_demo`，
  再在该模块 README 的示例表里登记；
- 想看设计动机：`docs/` 下有各模块的专题文档（见 [README.md](README.md#文档索引)）。
