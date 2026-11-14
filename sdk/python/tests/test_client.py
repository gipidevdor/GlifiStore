from __future__ import annotations

import asyncio
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
try:
    import glifistore  # noqa: F401
except ImportError:
    sys.path.insert(0, str(PACKAGE_ROOT / "src"))

from glifistore import (  # noqa: E402
    AsyncClient,
    Client,
    ClientConfig,
    InvalidArgument,
    MutationOutcome,
    NotFound,
    Overloaded,
    PipelineOpcode,
    PipelineOutcome,
    PipelineRequest,
    TransportError,
)
from glifistore.client import _enrich, _retryability_for, build_ssl_context  # noqa: E402
from glifistore.protocol import (  # noqa: E402
    Opcode,
    Status,
    WorkerRouting,
    decode_request,
    encode_init_identity,
    encode_response,
    worker_for,
)


class FakeServer:
    """Minimal protocol-v2 server with one bound connection per Worker."""

    def __init__(
        self,
        *,
        worker_count: int = 1,
        disconnect_on_put: bool = False,
        internal_error_on_put: bool = False,
        stall_on_get: bool = False,
        stall_on_put: bool = False,
        stall_on_put_workers: frozenset[int] | set[int] | None = None,
        drop_on_backup: bool = False,
        internal_error_on_backup: bool = False,
        wrong_request_id_on_backup: bool = False,
        fail_rebind_workers: frozenset[int] | set[int] | None = None,
        ssl_context: ssl.SSLContext | None = None,
        routing: WorkerRouting | None = None,
    ) -> None:
        self._worker_count = worker_count
        self._disconnect_on_put = disconnect_on_put
        self._internal_error_on_put = internal_error_on_put
        self._stall_on_get = stall_on_get
        self._stall_on_put = stall_on_put
        self._stall_on_put_workers = set(stall_on_put_workers or ())
        self._drop_on_backup = drop_on_backup
        self._internal_error_on_backup = internal_error_on_backup
        self._wrong_request_id_on_backup = wrong_request_id_on_backup
        self._fail_rebind_workers = set(fail_rebind_workers or ())
        self._bind_counts: dict[int, int] = {}
        self._bind_counts_lock = threading.Lock()
        self._backup_requests = 0
        self._backup_lock = threading.Lock()
        self._ssl_context = ssl_context
        self._routing = routing or WorkerRouting()
        self._values: dict[bytes, bytes] = {}
        self._values_lock = threading.Lock()
        self._listener = socket.socket()
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(max(worker_count * 4, 8))
        self.port = self._listener.getsockname()[1]
        self._threads: list[threading.Thread] = []
        self._stop = threading.Event()
        self._acceptor = threading.Thread(target=self._accept, daemon=True)
        self._acceptor.start()

    @property
    def backup_requests(self) -> int:
        with self._backup_lock:
            return self._backup_requests

    @staticmethod
    def _receive_exact(connection: socket.socket, size: int) -> bytes:
        output = bytearray()
        while len(output) < size:
            chunk = connection.recv(size - len(output))
            if not chunk:
                raise EOFError
            output.extend(chunk)
        return bytes(output)

    def _receive(self, connection: socket.socket):
        prefix = self._receive_exact(connection, 4)
        size = int.from_bytes(prefix, "little")
        return decode_request(prefix + self._receive_exact(connection, size - 4))

    def _send(
        self,
        connection: socket.socket,
        status: Status,
        request_id: int,
        *,
        owner_worker: int,
        value: bytes = b"",
    ) -> None:
        connection.sendall(
            encode_response(
                status,
                request_id,
                value=value,
                owner_worker=owner_worker,
                worker_count=self._worker_count,
                routing_epoch=9,
            )
        )

    def _accept(self) -> None:
        self._listener.settimeout(0.2)
        try:
            while not self._stop.is_set():
                try:
                    connection, _ = self._listener.accept()
                except TimeoutError:
                    continue
                except OSError:
                    break
                if self._ssl_context is not None:
                    try:
                        connection = self._ssl_context.wrap_socket(connection, server_side=True)
                    except ssl.SSLError:
                        connection.close()
                        continue
                thread = threading.Thread(target=self._serve, args=(connection,), daemon=True)
                self._threads.append(thread)
                thread.start()
        finally:
            self._listener.close()

    def stop(self) -> None:
        self._stop.set()
        try:
            socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
        except OSError:
            pass

    def _serve(self, connection: socket.socket) -> None:
        bound_worker: int | None = None
        try:
            with connection:
                while True:
                    request = self._receive(connection)
                    if request.opcode is Opcode.INIT:
                        self._send(
                            connection,
                            Status.OK,
                            request.request_id,
                            owner_worker=0,
                            value=encode_init_identity(self._routing),
                        )
                    elif request.opcode is Opcode.HEALTH:
                        # Wire v2: HEALTH/READY/STATS are accepted unbound (docs/spec/wire-protocol-v2.md).
                        self._send(
                            connection,
                            Status.OK,
                            request.request_id,
                            owner_worker=bound_worker if bound_worker is not None else 0,
                            value=b"GlifiStore/live",
                        )
                    elif request.opcode is Opcode.READY:
                        self._send(
                            connection,
                            Status.OK,
                            request.request_id,
                            owner_worker=bound_worker if bound_worker is not None else 0,
                            value=b"GlifiStore/ready",
                        )
                    elif request.opcode is Opcode.STATS:
                        self._send(
                            connection,
                            Status.OK,
                            request.request_id,
                            owner_worker=bound_worker if bound_worker is not None else 0,
                            value=b"GlifiStore/stats\n",
                        )
                    elif request.opcode is Opcode.BIND_WORKER:
                        if request.target_worker >= self._worker_count:
                            self._send(
                                connection,
                                Status.INVALID_REQUEST,
                                request.request_id,
                                owner_worker=0,
                            )
                            continue
                        with self._bind_counts_lock:
                            count = self._bind_counts.get(request.target_worker, 0) + 1
                            self._bind_counts[request.target_worker] = count
                        if request.target_worker in self._fail_rebind_workers and count > 1:
                            # Close without BIND OK so ensure_connected fails on rebind.
                            return
                        bound_worker = request.target_worker
                        self._send(
                            connection,
                            Status.OK,
                            request.request_id,
                            owner_worker=bound_worker,
                        )
                    elif bound_worker is None:
                        self._send(
                            connection,
                            Status.NOT_BOUND,
                            request.request_id,
                            owner_worker=0,
                        )
                    elif request.opcode is Opcode.PING:
                        self._send(
                            connection,
                            Status.OK,
                            request.request_id,
                            owner_worker=bound_worker,
                            value=request.value,
                        )
                    elif request.opcode is Opcode.BACKUP:
                        with self._backup_lock:
                            self._backup_requests += 1
                        if self._drop_on_backup:
                            return
                        if self._internal_error_on_backup:
                            self._send(
                                connection,
                                Status.INTERNAL_ERROR,
                                request.request_id,
                                owner_worker=bound_worker,
                                value=b"report failed",
                            )
                            continue
                        reply_id = (
                            request.request_id ^ 1
                            if self._wrong_request_id_on_backup
                            else request.request_id
                        )
                        self._send(
                            connection,
                            Status.OK,
                            reply_id,
                            owner_worker=bound_worker,
                            value=b"status=ok files=0 bytes=0",
                        )
                    elif request.opcode is Opcode.PUT:
                        owner = worker_for(request.key, self._worker_count, self._routing)
                        if bound_worker != owner:
                            self._send(
                                connection,
                                Status.WRONG_OWNER,
                                request.request_id,
                                owner_worker=owner,
                            )
                            continue
                        with self._values_lock:
                            self._values[request.key] = request.value
                        if self._disconnect_on_put:
                            return
                        if self._internal_error_on_put:
                            self._send(
                                connection,
                                Status.INTERNAL_ERROR,
                                request.request_id,
                                owner_worker=bound_worker,
                            )
                            continue
                        if self._stall_on_put or bound_worker in self._stall_on_put_workers:
                            # Hold without a response so clients can cancel mid-receive.
                            self._stop.wait(timeout=3600)
                            return
                        self._send(
                            connection,
                            Status.OK,
                            request.request_id,
                            owner_worker=bound_worker,
                        )
                    elif request.opcode is Opcode.GET:
                        if self._stall_on_get:
                            self._stop.wait(timeout=3600)
                            return
                        owner = worker_for(request.key, self._worker_count, self._routing)
                        if bound_worker != owner:
                            self._send(
                                connection,
                                Status.WRONG_OWNER,
                                request.request_id,
                                owner_worker=owner,
                            )
                            continue
                        with self._values_lock:
                            value = self._values.get(request.key)
                        if value is None:
                            self._send(
                                connection,
                                Status.NOT_FOUND,
                                request.request_id,
                                owner_worker=bound_worker,
                            )
                        else:
                            self._send(
                                connection,
                                Status.OK,
                                request.request_id,
                                owner_worker=bound_worker,
                                value=value,
                            )
                    elif request.opcode is Opcode.ERASE:
                        owner = worker_for(request.key, self._worker_count, self._routing)
                        if bound_worker != owner:
                            self._send(
                                connection,
                                Status.WRONG_OWNER,
                                request.request_id,
                                owner_worker=owner,
                            )
                            continue
                        with self._values_lock:
                            found = self._values.pop(request.key, None) is not None
                        self._send(
                            connection,
                            Status.OK if found else Status.NOT_FOUND,
                            request.request_id,
                            owner_worker=bound_worker,
                        )
        except (EOFError, OSError, ValueError):
            pass

    def join(self) -> None:
        self.stop()
        self._acceptor.join(timeout=2)
        for thread in self._threads:
            thread.join(timeout=2)


