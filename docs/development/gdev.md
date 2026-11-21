# GlifiStore in GDev e Codex

Il progetto è dichiarato in `.gdev-assurance/project.eld`. Le operazioni
quotidiane di configurazione, build e test sono in `.gdev-project.eld` e
richiamano `scripts/dev.sh`, che prepara anche `.tools/venv/bin` nel PATH.
Apri la cartella del repository in GDev e scegli il profilo **Sviluppo**.
I profili ASan e TSan avviano le campagne esistenti, con i loro costi di disco.

`engineering/requirements`, `hazards`, `gates`, `waivers` e `claims` restano
autorevoli. `.gdev-assurance/catalog.json` è una vista derivata versionata,
generata con l'esportatore e verificata contro le fonti correnti. Conserva
gli identificativi GS, gli stati originali, collegamenti, prove e rischi.
Lo stato `CURRENT` indica la freschezza della vista. Non chiude alcun gate.
Il requisito `GS-GDEV-001` riguarda soltanto questa integrazione degli strumenti.

Prima di pianificare una correzione, consulta gli hazard pertinenti e i loro
requisiti, contratti e rischi residui. Per capacità e routing parti da
[HAZ-045 e HAZ-046](../../engineering/hazards/routing-capacity.yaml),
[ADR 0033](../adr/0033-online-rebalance-deferred.md) e
[semantica dei client](../spec/client-semantics-v1.md).
Il carico concentrato su uno shard richiede misure dedicate di capacità,
latenza e isolamento. Lo scenario con il 90% delle richieste sullo stesso
shard è illustrativo. La disponibilità di CPU sugli altri shard e il
progresso della compaction non dimostrano la sostenibilità di quel carico.
`WRONG_OWNER` mantiene il trattamento previsto dai client ufficiali;
nuove politiche di forwarding, rerouting e retry richiedono una decisione architetturale
esplicita. Per admission e compaction considera anche i percorsi residui
di [HAZ-044](../../engineering/hazards/durable-group-rejections.yaml).
Conserva separati gli esiti delle prove e l'accettazione dei rischi.

Dopo una modifica ai cataloghi esegui, dalla radice del repository

```sh
.tools/venv/bin/python3 engineering/tools/validate_assurance.py
.tools/venv/bin/python3 engineering/tools/export_gdev_catalog.py
.tools/venv/bin/python3 engineering/tools/gdev_verify.py metadata
```

Con `gdev` nel PATH, l'agente può usare le stesse letture dell'editor

```sh
gdev agente uso
gdev agente assicurazione "$PWD" progetto
gdev agente assicurazione "$PWD" catalogo GATE-COMPACTION-DELTA
gdev agente assicurazione "$PWD" piano-procedura GS-GDEV-METADATA
```

Il comando tecnico `catalog` accetta un prefisso di ID e un cursore,
per esempio `GATE-@100`. La risposta conserva totale, prossimo cursore,
origine e problemi dei cataloghi. Una vista invalida espone `recovery`
con directory e comando dichiarato dal progetto. La lettura non lo esegue.

Per registrare una campagna apri una lavorazione, collega
`.gdev-assurance/work/gdev-integration.eld` e l'istantanea del progetto,
segui le transizioni consentite e usa `esegui-procedura` in verifica.
Le procedure `GS-GDEV-METADATA`, `GS-FORMAT` e `GS-NATIVE` chiamano i
controlli reali. Il database locale `.gdev/` è ignorato da Git e conserva
tentativi, errori e revisioni. Una modifica successiva alle fonti invalida
le prove precedenti. Le approvazioni umane restano passaggi separati.

Nel modulo agenti di GDev, `C-c a o` apre Codex nel progetto e `C-c a O`
usa il selettore nativo per riprendere una sessione. `C-c a d` apre il
confronto delle modifiche e `C-c a Q` arresta soltanto quella sessione.
GDev passa il percorso del proprio server MCP alla CLI senza cambiare le
protezioni impostate dall'utente. Dal terminale, `.codex/config.toml`
richiede il comando `gdev-mcp` nel PATH, installato dal bootstrap di GDev.
La CLI Codex deve essere già installata e autenticata.

Per C++ usa il preset CMake desiderato e il suo
`build/<preset>/compile_commands.json`. Il formatter richiesto è
`.clang-format-version`. Prima di consegnare esegui sempre i controlli
di `scripts/dev.sh`, anche quando l'editor non segnala problemi.

Il ciclo di correzione riproduce il difetto, corregge il livello che lo
governa, aggiunge una regressione e ripete il flusso tra i due progetti.
Conserva i tentativi falliti. Se manca una dipendenza o una decisione
esterna, registra il limite osservato e la condizione per riprendere.
Non reiterare automaticamente un'operazione che continua a fallire
senza nuove informazioni.

Nelle letture paginate confronta `inputs_sha256` dei cataloghi tra le pagine.
Se cambia, riparti dalla prima pagina sulla nuova revisione delle fonti.

Per usare gli strumenti locali anche nell'editor, `M-x gdev-trust-project`
usa il controllo di fiducia già previsto da GDev. Per clangd seleziona il
preset CMake del progetto; la sola presenza di più preset non equivale
alla scelta di uno di essi.
