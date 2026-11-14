# Residency model

Status: descriptive of implemented ownership plus future residency constraints
Applies to: volatile memory and persistence v1
Owner: storage-engine maintainers
Last reviewed: 2026-11-14

Mutable Index state and volatile Segments are resident in RAM. Durable active and sealed Segments
are fixed-size files; the runtime keeps bounded descriptors/generation pins and performs positional
reads rather than requiring complete Segment residency. Immutable `ReadGeneration` metadata is
resident, while cold value bytes are materialized from the pinned file generation.

```text
ACTIVE volatile: resident and writable only by its owner mutation executor
ACTIVE durable: file-backed mutable Segment + published immutable generation pin
SEALED durable: immutable file generation, read through a shared pin/descriptor
VALUE HOT: bytes already carried by the adopted ReadGeneration
VALUE COLD: positional read and validation required
DEAD: retired and reclaimable after safety conditions
```

`/tmp` is never an architectural default because it may be a RAM-backed or automatically cleaned
filesystem. Data directories are configurable. Production defaults will follow each OS convention
without hard-coding paths in the core.

Proposed [ADR 0045](../adr/0045-memory-resident-durable-reads.md), as amended by
[ADR 0046](../adr/0046-unified-memory-reserve.md), serves durable GETs from in-memory Segment images
admitted under one memory reserve, and keeps this positional path for Segments without an image.
The same reserve governs the Worker Index, which is never refused and is paged to swap or to an
unlinked spill file beyond it. See the [lean memory algorithm](lean-memory-algorithm.md).

```text
VALUE RESIDENT: decoded from the Segment image with the cold path's validation, no file read
```

The current durable runtime does not use `mmap` as its value-access contract. Any future mapping or
eviction policy must use high/low watermarks, must not invalidate active generation pins, and must
separate page residency from durability.

Public C++ reads are owning copies; no public pinned-read handle is implemented. Internal generation
pins keep the exact Segment generation usable independently of pathname retirement. Descriptor
close, deletion, unmapping (if introduced), and Segment reuse must wait until the relevant internal
reader/reclamation obligation is released.
