# ADR 0003: Log-indexed storage

- Status: accepted
- Date: 2026-07-11

Writes append immutable Records to an active Segment and publish a positional `RecordRef` in the
Index. Reads use exact-key Index lookup followed by validated `segment base + offset` access.
Obsolete Records remain until whole-Segment reclaim or vacuum; no published Record is updated in
place.
