# Durable compaction model

**Descriptive** · oldest-prefix compaction ([ADR 0044](../adr/0044-bounded-prefix-compaction-remap.md))
on persistence format v1

This page states durable compaction as mathematics: definitions, theorems with proofs, the
assumptions they need, and the exhaustive checks that tie the model to the code. It defines no
behavior; ADR 0044, [persistence v1](../spec/persistence-v1.md) and the
[recovery matrix](../spec/recovery-state-matrix-v1.md) do. The [guide](durable-compaction.md) says
how the transaction runs; this page says why what it does is correct.

A claim here is a theorem of the model, proved in the text and checked by exhaustion over a finite
scope that the last section names. The runtime is then compared with the model over the same scope,
so the evidence is a refinement argument and not a list of examples.

## 1. Objects

A **Record** r has a sequence s(r) ∈ ℕ₊, unique within a Worker; a key κ(r); a kind ε(r) ∈
{put, erase}; an expiry x(r) ∈ ℕ (0 means none); and an encoded size z(r). P is the Record bytes
that one Segment holds: `kSegmentSizeBytes − kSegmentHeaderReservedBytes`.

A **history** H of one Worker is σ₁ … σₙ α: its n sealed Segments in Manifest order, then its
active Segment, each a finite sequence of Records in append order. Records(H) is their
concatenation.

**(O) Order.** Sequence numbers strictly increase along Records(H). Recovery fails closed on any
Worker whose Segments, in Manifest order, break this.

**Time** t ∈ ℕ is nanoseconds; t = 0 means no clock. A Record is *expired at t* when x(r) ≠ 0,
t ≠ 0 and x(r) ≤ t (`RecordView::expired`), and *hidden at t* when it is an erase or expired at t.
Hidden is monotone: hidden at t implies hidden at every t′ ≥ t.

**Decision.** W_H(κ) is the Record of κ with the greatest sequence. D_H(κ, t) is ⊥ when κ has no
Record or W_H(κ) is hidden at t, and W_H(κ) otherwise: the deciding Record, whose value is the
key's value. The visible state is V_H(t) = κ ↦ D_H(κ, t). Recovery computes exactly this ("the
newest key decision is a tombstone or expired").

**Index.** A partial map 𝕀 from keys to Records. At the time t_s of a snapshot the runtime keeps:

- **J1** 𝕀(κ) ∈ {⊥, W_H(κ)}: an entry names the key's newest Record;
- **J2** D_H(κ, t_s) ≠ ⊥ implies 𝕀(κ) = W_H(κ): every visible key is indexed;
- **J3** an entry names a put, never a tombstone.

Recovery at t_o establishes J with 𝕀(κ) = W_H(κ) exactly when D_H(κ, t_o) ≠ ⊥. A later PUT replaces
an entry, an ERASE removes it, and the lazy reclaim of an expired put removes only an entry that is
expired at the time of the read; none of them breaks J (given A1 below).

## 2. The operator

Fix 1 ≤ k ≤ n and a time t_c ≥ t_s. With P_k = Records(σ₁ … σ_k):

- Sel = { W_H(κ) : 𝕀(κ) ≠ ⊥ and W_H(κ) ∈ P_k }, the Index-referenced Records of the prefix;
- Drop = { r ∈ Sel : r expired at t_c };  Cp = Sel ∖ Drop;
- ω₁ … ω_m is the next-fit packing of Cp, in sequence order, into Segments of P bytes, each Record
  copied unchanged (same sequence, key, kind, expiry and value);
- **C_k(H) = ω₁ … ω_m σ_{k+1} … σₙ α**, and the transaction is *admissible* when m < k, so that it
  frees k − m ≥ 1 Segments (the blob profile also admits m = k when it reclaims blob bytes);
- the **remap** gives 𝕀′(κ) the copy of W_H(κ) when W_H(κ) ∈ Cp, ⊥ when W_H(κ) ∈ Drop, and 𝕀(κ)
  otherwise.

**Proposition 1.** C_k(H) satisfies (O), and (C_k(H), 𝕀′) satisfies J at t_c.

*Proof.* The outputs carry prefix sequence numbers in increasing order, all below those of
σ_{k+1} …, so O holds. A copy has the sequence of W_H(κ), so it is the newest Record of κ in the
new history (Lemma B below): J1 and J3. A key visible at t_c is a put not expired at t_c, hence not
expired at t_s, hence visible at t_s and indexed; if its newest Record is in the prefix it is in Cp
and 𝕀′ names its copy: J2. ∎

## 3. Prefix closure

