---- MODULE PersistenceRecovery ----
\* Bounded abstract model of the persistence-v1 replacement publication path.
\* Requirement: GS-PERSIST-ORDER-001.
\*
\* The model deliberately treats an unsynchronized write or rename as
\* nondeterministic at crash time. A completed synchronization is durable in
\* the model. This is a logical ordering/recovery proof, not a filesystem,
\* controller, firmware, E3, or E4 certification.

EXTENDS TLC

VARIABLES
  phase,
  recordWritten,
  recordSynced,
  slotWritten,
  slotSynced,
  manifestWritten,
  manifestSynced,
  manifestRenamed,
  directorySynced,
  diskRecord,
  diskSlot,
  diskManifest,
  faultInjected,
  outcome

vars ==
  <<phase, recordWritten, recordSynced, slotWritten, slotSynced,
    manifestWritten, manifestSynced, manifestRenamed, directorySynced,
    diskRecord, diskSlot, diskManifest, faultInjected, outcome>>

Phases == {"running", "recovering", "ready", "failed"}
Manifests == {"old", "new"}
Outcomes == {"none", "must_not_exist", "must_exist", "failed_closed"}

TypeOK ==
  /\ phase \in Phases
  /\ recordWritten \in BOOLEAN
  /\ recordSynced \in BOOLEAN
  /\ slotWritten \in BOOLEAN
  /\ slotSynced \in BOOLEAN
  /\ manifestWritten \in BOOLEAN
  /\ manifestSynced \in BOOLEAN
  /\ manifestRenamed \in BOOLEAN
  /\ directorySynced \in BOOLEAN
  /\ diskRecord \in BOOLEAN
  /\ diskSlot \in BOOLEAN
  /\ diskManifest \in Manifests
  /\ faultInjected \in BOOLEAN
  /\ outcome \in Outcomes

Init ==
  /\ phase = "running"
  /\ recordWritten = FALSE
  /\ recordSynced = FALSE
  /\ slotWritten = FALSE
  /\ slotSynced = FALSE
  /\ manifestWritten = FALSE
  /\ manifestSynced = FALSE
  /\ manifestRenamed = FALSE
  /\ directorySynced = FALSE
  /\ diskRecord = FALSE
  /\ diskSlot = FALSE
  /\ diskManifest = "old"
  /\ faultInjected = FALSE
  /\ outcome = "none"

WriteRecord ==
  /\ phase = "running"
  /\ ~recordWritten
  /\ recordWritten' = TRUE
  /\ UNCHANGED <<phase, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected, outcome>>

SyncRecord ==
  /\ phase = "running"
  /\ recordWritten
  /\ ~recordSynced
  /\ recordSynced' = TRUE
  /\ diskRecord' = TRUE
  /\ UNCHANGED <<phase, recordWritten, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskSlot, diskManifest, faultInjected, outcome>>

WriteSlot ==
  /\ phase = "running"
  /\ recordSynced
  /\ ~slotWritten
  /\ slotWritten' = TRUE
  /\ UNCHANGED <<phase, recordWritten, recordSynced, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected, outcome>>

SyncSlot ==
  /\ phase = "running"
  /\ slotWritten
  /\ ~slotSynced
  /\ slotSynced' = TRUE
  /\ diskSlot' = TRUE
  /\ UNCHANGED <<phase, recordWritten, recordSynced, slotWritten,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskRecord, diskManifest, faultInjected, outcome>>

WriteManifest ==
  /\ phase = "running"
  /\ slotSynced
  /\ ~manifestWritten
  /\ manifestWritten' = TRUE
  /\ UNCHANGED <<phase, recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestSynced, manifestRenamed, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected, outcome>>

SyncManifest ==
  /\ phase = "running"
  /\ manifestWritten
  /\ ~manifestSynced
  /\ manifestSynced' = TRUE
  /\ UNCHANGED <<phase, recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestRenamed, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected, outcome>>

RenameManifest ==
  /\ phase = "running"
  /\ manifestSynced
  /\ ~manifestRenamed
  /\ manifestRenamed' = TRUE
  /\ UNCHANGED <<phase, recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected, outcome>>

