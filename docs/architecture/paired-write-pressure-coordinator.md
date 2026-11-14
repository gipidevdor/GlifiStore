# Paired write-pressure coordinator

**Descriptive** · paired runtime write-path coordination planned for 0.1.0 · reviewed 2026-08-27

How a future coordinator should size Writer publication batches from pressure that already exists,
which completion policies measurement rejected, and what evidence must exist before the policy is
wired. It is a measured design, not yet a normative behavior change:
[ADR 0031](../adr/paired-reader-writer-shards.md),
[ADR 0032](../adr/0032-paired-concurrency-embedded-store.md),
[ADR 0037](../adr/0037-shard-execution-token-flat-combining.md) and the
[mutation lifecycle](../spec/mutation-lifecycle.md) remain authoritative.

## Measured boundary

The official TCP runtime already admits adjacent PUT and ERASE frames into a bounded 32-record
mutation window and treats GET as a visibility barrier. In the measured 99/1 and 95/5 workloads,
every mutation formed its own Writer batch and publication; at 90/10 with four clients, the mean was
only 1.05–1.09 records. The cause is not mainly a failure to drain the SPSC: a connection-local GET
commonly follows the mutation and cannot pass until the generation containing that mutation is
visible.

Two attempts to wait briefly for future work increased tail latency and reduced throughput. The
coordinator must therefore consume work that is already available; it must not manufacture pressure
by delaying a volatile single PUT.

## Inputs

The coordinator takes one immutable snapshot after the first dequeue:

```cpp
struct PublicationPressureSnapshot {
    std::size_t queue_records;
    std::size_t queue_bytes;
    std::uint64_t oldest_age_ns;
    std::size_t completion_free_records;
    std::uint64_t foreground_read_operations;
    std::uint64_t foreground_p99_ns;
    std::size_t retired_generations;
    std::size_t merge_post_entries;
    std::uint64_t maintenance_debt_bytes;
    DurabilityMode durability;
};
```

Every counter is bounded or sampled. No decision may allocate, acquire a global lock, inspect
client-controlled unbounded state or add an Index lookup.

## Policy

For volatile mutations:

1. dequeue the first admitted mutation immediately;
2. snapshot queue and completion capacity;
3. drain only records already available, capped by 32 records, the byte budget, completion capacity
   and the oldest admitted deadline;
4. close immediately when the queue is empty, a GET or barrier is waiting, completion capacity is
   exhausted, generation or merge pressure requires relief, or the publication deadline is reached;
5. build and publish one immutable generation for the drained FIFO prefix;
6. deliver one completion per mutation without changing response order.

For durable-group, the configured physical group deadline may wait, because it is already part of
the durability policy. Closure must be the first of: records, bytes, oldest deadline, completion
capacity, shutdown, foreground fairness or a device coordinator decision. Platform defaults remain
evidence-row specific.

The decision output is explicit:

```cpp
struct PublicationDecision {
    std::size_t maximum_records;
    std::size_t maximum_bytes;
    std::uint64_t close_deadline_ns;
    bool close_now;
    bool foreground_priority;
    bool maintenance_may_advance;
};
```

Identical snapshots and configuration must produce identical decisions. Policy code is a pure
function; queue draining and publication remain in the sole Writer.

The design does not claim that a larger average batch is always better. A larger batch is accepted
only when it comes from already queued pressure and improves throughput without violating the
tail-latency and correctness gates.

## Completion delivery

SPSC delivery itself costs tens of nanoseconds. On macOS the pipe wakeup costs roughly 0.6–1.2 µs,
but a notification gate that removed about 86% of wakeups worsened p99, because completions waited
behind more Reader work. Notification coalescing is therefore not accepted as a generic policy.

Any future platform optimization must preserve this anti-lost-wakeup rule:

- a completion queued while the Reader may sleep must create an observable poller event;
- the Reader may omit a new event only while it has explicitly promised to drain the completion
  lane again;
- a syscall reduction is insufficient evidence if p99 or p99.9 regresses.

Linux `eventfd` and the BSD/macOS wakeups are separate platform implementations. The macOS
`EVFILT_USER` candidate reduced direct notification time by about 18% but consistently worsened p99
and p99.9 in two seven-repeat orders, and was removed. The pipe therefore remains the BSD/macOS
baseline, and Linux retains `eventfd`. A future retry needs a materially different scheduling
protocol and must beat the tails, not merely syscall time.

ACK socket-drain coalescing follows the same rule. A bounded look-ahead that combined only adjacent,
already-queued completions still regressed 90/10 throughput and tail latency, so immediate ACK drain
remains the baseline. A lower syscall count alone is not sufficient when the ACK participates in
useful client and connection flow control.

## Publication builder scratch

The Writer reuses two thread-local tracking buffers: one for the generation being built and one for
the post-cut delta during incremental merge. Each reserves the normative 32-mutation bound once and
is cleared before reuse. The scratch holds only temporary owning `shared_ptr` entries used while
wiring the immutable COW directory; the published generation owns every reachable page, block and
chunk independently.

Reader ownership, epoch publication and reclamation are unchanged. Two buffers are required because
the current and post-cut builders can coexist. A fixed array was measured and rejected: constructing
all 96 empty ownership slots on every publication regressed the release workload, although it
improved the isolated scope.

## Maintenance quanta

Maintenance must never reuse foreground publication batching as an implicit lock. Each shard-local
quantum is bounded by all three limits:

- maximum copied records and bytes;
- maximum elapsed time;
- foreground pressure, checked between quanta.

After a quantum, maintenance yields publication authority when foreground mutations exist, a
completion lane is near capacity, foreground p99 crosses its configured threshold, or generation
retirement is close to its bound. Reclaim and capacity safety may bypass the latency suspension,
but the bypass and the debt must remain observable.

## Evidence required before wiring

- timed distribution of queue depth and of already-available records at first dequeue;
- publication build time, separated from pointer publication;
- completion lane free capacity and wake/resume time;
- p1/p32/p64/p128 for 99/1, 95/5, 90/10 and RAW;
- durable group fill, close reason, queue wait and physical commit;
- maintenance and rotation publication wait, with and without foreground pressure;
- ASan/UBSan, TSan, fault injection, stop/drain, disconnect and recovery campaigns.
