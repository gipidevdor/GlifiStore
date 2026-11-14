-module(glifistore_protocol).
-export([
    version/0,
    request_header_bytes/0,
    response_header_bytes/0,
    max_frame_bytes/0,
    no_worker/0,
    identity/0,
    opcode_init/0,
    opcode_ping/0,
    opcode_get/0,
    opcode_put/0,
    opcode_erase/0,
    opcode_bind_worker/0,
    opcode_health/0,
    opcode_ready/0,
    opcode_stats/0,
    opcode_backup/0,
    status_ok/0,
    status_invalid_request/0,
    status_unsupported/0,
    status_internal_error/0,
    status_not_found/0,
    status_overloaded/0,
    status_wrong_owner/0,
    status_not_bound/0,
    status_permission_denied/0,
    request_frame_size/2,
    encode_request/6,
    decode_request/2,
    encode_response/6,
    decode_response/2,
    fnv1a64/1,
    siphash24/3,
    hash_key_routing/2,
    encode_init_identity/1,
    decode_init_identity/1,
    worker_for/2,
    worker_for/3,
    routing_alg_fnv1a64_v1/0,
    routing_alg_siphash24_v1/0,
    default_routing/0
]).

-define(VERSION, 2).
-define(REQUEST_HEADER_BYTES, 40).
-define(RESPONSE_HEADER_BYTES, 40).
-define(MAX_FRAME_BYTES, 2097152).
-define(NO_WORKER, 16#FFFFFFFF).

-define(OP_INIT, 1).
-define(OP_PING, 2).
-define(OP_GET, 3).
-define(OP_PUT, 4).
-define(OP_ERASE, 5).
-define(OP_BIND, 6).
-define(OP_HEALTH, 7).
-define(OP_READY, 8).
-define(OP_STATS, 9).
-define(OP_BACKUP, 10).

-define(ST_OK, 0).
-define(ST_INVALID, 1).
-define(ST_UNSUPPORTED, 2).
-define(ST_INTERNAL, 3).
-define(ST_NOT_FOUND, 4).
-define(ST_OVERLOADED, 5).
-define(ST_WRONG_OWNER, 6).
-define(ST_NOT_BOUND, 7).
-define(ST_PERMISSION_DENIED, 8).

-define(FNV_OFFSET, 14695981039346656037).
-define(FNV_PRIME, 1099511628211).
-define(U64_MASK, 16#FFFFFFFFFFFFFFFF).
-define(ROUTING_ALG_FNV1A64_V1, 1).
-define(ROUTING_ALG_SIPHASH24_V1, 2).
-define(WORKER_ROUTING_SIP_KEY1_XOR, 16#6a09e667f3bcc909).
-define(INIT_IDENTITY_EXTENDED_BYTES, (byte_size(<<"GlifiStore/2">>) + 1 + 4 + 8)).
-define(SIP_C0, 16#736f6d6570736575).
-define(SIP_C1, 16#646f72616e646f6d).
-define(SIP_C2, 16#6c7967656e657261).
-define(SIP_C3, 16#7465646279746573).

version() -> ?VERSION.
request_header_bytes() -> ?REQUEST_HEADER_BYTES.
response_header_bytes() -> ?RESPONSE_HEADER_BYTES.
max_frame_bytes() -> ?MAX_FRAME_BYTES.
no_worker() -> ?NO_WORKER.
identity() -> <<"GlifiStore/2">>.
opcode_init() -> ?OP_INIT.
opcode_ping() -> ?OP_PING.
opcode_get() -> ?OP_GET.
opcode_put() -> ?OP_PUT.
opcode_erase() -> ?OP_ERASE.
opcode_bind_worker() -> ?OP_BIND.
opcode_health() -> ?OP_HEALTH.
opcode_ready() -> ?OP_READY.
opcode_stats() -> ?OP_STATS.
opcode_backup() -> ?OP_BACKUP.
status_ok() -> ?ST_OK.
status_invalid_request() -> ?ST_INVALID.
status_unsupported() -> ?ST_UNSUPPORTED.
status_internal_error() -> ?ST_INTERNAL.
status_not_found() -> ?ST_NOT_FOUND.
status_overloaded() -> ?ST_OVERLOADED.
status_wrong_owner() -> ?ST_WRONG_OWNER.
status_not_bound() -> ?ST_NOT_BOUND.
status_permission_denied() -> ?ST_PERMISSION_DENIED.

-spec request_frame_size(binary(), binary()) -> non_neg_integer().
request_frame_size(Key, Value) ->
    ?REQUEST_HEADER_BYTES + byte_size(Key) + byte_size(Value).

-spec encode_request(
    non_neg_integer(),
    non_neg_integer(),
    binary(),
    binary(),
    non_neg_integer(),
    non_neg_integer()
) -> {ok, binary()} | {error, {invalid_argument, binary()}}.
encode_request(Opcode, RequestId, Key, Value, ExpireAtNs, TargetWorker) ->
    KeyBin = ensure_binary(Key),
    ValueBin = ensure_binary(Value),
    case validate_request_fields(Opcode, KeyBin, ValueBin, ExpireAtNs, TargetWorker) of
        ok ->
            case u64(RequestId, <<"request_id">>) of
                ok ->
                    case u64(ExpireAtNs, <<"expire_at_ns">>) of
                        ok ->
                            case u32(TargetWorker, <<"target_worker">>) of
                                ok ->
                                    FrameSize = request_frame_size(KeyBin, ValueBin),
                                    case FrameSize =< ?MAX_FRAME_BYTES of
                                        true ->
                                            Header = <<
                                                FrameSize:32/little,
                                                ?VERSION:16/little,
                                                Opcode:8,
                                                0:8,
                                                RequestId:64/little,
                                                (byte_size(KeyBin)):32/little,
                                                (byte_size(ValueBin)):32/little,
                                                ExpireAtNs:64/little,
                                                TargetWorker:32/little,
                                                0:32/little
                                            >>,
                                            {ok, <<Header/binary, KeyBin/binary, ValueBin/binary>>};
                                        false ->
                                            {error,
                                                {invalid_argument,
                                                    <<"request exceeds the protocol frame limit">>}}
                                    end;
                                {error, _} = Err ->
                                    Err
                            end;
                        {error, _} = Err ->
                            Err
                    end;
                {error, _} = Err ->
                    Err
            end;
        {error, _} = Err ->
            Err
    end.

-spec decode_request(binary(), non_neg_integer()) ->
    {ok, map()} | {error, {invalid_argument, binary()}}.
decode_request(Frame, MaximumFrameBytes) ->
    Max = case MaximumFrameBytes of
        0 -> ?MAX_FRAME_BYTES;
        N -> N
    end,
    case Frame of
        <<FrameSize:32/little, Rest/binary>> when byte_size(Frame) >= ?REQUEST_HEADER_BYTES ->
            case FrameSize =:= byte_size(Frame) andalso FrameSize =< Max of
                true ->
                    decode_request_body(FrameSize, Rest, Frame);
                false ->
                    {error, {invalid_argument, <<"request frame extent is invalid">>}}
            end;
        _ when byte_size(Frame) < ?REQUEST_HEADER_BYTES ->
            {error, {invalid_argument, <<"request is shorter than its header">>}};
        _ ->
            {error, {invalid_argument, <<"request frame extent is invalid">>}}
    end.

decode_request_body(_FrameSize, Rest, Frame) ->
    case Rest of
        <<Version:16/little, Opcode:8, 0:8, RequestId:64/little,
          KeySize:32/little, ValueSize:32/little, ExpireAtNs:64/little,
          TargetWorker:32/little, 0:32/little, Tail/binary>> ->
            case Version =:= ?VERSION of
                true ->
                    case valid_opcode(Opcode) of
                        true ->
                            PayloadSize = KeySize + ValueSize,
                            case ?REQUEST_HEADER_BYTES + PayloadSize =:= byte_size(Frame) of
                                true ->
                                    <<Key:KeySize/binary, Value:ValueSize/binary, _/binary>> = Tail,
                                    case validate_request_fields(Opcode, Key, Value, ExpireAtNs, TargetWorker) of
                                        ok ->
                                            {ok, #{
                                                opcode => Opcode,
                                                request_id => RequestId,
                                                expire_at_ns => ExpireAtNs,
                                                target_worker => TargetWorker,
                                                key => Key,
                                                value => Value
                                            }};
                                        {error, _} = Err ->
                                            Err
                                    end;
                                false ->
                                    {error, {invalid_argument, <<"request payload extent is invalid">>}}
                            end;
                        false ->
                            {error, {invalid_argument, <<"request opcode is unknown">>}}
                    end;
                false ->
                    {error, {invalid_argument, <<"request protocol version is unsupported">>}}
            end;
        <<Version:16/little, _/binary>> ->
            case Version =:= ?VERSION of
                false ->
                    {error, {invalid_argument, <<"request protocol version is unsupported">>}};
                true ->
                    {error, {invalid_argument, <<"request canonical fields are invalid">>}}
            end;
        _ ->
            {error, {invalid_argument, <<"request canonical fields are invalid">>}}
    end.

-spec encode_response(
    non_neg_integer(),
    non_neg_integer(),
    binary(),
    non_neg_integer(),
    non_neg_integer(),
    non_neg_integer()
) -> {ok, binary()} | {error, {invalid_argument, binary()}}.
encode_response(Status, RequestId, Value, OwnerWorker, WorkerCount, RoutingEpoch) ->
    case valid_status(Status) of
        true ->
            ValueBin = ensure_binary(Value),
            case u64(RequestId, <<"request_id">>) of
                ok ->
                    case u32(OwnerWorker, <<"owner_worker">>) of
                        ok ->
                            case u32(WorkerCount, <<"worker_count">>) of
                                ok ->
                                    case u64(RoutingEpoch, <<"routing_epoch">>) of
                                        ok ->
                                            FrameSize = ?RESPONSE_HEADER_BYTES + byte_size(ValueBin),
                                            case FrameSize =< ?MAX_FRAME_BYTES of
                                                true ->
                                                    Header = <<
                                                        FrameSize:32/little,
                                                        ?VERSION:16/little,
                                                        Status:16/little,
                                                        RequestId:64/little,
                                                        (byte_size(ValueBin)):32/little,
                                                        OwnerWorker:32/little,
                                                        WorkerCount:32/little,
                                                        0:32/little,
                                                        RoutingEpoch:64/little
                                                    >>,
                                                    {ok, <<Header/binary, ValueBin/binary>>};
                                                false ->
                                                    {error,
                                                        {invalid_argument,
                                                            <<"response exceeds the protocol frame limit">>}}
                                            end;
                                        {error, _} = Err ->
                                            Err
                                    end;
                                {error, _} = Err ->
                                    Err
                            end;
                        {error, _} = Err ->
                            Err
                    end;
                {error, _} = Err ->
                    Err
            end;
        false ->
            {error, {invalid_argument, <<"status is not defined by wire protocol v2">>}}
    end.

-spec decode_response(binary(), non_neg_integer()) ->
    {ok, map()} | {error, {invalid_argument, binary()}}.
decode_response(Frame, MaximumFrameBytes) ->
    Max = case MaximumFrameBytes of
        0 -> ?MAX_FRAME_BYTES;
        N -> N
    end,
    case Frame of
        <<FrameSize:32/little, Rest/binary>> when byte_size(Frame) >= ?RESPONSE_HEADER_BYTES ->
            case FrameSize =:= byte_size(Frame) andalso FrameSize =< Max of
                true ->
                    decode_response_body(FrameSize, Rest);
                false ->
                    {error, {invalid_argument, <<"response frame extent is invalid">>}}
            end;
        _ when byte_size(Frame) < ?RESPONSE_HEADER_BYTES ->
            {error, {invalid_argument, <<"response is shorter than its header">>}};
        _ ->
            {error, {invalid_argument, <<"response frame extent is invalid">>}}
    end.

decode_response_body(FrameSize, Rest) ->
    case Rest of
        <<Version:16/little, Status:16/little, RequestId:64/little, ValueSize:32/little,
          OwnerWorker:32/little, WorkerCount:32/little, 0:32/little, RoutingEpoch:64/little,
          Tail/binary>> ->
            case Version =:= ?VERSION of
                true ->
                    case valid_status(Status) of
                        true ->
                            case ?RESPONSE_HEADER_BYTES + ValueSize =:= FrameSize of
                                true ->
                                    <<Value:ValueSize/binary, _/binary>> = Tail,
                                    {ok, #{
                                        status => Status,
                                        request_id => RequestId,
                                        owner_worker => OwnerWorker,
                                        worker_count => WorkerCount,
                                        routing_epoch => RoutingEpoch,
                                        value => Value
                                    }};
                                false ->
                                    {error, {invalid_argument, <<"response value extent is invalid">>}}
                            end;
                        false ->
                            {error, {invalid_argument, <<"response status is unknown">>}}
                    end;
                false ->
                    {error, {invalid_argument, <<"response protocol version is unsupported">>}}
            end;
        <<Version:16/little, _/binary>> ->
            case Version =:= ?VERSION of
                false ->
                    {error, {invalid_argument, <<"response protocol version is unsupported">>}};
                true ->
                    {error, {invalid_argument, <<"response reserved field is noncanonical">>}}
            end;
        _ ->
            {error, {invalid_argument, <<"response reserved field is noncanonical">>}}
    end.

-spec fnv1a64(binary()) -> non_neg_integer().
fnv1a64(Key) ->
    fnv1a64(Key, ?FNV_OFFSET).

routing_alg_fnv1a64_v1() -> ?ROUTING_ALG_FNV1A64_V1.
routing_alg_siphash24_v1() -> ?ROUTING_ALG_SIPHASH24_V1.
default_routing() -> #{algorithm => ?ROUTING_ALG_FNV1A64_V1, seed => 0}.

%% SipHash-2-4 (Aumasson/Bernstein). Independent implementation for siphash24-v1 routing;
%% see repository THIRD_PARTY_NOTICES.md. Not a copy of a third-party source tree.
-spec siphash24(binary(), non_neg_integer(), non_neg_integer()) -> non_neg_integer().
siphash24(Key0, K0, K1) ->
    Key = ensure_binary(Key0),
    V0 = (K0 bxor ?SIP_C0) band ?U64_MASK,
    V1 = (K1 bxor ?SIP_C1) band ?U64_MASK,
    V2 = (K0 bxor ?SIP_C2) band ?U64_MASK,
    V3 = (K1 bxor ?SIP_C3) band ?U64_MASK,
    siphash24_body(Key, V0, V1, V2, V3, byte_size(Key), 0).

siphash24_body(Key, V0, V1, V2, V3, Length, Offset) when Offset + 8 =< Length ->
    <<_:Offset/binary, Message:64/little, _/binary>> = Key,
    V3a = V3 bxor Message,
    {A0, A1, A2, A3} = sipround(V0, V1, V2, V3a),
    {B0, B1, B2, B3} = sipround(A0, A1, A2, A3),
    siphash24_body(Key, B0 bxor Message, B1, B2, B3, Length, Offset + 8);
siphash24_body(Key, V0, V1, V2, V3, Length, Offset) ->
    Message0 = (Length bsl 56) band ?U64_MASK,
    Message = siphash_tail(Key, Offset, Length, 0, Message0),
    V3a = V3 bxor Message,
    {A0, A1, A2, A3} = sipround(V0, V1, V2, V3a),
    {B0, B1, B2, B3} = sipround(A0, A1, A2, A3),
    V0b = B0 bxor Message,
    V2b = B2 bxor 16#FF,
    {C0, C1, C2, C3} = sipround(V0b, B1, V2b, B3),
    {D0, D1, D2, D3} = sipround(C0, C1, C2, C3),
    {E0, E1, E2, E3} = sipround(D0, D1, D2, D3),
    {F0, F1, F2, F3} = sipround(E0, E1, E2, E3),
    (F0 bxor F1 bxor F2 bxor F3) band ?U64_MASK.

siphash_tail(_Key, Offset, Length, _I, Acc) when Offset >= Length ->
    Acc;
siphash_tail(Key, Offset, Length, I, Acc) ->
    <<_:Offset/binary, Byte, _/binary>> = Key,
    Acc1 = Acc bor ((Byte bsl (8 * I)) band ?U64_MASK),
    siphash_tail(Key, Offset + 1, Length, I + 1, Acc1 band ?U64_MASK).

sipround(V0, V1, V2, V3) ->
    V0a = (V0 + V1) band ?U64_MASK,
    V1a = rotl64(V1, 13),
    V1b = V1a bxor V0a,
    V0b = rotl64(V0a, 32),
    V2a = (V2 + V3) band ?U64_MASK,
    V3a = rotl64(V3, 16),
    V3b = V3a bxor V2a,
    V0c = (V0b + V3b) band ?U64_MASK,
    V3c = rotl64(V3b, 21),
    V3d = V3c bxor V0c,
    V2b = (V2a + V1b) band ?U64_MASK,
    V1c = rotl64(V1b, 17),
    V1d = V1c bxor V2b,
    V2c = rotl64(V2b, 32),
    {V0c, V1d, V2c, V3d}.

rotl64(Value, Shift) ->
    (((Value bsl Shift) band ?U64_MASK) bor (Value bsr (64 - Shift))) band ?U64_MASK.

validate_routing(#{algorithm := ?ROUTING_ALG_FNV1A64_V1, seed := 0}) ->
    ok;
validate_routing(#{algorithm := ?ROUTING_ALG_FNV1A64_V1, seed := _}) ->
    {error, {invalid_argument, <<"fnv1a64-v1 Worker routing requires a zero hash seed">>}};
validate_routing(#{algorithm := ?ROUTING_ALG_SIPHASH24_V1, seed := _}) ->
    ok;
validate_routing(_) ->
    {error, {invalid_argument, <<"unsupported Worker routing algorithm">>}}.

hash_key_routing(Key, Routing0) ->
    Routing = normalize_routing(Routing0),
    case validate_routing(Routing) of
        ok ->
            case maps:get(algorithm, Routing) of
                ?ROUTING_ALG_SIPHASH24_V1 ->
                    Seed = maps:get(seed, Routing),
                    {ok, siphash24(ensure_binary(Key), Seed, Seed bxor ?WORKER_ROUTING_SIP_KEY1_XOR)};
                _ ->
                    {ok, fnv1a64(ensure_binary(Key))}
            end;
        Error ->
            Error
    end.

normalize_routing(undefined) ->
    default_routing();
normalize_routing(Routing) when is_map(Routing) ->
    #{
        algorithm => maps:get(algorithm, Routing, ?ROUTING_ALG_FNV1A64_V1),
        seed => maps:get(seed, Routing, 0)
    }.

encode_init_identity(Routing0) ->
    Routing = normalize_routing(Routing0),
    case validate_routing(Routing) of
        ok ->
            case maps:get(algorithm, Routing) of
                ?ROUTING_ALG_SIPHASH24_V1 ->
                    Algo = maps:get(algorithm, Routing),
                    Seed = maps:get(seed, Routing),
                    {ok, <<(identity())/binary, 0, Algo:32/little, Seed:64/little>>};
                _ ->
                    {ok, identity()}
            end;
        Error ->
            Error
    end.

decode_init_identity(Value0) ->
    Value = ensure_binary(Value0),
    Ident = identity(),
    IdentSize = byte_size(Ident),
    case Value of
        Ident ->
            {ok, default_routing()};
        _ when byte_size(Value) =/= ?INIT_IDENTITY_EXTENDED_BYTES ->
            {error, {invalid_argument, <<"server INIT identity value has unexpected length">>}};
        <<Prefix:IdentSize/binary, 0, Algo:32/little, Seed:64/little>> ->
            case Prefix =:= Ident of
                false ->
                    {error, {invalid_argument, <<"server INIT identity prefix is invalid">>}};
                true ->
                    State = #{algorithm => Algo, seed => Seed},
                    case validate_routing(State) of
                        ok when Algo =:= ?ROUTING_ALG_SIPHASH24_V1 ->
                            {ok, State};
                        ok ->
                            {error, {invalid_argument, <<"server INIT extended identity must use siphash24-v1 routing">>}};
                        Error ->
                            Error
                    end
            end;
        _ ->
            {error, {invalid_argument, <<"server INIT identity prefix is invalid">>}}
    end.

-spec worker_for(binary(), pos_integer()) ->
    {ok, non_neg_integer()} | {error, {invalid_argument, binary()}}.
worker_for(Key, WorkerCount) ->
    worker_for(Key, WorkerCount, default_routing()).

-spec worker_for(binary(), pos_integer(), map()) ->
    {ok, non_neg_integer()} | {error, {invalid_argument, binary()}}.
worker_for(Key, WorkerCount, Routing) when WorkerCount > 0 ->
    case hash_key_routing(Key, Routing) of
        {ok, Digest} -> {ok, Digest rem WorkerCount};
        Error -> Error
    end;
worker_for(_Key, _WorkerCount, _Routing) ->
    {error, {invalid_argument, <<"worker_count must be positive">>}}.

ensure_binary(undefined) -> <<>>;
ensure_binary(B) when is_binary(B) -> B;
ensure_binary(_) -> erlang:error(badarg).

fnv1a64(<<Byte, Rest/binary>>, Value0) ->
    Value1 = (Value0 bxor Byte) * ?FNV_PRIME,
    fnv1a64(Rest, Value1 band ?U64_MASK);
fnv1a64(<<>>, Value) ->
    Value.

u64(V, _Field) when is_integer(V), V >= 0, V =< ?U64_MASK ->
    ok;
u64(_, Field) ->
    {error, {invalid_argument, iolist_to_binary([Field, <<" is outside unsigned 64-bit range">>])}}.

u32(V, _Field) when is_integer(V), V >= 0, V =< 16#FFFFFFFF ->
    ok;
u32(_, Field) ->
    {error, {invalid_argument, iolist_to_binary([Field, <<" is outside unsigned 32-bit range">>])}}.

valid_opcode(Opcode) when Opcode >= ?OP_INIT, Opcode =< ?OP_BACKUP ->
    true;
valid_opcode(_) ->
    false.

valid_status(Status) when Status >= ?ST_OK, Status =< ?ST_PERMISSION_DENIED ->
    true;
valid_status(_) ->
    false.

validate_request_fields(Opcode, Key, Value, ExpireAtNs, TargetWorker) ->
    case Opcode of
        ?OP_INIT ->
            reject_if(
                Key =:= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0
                    andalso TargetWorker =:= ?NO_WORKER,
                <<"INIT request cannot carry key, value, expiry, or target_worker">>
            );
        ?OP_PING ->
            reject_if(
                Key =:= <<>> andalso ExpireAtNs =:= 0 andalso TargetWorker =:= ?NO_WORKER,
                <<"PING request cannot carry key, expiry, or target_worker">>
            );
        ?OP_GET ->
            reject_if(
                Key =/= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0
                    andalso TargetWorker =:= ?NO_WORKER,
                <<"GET request requires a key and cannot carry value, expiry, or target_worker">>
            );
        ?OP_PUT ->
            reject_if(
                Key =/= <<>> andalso TargetWorker =:= ?NO_WORKER,
                <<"PUT request requires a key and cannot carry target_worker">>
            );
        ?OP_ERASE ->
            reject_if(
                Key =/= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0
                    andalso TargetWorker =:= ?NO_WORKER,
                <<"ERASE request requires a key and cannot carry value, expiry, or target_worker">>
            );
        ?OP_BIND ->
            case Key =:= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0 of
                true ->
                    case TargetWorker =:= ?NO_WORKER of
                        true ->
                            {error,
                                {invalid_argument,
                                    <<"BIND_WORKER request requires an explicit target_worker">>}};
                        false ->
                            ok
                    end;
                false ->
                    {error,
                        {invalid_argument,
                            <<"BIND_WORKER request cannot carry key, value, or expiry">>}}
            end;
        ?OP_HEALTH ->
            reject_if(
                Key =:= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0
                    andalso TargetWorker =:= ?NO_WORKER,
                <<"lifecycle probe cannot carry key, value, expiry, or target_worker">>
            );
        ?OP_READY ->
            reject_if(
                Key =:= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0
                    andalso TargetWorker =:= ?NO_WORKER,
                <<"lifecycle probe cannot carry key, value, expiry, or target_worker">>
            );
        ?OP_STATS ->
            reject_if(
                Key =:= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0
                    andalso TargetWorker =:= ?NO_WORKER,
                <<"lifecycle probe cannot carry key, value, expiry, or target_worker">>
            );
        ?OP_BACKUP ->
            reject_if(
                Key =/= <<>> andalso Value =:= <<>> andalso ExpireAtNs =:= 0
                    andalso TargetWorker =:= ?NO_WORKER,
                <<"BACKUP requires a destination path key and no value, expiry, or target_worker">>
            );
        _ ->
            {error, {invalid_argument, <<"opcode is not defined by wire protocol v2">>}}
    end.

reject_if(true, _Msg) ->
    ok;
reject_if(false, Msg) ->
    {error, {invalid_argument, Msg}}.
