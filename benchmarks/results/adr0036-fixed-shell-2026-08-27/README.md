# ADR 0036 fixed-shell diagnostic — 2026-08-27

Status: local microbenchmark on an uncommitted candidate. This is not V11/V12, CI evidence, or a
production-runtime result.

## Setup

- Apple M4, 10 physical/logical CPUs;
- macOS Darwin 25.6.0, arm64;
- repository revision `31bd35f`, dirty candidate tree;
- `build/macos-release`, hot-path phases disabled;
- one real `PairReadGeneration`, one same-key mutation per publication;
- 20.000 incremental publications, 2 warmups, 11 measured repeats;
- fixed candidate alternates two preallocated 512-byte shell slots;
- execution order reverses on alternating repeats.

Command:

```text
build/macos-release/glifistore_generation_shell_benchmark \
  --ops 20000 --warmup 2 --repeats 11
```

## Median

| Construction | publications/s | ns/publication | Delta throughput |
| --- | ---: | ---: | ---: |
| default `make_shared` | 2.935.241 | 340,69 | baseline |
| fixed shell | 2.849.747 | 350,91 | **−2,91%** |

Every fixed run performed 20.000 shell placements and 19.998 reincarnations; checksum was identical
in all rows. “Shell allocation” in `raw.tsv` counts allocator placement calls into preallocated
storage, not heap allocations.

## Decision

The candidate removes the shell/control-block heap allocation but does not improve this
single-Writer median. The 2,91% diagnostic regression is below ADR 0036's 5% V12 rejection
threshold, but this test cannot satisfy that gate: it has no paired Reactor, Reader concurrency,
lane pressure, affinity, latency percentiles, or allocator-tail measurement.

Do not integrate it into the official runtime yet. The next experiment must determine whether the
cost comes from backing-storage shared ownership and checked occupancy, then run the complete
worker-affine two-thread V11/V12 matrix. A faster lifetime representation is acceptable only if it
keeps the bank alive through final weak/control-block destruction by construction and retains the
same shutdown proof.

## Inline-owner isolation

The follow-up `raw-inline-pool.tsv` compares two implementations with the same reservation,
publication-token, adoption and reclaim protocol:

| Construction | Median publications/s | Median ns/publication | Delta |
| --- | ---: | ---: | ---: |
| pool with shared backing owner | 2.552.241 | 391,81 | baseline |
| structurally nested inline pool | 2.744.394 | 364,38 | **+7,53%** |

The inline wrapper owns shell bytes and pool in one non-movable lifetime domain, destroys the pool
before the bank, never exposes built `shared_ptr`s, and therefore uses no backing-owner refcount.
Its 10.000-publication concurrent Writer/Reader stress is TSan-clean.

This isolates shared backing ownership as a real cost, but does not rescue the design. In the same
run plain `make_shared` reached a 2.953.901 publications/s median; the inline pool remained 7,09%
behind. The comparison includes slot protocol work on only one side and is not V12, but it confirms
that retaining `allocate_shared` and its control block is the wrong endpoint. The next candidate
must construct and retire the generation object directly in a slot without a shared control block.

## Direct-object construction

`raw-direct-ring.tsv` records the next candidate. It uses the same production delta builder and
validation routine, but constructs the concrete generation directly in one of two alternating
slots with `construct_at`. There is no allocator, `shared_ptr`, control block or backing refcount.

| Construction | Median publications/s | Median ns/publication | Delta |
| --- | ---: | ---: | ---: |
| default `make_shared` | 2.791.071 | 358,29 | baseline |
| direct-object ring | 3.267.796 | 306,02 | **+17,08%** |

All 22 rows have the same checksum. The differential test compares 256 successive official and
direct publications across epoch, visibility, delta cardinality, retained versions and returned
value. Rejected builds preserve the old current object and leave the destination slot reusable.

This is the first positive construction result, but the ring is intentionally Writer-synchronous:
it destroys the previous object immediately and has no Reader publication token, safe epoch, cold
borrow, merge ownership, shutdown protocol or durable refresh. It validates the direct-object
endpoint only. V11/V12 remain open until those lifetime protocols are composed and the official
worker-affine matrix is run.

## Direct-object slot protocol

`raw-direct-slot-pool.tsv` composes direct construction with the complete bounded publication
protocol: reservation before Store authority, release/acquire `{epoch, slot}` token, Reader
adoption, safe-epoch reclaim, cold-borrow frontier and terminal shutdown. Unlike the ring, epoch
zero also lives directly in a slot; there is no generation `shared_ptr` or control block.

| Construction | Median publications/s | Median ns/publication | Delta vs `make_shared` |
| --- | ---: | ---: | ---: |
| default `make_shared` | 2.965.966 | 337,16 | baseline |
| direct-object ring | 3.473.881 | 287,86 | +17,12% |
| direct-object slot pool | 3.186.299 | 313,84 | **+7,43%** |

The measured safety protocol costs 8,28% against the unsafe synchronous ring, while retaining a
7,43% advantage over `make_shared` in this construction microbenchmark. Every row has the same
checksum; the extra initial placement explains 20.001 direct-pool constructions. Release,
ASan+UBSan and TSan focused tests are green, including 10.000 concurrent publications.

This result is still not V11/V12. The benchmark executes Reader adoption on the Writer thread and
does not include lane wakeup, socket ownership, worker affinity, merge, durable refresh, latency
percentiles or shutdown concurrency. It is sufficient to keep the direct QSBR design alive and to
reject immediate runtime integration until a two-thread worker-affine prototype exists.
