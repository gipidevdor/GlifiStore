<!-- GENERATED FILE. Authority: engineering/hazards/*.yaml -->

# Hazard register

Generated from `engineering/hazards/`. GlifiStore is an architectural prototype;
accepted residual risks do not imply production readiness.

| ID | Event | Severity | Probability | Detectability | State | Requirements |
| --- | --- | --- | --- | --- | --- | --- |
| `HAZ-001` | Conferma falsa di durabilità | catastrofica | media | media | aperto | `GS-PERSIST-ACK-001` |
| `HAZ-002` | Perdita silenziosa | catastrofica | bassa | bassa | mitigato | `GS-RECOVERY-FAILCLOSED-001` |
| `HAZ-003` | Lettura obsoleta come corrente | alta | media | media | mitigato | `GS-CONCUR-PAIR-001`, `GS-CONCUR-LIN-001` |
| `HAZ-004` | Resurrezione di una cancellazione | alta | bassa | media | mitigato | `GS-RECOVERY-DET-001`, `GS-PERSIST-ORDER-001` |
| `HAZ-005` | Doppia applicazione | alta | bassa | media | mitigato | `GS-PROTO-WIRE-001`, `GS-PERSIST-ACK-001` |
| `HAZ-006` | Recupero non deterministico | catastrofica | bassa | alta | mitigato | `GS-RECOVERY-DET-001` |
| `HAZ-007` | Perdita di una mutazione confermata | catastrofica | media | media | aperto | `GS-PERSIST-ACK-001` |
| `HAZ-008` | Uso dopo liberazione | catastrofica | bassa | alta | mitigato | `GS-CONCUR-PAIR-001`, `GS-CORE-CLOSE-001` |
| `HAZ-009` | Recupero prematuro di segmenti | alta | bassa | media | mitigato | `GS-CONCUR-PAIR-001` |
| `HAZ-010` | Scrittura nella partizione errata | catastrofica | bassa | alta | mitigato | `GS-PROTO-WIRE-001`, `GS-CONCUR-PAIR-001` |
| `HAZ-011` | Divergenza fra instradamento e manifesto | alta | bassa | media | mitigato | `GS-COMPAT-FIXTURE-001`, `GS-PROTO-WIRE-001` |
| `HAZ-012` | Compattazione che elimina dati visibili | catastrofica | bassa | media | mitigato | `GS-RECOVERY-DET-001`, `GS-PERSIST-ORDER-001` |
| `HAZ-013` | Rotazione incompleta | alta | media | alta | mitigato | `GS-RECOVERY-DET-001` |
| `HAZ-014` | Corruzione del manifesto | catastrofica | media | alta | accettato | `GS-PERSIST-ORDER-001`, `GS-RECOVERY-FAILCLOSED-001` |
| `HAZ-015` | Corruzione delle caselle di conferma | catastrofica | media | alta | aperto | `GS-PERSIST-ORDER-001` |
| `HAZ-016` | Esaurimento dello spazio | alta | media | alta | mitigato | `GS-RECOVERY-FAILCLOSED-001`, `GS-OPS-CONFIG-001`, `GS-PERSIST-FAULT-001` |
| `HAZ-017` | Quota esaurita | alta | bassa | alta | mitigato | `GS-RECOVERY-FAILCLOSED-001` |
| `HAZ-018` | Errori di scrittura differita | alta | media | bassa | accettato | `GS-PERSIST-ACK-001` |
| `HAZ-019` | Errori di sincronizzazione | alta | media | media | mitigato | `GS-PERSIST-ORDER-001` |
| `HAZ-020` | Fallimento durante la chiusura | alta | media | media | mitigato | `GS-CORE-CLOSE-001` |
| `HAZ-021` | Arresto durante il backup | media | media | media | mitigato | `GS-OPS-BACKUP-001` |
| `HAZ-022` | Ripristino incompatibile | alta | media | alta | mitigato | `GS-OPS-BACKUP-001`, `GS-COMPAT-FIXTURE-001`, `GS-COMPAT-NN1-001` |
| `HAZ-023` | Errore di autorizzazione | alta | media | alta | mitigato | `GS-SEC-PROFILE-001` |
| `HAZ-024` | Aggiramento dei limiti | alta | media | media | mitigato | `GS-SEC-PROFILE-001`, `GS-SEC-BOUNDS-001`, `GS-OPS-CONFIG-001` |
| `HAZ-025` | Saturazione delle code | media | alta | alta | mitigato | `GS-OPS-CONFIG-001`, `GS-OPS-SOAK-001`, `GS-CONCUR-PAIR-001` |
| `HAZ-026` | Starvation | media | media | bassa | mitigato | `GS-OPS-CONFIG-001` |
| `HAZ-027` | Blocco permanente | alta | bassa | bassa | mitigato | `GS-CONCUR-PAIR-001`, `GS-CONCUR-LIVE-001`, `GS-CORE-CLOSE-001` |
| `HAZ-028` | Errore nella pubblicazione delle generazioni | alta | media | media | mitigato | `GS-CONCUR-PAIR-001`, `GS-CONCUR-MEM-001`, `GS-CONCUR-TLA-001` |
| `HAZ-029` | Deriva o contaminazione della C ABI pubblica | alta | media | alta | mitigato | `GS-COMPAT-CABI-001` |
| `HAZ-030` | Pubblicazione di byte diversi da quelli verificati o con identita non provata | catastrofica | media | alta | mitigato | `GS-RELEASE-ARTIFACT-001` |
| `HAZ-031` | Amplificazione memoria o alta marea RSS non attribuita nel runtime paired | alta | alta | media | aperto | `GS-CONCUR-PAIR-001` |
| `HAZ-032` | Debito di merge paired concentrato in una pausa terminale o overload spurio | alta | media | alta | mitigato | `GS-CONCUR-PAIR-001`, `GS-CONCUR-COMBINE-001` |
| `HAZ-033` | Compaction I/O causa jitter foreground o il rate gate affama candidati grandi | alta | alta | alta | mitigato | `GS-OPS-CONFIG-001`, `GS-OPS-DEBT-001`, `GS-PERSIST-AMP-001` |
| `HAZ-034` | Pacchetto dichiarato provato oltre le prove effettivamente eseguite | alta | media | alta | mitigato | `GS-RELEASE-PACKAGE-001`, `GS-RELEASE-UPGRADE-001`, `GS-RELEASE-ARTIFACT-001` |
