# Paired Writer pressure and completion-coalescing A/B

Date: 2026-08-27  
Host class: local Apple Silicon, macOS, APFS  
Status: diagnostic evidence only; not release or cross-platform evidence

> **Durability correction:** the retained batch-4 strict-group cell reached a path that published
> its threshold-closing commit slot as deferred and therefore skipped the required final slot sync.
> Its throughput/latency and “physical durable commit” interpretation are invalidated. The
> completion-notification count remains a useful structural observation and is reproduced by the
> corrected strict run in `../local-macos-2026-08-27-strict-group-sync-fix/`.

## Question

Can the paired Writer reduce fixed handoff/completion cost without moving the PUT ACK,
visibility or durability linearization points, and can a short volatile coalescing delay improve
batch density without damaging GET tail latency?

## Accepted change

- Volatile Writer batches have explicit record and admission-byte bounds (32 records and 256 KiB
  by default). An oversized single admitted mutation remains a singleton; the byte bound does not
  redefine admission policy.
- All FIFO completion entries for a Writer batch are delivered before notification. The official
  single Reader is notified once per batch. The Reader remains the sole connection owner and
  still decides and emits every response individually.
- Admission retains a preallocated payload slot until the Reader drains the matching completion.
  Payload and completion rings share the same bounded lane capacity, so exhaustion remains a
  pre-Store overload and cannot strand an already-linearized mutation.
- No durable ordering, publication, read-after-write or ACK edge changes.

The retained durable-group run is `accepted-durable-group-batch4.txt`: 800 completed mutations,
200 four-record Writer batches, 200 publications, 200 commit-slot publications and 200 Reader
notifications. Relative to the old one-notification-per-mutation behavior, this removes 600 of 800
wake calls (75%) while preserving the exact batch membership. The matching baseline is
`../local-macos-2026-08-27-full-31bd35f-dirty/durable/durable-group-w1-c4-p32-batch4.txt`: it also
records 200 four-record Writer batches and 200 commits for 800 mutations, but 800 notifications.
Neither file crossed the required strict final synchronization and neither may be used for
durable-group throughput or latency. The corrected batch-4 run still records 200 immediate durable
batches and 200 notifications.

`accepted-volatile-read90-write10-w1-c4.txt` validates the final instrumented binary and its
counters (10,000 writes, 9,376 opportunistic batches/publications and 9,376 notifications). It
reports about 1.021 Mops/s with p99 165.2 microseconds, close to the same-day no-wait control. This
unisolated laptop remains too noisy for a small throughput claim; the structural counter
relationship is the retained result.

## Rejected changes

The following experiments are retained so that they are not rediscovered without evidence:

- A voluntary volatile wait of 1 microsecond increased average batch size from 1.0653 to 1.0891,
  but reduced the measured 90/10 throughput from about 1.016 Mops/s to 1.001 Mops/s and raised p99
  from 168.4 to 176.3 microseconds.
- A voluntary volatile wait of 2 microseconds increased average batch size to 1.1225, but reduced
  throughput to about 0.950 Mops/s and raised p99 to 220.4 microseconds.
- Durable early-close, quiet-window and adaptive-target prototypes changed physical commit counts
  and showed unstable APFS results. None demonstrated a repeatable tail-latency win while
  preserving the intended group density, so none is present in the final implementation.
- An earlier cross-batch notification gate reduced notifications more aggressively but delayed
  completions and damaged tail latency. The accepted implementation coalesces only within the
  Writer batch that already exists.

Files prefixed `adaptive*` and `candidate-*` are rejected prototypes. Some headers contain a stale
dirty SHA because CMake metadata had not been regenerated between local variants; filenames and
this decision record, not those embedded SHAs, identify the variants. Files prefixed `accepted-*`
were produced after regeneration and report `5feccd3-dirty`.

## Interpretation

This closes only the redundant completion-wakeup portion of the volatile/durable write-path fixed
cost. It does not prove that the Writer handoff itself is optimal, that APFS durable throughput is
portable, or that device-level group-commit coordination is complete. Those require an isolated
Linux/macOS matrix, hardware-counter capture and real filesystem/device measurements.
