#!/usr/bin/env python3
"""Bounded offered-rate volatile GET experiment; never automatically qualifies a capacity profile."""

from __future__ import annotations

import argparse
import asyncio
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sdk/python/src"))
from glifistore.protocol import Opcode, Status, decode_response, encode_request  # noqa: E402

if __package__:
    from .benchmark_owner_capacity import environment
else:
    from benchmark_owner_capacity import environment


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rate", type=int, default=10_000)
    parser.add_argument("--duration-ms", type=int, default=5000)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--value-size", type=int, default=1024)
    parser.add_argument("--helpers", type=int, default=0)
    parser.add_argument("--slo-us", type=int, default=5000)
    args = parser.parse_args(argv)
    if not (1 <= args.rate <= 250_000 and 500 <= args.duration_ms <= 30_000
            and args.rate * args.duration_ms <= 2_000_000_000 and 1 <= args.clients <= 64
            and 0 <= args.helpers <= 4 and 0 <= args.value_size <= 256 * 1024
            and 1 <= args.slo_us <= 10_000_000):
        parser.error("experiment bounds exceeded (maximum two million scheduled GETs)")
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error("binary must be an existing executable")
    if args.output.exists():
        parser.error("output must be a new directory; existing evidence is never overwritten")
    return args


async def receive(reader):
    header = await reader.readexactly(40)
    size = int.from_bytes(header[:4], "little")
    if not 40 <= size <= 2 * 1024 * 1024:
        raise ValueError("noncanonical response extent")
    return decode_response(header + await reader.readexactly(size - 40))


def check(response, request_id, value=None):
    if (response.status != Status.OK or response.request_id != request_id
            or response.owner_worker != 0 or response.worker_count != 1
            or response.routing_epoch != 1 or (value is not None and response.value != value)):
        raise ValueError("response identity, order, status or bytes differ")


