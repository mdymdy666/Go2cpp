# 无性能报告模块的微基准（perf_uncovered）

覆盖台账 M24–M35 的 12 个模块，每个模块一个自包含 `.cpp`，可直接 `g++ -O2` 编译。

## 编译

```bash
cd /UserData/CodexWorkSpace/Go2cpp
ROOT=$PWD
OUT=/tmp/perf_uncovered
mkdir -p $OUT
CXX="g++ -O2 -std=c++20 -DNDEBUG -I$ROOT/include -I$ROOT/third_party -I$ROOT"
LIB="-L$ROOT/.build-bento-wsl -Wl,-rpath,$ROOT/.build-bento-wsl -lpthread"

$CXX tests/bench/perf_uncovered/bench_json.cpp       -o $OUT/bench_json       $LIB -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_error.cpp      -o $OUT/bench_error      $LIB -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_resilience.cpp -o $OUT/bench_resilience $LIB -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_trace.cpp      -o $OUT/bench_trace      $LIB -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_arena.cpp src/storage/arena.cpp -o $OUT/bench_arena $LIB
$CXX tests/bench/perf_uncovered/bench_metrics.cpp    -o $OUT/bench_metrics    $LIB -lgo2cpp_metrics -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_config.cpp     -o $OUT/bench_config     $LIB \
     -lgo2cpp_config -lgo2cpp_error -lgo2cpp_log -lgo2cpp_os -lgo2cpp_io \
     -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context -lgo2cpp_sync
$CXX tests/bench/perf_uncovered/bench_log.cpp        -o $OUT/bench_log        $LIB -lgo2cpp_log -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_relay.cpp      -o $OUT/bench_relay      $LIB -lgo2cpp_relay -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_io.cpp         -o $OUT/bench_io         $LIB \
     -lgo2cpp_io -lgo2cpp_scheduler -lgo2cpp_fiber -lgo2cpp_context -lgo2cpp_sync -lgo2cpp_error
$CXX tests/bench/perf_uncovered/bench_hook.cpp       -o $OUT/bench_hook       $LIB
# orm：该构建配置未产出 storage 库，直接编源文件（db.cpp + orm.cpp）
$CXX tests/bench/perf_uncovered/bench_orm.cpp src/storage/db.cpp src/storage/orm.cpp \
     -o $OUT/bench_orm $LIB -lgo2cpp_log -lgo2cpp_error -lsqlite3
```

## 运行

```bash
bash tests/bench/perf_uncovered/run_5rounds.sh   # 5 轮串行，taskset -c 1
```

`hook` 无法在同一进程内对照：由脚本分别跑「不 preload」与
`LD_PRELOAD=.build-bento-wsl/libgo2cpp_hook.so` 两次。

## 口径

每个基准输出一行 `metric ops=... total_ms=... ops_per_sec=... ns_per_op=...`；
脚手架（`bench_common.hpp`）按实测吞吐自适应迭代数，**单样本 100 ms–1 s**，
避免 <100 ms 的计时噪声。详见 `docs/reports/perf-uncovered-2026-11-05.md`。
