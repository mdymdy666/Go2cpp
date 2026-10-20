#!/usr/bin/env python3
"""假 DebugServer：实现 `GO2CPP-DEBUG/1` 文本协议，用于 go2cpp-cli 集成测试。

协议（与 include/go2cpp/debug/debug_server.hpp 一致）：
  请求：一行命令 + "\n"
  响应：`GO2CPP-DEBUG/1 OK <payload_bytes>\n<payload>` 或
        `GO2CPP-DEBUG/1 ERR <code> <message>\n`

支持 --scenario default|partial|no-scheduler，--error-mode、--bad-protocol、
--truncate-payload、--delay-ms、--max-connections。
"""

import argparse
import json
import os
import socket
import sys
import time

PROTOCOL = "GO2CPP-DEBUG/1"

STATUS_FULL = (
    "pid=4242\n"
    "protocol=GO2CPP-DEBUG/1\n"
    "debug_dump=on\n"
    "running=true\n"
    "path=/tmp/fake.sock\n"
    "uptime_ns=1234567890\n"
    "components=3\n"
    "requests=7\n"
)

STATUS_PARTIAL = "pid=4242\n"

SCHEDULER_FULL = {
    "schema": "go2cpp.dump.scheduler/1",
    "version": 1,
    "consistent": "per_call",
    "steady_ns": 1000,
    "wall_unix_ns": 2000,
    "running": True,
    "processor_count": 4,
    "worker_count": 6,
    "runnable_total": 12,
    "runnable_global": 3,
    "runqueue": {"local_total": 9, "global": 3},
    "sysmon_running": True,
    "sysmon_passes": 4210,
    "metrics": {
        "task_runs": 100000,
        "task_completions": 99900,
        "fiber_resume_ns": 880000000,
        "local_queue_pops": 90000,
        "steal_pops": 400,
    },
    "processors": [
        {"id": 0, "state": "kRunning", "queued": 3},
        {"id": 1, "state": "kIdle", "queued": 0},
        {"id": 2, "state": "kRunning", "queued": 2},
        {"id": 3, "state": "kIdle", "queued": 4},
    ],
    "tasks": [
        {"id": 1, "state": "kRunning"},
        {"id": 2, "state": "kWaiting"},
        {"id": 3, "state": "kWaiting"},
    ],
}

SCHEDULER_PARTIAL = {"schema": "go2cpp.dump.scheduler/1", "version": 1, "processor_count": 4}

RUNTIME = {"schema": "go2cpp.dump.runtime/1", "version": 1, "pid": 4242, "debug_dump": True}

ALL_JSON = {
    "schema": "go2cpp.dump.all/1",
    "version": 1,
    "pid": 4242,
    "steady_ns": 1000,
    "wall_unix_ns": 2000,
    "components": {"scheduler": SCHEDULER_FULL, "runtime": RUNTIME},
    "unavailable": ["fiber", "io"],
}

SCHEDULER_TEXT = (
    "schema=go2cpp.dump.scheduler/1\n"
    "version=1\n"
    "running=true\n"
    "processor_count=4\n"
    "runnable_total=12\n"
)

ALL_TEXT = (
    "schema=go2cpp.dump.all/1\n"
    "version=1\n"
    "pid=4242\n"
    "component_count=2\n"
    "unavailable=fiber,io\n"
    "\n[component scheduler]\n"
    + SCHEDULER_TEXT
    + "\n[component runtime]\n"
    "schema=go2cpp.dump.runtime/1\n"
    "version=1\n"
    "pid=4242\n"
)


def ok(payload: str, truncate: bool) -> bytes:
    data = payload.encode("utf-8")
    declared = len(data) + (100 if truncate else 0)
    return (PROTOCOL + " OK " + str(declared) + "\n").encode("utf-8") + data


def err(code: str, message: str) -> bytes:
    return (PROTOCOL + " ERR " + code + " " + message + "\n").encode("utf-8")


def response_for(command: str, scenario: str, truncate: bool) -> bytes:
    normalized = " ".join(command.split())
    upper = normalized.upper()

    if scenario == "partial" and upper == "STATUS":
        return ok(STATUS_PARTIAL, truncate)
    if upper == "PING":
        return ok("pong", truncate)
    if upper == "STATUS":
        return ok(STATUS_FULL, truncate)
    if upper == "LIST":
        return ok("protocol=GO2CPP-DEBUG/1\nscheduler\tavailable\nruntime\tavailable\n", truncate)
    if upper == "HELP":
        return ok("commands: PING | LIST | STATUS | DUMP | JSON | EVENTS | HELP\n", truncate)
    if upper.startswith("JSON "):
        component = normalized[5:].strip()
        if component == "ALL":
            return ok(json.dumps(ALL_JSON, ensure_ascii=False), truncate)
        if component == "scheduler":
            if scenario in ("no-scheduler", "partial"):
                if scenario == "no-scheduler":
                    return err("unknown-component", "scheduler-not-registered")
                return ok(json.dumps(SCHEDULER_PARTIAL, ensure_ascii=False), truncate)
            return ok(json.dumps(SCHEDULER_FULL, ensure_ascii=False), truncate)
        if component == "runtime":
            return ok(json.dumps(RUNTIME, ensure_ascii=False), truncate)
        return err("unknown-component", component + "-not-registered")
    if upper.startswith("DUMP ") or upper.startswith("TEXT "):
        component = normalized.split(" ", 1)[1].strip() if " " in normalized else ""
        if component.upper() == "ALL":
            return ok(ALL_TEXT, truncate)
        if component == "scheduler":
            return ok(SCHEDULER_TEXT, truncate)
        if component == "runtime":
            return ok("schema=go2cpp.dump.runtime/1\nversion=1\npid=4242\n", truncate)
        return err("unknown-component", component + "-not-registered")
    return err("bad-command", "unknown-command")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--scenario", default="default",
                        choices=["default", "partial", "no-scheduler"])
    parser.add_argument("--error-mode", action="store_true")
    parser.add_argument("--bad-protocol", action="store_true")
    parser.add_argument("--truncate-payload", action="store_true")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--max-connections", type=int, default=16)
    args = parser.parse_args()

    for suffix in ("", ".ready"):
        try:
            os.unlink(args.socket + suffix)
        except FileNotFoundError:
            pass

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(args.socket)
    server.listen(64)
    with open(args.socket + ".ready", "w", encoding="utf-8") as handle:
        handle.write("ready\n")

    served = 0
    while served < args.max_connections:
        try:
            connection, _ = server.accept()
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
            if args.delay_ms:
                time.sleep(args.delay_ms / 1000.0)
            if args.error_mode:
                payload = err("internal", "test-error")
            elif args.bad_protocol:
                payload = b"DEBUG/9 OK 2\nhi\n"
            else:
                payload = response_for(command, args.scenario, args.truncate_payload)
            try:
                connection.sendall(payload)
            except OSError:
                pass
        served += 1

    server.close()
    try:
        os.unlink(args.socket)
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
