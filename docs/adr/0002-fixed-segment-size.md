# ADR 0002: Fixed 64 MiB Segments

- Status: accepted
- Date: 2026-07-11

The physical append, persistence, checksum, residency, vacuum, and reclaim unit is fixed at 64 MiB
(67,108,864 bytes). This balances rotation overhead with reclaim granularity. Records are variable
size, 8-byte aligned, immutable, and do not cross normal Segment boundaries.
