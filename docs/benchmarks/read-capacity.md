# Offered-rate read-capacity experiments

ADR [0049](../adr/0049-adaptive-read-capacity.md) implements an opt-in volatile GET helper
prototype. Helper cap defaults to zero. No shipped capacity curve or threshold is qualified.
Logical key ownership, one authoritative Writer, wire v2 and mutation acknowledgements remain
unchanged. A helper is read-only and never owns a socket.

Build the local Release daemon, then use a **new** evidence directory:

```sh
python3 scripts/benchmark_read_capacity.py \
  --binary build/macos-release/glifistored \
  --output /absolute/scratch/read-capacity-h1 \
  --rate 15000 --duration-ms 5000 --clients 8 --value-size 1024 --helpers 1
```

Compare `--helpers 0`, `1`, `2` and `4` in separate directories at the same offered rate, SLO,
value size and client count, on an otherwise quiet machine. The driver starts its own volatile
loopback daemon on a port chosen by the daemon, uses the canonical Python SDK codec and validates
every request id, response status, owner and value. One owner and one key deliberately isolate
that local experiment; they do not prove sibling isolation or shared-resource capacity curves.

The sender follows monotonic scheduled arrival times independently of response completions.
Each connection has at most 64 outstanding requests and a bounded transport backlog. Offers
that cannot enter are counted as `missed`; they are not silently converted into lower offered
load. Latency starts at the scheduled arrival, including client scheduling delay. A slow client
can invalidate a capacity inference. The output distinguishes scheduled, admitted and completed
work, retains 100ms cumulative windows and records server CPU including startup/drain. SLO is
p99 scheduled-to-response latency; the driver never asserts every individual latency met it.

Limits: 500–30000ms, at most two million scheduled GETs, 64 connections, values up to 256KiB
and four helpers in this driver. The runtime hard helper cap is 16. Failures, server log, binary
and codec hashes, native input hashes and environment are retained. Existing evidence directories
are refused. `qualification=unqualified-local-experiment` is intentional even when all offers
complete below the p99 SLO. Qualifying a profile requires repeated sustained mixed/multi-owner
campaigns, stable backlog, CPU/cost and workload-drift evidence on the target platform.

For a manual fixed experiment:

```sh
build/macos-release/glifistored --port 7379 --read-helper-cap 2 --read-helper-fixed-owner 0
```

Without the explicit fixed owner or C++ externally qualified profiles, a nonzero cap creates
sleeping helpers that remain unassigned. Durable helper configurations and invalid owner/cap
combinations are rejected. Full cells and keys above 1024 bytes use the ordinary Reader. One
GET per connection may be delegated; later requests wait for its response, and large values
are materialized on the primary from the exact protected selection.

STATS exposes ready/delegated volatile reads, configured cap, fixed-experiment mode, control
steps, soft CPU target, occupied/arming/active/draining allocations, grants and retirements.
Ready reads are dispatch observations, not external offered arrivals or sustainable capacity.
Occupied includes draining allocations until safe acknowledgement. Hardware concurrency and
process CPU are estimates, never physical-core reservations or energy measurements.

GDev procedure `GS-READ-CAPACITY` executes the driver regression and native controller/helper
checks from `.gdev-assurance/project.eld`. Local checks leave release gates and human review open.
