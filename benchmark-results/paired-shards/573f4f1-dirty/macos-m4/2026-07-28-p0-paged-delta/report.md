# P0 paged delta + StableRecord — A/B 2026-07-28

## Esito

Il P0 supera il gate mixed che aveva bloccato il primo prototipo. La copia cumulativa di record e
payload è stata sostituita da `StableRecord` allocati una volta e da pagine delta immutable
copy-on-write da 16 slot. Il GET continua a usare al massimo un lookup delta e uno base.

| Valore | Workload | Current ops/s | Paired ops/s | Rapporto | Current p99 | Paired p99 |
|---:|---|---:|---:|---:|---:|---:|
| 64 B | GET 100% | 5,408 M | 13,534 M | 2,50× | 292 ns | 167 ns |
| 64 B | GET/PUT 95/5 | 5,268 M | 11,301 M | 2,15× | 292 ns | 208 ns |
| 1 KiB | GET 100% | 2,401 M | 13,364 M | 5,57× | 500 ns | 167 ns |
| 1 KiB | GET/PUT 95/5 | 2,383 M | 10,898 M | 4,57× | 500 ns | 208 ns |

Rispetto al primo gate paired, il 95/5 passa da 1,63 a 11,30 Mops/s a 64 B e da 0,59 a
10,90 Mops/s a 1 KiB. La publication latency cumulativa scende da 2,46/6,97 secondi a
0,34/0,35 secondi, pur con circa 24.700 publication invece di circa 6.400: il Writer più veloce
svuota la SPSC in batch medi più piccoli.

## Correttezza e boundedness

- verifica finale A/B byte-per-byte: pass;
- publication backpressure: zero;
- generation high-watermark: 4–5 su 258 slot;
- tutte le generation ritirate;
- un test dedicato verifica l'overshoot di un batch completo oltre una soglia merge pari a uno;
- nessun payload viene copiato fra generation: base/delta/pagine copiano handle, non byte valore.

## Debito residuo

La directory piatta copia 512 handle di pagina per publication nel profilo 4.096 chiavi: circa
12,6 milioni di handle nei run. È lavoro Writer-only e il gate è già positivo, ma resta un target
per workload con delta molto più grandi. `make_shared`, stringa e vector producono inoltre più
allocazioni fisiche della metrica logica `payload_allocations`; servono slab/arena e hook allocator
prima di un claim production.

Il confronto resta in-process e non include TCP, output lease, Segment o durability. Autorizza il
prototipo Reactor non-default, non l'attivazione del runtime paired in produzione.
