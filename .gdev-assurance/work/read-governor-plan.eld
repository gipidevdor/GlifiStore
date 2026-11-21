;;; read-governor-plan.eld --- Governo della capacità GET -*- lisp-data -*-
(:version 1 :title "Pianificare il governo adattivo della capacità di lettura" :type docs
 :objective "Definire capacità sostenibile, soglie qualificate, budget globale, equità, lifetime e costo per lettura utile per valorizzare hardware e integrazioni."
 :scope "Piano e ADR proposta, requisiti in bozza e gate non implementato. Nessuna attivazione di helper o modifica al runtime."
 :requirements ("GS-GDEV-001")
 :inputs ("AGENTS.md" "engineering/hazards/routing-capacity.yaml" "engineering/hazards/durable-group-rejections.yaml"
          "docs/adr/0048-volatile-tcp-response-borrow.md")
 :prerequisites ("Mandato utente: partire dal piano di gestione della capacità di lettura, usare la capacità disponibile e aggiungere lettori su altri core oltre il 70% di carico; ragionare bene sul modello di governo. Esecutore Codex.")
 :owner "Manutentori GlifiStore" :priority must :depends-on nil
 :procedure (:id "GS-GDEV-METADATA" :version 1) :snippets nil
 :acceptance ("Capacità utile distinta da percentuale CPU e throughput osservato"
              "Decisioni di espansione, ritiro, equità e budget definite senza oscillazioni note"
              "Fasi di realizzazione e prove mancanti esplicite; nessuna chiusura di hazard o gate")
 :artifacts nil :revisions ("La pianificazione non implementa né accetta il modello concorrente proposto."
                           "L’utente autorizza soglie migliori dell’esempio iniziale e chiede di proteggere gli investimenti. I candidati 65/45% restano da qualificare; nessun ritorno economico è attestato."))
