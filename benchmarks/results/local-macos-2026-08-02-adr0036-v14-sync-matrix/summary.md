# ADR 0036 V14 — full sync crash matrix (production-baseline)

Date: 2026-08-02
Preset: `macos-release` (Apple Silicon)
Claim ceiling: lab evidence for status-quo durable boundaries; does not land slot-pool.

## Command

```text
./build/macos-release/glifistore_crash_persistence --mode matrix
```

## Results

91 checkpoint cases (bootstrap / put / rotate / compact / compact-multi-*), exit 0.
No ACK-before-publish / recovery regressions observed on this host.

See `run.txt` for the per-boundary log.
