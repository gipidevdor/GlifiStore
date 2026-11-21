# Proprietà matematiche del governo delle letture

**Analisi della proposta, 2026-11-21. Nessuna prova del runtime multilettore.**
Questo allegato distingue le proprietà del [piano di governo](adaptive-read-management.md)
da quelle ancora da dimostrare. Il controllo a soglie è un'euristica progettata con vincoli,
non un algoritmo già dimostrato stabile, ottimale o privo di starvation in tutte le condizioni.

## 1. Capacità e latenza

Usare il budget utile quando c'è domanda è un obiettivo di impiego delle risorse. Non implica
che sia possibile servire qualsiasi offerta, tenere ogni core al 100% e conservare una latenza
limitata senza backpressure o perdite. Con servizio finito, un'offerta media superiore al
servizio produce lavoro arretrato, rifiuti o attesa a monte.

Un controesempio alla garanzia universale di saturazione senza costo sulla latenza è la coda
M/M/1: arrivi Poisson di intensità `lambda`, servizi esponenziali indipendenti di intensità `mu`,
un servente e buffer illimitato. Per `lambda < mu`, il tempo medio totale nel sistema è

```text
rho = lambda / mu
E[T] = 1 / (mu - lambda) = (1 / mu) / (1 - rho)
```

Avvicinandosi a `rho = 1`, quel tempo diverge. Al 70% il fattore sul servizio medio è circa
3,33, al 90% è 10 e al 99% è 100. Questi numeri riguardano soltanto quel modello: non sono
una previsione della latenza GlifiStore né una dimostrazione che il 70% sia la soglia giusta.
Un carico deterministico speciale può comportarsi diversamente; il controesempio basta a
escludere una promessa generale. [Modello e formula nelle dispense MIT](https://web.mit.edu/dimitrib/www/Queueing_Data_Nets.pdf).

Con code limitate la memoria resta limitata, ma l'eccesso può accumularsi nei client o nella
rete, oppure essere rifiutato. Questo non dimostra stabilità del servizio dell'intera offerta.
Lo SLO richiede margine rispetto alla capacità effettiva e ipotesi sui burst.

## 2. Stato e conservazione del lavoro

Per ogni shard, distinguere arrivi offerti dal generatore, arrivi osservabili al server,
richieste ammesse, lavoro pronto, attese di visibilità, output in attesa e cancellazioni.
La contabilità del totale `N_s` di letture ammesse ma non terminate rispetta

```text
N_s[t+1] = N_s[t] + A_s[t] - D_s[t] - K_s[t]
```

`A` conta le ammissioni, `D` le risposte terminate lato server e `K` le cancellazioni. I
completamenti del solo helper non sono `D`: l'output potrebbe essere ancora bloccato. La
campagna misura separatamente ricezione/validazione lato client e offerta non inviata o rifiutata.
Nessuna richiesta può sparire dai conti per migliorare artificialmente goodput o latenza.
Il goodput conta esiti corretti del GET nel workload dichiarato, compresi miss attesi;
rifiuti di ammissione, cancellazioni e internal error non diventano lavoro utile completato.

La pressione che autorizza helper usa solo lavoro delegabile; la conservazione considera
anche gli stadi bloccati. Una coda pronta vuota con output pieno è un limite diverso da un Reader
che non riesce a eseguire i GET. I soli completamenti non osservano bene la saturazione e possono
ridurre erroneamente la domanda stimata.

## 3. Capacità accoppiate

La forma generale del servizio è `mu_s(x, z)`: `x` è il vettore degli helper di tutti gli
shard e `z` rappresenta carico Writer, rete, banda memoria, quote e collocamento sui core.
La notazione abbreviata `C_s(k, p)` del piano fissa questo contesto nel profilo `p`.
Non rende indipendenti gli shard e non consente di sommarne ciecamente le capacità isolate.

Un helper assegnato ad A può saturare memoria condivisa e rallentare B. L'allocazione cambia
quindi anche la capacità che l'osservatore sta stimando. Budget CPU globale e isteresi locale
non bastano da soli a provare la stabilità del sistema accoppiato e ritardato.
Le campagne devono qualificare più owner caldi insieme e il beneficio marginale.

Una dimostrazione robusta di stabilità delle code richiede un modello degli arrivi, un insieme
ammissibile di servizi, una domanda con margine interno a tale insieme e una politica che lo
sappia sfruttare. I risultati sulle regioni di stabilità valgono per modelli e politiche specifici;
non si trasferiscono automaticamente al nostro controllo a soglie.
[Tassiulas ed Ephremides, archivio degli autori](https://drum.lib.umd.edu/items/571fda52-aefb-4497-9a2d-69d8c7c907b9/full).

Qui non sono ancora identificati modello dell'impianto, ritardi massimi, regione di servizio
condivisa o errore delle stime. Non si affermano ottimalità del throughput o stabilità globale.

Un possibile obbligo formale, fissate le ipotesi sugli arrivi e i momenti dei servizi, è una
funzione di Lyapunov `V(Q) = sum(Q_s^2)/2`, dove `Q` comprende le code e le classi necessarie
del modello completo, con deriva condizionale

```text
E[V(Q[t+1]) - V(Q[t]) | stato[t]] <= K - epsilon * sum(Q_s[t]), epsilon > 0
```

Con aspettative finite e `V >= 0`, sommare questa disuguaglianza nel tempo darebbe un limite
alla media temporale del backlog. Qui la disuguaglianza non è stata dimostrata per la politica
proposta. Applicarla soltanto a code ammesse già troncate da backpressure, ignorando l'offerta
non servita, non dimostrerebbe la capacità di servizio richiesta dall'utente.

## 4. Sicurezza dei crediti nel modello astratto

Sia `O` il numero di crediti helper impegnati, includendo arming, active e draining, e `B` il
totale di byte prenotati. Gli invarianti desiderati sono

```text
0 <= O <= H_cap
0 <= B <= M_cap
ogni credito appartiene a una sola assegnazione identificata
```

Stato iniziale: `O = 0`, `B = 0`. Una concessione incrementa `O` solo se `O < H_cap` e riserva
`b` byte non negativi solo se `b <= M_cap - B`. Una concessione applica entrambe le guardie;
un fallimento resta senza effetti oppure conserva un debito identificato fino al rollback.
Arming, attivazione e drain non restituiscono il credito.
Il rilascio decrementa i contatori soltanto presentando l'identificativo ancora valido di una
concessione, una volta sola, dopo la conclusione del protocollo di proprietà. Tutte le altre
transizioni conservano i contatori. Per induzione, queste guardie preservano gli invarianti.

La dimostrazione è condizionata a quelle transizioni: il codice futuro deve renderle atomiche,
validare i contatori senza overflow, impedire doppio rilascio/ABA e non autorizzare nuovi grant
da istantanee obsolete. Un contatore dichiarato non prova tale implementazione. La diminuzione
del target dinamico non modifica `H_cap`: avvia drain, non cancella debiti. Il limite degli
helper e dei byte non costituisce una prenotazione di core presso lo scheduler.
`B` contabilizza le cariche del sottosistema helper, non dimostra un limite sul RSS totale.
Stack, strutture del pool e generazioni trattenute richiedono la contabilità del rispettivo
dominio e la memory reserve autorevole; un limite ai soli task non copre tale memoria.

La sicurezza delle generazioni è un invariante diverso:

```text
reclaim(g) => g non pubblicata e nessun lettore, Writer/merge, trasferimento o risultato ne detiene un pin/protezione
```

Il registro multilettore e il suo protocollo di memoria non esistono ancora. Formulare questa
condizione non la dimostra; la prova sui crediti non implica assenza di use-after-free.

## 5. Proprietà locale dell'isteresi controfattuale

Considerare livelli adiacenti `k` e `k+1`, domanda costante `lambda`, profilo fisso con
`C_s(k,p) > 0`, misure esatte e nessun trasferimento per equità o cambio di budget.
L'espansione confronta `lambda / C_s(k,p)` con `theta_up`; il ritiro da `k+1` confronta
lo stesso rapporto controfattuale con `theta_down`.

Se `theta_down < theta_up`, non possono valere insieme

```text
lambda / C_s(k,p) >= theta_up
lambda / C_s(k,p) <= theta_down
```

La banda morta esclude il ciclo di andata e ritorno su quello stesso collegamento nel regime
stazionario ideale. Il carico corrente con `k+1` non dà tale proprietà: con capacità illustrative
`C(0)=1`, `C(1)=2` e domanda `0,75`, il carico passa dal 75% al 37,5%. Ritirare sotto il 40%
del carico corrente spegnerebbe l'helper e ricomincerebbe il ciclo; senza helper rimane invece 75%.

Se l'errore sul rapporto di riferimento è sempre limitato in valore assoluto da `epsilon`, una
condizione sufficiente per escludere i due attraversamenti sullo stesso rapporto costante è
`theta_up - theta_down > 2*epsilon`. Il limite deve coprire tutti gli errori pertinenti.
Intervalli di confidenza statistici non sono automaticamente limiti deterministici.
Domanda variabile, profili aggiornati, ritardi e allocazioni diverse agli altri owner escono
da questa dimostrazione. Non ne segue la stabilità globale o una latenza massima dei burst.
Anche la convergenza del numero di helper, quando dimostrabile nel caso ideale a livelli finiti,
non prova stabilità delle code: l'allocazione può fermarsi al massimo disponibile con domanda
ancora superiore al servizio. Senza tempi di attuazione e drain limitati non segue un tempo
massimo di assestamento.

## 6. Progresso, equità e tempi

Un arbitro che riparte sempre dal primo owner può affamare gli altri pur rispettando i limiti.
Prima di R1 va fissato l'algoritmo esatto: proposta di deficit round robin con pesi positivi,
costo in CPU, cursore persistente e rinnovi che non scavalchino indefinitamente chi attende.
I crediti maturati durante inattività non devono diventare una riserva illimitata al prossimo burst.

Una prova di progresso necessita almeno di owner in numero finito, risorse che tornino
disponibili, batch che terminino, destinatari dei completamenti che avanzino, rinnovi limitati
in presenza di contendenti e scheduler che dia esecuzione ai thread pronti. Se il budget resta
zero o un drain non termina, non si può promettere servizio a tutti. Il caso va rilevato,
non nascosto inventando un credito libero.

Una prova di limite temporale richiederebbe limiti su burst, servizio, registrazione, drain
e ritardo dello scheduler. I timeout e le finestre del controllo non forniscono tali limiti.
Il primo prodotto resta best effort con SLO qualificati; nessuna garanzia real-time deriva
da questa proposta.

Massimizzare GET/s, dare risorse eque e rispettare tutte le latenze può richiedere scelte diverse
con workload di costo diverso. L'ordine proposto è: sicurezza e contratti, progresso di Writer e
altri owner, SLO nel dominio qualificato, equità delle risorse, poi aumento del goodput utile.
Non è ancora un problema di ottimizzazione risolto o un teorema di ottimalità della politica.

## 7. Stato delle proprietà

| Proprietà | Stato della proposta | Obbligo prima dell'abilitazione |
| --- | --- | --- |
| Limiti helper e byte | Preservabili per induzione con le guardie sopra | Modello delle transizioni, contatori atomici e fault test del codice |
| Protezione generazioni | Invariante richiesto, protocollo non provato | Handoff/registrazione/reclaim, modello di memoria e prove concorrenti |
| Nessun ciclo locale ideale tra due livelli | Argomento condizionato sopra | Rumore, ritardi, profili e assestamento qualificati |
| Stabilità globale delle code | Non dimostrata | Modello di arrivo/servizio accoppiato e analisi della politica scelta |
| Assenza di starvation | Non dimostrata | Algoritmo esatto, ipotesi di progresso e allocazioni/rinnovi avversi |
| Assenza di deadlock e drain conclusivo | Non dimostrata | Dipendenze di attesa, completamenti garantiti e protocollo di arresto verificati |
| Latenza limitata | Nessuna garanzia incondizionata | Domanda/servizio/scheduler delimitati oppure SLO empirici con perimetro esplicito |
| Uso di tutta la capacità utile | Obiettivo condizionato, non efficienza misurata | Backlog, budget, beneficio marginale e tempi di attuazione osservati |
| Ottimalità del throughput | Non rivendicata | Richiederebbe un altro risultato formale con le sue ipotesi |

Simulatori e test con clock finto devono cercare controesempi di oscillazione, falsa liberazione
dei crediti, accoppiamento fra owner, stime errate, drain bloccati e starvation. Una campagna
senza errori non è una dimostrazione universale. Il gate resta di progetto finché le proprietà
richieste non sono provate nel perimetro dichiarato, con evidenza e rischi residui conservati.
