# ADR 0036 two-thread direct publication diagnostic — 2026-08-27

Status: local candidate diagnostic. This is not V11/V12 acceptance, CI evidence, or a result from
the official `ShardPairRuntime`.

## Scope

The benchmark runs one persistent Reader thread and one persistent Writer thread over the real
`PairReadGeneration`. Both alternatives use the same 65-slot reservation, `{epoch, slot}`
release/acquire publication, safe-epoch reclaim, bounded exhaustion and shutdown protocol.

- `shared_slot_protocol` constructs each generation with the normal `make_shared` path;
- `direct_slot_protocol` constructs every generation, including epoch zero, directly in its slot;
- the Reader continuously adopts publications and verifies final visibility;
- the Writer publishes 20,000 same-key mutations from prebuilt Segment records;
- one publication latency is sampled every 64 mutations;
- implementation order reverses on alternating repeats.

The requested executor affinity was unavailable for both threads on this host. macOS affinity is
advisory even when accepted; this run reports `unavailable` and CPU `-1`, so it must not be called a
pinned or physical-core-isolated result.

Command:

```text
build/macos-native-release/glifistore_generation_publication_benchmark \
  --ops 20000 --warmup 3 --repeats 11
```

## Medians

| Protocol | publications/s | ns/publication | sampled p50 | sampled p99 |
| --- | ---: | ---: | ---: | ---: |
| shared generation | 1,190,497 | 839.99 | 1,000 ns | 1,417 ns |
| direct generation | 1,275,537 | 783.98 | 1,000 ns | 1,500 ns |
| delta | **+7.14%** | **-6.67%** | 0.00% | +5.86% |

Every run reached epoch/visibility 20,000 with checksum 40,000. Neither alternative silently
overwrote a slot. The direct candidate retained bounded live debt; observed high-watermarks vary
with scheduling because the Reader may lawfully skip intermediate immutable generations.

The throughput result is positive, but the sampled p99 is not: direct-object construction does not
yet prove a latency-tail improvement. The sample contains about 313 observations per repeat and is
only a diagnostic, not a percentile campaign.

## Reader GET workload

`raw-reader-get.tsv` repeats the campaign with one real same-key `PairReadGeneration::get` on every
Reader adoption. GET latency is sampled every 256 Reader operations. This is still an L1-hot,
two-byte value without protocol serialization or socket I/O, but it exercises the immutable delta
lookup and value materialization concurrently with publication.

```text
build/macos-native-release/glifistore_generation_publication_benchmark \
  --ops 20000 --warmup 3 --repeats 11 --reader-work get
```

| Protocol | publications/s | ns/publication | publication p50 | publication p99 | GET p50 | GET p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| shared generation | 817,965 | 1,222.55 | 1,375 ns | 1,750 ns | 83 ns | 250 ns |
| direct generation | 919,357 | 1,087.72 | 1,250 ns | 1,583 ns | 83 ns | 250 ns |
| delta | **+12.40%** | **-11.03%** | **-9.09%** | **-9.54%** | 0.00% | 0.00% |

Under this workload the earlier publication-tail warning does not reproduce: direct construction
improves both sampled publication percentiles while GET p50/p99 remain unchanged. It is stronger
directional evidence, but the dataset is intentionally tiny and the host still provided no
affinity guarantee.

## Safety rows

The same executable completed 5,000 publications for both alternatives under ASan+UBSan and TSan.
The focused direct-pool unit/stress suite also passed 5/5 under both sanitizers. Repeated adoption
now avoids a redundant `reader_safe_epoch` release store when the frontier is unchanged; advancing
the frontier still performs the release required by Writer reclaim.

## Decision

Keep the direct-object protocol as the integration candidate. Do not close V11/V12 and do not
replace the official runtime yet. The next proof must embed it in the experimental paired path and
measure real Reader GET work, Writer lane wakeup, publication delay and GET p99/p99.9. Linux rows
must additionally demonstrate actual CPU pinning; macOS rows must remain explicit about advisory or
unavailable affinity.
