# Paired cold-read scatter output

**Historical** · macOS arm64, paired daemon, Release · 2026-07-29

## Outcome

**Accepted in an adaptive, confined form.** The cleartext cold GET no longer copies `OwnedValue`
into the connection buffer when the payload is at least 4 KiB and the connection has not shown
pipeline use. Header and value stay as two stable owning extents until fully drained through
`sendmsg`. TLS keeps the contiguous frame because `SSL_write` does not offer the same scatter/gather
contract.

It is not an end-to-end zero-copy: the cold helper still reads and verifies the Record in its own
scratch and materializes an `OwnedValue`. The block removes exactly the following copy,
`OwnedValue -> Connection::output`, in the admitted case.

The indiscriminate scatter candidate was rejected. On 64 B values it lost 14.9% of throughput at
pipeline 1 and 3.1% at pipeline 32; on 64 KiB values a single lease also serialized pipeline 8. The
accepted version therefore applies two measured decisions:

- below 4 KiB it uses the contiguous path, because the fixed cost of `sendmsg` exceeds the copy;
- after observing more than one Store request in the same input turn, the connection stays
  contiguous to preserve the overlap between output and the next cold read;
- the scatter path keeps a single bounded lease, disarms the read interest during a partial write
  and introduces neither a multi-extent queue nor framing allocations per response.

## Ownership and backpressure invariants

- socket and lease belong exclusively to the Reader/Reactor;
- the lease owns the `OwnedValue`; no `RecordRef`, descriptor or generation pin is used after the
  release of the I/O task's QSBR lease;
- an already queued contiguous response always precedes the scatter header and value in the same
  `sendmsg`;
- buffer, header and value offsets advance separately across short writes and `EAGAIN`;
- the `maximum_output_bytes` watermark includes all pending extents;
- with an active lease the socket is not read: the pressure stays in the kernel receive buffer and
  user memory does not grow without bound;
- close, timeout, hangup, shutdown drain and slot reuse consider and destroy the lease;
- the Writer never writes to the socket and does not take catalog locks for the drain.

Aggregate and per-Reactor metrics: `output_scatter_responses`, `output_scatter_bytes`,
`output_scatter_partial_writes`, `output_scatter_completions`.

## macOS arm64 A/B

Release Apple LLVM 21, one pair, four clients, durable-periodic 1000 ms, key 16 B. The raw results
are in `benchmarks/results/paired-scatter-output-2026-07-29/`. They are sequential, unpinned runs
and the 64 KiB sample has three repetitions: the tails are local evidence, not a cross-platform
claim.

### Why the indiscriminate candidate was rejected

| 64 B workload | borrowed baseline | always scatter | throughput delta |
|---|---:|---:|---:|
| GET p1 | 112,984/s | 96,176/s | -14.9% |
| GET p32 | 186,012/s | 180,333/s | -3.1% |
| GET/PUT 99/1 p32 | 173,874/s | 159,199/s | -8.4% |

The adaptive 64 B run, which uses the contiguous frame, returns to 109,199/s at p1 and 185,355/s at
p32. The unpinned spread does not allow telling the residual from noise; above all, it removes the
structural regression of `sendmsg` on small payloads.

### Large gate, pipeline 1

| metric, 64 KiB | contiguous | accepted scatter | delta |
|---|---:|---:|---:|
| throughput | 27,602/s | 27,487/s | -0.4% |
| p50 | 95.1 us | 95.2 us | +0.04% |
| p95 | 128.9 us | 125.3 us | -2.8% |
| p99 | 173.0 us | 149.8 us | -13.4% |
| p99.9 | 319.2 us | 223.5 us | -30.0% |

Throughput is neutral in the sample and the high percentiles improve. Process RSS is not used for a
claim: it includes the durable dataset, the page cache and the sequential order of the runs.

At pipeline 8 the final version classifies the connection as pipelined and uses the contiguous path.
The local run has too much spread and thermal drift (33.1-40.4k/s) for a numerical comparison; the
deterministic test instead verifies that two large cold GETs preserve order and zero scatter leases
on that connection. A future multi-extent scatter will require a preallocated structure, a per-byte
watermark and a separate A/B before replacing this choice.

## Safety gate

- Debug suite: 461/461;
- ASan + UBSan: 461/461; full preset 37/37 including crash and allocation fault matrices;
- TSan: 461/461;
- strict ISO C++23/hardening build: pass;
- real short write with a narrowed receive buffer and a 768 KiB payload;
- large cold pipeline, response order and contiguous classification;
- TLS stays on the contiguous owning path;
- no change to wire v2, Record, Segment, Manifest or persistence v1.

## Next gate

The P0 output is closed for the direct cleartext path; `get-into` can still remove the
materialization in the helper, but it requires buffer ownership and checksum verification without
borrowing beyond the generation. The next block with more general impact is the preallocated
mutation arena, to remove `string`/`vector` for PUT/ERASE while keeping completion credit and byte
budget unchanged.

## Successor (2026-07-31)

The promotion of `get-into` / multi-extent queue was **rejected** without bounded proof and win:
[`paired-get-into-multi-extent-reject-2026-07-31.md`](paired-get-into-multi-extent-reject-2026-07-31.md).
The adaptive path of this gate remains the production data path.
