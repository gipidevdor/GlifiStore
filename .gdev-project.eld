;;; .gdev-project.eld --- Operazioni condivise, senza percorsi locali -*- lisp-data -*-
(:version 1 :default "Sviluppo"
 :profiles (("Sviluppo" :cwd "."
             :configure ("./scripts/dev.sh" "configure")
             :build ("./scripts/dev.sh" "build")
             :test ("./scripts/dev.sh" "test"))
            ("ASan" :cwd "." :test ("./scripts/dev.sh" "asan"))
            ("TSan" :cwd "." :test ("./scripts/dev.sh" "tsan"))
            ("Verifiche rapide" :cwd "." :test ("./scripts/dev.sh" "verify"))))
