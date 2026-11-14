# Manual pages

Portable [mdoc(7)](https://man.openbsd.org/mdoc.7) sources for every binary the Runtime component
installs.

| Page | Section | Covers |
| --- | --- | --- |
| `glifistore.7` | 7 | Overview |
| `glifistored.8` | 8 | `glifistored` |
| `glifistore_demo.1` | 1 | `glifistore_demo` |
| `glifistore_inspect_segment.1` | 1 | `glifistore_inspect_segment` |
| `glifistore_verify_store.1` | 1 | `glifistore_verify_store` |
| `glifistore_backup_store.1` | 1 | `glifistore_backup_store` |
| `glifistore_migrate_store.1` | 1 | `glifistore_migrate_store` |
| `glifistore_repair_store.1` | 1 | `glifistore_repair_store` |
| `glifistore_rebuild_index.1` | 1 | `glifistore_rebuild_index` |

## Build and install

```bash
cmake -S . -B build -DGLIFISTORE_MAN_DATE="August 1, 2026"
cmake --build build --target glifistore_manpages
cmake --install build --component Runtime
```

Pages install under `${CMAKE_INSTALL_MANDIR}/man{1,7,8}` from `GNUInstallDirs`: usually
`share/man` on Linux and BSD, and the Homebrew prefix on macOS. Installs are uncompressed by
default, so they do not need `gzip`; packaging may compress afterwards, or compress at install time:

```bash
cmake -S . -B build -DGLIFISTORE_COMPRESS_MANPAGES=ON
```

## Validate

```bash
./scripts/validate-manpages.sh             # build/macos-ci/man or build/man, else the sources
./scripts/validate-manpages.sh build/man   # a given build tree
```

The script needs `mandoc` (OpenBSD, macOS, or the `mandoc` package on Linux) and falls back to
`groff -mandoc -Tutf8` where only groff exists.
