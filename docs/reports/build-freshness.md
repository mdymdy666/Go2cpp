# 构建一致性门禁（build freshness）

2026-11-04 两次事故（13 个假 SEGFAULT、Parser 假复现）的根因都不是代码缺陷，而是
**exe 与 .so 不同批**：只重建了部分库，或复用了旧构建目录，测试可执行文件仍链接旧
接口。本门禁把「记得先重建」变成机器校验。仅新增/修改 `scripts/`、`app/douyin/build.sh`。

## 设计（`scripts/check_build_freshness.sh`）

输入：构建目录（默认 `<repo>/.build-bento-wsl`），源码树由脚本位置推导，可用
`--repo/--build-dir` 覆盖。退出码：`0` 新鲜、`1` 需要重建、`2` 用法/环境错误。

| 规则 | 内容 | 依据 |
|---|---|---|
| R0 | 构建目录或关键产物缺失 | 没有任何东西可测 |
| R1 | 最新源码 mtime > 最新关键产物 mtime | 有代码没被编进产物 |
| R2 | `go2cpp_tests` 比它 `readelf -d` 里 NEEDED 的某个 `.so` 旧 | 典型「旧 exe + 新 so」 |

关键产物 = `go2cpp_tests`（可执行）+ `libgo2cpp_*.so*`（共享库，递归 4 层）。人类可读输出
（摘要 +「需要重建」清单，最多 20 条）与 `--json`；`--quiet` 只在失败时打印。

**头文件口径**：`src/**/*.cpp` 全部参与；`include/go2cpp/**/*.hpp` 只比较构建目录 `*.d`
依赖文件里真实出现过的（实测 320/372）。原因：`bytes.hpp`、`sync.hpp`、`core/status.hpp`
等 9 个聚合头是 public 入口，内部没有 TU 包含，touch 后不会产生新产物；若一并参与，
门禁会永久报「需要重建」且重建无法修复。无 `*.d` 时退回全部头文件。

**R2 只检查 exe 真正 NEEDED 的 .so**：否则「只重建了某模块 .so」同样会造成修不好的假阳性。

实现坑：**EXIT trap 的最后一条命令成功会覆盖脚本退出码**（门禁会永远 exit 0），handler
里必须 `trap - EXIT` 后 `exit $rc`；这个 bug 在首轮验证就被抓到并修掉。

## 接入点

1. `scripts/ci.sh`：`build` 之后、`ctest` 之前新增 `freshness` 步骤（`--quick`/`--full` 都
   经过）；失败先 `cmake --build` 自动重建、再复查，仍不一致才 FAIL（提示产物可能被手工
   替换）。`--full` 的 ASAN 目录同样加 `asan-freshness`。
2. `app/douyin/build.sh`：手工 g++ 前先校验；默认不一致就停止并给重建命令，
   `DOUYIN_AUTOREBUILD=1` 时自动 `cmake --build` 后复查，避免链出旧 ABI 的 exe。

## 验证结果

| 组 | 场景 | 结果 |
|---|---|---|
| A | 正常（产物新于源码） | exit 0，`RESULT: FRESH` |
| B | `touch include/go2cpp/network/http.hpp` | exit 1，STALE，清单列出该头文件 `+9s` |
| C | `ci.sh --quick --no-configure --build-dir .build-bento-wsl` | build 7s OK → freshness **FRESH 1s OK** → ctest 275 passed / 2 failed |
| 真实 | 真树初始状态（源码 18:23 > 产物 18:06） | 首次运行即 STALE，证明不是马后炮 |

C 组 ctest 的两个失败与本改动无关：`bento_peer_hub` 单独复跑通过（同机多 agent 并发构建/
测试的端口竞争）；`monitor_capacity` 是既有容量断言（并发 push 95% 阈值，实测 94%）。两次
`ci.sh` 运行的 freshness 步骤都是 `OK`，且 `--quick` 的 build 步骤自动吃掉了 touch 造成的
重建需求——这正是「自动重建后继续」的路径。

## 已知边界

- 只覆盖 `include/go2cpp/**/*.hpp` 与 `src/**/*.cpp`；`tests/`、`app/`、`mcp/` 的源码变更
  不在 R1 范围内（叶子目标，重建由 CMake 依赖保证）。
- R2 依赖 `readelf`，缺失时退回 `objdump`，都没有则跳过并打印说明。
- 判定的是「批次一致性」，不保证产物内容正确；内容正确性仍由 ctest 负责。
