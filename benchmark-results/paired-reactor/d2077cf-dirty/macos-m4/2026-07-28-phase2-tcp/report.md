# Paired Reactor Phase 2 — TCP A/B

Data: 2026-07-28

Commit base: `d2077cf` con snapshot metriche sperimentale nel working tree

Piattaforma: Apple M4, macOS, build `macos-native-release`

Confronto: server corrente, 1 Worker, contro paired Reactor volatile, 1 Reader + 1 Writer

## Metodo

Il benchmark usa il client C++ pubblico e wire protocol v2 su loopback. Ogni cella avvia entrambe
le implementazioni, carica lo stesso dataset, alterna l'ordine current/paired, esegue una warmup e
tre o cinque ripetizioni. Il workload è deterministico; le mutazioni sono distribuite con una
permutazione modulo 100 invece di essere raggruppate all'inizio di ogni blocco.

Le latenze sono del completamento del batch client, non del singolo GET. Per questo le celle mixed
non dimostrano ancora il requisito p99 GET isolato sotto scritture: misurano correttamente il costo
end-to-end osservato dal client, inclusa ogni barriera di ordinamento PUT→GET.

## Risultati principali

### Scaling GET, 64 B, pipeline 32

| Client | Current ops/s | Paired ops/s | Delta | Current p99 us | Paired p99 us |
|---:|---:|---:|---:|---:|---:|
| 1 | 686.285 | 699.780 | +2,0% | 53,1 | 52,4 |
| 2 | 1.369.340 | 1.434.630 | +4,8% | 63,1 | 63,7 |
| 4 | 1.783.200 | 1.794.370 | +0,6% | 120,5 | 118,8 |
| 8 | 1.959.270 | 1.989.700 | +1,6% | 172,3 | 191,1 |

Il GET owner-bound è competitivo e conserva un piccolo vantaggio throughput fino alla saturazione
del singolo Reader. Il p99 non migliora uniformemente e a otto client regredisce. Non è scaling
multi-pair: tutte le connessioni condividono la stessa coppia.

### Sensibilità pipeline, GET 64 B, 4 client

| Pipeline | Current ops/s | Paired ops/s | Delta |
|---:|---:|---:|---:|
| 1 | 102.957 | 101.139 | -1,8% |
| 8 | 731.359 | 712.921 | -2,5% |
| 32 | 2.050.680 | 2.068.920 | +0,9% |
| 128 | 3.280.530 | 3.655.200 | +11,4% |

Il paired Reactor ammortizza bene parser e scatter/gather soprattutto a pipeline 128. Pipeline 1 e
8 sono lievemente negative e vanno considerate inconclusive finché il run non è ripetuto su host
isolato; non emerge più la forte regressione osservata nel run preliminare con hash stale.

### Mix, 64 B, pipeline 32, 4 client

| Mix GET/PUT | Current ops/s | Paired ops/s | Delta | p99 current us | p99 paired us | Batch publication medio |
|---:|---:|---:|---:|---:|---:|---:|
| 100/0 | 1.795.570 | 1.811.090 | +0,9% | 113,8 | 114,1 | — |
| 99/1 | 1.765.720 | 1.453.400 | -17,7% | 116,8 | 160,7 | 1,07 |
| 95/5 | 1.945.690 | 900.902 | -53,7% | 109,2 | 252,4 | 1,25 |
| 90/10 | 2.035.390 | 559.837 | -72,5% | 94,2 | 340,8 | 1,43 |

Questo è il fallimento principale. Una connessione non può oltrepassare una mutazione prima della
publication/completion se la richiesta seguente può essere un GET: è la barriera che garantisce
read-after-write. Con quattro connessioni il Writer vede quindi al massimo quattro mutazioni
coalescibili e, nella pratica, quasi sempre una sola. COW delta, publication e due passaggi di
scheduling dominano il lavoro utile sui valori piccoli.

Portare la deadline Writer a 32 us aumenta il batch medio fino a 2,52, ma peggiora il throughput
paired a 819 mila ops/s contro 964 mila a 2 us. Il problema non si risolve aspettando più a lungo.

### Dimensione valore, 4 client

| Valore / mix / pipeline | Current ops/s | Paired ops/s | Delta | p99 current us | p99 paired us |
|---|---:|---:|---:|---:|---:|
| 1 KiB, GET, p32 | 1.196.140 | 1.441.510 | +20,5% | 162,9 | 146,5 |
| 64 KiB, GET, p32 | 43.117 | 143.368 | +232,5% | 3.087,9 | 1.121,6 |
| 256 KiB, GET, p8 | 10.412 | 33.442 | +221,2% | 7.099,4 | 1.421,8 |
| 1 KiB, 95/5, p32 | 1.193.020 | 869.132 | -27,1% | 160,8 | 252,3 |
| 64 KiB, 95/5, p32 | 41.213 | 108.388 | +163,0% | 3.676,4 | 2.003,4 |
| 256 KiB, 95/5, p8 | 10.617 | 29.860 | +181,2% | 5.624,9 | 2.185,4 |

Scatter/gather è un successo netto. Dai 64 KiB in su il risparmio della concatenazione/copia output
supera ampiamente il costo delle mutazioni. Le write parziali attivano il pin di generazione; il
massimo osservato è tre pin simultanei e il backlog finale è zero.

## Correttezza e safety

- Debug: 446/446 test;
- ASan+UBSan: 446/446 test;
- TSan: il primo run ha trovato una race nella lettura cross-thread delle metriche Reader-locali;
- correzione: snapshot delle metriche a fine event-loop turn, protetta fuori dal data path;
- TSan dopo la correzione: 446/446 test, nessuna race;
- fault/crash suite CTest: 37/37 target, inclusa la matrice crash sync;
- test deterministico socket lento: write parziale, pin osservato, pin rilasciato a zero;
- nessuna mutation queue full o publication backpressure nei run registrati.

## Giudizio

Il prototipo dimostra che Reader=Reactor, GET diretto e risposta scatter/gather sono una direzione
valida. Non dimostra ancora che la coppia Reader–Writer sia pronta a sostituire il runtime corrente.
Il gate è **pass parziale per GET e valori grandi**, **fail per mutazioni piccole**; pipeline 1/8 è
inconclusive e non è il blocker principale.

Non va aggiunto multi-pair per mascherare il costo single-pair: moltiplicherebbe un percorso di
mutazione ancora inefficiente. Il prossimo P0 deve misurare e ridurre separatamente:

1. Reader submit → Writer dequeue;
2. costruzione frozen delta / allocazioni per publication;
3. release publication → completion wakeup;
4. completion → adozione → ACK;
5. barriera per connessione e opportunità di coalescing di PUT consecutivi prima del primo GET.

L'integrazione in `glifistored`, la durabilità e il multi-pair restano non autorizzati finché 99/1
a 64 B non rientra almeno nel rumore della baseline e il p99 del solo GET viene misurato sotto un
Writer concorrente separato.

## Artifact raw

- `environment.txt`
- `get-scaling.txt`
- `mix-ratios.txt`
- `pipeline-sensitivity.txt`
- `value-sizes.txt`
- `batch-wait-sensitivity.txt`
