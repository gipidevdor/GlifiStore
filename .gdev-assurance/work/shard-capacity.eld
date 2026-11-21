;;; shard-capacity.eld --- Carico TCP sbilanciato -*- lisp-data -*-
(:version 1 :title "Misure per shard di un carico TCP sbilanciato" :type feature
 :objective "Rendere riproducibile HAZ-045 con distribuzione esplicita dei client, conteggi e latenze per owner e prove del contratto del benchmark."
 :scope "Strumenti di misura del server esistente; nessuna modifica a ownership, wire v2, client, acknowledgement o runtime del motore."
 :requirements ("GS-GDEV-001")
 :inputs ("AGENTS.md" "engineering/hazards/routing-capacity.yaml" "engineering/hazards/durable-group-rejections.yaml"
          "docs/adr/0033-online-rebalance-deferred.md" "docs/spec/benchmark-standard.md")
 :prerequisites ("Mandato utente: Procedi con sviluppo, tenendo conto degli ultimi hazard. Esecutore Codex."
                 "HAZ-045/046 restano aperti. Una misura closed-loop finita non dimostra un'offerta costante né isolamento in saturazione.")
 :owner "Manutentori GlifiStore" :priority must :depends-on nil
 :procedure (:id "GS-CAPACITY" :version 1) :snippets nil
 :acceptance ("Piano dei client e conteggi per owner verificati, inclusi input non validi"
              "Profili TCP per ripetizione con latenze e finestra di completamento per owner"
              "Evidenza locale con sorgenti, parametri, limiti e tentativi falliti conservati")
 :artifacts nil :revisions ("La misura precede eventuali decisioni architetturali su bilanciamento e routing."))
