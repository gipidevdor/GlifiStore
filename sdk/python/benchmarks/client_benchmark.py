#!/usr/bin/env python3
"""Reproducible external-server benchmark for the Python GlifiStore SDK."""

from __future__ import annotations

import argparse
import asyncio
import statistics
import sys
import threading
from pathlib import Path
from time import perf_counter
from typing import Callable

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE_ROOT / "src"))

from glifistore import (  # noqa: E402
    AsyncClient,
    Client,
    ClientConfig,
    PipelineOpcode,
    PipelineRequest,
    PipelineResponse,
    __version__,
)


def material(
    operations: int,
    workers: int,
    pipeline: int,
    owner_for: Callable[[bytes], int],
) -> list[list[list[PipelineRequest]]]:
    quotas = [
        operations // workers + (1 if worker < operations % workers else 0)
        for worker in range(workers)
    ]
    requests: list[list[PipelineRequest]] = [[] for _ in range(workers)]
    candidate = 0
    while any(quotas):
        key = f"python-bench-{candidate:012d}".encode()
        owner = owner_for(key)
        if quotas[owner]:
            value = bytes([candidate & 0xFF]) * 64
            requests[owner].append(PipelineRequest(PipelineOpcode.PUT, key, value))
            requests[owner].append(PipelineRequest(PipelineOpcode.GET, key))
            quotas[owner] -= 1
        candidate += 1
    batch_frames = pipeline * 2
    return [
        [
            worker_requests[offset : offset + batch_frames]
            for offset in range(0, len(worker_requests), batch_frames)
        ]
        for worker_requests in requests
    ]


def batch_rounds(
    batches: list[list[list[PipelineRequest]]],
) -> list[list[PipelineRequest]]:
    rounds: list[list[PipelineRequest]] = []
    for offset in range(max(map(len, batches), default=0)):
        rounds.append(
            [
                request
                for worker_batches in batches
                if offset < len(worker_batches)
                for request in worker_batches[offset]
            ]
        )
    return rounds


def validate_responses(batch: list[PipelineRequest], responses: list[PipelineResponse]) -> None:
    if len(responses) != len(batch) or not all(response.succeeded for response in responses):
        raise RuntimeError("pipeline request failed")
    for index in range(1, len(batch), 2):
        if responses[index].value != bytes(batch[index - 1].value):
            raise RuntimeError("pipeline GET value mismatch")


def run_pipeline_concurrent(client: Client, batches: list[list[list[PipelineRequest]]]) -> float:
    barrier = threading.Barrier(len(batches) + 1)
    failures: list[str] = []

    def run(worker_batches: list[list[PipelineRequest]]) -> None:
        barrier.wait()
        for batch in worker_batches:
            responses = client.execute_pipeline(batch)
            if len(responses) != len(batch) or not all(
                response.succeeded for response in responses
            ):
                failures.append("pipeline request failed")
                return
            for index in range(1, len(batch), 2):
                if responses[index].value != bytes(batch[index - 1].value):
                    failures.append("pipeline GET value mismatch")
                    return

    threads = [threading.Thread(target=run, args=(worker_batches,)) for worker_batches in batches]
    for thread in threads:
        thread.start()
    barrier.wait()
    started = perf_counter()
    for thread in threads:
        thread.join()
    elapsed = perf_counter() - started
    if failures:
        raise RuntimeError(failures[0])
    return elapsed


def run_pipeline_sequential(client: Client, batches: list[list[list[PipelineRequest]]]) -> float:
    started = perf_counter()
    for worker_batches in batches:
        for batch in worker_batches:
            responses = client.execute_pipeline(batch)
            if len(responses) != len(batch) or not all(
                response.succeeded for response in responses
            ):
                raise RuntimeError("pipeline request failed")
            for index in range(1, len(batch), 2):
                if responses[index].value != bytes(batch[index - 1].value):
                    raise RuntimeError("pipeline GET value mismatch")
    return perf_counter() - started


def run_batch(client: Client, batches: list[list[list[PipelineRequest]]]) -> float:
    started = perf_counter()
    for batch in batch_rounds(batches):
        responses = client.execute_batch(batch)
        validate_responses(batch, responses)
    return perf_counter() - started


