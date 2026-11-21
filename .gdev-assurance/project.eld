;;; project.eld --- GlifiStore nel progetto formale GDev -*- lisp-data -*-
(:version 1 :id "GLIFISTORE" :name "GlifiStore" :profile quality :criticality high
 :purpose "Sviluppare e verificare il prototipo architetturale GlifiStore conservando autorità, prove e rischi residui."
 :scope "Integrazione dell'ambiente di sviluppo. I contratti del motore restano nei cataloghi YAML di engineering e nelle ADR."
 :owner "Manutentori GlifiStore"
 :context "C++23, CMake, runtime paired Reader–Writer; macOS, Linux, FreeBSD e OpenBSD secondo la matrice di evidenza."
 :verification "Procedure eseguibili sui sorgenti correnti. Impronte e revisioni conservate da GDev. I gate si modificano soltanto nelle fonti autorevoli."
 :validation "Uso locale del prototipo; nessuna certificazione, revisione umana o accettazione di rilascio implicita."
 :configuration "Git, CMakePresets.json, .clang-format-version e strumenti locali .tools/venv."
 :release "Architectural prototype. Solo engineering/gates e le evidenze esistenti determinano il claim ammesso."
 :maintenance "Ogni errore riproducibile produce una correzione nel livello responsabile e una regressione riutilizzabile; i tentativi falliti restano nell'audit."
 :standards ((:id "nist-sp-800-218" :edition "1.1" :applicability not-applicable :decision accepted :scope bibliographic
              :reason "Il perimetro di questo manifesto verifica il collegamento degli strumenti. Valutazione normativa e adozione degli standard del motore restano fuori da questa integrazione e non sono attestate."))
 :catalogs ((:id "GS-ASSURANCE" :path ".gdev-assurance/catalog.json"
             :directories ("engineering/requirements" "engineering/hazards" "engineering/gates" "engineering/waivers" "engineering/claims" "engineering/schemas")
             :files ("engineering/tools/export_gdev_catalog.py")
             :refresh (".tools/venv/bin/python3" "engineering/tools/export_gdev_catalog.py")))
 :source-roots ("include" "src" "tests" "cmake" "scripts" "engineering" "docs")
 :source-files ("AGENTS.md" "CMakeLists.txt" "CMakePresets.json" ".clang-format-version" ".gdev-project.eld" ".codex/config.toml"
                ".gdev-assurance/work/gdev-integration.eld")
 :requirements
 ((:id "GS-GDEV-001" :version 1 :statement "La vista GDev conserva ID, stati e riferimenti delle fonti YAML e segnala le viste superate."
   :origin "Mandato di integrazione dell'utente" :rationale "Un'autorità per ogni dato; nessuna promozione implicita dei gate."
   :category process :priority must :criticality high :owner "Manutentori GlifiStore" :status accepted :applicability applicable
   :acceptance "Esportazione deterministica, controllo di freschezza e regressioni riusciti sulla revisione corrente."
   :method test :sources ("engineering/tools/export_gdev_catalog.py" "engineering/tools/gdev_verify.py")
   :tests ("engineering/tools/gdev_verify.py") :procedure "GS-GDEV-METADATA"))
 :procedures
 ((:id "GS-GDEV-METADATA" :version 1 :title "Cataloghi e integrazione" :mode verification :types ("feature" "bugfix" "refactor" "docs")
   :requirements ("GS-GDEV-001")
   :steps ((:id "metadata" :plan ".gdev-assurance/procedures/metadata.lavorazione" :timeout 120 :replay safe :required t
            :inputs ("engineering/tools/gdev_verify.py") :outputs nil)))
  (:id "GS-FORMAT" :version 1 :title "Formato nativo fissato dal progetto" :mode verification :types ("feature" "bugfix" "refactor" "docs")
   :steps ((:id "format" :plan ".gdev-assurance/procedures/format.lavorazione" :timeout 180 :replay safe :required t :inputs ("engineering/tools/gdev_verify.py") :outputs nil)))
  (:id "GS-NATIVE" :version 1 :title "Build e CTest del preset locale" :mode verification :types ("feature" "bugfix" "refactor" "docs")
   :steps ((:id "native" :plan ".gdev-assurance/procedures/native.lavorazione" :timeout 3600 :replay safe :required t :inputs ("engineering/tools/gdev_verify.py") :outputs nil))))
 :artifacts nil :snippets nil :risks
 ((:id "GS-GDEV-R01" :description "Una prova tecnica locale può essere confusa con una chiusura di gate o una decisione umana."
   :mitigation "Stati originali separati, standard proposti visibili e certificazione sempre non attribuita dall'integrazione."
   :owner "Manutentori GlifiStore")))
