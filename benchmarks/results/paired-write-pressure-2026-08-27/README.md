# Paired write-pressure attribution — 2026-08-27

## Scope

This local campaign measures the official `ShardPairRuntime` through the real TCP server. It does not use the
historical experimental paired reactor. The tree was based on `31bd35f`; the benchmark and telemetry changes
were intentionally uncommitted while the A/B candidates were evaluated.

Host: Apple arm64, macOS, Apple LLVM 21.0.0. CPU affinity is advisory and was not requested. Thermal and
background noise were visible in the sample spread, so throughput and latency values below are diagnostic,
not release evidence.

## Exact structural result

The benchmark now excludes seed traffic from paired and durable counters. The measured write path shows:

- volatile, one client: 20,000 mutations, 20,000 Writer batches, 20,000 publications;
- volatile, four clients, 1% writes: 2,000 mutations, 2,000 Writer batches, 2,000 publications;
- volatile, four clients, 5% writes: 10,000 mutations, 10,000 Writer batches, 10,000 publications;
- volatile, four clients, 10% writes: 20,000 mutations, 18,296 Writer batches and publications (1.093 records/batch);
- durable-group, four clients, 10% writes: 2,000 mutations, 1,000 Writer batches/publications and 1,000 physical
  commit batches (2 records/batch).

Therefore voluntary Writer waiting is the wrong first optimization for the tested volatile mixed workloads:
each write is normally followed by a connection-local GET visibility barrier. The publication is semantically
required before that connection can continue.

## Candidate decisions

Two bounded candidates were implemented and removed after measurement:

1. Pressure-credit wait, 8 us: batch ratio stayed flat (1.072 to 1.070), median throughput fell about 14%, and
   p99 rose about 64% in the immediate A/B.
2. Inter-arrival wait, 2 us: batch ratio rose to 1.379, but median throughput fell to about 431 kops/s and p99
   rose to about 494 us. Rejected.
3. Completion wakeup gate: wakeup emissions fell from 20,000 to about 2,828, but median throughput fell about
   3% and p99 rose about 25% in the immediate A/B. Rejected because tail latency is the primary gate.
4. Completion ACK coalescing without wait: adjacent completions already present for one connection shared a
   socket drain. In the active 90/10 case it reduced useful feedback cadence and fell from about 725 kops/s to
   458 kops/s in the short cross-check, with p99 rising from 0.51 ms to 1.80 ms. Rejected; immediate ACK drain
   remains part of the effective flow-control loop.
5. Fixed 32-entry COW tracking arrays: the isolated generation scope improved, but constructing 96 empty
   `shared_ptr` slots per publication regressed the stable release run from about 947 kops/s to 853 kops/s and
   raised p99 from 192 us to 260 us. Rejected.

No rejected behavior remains in the source tree.

## Accepted slice

- Exact post-seed counters for Writer batches, publication records and completion notifications.
- Real 99/1, 95/5 and 90/10 TCP workload modes.
- Benchmark output for coalescing and publication ratios.
- Runtime observability for the same counters.
- Regression assertions tying completed records to batches, publications and notifications.
- Compile-time lab attribution for batch collection, Store apply, generation construction/publication,
  completion delivery/wakeup and Reactor completion phases. Normal builds compile these scopes out.
- Reusable Writer-thread-local COW tracking scratch for current and post-cut builders. Capacity is reserved
  once at the normative 32-record publication bound and cleared between publications; no object reachable by
  Reader points into the scratch.

## Accepted generation-builder A/B

The reusable scratch retained the immutable generation topology and removed the three transient tracking
allocations after first use. In the sequential lab-attributed 90/10 p32 comparison, `generation_build` fell
from 897 ns to 739 ns per publication (-17.6%) and mean Writer service from 1,588 ns to 1,442 ns (-9.2%).

The least noisy seven-repeat release pair measured 947 kops/s, p99 192 us for the baseline and 953 kops/s,
p99 191 us with reusable scratch. Other same-machine runs varied substantially with host scheduling and thermal
state, so the accepted claim is the direct phase/service reduction with no observed tail regression—not a
portable end-to-end throughput percentage. Exact comparison rows are retained in `generation-builder-ab.tsv`.

## Accepted hot large-GET scatter A/B

Lab-only socket outcome counters showed that small responses are already well amortized: at pipeline 32 the
64 B GET workload drained about 24.4 responses per successful socket write, with no `EAGAIN` or partial write.
The 64 KiB contiguous path instead copied every materialized value a second time into the connection output
buffer. It reported 68 successful writes, 18 would-block outcomes and 18 partial writes per 1,000 responses.

The accepted cleartext hot path transfers one owning `OwnedValue` into the existing, bounded connection lease.
While that lease is active, the Reactor stops parsing that connection and resumes its buffered frames only
after `sendmsg` has drained header and value. This preserves response order and caps the state at one value per
connection; it does not retain a `RecordRef`, Segment, file handle, generation borrow, or mutable Index object.
TLS remains contiguous. Cold durable GET keeps its previous adaptive pipeline policy so disk materialization
can overlap the following request.

