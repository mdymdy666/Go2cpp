#!/usr/bin/env python3
"""直接通过 Unix socket 查询真实 DebugServer，并与 go2cpp-cli 的 dump 结果对比。

用法: python3 /tmp/go2cpp_debug_probe.py <pid> <cli-dump.json>
退出码 0 表示全部断言通过。
"""
import json
import socket
import sys
from pathlib import Path

PROTOCOL = "GO2CPP-DEBUG/1"


def call(sock_path: str, command: str):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(5.0)
        s.connect(sock_path)
        s.sendall((command + "\n").encode("utf-8"))
        chunks = []
        while True:
            data = s.recv(65536)
            if not data:
                break
            chunks.append(data)
    raw = b"".join(chunks)
    head, sep, body = raw.partition(b"\n")
    assert sep, f"no header line: {raw[:80]!r}"
    head_text = head.decode("utf-8")
    assert head_text.startswith(PROTOCOL + " "), head_text
    rest = head_text[len(PROTOCOL) + 1 :]
    if rest.startswith("ERR "):
        code, _, message = rest[4:].partition(" ")
        return ("ERR", code, message)
    assert rest.startswith("OK "), head_text
    size = int(rest[3:])
    assert len(body) == size, (len(body), size)
    return ("OK", body.decode("utf-8"))


def main() -> int:
    pid = int(sys.argv[1])
    cli_path = Path(sys.argv[2])
    sock_path = f"/tmp/go2cpp-{pid}.sock"
    cli = json.loads(cli_path.read_text(encoding="utf-8"))["components"]

    status_kind, status_payload = call(sock_path, "STATUS")
    assert status_kind == "OK"
    status = {}
    for line in status_payload.splitlines():
        key, _, value = line.partition("=")
        status[key] = value
    print("socket STATUS:", json.dumps(status, ensure_ascii=False, sort_keys=True))
    assert status["protocol"] == PROTOCOL
    assert status["debug_dump"] == "on"
    assert status["running"] == "true"
    assert status["path"] == sock_path

    sched_kind, sched_payload = call(sock_path, "JSON scheduler")
    assert sched_kind == "OK"
    sched = json.loads(sched_payload)
    print("socket JSON scheduler: schema=%s processors=%s workers=%s tasks=%s" % (
        sched["schema"], sched["processor_count"], sched["worker_count"], len(sched["tasks"])))
    assert sched["schema"] == "go2cpp.dump.scheduler/1"
    assert sched["processor_count"] == cli["scheduler"]["processor_count"]
    assert sched["worker_count"] == cli["scheduler"]["worker_count"]
    assert sched["running"] == cli["scheduler"]["running"]
    assert sched["runqueue"] == cli["scheduler"]["runqueue"]
    assert sched["metrics"]["task_runs"] == cli["scheduler"]["metrics"]["task_runs"]
    assert len(sched["tasks"]) == len(cli["scheduler"]["tasks"])
    assert [t["id"] for t in sched["tasks"]] == [t["id"] for t in cli["scheduler"]["tasks"]]
    assert all(t["state"] == "kWaiting" for t in sched["tasks"])

    fiber_kind, fiber_payload = call(sock_path, "JSON fiber")
    assert fiber_kind == "OK"
    fiber = json.loads(fiber_payload)
    print("socket JSON fiber: id=%s state=%s reason=%s task_id=%s" % (
        fiber["id"], fiber["state"], fiber["suspend_reason"], fiber["task_id"]))
    assert fiber["schema"] == "go2cpp.dump.fiber/1"
    for key in ("id", "state", "suspend_reason", "task_id", "nesting_depth",
                "has_stack", "growable", "requested_bytes", "usable_bytes",
                "reserve_bytes", "mapping_base", "mapping_top", "committed_begin"):
        assert fiber[key] == cli["fiber"][key], (key, fiber[key], cli["fiber"][key])

    chan_kind, chan_payload = call(sock_path, "JSON channel")
    assert chan_kind == "OK"
    channel = json.loads(chan_payload)
    print("socket JSON channel: capacity=%s recv_waiters=%s send_waiters=%s" % (
        channel["capacity"], channel["recv_waiters"], channel["send_waiters"]))
    assert channel == cli["channel"], (channel, cli["channel"])
    assert channel["recv_waiters"] >= 2

    kind, text = call(sock_path, "DUMP scheduler")
    assert kind == "OK"
    assert "schema=go2cpp.dump.scheduler/1" in text
    assert "processor_count=2" in text

    kind, code, message = call(sock_path, "JSON no-such-component")
    assert kind == "ERR" and code == "unknown-component", (kind, code, message)

    print("PROBE_OK: CLI dump 与 DebugServer 原始响应字段一致（socket=%s）" % sock_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