async def workload(args, port):
    connections = []
    receivers = []
    value = b"v" * args.value_size
    key = b"read-capacity-key"
    latencies = []
    completed = 0
    admitted = 0
    missed = 0
    windows = []
    finished = asyncio.Event()
    try:
        for _ in range(args.clients):
            reader, writer = await asyncio.open_connection("127.0.0.1", port)
            pending = deque()
            connections.append((reader, writer, pending))
            for opcode, request_id, extra in ((Opcode.INIT, 1, {}),
                    (Opcode.BIND_WORKER, 2, {"target_worker": 0})):
                writer.write(encode_request(opcode, request_id, **extra))
                await writer.drain()
                check(await receive(reader), request_id)
        reader, writer, _ = connections[0]
        writer.write(encode_request(Opcode.PUT, 3, key=key, value=value))
        await writer.drain()
        check(await receive(reader), 3)

        async def responses(reader, pending):
            nonlocal completed
            while True:
                response = await receive(reader)
                if not pending:
                    raise ValueError("response has no outstanding admission")
                request_id, scheduled_ns = pending.popleft()
                check(response, request_id, value)
                latencies.append(time.monotonic_ns() - scheduled_ns)
                completed += 1
                finished.set()

        receivers = [asyncio.create_task(responses(reader, pending))
                     for reader, _, pending in connections]
        start = time.monotonic_ns() + 5_000_000
        total = args.rate * args.duration_ms // 1000
        next_request = 0
        next_window = start + 100_000_000
        end = start + args.duration_ms * 1_000_000
        while True:
            now = time.monotonic_ns()
            due = min(total, max(0, (now - start) * args.rate // 1_000_000_000))
            # Scheduling is independent of responses. At most 64 requests per connection
            # and 64 KiB transport backlog; missed offers remain visible, never backfilled.
            while next_request < due:
                reader, writer, pending = connections[next_request % args.clients]
                scheduled_ns = start + next_request * 1_000_000_000 // args.rate
                request_id = next_request + 10
                next_request += 1
                if len(pending) >= 64 or writer.transport.get_write_buffer_size() >= 64 * 1024:
                    missed += 1
                    continue
                writer.write(encode_request(Opcode.GET, request_id, key=key))
                pending.append((request_id, scheduled_ns))
                admitted += 1
            if now >= next_window:
                windows.append({"elapsed_ns": now - start, "scheduled": next_request,
                                "admitted": admitted, "completed": completed,
                                "outstanding": admitted - completed, "missed": missed})
                next_window = now + 100_000_000
            for task in receivers:
                if task.done():
                    task.result()
            if now >= end:
                break
            await asyncio.sleep(0.0005)
        # Offers not scheduled before the finite deadline are client misses, not server work.
        missed += total - next_request
        drain_deadline = time.monotonic() + 5
        while completed < admitted:
            for task in receivers:
                if task.done():
                    task.result()
            if time.monotonic() > drain_deadline:
                raise TimeoutError("admitted responses did not drain within five seconds")
            finished.clear()
            try:
                await asyncio.wait_for(finished.wait(), min(0.1, max(0.001, drain_deadline - time.monotonic())))
            except TimeoutError:
                pass
        # Collect server observations only after client responses have drained.
        for task in receivers:
            task.cancel()
        await asyncio.gather(*receivers, return_exceptions=True)
        reader, writer, _ = connections[0]
        writer.write(encode_request(Opcode.STATS, total + 100))
        await writer.drain()
        stats = await receive(reader)
        check(stats, total + 100)
        latencies.sort()
        p99 = latencies[max(0, (len(latencies) * 99 + 99) // 100 - 1)] if latencies else None
        return {"qualification": "unqualified-local-experiment", "scheduled": total,
                "admitted": admitted, "completed": completed, "missed": missed,
                "p99_scheduled_to_response_ns": p99, "slo_ns": args.slo_us * 1000,
                "all_offers_served_and_p99_within_slo": missed == 0 and p99 is not None and p99 <= args.slo_us * 1000,
                "windows": windows, "server_stats": stats.value.decode("ascii")}
    finally:
        for task in receivers:
            task.cancel()
        await asyncio.gather(*receivers, return_exceptions=True)
        for _, writer, _ in connections:
            writer.close()
        await asyncio.gather(*(writer.wait_closed() for _, writer, _ in connections), return_exceptions=True)


def run(args):
    args.output.mkdir(parents=True)
    binary = args.binary.resolve()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    metadata = environment(binary)
    metadata["driver_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    metadata["configuration"] = {key: str(value) if isinstance(value, Path) else value
                                  for key, value in vars(args).items()}
    codec_path = ROOT / "sdk/python/src/glifistore/protocol.py"
    codec_digest = hashlib.sha256(codec_path.read_bytes()).hexdigest()
    metadata["python_wire_codec_sha256"] = codec_digest
    metadata["binary_sha256"] = digest
    (args.output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    command = [str(binary), "--port", "0", "--shard-pairs", "1", "--log-format", "json",
               "--max-connections", str(args.clients + 4), "--read-helper-cap", str(args.helpers)]
    if args.helpers:
        command += ["--read-helper-fixed-owner", "0"]
    process = None
    result = {"status": "failed", "command": command}
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    try:
        with (args.output / "daemon.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
            deadline = time.monotonic() + 10
            port = None
            while time.monotonic() < deadline and process.poll() is None:
                for line in (args.output / "daemon.log").read_text().splitlines():
                    try:
                        record = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if record.get("event") == "listen":
                        port = record.get("cleartext_port")
                if port:
                    break
                time.sleep(0.01)
            if not port:
                raise RuntimeError("daemon did not publish its own listen port")
            result.update(asyncio.run(asyncio.wait_for(workload(args, port), args.duration_ms / 1000 + 15)))
            result["status"] = "completed"
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        if process is not None:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                result["status"] = "failed"
                result["shutdown"] = "killed after deadline"
            result["daemon_returncode"] = process.returncode
            if process.returncode != 0:
                result["status"] = "failed"
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        result["server_user_cpu_seconds"] = after.ru_utime - before.ru_utime
        result["server_system_cpu_seconds"] = after.ru_stime - before.ru_stime
        result["binary_unchanged"] = hashlib.sha256(binary.read_bytes()).hexdigest() == digest
        result["codec_unchanged"] = hashlib.sha256(codec_path.read_bytes()).hexdigest() == codec_digest
        if not result["binary_unchanged"] or not result["codec_unchanged"]:
            result["status"] = "failed"
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key not in ("windows", "server_stats")}, indent=2))
    return 0 if result["status"] == "completed" else 1


if __name__ == "__main__":
    raise SystemExit(run(arguments()))
