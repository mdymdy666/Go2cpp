#!/usr/bin/env python3
"""graph 子命令的假数据源：同时模拟 DebugServer（GO2CPP-DEBUG/1）与 Doctor（G2CD/1）。

只用于 `tools/cli/tests/run_graph.sh`，不进入构建依赖。数据是固定的三 goroutine /
两个 mutex 的锁顺序死锁场景：

- G11 持有 res-a（DebugServer resource_id=1）并等待 res-b；
- G12 持有 res-b（id=2）并等待 res-a；
- 所以 wait-for 图应产生 res-a -> G11 -> res-b -> G12 -> res-a 的环。

用法:
  python3 graph_fake_server.py --debug-socket /tmp/d.sock --doctor-socket /tmp/doc.sock
  python3 graph_fake_server.py --dump-fixtures /tmp/dir
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading

DEBUG_PROTOCOL = "GO2CPP-DEBUG/1"
G2CD_MAGIC = 0x44433247
G2CD_VERSION = 1


def _event(seq, ts_ns, code, gid, resource_id=0, result="ok", waiters=0, target_gid=0,
           detail=""):
    return {
        "seq": seq,
        "ts_ns": ts_ns,
        "thread_id": 1,
        "tid": 100,
        "pid": 0,
        "code": code,
        "code_value": 0,
        "gid": gid,
        "arg2": 0,
        "arg3": 0,
        "arg4": 0,
        "primitive_reason": "",
        "critical": False,
        "resource_id": resource_id,
        "target_gid": target_gid,
        "result": 0,
        "result_name": result,
        "waiter_depth": waiters,
        "value": 0,
        "value_known": False,
        "stack_id": 0,
        "chain_id": 0,
        "flags": 0,
        "detail": detail,
    }


RESOURCES = {
    "schema": "go2cpp.dump.view.resources/1",
    "version": 1,
    "ok": True,
    "view": "resources",
    "window": {"truncated": False, "event_count": 12},
    "resources": {
        "resources": [
            {"id": 1, "kind": "kMutex", "name": "kMutex#1", "alive": True,
             "event_count": 4, "holds": 2, "waits": 1, "wakes": 1, "max_waiters": 1},
            {"id": 2, "kind": "kMutex", "name": "kMutex#2", "alive": True,
             "event_count": 4, "holds": 2, "waits": 1, "wakes": 1, "max_waiters": 1},
            {"id": 3, "kind": "kMutex", "name": "kMutex#3", "alive": True,
             "event_count": 0},
            {"id": 4, "kind": "kMutex", "name": "kMutex#4", "alive": True,
             "event_count": 0},
            {"id": 5, "kind": "kParkingCondition", "name": "kParkingCondition#5",
             "alive": True, "event_count": 10, "waits": 2, "wakes": 1,
             "max_waiters": 2},
        ],
        "count": 5,
        "by_kind": {"kMutex": 4, "kParkingCondition": 1},
        "alive": 5,
    },
}

HISTORY = {
    1: {
        "schema": "go2cpp.dump.view.history/1",
        "version": 1,
        "ok": True,
        "view": "history",
        "window": {"truncated": False, "event_count": 2},
        "history": {
            "resource_id": 1,
            "resource": {"id": 1, "kind": "kMutex", "name": "kMutex#1",
                         "alive": True, "address": 4096},
            "events": [
                _event(10, 1000, "kMutexLock", 11, 1, "ok", 0, detail="mutex-contended"),
                _event(11, 2000, "kMutexLock", 12, 1, "blocked", 1,
                       detail="mutex-contended"),
            ],
            "count": 2,
            "summary": {"events": 2, "holds": 1, "waits": 1, "wakes": 0,
                        "max_waiters": 1},
        },
    },
    2: {
        "schema": "go2cpp.dump.view.history/1",
        "version": 1,
        "ok": True,
        "view": "history",
        "window": {"truncated": False, "event_count": 2},
        "history": {
            "resource_id": 2,
            "resource": {"id": 2, "kind": "kMutex", "name": "kMutex#2",
                         "alive": True, "address": 8192},
            "events": [
                _event(12, 1500, "kMutexLock", 12, 2, "ok", 0, detail="mutex-contended"),
                _event(13, 2500, "kMutexLock", 11, 2, "blocked", 1,
                       detail="mutex-contended"),
            ],
            "count": 2,
            "summary": {"events": 2, "holds": 1, "waits": 1, "wakes": 0,
                        "max_waiters": 1},
        },
    },
}

SCHED = {
    "schema": "go2cpp.dump.view.sched/1",
    "version": 1,
    "ok": True,
    "view": "sched",
    "window": {"truncated": False, "event_count": 10},
    "sched": {
        "counts": {"enqueue": 1, "runqueue_push": 1, "park": 2},
        "chains": [],
        "flow": [
            _event(20, 3000, "kGCreated", 11),
            _event(21, 3010, "kGStarted", 11),
            _event(22, 3020, "kParkEnter", 11, 1, "ok", 1, detail="park mutex"),
            _event(23, 3030, "kWakeEnqueued", 11, 1, "wake", 1),
            _event(24, 3040, "kGResumed", 11),
            _event(25, 3100, "kGCreated", 12),
            _event(26, 3110, "kGStarted", 12),
            _event(27, 3120, "kParkEnter", 12, 2, "ok", 1, detail="park mutex"),
            _event(28, 3200, "kGCompleted", 11),
            _event(29, 3210, "kGCompleted", 12),
        ],
    },
}

GMP = {
    "schema": "go2cpp.dump.view.gmp/1",
    "version": 1,
    "ok": True,
    "view": "gmp",
    "window": {"truncated": False, "event_count": 0},
    "gmp": {
        "summary": {
            "available": True,
            "processor_count": 2,
            "worker_count": 2,
            "runnable_total": 1,
            "runnable_global": 0,
            "running": True,
            "runqueue": {"local_total": 1, "global": 0},
            "metrics": {"task_runs": 2, "task_completions": 2},
        },
        "processors": [
            {"id": 0, "state": "kRunning", "queued": 1},
            {"id": 1, "state": "kIdle", "queued": 0},
        ],
        "machines": [
            {"id": 0, "state": "kRunning", "processor": 0, "blocking_task": 0,
             "long_syscall_count": 0, "replacement": False},
            {"id": 1, "state": "kParked", "processor": 1, "blocking_task": 0,
             "long_syscall_count": 0, "replacement": False},
        ],
        "g": [
            {"gid": 11, "state": "kWaiting", "started": True,
             "last_processor_id": 0, "failed": False, "cancellation_requested": False},
            {"gid": 12, "state": "kWaiting", "started": True,
             "last_processor_id": 1, "failed": False, "cancellation_requested": False},
        ],
        "g_by_state": {"kWaiting": 2},
        "blocked": {
            "count": 2,
            "by_reason": {"kMutex": 2},
            "items": [
                {"gid": 11, "reason": "kMutex", "status": "open"},
                {"gid": 12, "reason": "kMutex", "status": "open"},
            ],
        },
    },
}

PARK = {
    "schema": "go2cpp.dump.view.park/1",
    "version": 1,
    "ok": True,
    "view": "park",
    "window": {"truncated": False, "event_count": 4},
    "park": {
        "count": 2,
        "items": [
            {"gid": 11, "park_seq": 22, "park_ts_ns": 3020,
             "reason": "kMutex", "status": "open", "suspend_reason": "kPark",
             "wake_seq": 0, "wake_ts_ns": 0, "wake_code": "", "wake_reason": "",
             "wake_latency_ns": 0, "return_seq": 0, "resume_delay_ns": 0},
            {"gid": 12, "park_seq": 27, "park_ts_ns": 3120,
             "reason": "kMutex", "status": "open", "suspend_reason": "kPark",
             "wake_seq": 0, "wake_ts_ns": 0, "wake_code": "", "wake_reason": "",
             "wake_latency_ns": 0, "return_seq": 0, "resume_delay_ns": 0},
        ],
    },
}

PRIMITIVES = {
    "schema": "go2cpp.dump.view.primitives/1",
    "version": 1,
    "ok": True,
    "view": "primitives",
    "window": {"truncated": False, "event_count": 4},
    "primitives": {
        "counts": {"mutex_lock": 2, "mutex_unlock": 0, "pc_wait": 0, "pc_notify": 0},
        "mutex": [
            _event(10, 1000, "kMutexLock", 11, 1, "ok", 0),
            _event(11, 2000, "kMutexLock", 12, 1, "blocked", 1),
            _event(12, 1500, "kMutexLock", 12, 2, "ok", 0),
            _event(13, 2500, "kMutexLock", 11, 2, "blocked", 1),
        ],
        "channel": [],
        "parking_condition": [],
        "selects": [],
        "park_reason_mix": {},
        "flow": [
            _event(10, 1000, "kMutexLock", 11, 1, "ok", 0),
            _event(12, 1500, "kMutexLock", 12, 2, "ok", 0),
            _event(11, 2000, "kMutexLock", 12, 1, "blocked", 1),
            _event(13, 2500, "kMutexLock", 11, 2, "blocked", 1),
        ],
    },
}

DEADLOCK_REPORT = {
    "schema": "go2cpp.diagnostic.deadlock-report/1",
    "version": 1,
    "verdict": "deadlock",
    "no_progress_ms": 1500,
    "threshold_ms": 600,
    "auto_force_wake": False,
    "detection_enabled": True,
    "live_tasks": 2,
    "schedulers": [{"name": "scheduler", "runnable": 0, "workers": 2}],
    "edges": [
        {"from": "scheduler", "kind": "scheduler-idle", "retryable": True},
        {"from": "res-a", "kind": "mutex", "held": True, "waiters": 1,
         "native_waiters": 0, "fiber_waiters": 1, "owner_gid": 11,
         "owner_known": True, "owner_alive": True, "retryable": False},
        {"from": "res-b", "kind": "mutex", "held": True, "waiters": 1,
         "native_waiters": 0, "fiber_waiters": 1, "owner_gid": 12,
         "owner_known": True, "owner_alive": True, "retryable": False},
    ],
    "events": [],
}


def fixtures():
    table = {
        "resources.json": RESOURCES,
        "history1.json": HISTORY[1],
        "history2.json": HISTORY[2],
        "sched.json": SCHED,
        "gmp.json": GMP,
        "park.json": PARK,
        "primitives.json": PRIMITIVES,
        "deadlock-report.json": DEADLOCK_REPORT,
    }
    return table


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def encode_g2cd(stream_id, payload, kind=2):
    body = payload.encode("utf-8")
    header = bytearray(24)
    struct.pack_into("<I", header, 0, G2CD_MAGIC)
    header[4] = G2CD_VERSION
    header[5] = kind
    struct.pack_into("<H", header, 6, 0)
    struct.pack_into("<I", header, 8, stream_id)
    struct.pack_into("<I", header, 12, 0)
    struct.pack_into("<I", header, 16, len(body))
    struct.pack_into("<I", header, 20, 0)
    checksum = fnv1a(bytes(header) + body)
    struct.pack_into("<I", header, 20, checksum)
    return bytes(header) + body


DEBUG_RESPONSES = {
    "resources": json.dumps(RESOURCES),
    "sched": json.dumps(SCHED),
    "gmp": json.dumps(GMP),
    "park": json.dumps(PARK),
    "primitives": json.dumps(PRIMITIVES),
    "history 1": json.dumps(HISTORY[1]),
    "history 2": json.dumps(HISTORY[2]),
}


def debug_response(command):
    normalized = " ".join(command.split())
    if not normalized.upper().startswith("VIEW "):
        return None, "bad-command", "unknown-command"
    key = normalized[5:].strip()
    if key in DEBUG_RESPONSES:
        return DEBUG_RESPONSES[key], None, None
    return None, "unknown-view", key + "-not-supported"


def serve_debug(path, stop):
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(path)
    server.listen(64)
    server.settimeout(0.2)
    while not stop.is_set():
        try:
            connection, _ = server.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        with connection:
            data = b""
            while b"\n" not in data and len(data) < 4096:
                chunk = connection.recv(4096)
                if not chunk:
                    break
                data += chunk
            command = data.split(b"\n")[0].decode("utf-8", "replace")
            payload, code, message = debug_response(command)
            if code is None:
                raw = payload.encode("utf-8")
                response = (DEBUG_PROTOCOL + " OK " + str(len(raw)) + "\n").encode() + raw
            else:
                response = (DEBUG_PROTOCOL + " ERR " + code + " " + message + "\n").encode()
            try:
                connection.sendall(response)
            except OSError:
                pass
    server.close()
    try:
        os.unlink(path)
    except OSError:
        pass


def serve_doctor(path, stop):
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(path)
    server.listen(64)
    server.settimeout(0.2)
    while not stop.is_set():
        try:
            connection, _ = server.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        with connection:
            data = b""
            while len(data) < 24:
                chunk = connection.recv(24 - len(data))
                if not chunk:
                    break
                data += chunk
            if len(data) < 24:
                continue
            payload_len = struct.unpack_from("<I", data, 16)[0]
            body = b""
            while len(body) < payload_len:
                chunk = connection.recv(payload_len - len(body))
                if not chunk:
                    break
                body += chunk
            stream_id = struct.unpack_from("<I", data, 8)[0]
            command = body.decode("utf-8", "replace")
            if "deadlock-report" in command:
                payload = json.dumps(DEADLOCK_REPORT)
                frame = encode_g2cd(stream_id, payload)
            else:
                payload = json.dumps({"schema": "go2cpp.diagnostic.status/1",
                                      "version": 1, "ok": True,
                                      "goroutines": {"total": 2, "waiting": 2}})
                frame = encode_g2cd(stream_id, payload)
            try:
                connection.sendall(frame)
            except OSError:
                pass
    server.close()
    try:
        os.unlink(path)
    except OSError:
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--debug-socket")
    parser.add_argument("--doctor-socket")
    parser.add_argument("--only", choices=["debug", "doctor"], default=None)
    parser.add_argument("--dump-fixtures")
    args = parser.parse_args()

    if args.dump_fixtures:
        os.makedirs(args.dump_fixtures, exist_ok=True)
        for name, value in fixtures().items():
            with open(os.path.join(args.dump_fixtures, name), "w", encoding="utf-8") as handle:
                json.dump(value, handle, ensure_ascii=False, indent=1)
        with open(os.path.join(args.dump_fixtures, "run.trace"), "w", encoding="utf-8") as handle:
            handle.write("# go2cpp-trace v1\n# BEGIN\n")
            handle.write("#1 OUTPUT SPAWN gid=11\n")
            handle.write("#2 OUTPUT PARK gid=11 reason=chan_wait\n")
            handle.write("#3 OUTPUT WAKE gid=11\n")
            handle.write("#4 OUTPUT ENQ gid=11\n")
            handle.write("#5 OUTPUT EXIT gid=11 state=kDead\n")
            handle.write("# END\n")
        with open(os.path.join(args.dump_fixtures, "record.g2cr"), "wb") as handle:
            handle.write(b"G2CRPLY\x00" + b"\x00" * 32)
        return 0

    if not args.debug_socket and not args.doctor_socket:
        parser.error("需要 --debug-socket 或 --doctor-socket")

    stop = threading.Event()
    threads = []
    if args.debug_socket and args.only != "doctor":
        thread = threading.Thread(target=serve_debug,
                                  args=(args.debug_socket, stop), daemon=True)
        thread.start()
        threads.append(thread)
        open(args.debug_socket + ".ready", "w", encoding="utf-8").write("ready\n")
    if args.doctor_socket and args.only != "debug":
        thread = threading.Thread(target=serve_doctor,
                                  args=(args.doctor_socket, stop), daemon=True)
        thread.start()
        threads.append(thread)
        open(args.doctor_socket + ".ready", "w", encoding="utf-8").write("ready\n")
    try:
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        stop.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
