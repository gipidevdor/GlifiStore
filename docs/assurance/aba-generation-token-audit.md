# ABA, generation, token and reuse audit

**Descriptive** · repository version 0.1.x · reviewed 2026-08-30

The manual reuse audit requested by `GS-CONCUR-MEM-001`: how each audited reused identity keeps a
stale holder from acting on a newer object. It is a descriptive audit, not a formal proof.
Normative behavior stays in the [concurrency and memory model](../spec/concurrency-memory-model.md)
and accepted ADRs; the audit does not raise the claim ceiling or make the ADR 0036 slot pool the
default.

## Reuse inventory

| Resource | Identity and width | Owner | Publication / acquire | Release and reclamation | Reuse precondition and ABA result |
| --- | --- | --- | --- | --- | --- |
| Reactor connection slot | `{slot:32, generation:32}`; generation zero is invalid | owning Reactor | token is stored in the platform poller and async completions; every callback calls `connection(token)` | poller removal, cancellation epoch increment for a cold read, socket/TLS/output teardown | generation advances before the slot returns to `free_slots_`; at `UINT32_MAX` the slot is permanently retired. Therefore an old token cannot equal a later live identity. Regression: `connection token generation retires a slot instead of wrapping into an ABA identity` |
| Cold-read cancellation | stable slot address + `uint64_t` epoch + captured expected value | Reactor writes; disk reader observes | task captures expected with relaxed load; close increments with release; worker checks with acquire | task completion destroys `PreparedColdRead` and Reactor releases its generation lease | the parent connection slot retires after at most `2^32-1` reuse cycles, so the subordinate 64-bit cancellation epoch cannot wrap while that slot remains reusable |
| Server MPSC cell | modular `size_t` cell sequence and enqueue position | one successful producer CAS, then sole consumer | producer release-publishes the cell; consumer acquire-loads sequence | consumer destroys the optional and release-publishes the next producer sequence | capacity is explicitly below half the unsigned range; a stalled reservation prevents the consumer and later producers from lapping it. Modular ordering uses unsigned subtraction, so wrap is defined and unambiguous. Boundary regression covers `SIZE_MAX` |
| Paired/server SPSC cell | modular `size_t` head/tail and a power-of-two slot | exactly one producer / one consumer | head/tail release publishes ownership; opposite side acquire-loads | consumer resets payload before tail release | capacity is below half range and distance never exceeds capacity; unsigned wrap preserves `head-tail`. Oversized ambiguous capacities are rejected |
| Mutation payload slot | `SlotId:32` plus FIFO `sequence:uint64_t` and logical byte cursors | Reader acquires/releases; Writer borrows between queue edges | mutation queue release publishes immutable slot payload; completion queue release returns ownership | FIFO completion validates sequence/cursor and returns slot to a pre-reserved free list | counters reset only when every slot is free and byte/sequence cursors agree. Near `uint64_t` exhaustion, new admission backpressures until quiescent reset; no live identity wraps |
| ADR 0036 generation slot | `{epoch:48, slot+1:16}` release token; zero invalid | shard Writer; one Reader adopts | Writer initializes the full graph then release-stores token; Reader acquire-loads and validates both slot and epoch | retired slot requires `slot.epoch < reader_safe_epoch` and `pins == 0`; shutdown also requires Reader quiescence | slot reuse increments a non-wrapping epoch. Publication rejects epochs above `2^48-1`; it never truncates or wraps. Regression exercises 10,000 reincarnations and the maximum encodable token |
| Default immutable generation (ADR 0036 Alternative A) | allocation identity plus generation epoch and `shared_ptr` ownership | shard Writer publishes; Readers hold counted `ReadLease` | pointer release/acquire publishes immutable contents | old generation remains in the retired vector until the safe Reader epoch / active-lease handshake permits erase | an allocator may reuse an address only after the previous ownership graph is destroyed; the counted adoption fence prevents reclaim during raw-pointer adoption. Address equality alone is never accepted as a generation identity |
| Incremental merge cut | pinned source generation/Segment set plus cut epoch | Writer-private `PairReadMerge` | no Reader sees an unfinished merge; only the finished immutable generation is published | completion transfers pins into the next generation; abandonment destroys the merge and its cut pin before terminal reclaim | a new merge obtains a new owned cut; a stale cut cannot survive reset or be consulted after publication/shutdown |
| Durable `RecordRef` / Segment | Store ID + Segment ID + `GenerationId:32` + sequence + checked offset/size | Manifest/catalog owns exact Segment generation; prepared reads pin it | catalog snapshot/capture transports an owning durable pin | pin destruction permits old Segment mapping/file retirement | compaction checks generation exhaustion and refuses wrap. Decode validates Segment generation, sequence, opcode, encoded size and range; same numeric offset in a reincarnated Segment is not the same identity |
| Manifest and compaction authority | Store ID + monotonic `manifest_generation:uint64_t`; intent binds exact old/next Manifests | catalog publisher under publication mutex | durable rename/directory-sync boundaries publish authority; recovery decodes and validates exact transition | old authority is retired only after durable next authority and cleanup rules | planners/transitions reject `UINT64_MAX`; generation never wraps. A stale intent is accepted only when it exactly matches a valid old→next transition |
| Shard execution token | binary `IDLE/EXECUTING`; deliberately not a historical identity | current CAS winner | acquire-CAS grants sole mutation ownership; release-store relinquishes it | after release, pending work is checked and token is reacquired when necessary | `IDLE→EXECUTING→IDLE` is harmless ABA: observers need only current exclusivity, not holder identity. The post-release pending check, not a generation counter, prevents lost work |
| Global routing revision | non-wrapping `uint64_t` even revision; odd means write in progress | writers serialized by atomic flag | seq_cst odd/payload/even publication; readers return only payload between equal even revisions | no external borrowed lifetime | concurrent readers cannot combine algorithm and seed from different revisions; revision exhaustion terminates instead of wrapping |

## The ABA interleaving

The dangerous interleaving: T1 reads identity A; T2 retires A, publishes B, then reuses the backing
resource as an apparent A; T1 resumes and acts on it.

- Connections prevent the final equality: reuse advances the generation, and exhaustion retires the
  slot.
- Generation slots prevent it with the epoch component and refuse epoch wrap; reclamation also waits
  for the Reader safe epoch and pins.
- Shared-pointer generations prevent reuse while T1 can own the old graph; the active-lease adoption
  fence closes the interval before ownership is visible to the reclaimer.
- Queue and mutation-slot counters use bounded unsigned modular distance. A stalled owner prevents a
  complete lap, and mutation logical counters reset only at global slot quiescence.
- The execution token is the only intentional A→B→A case: A means "unowned", not an object
  identity. A caller that loses the CAS never retains authority, and pending work is rechecked after
  release.

## Residual limits

- Generation and Manifest exhaustion branches and modular boundary helpers are tested, but `2^32`,
  `2^48` and `2^64` operational lifetimes are not exhaustively executed.
- The shared-pointer adoption and reclamation argument is manually reviewed and dynamically
  stressed; it is not a machine-checked C++ memory-model proof.
- Physical power-loss timing and storage-controller reordering are outside this host audit.
