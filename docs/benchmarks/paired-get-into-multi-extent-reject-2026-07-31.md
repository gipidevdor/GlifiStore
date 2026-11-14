# Paired get-into and multi-extent scatter rejection

**Historical** · paired daemon, cold GET output path · 2026-07-31

## Outcome

**Rejected (P1 promotion).** There is no candidate implementation of `get-into` or of a multi-extent
queue on the production daemon, and no A/B that beats the adaptive path already accepted on pipeline
**and** bounded memory. Promoting now would reopen lifetime and backpressure without proof.

The cleartext cold GET stays on the model of
[`paired-scatter-output-2026-07-29.md`](paired-scatter-output-2026-07-29.md): below 4 KiB, on TLS,
or after observed pipelining it uses the contiguous frame; otherwise a single owning two-extent
lease (`sendmsg`) with a watermark and a disarm of the read interest.

## Why not promote

1. **No proof.** There is no branch, flag or harness that compares get-into / multi-extent against
   the adaptive path across 64 B–256 KiB and pipeline 1/8/32/128 with controlled memory and short
   writes.
2. **Lifetime.** `read_record_into` fills the reusable scratch of the cold helper; CRC/key/TTL are
   valid only once the read is complete. Pointing `sendmsg` at that scratch without an owning slot
   or a QSBR pin until the final drain violates the contract already closed by the borrowed
   cold-read gate.
3. **Backpressure.** A multi-extent queue must preserve wire order, short writes, close/timeout,
   handoff and the aggregate watermark. The single lease avoids serializing pipelined cold GETs
   behind a slow drain — exactly the failure of the indiscriminate scatter candidate (−14.9% at 64 B
   p1; serialization at 64 KiB p8).
4. **TLS.** `SSL_write` does not keep the caller's iovecs; a contiguous materialization is still
   needed.
5. **QSBR.** A get-into that exposes generation-borrowed iovecs must retain the generation until the
   last short write (ADR 0031). The current owning lease releases the I/O pin at completion and does
   not couple slow clients to reclamation.

## Reopening criteria (all required)

- a fixed-capacity extent descriptor per connection, with credits for bytes **and** extents;
- an owning destination per response (arena slot) **or** an explicit QSBR pin until the final drain;
- a separate contiguous TLS comparator;
- deterministic tests: short write, slow client, pipeline order, close/reuse, shutdown,
  retire/compaction pressure;
- interleaved A/B on hard-pinned Linux against the adaptive path across 64 B–256 KiB and
  pipeline 1/8/32/128;
- no worsening of batch p99 on mixed/pipelined beyond the combined spread.

As long as even one of these points is missing, the state stays **reject pending proof**, not
"deferred optimization implied by the scatter accept".

## Relation to the previous gates

| Gate | Relevant decision |
|---|---|
| inline cold-read | bounded slot; no raw pointer without an epoch pin |
| borrowed cold-read | I/O lease until completion; value still `OwnedValue` |
| scatter output | accept only adaptive 4 KiB+ cleartext non-pipelined; reject always-on scatter |
| this document | reject promotion of get-into / multi-extent without bounded proof and win |

## Verification of this rejection

- no change to the production data path;
- the numerical evidence for the scatter rejection stays in
  `benchmarks/results/paired-scatter-output-2026-07-29/`.
