"""Synchronous native Python client for GlifiStore."""

from __future__ import annotations

import concurrent.futures
import socket
import ssl
import threading
import time
from collections.abc import Sequence
from dataclasses import dataclass
from enum import Enum
from typing import cast

from .protocol import (
    MAX_FRAME_BYTES,
    RESPONSE_HEADER_BYTES,
    Opcode,
    Response,
    Status,
    WorkerRouting,
    _decode_response_from,
    decode_init_identity,
    encode_request,
    worker_for,
    _encode_validated_request_header,
    _validated_request_size,
)


class GlifiError(Exception):
    """Base class for Python client failures with client-semantics v1 fields."""

    category: str = "internal"

    def __init__(
        self,
        message: str,
        *,
        category: str | None = None,
        wire_status: int | None = None,
        mutation_outcome: MutationOutcome | None = None,
        bytes_sent: int = 0,
        request_id: int | None = None,
        worker: int | None = None,
        routing_epoch: int | None = None,
        retryability: str | None = None,
        operation: str | None = None,
    ) -> None:
        super().__init__(message)
        self.message = message
        if category is not None:
            self.category = category
        self.wire_status = wire_status
        self.mutation_outcome = mutation_outcome
        self.bytes_sent = bytes_sent
        self.request_id = request_id
        self.worker = worker
        self.routing_epoch = routing_epoch
        self.operation = operation
        indeterminate = mutation_outcome is MutationOutcome.INDETERMINATE
        self.retryability = retryability or _retryability_for(
            self.category,
            bytes_sent > 0 and mutation_outcome is not None,
            indeterminate,
        )


class InvalidArgument(GlifiError):
    category = "invalid_argument"


class ProtocolError(GlifiError):
    category = "protocol"


class TransportError(GlifiError):
    category = "transport"


class NotFound(GlifiError):
    category = "not_found"


class Overloaded(GlifiError):
    category = "overloaded"


class Unavailable(GlifiError):
    category = "unavailable"


class PermissionDenied(GlifiError):
    category = "permission_denied"


class InternalError(GlifiError):
    category = "internal"


def _retryability_for(category: str, mutation_sent: bool, indeterminate: bool) -> str:
    if indeterminate:
        return "reconcile_first"
    if category == "invalid_argument" and not mutation_sent:
        return "never"
    if category == "transport" and not mutation_sent:
        return "same_request"
    if category == "overloaded":
        # Wire OVERLOADED collapses admission and capacity exhaustion; fail closed.
        return "never"
    if category == "permission_denied":
        return "never"
    if category == "not_found":
        return "new_attempt"
    if category == "unavailable":
        return "never"
    if mutation_sent:
        return "reconcile_first"
    return "new_attempt"


def _enrich(
    error: GlifiError,
    *,
    operation: str | None = None,
    request_id: int | None = None,
    worker: int | None = None,
    routing_epoch: int | None = None,
    bytes_sent: int | None = None,
    mutation_outcome: MutationOutcome | None = None,
    wire_status: int | None = None,
) -> GlifiError:
    if operation is not None:
        error.operation = operation
    if request_id is not None:
        error.request_id = request_id
    if worker is not None:
        error.worker = worker
    if routing_epoch is not None:
        error.routing_epoch = routing_epoch
    if bytes_sent is not None:
        error.bytes_sent = bytes_sent
    if mutation_outcome is not None:
        error.mutation_outcome = mutation_outcome
    if wire_status is not None:
        error.wire_status = wire_status
    indeterminate = error.mutation_outcome is MutationOutcome.INDETERMINATE
    mutation_sent = error.bytes_sent > 0 and error.mutation_outcome is not None
    # Indeterminate always means reconcile_first — including transport failures after a
    # full mutation send where bytes_sent was omitted (must not fall through to same_request).
    if indeterminate:
        error.retryability = "reconcile_first"
    elif (
        error.mutation_outcome is not None
        and error.bytes_sent > 0
        and error.category == "transport"
    ):
        error.retryability = "reconcile_first"
    elif error.bytes_sent == 0 and error.category == "transport":
        error.retryability = "same_request"
    else:
        error.retryability = _retryability_for(error.category, mutation_sent, indeterminate)
    return error


def _clone_error(error: GlifiError) -> GlifiError:
    """Fresh error of the same type so per-pipeline-slot enrich does not share state."""
    return type(error)(
        error.message,
        category=error.category,
        wire_status=error.wire_status,
    )


def _pipeline_op_name(opcode: PipelineOpcode) -> str:
    if opcode is PipelineOpcode.PUT:
        return "put"
    if opcode is PipelineOpcode.ERASE:
        return "erase"
    return "get"


class MutationOutcome(Enum):
    COMMITTED = "committed"
    REJECTED = "rejected"
    INDETERMINATE = "indeterminate"


@dataclass(frozen=True, slots=True)
class MutationResult:
    outcome: MutationOutcome
    error: GlifiError | None = None

    @property
    def committed(self) -> bool:
        return self.outcome is MutationOutcome.COMMITTED


class PipelineOpcode(Enum):
    GET = Opcode.GET
    PUT = Opcode.PUT
    ERASE = Opcode.ERASE


