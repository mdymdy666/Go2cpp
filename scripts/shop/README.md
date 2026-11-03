# scripts/shop —— 电商 demo 的本机守护与探针

> 从 `/tmp/shop` 固化而来（2026-11-03，B2/B3）。**脚本本身在仓库**，demo 源码仍在 `/tmp/shop`（重启会丢，源码级备份见 `E:\tmp\shop`）。

## 文件

| 文件 | 行数 | 作用 |
|---|---:|---|
| `start-all.sh` | 166 | 幂等启动：product(9101) → review(9102) → order(9103) → gateway(8080) → tunnel-client；6 项健康检查（5 本机 + 1 公网），任一非 200 退出非 0 |
| `stop-all.sh` | 47 | 只用 `pkill -x`（gateway/product_service/review_service/order_service/tunnel-client），等待退出并复核端口 |
| `probe.sh` | 69 | 每 60s curl 公网 `/shop/api/health`；失败写 `logs/probe.log`（时间戳 + code + 连续失败数），200 清零；支持 `--once` / `--minutes N` |

## 用法

```bash
# 默认 ROOT=/tmp/shop；也可用环境变量覆盖全部参数
ROOT=/tmp/shop ./scripts/shop/start-all.sh
./scripts/shop/stop-all.sh
./scripts/shop/probe.sh --once
./scripts/shop/probe.sh --minutes 30
```

可覆盖变量：`ROOT`、`SHOP_PORT`、`PRODUCT_PORT`、`REVIEW_PORT`、`ORDER_PORT`、`TUNNEL_BIN`、`TUNNEL_SERVER`、`TUNNEL_SUBDOMAIN`、`TUNNEL_TOKEN`、`PUBLIC_URL`。

## 实测（2026-11-03）

- 全停 → 全起：5 项本机 + 公网 health 全 200（exit 0）；
- 幂等：第二次启动全部打印「已在运行 pid=…，跳过启动」，进程数仍各 1；
- 探针 5 分钟 5 次全 200（`probe.log` 未生成）。

## 注意

- **禁止 `pkill -f`**（会匹配到自己的 shell）；脚本已统一 `pkill -x`。
- 公网排查：网关没有 `/__tunnel__/status`（那是 tunnel-server 的），异常时先看本机 `/api/health`，再看 `logs/tunnel-client.log` 尾部与 `http://<vps>:8443/__tunnel__/status`。
- `/tmp/shop` 属易失目录（已发生过一次全丢）；`start-all.sh` 前置检查会报「找不到 build/gateway」并退出。
