"""Asynchronous native Python client for GlifiStore."""

from __future__ import annotations

import asyncio
import socket
import ssl
import time
from collections.abc import Sequence

from .client import (
    ClientConfig,
    GlifiError,
    InternalError,
    InvalidArgument,
    MutationOutcome,
    MutationResult,
    NotFound,
    Overloaded,
    PermissionDenied,
    PipelineOpcode,
    PipelineOutcome,
    PipelineRequest,
    PipelineResponse,
    ProtocolError,
    TransportError,
    Unavailable,
    _clone_error,
    _enrich,
    _pipeline_op_name,
    _resolved_pipeline,
    build_ssl_context,
)
from .protocol import (
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
        self.lock = asyncio.Lock()
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None
        self.input = bytearray()
        self.input_offset = 0

    async def reset(self) -> None:
        writer, self.writer = self.writer, None
        self.reader = None
        self.input.clear()
        self.input_offset = 0
        if writer is None:
            return
        writer.close()
        try:
            # Shield so a second cancel during close cannot skip local teardown
            # already done above; swallow CancelledError so callers that already
            # classified a post-send outcome (§6.3) are not erased by wait_closed.
            await asyncio.shield(writer.wait_closed())
        except OSError:
            pass
        except asyncio.CancelledError:
            pass


class AsyncClient:
    """Asyncio client with one bound connection per Worker.

    Health is a plain boolean (no nested locks with connection locks). Incomplete
    exchanges, including task cancellation, poison the Worker connection.
    """

    def __init__(self, config: ClientConfig) -> None:
        self._validate_config(config)
        self._config = config
        self._connections: list[_Connection] = []
        self._worker_count = 0
        self._routing_epoch = 0
        self._routing = WorkerRouting()
        self._request_id = 1
        self._request_id_lock = asyncio.Lock()
        self._healthy = True

    @classmethod
    async def connect(cls, config: ClientConfig | None = None) -> AsyncClient:
        client = cls(config or ClientConfig())
        first = _Connection(0)
        try:
            worker_count, routing_epoch, routing = await client._bootstrap(first, None)
            client._worker_count = worker_count
            client._routing_epoch = routing_epoch
            client._routing = routing
            client._connections.append(first)
            expected = (worker_count, routing_epoch, routing)
            for worker in range(1, worker_count):
                connection = _Connection(worker)
                await client._bootstrap(connection, expected)
                client._connections.append(connection)
        except Exception:
            await client.close()
            raise
        return client

    @staticmethod
    def _validate_config(config: ClientConfig) -> None:
        # Keep sync/async validation identical (including TLS fail-closed rules).
        from .client import Client

        Client._validate_config(config)

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

    async def get(
        self,
        key: bytes | bytearray | memoryview,
        *,
        timeout: float | None = None,
    ) -> bytes:
        return await self._read(Opcode.GET, bytes(key), b"", timeout=timeout)

    async def ping(
        self,
        payload: bytes | bytearray | memoryview = b"",
        *,
        timeout: float | None = None,
    ) -> bytes:
        return await self._read(Opcode.PING, b"", bytes(payload), timeout=timeout)

    async def health(self, *, timeout: float | None = None) -> bytes:
        return await self._read(Opcode.HEALTH, b"", b"", timeout=timeout)

    async def ready(self, *, timeout: float | None = None) -> bytes:
        return await self._read(Opcode.READY, b"", b"", timeout=timeout)

    async def stats(self, *, timeout: float | None = None) -> bytes:
        return await self._read(Opcode.STATS, b"", b"", timeout=timeout)

    async def backup(
        self,
        destination: str | bytes | bytearray | memoryview,
        *,
        timeout: float | None = None,
    ) -> bytes:
        # Online fenced BACKUP (opcode 10): not hot zero-impact; admin under secure authz.
        path = destination.encode("utf-8") if isinstance(destination, str) else bytes(destination)
        if not path:
            raise InvalidArgument("backup destination must be non-empty")
        return await self._read(Opcode.BACKUP, path, b"", timeout=timeout)

    async def put(
        self,
        key: bytes | bytearray | memoryview,
        value: bytes | bytearray | memoryview,
        *,
        expire_at_ns: int = 0,
        timeout: float | None = None,
    ) -> MutationResult:
        return await self._mutate(
            Opcode.PUT, bytes(key), bytes(value), expire_at_ns, timeout=timeout
        )

    async def erase(
        self,
        key: bytes | bytearray | memoryview,
        *,
        timeout: float | None = None,
    ) -> MutationResult:
        return await self._mutate(Opcode.ERASE, bytes(key), b"", 0, timeout=timeout)

    async def execute_pipeline(
        self,
        requests: Sequence[PipelineRequest],
        *,
        timeout: float | None = None,
    ) -> list[PipelineResponse]:
        return await self._execute_pipeline(requests, timeout=timeout)

    async def _execute_pipeline(
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
            request_id = await self._next_request_id()
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
        async with connection.lock:
            if not self._healthy:
                raise Unavailable("client closed before pipeline admission")
            await self._ensure_connected(connection)
            completed = False

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
                try:
                    await self._send(connection, output, deadline)
                except _SendFailure as error:
                    mark_unresolved(0, error.error, error.bytes_sent)
                    completed = True
                    await connection.reset()
                    return _resolved_pipeline(responses)

                for index, (opcode, _, _, _) in enumerate(normalized):
                    try:
                        response = await self._receive_response(connection, deadline)
                        self._validate_response(response, metadata[index][0], worker)
                    except (TransportError, ProtocolError, Unavailable) as error:
                        mark_unresolved(index, error, len(output))
                        completed = True
                        await connection.reset()
                        return _resolved_pipeline(responses)
                    except asyncio.CancelledError:
                        # Classify before poison: reset() awaits close and must not
                        # re-raise CancelledError over indeterminate slots (§6.3).
                        mark_unresolved(
                            index,
                            TransportError("request cancelled after send"),
                            len(output),
                        )
                        completed = True
                        try:
                            await connection.reset()
                        except asyncio.CancelledError:
                            pass
                        return _resolved_pipeline(responses)
                    if response.status is Status.OK:
                        if opcode in (PipelineOpcode.PUT, PipelineOpcode.ERASE) and response.value:
                            mark_unresolved(
                                index,
                                ProtocolError("mutation response value must be empty"),
                                len(output),
                            )
                            completed = True
                            await connection.reset()
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
                            MutationOutcome.INDETERMINATE
                            if indeterminate
                            else MutationOutcome.REJECTED
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
                completed = True
                return _resolved_pipeline(responses)
            finally:
                if not completed:
                    await connection.reset()

    async def execute_batch(
        self,
        requests: Sequence[PipelineRequest],
        *,
        timeout: float | None = None,
    ) -> list[PipelineResponse]:
        """Group by Worker, run per-Worker pipelines concurrently, restore caller order."""
        if not requests:
            return []
        if not self._healthy:
            raise Unavailable("client is closed or routing metadata changed")
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

        def _cancelled_group(
            worker: int,
            items: list[tuple[int, PipelineRequest]],
        ) -> list[PipelineResponse]:
            # Pre-pipeline / unclassified cancel: known not newly committed.
            cancelled = TransportError("request cancelled")
            failed: list[PipelineResponse] = []
            for _, request in items:
                is_mutation = request.opcode in (
                    PipelineOpcode.PUT,
                    PipelineOpcode.ERASE,
                )
                failed.append(
                    PipelineResponse(
                        PipelineOutcome.FAILED,
                        error=_enrich(
                            _clone_error(cancelled),
                            operation=_pipeline_op_name(request.opcode),
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=0,
                            mutation_outcome=(MutationOutcome.REJECTED if is_mutation else None),
                        ),
                    )
                )
            return failed

        async def run_group(
            worker: int,
            items: list[tuple[int, PipelineRequest]],
        ) -> tuple[list[int], list[PipelineResponse]]:
            indices = [index for index, _ in items]
            group_requests = [request for _, request in items]
            try:
                return indices, await self._execute_pipeline(
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
                    for _, request in items
                ]
                return indices, failed
            except asyncio.CancelledError:
                # Cancel escaped execute_pipeline (lock / bootstrap / pre-send).
                # After-send cancel is classified inside the pipeline and returned.
                return indices, _cancelled_group(worker, items)

        group_items = list(groups.items())
        tasks = [asyncio.create_task(run_group(worker, items)) for worker, items in group_items]
        try:
            await asyncio.wait(tasks)
        except asyncio.CancelledError:
            # Outer cancel cancels gather-style fan-in and would discard sibling
            # results (§5 / §6.3). Stop peers, let pipelines classify, then return
            # the slot vector instead of re-raising CancelledError.
            for task in tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

        for task, (worker, items) in zip(tasks, group_items, strict=True):
            if task.cancelled():
                group_responses = _cancelled_group(worker, items)
                indices = [index for index, _ in items]
            else:
                exc = task.exception()
                if exc is not None:
                    if isinstance(exc, GlifiError):
                        # Defensive: run_group already enriches; keep rejected /
                        # bytes_sent=0 if a GlifiError somehow escapes.
                        indices = [index for index, _ in items]
                        group_responses = [
                            PipelineResponse(
                                PipelineOutcome.FAILED,
                                error=_enrich(
                                    _clone_error(exc),
                                    operation=_pipeline_op_name(request.opcode),
                                    worker=worker,
                                    routing_epoch=self._routing_epoch,
                                    bytes_sent=0,
                                    mutation_outcome=(
                                        MutationOutcome.REJECTED
                                        if request.opcode
                                        in (PipelineOpcode.PUT, PipelineOpcode.ERASE)
                                        else None
                                    ),
                                ),
                            )
                            for _, request in items
                        ]
                    elif isinstance(exc, asyncio.CancelledError):
                        indices = [index for index, _ in items]
                        group_responses = _cancelled_group(worker, items)
                    else:
                        raise exc
                else:
                    indices, group_responses = task.result()
            for index, response in zip(indices, group_responses, strict=True):
                responses[index] = response

        return [
            response if response is not None else PipelineResponse(PipelineOutcome.FAILED)
            for response in responses
        ]

    async def execute_worker_pipelines(
        self,
        batches: Sequence[Sequence[PipelineRequest]],
        *,
        timeout: float | None = None,
    ) -> list[list[PipelineResponse]]:
        """Run one pre-sharded pipeline vector per Worker (Perl/Erlang parity)."""
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

        def _failed_group(worker: int, group_requests: list[PipelineRequest], error: GlifiError) -> list[PipelineResponse]:
            return [
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

        async def run_group(
            worker: int, group_requests: list[PipelineRequest]
        ) -> tuple[int, list[PipelineResponse]]:
            try:
                return worker, await self._execute_pipeline(
                    group_requests,
                    deadline_override=shared_deadline,
                    routed_worker=worker,
                )
            except GlifiError as error:
                return worker, _failed_group(worker, group_requests, error)
            except asyncio.CancelledError:
                return worker, _failed_group(
                    worker, group_requests, TransportError("request cancelled")
                )

        if not prepared:
            return results
        tasks = [
            asyncio.create_task(run_group(worker, group_requests))
            for worker, group_requests in prepared
        ]
        try:
            await asyncio.wait(tasks)
        except asyncio.CancelledError:
            for task in tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

        for task, (worker, group_requests) in zip(tasks, prepared, strict=True):
            if task.cancelled():
                results[worker] = _failed_group(
                    worker, group_requests, TransportError("request cancelled")
                )
                continue
            exc = task.exception()
            if exc is not None:
                if isinstance(exc, GlifiError):
                    results[worker] = _failed_group(worker, group_requests, exc)
                elif isinstance(exc, asyncio.CancelledError):
                    results[worker] = _failed_group(
                        worker, group_requests, TransportError("request cancelled")
                    )
                else:
                    raise exc
            else:
                worker_id, group_responses = task.result()
                results[worker_id] = group_responses
        return results

    async def close(self) -> None:
        self._healthy = False
        for connection in self._connections:
            async with connection.lock:
                await connection.reset()

    async def __aenter__(self) -> AsyncClient:
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.close()

    async def _next_request_id(self) -> int:
        async with self._request_id_lock:
            current = self._request_id
            self._request_id = 1 if current == 0xFFFF_FFFF_FFFF_FFFF else current + 1
            return current

    async def _open_stream(self) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
        try:
            if self._config.unix_socket_path:
                return await asyncio.wait_for(
                    asyncio.open_unix_connection(self._config.unix_socket_path),
                    timeout=self._config.connect_timeout,
                )
            ssl_context = None
            server_hostname = None
            if self._config.tls:
                ssl_context = build_ssl_context(self._config)
                server_hostname = self._config.server_name or self._config.host
            return await asyncio.wait_for(
                asyncio.open_connection(
                    self._config.host,
                    self._config.port,
                    ssl=ssl_context,
                    server_hostname=server_hostname,
                ),
                timeout=self._config.connect_timeout,
            )
        except (OSError, asyncio.TimeoutError, ssl.SSLError) as error:
            raise Unavailable(f"could not connect to GlifiStore: {error}") from error

    async def _bootstrap(
        self,
        connection: _Connection,
        expected: tuple[int, int, WorkerRouting] | None,
    ) -> tuple[int, int, WorkerRouting]:
        await connection.reset()
        connection.reader, connection.writer = await self._open_stream()
        deadline = _deadline_after(self._config.request_timeout)
        try:
            sock = connection.writer.get_extra_info("socket")
            if sock is not None:
                try:
                    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                except OSError:
                    pass
            init_id = await self._next_request_id()
            response = await self._exchange(
                connection, encode_request(Opcode.INIT, init_id), deadline
            )
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
            bind_id = await self._next_request_id()
            bound = await self._exchange(
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
            await connection.reset()
            raise Unavailable(str(error.error)) from error
        except TransportError as error:
            await connection.reset()
            raise Unavailable(str(error)) from error
        except Exception:
            await connection.reset()
            raise

    async def _ensure_connected(self, connection: _Connection) -> None:
        if connection.writer is None or connection.reader is None:
            await self._bootstrap(
                connection, (self._worker_count, self._routing_epoch, self._routing)
            )

    async def _send(self, connection: _Connection, frame: bytes, deadline: float) -> None:
        assert connection.writer is not None
        try:
            connection.writer.write(frame)
        except OSError as error:
            raise _SendFailure(TransportError(f"request send failed: {error}"), 0) from error
        try:
            await asyncio.wait_for(connection.writer.drain(), timeout=_remaining(deadline))
        except (OSError, asyncio.TimeoutError, TransportError) as error:
            wrapped = (
                error
                if isinstance(error, TransportError)
                else TransportError(f"request send failed: {error}")
            )
            # write() handed the frame to the transport buffer; treat as may-have-sent.
            raise _SendFailure(wrapped, len(frame)) from error
        except asyncio.CancelledError:
            # write() already queued the frame; classify like a mid-drain failure (§6.3).
            raise _SendFailure(TransportError("request send cancelled"), len(frame)) from None

    async def _receive_response(self, connection: _Connection, deadline: float) -> Response:
        assert connection.reader is not None
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
                chunk = await asyncio.wait_for(
                    connection.reader.read(64 * 1024), timeout=_remaining(deadline)
                )
                if not chunk:
                    raise OSError("server closed the connection")
                connection.input.extend(chunk)
            except (OSError, asyncio.TimeoutError, TransportError) as error:
                if isinstance(error, TransportError):
                    raise
                raise TransportError(f"response receive failed: {error}") from error

    async def _exchange(self, connection: _Connection, frame: bytes, deadline: float) -> Response:
        await self._send(connection, frame, deadline)
        return await self._receive_response(connection, deadline)

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

    async def _read(
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
        async with connection.lock:
            if not self._healthy:
                raise Unavailable("client closed before read admission")
            last_error: GlifiError = Unavailable("request was not attempted")
            for _ in range(2):
                request_id: int | None = None
                frame = b""
                exchange_started = False
                try:
                    await self._ensure_connected(connection)
                    request_id = await self._next_request_id()
                    try:
                        frame = encode_request(opcode, request_id, key=key, value=value)
                    except ValueError as error:
                        raise InvalidArgument(str(error)) from error
                    if len(frame) > self._config.maximum_frame_bytes:
                        raise InvalidArgument("request exceeds the configured frame limit")
                    exchange_started = True
                    response = await self._exchange(connection, frame, deadline)
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
                        # Receive-side failure implies the BACKUP/PING frame left the client.
                        last_error = error
                        bytes_sent = len(frame)
                    await connection.reset()
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
                    await connection.reset()
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
                    if not self._healthy:
                        raise
                    last_error = error
                except ProtocolError as error:
                    await connection.reset()
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
                except asyncio.CancelledError:
                    if exchange_started and opcode is Opcode.BACKUP:
                        # Classify before poison so a cancel during close cannot
                        # erase indeterminate / reconcile_first (§6.3).
                        enriched = _enrich(
                            TransportError("request cancelled after send"),
                            operation="backup",
                            request_id=request_id,
                            worker=worker,
                            routing_epoch=self._routing_epoch,
                            bytes_sent=len(frame),
                            mutation_outcome=MutationOutcome.INDETERMINATE,
                        )
                        try:
                            await connection.reset()
                        except asyncio.CancelledError:
                            pass
                        raise enriched from None
                    if exchange_started:
                        try:
                            await connection.reset()
                        except asyncio.CancelledError:
                            pass
                    raise
                except BaseException:
                    if exchange_started:
                        await connection.reset()
                    raise
            raise last_error

    async def _mutate(
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
        async with connection.lock:
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
                exchange_started = False
                request_id = 0
                try:
                    await self._ensure_connected(connection)
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
                    request_id = await self._next_request_id()
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
                    exchange_started = True
                    response = await self._exchange(connection, frame, deadline)
                    self._validate_response(response, request_id, worker)
                except _SendFailure as error:
                    await connection.reset()
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
                    await connection.reset()
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
                except asyncio.CancelledError:
                    if exchange_started:
                        # Classify before poison: reset awaits close and must not
                        # re-raise CancelledError over indeterminate (§6.3 / §6.1).
                        result = MutationResult(
                            MutationOutcome.INDETERMINATE,
                            _enrich(
                                TransportError("request cancelled after send"),
                                operation=op,
                                request_id=request_id,
                                worker=worker,
                                routing_epoch=self._routing_epoch,
                                bytes_sent=len(frame),
                                mutation_outcome=MutationOutcome.INDETERMINATE,
                            ),
                        )
                        try:
                            await connection.reset()
                        except asyncio.CancelledError:
                            pass
                        return result
                    raise
                except BaseException:
                    if exchange_started:
                        await connection.reset()
                    raise
                if response.status is Status.OK:
                    if response.value:
                        await connection.reset()
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