@dataclass(frozen=True, slots=True)
class PipelineRequest:
    opcode: PipelineOpcode
    key: bytes | bytearray | memoryview
    value: bytes | bytearray | memoryview = b""
    expire_at_ns: int = 0


class PipelineOutcome(Enum):
    SUCCEEDED = "succeeded"
    FAILED = "failed"
    INDETERMINATE = "indeterminate"


@dataclass(frozen=True, slots=True)
class PipelineResponse:
    outcome: PipelineOutcome
    value: bytes = b""
    error: GlifiError | None = None

    @property
    def succeeded(self) -> bool:
        return self.outcome is PipelineOutcome.SUCCEEDED


def _resolved_pipeline(
    responses: list[PipelineResponse | None],
) -> list[PipelineResponse]:
    """Narrow a slot vector after every position has been decided exactly once."""
    return cast(list[PipelineResponse], responses)


@dataclass(frozen=True, slots=True)
class ClientConfig:
    host: str = "127.0.0.1"
    port: int = 7379
    # When set, dial AF_UNIX at this path instead of TCP host:port (ADR 0029).
    # Mutually exclusive with TLS.
    unix_socket_path: str | None = None
    connect_timeout: float = 3.0
    request_timeout: float = 5.0
    maximum_frame_bytes: int = MAX_FRAME_BYTES
    maximum_pipeline_requests: int = 256
    maximum_pipeline_bytes: int = 1024 * 1024
    # Opt-in TLS 1.3 (ADR 0020). Cleartext remains the default. When tls is True the
    # client fails closed (no cleartext fallback). Hostname/SNI verification is on
    # unless insecure_skip_verify is set (lab escape only).
    tls: bool = False
    tls_ca: str | None = None
    cert_file: str | None = None
    key_file: str | None = None
    server_name: str | None = None
    insecure_skip_verify: bool = False


