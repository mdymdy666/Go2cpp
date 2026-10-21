#!/usr/bin/env bash
#
# 并发原语「随机 + 大数据量 + 真实多场景」5 轮测试驱动。
#
# 做什么：
#   1. 编译 OFF 构建（GO2CPP_DEBUG_DUMP=OFF）与 ON 构建（DEBUG_DUMP=ON）
#      两份测试二进制，分别链接对应构建目录的共享库；
#   2. 生成 5 个互不相同的随机种子（打印；可用 SEEDS="a b c d e" 复现）；
#   3. 每轮：`/usr/bin/time -v` + `timeout 120` 跑 OFF 轮（≥100 万操作），
#      再用同一 seed/round 跑 ON 轮做 debug 事件守恒校验；
#   4. 解析每轮 RESULT 行与 time -v 数据，输出 TSV + Markdown + 终端摘要。
#
# 用法：
#   bash tests/concurrency/run_random_5rounds.sh
#   SEEDS="111 222 333 444 555" bash tests/concurrency/run_random_5rounds.sh
#   ROUNDS=1 MIN_OPS=100000 SCALE_OFF=5 SCALE_ON=5 SKIP_ON=1 bash ... （冒烟）
#
# 环境变量：
#   ROUNDS       轮数（默认 5）
#   MIN_OPS      每轮最少操作数（默认 1000000）
#   SCALE_OFF    OFF 轮规模百分比（默认 100）
#   SCALE_ON     ON 轮规模百分比（默认 100）
#   TIMEOUT_SEC  单进程超时（默认 120）
#   SEEDS        空格分隔的固定种子列表（默认从 /dev/urandom 生成）
#   SKIP_ON      1 = 不跑 debug 事件轮
#   SKIP_BUILD   1 = 复用已有二进制
#   BUILD_OFF    OFF 构建目录（默认 build-work）
#   BUILD_ON     ON 构建目录（默认 build-r5-on）
#   WORK_DIR     二进制/日志目录（默认 /tmp/go2cpp-random-5rounds）
#
# 输出：
#   tests/bench/results/random_5rounds_<ts>.tsv        每轮原始数据
#   tests/bench/results/random_5rounds_<ts>.md         数据表（简版）
#   tests/bench/results/random5_<ts>/                  每轮日志 + time -v
#   /tmp/random_5rounds.md                             报告（脚本生成的骨架）
#
# 边界：只新增 tests/**、docs/**；不改 src；不 commit。

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/tests/concurrency/test_random_5rounds.cpp"
BUILD_OFF="${BUILD_OFF:-$ROOT/build-work}"
BUILD_ON="${BUILD_ON:-$ROOT/build-r5-on}"
WORK="${WORK_DIR:-/tmp/go2cpp-random-5rounds}"
RESULTS="$ROOT/tests/bench/results"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
ROUNDS="${ROUNDS:-5}"
MIN_OPS="${MIN_OPS:-1000000}"
SCALE_OFF="${SCALE_OFF:-100}"
SCALE_ON="${SCALE_ON:-100}"
TIMEOUT_SEC="${TIMEOUT_SEC:-120}"
SEEDS="${SEEDS:-}"
SKIP_ON="${SKIP_ON:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"

BIN_OFF="$WORK/bin/test_random_5rounds_off"
BIN_ON="$WORK/bin/test_random_5rounds_on"
LOGDIR="$RESULTS/random5_${RUN_ID}"
RAW="$RESULTS/random_5rounds_${RUN_ID}.tsv"
REPORT="$RESULTS/random_5rounds_${RUN_ID}.md"
REPORT_TMP="/tmp/random_5rounds.md"

# 串行化：同一时间只允许一个 5 轮实例（编译与采集共用一把锁）。
exec 9>/tmp/go2cpp-random-5rounds.lock
if ! flock -n 9; then
    echo "另一个 run_random_5rounds 正在运行（锁: /tmp/go2cpp-random-5rounds.lock）" >&2
    exit 4
fi

mkdir -p "$WORK/bin" "$WORK/logs" "$LOGDIR" "$RESULTS" || exit 2

log() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------------------
# 构建
# ---------------------------------------------------------------------------
LIB_LIST=(
    go2cpp_control_flow go2cpp_channel go2cpp_future go2cpp_math go2cpp_rand
    go2cpp_path go2cpp_container go2cpp_compress go2cpp_encoding go2cpp_metrics
    go2cpp_debug go2cpp_sync go2cpp_debug_event go2cpp_io go2cpp_context
    go2cpp_error go2cpp_config go2cpp_scheduler go2cpp_fiber go2cpp_log
    go2cpp_hash
)