class ClientTests(unittest.TestCase):
    def test_binary_put_get_ping_and_erase(self) -> None:
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port)) as client:
            self.assertEqual(client.worker_count, 1)
            self.assertEqual(client.routing_epoch, 9)
            self.assertEqual(client.worker_for(b"binary\x00key"), 0)
            self.assertTrue(client.put(b"binary\x00key", b"value\x00\xff").committed)
            self.assertEqual(client.get(b"binary\x00key"), b"value\x00\xff")
            self.assertEqual(client.ping(b"hello"), b"hello")
            report = client.backup("/tmp/glifistore-sdk-backup")
            self.assertIn(b"status=ok", report)
            with self.assertRaises(InvalidArgument):
                client.backup("")
            rejected = client.put(b"bad-expiry", b"value", expire_at_ns=-1)
            self.assertEqual(rejected.outcome, MutationOutcome.REJECTED)
            self.assertTrue(client.erase(b"binary\x00key").committed)
            with self.assertRaises(NotFound) as raised:
                client.get(b"binary\x00key")
            missing = raised.exception
            self.assertEqual(missing.category, "not_found")
            self.assertEqual(missing.wire_status, int(Status.NOT_FOUND))
            self.assertEqual(missing.retryability, "new_attempt")
        server.join()

    def test_erase_absent_is_rejected(self) -> None:
        # client-semantics §3: wire NOT_FOUND on standalone ERASE → rejected.
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port)) as client:
            result = client.erase(b"missing-key")
            self.assertEqual(result.outcome, MutationOutcome.REJECTED)
            self.assertIsInstance(result.error, NotFound)
            assert result.error is not None
            self.assertEqual(result.error.category, "not_found")
            self.assertEqual(result.error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(result.error.retryability, "new_attempt")
        server.join()

    def test_pipeline_erase_absent_is_failed_rejected(self) -> None:
        # client-semantics §3: wire NOT_FOUND on pipeline ERASE → failed + rejected.
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port)) as client:
            responses = client.execute_pipeline(
                [PipelineRequest(PipelineOpcode.ERASE, b"missing-key")]
            )
            self.assertEqual(len(responses), 1)
            self.assertEqual(responses[0].outcome, PipelineOutcome.FAILED)
            assert responses[0].error is not None
            self.assertIsInstance(responses[0].error, NotFound)
            self.assertEqual(responses[0].error.category, "not_found")
            self.assertEqual(responses[0].error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(responses[0].error.retryability, "new_attempt")
            self.assertEqual(responses[0].error.wire_status, int(Status.NOT_FOUND))
        server.join()

    def test_batch_erase_absent_is_failed_rejected(self) -> None:
        # client-semantics §3/§5: execute_batch is per-Worker pipeline grouping; absent ERASE
        # still maps NOT_FOUND → failed + rejected.
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port)) as client:
            responses = client.execute_batch(
                [PipelineRequest(PipelineOpcode.ERASE, b"missing-key")]
            )
            self.assertEqual(len(responses), 1)
            self.assertEqual(responses[0].outcome, PipelineOutcome.FAILED)
            assert responses[0].error is not None
            self.assertIsInstance(responses[0].error, NotFound)
            self.assertEqual(responses[0].error.category, "not_found")
            self.assertEqual(responses[0].error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(responses[0].error.retryability, "new_attempt")
            self.assertEqual(responses[0].error.wire_status, int(Status.NOT_FOUND))
        server.join()

    def test_worker_pipelines_erase_absent_is_failed_rejected(self) -> None:
        # client-semantics §3/§5: pre-sharded worker pipelines inherit pipeline ERASE mapping.
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port)) as client:
            results = client.execute_worker_pipelines(
                [[PipelineRequest(PipelineOpcode.ERASE, b"missing-key")]]
            )
            self.assertEqual(len(results), 1)
            self.assertEqual(len(results[0]), 1)
            self.assertEqual(results[0][0].outcome, PipelineOutcome.FAILED)
            assert results[0][0].error is not None
            self.assertIsInstance(results[0][0].error, NotFound)
            self.assertEqual(results[0][0].error.category, "not_found")
            self.assertEqual(results[0][0].error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(results[0][0].error.retryability, "new_attempt")
            self.assertEqual(results[0][0].error.wire_status, int(Status.NOT_FOUND))
        server.join()

    def test_backup_does_not_blind_retry_after_bytes_sent(self) -> None:
        # Online BACKUP is not idempotent to the same destination. A lost OK must
        # surface indeterminate / reconcile_first — not a second BACKUP attempt.
        server = FakeServer(drop_on_backup=True)
        with Client.connect(ClientConfig(port=server.port, request_timeout=0.2)) as client:
            with self.assertRaises(TransportError) as raised:
                client.backup("/tmp/glifistore-sdk-backup-drop")
            error = raised.exception
            self.assertGreater(error.bytes_sent, 0)
            self.assertEqual(error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(error.retryability, "reconcile_first")
            self.assertEqual(server.backup_requests, 1)
        server.join()

    def test_backup_internal_error_is_reconcile_first(self) -> None:
        # Wire INTERNAL_ERROR after a possible committed fenced copy must not
        # advertise new_attempt (same-destination retry would look like failure).
        from glifistore import InternalError

        server = FakeServer(internal_error_on_backup=True)
        with Client.connect(ClientConfig(port=server.port)) as client:
            with self.assertRaises(InternalError) as raised:
                client.backup("/tmp/glifistore-sdk-backup-internal")
            error = raised.exception
            self.assertEqual(error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(error.retryability, "reconcile_first")
            self.assertEqual(error.wire_status, int(Status.INTERNAL_ERROR))
            self.assertEqual(server.backup_requests, 1)
        server.join()

    def test_backup_validate_failure_is_reconcile_first(self) -> None:
        # Framed BACKUP response with mismatched request_id — fenced copy may exist.
        from glifistore import ProtocolError

        server = FakeServer(wrong_request_id_on_backup=True)
        with Client.connect(ClientConfig(port=server.port)) as client:
            with self.assertRaises(ProtocolError) as raised:
                client.backup("/tmp/glifistore-sdk-backup-wrong-id")
            error = raised.exception
            self.assertEqual(error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(error.retryability, "reconcile_first")
            self.assertGreater(error.bytes_sent, 0)
            self.assertEqual(server.backup_requests, 1)
        server.join()

    def test_internal_error_mutation_is_indeterminate(self) -> None:
        server = FakeServer(internal_error_on_put=True)
        with Client.connect(ClientConfig(port=server.port)) as client:
            result = client.put(b"key", b"value")
            self.assertEqual(result.outcome, MutationOutcome.INDETERMINATE)
            self.assertIsNotNone(result.error)
            assert result.error is not None
            self.assertEqual(result.error.category, "internal")
            self.assertEqual(result.error.wire_status, int(Status.INTERNAL_ERROR))
            self.assertEqual(result.error.operation, "put")
            self.assertEqual(result.error.retryability, "reconcile_first")
            self.assertEqual(result.error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertGreater(result.error.bytes_sent, 0)
        server.join()

    def test_overloaded_retryability_is_never(self) -> None:
        self.assertEqual(_retryability_for("overloaded", False, False), "never")
        self.assertEqual(_retryability_for("overloaded", True, False), "never")
        error = Overloaded("server is overloaded")
        self.assertEqual(error.category, "overloaded")
        self.assertEqual(error.retryability, "never")

    def test_close_does_not_deadlock_against_inflight_read(self) -> None:
        server = FakeServer()
        client = Client.connect(ClientConfig(port=server.port, request_timeout=1.0))
        started = threading.Event()
        finished = threading.Event()

        def reader() -> None:
            started.set()
            try:
                client.get(b"missing")
            except Exception:
                pass
            finished.set()

        thread = threading.Thread(target=reader)
        thread.start()
        self.assertTrue(started.wait(1.0))
        client.close()
        self.assertTrue(finished.wait(2.0))
        thread.join(timeout=2.0)
        self.assertFalse(client.healthy)
        server.join()

    def test_multi_worker_bootstrap_and_routing(self) -> None:
        server = FakeServer(worker_count=2)
        with Client.connect(ClientConfig(port=server.port)) as client:
            self.assertEqual(client.worker_count, 2)
            keys = [f"mw-{index}".encode() for index in range(32)]
            owners = {key: client.worker_for(key) for key in keys}
            self.assertEqual(set(owners.values()), {0, 1})
            for key in keys:
                self.assertTrue(client.put(key, key[::-1]).committed)
            for key in keys:
                self.assertEqual(client.get(key), key[::-1])
                self.assertEqual(worker_for(key, 2), owners[key])
        server.join()

    def test_health_ready_stats_and_routing(self) -> None:
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port)) as client:
            self.assertTrue(client.healthy)
            self.assertIsInstance(client.routing, WorkerRouting)
            self.assertEqual(client.health(), b"GlifiStore/live")
            self.assertEqual(client.ready(), b"GlifiStore/ready")
            stats = client.stats()
            self.assertTrue(stats.startswith(b"GlifiStore/stats"))
        server.join()

    def test_unix_socket_refuses_tls(self) -> None:
        with self.assertRaises(InvalidArgument) as raised:
            Client.connect(
                ClientConfig(
                    unix_socket_path="/tmp/glifistore-py-uds-tls-refuse.sock",
                    tls=True,
                )
            )
        self.assertIn("AF_UNIX", str(raised.exception))

    def test_keyed_siphash_init_and_routing(self) -> None:
        routing = WorkerRouting(algorithm=2, seed=0x1111_2222_3333_4444)
        server = FakeServer(worker_count=8, routing=routing)
        with Client.connect(ClientConfig(port=server.port)) as client:
            self.assertTrue(client.routing.keyed)
            self.assertEqual(client.routing.seed, routing.seed)
            key = b"tenant-a/orders/1"
            self.assertEqual(client.worker_for(key), 6)
            self.assertTrue(client.put(key, b"v").committed)
            self.assertEqual(client.get(key), b"v")
        server.join()

    def test_disconnect_after_mutation_is_indeterminate(self) -> None:
        server = FakeServer(disconnect_on_put=True)
        with Client.connect(ClientConfig(port=server.port)) as client:
            result = client.put(b"key", b"value")
            self.assertEqual(result.outcome, MutationOutcome.INDETERMINATE)
            self.assertIsNotNone(result.error)
            assert result.error is not None
            self.assertGreater(result.error.bytes_sent, 0)
            self.assertEqual(result.error.retryability, "reconcile_first")
            self.assertEqual(result.error.mutation_outcome, MutationOutcome.INDETERMINATE)
        server.join()

    def test_indeterminate_enrich_ignores_zero_bytes_sent(self) -> None:
        # Receive-after-send paths historically omitted bytes_sent; transport+0 must not
        # advertise same_request when mutation_outcome is already indeterminate.
        error = _enrich(
            TransportError("socket closed"),
            mutation_outcome=MutationOutcome.INDETERMINATE,
        )
        self.assertEqual(error.bytes_sent, 0)
        self.assertEqual(error.retryability, "reconcile_first")
        self.assertEqual(error.mutation_outcome, MutationOutcome.INDETERMINATE)

    def test_pipeline_preserves_order_and_owned_values(self) -> None:
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port)) as client:
            requests: list[PipelineRequest] = []
            expected: list[bytes] = []
            for index in range(64):
                value = f"pipeline-{index}".encode()
                expected.append(value)
                requests.append(PipelineRequest(PipelineOpcode.PUT, b"key", value))
                requests.append(PipelineRequest(PipelineOpcode.GET, b"key"))
            responses = client.execute_pipeline(requests)
            self.assertEqual(len(responses), len(requests))
            for index, value in enumerate(expected):
                self.assertTrue(responses[index * 2].succeeded)
                self.assertTrue(responses[index * 2 + 1].succeeded)
                self.assertEqual(responses[index * 2 + 1].value, value)
        server.join()

    def test_pipeline_disconnect_classifies_each_request(self) -> None:
        server = FakeServer(disconnect_on_put=True)
        with Client.connect(ClientConfig(port=server.port)) as client:
            responses = client.execute_pipeline(
                [
                    PipelineRequest(PipelineOpcode.PUT, b"key", b"value"),
                    PipelineRequest(PipelineOpcode.GET, b"key"),
                    PipelineRequest(PipelineOpcode.ERASE, b"key"),
                ]
            )
            self.assertEqual(responses[0].outcome, PipelineOutcome.INDETERMINATE)
            self.assertEqual(responses[1].outcome, PipelineOutcome.FAILED)
            self.assertEqual(responses[2].outcome, PipelineOutcome.INDETERMINATE)
            assert responses[0].error is not None
            self.assertGreater(responses[0].error.bytes_sent, 0)
            self.assertEqual(responses[0].error.retryability, "reconcile_first")
            self.assertEqual(responses[0].error.mutation_outcome, MutationOutcome.INDETERMINATE)
            assert responses[2].error is not None
            self.assertEqual(responses[2].error.retryability, "reconcile_first")
            self.assertEqual(responses[2].error.mutation_outcome, MutationOutcome.INDETERMINATE)
        server.join()

    def test_pipeline_internal_error_mutation_is_reconcile_first(self) -> None:
        server = FakeServer(internal_error_on_put=True)
        with Client.connect(ClientConfig(port=server.port)) as client:
            responses = client.execute_pipeline(
                [PipelineRequest(PipelineOpcode.PUT, b"key", b"value")]
            )
            self.assertEqual(responses[0].outcome, PipelineOutcome.INDETERMINATE)
            assert responses[0].error is not None
            self.assertEqual(responses[0].error.retryability, "reconcile_first")
            self.assertEqual(responses[0].error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(responses[0].error.wire_status, int(Status.INTERNAL_ERROR))
            self.assertGreater(responses[0].error.bytes_sent, 0)
        server.join()

    def test_pipeline_limits_fail_before_network_transmission(self) -> None:
        server = FakeServer()
        with Client.connect(ClientConfig(port=server.port, maximum_pipeline_requests=1)) as client:
            with self.assertRaises(InvalidArgument):
                client.execute_pipeline(
                    [
                        PipelineRequest(PipelineOpcode.GET, b"key"),
                        PipelineRequest(PipelineOpcode.GET, b"key"),
                    ]
                )
        server.join()

    def test_per_call_timeout_overrides_config(self) -> None:
        server = FakeServer(stall_on_get=True)
        with Client.connect(ClientConfig(port=server.port, request_timeout=5.0)) as client:
            with self.assertRaises(TransportError):
                client.get(b"key", timeout=0.05)
            with self.assertRaises(InvalidArgument):
                client.get(b"key", timeout=0)
        server.join()

    def test_batch_groups_workers_and_restores_order(self) -> None:
        server = FakeServer(worker_count=2)
        with Client.connect(ClientConfig(port=server.port)) as client:
            keys = [f"batch-{index}".encode() for index in range(64)]
            owners = {key: client.worker_for(key) for key in keys}
            self.assertEqual(set(owners.values()), {0, 1})
            requests = [PipelineRequest(PipelineOpcode.PUT, key, key[::-1]) for key in keys] + [
                PipelineRequest(PipelineOpcode.GET, key) for key in keys
            ]
            with mock.patch.object(client, "worker_for", wraps=client.worker_for) as routed:
                responses = client.execute_batch(requests)
            self.assertEqual(routed.call_count, len(requests))
            self.assertEqual(len(responses), len(requests))
            for response in responses:
                self.assertTrue(response.succeeded)
            for index, key in enumerate(keys):
                self.assertEqual(responses[len(keys) + index].value, key[::-1])
        server.join()

    def test_batch_freezes_mutable_key_with_routing_decision(self) -> None:
        server = FakeServer(worker_count=2)
        with Client.connect(ClientConfig(port=server.port)) as client:
            keys = [f"freeze-{index:04d}".encode() for index in range(128)]
            key0 = next(key for key in keys if client.worker_for(key) == 0)
            key1 = next(key for key in keys if client.worker_for(key) == 1)
            mutable_key = bytearray(key0)
            owner_for = client.worker_for

            def route_then_mutate(frozen_key: bytes) -> int:
                owner = owner_for(frozen_key)
                mutable_key[:] = key1
                return owner

            with mock.patch.object(client, "worker_for", side_effect=route_then_mutate):
                responses = client.execute_batch(
                    [PipelineRequest(PipelineOpcode.PUT, mutable_key, b"frozen")]
                )
            self.assertTrue(responses[0].succeeded)
            self.assertEqual(client.get(key0), b"frozen")
        server.join()

    def test_batch_preserves_sibling_results_when_one_worker_fails(self) -> None:
        # Non-atomic batch: Worker A success must survive Worker B admission failure.
        server = FakeServer(worker_count=2, fail_rebind_workers={1})
        with Client.connect(ClientConfig(port=server.port)) as client:
            keys = [None, None]
            candidate = 0
            while any(key is None for key in keys):
                key = f"sib-{candidate}".encode()
                worker = client.worker_for(key)
                if keys[worker] is None:
                    keys[worker] = key
                candidate += 1
            assert keys[0] is not None and keys[1] is not None
            client._connections[1].reset()
            responses = client.execute_batch(
                [
                    PipelineRequest(PipelineOpcode.PUT, keys[0], b"a"),
                    PipelineRequest(PipelineOpcode.PUT, keys[1], b"b"),
                ]
            )
            self.assertEqual(len(responses), 2)
            self.assertTrue(responses[0].succeeded)
            self.assertEqual(responses[1].outcome, PipelineOutcome.FAILED)
            self.assertIsNotNone(responses[1].error)
            self.assertEqual(responses[1].error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(responses[1].error.bytes_sent, 0)
            self.assertEqual(client.get(keys[0]), b"a")
        server.join()

    def test_worker_pipelines_pre_sharded_fanout(self) -> None:
        server = FakeServer(worker_count=2)
        with Client.connect(ClientConfig(port=server.port)) as client:
            keys = [None, None]
            candidate = 0
            while any(key is None for key in keys):
                key = f"wp-{candidate}".encode()
                worker = client.worker_for(key)
                if keys[worker] is None:
                    keys[worker] = key
                candidate += 1
            assert keys[0] is not None and keys[1] is not None
            wave = [
                [
                    PipelineRequest(PipelineOpcode.PUT, keys[0], b"a"),
                    PipelineRequest(PipelineOpcode.GET, keys[0]),
                ],
                [
                    PipelineRequest(PipelineOpcode.PUT, keys[1], b"b"),
                    PipelineRequest(PipelineOpcode.GET, keys[1]),
                ],
            ]
            results = client.execute_worker_pipelines(wave)
            self.assertEqual(len(results), 2)
            self.assertTrue(results[0][0].succeeded)
            self.assertEqual(results[0][1].value, b"a")
            self.assertTrue(results[1][0].succeeded)
            self.assertEqual(results[1][1].value, b"b")
            sparse = client.execute_worker_pipelines([[], [PipelineRequest(PipelineOpcode.GET, keys[1])]])
            self.assertEqual(sparse[0], [])
            self.assertTrue(sparse[1][0].succeeded)
            with self.assertRaises(InvalidArgument):
                client.execute_worker_pipelines([[PipelineRequest(PipelineOpcode.GET, keys[1])], []])
            with self.assertRaises(InvalidArgument):
                client.execute_worker_pipelines([[PipelineRequest(PipelineOpcode.GET, keys[0])]])
        server.join()

    def test_batch_per_worker_pipeline_limit(self) -> None:
        server = FakeServer(worker_count=2)
        with Client.connect(ClientConfig(port=server.port, maximum_pipeline_requests=1)) as limited:
            key0 = next(
                key
                for key in (f"batch-limit-{index}".encode() for index in range(64))
                if limited.worker_for(key) == 0
            )
            with self.assertRaises(InvalidArgument):
                limited.execute_batch(
                    [
                        PipelineRequest(PipelineOpcode.GET, key0),
                        PipelineRequest(PipelineOpcode.GET, key0),
                    ]
                )
        server.join()


class AsyncClientTests(unittest.IsolatedAsyncioTestCase):
    async def test_async_put_get_pipeline_and_close(self) -> None:
        server = FakeServer()
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            self.assertEqual(client.worker_count, 1)
            self.assertTrue((await client.put(b"async\x00key", b"value\xff")).committed)
            self.assertEqual(await client.get(b"async\x00key"), b"value\xff")
            self.assertEqual(await client.ping(b"ping"), b"ping")
            responses = await client.execute_pipeline(
                [
                    PipelineRequest(PipelineOpcode.PUT, b"async\x00key", b"next"),
                    PipelineRequest(PipelineOpcode.GET, b"async\x00key"),
                ]
            )
            self.assertTrue(responses[0].succeeded)
            self.assertEqual(responses[1].value, b"next")
        server.join()

    async def test_async_erase_absent_is_rejected(self) -> None:
        # client-semantics §3: wire NOT_FOUND on standalone ERASE → rejected.
        server = FakeServer()
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            result = await client.erase(b"missing-key")
            self.assertEqual(result.outcome, MutationOutcome.REJECTED)
            self.assertIsInstance(result.error, NotFound)
            assert result.error is not None
            self.assertEqual(result.error.category, "not_found")
            self.assertEqual(result.error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(result.error.retryability, "new_attempt")
        server.join()

    async def test_async_pipeline_erase_absent_is_failed_rejected(self) -> None:
        # client-semantics §3: wire NOT_FOUND on pipeline ERASE → failed + rejected.
        server = FakeServer()
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            responses = await client.execute_pipeline(
                [PipelineRequest(PipelineOpcode.ERASE, b"missing-key")]
            )
            self.assertEqual(len(responses), 1)
            self.assertEqual(responses[0].outcome, PipelineOutcome.FAILED)
            assert responses[0].error is not None
            self.assertIsInstance(responses[0].error, NotFound)
            self.assertEqual(responses[0].error.category, "not_found")
            self.assertEqual(responses[0].error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(responses[0].error.retryability, "new_attempt")
            self.assertEqual(responses[0].error.wire_status, int(Status.NOT_FOUND))
        server.join()

    async def test_async_batch_erase_absent_is_failed_rejected(self) -> None:
        # client-semantics §3/§5: execute_batch absent ERASE → failed + rejected.
        server = FakeServer()
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            responses = await client.execute_batch(
                [PipelineRequest(PipelineOpcode.ERASE, b"missing-key")]
            )
            self.assertEqual(len(responses), 1)
            self.assertEqual(responses[0].outcome, PipelineOutcome.FAILED)
            assert responses[0].error is not None
            self.assertIsInstance(responses[0].error, NotFound)
            self.assertEqual(responses[0].error.category, "not_found")
            self.assertEqual(responses[0].error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(responses[0].error.retryability, "new_attempt")
            self.assertEqual(responses[0].error.wire_status, int(Status.NOT_FOUND))
        server.join()

    async def test_async_worker_pipelines_erase_absent_is_failed_rejected(self) -> None:
        # client-semantics §3/§5: pre-sharded worker pipelines inherit pipeline ERASE mapping.
        server = FakeServer()
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            results = await client.execute_worker_pipelines(
                [[PipelineRequest(PipelineOpcode.ERASE, b"missing-key")]]
            )
            self.assertEqual(len(results), 1)
            self.assertEqual(len(results[0]), 1)
            self.assertEqual(results[0][0].outcome, PipelineOutcome.FAILED)
            assert results[0][0].error is not None
            self.assertIsInstance(results[0][0].error, NotFound)
            self.assertEqual(results[0][0].error.category, "not_found")
            self.assertEqual(results[0][0].error.mutation_outcome, MutationOutcome.REJECTED)
            self.assertEqual(results[0][0].error.retryability, "new_attempt")
            self.assertEqual(results[0][0].error.wire_status, int(Status.NOT_FOUND))
        server.join()

    async def test_async_multi_worker_concurrent_pipelines(self) -> None:
        server = FakeServer(worker_count=2)
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            self.assertEqual(client.worker_count, 2)

            async def round_trip(key: bytes) -> None:
                value = key[::-1]
                result = await client.put(key, value)
                self.assertTrue(result.committed)
                self.assertEqual(await client.get(key), value)

            await asyncio.gather(*(round_trip(f"a{i}".encode()) for i in range(20)))
        server.join()

    async def test_async_batch_groups_workers_and_restores_order(self) -> None:
        server = FakeServer(worker_count=2)
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            keys = [f"abatch-{index}".encode() for index in range(32)]
            self.assertEqual({client.worker_for(key) for key in keys}, {0, 1})
            requests = [PipelineRequest(PipelineOpcode.PUT, key, key[::-1]) for key in keys] + [
                PipelineRequest(PipelineOpcode.GET, key) for key in keys
            ]
            with mock.patch.object(client, "worker_for", wraps=client.worker_for) as routed:
                responses = await client.execute_batch(requests)
            self.assertEqual(routed.call_count, len(requests))
            self.assertEqual(len(responses), len(requests))
            for response in responses:
                self.assertTrue(response.succeeded)
            for index, key in enumerate(keys):
                self.assertEqual(responses[len(keys) + index].value, key[::-1])
        server.join()

    async def test_async_worker_pipelines_pre_sharded_fanout(self) -> None:
        server = FakeServer(worker_count=2)
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            keys: list[bytes | None] = [None, None]
            candidate = 0
            while any(key is None for key in keys):
                key = f"awp-{candidate}".encode()
                worker = client.worker_for(key)
                if keys[worker] is None:
                    keys[worker] = key
                candidate += 1
            assert keys[0] is not None and keys[1] is not None
            wave = [
                [
                    PipelineRequest(PipelineOpcode.PUT, keys[0], b"a"),
                    PipelineRequest(PipelineOpcode.GET, keys[0]),
                ],
                [
                    PipelineRequest(PipelineOpcode.PUT, keys[1], b"b"),
                    PipelineRequest(PipelineOpcode.GET, keys[1]),
                ],
            ]
            results = await client.execute_worker_pipelines(wave)
            self.assertEqual(len(results), 2)
            self.assertTrue(results[0][0].succeeded)
            self.assertEqual(results[0][1].value, b"a")
            self.assertTrue(results[1][0].succeeded)
            self.assertEqual(results[1][1].value, b"b")
            sparse = await client.execute_worker_pipelines(
                [[], [PipelineRequest(PipelineOpcode.GET, keys[1])]]
            )
            self.assertEqual(sparse[0], [])
            self.assertTrue(sparse[1][0].succeeded)
            with self.assertRaises(InvalidArgument):
                await client.execute_worker_pipelines(
                    [[PipelineRequest(PipelineOpcode.GET, keys[1])], []]
                )
            with self.assertRaises(InvalidArgument):
                await client.execute_worker_pipelines(
                    [[PipelineRequest(PipelineOpcode.GET, keys[0])]]
                )
        server.join()

    async def test_async_batch_freezes_mutable_key_with_routing_decision(self) -> None:
        server = FakeServer(worker_count=2)
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            keys = [f"afreeze-{index:04d}".encode() for index in range(128)]
            key0 = next(key for key in keys if client.worker_for(key) == 0)
            key1 = next(key for key in keys if client.worker_for(key) == 1)
            mutable_key = bytearray(key0)
            owner_for = client.worker_for

            def route_then_mutate(frozen_key: bytes) -> int:
                owner = owner_for(frozen_key)
                mutable_key[:] = key1
                return owner

            with mock.patch.object(client, "worker_for", side_effect=route_then_mutate):
                responses = await client.execute_batch(
                    [PipelineRequest(PipelineOpcode.PUT, mutable_key, b"frozen")]
                )
            self.assertTrue(responses[0].succeeded)
            self.assertEqual(await client.get(key0), b"frozen")
        server.join()

    async def test_async_batch_cancel_preserves_sibling_results(self) -> None:
        # Worker-0 PUT succeeds; Worker-1 stalls after accepting PUT. Cancelling the
        # batch task must return the slot vector (sibling committed + stalled
        # indeterminate), not bare CancelledError that discards siblings (§5 / §6.3).
        server = FakeServer(worker_count=2, stall_on_put_workers={1})
        async with await AsyncClient.connect(
            ClientConfig(port=server.port, request_timeout=5.0)
        ) as client:
            keys: list[bytes | None] = [None, None]
            candidate = 0
            while any(key is None for key in keys):
                key = f"acancel{candidate:08d}".encode()
                worker = client.worker_for(key)
                if keys[worker] is None:
                    keys[worker] = key
                candidate += 1
            assert keys[0] is not None and keys[1] is not None
            requests = [
                PipelineRequest(PipelineOpcode.PUT, keys[0], b"a"),
                PipelineRequest(PipelineOpcode.PUT, keys[1], b"b"),
            ]
            task = asyncio.create_task(client.execute_batch(requests))
            await asyncio.sleep(0.05)
            task.cancel()
            responses = await task
            self.assertEqual(len(responses), 2)
            self.assertTrue(responses[0].succeeded)
            self.assertEqual(responses[0].outcome, PipelineOutcome.SUCCEEDED)
            self.assertEqual(responses[1].outcome, PipelineOutcome.INDETERMINATE)
            self.assertIsNotNone(responses[1].error)
            assert responses[1].error is not None
            self.assertGreater(responses[1].error.bytes_sent, 0)
            self.assertEqual(responses[1].error.retryability, "reconcile_first")
            self.assertEqual(responses[1].error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(await client.get(keys[0]), b"a")
        server.join()

    async def test_async_cancel_classifies_despite_cancel_during_reset(self) -> None:
        # Post-send cancel must classify indeterminate before poison. If reset's
        # wait_closed (or a hostile reset) raises CancelledError, the app must still
        # get reconcile_first — not bare CancelledError / rejected (§6.3).
        from glifistore import async_client as ac_mod

        server = FakeServer(stall_on_put=True)
        async with await AsyncClient.connect(
            ClientConfig(port=server.port, request_timeout=5.0)
        ) as client:

            async def reset_raises_cancel(self: ac_mod._Connection) -> None:
                writer, self.writer = self.writer, None
                self.reader = None
                self.input.clear()
                self.input_offset = 0
                if writer is not None:
                    writer.close()
                raise asyncio.CancelledError()

            with mock.patch.object(ac_mod._Connection, "reset", reset_raises_cancel):
                task = asyncio.create_task(client.put(b"key", b"value"))
                await asyncio.sleep(0.05)
                task.cancel()
                result = await task
            self.assertEqual(result.outcome, MutationOutcome.INDETERMINATE)
            assert result.error is not None
            self.assertGreater(result.error.bytes_sent, 0)
            self.assertEqual(result.error.retryability, "reconcile_first")
            self.assertEqual(result.error.mutation_outcome, MutationOutcome.INDETERMINATE)
        server.join()

    async def test_async_pipeline_cancel_classifies_despite_cancel_during_reset(
        self,
    ) -> None:
        from glifistore import async_client as ac_mod

        server = FakeServer(stall_on_put=True)
        async with await AsyncClient.connect(
            ClientConfig(port=server.port, request_timeout=5.0)
        ) as client:

            async def reset_raises_cancel(self: ac_mod._Connection) -> None:
                writer, self.writer = self.writer, None
                self.reader = None
                self.input.clear()
                self.input_offset = 0
                if writer is not None:
                    writer.close()
                raise asyncio.CancelledError()

            with mock.patch.object(ac_mod._Connection, "reset", reset_raises_cancel):
                task = asyncio.create_task(
                    client.execute_pipeline([PipelineRequest(PipelineOpcode.PUT, b"key", b"value")])
                )
                await asyncio.sleep(0.05)
                task.cancel()
                responses = await task
            self.assertEqual(responses[0].outcome, PipelineOutcome.INDETERMINATE)
            assert responses[0].error is not None
            self.assertGreater(responses[0].error.bytes_sent, 0)
            self.assertEqual(responses[0].error.retryability, "reconcile_first")
            self.assertEqual(responses[0].error.mutation_outcome, MutationOutcome.INDETERMINATE)
        server.join()

    async def test_async_disconnect_after_mutation_is_indeterminate(self) -> None:
        server = FakeServer(disconnect_on_put=True)
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            result = await client.put(b"key", b"value")
            self.assertEqual(result.outcome, MutationOutcome.INDETERMINATE)
            self.assertIsNotNone(result.error)
            assert result.error is not None
            self.assertGreater(result.error.bytes_sent, 0)
            self.assertEqual(result.error.retryability, "reconcile_first")
            self.assertEqual(result.error.mutation_outcome, MutationOutcome.INDETERMINATE)
        server.join()

    async def test_async_backup_does_not_blind_retry_after_bytes_sent(self) -> None:
        # Online BACKUP is not idempotent to the same destination. Match sync /
        # C++: lost OK after send → indeterminate / reconcile_first, one attempt.
        server = FakeServer(drop_on_backup=True)
        async with await AsyncClient.connect(
            ClientConfig(port=server.port, request_timeout=0.2)
        ) as client:
            with self.assertRaises(TransportError) as raised:
                await client.backup("/tmp/glifistore-sdk-async-backup-drop")
            error = raised.exception
            self.assertGreater(error.bytes_sent, 0)
            self.assertEqual(error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(error.retryability, "reconcile_first")
            self.assertEqual(server.backup_requests, 1)
        server.join()

    async def test_async_backup_internal_error_is_reconcile_first(self) -> None:
        from glifistore import InternalError

        server = FakeServer(internal_error_on_backup=True)
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            with self.assertRaises(InternalError) as raised:
                await client.backup("/tmp/glifistore-sdk-async-backup-internal")
            error = raised.exception
            self.assertEqual(error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(error.retryability, "reconcile_first")
            self.assertEqual(error.wire_status, int(Status.INTERNAL_ERROR))
            self.assertEqual(server.backup_requests, 1)
        server.join()

    async def test_async_internal_error_mutation_is_indeterminate(self) -> None:
        server = FakeServer(internal_error_on_put=True)
        async with await AsyncClient.connect(ClientConfig(port=server.port)) as client:
            result = await client.put(b"key", b"value")
            self.assertEqual(result.outcome, MutationOutcome.INDETERMINATE)
            assert result.error is not None
            self.assertEqual(result.error.retryability, "reconcile_first")
            self.assertEqual(result.error.mutation_outcome, MutationOutcome.INDETERMINATE)
            self.assertEqual(result.error.wire_status, int(Status.INTERNAL_ERROR))
            self.assertGreater(result.error.bytes_sent, 0)
        server.join()


class ClientTlsTests(unittest.TestCase):
    def test_tls_config_requires_cert_and_key_pair(self) -> None:
        with self.assertRaises(InvalidArgument):
            Client.connect(ClientConfig(port=1, tls=True, cert_file="only-cert.pem"))

    def test_build_ssl_context_requires_tls_1_3(self) -> None:
        context = build_ssl_context(ClientConfig(tls=True, insecure_skip_verify=True))
        self.assertEqual(context.minimum_version, ssl.TLSVersion.TLSv1_3)
        self.assertEqual(context.maximum_version, ssl.TLSVersion.TLSv1_3)
        self.assertEqual(context.verify_mode, ssl.CERT_NONE)

    def test_sync_and_async_tls_ping(self) -> None:
        material = _self_signed_material()
        if material is None:
            self.skipTest("openssl CLI unavailable for ephemeral certs")
        cert_path, key_path = material
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.minimum_version = ssl.TLSVersion.TLSv1_3
        server_context.load_cert_chain(certfile=cert_path, keyfile=key_path)
        server = FakeServer(ssl_context=server_context)
        config = ClientConfig(
            port=server.port,
            tls=True,
            tls_ca=cert_path,
            server_name="localhost",
        )
        with Client.connect(config) as client:
            self.assertEqual(client.ping(b"tls-ping"), b"tls-ping")

        async def _async_ping() -> None:
            async with await AsyncClient.connect(config) as async_client:
                self.assertEqual(await async_client.ping(b"tls-async"), b"tls-async")

        asyncio.run(_async_ping())
        server.join()


def _self_signed_material() -> tuple[str, str] | None:
    directory = tempfile.mkdtemp(prefix="glifistore-py-tls-")
    cert_path = str(Path(directory) / "server.crt")
    key_path = str(Path(directory) / "server.key")
    command = [
        "openssl",
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-keyout",
        key_path,
        "-out",
        cert_path,
        "-days",
        "1",
        "-subj",
        "/CN=localhost",
    ]
    try:
        completed = subprocess.run(command, check=False, capture_output=True, timeout=30)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0:
        return None
    return cert_path, key_path


if __name__ == "__main__":
    unittest.main()