On the seven-repeat local 64 KiB/p32 comparison, median throughput moved from 33,110 to 41,957 ops/s (+26.7%)
and p99 from 4.99 ms to 3.35 ms (-32.8%). At 256 KiB/p8, throughput moved from 9,488 to 11,385 ops/s (+20.0%)
and p99 from 4.18 ms to 3.23 ms (-22.5%). The contiguous 256 KiB/p32 baseline exceeded the configured 4 MiB
output watermark; the bounded lease completed the same workload without increasing the watermark. This is a
capacity-policy result, not evidence that the old response codec was incorrect.

The threshold sweep rejected 4 KiB for pipelined hot scatter because its short local sample improved median
and p99 but had an ambiguous p99.9. At 8 and 16 KiB both throughput and measured tails improved. The final
threshold is therefore 8 KiB on the measured macOS row and a conservative 16 KiB on Linux/BSD until controlled
platform evidence exists. The older non-pipelined cold threshold remains 4 KiB. A 64 B seven-repeat cross-check
was neutral (-0.09% median throughput, improved observed p99), confirming that the common small-value branch is
unchanged. Exact results are retained in `hot-large-get-scatter-ab.tsv`; syscall outcomes are in
`socket-write-attribution.tsv`.

## COW directory attribution and rejected root candidate

The lab build now separates the immutable Delta root copy, chunk/block/page clones, arena record materialization
and generation-shell allocation. In the representative 90/10 run, generation construction averaged 1.08 us:
directory-root copy was 114 ns, chunk clone 112 ns, block clone 91 ns, page clone 62 ns, record store 50 ns and
generation shell 44 ns. The 16×16×16 hierarchy is already the balanced branching point for the normative 4,096
pages; changing one fanout merely moves shared-pointer traffic between levels.

A bounded inline root for the normative 16 chunks was implemented and removed after measurement. Enlarging the
co-allocated `DeltaState` disturbed allocator/cache behavior: root copy rose to 168 ns, shell allocation to 88 ns
and total generation construction to 1.68 us. It also amplified all three clone costs. The vector root remains;
no rejected inline representation or freelist is present in the runtime.

## Rejected kqueue user-event wakeup

On macOS, replacing the portable nonblocking pipe with `EVFILT_USER/NOTE_TRIGGER` reduced direct completion
notification cost from about 703 ns to 575 ns (-18%). It nevertheless worsened the latency objective in both
seven-repeat orders. Forward order moved p99 from 151 to 156 us and p99.9 from 186 to 195 us; reverse order moved
p99 from 149 to 154 us and p99.9 from 201 to 210 us. Throughput and p50 improved slightly, but the likely
scheduler/coalescing change is not acceptable for a tail-first design. The candidate was removed completely:
macOS/BSD retain the pipe and Linux retains eventfd. Exact rows are in `cow-and-wakeup-ab.tsv`.

## Cost ranking from the lab build

For volatile 90/10, four clients, p32, the representative per-mutation/per-batch means were:

1. completion pipeline resume: 2.98 us (useful parsing/dispatch of the next buffered request);
2. completion socket flush: 3.01 us (platform I/O and response drain);
3. generation build: 1.15 us per publication;
4. completion wakeup: 0.76 us;
5. Store apply: 0.74 us, including encode/copy (~89 ns) and Index publish (~265 ns);
6. generation pointer publish: ~44 ns;
7. completion delivery into the SPSC: ~53 ns.

These are instrumented scopes and some Writer scopes contain Store leaf scopes; their percentages must not be
treated as mutually exclusive wall-clock percentages. The ranking and mean costs are still useful. The result
rejects the premise that Index optimization is the next priority. It also shows that `pipeline_resume` is not
pure overhead: it performs the next useful GET/barrier work.

For durable-group 90/10, the physical Store apply/commit scope was ~4.64 ms per two-record batch, while
generation build was ~10.4 us and pointer publication ~155 ns. Physical durability dominates by orders of
magnitude; no publication micro-optimization can make synchronous device semantics behave like volatile RAM.

## Next measured target

The transient Builder tracking allocation and the avoidable hot large-value output copy are removed. Directory
spine and kqueue alternatives are now measured and rejected. Further removal of COW ownership/control-block cost
would require the explicit generation-slot/QSBR protocol of ADR 0036 rather than another local container trick;
that proof boundary is the next architectural block. The hot-scatter threshold still requires controlled Linux
and BSD rows; platform results remain separate.

## Verification

- release build completed;
- focused hot/cold scatter tests: 3/3;
- complete parallel CTest matrix: 51/52 passed; the fixture test completed its assertions but crossed its
  15-second budget under concurrent sanitizer/CTest load, then passed 1/1 serially in 3.92 seconds;
- allocation-fault campaign: passed;
- ASan/UBSan paired suite: 57/57, plus the hot/cold scatter filters;
- TSan paired suite: 57/57, plus the hot/cold scatter filters;
- crash matrix: passed;
- assurance validation: 30 requirements, 30 hazards, 26 gates, 2 waivers, zero warnings.

These checks do not change the project claim ceiling: GlifiStore remains an architectural prototype.
