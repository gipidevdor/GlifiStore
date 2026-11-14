# Maintenance exact no-gain memo

Date: 2026-08-27  
Host class: local Apple Silicon, macOS, APFS  
Status: diagnostic evidence only; not release or cross-platform evidence

## Question

How expensive is an exact no-gain compaction scan, and what work can safely be avoided when the
same physical candidate is evaluated again without any relevant state change?

## Result

The seven measured public `Store::compact()` calls each verified 1,020 records / 255.077820 MiB
and correctly produced no replacement Segment. Median scan time was 140.012041 ms (about
1.78 GiB/s). The correction does not make this scan faster; it prevents the maintenance
controller from repeating it for an unchanged candidate until the bounded retry expires.

Compared with the retained 2026-08-27 baseline, whose median was 132.713250 ms, the individual
scan is 5.50% slower on this uncontrolled local run. This is noise/regression evidence, not an
improvement claim. The accepted benefit is structural and separately tested: one memo hit avoids
one complete 255.077820 MiB scan, while a changed candidate or pressure/emergency immediately
re-enables compaction.

The default no-gain threshold is now one exact attempt rather than eight. With the default
one-second minimum and 60-second maximum evaluation intervals, an unchanged candidate can no
longer cause an initial burst of eight identical scans. The retry remains bounded to 60 seconds
so time-dependent TTL reclaim is not postponed indefinitely.

## Proof boundaries

- `no-gain-scan.txt` measures the real v1 planning scan cost only.
- Unit tests prove unchanged-candidate suppression, immediate invalidation, pressure bypass and
  bounded expiry.
- New STATS fields expose suppressed scans and remaining retry time.
- This memo does **not** solve the monolithic useful-compaction transaction. The subsequent
  ADR 0039 block stages and verifies complete v1 outputs before the publication lease; its
  independent measurements are retained in `../local-macos-2026-08-27-pre-intent-staging/`.

Baseline: `../local-macos-2026-08-27-full-31bd35f-dirty/compaction/no-gain.txt`.
