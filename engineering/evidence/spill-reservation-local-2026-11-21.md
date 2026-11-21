# macOS spill reservation regression, 2026-11-21

Scope is a local architectural-prototype check on macOS 27.2.1 arm64.
This record does not accept ADR 0046, close a gate, establish CI evidence or certify durability.
Base revision `da44183903d394a6e58b84a61006813e9e94f9f7` plus the source fingerprints below identifies the tested change.

## Failure and correction

The first complete CTest campaign passed 71 of 72 groups. The main native group had two failures
in `tests/unit/memory_reserve_tests.cpp`, where an 8 MiB spill region fell back to anonymous memory.
An isolated system-call probe reproduced `F_PREALLOCATE` with contiguous allocation returning
`ENOSPC`, followed immediately by successful noncontiguous allocation of all 8,388,608 bytes.
The machine had more than 170 GiB available. Contiguous allocation failure was incorrectly treated
as filesystem exhaustion before trying separate extents.

The allocator now retries with `F_ALLOCATEALL` after every failed contiguous request, including
`ENOSPC`. Exhaustion of the second reservation retains the existing anonymous-memory fallback.
The new `spill_contiguous_reserve` fault site makes the fragmentation regression deterministic.
No persistent authority, acknowledgement, format or wire behavior changes.

## Verification on the corrected native sources

- `./scripts/dev.sh format` and `format-check` passed with clang-format 21.1.8.
- The native test filter `spill` passed 4 cases, including injected fragmentation and exhaustion.
- CTest `-R '^glifistore_tests$'` passed its main native group, 258.33 seconds.
- After building all final targets, CTest `-E '^glifistore_tests$'` passed the other 71 groups,
  including allocation-fault and process-crash checks, 369.09 seconds.
- Together the two final invocations cover all 72 CTest groups on the corrected native code.
- Memory-order inventory regenerated and checked, 1,125 explicit orders in 18 reviewed domains.

| Source | SHA-256 |
| --- | --- |
| `src/core/memory_reserve.cpp` | `5338e79a642c18bb2813e4bb4568adeef5d279a5aa2b9e68782b022bf9d374a0` |
| `include/glifistore/core/fault_injection.hpp` | `8e74d7204ea57d6c579a1f79d2ac2271eead3c39c8fb52513abae4600e53ead1` |
| `tests/unit/memory_reserve_tests.cpp` | `01499b147d12f6727078b63b3f6c47efe603dd543697567a0ec8cd4ebe642363` |

## Residual risk

Evidence is local to this macOS row. Linux and BSD reservation paths are unchanged and were not
run on their native systems. Copy-on-write filesystems or snapshots may still need additional
blocks on later page writes, as recorded by HAZ-043. The gate state remains unchanged.
Initial failure logs and the successful rerun logs are retained in the local GDev assurance
archive; temporary executables, stores and the build tree are removed after recording evidence.
