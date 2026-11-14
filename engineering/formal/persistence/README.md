# Persistence and recovery TLA+ model

A bounded, abstract TLA+ model of the persistence v1 replacement-publication chain:

```text
Record write -> Record sync -> commit-slot write -> commit-slot sync
-> Manifest write/sync/rename -> directory sync -> crash -> recovery oracle
```

Linked requirement: `GS-PERSIST-ORDER-001`. The normative behavior stays in
[persistence v1](../../../docs/spec/persistence-v1.md) and the
[recovery state-transition matrix v1](../../../docs/spec/recovery-state-matrix-v1.md).

## What it checks

The crash transition explores both permitted outcomes of an issued but incomplete write or rename.
The recovery oracle admits only the old complete authority (`MUST_NOT_EXIST`) or the new complete
authority (`MUST_EXIST`). An explicit committed-corruption transition must end fail-closed. The
model is the formal counterpart of the concrete SIGKILL matrices.

Weak fairness is declared only for the three terminating recovery actions, so that TLC can verify
that recovery reaches `ready` or `failed`; the model assumes no fairness for writes or crashes.

The model does not cover the filesystem, controller, firmware, physical power loss, timing or the
full byte codecs. It therefore does not raise the E2 claim ceiling or establish E3 or E4.

## Run TLC

```bash
export TLA2TOOLS_JAR=/path/to/tla2tools.jar
./engineering/formal/persistence/run-tlc.sh
```

CI runs it in `.github/workflows/formal-models.yml`. TLC errors and the bounded wall-clock timeout
fail closed.
