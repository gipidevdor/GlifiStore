# PUT volatile: censimento ReadGeneration O(1) — macOS locale, 2026-08-27

Stato: evidenza ingegneristica locale su sorgente dirty, **non** gate di rilascio e non claim
multi-piattaforma.

## Ambiente

- Sorgente: `5feccd3-dirty`, branch `codex/paired-write-pressure`
- Host: Apple M4, arm64, 16 GiB
- OS: macOS 26.6.2 (25G83)
- Compiler: Apple clang 21.0.0 (`clang-2100.1.1.101`)
- Build: `macos-native-release`, CPU pinning non disponibile/non richiesto

Il worktree comprende il programma di correzione ancora non consolidato. Le misure devono essere
ripetute da un commit immutabile e su tutte le righe piattaforma prima di sostenere un claim di
rilascio.

## Causa

Il Writer pubblicava correttamente una nuova generazione immutabile, poi aggiornava il censimento
operativo chiamando `PairReadGeneration::memory_stats()`. Il censimento del delta percorreva ogni
chunk, block e page raggiungibile a ogni singola pubblicazione COW; il censimento della base
percorreva tutti i blocchi delle chiavi esterne. Il costo era quindi dipendente dalla topologia
dell'indice e veniva pagato anche se nessuno leggeva le metriche.

Un campionamento CPU su 2.000.000 PUT ha attribuito 2.445 campioni su 3.744 (65,3%) a
`DeltaState::append_memory_stats`. Nello stesso workload il throughput medio era sceso a 74.603
PUT/s per l'effetto combinato di censimento crescente e reale lavoro di merge.

## Correzione

- `DeltaState` conserva i conteggi esatti di page, block e chunk raggiungibili.
- Il solo Writer incrementa i conteggi quando una pubblicazione materializza un nodo prima vuoto;
  clonare o sostituire un nodo COW già raggiungibile non cambia il censimento.
- La base conserva il totale dei byte dei key block mentre li alloca.
- `memory_stats()` usa soltanto contatori, capacità e aritmetica saturante: costo O(1), nessuna
  scansione e nessuna allocazione.
- Il fan-out usato esclusivamente per armare il fail-closed è costruito una volta per runtime,
  anziché allocato da ogni PUT riuscito. La sua vista resta immutable dopo la costruzione.

Non cambiano routing, FIFO, punto di linearizzazione, publication release/acquire, read-after-write,
durabilità, punto di ACK, formato persistente v1 o wire v2. Il GET non acquisisce nuovi lock e non
legge questi contatori.

## A/B isolata

Caso identico: 200.000 operazioni, key 16 B, value 64 B, un Worker, due warmup, sette ripetizioni.
La baseline include già il fan-out fail-closed preallocato: la differenza isola il censimento O(1).

| Variante | Mediana PUT/s | ns/PUT | RSS mediano |
| --- | ---: | ---: | ---: |
| Censimento topologico per scansione | 122.694 | 8.150,38 | 114.180.000 B |
| Censimento incrementale O(1) | 522.176 | 1.915,06 | 114.082.000 B |
| Differenza | **+325,6%** | **−76,5%** | −0,09% |

Il fan-out fail-closed preallocato, misurato separatamente prima della correzione del censimento,
era entro 0,1% (122.792 contro 122.694 PUT/s): viene mantenuto perché elimina un'allocazione
inutile per richiesta, non perché costituisca un guadagno misurabile isolato.

## Sweep e batch dopo la correzione

Undici ripetizioni, tre warmup, 200.000 operazioni, un Worker:

| Workload | Mediana ops/s | ns/op | Min–max ops/s |
| --- | ---: | ---: | ---: |
| PUT, value 16 B | 500.007 | 1.999,97 | 481.312–529.752 |
| PUT, value 64 B | 492.524 | 2.030,36 | 478.705–521.000 |
| PUT, value 256 B | 496.329 | 2.014,79 | 475.283–521.659 |
| PUT, value 1 KiB | 436.644 | 2.290,20 | 420.855–457.513 |
| PUT batch 32, value 64 B | 672.038 | 1.488,01 | 663.918–703.428 |

Il batch resta più efficiente perché ammortizza il reale lavoro di publication/merge. Non è stato
cambiato alcun contratto del PUT singolo per inseguire il throughput batch.

Nel campionamento successivo, `memory_stats()` rappresentava 3 campioni su 2.256 (0,13%). Il costo
dominante residuo era lavoro reale: publication COW, merge incrementale proporzionale e retirement
delle generazioni. Il test lungo da 1.500.000 insert non è uno steady-state controllato: cresce la
base per tutto il run e ha misurato 182.632 PUT/s medi; non va confuso con la cella da 200.000.

## Verifica

- `glifistore_tests`: 629/629.
- CTest `macos-native-release`: 53/53, inclusi crash-sync e allocation fault campaign.
- ASan+UBSan: censimento arena/COW e quattro test paired concorrenti verdi.
- TSan: FIFO combiner, GET/PUT linearizzato e delta directory COW verdi.
- Assurance: 30 requirements, 31 hazards, 26 gates, 2 waivers, 0 warning.

Il test del delta verifica in particolare che 64 overwrite della stessa chiave clonino il nodo COW
senza aumentare il conteggio della topologia raggiungibile, mentre record arena e lower bound
restano coerenti.

## Rischio residuo

1. Ripetere l'A/B da commit pulito su macOS, Linux, FreeBSD e OpenBSD.
2. Separare insert growth, overwrite steady-state e merge overlap in campagne di durata lunga.
3. Attribuire il costo residuo di publication, merge e retirement con contatori lab aggiornati.
4. Non rimuovere publication-per-ACK o bounded generation admission per ridurre il costo residuo.

