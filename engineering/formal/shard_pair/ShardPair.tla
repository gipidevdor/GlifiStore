---- MODULE ShardPair ----
\* Reduced TLA+ model of one GlifiStore paired shard (ADR 0031/0032).
\* Requirement: GS-CONCUR-TLA-001.

EXTENDS Naturals, Sequences, TLC

CONSTANTS MaxEpoch, MaxQueue, MaxClients

ASSUME MaxEpoch \in Nat /\ MaxEpoch >= 1
ASSUME MaxQueue \in Nat /\ MaxQueue >= 1
ASSUME MaxClients \in Nat /\ MaxClients >= 1

VARIABLES epoch, published, queue, shuttingDown, drained, client, adopted

ClientState == {"idle", "mutating", "reading", "done"}

TypeOK ==
  /\ epoch \in 0..MaxEpoch
  /\ published \in 0..MaxEpoch
  /\ queue \in 0..MaxQueue
  /\ shuttingDown \in BOOLEAN
  /\ drained \in BOOLEAN
  /\ client \in [0..(MaxClients-1) -> ClientState]
  /\ adopted \in [0..(MaxClients-1) -> 0..MaxEpoch]

Init ==
  /\ epoch = 0
  /\ published = 0
  /\ queue = 0
  /\ shuttingDown = FALSE
  /\ drained = FALSE
  /\ client = [c \in 0..(MaxClients-1) |-> "idle"]
  /\ adopted = [c \in 0..(MaxClients-1) |-> 0]

Enqueue(c) ==
  /\ ~shuttingDown /\ ~drained /\ client[c] = "idle" /\ queue < MaxQueue
  /\ epoch + queue < MaxEpoch
  /\ queue' = queue + 1
  /\ client' = [client EXCEPT ![c] = "mutating"]
  /\ UNCHANGED <<epoch, published, shuttingDown, drained, adopted>>

WriterStep ==
  /\ ~drained /\ queue > 0 /\ epoch < MaxEpoch
  /\ queue' = queue - 1
  /\ epoch' = epoch + 1
  /\ published' = epoch + 1
  /\ UNCHANGED <<shuttingDown, drained, client, adopted>>

Adopt(c) ==
  /\ ~drained /\ client[c] \in {"idle", "reading"} /\ published > 0
  /\ adopted' = [adopted EXCEPT ![c] = published]
  /\ client' = [client EXCEPT ![c] = "reading"]
  /\ UNCHANGED <<epoch, published, queue, shuttingDown, drained>>

CompleteMutation(c) ==
  /\ client[c] = "mutating" /\ published >= 1
  /\ client' = [client EXCEPT ![c] = "idle"]
  /\ UNCHANGED <<epoch, published, queue, shuttingDown, drained, adopted>>

FinishRead(c) ==
  /\ client[c] = "reading"
  /\ client' = [client EXCEPT ![c] = "idle"]
  /\ adopted' = [adopted EXCEPT ![c] = 0]
  /\ UNCHANGED <<epoch, published, queue, shuttingDown, drained>>

BeginShutdown ==
  /\ ~shuttingDown
  /\ shuttingDown' = TRUE
  /\ UNCHANGED <<epoch, published, queue, drained, client, adopted>>

Drain ==
  /\ shuttingDown /\ ~drained /\ queue = 0
  /\ \A c \in 0..(MaxClients-1) : client[c] # "mutating"
  /\ drained' = TRUE
  /\ UNCHANGED <<epoch, published, queue, shuttingDown, client, adopted>>

Next ==
  \/ \E c \in 0..(MaxClients-1) : Enqueue(c)
  \/ WriterStep
  \/ \E c \in 0..(MaxClients-1) : Adopt(c)
  \/ \E c \in 0..(MaxClients-1) : CompleteMutation(c)
  \/ \E c \in 0..(MaxClients-1) : FinishRead(c)
  \/ BeginShutdown
  \/ Drain

vars == <<epoch, published, queue, shuttingDown, drained, client, adopted>>

Behavior == Init /\ [][Next]_vars

\* Once shutdown starts, fair Writer/completion/drain scheduling must empty the
\* bounded lane and reach the terminal drained state.
Spec ==
  /\ Behavior
  /\ WF_vars(WriterStep)
  /\ WF_vars(Drain)
  /\ \A c \in 0..(MaxClients-1) : WF_vars(CompleteMutation(c))

AdoptedLeqPublished == \A c \in 0..(MaxClients-1) : adopted[c] <= published
DrainEmpty == drained => (queue = 0 /\ \A c \in 0..(MaxClients-1) : client[c] # "mutating")
Liveness == shuttingDown ~> drained

====