require_build() {
    local build="$1" label="$2"
    if [ ! -f "$build/CMakeCache.txt" ] || [ -z "$(ls "$build"/libgo2cpp_sync.so 2>/dev/null)" ]; then
        echo "缺少 $label 构建: $build" >&2
        echo "请先配置并构建：cmake -S \"$ROOT\" -B \"$build\" -DGO2CPP_DEBUG_DUMP=ON/OFF ..." >&2
        return 2
    fi
}

# compile <build> <output> <extra-cxx-flags...>
compile() {
    local build="$1" out="$2"
    shift 2
    local args=(-O2 -std=c++17 -DNDEBUG -pthread -Wall -Wextra
                -I"$ROOT/include" -I"$ROOT/tests" "$SRC" -o "$out")
    args+=("$@")
    args+=(-Wl,--no-as-needed -Wl,-rpath,"$build" -L"$build")
    local lib
    for lib in "${LIB_LIST[@]}"; do
        args+=("-l$lib")
    done
    /usr/bin/c++ "${args[@]}"
}

build_binaries() {
    if [ "$SKIP_BUILD" = "1" ] && [ -x "$BIN_OFF" ] && { [ "$SKIP_ON" = "1" ] || [ -x "$BIN_ON" ]; }; then
        log "[build] 复用已有二进制（SKIP_BUILD=1）"
        return 0
    fi
    require_build "$BUILD_OFF" OFF || return 2
    log "[build] OFF 二进制 -> $BIN_OFF"
    if ! compile "$BUILD_OFF" "$BIN_OFF" 2> "$WORK/logs/build_off.log"; then
        echo "OFF 编译失败，见 $WORK/logs/build_off.log" >&2
        tail -20 "$WORK/logs/build_off.log" >&2
        return 2
    fi
    if [ "$SKIP_ON" != "1" ]; then
        require_build "$BUILD_ON" ON || return 2
        local cap crit
        cap="$(sed -n 's/^GO2CPP_DEBUG_EVENT_CAPACITY:[^=]*=//p' "$BUILD_ON/CMakeCache.txt" | head -1)"
        crit="$(sed -n 's/^GO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY:[^=]*=//p' "$BUILD_ON/CMakeCache.txt" | head -1)"
        cap="${cap:-256}"
        crit="${crit:-64}"
        log "[build] ON 二进制 -> $BIN_ON (event_capacity=$cap critical_capacity=$crit)"
        if ! compile "$BUILD_ON" "$BIN_ON" \
                -DGO2CPP_DEBUG_DUMP=1 \
                -DGO2CPP_DEBUG_EVENT_CAPACITY="$cap" \
                -DGO2CPP_DEBUG_EVENT_CRITICAL_CAPACITY="$crit" \
                2> "$WORK/logs/build_on.log"; then
            echo "ON 编译失败，见 $WORK/logs/build_on.log" >&2
            tail -20 "$WORK/logs/build_on.log" >&2
            return 2
        fi
    fi
    return 0
}

# ---------------------------------------------------------------------------
# 种子与场景覆盖
# ---------------------------------------------------------------------------
gen_seed() {
    od -An -N4 -tu4 /dev/urandom 2>/dev/null | tr -d ' \n'
}

# 生成一组（ROUNDS 个）互不相同的种子，并保证 5 轮场景并集覆盖全部 6 种。
plan_seeds() {
    local attempt seeds seed plan union_missing r
    for attempt in $(seq 1 100); do
        seeds=()
        for r in $(seq 1 "$ROUNDS"); do
            for _ in $(seq 1 20); do
                seed="$(gen_seed)"
                [ -n "$seed" ] || seed="$((RANDOM * 32768 + RANDOM))"
                local dup=0
                for s in "${seeds[@]:-}"; do
                    [ "$s" = "$seed" ] && dup=1
                done
                [ "$dup" = "0" ] && break
            done
            seeds+=("$seed")
        done
        union_missing=0
        local seen=" "
        for r in $(seq 0 $((ROUNDS - 1))); do
            plan="$("$BIN_OFF" --plan --seed "${seeds[$r]}" --round "$r" 2>/dev/null | sed -n 's/.*scenarios=//p')"
            local IFS=','
            for s in $plan; do
                case "$seen" in
                    *" $s "*) ;;
                    *) seen="$seen$s ";;
                esac
            done
            unset IFS
        done
        for s in pc rwcache workpool pipeline mixedlocks storm; do
            case "$seen" in
                *" $s "*) ;;
                *) union_missing=$((union_missing + 1));;
            esac
        done
        if [ "$union_missing" = "0" ] || [ "$ROUNDS" -lt 5 ]; then
            PLAN_SEEDS=("${seeds[@]}")
            return 0
        fi
        log "[seeds] 第 $attempt 组并集缺 $union_missing 种场景，重抽"
    done
    # 极端情况：放弃覆盖约束，仍然返回一组可用种子（报告会如实标注）。
    PLAN_SEEDS=("${seeds[@]}")
    return 1
}

