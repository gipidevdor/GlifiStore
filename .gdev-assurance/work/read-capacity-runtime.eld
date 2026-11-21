;;; read-capacity-runtime.eld --- Controllo e lettori GET -*- lisp-data -*-
(:version 1 :title "Implementare controllo deterministico e helper GET limitati" :type feature
 :objective "Realizzare decisioni riproducibili, limiti rigidi e delega GET con protezione della generazione e fallback sul Reader."
 :scope "Controllore puro, pool condiviso sperimentale e integrazione volatile GET. Qualifica delle soglie e delle piattaforme separata dalle prove locali."
 :requirements ("GS-GDEV-001")
 :inputs ("AGENTS.md" "docs/adr/0049-adaptive-read-capacity.md" "engineering/hazards/routing-capacity.yaml" "engineering/hazards/durable-group-rejections.yaml")
 :prerequisites ("Mandato esplicito dell’utente: Implementa corri. Esecutore Codex.")
 :owner "Manutentori GlifiStore" :priority must :depends-on nil
 :procedure (:id "GS-READ-CAPACITY" :version 1) :snippets nil
 :acceptance ("Stessa traccia e configurazione producono le stesse decisioni"
              "Budget occupato fino al termine del drain; profilo sconosciuto non attiva helper"
              "Nessun prestito senza protezione e nessun cambiamento a routing, visibilità o ordine delle risposte")
 :artifacts nil :revisions ("L’implementazione locale non qualifica capacità sostenibile né chiude i gate di rilascio."))
