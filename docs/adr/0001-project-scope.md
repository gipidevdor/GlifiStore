# ADR 0001: Project scope

- Status: accepted
- Date: 2026-07-11

GlifiStore is a native C++ key-value store with one logical key-space, a binary wire protocol,
automatic many-core execution, and bounded resource use. Text-protocol compatibility, SQL, and
generic querying are non-goals: they enlarge the correctness surface without serving the exact-key
fast path.