# ---------------------------------------------------------------------------
# 单轮执行
# ---------------------------------------------------------------------------
parse_result_field() {
    local line="$1" field="$2"
    printf '%s\n' "$line" | sed -n "s/.* ${field}=\\([^ ]*\\).*/\\1/p"
}

# run_case <tag> <bin> <seed> <round> <scale> <min_ops>
run_case() {
    local tag="$1" bin="$2" seed="$3" round="$4" scale="$5" minops="$6"
    local log="$LOGDIR/${tag}_r${round}.log"
    local tlog="$LOGDIR/time_${tag}_r${round}.log"
    local load_before load_after
    local mono_start mono_end mono_wall
    load_before="$(cut -d' ' -f1-3 /proc/loadavg)"
    mono_start="$(cut -d' ' -f1 /proc/uptime)"
    timeout -k 5 "$TIMEOUT_SEC" /usr/bin/time -v -o "$tlog" "$bin" \
        --seed "$seed" --round "$round" --scale-percent "$scale" \
        --min-ops "$minops" --watchdog-total-sec 110 --watchdog-idle-sec 25 \
        > "$log" 2>&1
    local rc=$?
    mono_end="$(cut -d' ' -f1 /proc/uptime)"
    load_after="$(cut -d' ' -f1-3 /proc/loadavg)"
    mono_wall="$(awk -v a="$mono_start" -v b="$mono_end" 'BEGIN{printf "%.3f", b - a}')"
    echo "$rc" > "$LOGDIR/${tag}_r${round}.rc"

    local result ops failures ok debug elapsed
    result="$(grep '^RESULT ' "$log" | tail -1)"
    ops="$(parse_result_field "$result" ops)"
    failures="$(parse_result_field "$result" failures)"
    ok="$(parse_result_field "$result" ok)"
    debug="$(parse_result_field "$result" debug)"
    elapsed="$(parse_result_field "$result" elapsed)"

    local wall rss cpu user sys
    wall="$(sed -n 's/^[[:space:]]*Elapsed (wall clock) time.*: //p' "$tlog" | tail -1)"
    rss="$(sed -n 's/^[[:space:]]*Maximum resident set size (kbytes): //p' "$tlog" | tail -1)"
    cpu="$(sed -n 's/^[[:space:]]*Percent of CPU this job got: //p' "$tlog" | tail -1)"
    user="$(sed -n 's/^[[:space:]]*User time (seconds): //p' "$tlog" | tail -1)"
    sys="$(sed -n 's/^[[:space:]]*System time (seconds): //p' "$tlog" | tail -1)"

    # WSL2 下 /usr/bin/time 的 wall/CPU%（基于 CLOCK_REALTIME）会漂移：实测
    # 4.0s 的轮次报 2.0s。这里用程序内部 steady_clock 的 elapsed 与 rusage 的
    # user+sys 重算 CPU%，/usr/bin/time 的原始值单独保留为参考列。
    local cpu_calc=""
    if [ -n "$user" ] && [ -n "$sys" ] && [ -n "$elapsed" ]; then
        cpu_calc="$(awk -v u="$user" -v s="$sys" -v e="$elapsed" \
            'BEGIN{ if (e > 0) printf "%.0f%%", (u + s) / e * 100 }')"
    fi

    local scenarios
    scenarios="$(sed -n 's/.*scenarios=//p' "$log" | head -1)"
    local events_all="" events_directed="" debug_check=""
    if [ "$debug" = "1" ]; then
        events_all="$(sed -n 's/^\[events:round\] all=\([0-9]*\).*/\1/p' "$log" | tail -1)"
        events_directed="$(sed -n 's/^\[events:directed\].*/directed-ok/p' "$log" | tail -1)"
        if [ -n "$events_directed" ]; then debug_check="pass"; else debug_check="missing"; fi
    fi

    {
        printf 'round\t%s\n' "$round"
        printf 'seed\t%s\n' "$seed"
        printf 'tag\t%s\n' "$tag"
        printf 'scenarios\t%s\n' "$scenarios"
        printf 'ops\t%s\n' "$ops"
        printf 'failures\t%s\n' "$failures"
        printf 'ok\t%s\n' "$ok"
        printf 'rc\t%s\n' "$rc"
        printf 'elapsed\t%s\n' "$elapsed"
        printf 'mono_wall\t%s\n' "$mono_wall"
        printf 'wall\t%s\n' "$wall"
        printf 'rss_kb\t%s\n' "$rss"
        printf 'cpu_pct\t%s\n' "$cpu"
        printf 'cpu_calc\t%s\n' "$cpu_calc"
        printf 'user_s\t%s\n' "$user"
        printf 'sys_s\t%s\n' "$sys"
        printf 'debug\t%s\n' "$debug"
        printf 'events_all\t%s\n' "$events_all"
        printf 'debug_check\t%s\n' "$debug_check"
        printf 'load_before\t%s\n' "$load_before"
        printf 'load_after\t%s\n' "$load_after"
    } > "$LOGDIR/${tag}_r${round}.meta"

    CASE_OK=0
    if [ "$rc" = "0" ] && [ "$ok" = "1" ] && [ -n "$ops" ] && [ "$ops" -ge "$minops" ]; then
        CASE_OK=1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
main() {
    if [ ! -f "$SRC" ]; then
        echo "缺少测试源文件: $SRC" >&2
        return 2
    fi
    build_binaries || return 2

    if [ -n "$SEEDS" ]; then
        read -r -a PLAN_SEEDS <<< "$SEEDS"
    else
        if ! plan_seeds; then
            log "[seeds] 警告：未能凑齐 6 场景并集，报告会标注实际组合"
        fi
    fi
    if [ "${#PLAN_SEEDS[@]}" -lt "$ROUNDS" ]; then
        echo "种子数量不足: ${#PLAN_SEEDS[@]} < $ROUNDS" >&2
        return 2
    fi

    log "==================================================================="
    log "并发原语 5 轮随机测试  run=$RUN_ID  rounds=$ROUNDS min_ops=$MIN_OPS"
    log "OFF build=$BUILD_OFF"
    log "ON  build=$BUILD_ON  skip_on=$SKIP_ON"
    log "seeds: ${PLAN_SEEDS[*]}"
    log "==================================================================="

    printf 'round\tseed\tscenarios\tops\tfailures\tok\trc\telapsed_s\tmono_wall_s\ttime_v_wall\trss_kb\tcpu_pct_time_v\tcpu_pct_calc\tuser_s\tsys_s\tdebug\tevents_all\tdebug_check\tload_before\tload_after\n' > "$RAW"

    local r seed plan
    local passed=0
    for r in $(seq 0 $((ROUNDS - 1))); do
        seed="${PLAN_SEEDS[$r]}"
        plan="$("$BIN_OFF" --plan --seed "$seed" --round "$r" | sed -n 's/.*scenarios=//p')"
        log ""
        log "---- round $((r + 1)) seed=$seed scenarios=$plan ----"
        run_case off "$BIN_OFF" "$seed" "$r" "$SCALE_OFF" "$MIN_OPS"
        local off_ok="$CASE_OK"
        log "[off] ok=$off_ok $(grep '^RESULT ' "$LOGDIR/off_r${r}.log" | tail -1)"
        if [ "$off_ok" = "1" ]; then
            passed=$((passed + 1))
        else
            log "[off] 失败详情:"
            grep -E '^\[FAIL\]|^\[watchdog\]|^\[scenario ' "$LOGDIR/off_r${r}.log" | tail -10
        fi
        if [ "$SKIP_ON" != "1" ]; then
            run_case on "$BIN_ON" "$seed" "$r" "$SCALE_ON" "$MIN_OPS"
            log "[on]  ok=$CASE_OK $(grep '^RESULT ' "$LOGDIR/on_r${r}.log" | tail -1)"
            if [ "$CASE_OK" != "1" ]; then
                grep -E '^\[FAIL\]|^\[watchdog\]|^\[scenario |^\[events:' "$LOGDIR/on_r${r}.log" | tail -12
            fi
        fi
        # 汇总一行（OFF 轮为主；ON 轮的数据在 meta 里）。
        local meta="$LOGDIR/off_r${r}.meta"
        local line
        line="$(printf '%s\t' "$((r + 1))" "$seed")"
        line+="$(sed -n 's/^scenarios\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^ops\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^failures\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^ok\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^rc\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^elapsed\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^mono_wall\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^wall\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^rss_kb\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^cpu_pct\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^cpu_calc\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^user_s\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^sys_s\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^debug\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^events_all\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^debug_check\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^load_before\t//p' "$meta")"$'\t'
        line+="$(sed -n 's/^load_after\t//p' "$meta")"
        printf '%s\n' "$line" >> "$RAW"
    done

    # ---- 汇总 Markdown ----
    {
        echo "# 并发原语 5 轮随机测试数据（$RUN_ID）"
        echo
        echo "- OFF 构建：\`$BUILD_OFF\`（GO2CPP_DEBUG_DUMP=OFF）"
        echo "- ON 构建：\`$BUILD_ON\`（GO2CPP_DEBUG_DUMP=ON）"
        echo "- 轮数：$ROUNDS，每轮最少操作数：$MIN_OPS，单进程超时：${TIMEOUT_SEC}s"
        echo "- 种子：\`${PLAN_SEEDS[*]:0:$ROUNDS}\`"
        echo
        echo "## 每轮数据（OFF 主轮）"
        echo
        echo "> 耗时以程序内部 steady_clock 为准；\`外部 wall\` 取自 /proc/uptime（单调时钟）。"
        echo "> 带 * 的列是 \`/usr/bin/time -v\` 原始值：WSL2 下它的 wall 与 CPU% 基于"
        echo "> CLOCK_REALTIME，实测会漂移（4.0s 报 2.0s），只作参考；CPU% 以"
        echo "> \`(user+sys)/内部耗时\` 重算为准。"
        echo
        echo "| 轮 | 种子 | 场景 | 操作数 | 内部耗时(s) | 外部 wall(s) | time -v wall* | 峰值 RSS(KB) | CPU%(计算) | CPU%(time -v)* | user(s) | sys(s) | 结果 |"
        echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|"
        awk -F'\t' 'NR > 1 {
            printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
                $1, $2, $3, $4, $8, $9, $10, $11, $13, $12, $14, $15,
                ($6 == 1 ? "PASS" : "FAIL")
        }' "$RAW"
        echo
        echo "## debug 事件轮（ON）"
        echo
        echo "| 轮 | 事件总数 | 关键事件快照/累计一致 | 结果 |"
        echo "|---|---|---|---|"
        for r in $(seq 0 $((ROUNDS - 1))); do
            local onlog="$LOGDIR/on_r${r}.log"
            [ -f "$onlog" ] || continue
            local all directed okline
            all="$(sed -n 's/^\[events:round\] all=\([0-9]*\).*/\1/p' "$onlog" | tail -1)"
            directed="$(sed -n 's/^\[events:directed\] \(.*\)$/\1/p' "$onlog" | tail -1)"
            okline="$(sed -n 's/^RESULT .* ok=\([0-9]*\).*/\1/p' "$onlog" | tail -1)"
            printf '| %s | %s | %s | %s |\n' "$((r + 1))" "${all:-n/a}" "${directed:-n/a}" "$([ "$okline" = "1" ] && echo PASS || echo FAIL)"
        done
        echo
        echo "## 通过情况"
        echo
        echo "- OFF 轮通过：$passed / $ROUNDS"
        local on_pass=0
        if [ "$SKIP_ON" != "1" ]; then
            for r in $(seq 0 $((ROUNDS - 1))); do
                grep -q '^RESULT .* ok=1' "$LOGDIR/on_r${r}.log" 2>/dev/null && on_pass=$((on_pass + 1))
            done
        fi
        echo "- ON 轮通过：$on_pass / $([ "$SKIP_ON" = "1" ] && echo 0 || echo "$ROUNDS")"
        echo
        echo "原始数据：\`$RAW\`；每轮日志：\`$LOGDIR\`"
    } > "$REPORT"
    cp "$REPORT" "$REPORT_TMP"

    log ""
    log "==================================================================="
    log "汇总：OFF 通过 $passed/$ROUNDS"
    log "数据：$RAW"
    log "报告：$REPORT"
    log "终端报告：$REPORT_TMP"
    log "==================================================================="
    if [ "$passed" -eq "$ROUNDS" ]; then
        return 0
    fi
    return 1
}

main
exit $?
