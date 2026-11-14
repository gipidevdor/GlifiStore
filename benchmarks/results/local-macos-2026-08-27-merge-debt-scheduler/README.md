# Paired merge debt scheduler — macOS locale 2026-08-27

## Scopo

Questa campagna verifica la correzione del merge incrementale della `ReadGeneration`. Il candidato:

- calcola il lavoro residuo esatto fra initialize, base e delta;
- rapporta il lavoro alla minore capacita residua fra entry e record-version del post-delta;
- esegue un quantum minimo per PUT singolo e due quantum per publication coalesciata;
- puo superare quel minimo solo per evitare l'esaurimento bounded prima della publication;
- elimina due chiamate duplicate di housekeeping dal combiner embedded;
- esegue il merge idle del Writer dedicato dopo la consegna dei completamenti, rilasciando il token
  fra quantum;
- tratta le transizioni di fase come lavoro senza slot, evitando lo stato `debt=0 && !ready`.

Wire v2, persistence v1, routing, linearizzazione, punto di ACK, visibilita e durabilita non cambiano.

## Host e metodo

- Apple M4 / arm64, macOS, Apple LLVM 21, Release `-O3 -mcpu=native`.
- Repository dirty per il programma di correzione in corso; SHA dichiarato dal benchmark:
  `5feccd3-dirty`.
- Alimentazione e isolamento termico non certificati. Numeri locali e advisory.
- Dataset insert-only, un Worker, chiave 16 B, valore 64 B, 200.000 operazioni salvo indicazione.
- Il controllo tail e un binario temporaneo fuori repository: stesso candidato finale con le due
  chiamate duplicate pre-publication ripristinate, quindi tre quantum singoli complessivi. Non e un
  confronto fra commit e non rappresenta tutta la storia precedente.

## Risultati

| Cella | Controllo | Candidato | Differenza |
| --- | ---: | ---: | ---: |
| PUT singolo, throughput | 506.244 ops/s (5 campioni) | 519.513 ops/s (9) | +2,62% |
| PUT batch 32, throughput | 670.405 ops/s (5) | 676.662 ops/s (9) | +0,93% |
| Insert growth 1,5 M | 182.632 ops/s | 186.112 ops/s | +1,91% |
| PUT timed, controllo tre-quantum | 482.574 ops/s | 522.026 ops/s | +8,18% |
| PUT timed p50 | 1.334 ns | 1.209 ns | -9,37% |
| PUT timed p95 | 2.375 ns | 1.792 ns | -24,55% |
| PUT timed p99 | 3.042 ns | 2.625 ns | -13,71% |
| PUT timed p99.9 | 236.709 ns | 92.750 ns | **-60,82%** |

Il profilo timed contiene un milione di osservazioni per variante (200.000 × 5). La lettura corretta
e: throughput ordinario e batch restano neutrali, mentre rimuovere i quantum duplicati riduce
materialmente la coda estrema attribuita al merge. Il candidato conserva comunque un p99.9 di circa
92,8 us: e lavoro reale, non una chiusura del rischio tail.

## Prove di bound e correttezza

Le nuove prove configurano volutamente `merge_delta_entries=2`, `maximum_post_entries=2` e
`quantum_slots=1`. Il vecchio schema poteva arrivare al retry terminale senza completare il merge.
Embedded combiner e Writer dedicato ora completano senza `read_merge_backpressure` e senza
`generation_admission_backpressure_total`; RAW e tutte le chiavi restano visibili.

La prova unitaria verifica inoltre budget proporzionale, capacita record-version/entry e transizione
finale esatta. La telemetria aggiunta espone:

- `read_merge_remaining_slots`;
- `read_merge_post_capacity_remaining`;
- `maximum_read_merge_quantum_slots`.

La validazione locale finale comprende 632/632 test funzionali Release, fault injection allocativa,
quattro casi mirati sia ASan/UBSan sia TSan, tre crash smoke (`sync`, `group`, `daemon_group`), il
nuovo benchmark smoke con 256 campioni di latenza e l'assurance validator senza warning. Queste
prove sono evidenza locale, non sostituiscono le matrici CI multipiattaforma.

## Limiti e rischio residuo

- Nessuna misura Linux/FreeBSD/OpenBSD, NUMA, oltre LLC o multi-pair in questa cartella.
- Nessun overlap TCP GET, compaction persistente o durable group in questo confronto mirato.
- Su basi molto grandi il termine proporzionale puo legittimamente superare il minimo per mantenere
  la memoria bounded; la metrica di massimo quantum rende visibile questo caso.
- Il progetto resta un architectural prototype; HAZ-032 e mitigato, non chiuso.

## Comandi principali

```text
glifistore_benchmarks --filter store-put --ops 200000 --key-size 16 --value-size 64 --workers 1
glifistore_benchmarks --filter store-put-batch --ops 200000 --key-size 16 --value-size 64 --workers 1
glifistore_benchmarks --filter store-parallel-put --ops 200000 --key-size 16 --value-size 64 \
  --workers 1 --threads 1 --distribution single-worker --warmup 1 --repeats 5 --latency
```