**Theorem 1.** Let H satisfy O and 𝕀 satisfy J at t_s ≤ t_c. For every key κ and every time
t ≥ t_c, D_{C_k(H)}(κ, t) and D_H(κ, t) are both ⊥ or are the same Record (the first a copy of the
second). So V is unchanged from t_c on.

**Lemma A.** If W_H(κ) ∈ P_k, every Record of κ lies in P_k. *Proof.* A Record outside P_k has a
greater sequence than every Record of P_k by O, which contradicts W_H(κ) being the greatest. ∎

**Lemma B.** In C_k(H) the Records of κ are: (i) exactly its Records outside P_k, when W_H(κ) ∉ P_k;
(ii) exactly the copy of W_H(κ), when W_H(κ) ∈ Cp; (iii) none, otherwise. *Proof.* Only newest
Records are selected, so no older Record of the prefix is copied. In case (ii) Lemma A leaves no
Record outside the prefix. ∎

*Proof of Theorem 1.* (i) The newest Record is unchanged, so the decision is. (ii) The copy is a
put with the same expiry, so it decides at every t as the original did. (iii) W_H(κ) ∈ Drop is
expired at t_c, so hidden at every t ≥ t_c, and κ has no Record afterwards. If W_H(κ) ∈ P_k is not
indexed, J2 gives D_H(κ, t_s) = ⊥, so it is hidden at every t ≥ t_s, and κ has no Record
afterwards. Both sides are ⊥. ∎

**Corollary (composition).** Proposition 1 re-establishes the hypotheses, so a second transaction at
t_c′ ≥ t_c preserves V from t_c′ on, and so does any finite sequence of them.

**Theorem 1b (a prefix is necessary).** Let the operator of §2 rewrite a set S of sealed Segments
instead of σ₁ … σ_k. Theorem 1 holds for every history if and only if S is a prefix. If it is not,
some σ_i ∉ S precedes some σ_j ∈ S: take a put of κ without expiry in σ_i and a tombstone of κ in
σ_j. The tombstone is not indexed, so it is dropped, and the older put outside S becomes the newest
Record: D is ⊥ before and the put after. This is why ADR 0044 defers arbitrary ranges until they
retain every tombstone and expired put.

**The hypotheses are tight.** *Time (A1):* a put expiring in (t, t_c] is visible at t in H and is
dropped in C_k(H), so V changes before t_c. *Index (J2):* a visible newest Record in the prefix that
the Index does not name is not copied, and the key is lost. *Order (O):* with a tombstone of
sequence 5 in σ₁ and a put of sequence 3 in σ₂, the transaction over σ₁ drops the tombstone and the
put reappears; recovery rejects such a Worker.

## 4. Foreground mutations

Let F be PUTs and ERASEs executed after the snapshot and before the commit gate. Their Records are
newer than all of H, so they follow every Segment of the prefix: they land in α, or in Segments that
a rotation seals from it. Call a key *touched* if F names it. Let 𝕀_F be the Index after F.
The commit applies the **conditional remap**: for W_H(κ) ∈ Cp, if 𝕀_F(κ) = W_H(κ) replace it by the
copy; for W_H(κ) ∈ Drop, if 𝕀_F(κ) = W_H(κ) erase it; otherwise leave it.

**Theorem 2.** With H_F = H and F appended, and C_k(H) with F appended:

- (a) for every κ and t ≥ t_c the decisions of the two histories agree;
- (b) the remapped Index satisfies J for the new history at t_c;
- (c) copies superseded = |{κ ∈ Cp : touched}|, remapped = |Cp| − superseded, and expired
  erasures = |{κ ∈ Drop : untouched}|.

*Proof.* The newest Record of a touched key is its last foreground Record in both histories, and
sequence numbers are unique, so 𝕀_F(κ) is that Record or ⊥, never W_H(κ): the remap leaves it and
the decision is unchanged (no ABA). An untouched key has no new Record and 𝕀_F(κ) = 𝕀(κ), so
Theorem 1 and the remap of §2 apply. The counts follow. ∎

A read that lazily reclaims an expired entry during the build is a further Index-only step: it
removes an entry whose Record is hidden at the read, so it changes no decision, and the remap skips
that entry as it skips a touched one. It is outside the checked scope.

**Corollary (no journal).** The remap is a total function evaluated in O(|Sel|) steps that neither
allocates nor fails, so what F contains enters no condition of the commit. The retired design
(ADR 0043) kept a journal of J publications and aborted when the copy time L_k / c saw more, so at
mutation rate λ it completed only if L_k ≤ J·c / λ, a bound that excludes every large Worker as λ
grows. A foreground mutation now meets the transaction in two windows only: the commit gate of its
Worker, where it is refused with `sequence_conflict`, and, when it must rotate, the publication
lease, where it waits. For arrivals independent of those windows the refused fraction is the
fraction of time they are open, whatever the mutation rate. This remark is about the arrival model
and is not machine-checked.

