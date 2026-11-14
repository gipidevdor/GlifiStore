# GlifiStore Perl client

`GlifiStore::Client` is a native, synchronous Perl client for GlifiStore wire protocol v2.

## Guarantees

- Keeps one bound TCP connection per Worker.
- Routes each binary key to its owner Worker as
  [ADR 0030](../../docs/adr/0030-keyed-worker-routing.md) specifies: a plain `GlifiStore/2` INIT
  identity means canonical FNV-1a 64-bit; the extended INIT identity selects SipHash-2-4.
- Supports arbitrary byte strings.
- Follows [client semantics v1](../../docs/spec/client-semantics-v1.md) for portable error, retry
  and deadline rules.
- Retries safe reads, including transient `unavailable` reconnects while routing metadata is stable.
- Preserves indeterminate mutation outcomes.

## Example

```perl
use GlifiStore::Client;

my $cache = GlifiStore::Client->connect(host => '127.0.0.1', port => 7379);
my $stored = $cache->put("session\x00key", "payload");
my $value = $cache->get("session\x00key") if $stored->{outcome} eq 'committed';
$cache->close;
```

## Install

The client needs Perl 5.32 or later and uses only core modules.

From CPAN, once published:

```bash
cpanm GlifiStore
```

From this source tree:

```bash
cd sdk/perl && perl Makefile.PL && make && make test && make install
```

The license is BSD-3-Clause.

## Configuration

`connect` takes named options, such as `host` and `port` in the example. Besides TCP, the client
can dial an AF_UNIX socket.

## TLS

Connections are cleartext TCP by default, without authentication: use cleartext only on loopback, a
private network or a sidecar. TLS 1.3 is opt-in with `tls => 1`, plus `tls_ca` or `ca_file`,
`cert_file` and `key_file` for mTLS, `server_name`, and `insecure_skip_verify` for lab use only.
Hostname and SNI verification is on by default.

TLS requires `IO::Socket::SSL`; without it, a TLS request fails closed. See the
[security roadmap](../../docs/security/roadmap.md); on OpenBSD the daemon uses LibreSSL.

## Errors and mutation outcomes

A mutation result is a hash reference whose `outcome` is `committed`, `rejected` or
`indeterminate`. Check it before relying on a write, as the example does.

## Pipelines and batches

Ordered, non-atomic pipelines take hash references and return one positional result per request.
`execute_batch` groups requests by Worker, overlaps the per-Worker pipelines and restores caller
order; it is not a transaction. When the work is already split per Worker, pass the per-Worker
vectors to `execute_worker_pipelines`.

```perl
my $responses = $cache->execute_pipeline([
    { opcode => 'put', key => 'key', value => 'value' },
    { opcode => 'get', key => 'key' },
]);
my $ordered = $cache->execute_batch(\@mixed_worker_requests);
```

## Backup and lifecycle probes

`$client->backup($destination, timeout => $seconds)` issues wire `BACKUP` and returns the bounded
ASCII report. The destination is a new, empty server-side path. BACKUP is an admin operation under
secure authz and an online fenced copy, not a zero-fence hot snapshot. An ambiguous failure is
reconciled first and never retried blindly.

`$client->health`, `$client->ready` and `$client->stats` issue wire `HEALTH`, `READY` and `STATS`
with an empty key and value.

## Concurrency

`GlifiStore::Client` is not shareable across ithreads and has no internal mutexes. Never use a
socket created before `fork` in the child: create the client after `fork`, one per process, and one
per thread if you use threads. Within one process, `execute_worker_pipelines` overlaps Workers.

## Performance

The client is pure Perl, designed pipeline-first rather than as a sequence of object-heavy
synchronous calls:

- a Worker pipeline appends validated header, key and value fragments into one contiguous scalar,
  without building a complete temporary frame per request, and drains it under one absolute
  deadline;
- response bytes accumulate in a reusable connection buffer, and several complete frames are parsed
  from each `sysread`;
- the client decoder keeps response metadata in a compact internal tuple instead of one transient
  six-field hash per frame; the public protocol decoder keeps its named-hash contract;
- pipeline result slots are built once, at their decided success or failure outcome, instead of
  placeholder failure hashes that the success path immediately replaces;
- `execute_worker_pipelines` drives every active Worker socket through one `IO::Select` loop and
  consumes the readiness that loop already returned before waiting again; a fragmented response
  returns to the same absolute-deadline wait;
- `execute_batch` hashes each key exactly once while grouping, reuses that validated ownership while
  encoding, overlaps the Worker pipelines, then restores caller order;
- Worker routing reuses the immutable identity validated during `INIT`; arbitrary routing hashes
  passed to the public protocol helpers are still normalized and checked on every call.

These choices reduce syscalls and expose server parallelism. They do not make the Perl path
allocation-free: public requests and results are hash references, non-pipeline calls assemble
encoded frames, and successful GET values become owned Perl scalars. These costs are measurable and
must not be described as negligible without a profile.

For a throughput-sensitive application:

- send ordered pipelines instead of alternating synchronous `put` and `get` calls;
- use `execute_batch` for mixed owners, or `execute_worker_pipelines` when the work is already
  sharded;
- choose the pipeline depth from a throughput and tail-latency sweep on the deployment workload.

Optimization follows measured cost, in this order: Perl-level allocation and copy reduction, buffer
reuse and parser shape; then syscall and scheduling behavior; then, if profiles still place CPU
there, an optional narrow XS codec or routing kernel. XS is a possible tool, not a decided answer.
Wrapping the C++ client through FFI is outside the current native-SDK design.

Run the benchmarks:

```bash
# Sequential and concurrent matrix from the repository root:
./scripts/benchmark_perl_client.sh
# Set WORKER_HASH_SEED to run the same matrix with the server's keyed SipHash routing.

# One configuration (default overlaps Workers when workers > 1):
perl sdk/perl/benchmarks/client_benchmark.pl --port 7379 --workers 4 \
  --ops 100000 --pipeline 128 --warmup 1 --repeats 7
# --no-concurrent forces sequential drain across Workers
# --batch measures mixed-owner execute_batch grouping and ordered results
```

Compare SDKs only through `./scripts/benchmark_sdk_clients.sh`, which fixes the validated workload
and result semantics. Do not turn a single local run or an old cross-runtime ratio into a client
limit.

## Limitations

- The client does not combine AF_UNIX with TLS: it refuses that configuration fail-closed.
- `connections_per_worker` and an event-loop adapter remain measurement-gated. More connections may
  help hot-Worker concurrency but add ordering, memory and reconnect complexity; an async adapter
  may improve application concurrency without improving single-pipeline CPU throughput.

## Development

From the repository root:

```bash
./scripts/test-perl-client.sh
./scripts/package-perl-client.sh
```

`./scripts/test-perl-client.sh` runs Perl::Critic at severity 1 (brutal) against `lib/` with
`.perlcriticrc`, then the unit tests. Install the develop tools from `sdk/perl/` with
`cpanm --installdeps --with-develop .`, or with `cpanm Perl::Critic Perl::Tidy`.

`GlifiStore::Protocol` exposes the full bidirectional codec and FNV-1a Worker routing.

[Perl client packaging](PACKAGING.md) describes the packaging check and the PAUSE and MetaCPAN
upload steps.
