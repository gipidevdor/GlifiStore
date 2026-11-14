# ADR 0036 prototype + production-baseline under ASan + UBSan

Date: 2026-08-02
Preset: `macos-asan` (AddressSanitizer + UndefinedBehaviorSanitizer; Apple Silicon)
Claim ceiling: lab evidence only; does not accept ADR 0036 for production landing.

## Command

```text
./build/macos-asan/glifistore_tests 'ADR 0036'
./build/macos-asan/glifistore_tests 'paired Store'
```

## Results

| Filter | Tests | Failures |
| --- | ---: | ---: |
| `ADR 0036` (V1/V2/V3/V6/V7/V9/V13) | 7 | 0 |
| `paired Store` (incl. overwrite storm) | 10 | 0 |

No AddressSanitizer or UndefinedBehaviorSanitizer reports on stderr for these runs.