async def run_pipeline_async(
    client: AsyncClient, batches: list[list[list[PipelineRequest]]]
) -> float:
    async def run(worker_batches: list[list[PipelineRequest]]) -> None:
        for batch in worker_batches:
            responses = await client.execute_pipeline(batch)
            if len(responses) != len(batch) or not all(
                response.succeeded for response in responses
            ):
                raise RuntimeError("pipeline request failed")
            for index in range(1, len(batch), 2):
                if responses[index].value != bytes(batch[index - 1].value):
                    raise RuntimeError("pipeline GET value mismatch")

    started = perf_counter()
    await asyncio.gather(*(run(worker_batches) for worker_batches in batches))
    return perf_counter() - started


async def run_batch_async(client: AsyncClient, batches: list[list[list[PipelineRequest]]]) -> float:
    started = perf_counter()
    for batch in batch_rounds(batches):
        responses = await client.execute_batch(batch)
        validate_responses(batch, responses)
    return perf_counter() - started


def report(
    *,
    name: str,
    runtime: str,
    execution: str,
    version: str,
    workers: int,
    pipeline: int,
    operation_count: int,
    samples: list[float],
) -> None:
    rates = [operation_count / sample for sample in samples]
    print("# glifistore Python client benchmark")
    print(
        f"# sdk_version={version} runtime={runtime} execution={execution} "
        f"workers={workers} pipeline_pairs={pipeline} operations={operation_count}"
    )
    print(
        f"name={name} sdk_version={version} runtime={runtime} execution={execution} "
        f"workers={workers} pipeline_pairs={pipeline} operations={operation_count} "
        f"samples={len(samples)} median_seconds={statistics.median(samples):.9f} "
        f"min_seconds={min(samples):.9f} max_seconds={max(samples):.9f} "
        f"median_ops_per_second={statistics.median(rates):.3f} "
        f"min_ops_per_second={min(rates):.3f} max_ops_per_second={max(rates):.3f}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--ops", type=int, default=200_000, help="PUT/GET pair count")
    parser.add_argument("--pipeline", type=int, default=64, help="PUT/GET pairs per batch")
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--runtime", choices=("sync", "async"), default="sync")
    parser.add_argument(
        "--execution",
        choices=("concurrent", "sequential", "batch"),
        default="concurrent",
        help=(
            "concurrent uses one thread/task per Worker; sequential drains Workers in order; "
            "batch delegates Worker grouping to execute_batch"
        ),
    )
    args = parser.parse_args()
    if min(args.workers, args.ops, args.pipeline, args.repeats) <= 0 or args.warmup < 0:
        parser.error("numeric options must be positive (warmup may be zero)")
    if args.runtime == "async" and args.execution == "sequential":
        # Async sequential is still one event loop; keep the flag for report parity.
        pass

    operation_count = args.ops * 2
    config = ClientConfig(host=args.host, port=args.port)

    if args.runtime == "sync":
        runners = {
            "concurrent": run_pipeline_concurrent,
            "sequential": run_pipeline_sequential,
            "batch": run_batch,
        }
        runner = runners[args.execution]
        with Client.connect(config) as client:
            if client.worker_count != args.workers:
                parser.error("server Worker count does not match --workers")
            batches = material(args.ops, args.workers, args.pipeline, client.worker_for)
            for _ in range(args.warmup):
                runner(client, batches)
            samples = [runner(client, batches) for _ in range(args.repeats)]
        name = (
            "python_client_batch_read_after_write"
            if args.execution == "batch"
            else "python_client_pipeline_read_after_write"
        )
    else:

        async def measure() -> list[float]:
            async with await AsyncClient.connect(config) as client:
                if client.worker_count != args.workers:
                    raise SystemExit("server Worker count does not match --workers")
                batches = material(args.ops, args.workers, args.pipeline, client.worker_for)
                runner = run_batch_async if args.execution == "batch" else run_pipeline_async
                for _ in range(args.warmup):
                    await runner(client, batches)
                return [await runner(client, batches) for _ in range(args.repeats)]

        samples = asyncio.run(measure())
        name = (
            "python_async_client_batch_read_after_write"
            if args.execution == "batch"
            else "python_async_client_pipeline_read_after_write"
        )

    report(
        name=name,
        runtime=args.runtime,
        execution=args.execution
        if args.execution == "batch"
        else (args.execution if args.runtime == "sync" else "concurrent"),
        version=__version__,
        workers=args.workers,
        pipeline=args.pipeline,
        operation_count=operation_count,
        samples=samples,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
