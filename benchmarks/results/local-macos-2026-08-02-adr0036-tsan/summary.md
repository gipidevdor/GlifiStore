# ADR 0036 prototype + production-baseline under ThreadSanitizer

Date: 2026-08-02
Preset: `macos-tsan` (Apple Silicon)
Claim ceiling: lab evidence only; does not accept ADR 0036 for production landing.

## Command

```text
./build/macos-tsan/glifistore_tests 'ADR 0036'
./build/macos-tsan/glifistore_tests 'paired Store'
```

## Results

| Filter | Tests | Failures |
| --- | ---: | ---: |
| `ADR 0036` (V1/V2/V3/V7/V9/V13) | 6 | 0 |
| `paired Store` (incl. overwrite storm) | 10 | 0 |

No ThreadSanitizer reports on stderr for these runs.
