;;; hot-reader.eld --- Riduzione delle copie GET -*- lisp-data -*-
(:version 1 :title "Ridurre le copie del Reader sullo shard caldo" :type feature
 :objective "Eliminare la materializzazione intermedia dei GET volatili copiati nel buffer TCP, con lifetime esplicito e confronto locale."
 :scope "Borrow interno sotto la lease della generazione, codifica contigua sincrona. Ownership, wire v2, retry, acknowledgement e percorso durable invariati. HAZ-045/046 restano aperti."
 :requirements ("GS-GDEV-001")
 :inputs ("AGENTS.md" "engineering/hazards/routing-capacity.yaml" "docs/adr/0048-volatile-tcp-response-borrow.md")
 :prerequisites ("Mandato utente: Ok correggi. Esecutore Codex. Nessuna revisione o accettazione umana del risultato è attestata.")
 :owner "Manutentori GlifiStore" :priority must :depends-on nil
 :procedure (:id "GS-CAPACITY" :version 1) :snippets nil
 :acceptance ("Viste validate e generazione protetta fino alla copia"
              "Risposte TCP proprietarie prima del ritorno al ciclo eventi"
              "Test e confronto locale conservati con rischi aperti")
 :artifacts nil :revisions ("La riduzione delle copie non implementa il bilanciamento dei Worker."))
