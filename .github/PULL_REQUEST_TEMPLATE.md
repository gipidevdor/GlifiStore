## Summary

<!-- What changed, and why? -->

## Safety and architecture

- [ ] Length, offset and allocation arithmetic is checked.
- [ ] Ownership and view lifetimes are explicit.
- [ ] An ADR documents any persisted-format or architectural decision, when applicable.
- [ ] No unbounded work is added to a latency-sensitive path.

## Validation

- [ ] Formatting and `./scripts/dev.sh verify`
- [ ] Unit and integration tests
- [ ] ASan and UBSan
- [ ] TSan, when concurrency is affected
- [ ] Fuzz regression or corpus, when decoding or recovery is affected
- [ ] A focused benchmark, when a performance claim is made

## Notes

<!-- Risks, migration concerns, follow-up work, or intentionally omitted checks. -->
