# GlifiStore Go client

The module `github.com/gpicchiarelli/GlifiStore/sdk/go` is a native Go client for GlifiStore wire
protocol v2.

## Guarantees

- Opens one TCP connection per Worker, with `TCP_NODELAY`.
- Routes each binary key to its owner Worker as
  [ADR 0030](../../docs/adr/0030-keyed-worker-routing.md) specifies: a plain `GlifiStore/2` INIT
  identity means canonical FNV-1a 64-bit; the extended INIT identity selects SipHash-2-4.
- Implements [client semantics v1](../../docs/spec/client-semantics-v1.md): portable error
  categories, monotonic request deadlines and `committed`, `rejected` and `indeterminate` mutation
  outcomes.
- Retries safe reads after a transient disconnect, with at most one automatic retry.
- Never reports an uncertain mutation as rejected.

## Example

```go
package main

import (
	"fmt"
	"log"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/client"
)

func main() {
	cache, err := client.Connect(client.Config{Host: "127.0.0.1", Port: 7379})
	if err != nil {
		log.Fatal(err)
	}
	defer cache.Close()

	if !cache.Put([]byte("session\x0042"), []byte("payload"), 0).Committed() {
		log.Fatal("put rejected")
	}
	value, err := cache.Get([]byte("session\x0042"))
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("%q\n", value)
}
```

## Install

The client needs Go 1.27 or later.

```bash
go get github.com/gpicchiarelli/GlifiStore/sdk/go@latest
```

The module path keeps the `github.com/gpicchiarelli/GlifiStore` prefix, while the repository now
lives at `github.com/gipidevdor/GlifiStore` and the old URL redirects. No `sdk/go/v*` tag exists
yet, so there is no tagged module release; the tag format is in [Go client packaging](PACKAGING.md).

`client.Version` holds the client version, which must match the repository `VERSION`. The license
is BSD-3-Clause.

## Configuration

`client.Connect` takes a `client.Config`; the example sets `Host` and `Port`. Besides TCP, the
client can dial an AF_UNIX socket.

## TLS

Connections are cleartext by default: treat the server as loopback, private network or sidecar.
TLS 1.3 is opt-in through `Config.TLS`: `Enable`, `CAFile`, `CertFile` and `KeyFile` for mTLS,
`ServerName`, and `InsecureSkipVerify` as a lab-only escape. Hostname and SNI verification is on by
default, and a TLS request never falls back silently to cleartext.

See the [security roadmap](../../docs/security/roadmap.md) and
[ADR 0020](../../docs/adr/0020-tls-outer-transport.md),
[ADR 0021](../../docs/adr/0021-secure-profile-authentication.md) and
[ADR 0022](../../docs/adr/0022-authorization-capabilities.md). OpenBSD is a first-class target, with
LibreSSL on the daemon.

## Errors and mutation outcomes

`Put` and `Erase` return a result whose `Committed()` is true only for the `committed` outcome; the
example stops when it is false.

## Pipelines and batches

Pipelines are single-Worker. `ExecuteBatch` groups requests by Worker, overlaps goroutines and
restores caller order. `ExecuteWorkerPipelines` accepts one pre-sharded vector per Worker.

```go
responses, err := cache.ExecuteBatch([]client.PipelineRequest{
	{Opcode: client.PipelinePut, Key: []byte("a"), Value: []byte("1")},
	{Opcode: client.PipelinePut, Key: []byte("b"), Value: []byte("2")},
	{Opcode: client.PipelineGet, Key: []byte("a")},
	{Opcode: client.PipelineGet, Key: []byte("b")},
})
if err != nil {
	log.Fatal(err)
}
_ = responses
```

## Backup and lifecycle probes

`Client.Backup(destination, options...)` issues wire `BACKUP` and returns the bounded ASCII report
as `[]byte`. The destination is a new, empty server-side path. BACKUP is an admin operation under
secure authz and an online fenced copy, not a zero-fence hot snapshot. An ambiguous failure requires
reconciliation and is not retried blindly.

The client also exposes `Ping`, `Health`, `Ready` and `Stats`.

## Concurrency

Each Worker connection has its own `sync.Mutex`, and `ExecuteBatch` fans out one goroutine per
Worker. Request IDs are atomic, and health is fail-closed without a nested lock under
a connection mutex.

## Performance

Run the same PUT/GET pipeline matrix as the Python and Perl clients against a live `glifistored`:

```bash
./scripts/benchmark_go_client.sh
```

Or build the harness and point it at an existing server:

```bash
cd sdk/go && go build -o bin/glifistore-bench ./cmd/glifistore-bench
./bin/glifistore-bench --port 7379 --workers 4 --ops 100000 --pipeline 128 --execution concurrent
```

`--execution batch` measures mixed-owner `ExecuteBatch` grouping and fan-out. The benchmark
generates keys through the connected client's negotiated routing identity rather than assuming
default FNV routing.

Prefer a pipeline depth around **8–32** pairs for peak Go throughput: depth 128 often regresses on
client CPU while raw TCP keeps climbing. The implementation:

- encodes contiguous little-endian frames into pre-sized, connection-local scratch buffers;
- sets one write deadline per send and one read deadline per exchange, or per pipeline receive
  phase;
- avoids allocating empty values through `DecodeResponseView` and `OwnBytes`, and copies GET
  payloads once;
- gives `ExecuteBatch` lazily preallocated, Worker-indexed request and index vectors and disjoint
  caller-order result slots, avoiding request recopying and a result-collection mutex;
- sends a one-Worker `ExecuteBatch` directly down the validated pipeline path, keeping positional
  rejected results for group-level pre-admission failures;
- hashes every key once to validate pipeline ownership, so the first key is not routed twice.

## Limitations

The client does not combine AF_UNIX with TLS.

## Development

| Package | Role |
| --- | --- |
| `protocol` | Wire codec, FNV and SipHash routing, golden fixture tests |
| `client` | Synchronous TCP and AF_UNIX client, with pipelines and batches |
| `cmd/glifistore-interop` | CLI for `scripts/test-sdk-interop.sh` |

Run the tests from this source tree:

```bash
cd sdk/go && go test ./...
```

[Go client packaging](PACKAGING.md) describes the packaging gate, the tag format and the
version-bump checklist.
