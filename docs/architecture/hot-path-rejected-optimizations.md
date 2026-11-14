# Rejected hot-path optimizations

**Historical** · 2026-08-01 hot-path performance program · 2026-08-25

Lab notes on the optimizations considered during the
[hot-path performance program](hot-path-performance-2026-08-01.md) and rejected, with the reason
for each. Accepted residuals, such as the PUT ack cost and uniform embedded PUT versus the
owner-bound daemon model, are documented in that program record.

| Optimization | Why rejected |
| --- | --- |
| Widen SPSC / TCP / disk-read queues | Breaks bounded memory and backpressure contracts |
| Remove key/value length or overflow checks | Hard safety constraint |
| ACK before Writer publication | Breaks RAW / client-visible happens-before |
| Drop ReadLease seq_cst fence without epoch lease protocol | Generation UAF risk under reclaim |
| Silent dual-runtime or hash/routing changes | Silent-change ban; needs ADR |
| Disable OperationGuard on GET | Breaks close admission linearization |
| shared_ptr-free generation pool copied from experimental prototype without ADR | Format/runtime authority change; needs requirements + proofs |
| Benchmark threshold widening / synthetic-only wins | Forbidden gaming |
| Global mutex around phase counters | Would distort the paths being measured |
| Thread-local DeltaPage/Block/Chunk freelist with custom `shared_ptr` deleter | Measured regression on uniform parallel PUT (Apple Silicon lab); allocator/custom-deleter tax exceeded reuse benefit |
| Expecting sync Writer coalesce alone to lift blocking `Store::put` | Blocking put presents N≈1; coalesce needs `put_batch` (or equivalent) to stage multi-mutation drains |
| TLS `PairReadGeneration` shell freelist + custom `shared_ptr` deleter (ADR 0035) | Same-machine A/B: mild single-thread PUT gain, −16% worker-affine PUT 2t; rejected |
| Writer post-sync busy-spin (64 yields) before merge/park | Collapsed `store_put` ~378 k → ~170 k; dual-sided spin tax |
| Writer sync-before-merge reorder alone | No reliable 1t PUT win vs merge-first; affine mixed; keep merge-first |
| Conditional `notify_one` gated on `writer_waiting` | Same-machine A/B vs unconditional wake: `store_put` 359 k vs 349 k (noise), affine 502 k vs 509 k; no reliable win. Sync PUT parks the Writer every op, so notify almost always fires anyway. Evidence: `benchmarks/results/local-macos-2026-08-02-writer-waiting/` |
| Coalesce only buffered GET responses, stopping before the next mutation | Halved measured socket-write calls, but decoded the following non-GET twice and serialized the next Writer admission behind the socket drain. It produced no stable gain. The accepted completion-resume path instead preserves one-in-flight overlap. |
| BSD `write(2)` for contiguous socket output after `SO_NOSIGPIPE` | macOS pipeline-128 dropped from 269–273 k to ~200 k ops/s; instrumented syscall mean rose from ~2.49 µs to ~2.80 µs. Keep `send(2)` and its platform signal-suppression contract. |
| Skip fresh `steady_clock` reads when idle timeout is disabled | Best pipeline-128 samples were identical (~274 k ops/s); median movement followed host outliers and did not establish a repeatable gain. The extra branch/helper was not justified. |
| Suppress completion wakeup syscalls while the Reactor appears active | A generation/CAS handshake improved one-pair pipeline-128 by ~2%, but four-pair throughput fell from 404–427 k to 307–308 k ops/s. Kernel wakeups materially aided Reader/Writer scheduling under contention; the candidate was fully reverted. |
| Inline 16-entry Delta directory root in `DeltaState` | Enlarged the generation co-allocation and regressed lab generation build from ~1.08 µs to ~1.68 µs; root, shell and node-clone costs all increased. Vector root restored. |
| macOS/BSD `EVFILT_USER` completion wakeup | Direct notify fell ~18%, but two seven-repeat orders consistently worsened p99 by ~3–4% and p99.9 by ~5%; pipe restored, Linux eventfd unchanged. |