## 5. The transition

For a Manifest M and a Worker with sealed entries e₁ … eₙ in Manifest order, let
**τ(M; k, m)**, for 1 ≤ k ≤ n and 0 ≤ m ≤ k, be M with its generation increased by one in which
e₁ … e_k are replaced by e₁⁺ … e_m⁺, where e⁺ is e with its generation increased by one. Outputs
keep their source's Segment ID, so Manifest order is preserved; nothing else changes (next Segment
ID, routing, other Workers, the active entry, e_{k+1} … eₙ).

**Theorem 3.** (a) τ(M; k, m) is a valid Manifest. (b) It is injective in (k, m); there are
(n + 1)(n + 2)/2 − 1 shapes. (c) The validator derives (k, m) from M and N and accepts N if and only
if N = τ(M; k, m). (d) The blob profile accepts only k = n.

*Proof.* (a) IDs stay strictly increasing and each Worker keeps one active entry. (b) In the
Worker's sealed list of τ(M; k, m) the first m entries are generation-bumped copies of e₁ …, and the
remaining n − k entries equal e_{k+1} …; both counts are readable from the list, even for k = m,
because an unchanged entry has the generation of its source and a bumped one has one more. (c) The
validator counts those two runs, forms the transition they name, and compares it with N. (d) is the
rule of ADR 0042. ∎

## 6. The choice of prefix

Let ℓ_i be the bytes of Index references into σ_i (expired puts the Index still holds count),
L_k = ℓ₁ + … + ℓ_k, o(L) = ⌈L / P⌉, and **ρ(k) = k − o(L_k)**, the Segments freed if the copies fit
the fewest outputs. For a copy budget B (0 means none) the admitted prefixes are
𝒜 = { k : B = 0 or L_k ≤ B }; since L_k does not decrease, 𝒜 = {1 … k_max}. The policy π picks, when
some k ∈ 𝒜 has ρ(k) ≥ 1, the smallest k that maximizes ρ; otherwise the longest admitted prefix, so
that the exact layout decides.

**Theorem 4.** (a) When a gain exists, π is the lexicographic maximum of (ρ(k), −L_k) over 𝒜:
the most Segments freed, then the least copy work. (b) L_π ≤ B whenever B > 0. (c) A larger budget
never promises less. (d) With B = 0, ρ(π) ≥ ρ(n): never less than the whole-set policy of ADR 0015.
(e) The blob profile admits only {n}.

*Proof.* (a) If k < k′ both maximize ρ and L_k = L_{k′}, then o is equal and ρ(k′) = ρ(k) + (k′ − k)
> ρ(k), which is absurd; so L increases strictly along the maximizers and the smallest one
minimizes it. (b) By definition of 𝒜. (c) 𝒜 only grows with B. (d) n ∈ 𝒜 when B = 0. ∎

**Theorem 5 (output count).** For Records of at most R bytes and total copied bytes L′ the
next-fit output count satisfies ⌈L′ / P⌉ ≤ m ≤ 1 + ⌊L′ / (P − R + 1)⌋. *Proof.* A Segment holds at
most P bytes. Every closed Segment holds more than P − R, because the next Record did not fit, so
(m − 1)(P − R + 1) ≤ L′. ∎ The estimate o(L_k) therefore falls short of the exact m by at most the
fragmentation these bounds allow, and exceeds it by the bytes of expired puts that the Index still
holds and cannot show. The transaction runs only if the exact m < k.

## 7. The persistent protocol

A **namespace state** is ν = (μ, ι, S, O): the authoritative Manifest μ ∈ {old, next}, whether the
intent exists, the sources S that still exist under their names, and for each output whether it is
absent, final or temporary. Segments listed by both authorities are never removed.

The **recovery function** R refuses (⊥, and removes nothing) unless

- μ = old, every source exists, and the intent exists or no output is final; or
- μ = next, every output is final, and the intent exists or no source exists.

Otherwise R(ν) removes the obsolete names (outputs under old, sources under next, and with no
intent only stale temporaries), then the intent, and keeps μ. The authority is chosen by Manifest
equality alone.

The **protocol** is: stage every output under a temporary name; write the intent; promote each
output; publish the next Manifest; retire each source in order; remove the intent. Reach is the set
of states after each step, with a crash between any two.

