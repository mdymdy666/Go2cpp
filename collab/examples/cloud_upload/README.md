# cloud_upload 示例（JinShan REQ-011：分片 + 断点续传）

本地文件 → 多个分片（大小可配）→ 逐片 HTTP PUT → 失败重试 → 断点续传 →
分片 SHA-256 + 整体清单校验 → 进度回调。只依赖仓库内组件（`http::Client` /
`http::Server`、`hash::Sha256`）与标准库，不引第三方。

## 文件

| 文件 | 说明 |
|---|---|
| `uploader.hpp/.cpp` | `cloud::Uploader`：分片、重试（指数退避）、断点续传、整体校验 |
| `main.cpp` | CLI：`--file <path> --endpoint <url> --chunk-size 8M`，打印进度与结果 |
| `server.cpp` | 测试用模拟服务端：收分片、回已收清单、校验分片/整体 SHA-256 |
| `build.sh` | 编译两个二进制（不改 CMake） |

## 协议（纯文本，见 `server.cpp`）

| 请求 | 语义 |
|---|---|
| `GET /manifest?sha256=<hex>&size=<n>&chunk_size=<n>&chunks=<n>` | 返回 `chunk_size/size/chunks/received=0,1,2` 四行清单 |
| `PUT /upload?sha256=<hex>&index=<i>&size=<n>&chunk_size=<n>&chunks=<n>` | body 为分片字节；头 `X-Chunk-SHA256` 为分片摘要 |
| `POST /finalize?sha256=<hex>&size=<n>&chunk_size=<n>&chunks=<n>` | 服务端按序重算整文件 SHA-256 并比对，返回 `OK <sha> <size> <chunks>` |

全文件 SHA-256 是内容身份：服务端按它分目录（`<root>/<sha>/<index>.part`）存分片，
因此同一文件重复上传幂等，重跑即续传。分片先写 `.tmp` 再 rename，避免半截文件
被清单误认为已完成。

## 编译

```bash
cd /UserData/CodexWorkSpace/Go2cpp
collab/examples/cloud_upload/build.sh
# 产物：/tmp/go2cpp_ex_cloud_upload/cloud_up、cloud_server
```

脚本等价命令（`$ROOT` 为仓库根，库目录为 `.build-bento-wsl`）：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude -pthread \
  collab/examples/cloud_upload/uploader.cpp collab/examples/cloud_upload/main.cpp \
  -o /tmp/go2cpp_ex_cloud_upload/cloud_up \
  -Wl,-rpath,$PWD/.build-bento-wsl -L$PWD/.build-bento-wsl \
  -lgo2cpp_network -lgo2cpp_hash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude -pthread \
  collab/examples/cloud_upload/server.cpp -o /tmp/go2cpp_ex_cloud_upload/cloud_server \
  -Wl,-rpath,$PWD/.build-bento-wsl -L$PWD/.build-bento-wsl \
  -lgo2cpp_network -lgo2cpp_hash
```

## 运行

终端 A（模拟服务端）：

```bash
/tmp/go2cpp_ex_cloud_upload/cloud_server --root /tmp/upload_store
# 可选：--fail-once 3（前 3 个分片 PUT 返回 500）
# 可选：--delay-ms 40（每片 PUT 前延迟，方便演示中途 kill）
# 可选：--port 29301
```

终端 B（上传）：

```bash
dd if=/dev/urandom of=/tmp/up16.bin bs=1M count=16
sha256sum /tmp/up16.bin
/tmp/go2cpp_ex_cloud_upload/cloud_up \
  --file /tmp/up16.bin --endpoint http://127.0.0.1:29300 --chunk-size 4M
