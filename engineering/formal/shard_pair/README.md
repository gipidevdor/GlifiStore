# Shard pair TLA+ model

A reduced formal model of one paired Reader–Writer shard: publish, adopt and shutdown drain.

Linked requirement: `GS-CONCUR-TLA-001`.

## Run TLC

```bash
export TLA2TOOLS_JAR=/path/to/tla2tools.jar
./engineering/formal/shard_pair/run-tlc.sh
```

CI runs it as a required bounded run in `.github/workflows/formal-models.yml`; TLC errors and the
timeout fail closed.

## Residual gaps

Durable I/O, compaction leases, multi-shard routing and full Store API linearizability are outside
this model's scope; see the separate [persistence model](../persistence/README.md) and the C++
history checker.