def build_ssl_context(config: ClientConfig) -> ssl.SSLContext:
    """Build a TLS 1.3 client context from ClientConfig (sync and async)."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    if config.insecure_skip_verify:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    else:
        context.check_hostname = True
        context.verify_mode = ssl.CERT_REQUIRED
        if config.tls_ca:
            context.load_verify_locations(cafile=config.tls_ca)
        else:
            context.load_default_certs()
    if config.cert_file and config.key_file:
        context.load_cert_chain(certfile=config.cert_file, keyfile=config.key_file)
    return context


class _SendFailure(Exception):
    def __init__(self, error: TransportError, bytes_sent: int) -> None:
        super().__init__(str(error))
        self.error = error
        self.bytes_sent = bytes_sent


def _deadline_after(seconds: float) -> float:
    return time.monotonic() + seconds


def _remaining(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TransportError("request deadline expired")
    return remaining


class _Connection:
    def __init__(self, worker: int) -> None:
        self.worker = worker
        self.lock = threading.Lock()
        self.socket: socket.socket | None = None
        self.input = bytearray()
        self.input_offset = 0

    def reset(self) -> None:
        current, self.socket = self.socket, None
        self.input.clear()
        self.input_offset = 0
        if current is not None:
            try:
                current.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            current.close()


class Client:
    """Thread-safe synchronous client with one bound connection per Worker.

    Health is a plain boolean written without nesting under connection locks,
    matching the C++ ``atomic`` fail-closed model and avoiding lock-order
    deadlocks with ``close()``.
    """

    def __init__(self, config: ClientConfig) -> None:
        self._validate_config(config)
        self._config = config
        self._connections: list[_Connection] = []
        self._worker_count = 0
        self._routing_epoch = 0
        self._routing = WorkerRouting()
        self._request_id = 1
        self._request_id_lock = threading.Lock()
        self._healthy = True

    @classmethod
    def connect(cls, config: ClientConfig | None = None) -> Client:
        client = cls(config or ClientConfig())
        first = _Connection(0)
        try:
            worker_count, routing_epoch, routing = client._bootstrap(first, None)
            client._worker_count = worker_count
            client._routing_epoch = routing_epoch
            client._routing = routing
            client._connections.append(first)
            expected = (worker_count, routing_epoch, routing)
            for worker in range(1, worker_count):
                connection = _Connection(worker)
                client._bootstrap(connection, expected)
                client._connections.append(connection)
        except Exception:
            client.close()
            raise
        return client

    @staticmethod
    def _validate_config(config: ClientConfig) -> None:
        unix = bool(config.unix_socket_path)
        if unix:
            if config.tls:
                raise InvalidArgument("TLS is not supported on AF_UNIX clients")
            if not config.unix_socket_path:
                raise InvalidArgument("unix_socket_path must be non-empty when set")
        elif not config.host or not 0 < config.port <= 65_535:
            raise InvalidArgument("client configuration is outside protocol limits")
        if (
            config.connect_timeout <= 0
            or config.request_timeout <= 0
            or not RESPONSE_HEADER_BYTES <= config.maximum_frame_bytes <= MAX_FRAME_BYTES
            or config.maximum_pipeline_requests <= 0
            or config.maximum_pipeline_bytes < 40
        ):
            raise InvalidArgument("client configuration is outside protocol limits")
        has_cert = bool(config.cert_file)
        has_key = bool(config.key_file)
        if config.tls and has_cert != has_key:
            raise InvalidArgument("TLS mTLS requires both cert_file and key_file (fail closed)")
        if (has_cert or has_key) and not config.tls:
            raise InvalidArgument("cert_file/key_file require tls=True (fail closed)")

    def _ssl_context(self) -> ssl.SSLContext:
        return build_ssl_context(self._config)

    @property
    def worker_count(self) -> int:
        return self._worker_count

    @property
    def routing_epoch(self) -> int:
        return self._routing_epoch

    @property
    def routing(self) -> WorkerRouting:
        return self._routing

    @property
    def healthy(self) -> bool:
        return self._healthy

    def worker_for(self, key: bytes | bytearray | memoryview) -> int:
        if self._worker_count <= 0:
            raise Unavailable("client is not connected")
        return worker_for(bytes(key), self._worker_count, self._routing)

    def get(
        self,
        key: bytes | bytearray | memoryview,
        *,
        timeout: float | None = None,
    ) -> bytes:
        return self._read(Opcode.GET, bytes(key), b"", timeout=timeout)

    def ping(
        self,
        payload: bytes | bytearray | memoryview = b"",
        *,
        timeout: float | None = None,
    ) -> bytes:
        return self._read(Opcode.PING, b"", bytes(payload), timeout=timeout)

    def health(self, *, timeout: float | None = None) -> bytes:
        """Wire HEALTH probe (Worker 0). On OK: ASCII GlifiStore/live."""
        return self._read(Opcode.HEALTH, b"", b"", timeout=timeout)

    def ready(self, *, timeout: float | None = None) -> bytes:
        """Wire READY probe (Worker 0). On OK: ASCII GlifiStore/ready."""
        return self._read(Opcode.READY, b"", b"", timeout=timeout)

    def stats(self, *, timeout: float | None = None) -> bytes:
        """Wire STATS text body (Worker 0), starting with GlifiStore/stats."""
        return self._read(Opcode.STATS, b"", b"", timeout=timeout)

    def backup(
        self,
        destination: str | bytes | bytearray | memoryview,
        *,
        timeout: float | None = None,
    ) -> bytes:
        # Online fenced BACKUP (opcode 10): not hot zero-impact; admin under secure authz.
        path = destination.encode("utf-8") if isinstance(destination, str) else bytes(destination)
        if not path:
            raise InvalidArgument("backup destination must be non-empty")
        return self._read(Opcode.BACKUP, path, b"", timeout=timeout)

    def put(
        self,
        key: bytes | bytearray | memoryview,
        value: bytes | bytearray | memoryview,
        *,
        expire_at_ns: int = 0,
        timeout: float | None = None,
    ) -> MutationResult:
        return self._mutate(Opcode.PUT, bytes(key), bytes(value), expire_at_ns, timeout=timeout)

    def erase(
        self,
        key: bytes | bytearray | memoryview,
        *,
        timeout: float | None = None,
    ) -> MutationResult:
        return self._mutate(Opcode.ERASE, bytes(key), b"", 0, timeout=timeout)

    def execute_pipeline(
        self,
        requests: Sequence[PipelineRequest],
        *,
        timeout: float | None = None,
    ) -> list[PipelineResponse]:
        """Execute one ordered, non-atomic pipeline on a single Worker connection."""
        return self._execute_pipeline(requests, timeout=timeout)

    def _execute_pipeline(
        self,
        requests: Sequence[PipelineRequest],
        *,
        timeout: float | None = None,
        deadline_override: float | None = None,
        routed_worker: int | None = None,
    ) -> list[PipelineResponse]:
        if not requests:
            return []
        if not self._healthy:
            raise Unavailable("client is closed or routing metadata changed")
        if len(requests) > self._config.maximum_pipeline_requests:
            raise InvalidArgument("pipeline exceeds the configured request limit")
        deadline = (
            deadline_override if deadline_override is not None else self._request_deadline(timeout)
        )

        normalized: list[tuple[PipelineOpcode, bytes, bytes, int]] = []
        parts: list[bytes] = []
        metadata: list[tuple[int, int]] = []
        output_size = 0
        worker = routed_worker
        for request in requests:
            if not isinstance(request.opcode, PipelineOpcode):
                raise InvalidArgument("pipeline request contains an invalid opcode")
            key, value = bytes(request.key), bytes(request.value)
            if routed_worker is None:
                request_worker = self.worker_for(key)
                if worker is None:
                    worker = request_worker
                elif request_worker != worker:
                    raise InvalidArgument("every pipeline key must route to the same Worker")
            if request.opcode in (PipelineOpcode.GET, PipelineOpcode.ERASE) and (
                value or request.expire_at_ns != 0
            ):
                raise InvalidArgument("GET and ERASE pipeline requests cannot carry PUT fields")
            request_id = self._next_request_id()
            try:
                frame_size, key, value = _validated_request_size(
                    request.opcode.value,
                    request_id,
                    key=key,
                    value=value,
                    expire_at_ns=request.expire_at_ns,
                )
            except ValueError as error:
                raise InvalidArgument(str(error)) from error
            if frame_size > self._config.maximum_frame_bytes:
                raise InvalidArgument("pipeline request exceeds the configured frame limit")
            if frame_size > self._config.maximum_pipeline_bytes - output_size:
                raise InvalidArgument("pipeline exceeds the configured aggregate byte limit")
            normalized.append((request.opcode, key, value, request.expire_at_ns))
            metadata.append((request_id, output_size))
            parts.extend(
                (
                    _encode_validated_request_header(
                        frame_size,
                        request.opcode.value,
                        request_id,
                        key_size=len(key),
                        value_size=len(value),
                        expire_at_ns=request.expire_at_ns,
                    ),
                    key,
                    value,
                )
            )
            output_size += frame_size

        assert worker is not None
        output = b"".join(parts)
        responses: list[PipelineResponse | None] = [None] * len(requests)
        connection = self._connections[worker]
        with connection.lock:
            if not self._healthy:
                raise Unavailable("client closed before pipeline admission")
            self._ensure_connected(connection)

            def mark_unresolved(first: int, error: GlifiError, bytes_sent: int) -> None:
                for index in range(first, len(normalized)):
                    opcode = normalized[index][0]
                    is_mutation = opcode in (PipelineOpcode.PUT, PipelineOpcode.ERASE)
                    mutation_may_have_arrived = is_mutation and bytes_sent > metadata[index][1]
                    pos_bytes = (
                        bytes_sent - metadata[index][1] if bytes_sent > metadata[index][1] else 0
                    )
                    mutation_outcome = None
                    if is_mutation:
                        mutation_outcome = (
                            MutationOutcome.INDETERMINATE
                            if mutation_may_have_arrived
                            else MutationOutcome.REJECTED
                        )
                    responses[index] = PipelineResponse(
                        PipelineOutcome.INDETERMINATE
                        if mutation_may_have_arrived
                        else PipelineOutcome.FAILED,
                        error=_enrich(
                            _clone_error(error),
                            operation=_pipeline_op_name(opcode),
                            request_id=metadata[index][0],
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=pos_bytes,
                            mutation_outcome=mutation_outcome,
                        ),
                    )

            try:
                self._send(connection, output, deadline)
            except _SendFailure as error:
                connection.reset()
                mark_unresolved(0, error.error, error.bytes_sent)
                return _resolved_pipeline(responses)

            for index, (opcode, _, _, _) in enumerate(normalized):
                try:
                    response = self._receive_response(connection, deadline)
                    self._validate_response(response, metadata[index][0], worker)
                except (TransportError, ProtocolError, Unavailable) as error:
                    connection.reset()
                    mark_unresolved(index, error, len(output))
                    return _resolved_pipeline(responses)
                if response.status is Status.OK:
                    if opcode in (PipelineOpcode.PUT, PipelineOpcode.ERASE) and response.value:
                        connection.reset()
                        mark_unresolved(
                            index,
                            ProtocolError("mutation response value must be empty"),
                            len(output),
                        )
                        return _resolved_pipeline(responses)
                    responses[index] = PipelineResponse(
                        PipelineOutcome.SUCCEEDED, value=response.value
                    )
                    continue
                is_mutation = opcode in (PipelineOpcode.PUT, PipelineOpcode.ERASE)
                indeterminate = is_mutation and response.status is Status.INTERNAL_ERROR
                mutation_outcome = None
                if is_mutation:
                    mutation_outcome = (
                        MutationOutcome.INDETERMINATE if indeterminate else MutationOutcome.REJECTED
                    )
                responses[index] = PipelineResponse(
                    PipelineOutcome.INDETERMINATE if indeterminate else PipelineOutcome.FAILED,
                    error=_enrich(
                        self._status_error(response.status),
                        operation=_pipeline_op_name(opcode),
                        request_id=metadata[index][0],
                        worker=worker,
                        routing_epoch=self._routing_epoch,
                        bytes_sent=len(output) - metadata[index][1],
                        mutation_outcome=mutation_outcome,
                        wire_status=int(response.status),
                    ),
                )
                if response.status in (Status.WRONG_OWNER, Status.NOT_BOUND):
                    self._healthy = False
            return _resolved_pipeline(responses)

    def execute_batch(
        self,
        requests: Sequence[PipelineRequest],
        *,
        timeout: float | None = None,
    ) -> list[PipelineResponse]:
        """Group by Worker, run per-Worker pipelines concurrently, restore caller order.

        Not atomic: Workers succeed or fail independently after admission.
        """
        if not requests:
            return []
        if not self._healthy:
            raise Unavailable("client is closed or routing metadata changed")
        # One absolute deadline for the whole batch across Workers.
        shared_deadline = self._request_deadline(timeout)

        groups: dict[int, list[tuple[int, PipelineRequest]]] = {}
        for index, request in enumerate(requests):
            if not isinstance(request.opcode, PipelineOpcode):
                raise InvalidArgument("batch request contains an invalid opcode")
            key = bytes(request.key)
            value = bytes(request.value)
            if request.opcode in (PipelineOpcode.GET, PipelineOpcode.ERASE) and (
                value or request.expire_at_ns != 0
            ):
                raise InvalidArgument("GET and ERASE batch requests cannot carry PUT fields")
            worker = self.worker_for(key)
            bucket = groups.setdefault(worker, [])
            if len(bucket) >= self._config.maximum_pipeline_requests:
                raise InvalidArgument("batch exceeds the configured per-Worker request limit")
            # Freeze bytes at admission: the trusted Worker and encoded key must
            # describe the same snapshot even for mutable buffer inputs.
            bucket.append(
                (
                    index,
                    PipelineRequest(request.opcode, key, value, request.expire_at_ns),
                )
            )

        responses: list[PipelineResponse | None] = [None] * len(requests)

        def run_group(
            worker: int,
            items: list[tuple[int, PipelineRequest]],
        ) -> tuple[list[int], list[PipelineResponse]]:
            indices = [index for index, _ in items]
            group_requests = [request for _, request in items]
            try:
                return indices, self._execute_pipeline(
                    group_requests,
                    deadline_override=shared_deadline,
                    routed_worker=worker,
                )
            except GlifiError as error:
                # Match C++/Erlang: stamp this Worker's slots failed with rejected
                # polarity (bytes_sent=0); keep sibling results.
                failed = [
                    PipelineResponse(
                        PipelineOutcome.FAILED,
                        error=_enrich(
                            _clone_error(error),
                            operation=_pipeline_op_name(request.opcode),
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=0,
                            mutation_outcome=(
                                MutationOutcome.REJECTED
                                if request.opcode in (PipelineOpcode.PUT, PipelineOpcode.ERASE)
                                else None
                            ),
                        ),
                    )
                    for _, request in items
                ]
                return indices, failed

        if len(groups) == 1:
            worker, items = next(iter(groups.items()))
            indices, group_responses = run_group(worker, items)
            for index, response in zip(indices, group_responses, strict=True):
                responses[index] = response
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(groups)) as pool:
                futures = [
                    pool.submit(run_group, worker, items) for worker, items in groups.items()
                ]
                for future in concurrent.futures.as_completed(futures):
                    indices, group_responses = future.result()
                    for index, response in zip(indices, group_responses, strict=True):
                        responses[index] = response

        return [
            response if response is not None else PipelineResponse(PipelineOutcome.FAILED)
            for response in responses
        ]

    def execute_worker_pipelines(
        self,
        batches: Sequence[Sequence[PipelineRequest]],
        *,
        timeout: float | None = None,
    ) -> list[list[PipelineResponse]]:
        """Run one pre-sharded pipeline vector per Worker (Perl/Erlang parity).

        ``len(batches)`` must equal ``worker_count``. Empty slots yield ``[]``.
        Every key in ``batches[i]`` must route to Worker ``i``. Not atomic.
        """
        if not self._healthy:
            raise Unavailable("client is closed or routing metadata changed")
        if len(batches) != self._worker_count:
            raise InvalidArgument("worker pipeline vector does not match Worker count")
        shared_deadline = self._request_deadline(timeout)

        prepared: list[tuple[int, list[PipelineRequest]]] = []
        results: list[list[PipelineResponse]] = [[] for _ in range(self._worker_count)]
        for worker, requests in enumerate(batches):
            if not requests:
                continue
            if len(requests) > self._config.maximum_pipeline_requests:
                raise InvalidArgument("pipeline exceeds the configured request limit")
            frozen: list[PipelineRequest] = []
            for request in requests:
                if not isinstance(request.opcode, PipelineOpcode):
                    raise InvalidArgument("pipeline request contains an invalid opcode")
                key = bytes(request.key)
                value = bytes(request.value)
                if self.worker_for(key) != worker:
                    raise InvalidArgument("pipeline key does not route to the expected Worker")
                if request.opcode in (PipelineOpcode.GET, PipelineOpcode.ERASE) and (
                    value or request.expire_at_ns != 0
                ):
                    raise InvalidArgument("GET and ERASE pipeline requests cannot carry PUT fields")
                frozen.append(PipelineRequest(request.opcode, key, value, request.expire_at_ns))
            prepared.append((worker, frozen))

        def run_group(worker: int, group_requests: list[PipelineRequest]) -> tuple[int, list[PipelineResponse]]:
            try:
                return worker, self._execute_pipeline(
                    group_requests,
                    deadline_override=shared_deadline,
                    routed_worker=worker,
                )
            except GlifiError as error:
                failed = [
                    PipelineResponse(
                        PipelineOutcome.FAILED,
                        error=_enrich(
                            _clone_error(error),
                            operation=_pipeline_op_name(request.opcode),
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=0,
                            mutation_outcome=(
                                MutationOutcome.REJECTED
                                if request.opcode in (PipelineOpcode.PUT, PipelineOpcode.ERASE)
                                else None
                            ),
                        ),
                    )
                    for request in group_requests
                ]
                return worker, failed

        if len(prepared) == 1:
            worker, group_requests = prepared[0]
            _, group_responses = run_group(worker, group_requests)
            results[worker] = group_responses
        elif prepared:
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(prepared)) as pool:
                futures = [
                    pool.submit(run_group, worker, group_requests)
                    for worker, group_requests in prepared
                ]
                for future in concurrent.futures.as_completed(futures):
                    worker, group_responses = future.result()
                    results[worker] = group_responses
        return results

    def close(self) -> None:
        self._healthy = False
        for connection in self._connections:
            with connection.lock:
                connection.reset()

    def __enter__(self) -> Client:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _next_request_id(self) -> int:
        with self._request_id_lock:
            current = self._request_id
            self._request_id = 1 if current == 0xFFFF_FFFF_FFFF_FFFF else current + 1
            return current

    def _open_socket(self) -> socket.socket:
        try:
            if self._config.unix_socket_path:
                opened = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                opened.settimeout(self._config.connect_timeout)
                opened.connect(self._config.unix_socket_path)
                return opened
            opened = socket.create_connection(
                (self._config.host, self._config.port), self._config.connect_timeout
            )
            opened.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if not self._config.tls:
                return opened
            server_hostname = self._config.server_name or self._config.host
            try:
                return self._ssl_context().wrap_socket(opened, server_hostname=server_hostname)
            except ssl.SSLError as error:
                opened.close()
                raise Unavailable(f"TLS handshake failed: {error}") from error
        except OSError as error:
            raise Unavailable(f"could not connect to GlifiStore: {error}") from error

    def _bootstrap(
        self,
        connection: _Connection,
        expected: tuple[int, int, WorkerRouting] | None,
    ) -> tuple[int, int, WorkerRouting]:
        connection.reset()
        connection.socket = self._open_socket()
        deadline = _deadline_after(self._config.request_timeout)
        try:
            init_id = self._next_request_id()
            response = self._exchange(connection, encode_request(Opcode.INIT, init_id), deadline)
            if (
                response.status is not Status.OK
                or response.request_id != init_id
                or not 0 < response.worker_count <= 256
                or response.routing_epoch == 0
            ):
                raise ProtocolError("server INIT response is inconsistent")
            try:
                routing = decode_init_identity(response.value)
            except ValueError as error:
                raise ProtocolError("server INIT response is inconsistent") from error
            metadata = (response.worker_count, response.routing_epoch, routing)
            if expected is not None and metadata != expected:
                raise Unavailable("server routing metadata changed during bootstrap")
            bind_id = self._next_request_id()
            bound = self._exchange(
                connection,
                encode_request(
                    Opcode.BIND_WORKER,
                    bind_id,
                    target_worker=connection.worker,
                ),
                deadline,
            )
            if (
                bound.status is not Status.OK
                or bound.request_id != bind_id
                or bound.owner_worker != connection.worker
                or bound.worker_count != response.worker_count
                or bound.routing_epoch != response.routing_epoch
            ):
                raise ProtocolError("server BIND_WORKER response is inconsistent")
            return metadata
        except _SendFailure as error:
            connection.reset()
            raise Unavailable(str(error.error)) from error
        except TransportError as error:
            connection.reset()
            raise Unavailable(str(error)) from error
        except Exception:
            connection.reset()
            raise

    def _ensure_connected(self, connection: _Connection) -> None:
        if connection.socket is None:
            self._bootstrap(connection, (self._worker_count, self._routing_epoch, self._routing))

    def _send(self, connection: _Connection, frame: bytes, deadline: float) -> None:
        assert connection.socket is not None
        sent = 0
        view = memoryview(frame)
        try:
            while sent < len(frame):
                connection.socket.settimeout(_remaining(deadline))
                count = connection.socket.send(view[sent:])
                if count == 0:
                    raise OSError("socket closed during send")
                sent += count
        except (OSError, TimeoutError, TransportError) as error:
            wrapped = (
                error
                if isinstance(error, TransportError)
                else TransportError(f"request send failed: {error}")
            )
            raise _SendFailure(wrapped, sent) from error

    def _receive_response(self, connection: _Connection, deadline: float) -> Response:
        assert connection.socket is not None
        while True:
            available = len(connection.input) - connection.input_offset
            if available >= 4:
                frame_size = int.from_bytes(
                    connection.input[connection.input_offset : connection.input_offset + 4],
                    "little",
                )
                if not RESPONSE_HEADER_BYTES <= frame_size <= self._config.maximum_frame_bytes:
                    raise ProtocolError("server response size is outside client limits")
                if available >= frame_size:
                    start = connection.input_offset
                    try:
                        response = _decode_response_from(
                            connection.input,
                            start,
                            frame_size,
                            self._config.maximum_frame_bytes,
                        )
                    except ValueError as error:
                        raise ProtocolError(str(error)) from error
                    connection.input_offset += frame_size
                    if connection.input_offset == len(connection.input):
                        connection.input.clear()
                        connection.input_offset = 0
                    return response
            if connection.input_offset:
                del connection.input[: connection.input_offset]
                connection.input_offset = 0
            try:
                connection.socket.settimeout(_remaining(deadline))
                chunk = connection.socket.recv(64 * 1024)
                if not chunk:
                    raise OSError("server closed the connection")
                connection.input.extend(chunk)
            except (OSError, TimeoutError, TransportError) as error:
                if isinstance(error, TransportError):
                    raise
                raise TransportError(f"response receive failed: {error}") from error

    def _exchange(self, connection: _Connection, frame: bytes, deadline: float) -> Response:
        self._send(connection, frame, deadline)
        return self._receive_response(connection, deadline)

    def _validate_response(self, response: Response, request_id: int, worker: int) -> None:
        if response.request_id != request_id:
            raise ProtocolError("server response request ID does not match")
        if (
            response.worker_count != self._worker_count
            or response.routing_epoch != self._routing_epoch
        ):
            self._healthy = False
            raise Unavailable("server routing metadata changed")
        if response.owner_worker != worker and response.status is not Status.WRONG_OWNER:
            self._healthy = False
            raise ProtocolError("server response came from the wrong Worker")

    @staticmethod
    def _status_error(status: Status | int) -> GlifiError:
        code = int(status)
        if code == Status.NOT_FOUND:
            error: GlifiError = NotFound("key was not found")
        elif code == Status.OVERLOADED:
            error = Overloaded("server is overloaded")
        elif code == Status.NOT_BOUND:
            error = Unavailable("server connection is not bound")
        elif code == Status.PERMISSION_DENIED:
            error = PermissionDenied("server denied the request")
        elif code == Status.WRONG_OWNER:
            error = ProtocolError("server rejected Worker routing")
        elif code in (Status.INVALID_REQUEST, Status.UNSUPPORTED):
            error = InvalidArgument("server rejected the request")
        elif code in (Status.INTERNAL_ERROR, Status.OK):
            error = InternalError("server reported an internal error")
        else:
            error = ProtocolError("server returned an unknown status")
        error.wire_status = code
        return error

    def _request_deadline(self, timeout: float | None) -> float:
        if timeout is not None and timeout <= 0:
            raise InvalidArgument("request timeout must be positive")
        seconds = self._config.request_timeout if timeout is None else timeout
        return _deadline_after(seconds)

    def _read(
        self,
        opcode: Opcode,
        key: bytes,
        value: bytes,
        *,
        timeout: float | None = None,
    ) -> bytes:
        if not self._healthy:
            raise Unavailable("client is closed or routing metadata changed")
        deadline = self._request_deadline(timeout)
        worker = (
            0
            if opcode in (Opcode.PING, Opcode.BACKUP, Opcode.HEALTH, Opcode.READY, Opcode.STATS)
            else self.worker_for(key)
        )
        connection = self._connections[worker]
        with connection.lock:
            if not self._healthy:
                raise Unavailable("client closed before read admission")
            last_error: GlifiError = Unavailable("request was not attempted")
            for _ in range(2):
                request_id: int | None = None
                frame = b""
                try:
                    self._ensure_connected(connection)
                    request_id = self._next_request_id()
                    try:
                        frame = encode_request(opcode, request_id, key=key, value=value)
                    except ValueError as error:
                        raise InvalidArgument(str(error)) from error
                    if len(frame) > self._config.maximum_frame_bytes:
                        raise InvalidArgument("request exceeds the configured frame limit")
                    response = self._exchange(connection, frame, deadline)
                    self._validate_response(response, request_id, worker)
                    if response.status is not Status.OK:
                        if response.status in (Status.WRONG_OWNER, Status.NOT_BOUND):
                            self._healthy = False
                        if opcode is Opcode.BACKUP and response.status is Status.INTERNAL_ERROR:
                            # Fenced copy may already be committed — same polarity as C++.
                            raise _enrich(
                                self._status_error(response.status),
                                operation="backup",
                                request_id=request_id,
                                worker=worker,
                                routing_epoch=self._routing_epoch,
                                bytes_sent=len(frame),
                                mutation_outcome=MutationOutcome.INDETERMINATE,
                            )
                        raise self._status_error(response.status)
                    return response.value
                except (TransportError, _SendFailure) as error:
                    if isinstance(error, _SendFailure):
                        last_error = error.error
                        bytes_sent = error.bytes_sent
                    else:
                        # Receive-side failure implies the BACKUP frame left the client.
                        last_error = error
                        bytes_sent = len(frame)
                    connection.reset()
                    if opcode is Opcode.BACKUP and bytes_sent > 0:
                        # Not idempotent to the same destination — same polarity as C++.
                        raise _enrich(
                            last_error,
                            operation="backup",
                            request_id=request_id,
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=bytes_sent,
                            mutation_outcome=MutationOutcome.INDETERMINATE,
                        ) from error
                except Unavailable as error:
                    connection.reset()
                    if opcode is Opcode.BACKUP and len(frame) > 0:
                        # Validate/metadata failure after a BACKUP response arrived.
                        raise _enrich(
                            error,
                            operation="backup",
                            request_id=request_id,
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=len(frame),
                            mutation_outcome=MutationOutcome.INDETERMINATE,
                        ) from error
                    if not self._healthy:
                        raise
                    last_error = error
                except ProtocolError as error:
                    connection.reset()
                    if opcode is Opcode.BACKUP and len(frame) > 0:
                        raise _enrich(
                            error,
                            operation="backup",
                            request_id=request_id,
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=len(frame),
                            mutation_outcome=MutationOutcome.INDETERMINATE,
                        ) from error
                    raise
            raise last_error

    def _mutate(
        self,
        opcode: Opcode,
        key: bytes,
        value: bytes,
        expire_at_ns: int,
        *,
        timeout: float | None = None,
    ) -> MutationResult:
        op = "put" if opcode is Opcode.PUT else "erase"
        if not self._healthy:
            return MutationResult(
                MutationOutcome.REJECTED,
                _enrich(
                    Unavailable("client is closed or routing metadata changed"),
                    operation=op,
                    mutation_outcome=MutationOutcome.REJECTED,
                ),
            )
        try:
            deadline = self._request_deadline(timeout)
        except InvalidArgument as error:
            return MutationResult(
                MutationOutcome.REJECTED,
                _enrich(error, operation=op, mutation_outcome=MutationOutcome.REJECTED),
            )
        worker = self.worker_for(key)
        connection = self._connections[worker]
        with connection.lock:
            if not self._healthy:
                return MutationResult(
                    MutationOutcome.REJECTED,
                    _enrich(
                        Unavailable("client closed before mutation admission"),
                        operation=op,
                        worker=worker,
                        routing_epoch=self._routing_epoch,
                        mutation_outcome=MutationOutcome.REJECTED,
                    ),
                )
            for attempt in range(2):
                try:
                    self._ensure_connected(connection)
                except GlifiError as error:
                    return MutationResult(
                        MutationOutcome.REJECTED,
                        _enrich(
                            error,
                            operation=op,
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            mutation_outcome=MutationOutcome.REJECTED,
                        ),
                    )
                try:
                    request_id = self._next_request_id()
                    try:
                        frame = encode_request(
                            opcode,
                            request_id,
                            key=key,
                            value=value,
                            expire_at_ns=expire_at_ns,
                        )
                    except ValueError as error:
                        return MutationResult(
                            MutationOutcome.REJECTED,
                            _enrich(
                                InvalidArgument(str(error)),
                                operation=op,
                                request_id=request_id,
                                worker=worker,
                                routing_epoch=self._routing_epoch,
                                mutation_outcome=MutationOutcome.REJECTED,
                            ),
                        )
                    if len(frame) > self._config.maximum_frame_bytes:
                        return MutationResult(
                            MutationOutcome.REJECTED,
                            _enrich(
                                InvalidArgument("request exceeds the configured frame limit"),
                                operation=op,
                                request_id=request_id,
                                worker=worker,
                                routing_epoch=self._routing_epoch,
                                mutation_outcome=MutationOutcome.REJECTED,
                            ),
                        )
                    response = self._exchange(connection, frame, deadline)
                    self._validate_response(response, request_id, worker)
                except _SendFailure as error:
                    connection.reset()
                    outcome = (
                        MutationOutcome.REJECTED
                        if error.bytes_sent == 0
                        else MutationOutcome.INDETERMINATE
                    )
                    promoted = _enrich(
                        error.error,
                        operation=op,
                        request_id=request_id,
                        worker=worker,
                        routing_epoch=self._routing_epoch,
                        bytes_sent=error.bytes_sent,
                        mutation_outcome=outcome,
                    )
                    if error.bytes_sent == 0:
                        if attempt == 0:
                            continue
                        return MutationResult(MutationOutcome.REJECTED, promoted)
                    return MutationResult(MutationOutcome.INDETERMINATE, promoted)
                except (TransportError, ProtocolError, Unavailable) as error:
                    connection.reset()
                    return MutationResult(
                        MutationOutcome.INDETERMINATE,
                        _enrich(
                            error,
                            operation=op,
                            request_id=request_id,
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=len(frame),
                            mutation_outcome=MutationOutcome.INDETERMINATE,
                        ),
                    )
                if response.status is Status.OK:
                    if response.value:
                        connection.reset()
                        return MutationResult(
                            MutationOutcome.INDETERMINATE,
                            _enrich(
                                ProtocolError("mutation response value must be empty"),
                                operation=op,
                                request_id=request_id,
                                worker=worker,
                                routing_epoch=self._routing_epoch,
                                bytes_sent=len(frame),
                                mutation_outcome=MutationOutcome.INDETERMINATE,
                            ),
                        )
                    return MutationResult(MutationOutcome.COMMITTED)
                error = _enrich(
                    self._status_error(response.status),
                    operation=op,
                    request_id=request_id,
                    worker=worker,
                    routing_epoch=self._routing_epoch,
                    wire_status=int(response.status),
                    bytes_sent=len(frame),
                )
                if response.status is Status.INTERNAL_ERROR:
                    return MutationResult(
                        MutationOutcome.INDETERMINATE,
                        _enrich(error, mutation_outcome=MutationOutcome.INDETERMINATE),
                    )
                if response.status in (Status.WRONG_OWNER, Status.NOT_BOUND):
                    self._healthy = False
                return MutationResult(
                    MutationOutcome.REJECTED,
                    _enrich(error, mutation_outcome=MutationOutcome.REJECTED),
                )
            return MutationResult(
                MutationOutcome.REJECTED,
                _enrich(
                    Unavailable("could not send mutation"),
                    operation=op,
                    worker=worker,
                    routing_epoch=self._routing_epoch,
                    mutation_outcome=MutationOutcome.REJECTED,
                ),
            )
