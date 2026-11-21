# TCP load concentrated on one shard

This workload measures the existing owner-bound wire v2 server for HAZ-045.
It changes client placement in the benchmark, with fixed Store ownership and
one bound connection per raw-wire client. It does not implement balancing,
forwarding or retries, and does not resolve HAZ-046 or the residual HAZ-044 paths.

Build and check its contract with the GDev procedure `GS-CAPACITY`, or directly
with `python3 engineering/tools/gdev_verify.py capacity`. This configures the
local Release preset, builds the native test and server-benchmark targets,
then checks the planner, report validator and real TCP scenarios.

An example with three quarters of the clients on Worker 0 is

```sh
./scripts/dev.sh benchmark-server --ops 100000 --workers 2 --clients 4 \
  --pipeline 32 --workload get-only --owner-profile --hot-worker-clients 3 \
  --latency --warmup 1 --repeats 7
```

Omit `--hot-worker-clients` for the original round-robin client bindings.
Use `--hot-worker N` to choose another hot Worker. Every sibling must retain
at least one client. Profiles require raw wire, a nonempty workload on each
client, at most 64 Workers and 1024 clients. The existing public C++ client
benchmark remains available separately.

Each client gets its original quotient/remainder share of the finite key
operations. For 103 key operations and 10 clients, a 9-client hot Worker gets
93 operations and its sibling gets 10. The read-after-write workload emits
two wire requests per key operation. Mixed workloads keep the existing
global-index selection of writes, so their GET/PUT mix can differ by owner.
Both counts are reported explicitly.

`owner_profile` rows identify repeat and owner and retain

- clients, planned GET/PUT requests and validated completed requests;
- global sample duration, first-client completion and last-client completion;
- throughput using that owner's shared-start-to-last-completion window;
- measured latency count, p50, p95, p99, p99.9 and maximum.

The latency population follows the existing TCP benchmark. Read-after-write
samples both responses; GET-only and mixed workloads sample GET responses.
`owner_latency_scope` names that population. With stride 1 every eligible
response must have a sample. No samples means zero quantiles, rather than
invented timings. The `owner_profile_version=1` report checks each matrix
coordinate, exact quotas, request mix, completion windows, rates, quantiles
and their agreement with the common benchmark result and metadata.

Preserve successful raw output in the campaign directory, then produce a
checked JSON report

```sh
python3 scripts/owner_capacity_report.py report balanced.txt hot.txt --output owners.json
```

The files use the common benchmark environment and sampling metadata. Skewed
result names include the hot owner and number of its clients, preventing
silent comparison with the ordinary hosted TCP matrix. This opt-in report
does not enter the canonical hosted throughput gate.

For a bounded local comparison, the campaign runner retains balanced and
concentrated allocations with 4 and 10 clients, both GET-only and
read-after-write. The defaults are 40,000 key operations, one warmup and
seven measured repeats; operations are capped at 200,000. Use a new output
directory; an existing one is preserved and refused.

```sh
python3 scripts/benchmark_owner_capacity.py build/macos-release/glifistore_server_benchmarks \
  --operations 200000 --output /path/to/new/campaign
```

The runner records hardware observations, build flags, binary/source
fingerprints, argv and raw responses. It validates each run before marking
it successful, and preserves failed or timed-out attempts and diagnostics
in `campaign.json`. This is an uncontrolled local exploration rather than
an accepted hardware baseline.

This is a finite closed-loop load. A cold client's completion ends its
traffic while hot clients can keep working. Therefore 90 percent of total
operations on one Worker does not mean 90 percent of arrivals throughout
the run. Completion windows expose this loss of overlap. Latencies include
pipeline batching and have no coordinated-omission correction. These results
cannot establish an open-loop saturation threshold or sustained sibling
isolation. A shard pair has a Reader and Writer, and the load generator
shares their process and CPUs. The reported foreground-thread floor must
be compared with physical and logical CPU counts before discussing scaling.

Single-hot-key contention, fixed offered-rate sweeps, per-owner queue/CPU
decomposition, dedicated hardware and native Linux/BSD campaigns remain
separate work. HAZ-045 stays open after a successful tooling check.

The [ADR 0048 local copy comparison](../../engineering/evidence/hot-reader-copy-local-2026-11-21/README.md)
retains an old/candidate/old run order for a reduction in volatile Reader copies.
It includes the hot-owner cases and preserves the same finite-loop limitations.

The proposed [read-management plan](../architecture/adaptive-read-management.md) defines the
future sustained-capacity campaign and adaptive-helper qualification. Its candidate thresholds
and cost per useful read cannot be calibrated from the finite profiles above; the governor is
not implemented.
