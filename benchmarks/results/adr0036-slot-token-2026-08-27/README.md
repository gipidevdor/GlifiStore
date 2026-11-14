# ADR 0036 prototype publication token — local diagnostic A/B

Date: 2026-08-27
Host: Apple arm64, macOS, Apple Clang
Scope: lab-only `glifistore_paired_benchmark`; not the official production publication path.

## Method

The pre-existing Release benchmark binary, built before this change from the same `31bd35f`-based
dirty worktree, provided the pointer-descriptor baseline. It was run first, then the dedicated
benchmark target was rebuilt with the `{epoch, slot}` token and run with the same arguments:

```text
--ops 300000 --keys 4096 --value-bytes 64 --repeats 7 --warmup 1
--batch-records 32 --batch-wait-us 2
```

Each executable alternates the current Store control and experimental paired implementation inside
its run. The two protocol executables were sequential rather than interleaved with each other, so
the result is diagnostic and does not close ADR 0036 V11/V12.

## Result

Against the immediately preceding pointer run, raw paired medians changed:

- GET 100%: 13.049 to 12.858 Mops/s (-1.46%); p99 remained 125 ns and p99.9 167 ns;
- GET/PUT 95/5: 11.068 to 10.872 Mops/s (-1.77%); p99 remained 167 ns and p99.9 208 ns.

The in-run current-Store controls moved -1.63% (GET) and -0.64% (mixed). Normalizing paired against
those controls gives +0.17% for GET and -1.14% for mixed. This does not show a >5% regression, but
it is insufficient to accept a production candidate: it is neither worker-affine production PUT
nor an interleaved protocol A/B.

A second token run printed the new telemetry and observed 30,617 real slot reincarnations across
30,620 publications, with zero publication backpressure. Its control path slowed materially at the
same time, so its throughput/tail rows are retained in `summary.tsv` as a host-noise observation,
not used for the primary comparison.

## Decision

Keep the packed token in the isolated prototype because it removes pointer ABA ambiguity and its
local cost is within diagnostic noise. Do **not** promote the slot pool to production. ADR 0036
remains proposed; durable refresh/crash integration and a production-congruent interleaved V11/V12
campaign remain open.

GlifiStore remains an architectural prototype.
