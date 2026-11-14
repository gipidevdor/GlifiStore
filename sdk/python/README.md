# GlifiStore Python client

The `glifistore` package is a pure-Python client for GlifiStore wire protocol v2, with a synchronous
`Client` and an `asyncio` `AsyncClient` that share one codec, configuration and outcome model.

## Guarantees

- Opens and binds one TCP connection per Worker.
- Routes each binary key to its owner Worker as
  [ADR 0030](../../docs/adr/0030-keyed-worker-routing.md) specifies: a plain `GlifiStore/2` INIT
  identity means canonical FNV-1a 64-bit; the extended INIT identity selects SipHash-2-4.
- Follows [client semantics v1](../../docs/spec/client-semantics-v1.md) for portable error, retry
  and deadline rules.
- Retries reads after a transient disconnect.
- Never reports an uncertain mutation as rejected.

## Example

```python
from glifistore import Client

with Client.connect() as cache:
    stored = cache.put(b"session:42", b"payload")
    if stored.committed:
        print(cache.get(b"session:42"))
```

## Install

The client needs Python 3.11 or later and has no runtime dependency outside the standard library.

From PyPI, once published:

```bash
pip install glifistore
```

From this source tree:

```bash
pip install ./sdk/python
```

The license is BSD-3-Clause.

## Configuration

Both clients take their settings from a `ClientConfig`. Besides TCP, both can dial an AF_UNIX
socket.

## TLS

Connections are cleartext TCP by default, without authentication: use cleartext only on loopback, a
private network or a sidecar. TLS 1.3 is opt-in on both clients: set `tls=True` in `ClientConfig`,
with `tls_ca`, `cert_file`, `key_file`, `server_name` and `insecure_skip_verify`. Hostname and SNI
verification is on by default, and TLS fails closed, with no cleartext fallback.

mTLS authentication follows [ADR 0021](../../docs/adr/0021-secure-profile-authentication.md) and the
[security roadmap](../../docs/security/roadmap.md). On OpenBSD the daemon uses LibreSSL.

## Errors and mutation outcomes

A mutation ends `committed`, `rejected` or `indeterminate`. Check `committed` on the result before
relying on a write, as the example does. Pipeline responses report `succeeded` for each position.

## Pipelines and batches

This `AsyncClient` example runs an ordered pipeline, a multi-Worker batch and pre-sharded
per-Worker pipelines:

```python
from glifistore import AsyncClient, PipelineOpcode, PipelineRequest

async with await AsyncClient.connect() as cache:
    responses = await cache.execute_pipeline([
        PipelineRequest(PipelineOpcode.PUT, b"key", b"value"),
        PipelineRequest(PipelineOpcode.GET, b"key"),
    ])
    assert all(response.succeeded for response in responses)
    # Multi-Worker: groups by Worker, overlaps pipelines, restores caller order.
    ordered = await cache.execute_batch([
        PipelineRequest(PipelineOpcode.PUT, b"a", b"1"),
        PipelineRequest(PipelineOpcode.PUT, b"b", b"2"),
        PipelineRequest(PipelineOpcode.GET, b"a"),
        PipelineRequest(PipelineOpcode.GET, b"b"),
    ])
    # Pre-sharded Worker vectors (len == worker_count).
    by_worker = await cache.execute_worker_pipelines([
        [PipelineRequest(PipelineOpcode.GET, b"a")],
        [PipelineRequest(PipelineOpcode.GET, b"b")],
    ])
```

## Backup and lifecycle probes

`Client.backup(destination)` and `await AsyncClient.backup(destination)` issue wire `BACKUP`, which
creates an online, fenced copy of the durable catalog in a new, empty server-side path. They return
the bounded ASCII report as `bytes`. BACKUP is an admin operation under secure authz and is not a
zero-fence hot snapshot. An ambiguous outcome, from the transport or `INTERNAL_ERROR`, requires
reconciliation, not a blind retry.

`health()`, `ready()` and `stats()` issue wire `HEALTH`, `READY` and `STATS` with an empty key and
value, on both clients.

## Concurrency

`Client` is thread-safe. For `asyncio` code, use `AsyncClient`.

## Performance

Start `glifistored`, then run from `sdk/python` the same ordered `PUT`/`GET` workload as the C++
client benchmark:

```bash
python3 benchmarks/client_benchmark.py --port 7379 --workers 4 \
  --ops 100000 --pipeline 128 --warmup 1 --repeats 7
```

`--execution batch` benchmarks `execute_batch()` grouping and fan-out directly, and
`--runtime async --execution batch` does the same for `AsyncClient`. Benchmark keys are assigned
with the routing identity negotiated with the running server, keyed SipHash configurations included.

## Limitations

Neither client combines AF_UNIX with TLS: both refuse that configuration fail-closed.

## Development

From the repository root, run the tests and verify the packaging:

```bash
./scripts/test-python-client.sh
./scripts/package-python-client.sh
```

[Python client packaging](PACKAGING.md) lists what the packaging check builds and installs, the
TestPyPI and PyPI upload steps and the version-bump checklist.
