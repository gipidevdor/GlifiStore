;;; gdev-integration.eld --- Integrazione dello sviluppo -*- lisp-data -*-
(:version 1 :title "GlifiStore in GDev e Codex" :type feature
 :objective "Usare cataloghi YAML autorevoli e procedure reali dal progetto formale GDev."
 :scope "Strumenti, manifesto, catalogo derivato e istruzioni agli agenti. Correzione della prenotazione spill su macOS secondo ADR 0046 e GS-OPS-MEMRES-001; nessun cambio a persistenza o acknowledgement."
 :requirements ("GS-GDEV-001") :inputs ("AGENTS.md" "docs/development/gdev.md" "engineering/requirements/durable-reads.yaml" "docs/adr/0046-unified-memory-reserve.md")
 :prerequisites ("Mandato utente: Procedi. GDev deve beneficiare genericamente di ogni errore e il flusso deve essere ergonomico anche per gli agenti. Esecutore Codex.")
 :owner "Manutentori GlifiStore" :priority must :depends-on nil
 :procedure (:id "GS-GDEV-METADATA" :version 1) :snippets nil
 :acceptance ("Catalogo corrente e controlli riusciti sulla revisione corrente"
              "Tentativi falliti conservati e gate originali inalterati")
 :artifacts nil :revisions ("L'autorizzazione a implementare non costituisce revisione umana del risultato."))
