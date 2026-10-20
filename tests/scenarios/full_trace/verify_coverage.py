# -*- coding: utf-8 -*-
"""全链路视图验收：读取 run_full_trace.sh 抓取的 JSON，输出事件覆盖率。

退出码 0 = 通过；1 = 有硬性检查失败。窗口截断导致的比例只报告、不误判，
但「关键事件类型全部出现过」与「park 至少有可解释去向」是硬性要求。
"""
import json
import sys
from pathlib import Path

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/full_trace")
VIEWS = ["gmp", "timeline", "sched", "park", "timer", "primitives"]


def load(name):
    path = OUT / f"{name}.json"
    if not path.is_file():
        raise SystemExit(f"FAIL: 缺少视图文件 {path}")
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def check(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def pct(numerator, denominator):
    if denominator <= 0:
        return 0.0
    return 100.0 * numerator / denominator


def main() -> int:
    data = {}
    for name in VIEWS:
        payload = load(name)
        check(payload.get("ok") is True, f"{name}.json 的 ok 不为 true")
        check(
            payload.get("schema") == f"go2cpp.dump.view.{name}/1",
            f"{name}.json schema 异常: {payload.get('schema')}",
        )
        data[name] = payload

    gmp = data["gmp"]
    timeline = data["timeline"]["timeline"]
    sched = data["sched"]["sched"]
    park = data["park"]["park"]
    timer = data["timer"]["timer"]
    primitives = data["primitives"]["primitives"]

    coverage = gmp["coverage"]
    totals = gmp["totals"]
    window = gmp["window"]

    # ---- 硬性：窗口与关键组件 ----
    check(window["event_count"] > 0, "事件窗口为空（EventRing 未记录？）")
    check(gmp["gmp"]["summary"]["available"] is True, "scheduler 组件不可用")
    check(len(gmp["gmp"]["processors"]) >= 1, "gmp 视图没有 P 信息")
    check(len(gmp["gmp"]["g"]) >= 4, "活动 G 少于 4 个")

    # ---- 硬性：每类事件都出现过（进程级 totals，不受窗口截断影响）----
    required = {
        "g_created": totals["g_created"],
        "g_started": totals["g_started"],
        "g_resumed": totals["g_resumed"],
        "g_completed": totals["g_completed"],
        "enqueue": totals["enqueue"],
        "claim_ok": totals["claim_ok"],
        "park": totals["park"],
        "wake": totals["wake"],
        "mutex_lock": totals["mutex_lock"],
        "mutex_unlock": totals["mutex_unlock"],
        "channel_send_block": totals["channel_send_block"],
        "channel_recv_block": totals["channel_recv_block"],
        "channel_select": totals["channel_select"],
        "pc_wait": totals["pc_wait"],
        "pc_notify": totals["pc_notify"],
        "timer_fire": totals["timer_fire"],
        "timer_callback": totals["timer_callback"],
        "timer_enqueue": totals["timer_enqueue"],
    }
    missing = [name for name, value in required.items() if value <= 0]
    check(not missing, "以下事件类型从未出现: " + ", ".join(missing))

    # ---- park ↔ wake 配对 ----
    pairing = park["pairing"]
    checked = pairing["checked"]
    paired = pairing["paired"]
    open_stuck = len(park["stuck"])
    explained = paired + open_stuck
    ratio = pct(explained, checked)
    check(checked > 0, "窗口内没有可检查的 park")
    check(ratio >= 50.0, f"park 有解释去向比例过低: {ratio:.1f}%")

    # ---- enqueue ↔ claim ----
    sched_counts = sched["counts"]
    check(sched_counts["enqueue"] > 0, "窗口内没有 enqueue")
    check(sched_counts["claim_ok"] > 0, "窗口内没有 claim_ok")
    window_pending = sched["pairing"]["window_enqueue_pending"]
    check(
        window_pending <= max(16, int(0.2 * sched_counts["enqueue"])),
        f"窗口内 enqueue 未 claim 数量异常: {window_pending}",
    )

    # ---- timer fire → 回调/G 入队 ----
    timer_pairing = timer["pairing"]
    check(timer_pairing["fire_total_all"] > 0, "timer 从未 fire")
    check(timer_pairing["callback_total_all"] > 0, "timer 回调从未投递")
    check(timer_pairing["g_enqueue_total_all"] > 0, "定时 Fiber 从未入队")

    # ---- timeline：完整生命周期样本 ----
    check(timeline["found_in_window"] is True, "timeline 未找到目标 G")
    check(timeline["counts"]["events"] > 0, "timeline 事件为空")
    check(timeline["counts"]["park"] > 0, "timeline 中没有 park")
    check(timeline["counts"]["wake"] > 0, "timeline 中没有 wake")
    check(timeline["counts"]["resume"] > 0, "timeline 中没有 resume")
    lifecycle = timeline["lifecycle"]
    for phase in ("created", "started", "completed"):
        check(phase in lifecycle, f"timeline 生命周期缺少 {phase}")

    # ---- primitives ----
    prim_counts = primitives["counts"]
    for key in (
        "mutex_lock",
        "channel_send_block",
        "channel_recv_block",
        "channel_select",
        "pc_wait",
    ):
        check(prim_counts[key] > 0, f"primitives 中 {key} 为 0")

    # ---- 报告 ----
    print("== 事件覆盖率（进程级累计 totals）==")
    print(f"事件窗口: count={window['event_count']} first_seq={window['first_seq']} "
          f"last_seq={window['last_seq']} gaps={window['gaps']} "
          f"truncated={window['truncated']} total_published={window['total_published']}")
    print(f"G 生命周期: created={totals['g_created']} started={totals['g_started']} "
          f"resumed={totals['g_resumed']} completed={totals['g_completed']} "
          f"failed={totals['g_failed']} cancelled={totals['g_cancelled']}")
    print(f"调度: enqueue={totals['enqueue']} "
          f"enqueue_rejected={totals['enqueue_rejected']} "
          f"claim_ok={totals['claim_ok']} claim_fail={totals['claim_fail']} "
          f"pending(enqueue-claim)={coverage['enqueue_pending_total']} "
          f"steal_ok={totals['steal_ok']} "
          f"worker_park={totals['worker_park']} sysmon={totals['sysmon_pass']}")
    print(f"park/wake: park={totals['park']} wake={totals['wake']} "
          f"delta={coverage['park_minus_wake']} park_return={totals['park_return']} "
          f"park_rejected={totals['park_rejected']} "
          f"wake_pending={totals['wake_pending']} "
          f"wake_rejected={totals['wake_rejected']}")
    print(f"窗口配对: checked={checked} paired={paired} stuck={open_stuck} "
          f"解释率={ratio:.1f}% (wake_latency 与 resume_delay 见 park.json)")
    print(f"原语: mutex_lock={totals['mutex_lock']} mutex_unlock={totals['mutex_unlock']} "
          f"mutex_fast={totals['mutex_fast_lock']} "
          f"channel_send_block={totals['channel_send_block']} "
          f"channel_recv_block={totals['channel_recv_block']} "
          f"channel_select={totals['channel_select']} "
          f"pc_wait={totals['pc_wait']} pc_notify={totals['pc_notify']}")
    print(f"timer: fire={totals['timer_fire']} callback={totals['timer_callback']} "
          f"g_enqueue={totals['timer_enqueue']} "
          f"窗口 fire_events={timer_pairing.get('fire_events', 0)} "
          f"fire_with_callback={timer_pairing.get('fire_events_with_callback', 0)} "
          f"fire_with_g_enqueue={timer_pairing.get('fire_events_with_enqueue', 0)}")
    print(f"timeline: gid={timeline['gid']} state={timeline['current_state']} "
          f"events={timeline['counts']['events']} park={timeline['counts']['park']} "
          f"wake={timeline['counts']['wake']} resume={timeline['counts']['resume']} "
          f"lifecycle={'created' in lifecycle}/{'started' in lifecycle}/"
          f"{'completed' in lifecycle}")
    print(f"PASS: 6 个视图可用，事件类型全覆盖，park↔wake 有解释率 {ratio:.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
