# ADR 0004: Index as derived state

- Status: accepted
- Date: 2026-07-11

The Index is required for fast lookup but is reconstructible from Segments. Rebuild compares full
key bytes and retains the valid Record with the highest monotonic sequence. The newest tombstone or
expired Record removes visibility. A persisted Index checkpoint may later accelerate startup but
cannot become the only recovery source.