**Theorem 6.** (a) R is a total function on the lattice. (b) It is idempotent: a recovered state
recovers to itself. (c) It is crash-monotone: removing obsolete names from a state with an intent,
in any order, as a crash during recovery does, leaves the result unchanged. (d) Every state of Reach
recovers, and to the next authority exactly when the Manifest step has completed. (e) For every
state of Reach and every t ≥ t_c the recovered files decide V_H(t).

*Proof.* R reads ν only through μ, the intent and four predicates, 'every source exists', 'no
source exists', 'every output is final' and 'no output is final', and a removal changes one name.
Every class of states, and of partial removals, therefore occurs with at most two sources and two
outputs, so (a)–(d) are properties of a finite table, checked over every state of up to four of
each. (e) R selects μ by equality. The Segments of old are σ₁ … σₙ α and those
of next are ω₁ … ω_m σ_{k+1} … σₙ α, so their decisions are V_H(t) and V_{C_k(H)}(t), equal by
Theorem 1. ∎

This is a refinement mapping. Let the abstraction of a state be V of the history of its
authoritative Manifest. Every persistent step and every recovery step leaves it unchanged (the
Manifest step by Theorem 1) and a foreground mutation is a PUT or ERASE of the abstract map
(Theorem 2), so a crash at any point followed by recovery yields the abstract state the clients
last saw.

## 8. Assumptions and what is not covered

- **A1 Time.** Every time given to recovery, to reads and to a transaction is part of one
  nondecreasing sequence of positive values. The runtime does not enforce it; a clock that steps
  back is outside the model.
- **A2 Index.** J holds. Recovery establishes it and every mutation preserves it; the builder reads
  every Record it selects and aborts unless it is a put of the indexed key
  (`source_index_records_verified`).
- **A3 Order.** (O) holds, as recovery enforces.
- **A4 Durable history.** The history is what is committed. In periodic and group modes a newer
  Record may be published but not yet synchronized when its superseded sealed Record is dropped, so a
  crash can lose both; ADR 0044 records this pre-existing exposure and prefix selection does not
  change it.
- **A5 Atomic publication.** An Index update and its Record append are one step for the model:
  the commit gate drains Writers, and a snapshot never sees a pending group.
- **A6 Independence.** Decision, selection, drop, copy and remap act key by key. Only the byte
  accounting of §6 couples keys, and it changes no decision.

The model says nothing about copy rate, latency or tail behavior, about power loss, or about any
platform's durability; it is no E3 or E4 evidence.

## 9. How the model is checked

Because decisions are per key (A6), a store whose keys are all the histories in a scope exercises
every shape in the scope at once. The model's functions are executable in
[`compaction_model.hpp`](../../tests/support/compaction_model.hpp).

| Theorem | Exhaustive scope | Check |
|---|---|---|
| 1, composition | all 442,276 histories of at most five Records over four sealed Segments and the active one; every prefix; every Index J allows (2,122,920 cases); 879,562 pairs of transactions | [`compaction_model_tests.cpp`](../../tests/unit/compaction_model_tests.cpp) |
| 1b, tightness | every range of four sealed Segments; the time, Index and order hypotheses | same |
| 2 | at most four Records over three sealed Segments and the active one, every foreground behavior (354,888 cases) | same |
| 3 | one to seven sealed Segments; every Manifest within four edits per entry of a three-Segment Worker, both profiles | same |
| 4, 5 | every live-byte vector over five levels (the last more than a Segment holds) for one to five Segments with every budget around each cumulative sum, both profiles (115,240 selections) against the runtime; 3,000 random layouts against the runtime's | same |
| 6 (a)–(d) | every state of up to four sources and four outputs; the protocol of every shape | same |
| refinement of 1 to 5 | the runtime on all 2,770 histories of at most three Records at once, for a strict prefix, the complete set and a zero-output prefix; and on 1,080 keys under every foreground behavior: decision, deciding Record and every counter equal the model's | [`persistence_compaction_model_tests.cpp`](../../tests/integration/persistence_compaction_model_tests.cpp) |
| refinement of 6 | 58 namespace states of three shapes: recovery equals R, refuses without removing, and a recovered directory is a fixpoint | [`compaction_recovery_tests.cpp`](../../tests/integration/compaction_recovery_tests.cpp) |
| 6 (d), process kills | 25 kills of one transaction at every persistent boundary, 114 checkpoints in all | [`crash_persistence_prefix.cpp`](../../tests/crash/crash_persistence_prefix.cpp) |

The counts of the unit and runtime checks are asserted exactly, so a scope that shrinks fails its
test. Histories longer than the scope are covered by the proofs above, not by enumeration. A failure
of any check names the key, its history, the time and the expected and actual decisions.
