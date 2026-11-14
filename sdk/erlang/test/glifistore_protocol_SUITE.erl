-module(glifistore_protocol_SUITE).
-compile(nowarn_export_all).
-compile(export_all).

-include_lib("common_test/include/ct.hrl").

all() ->
    [
        request_encoder_matches_fixtures,
        request_decoder_round_trips,
        response_round_trips,
        worker_routing,
        keyed_routing_and_init_identity,
        rejects_noncanonical_reserved
    ].

request_encoder_matches_fixtures(Config) ->
    Expected = frames(fixture(Config, "wire_requests_v2.hex")),
    Encoded = [
        enc(glifistore_protocol:opcode_init(), 1, <<>>, <<>>, 0, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_ping(), 2, <<>>, <<"\0ping\377">>, 0, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_get(), 3, <<"get\0key">>, <<>>, 0, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_put(), 4, <<"put\0key">>, <<16, 32, 255>>, 123456789, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_erase(), 5, <<"erase-key">>, <<>>, 0, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_bind_worker(), 6, <<>>, <<>>, 0, 2),
        enc(glifistore_protocol:opcode_health(), 7, <<>>, <<>>, 0, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_ready(), 8, <<>>, <<>>, 0, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_stats(), 9, <<>>, <<>>, 0, glifistore_protocol:no_worker()),
        enc(glifistore_protocol:opcode_backup(), 10, <<"/tmp/glifistore-backup">>, <<>>, 0,
            glifistore_protocol:no_worker())
    ],
    true = Expected =:= Encoded.

request_decoder_round_trips(Config) ->
    Expected = frames(fixture(Config, "wire_requests_v2.hex")),
    Reencoded = [
        begin
            {ok, Decoded} = glifistore_protocol:decode_request(Frame, glifistore_protocol:max_frame_bytes()),
            {ok, Out} = glifistore_protocol:encode_request(
                maps:get(opcode, Decoded),
                maps:get(request_id, Decoded),
                maps:get(key, Decoded),
                maps:get(value, Decoded),
                maps:get(expire_at_ns, Decoded),
                maps:get(target_worker, Decoded)
            ),
            Out
        end
        || Frame <- Expected
    ],
    true = Expected =:= Reencoded.

response_round_trips(Config) ->
    Expected = frames(fixture(Config, "wire_responses_v2.hex")),
    Reencoded = [
        begin
            {ok, Decoded} = glifistore_protocol:decode_response(Frame, glifistore_protocol:max_frame_bytes()),
            {ok, Out} = glifistore_protocol:encode_response(
                maps:get(status, Decoded),
                maps:get(request_id, Decoded),
                maps:get(value, Decoded),
                maps:get(owner_worker, Decoded),
                maps:get(worker_count, Decoded),
                maps:get(routing_epoch, Decoded)
            ),
            Out
        end
        || Frame <- Expected
    ],
    true = Expected =:= Reencoded.

worker_routing(_Config) ->
    Key = <<"session\00042">>,
    {ok, Worker} = glifistore_protocol:worker_for(Key, 4),
    true = Worker =:= glifistore_protocol:fnv1a64(Key) rem 4,
    {ok, Worker2} = glifistore_protocol:worker_for(Key, 4),
    true = Worker =:= Worker2.

%% Paper vectors from Aumasson & Bernstein, SipHash: a fast short-input PRF.
keyed_routing_and_init_identity(_Config) ->
    K0 = 16#0706050403020100,
    K1 = 16#0f0e0d0c0b0a0908,
    true = glifistore_protocol:siphash24(<<>>, K0, K1) =:= 16#726fdb47dd0e0e31,
    true = glifistore_protocol:siphash24(<<0, 1, 2>>, K0, K1) =:= 16#85676696d7fb7e2d,
    Keyed = #{
        algorithm => glifistore_protocol:routing_alg_siphash24_v1(),
        seed => 16#1111222233334444
    },
    {ok, Digest} = glifistore_protocol:hash_key_routing(<<"tenant-a/orders/1">>, Keyed),
    true = Digest =:= 16#712fcec57ac84546,
    {ok, 6} = glifistore_protocol:worker_for(<<"tenant-a/orders/1">>, 8, Keyed),
    {ok, Fnv} = glifistore_protocol:hash_key_routing(<<"tenant-a/orders/1">>, glifistore_protocol:default_routing()),
    true = Digest =/= Fnv,
    {ok, Plain} = glifistore_protocol:encode_init_identity(glifistore_protocol:default_routing()),
    true = Plain =:= glifistore_protocol:identity(),
    {ok, DecodedPlain} = glifistore_protocol:decode_init_identity(Plain),
    true = DecodedPlain =:= glifistore_protocol:default_routing(),
    ExtendedSeed = 16#ABCDEF0123456789,
    {ok, Extended} = glifistore_protocol:encode_init_identity(#{
        algorithm => glifistore_protocol:routing_alg_siphash24_v1(),
        seed => ExtendedSeed
    }),
    true = byte_size(Extended) =:= byte_size(glifistore_protocol:identity()) + 1 + 4 + 8,
    {ok, Decoded} = glifistore_protocol:decode_init_identity(Extended),
    true = Decoded =:= #{
        algorithm => glifistore_protocol:routing_alg_siphash24_v1(),
        seed => ExtendedSeed
    },
    {error, _} = glifistore_protocol:decode_init_identity(<<"GlifiStore/2", 0, "bad">>),
    {error, _} = glifistore_protocol:decode_init_identity(<<"GlifiStore/3">>).

rejects_noncanonical_reserved(_Config) ->
    {ok, Frame} = glifistore_protocol:encode_request(
        glifistore_protocol:opcode_ping(), 1, <<>>, <<"x">>, 0, glifistore_protocol:no_worker()
    ),
    <<Head:36/binary, _:32/little, Tail/binary>> = Frame,
    Mutated = <<Head/binary, 1:32/little, Tail/binary>>,
    {error, _} = glifistore_protocol:decode_request(Mutated, glifistore_protocol:max_frame_bytes()).

enc(Opcode, RequestId, Key, Value, Expire, Target) ->
    {ok, Frame} = glifistore_protocol:encode_request(Opcode, RequestId, Key, Value, Expire, Target),
    Frame.

fixture(_Config, Name) ->
    SuiteDir = filename:dirname(?FILE),
    Path = filename:join([SuiteDir, "fixtures", Name]),
    {ok, Bin} = file:read_file(Path),
    parse_hex(Bin).

parse_hex(Bin) ->
    Tokens = [X || X <- string:tokens(binary_to_list(Bin), "\n\r\t "), X =/= ""],
    binary:decode_hex(list_to_binary(lists:concat(Tokens))).

frames(Corpus) ->
    frames(Corpus, []).

frames(<<>>, Acc) ->
    lists:reverse(Acc);
frames(Corpus, Acc) ->
    <<Size:32/little, _/binary>> = Corpus,
    <<Frame:Size/binary, Rest/binary>> = Corpus,
    frames(Rest, [Frame | Acc]).
