# GlifiStore hot-path performance lab results

Generated locally on 2026-08-01 (Apple M4, `macos-release`).

> Architectural prototype / lab evidence only. Not a production capacity claim.
> Baseline: `benchmarks/results/local-macos-2026-08-01/` @ `629bc68`.

## Headline medians

| Cell | Baseline | After | Notes |
| --- | ---: | ---: | --- |
| store_get_copy 1t 64B | 2.20 M ops/s (455 ns) | 3.50 M (286 ns) | 15 repeats; target <300 ns |
| parallel GET worker-affine 2t | 5.07 M | 8.46 M | target ≥6 M |
| store_put 1t 64B | 378 k | ~369 k | PUT residual: Writer ack |
| parallel PUT worker-affine 2t | 506 k | ~504 k | |
| parallel PUT uniform 2t | 140 k | ~241 k | improved; still < 1t PUT |
| TCP w1 p1 | 47.6 k | 46.5 k | |
| TCP w4 p32 | 282 k | 302 k | 6.5× vs w1 p1 |
| TCP w4 p128 | 206 k | 312 k | p128 no longer regresses vs p32 |

## Artifacts

- Attribution (phases ON): `attribution-phases.txt`, `attribution-store-put-get.txt`
- GET/PUT cells: `store-get*.txt`, `store-put*.txt`, `parallel-*.txt`
- TCP: `server-tcp-*.txt`
- Narrative + diagrams: `docs/architecture/hot-path-performance-2026-08-01.md`
