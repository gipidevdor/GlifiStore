# Paired Reader–Writer volatile A/B — 2026-07-28

## Giudizio

Il modello Reader è promettente e supera il gate di isolamento; l'implementazione Writer del
prototipo non supera il gate di throughput mixed sui dataset principali. La decisione corretta è
proseguire con l'architettura a coppie, ma non integrare ancora il prototipo nel daemon.

Il GET paired owner-bound non usa mutex, code, refcount o allocazioni e usa al massimo due lookup.
Con valori 64 B raggiunge 14,97 Mops/s contro 5,45 Mops/s (2,75×); con 1 KiB 14,94 Mops/s contro
2,45 Mops/s (6,11×). Il p99 scende rispettivamente da 292 a 166 ns e da 500 a 167 ns.

Nel mix GET/PUT 95/5 il quadro si ribalta: 64 B raggiunge soltanto 1,63 Mops/s contro 5,34 Mops/s
(0,30×), mentre 1 KiB raggiunge 0,59 Mops/s contro 2,41 Mops/s (0,25×). Il p99 GET resta migliore
(125/166 ns contro 292/500 ns), quindi il Reader è isolato, ma il Writer spende troppo nella costruzione
delle publication e limita il throughput complessivo.

## Risultati mediani

| Valore | Workload | Current ops/s | Paired ops/s | Rapporto | Current p99 | Paired p99 |
|---:|---|---:|---:|---:|---:|---:|
| 64 B | GET 100% | 5.453 M | 14.970 M | 2,75× | 292 ns | 166 ns |
| 64 B | GET/PUT 95/5 | 5.339 M | 1.628 M | 0,30× | 292 ns | 125 ns |
| 1 KiB | GET 100% | 2.446 M | 14.940 M | 6,11× | 500 ns | 167 ns |
| 1 KiB | GET/PUT 95/5 | 2.408 M | 0.594 M | 0,25× | 500 ns | 166 ns |

Le sensitivity 64/256 KiB usano dataset e conteggi ridotti e non sono confrontabili con la matrice
principale. Mostrano soprattutto il diverso contratto di ritorno: la baseline copia ogni valore,
il paired restituisce uno span destinato allo scatter/gather. I rapporti di 252×/974× sul GET non
sono un claim daemon o TCP.

## Causa tecnica

Ogni batch copia il `MutableDeltaIndex`, costruisce un nuovo frozen index e, alla soglia, ricostruisce
la base. Il costo medio di publication osservato è circa 385 us a 64 B e 1,09 ms a 1 KiB, con batch
medio vicino a 32 record. Il costo cresce con i byte del delta, non con il solo record corrente.
La telemetria `ingress_value_bytes_copied` misura soltanto la copia slot→record; non pretende di
misurare le copie interne implicite di `std::vector`/`ReadRecord`, che sono il debito P0 emerso.

QSBR non è il collo di bottiglia: generation high-watermark 2–5, tutte le generation ritirate,
zero publication backpressure in ogni run. ASan/UBSan e TSAN passano sul prototipo corrente.

## Validità e limiti

- Run interleaved, una warmup e sette sample per cella principale, build Release nativa.
- Tutti i lookup hanno restituito un valore; la verifica finale byte-per-byte fra current e paired è
  passata dopo i workload.
- È un microbenchmark in-process del data path, non TCP, non Reactor reale, non durable e non
  multi-pair.
- Current usa `Store::get_copy`; paired usa uno span. Questo rappresenta la direzione zero-copy ma
  rende i valori grandi una sensitivity, non una comparazione equivalente.
- macOS non offre hard affinity nel run; client Reader e Writer condividono la macchina senza core
  isolation. Le latenze sono campionate 1/64 e quantizzate dal clock a circa 41–42 ns.
- Non sono ancora disponibili IPC, cache/LLC/dTLB miss o branch miss: su macOS richiedono un run
  Instruments separato. Non sono stati stimati.

## Decisione P0 successiva

1. Eliminare la copia cumulativa del delta per publication: cut/buffer doppio o immutable delta
   builder con ownership trasferita, senza copiare valori.
2. Separare publication frequente della visibilità da merge base incrementale; massimo due livelli
   deve restare un'invariante.
3. Memorizzare payload una sola volta in storage stabile Writer-owned e pubblicare riferimenti
   generation-pinned, non `vector<byte>` duplicati nell'Index.
4. Aggiungere contatori reali per allocation e byte copiati nel builder.
5. Ripetere gli stessi due gate; accettare il blocco solo se il 95/5 non regredisce oltre il rumore e
   conserva il vantaggio p99 GET.

File: `samples-final.csv` contiene tutti i campioni finali; `samples.csv` conserva la sensitivity
preliminare; `telemetry.csv` contiene i contatori finali; `environment.txt` descrive l'ambiente.
