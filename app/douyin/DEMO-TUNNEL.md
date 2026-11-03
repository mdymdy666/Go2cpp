# 公网穿透演示（3 分钟，用你的虚拟机验证）

> 目的：直观证明 `http://82.157.147.127:8443/douyin/` 的流量**确实经过 VPS 隧道**，
> 而不是本机自访问。你需要一台**虚拟机（或另一台能上公网的设备）**。

## 一、准备

- 虚拟机：能打开浏览器、能访问公网（NAT/桥接都行）；
- 本机（WSL）：抖音 7 进程 + `tunnel-client` 正在运行（`serve_public.sh` 启动）。

## 二、对照实验（4 步）

| 步骤 | 操作 | 虚拟机里预期看到 |
|---|---|---|
| 1 | 虚拟机浏览器打开 `http://82.157.147.127:8443/douyin/` | 正常页面（能刷 feed、播视频） |
| 2 | **本机**停隧道：`bash app/douyin/scripts/stop_public.sh`（或只 `pkill -x tunnel-client`） | 刷新 → **502 Bad Gateway / 无法连接** |
| 3 | 同时在本机访问 `http://127.0.0.1:28080/api/feed?limit=1` | **200**（本地服务本身正常） |
| 4 | 本机重启隧道：`bash app/douyin/scripts/serve_public.sh` | 刷新 → **恢复 200**，页面/视频正常 |

第 2 步是核心：**公网断了、本机还通** → 证明公网访问依赖隧道，不是本地直连。

## 三、旁证（VPS 侧）

浏览器或 curl 打开：

```bash
curl http://82.157.147.127:8443/__tunnel__/status
```

会看到：

```json
{"tunnels":[{
  "subdomain":"douyin",
  "peer":"<你本机的公网 IP>:<端口>",   ← 本机主动连出到 VPS
  "local":"127.0.0.1:28080",          ← 隧道落地到本机
  "bytes_in":..., "bytes_out":...,    ← 随访问增长
  "online_ms":...
}]}
```

- `peer` 是你家宽带的公网 IP（不是 127.0.0.1）；
- `local` 是 `127.0.0.1:28080`（本机 gateway）；
- 每次刷新页面，`bytes_in/bytes_out` 都会增长。

再在 VPS 上看 TCP 连接（本机 → VPS）：

```bash
ssh ubuntu@82.157.147.127 'ss -tn state established "( sport = :7443 )"'
# 10.2.0.16:7443  ←  <你本机公网 IP>:<端口>
```

## 四、一键脚本

```bash
bash app/douyin/scripts/demo_tunnel.sh
```

脚本会自动跑完「公网 200 → 停 client → 公网 502 + 本机 200 → 起 client → 公网 200」并打印每步结果，
你可以把它的输出贴给任何人看。

## 五、常见疑问

| 疑问 | 回答 |
|---|---|
| 为什么不是 https | 自研 tunnel 只做 TCP 转发，没有 TLS；用 IP 访问无法签证书。需要 HTTPS 要加域名 + Caddy/nginx 终结 |
| 虚拟机也在同一台电脑上，算不算公网 | 流量出本机 → 到腾讯云 VPS（82.157.147.127）→ 再回你本机；跳过 VPS 就不通（第 2 步验证） |
| 本机有没有公网 IP | 没有（NAT 后面），所以必须 client 主动连出 VPS |