```

成功时客户端退出码 0，输出 `[result] OK` 与本地 SHA-256；服务端日志的
`POST /finalize ... computed=<sha>` 与本地 `sha256sum` 相同。

## 验收步骤（本机 WSL2 实测）

### 1) 正常上传 + SHA 对比（16 MiB / 4 MiB 分片）

```
[result] OK
[result] size=16777216 chunks=4 uploaded=4 resumed=0 retries=0 sha256=43a6131a...
[server] POST /finalize sha=43a6131a... computed=43a6131a... -> 200
43a6131a37ea3c9bea26f2740ec3fcdfecaedbaa39072b23856490c4726777f8  /tmp/up16.bin
```

### 2) 断点续传（64 MiB / 1 MiB 分片，中途 timeout 打断）

```bash
# 服务端加 --delay-ms 40 便于打断
timeout 1.2 /tmp/go2cpp_ex_cloud_upload/cloud_up \
  --file /tmp/up64.bin --endpoint http://127.0.0.1:29300 --chunk-size 1M
ls /tmp/up_store_resume/*/*.part | wc -l   # 24
# 重跑同一条命令（去掉 timeout）
```

第二次运行输出：

```
[uploader] 服务端已收 25/64 分片（25.00 MiB 已就绪），跳过这些分片继续
[progress] 26214400/67108864 bytes (39%)   # 从 25 MiB 处起跳，不是 0
[result] OK ... uploaded=39 resumed=25
[server] POST /finalize ... computed=f7390287... -> 200
```

（第一次被打断时服务端仍在写第 25 片，所以清单是 25 而不是 kill 瞬间的 24。）

### 3) 失败重试（服务端 `--fail-once 3`）

```
[server] PUT /upload index=0 注入 500（剩余注入次数 2）
[uploader] 上传分片 第 1/5 次重试：HTTP 500 ERR injected failure（等待 100 ms）
[uploader] 上传分片 第 2/5 次重试：HTTP 500 ...（等待 200 ms）
[uploader] 上传分片 第 3/5 次重试：HTTP 500 ...（等待 400 ms）
[result] OK ... retries=3
```

### 4) 整体校验能发现被篡改分片

把已收的第 2 片覆盖成同样长度的随机数据后重跑：

```
[result] FAIL
[result] error=整体校验失败：HTTP 400 ERR 整体 SHA-256 不符：声明 43a6131a...，实际 36f8f035...
```

删掉损坏的 `2.part` 再跑：`resumed=3 uploaded=1` → `[result] OK`。

### 5) 1 GiB 不 OOM（1 GiB 稀疏文件 / 8 MiB 分片）

```bash
truncate -s 1G /tmp/up1g.bin
/usr/bin/time -v /tmp/go2cpp_ex_cloud_upload/cloud_up \
  --file /tmp/up1g.bin --endpoint http://127.0.0.1:29300 --chunk-size 8M
```

```
[result] OK size=1073741824 chunks=128 uploaded=128 seconds=6.339
Maximum resident set size (kbytes): 48028
[server] POST /finalize sha=49bc20df... computed=49bc20df... -> 200
```

内存上界 = 一个分片缓冲 + 一次请求体拷贝 + SHA-256 上下文，与文件大小无关：
1 GiB 文件峰值 48 MB（8 MiB 分片）。

## 限制与说明

- 只支持 `http://`，未接 TLS；真实云端需在 `Perform()` 处换成 HTTPS 客户端或加隧道。
- 续传信任服务端返回的清单；落到磁盘后被篡改的分片由 `finalize` 的整文件
  SHA-256 兜底，发现后需删除坏分片再重跑（示例未自动定位坏片）。
- `cloud_up` 只用到标准库 IO 与仓库 HTTP 客户端，代码本身不依赖 POSIX；
  但 Go2Cpp `socket` 层的 Windows 适配尚未在本示例验证，实测环境为 WSL2 Ubuntu。
  `cloud_server` 是测试桩，用 `net::ListenTCP` + `http::Server`，仅在 WSL2 实测。
- 服务端单线程 accept、单连接串行处理，只用于验收，不做并发/鉴权/配额。