SyncDirectory ==
  /\ phase = "running"
  /\ manifestRenamed
  /\ ~directorySynced
  /\ directorySynced' = TRUE
  /\ diskManifest' = "new"
  /\ UNCHANGED <<phase, recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed,
                  diskRecord, diskSlot, faultInjected, outcome>>

\* At a process stop, an issued but unsynchronized write/rename may either be
\* absent or durable. Synchronized state cannot move backwards.
Crash ==
  /\ phase = "running"
  /\ \E survivingRecord \in
          IF recordWritten /\ ~recordSynced THEN {diskRecord, TRUE} ELSE {diskRecord}:
       \E survivingSlot \in
            IF slotWritten /\ ~slotSynced THEN {diskSlot, TRUE} ELSE {diskSlot}:
         \E survivingManifest \in
              IF manifestRenamed /\ ~directorySynced
                THEN {diskManifest, "new"}
                ELSE {diskManifest}:
           /\ phase' = "recovering"
           /\ diskRecord' = survivingRecord
           /\ diskSlot' = survivingSlot
           /\ diskManifest' = survivingManifest
           /\ UNCHANGED <<recordWritten, recordSynced, slotWritten, slotSynced,
                           manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                           faultInjected, outcome>>

\* Negative proof seam: committed authority with missing committed content must
\* never be served and must be classified fail-closed.
InjectCommittedCorruption ==
  /\ phase = "running"
  /\ diskManifest = "new"
  /\ \/ /\ diskRecord' = FALSE
        /\ diskSlot' = diskSlot
     \/ /\ diskRecord' = diskRecord
        /\ diskSlot' = FALSE
  /\ phase' = "recovering"
  /\ faultInjected' = TRUE
  /\ UNCHANGED <<recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskManifest, outcome>>

RecoverOld ==
  /\ phase = "recovering"
  /\ diskManifest = "old"
  /\ phase' = "ready"
  /\ outcome' = "must_not_exist"
  /\ UNCHANGED <<recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected>>

RecoverNew ==
  /\ phase = "recovering"
  /\ diskManifest = "new"
  /\ diskRecord
  /\ diskSlot
  /\ phase' = "ready"
  /\ outcome' = "must_exist"
  /\ UNCHANGED <<recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected>>

RejectInvalidNew ==
  /\ phase = "recovering"
  /\ diskManifest = "new"
  /\ ~(diskRecord /\ diskSlot)
  /\ phase' = "failed"
  /\ outcome' = "failed_closed"
  /\ UNCHANGED <<recordWritten, recordSynced, slotWritten, slotSynced,
                  manifestWritten, manifestSynced, manifestRenamed, directorySynced,
                  diskRecord, diskSlot, diskManifest, faultInjected>>

Next ==
  \/ WriteRecord
  \/ SyncRecord
  \/ WriteSlot
  \/ SyncSlot
  \/ WriteManifest
  \/ SyncManifest
  \/ RenameManifest
  \/ SyncDirectory
  \/ Crash
  \/ InjectCommittedCorruption
  \/ RecoverOld
  \/ RecoverNew
  \/ RejectInvalidNew

Behavior == Init /\ [][Next]_vars

Spec ==
  /\ Behavior
  /\ WF_vars(RecoverOld)
  /\ WF_vars(RecoverNew)
  /\ WF_vars(RejectInvalidNew)

WriteOrder ==
  /\ recordSynced => recordWritten
  /\ slotWritten => recordSynced
  /\ slotSynced => slotWritten
  /\ manifestWritten => slotSynced
  /\ manifestSynced => manifestWritten
  /\ manifestRenamed => manifestSynced
  /\ directorySynced => manifestRenamed

NormalAuthorityComplete ==
  (~faultInjected /\ diskManifest = "new") => (diskRecord /\ diskSlot)

RecoveryOracle ==
  phase = "ready" =>
    \/ /\ diskManifest = "old"
       /\ outcome = "must_not_exist"
    \/ /\ diskManifest = "new"
       /\ diskRecord
       /\ diskSlot
       /\ outcome = "must_exist"

CommittedCorruptionFailsClosed ==
  faultInjected =>
    /\ phase # "ready"
    /\ (phase = "failed" => outcome = "failed_closed")

RecoveryTerminates ==
  phase = "recovering" ~> phase \in {"ready", "failed"}

CommittedCorruptionIsRejected ==
  faultInjected ~> phase = "failed"

====
