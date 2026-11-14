#!/usr/bin/env escript
%%! -noshell -noinput
-mode(compile).

main(Args) ->
    add_beam_path(),
    run(parse_args(maps:new(), Args)).

add_beam_path() ->
    case os:getenv("GLIFISTORE_INTEROP_USE_INSTALLED") of
        "1" ->
            {module, glifistore_client} = code:ensure_loaded(glifistore_client),
            Loaded = filename:absname(code:which(glifistore_client)),
            SourceRoot = filename:absname(os:getenv("GLIFISTORE_SOURCE_ROOT")),
            false = lists:prefix(SourceRoot ++ "/", Loaded);
        _ ->
            Script = escript:script_name(),
            Root = filename:dirname(filename:dirname(Script)),
            Beam = filename:join([Root, "_build", "default", "lib", "glifistore", "ebin"]),
            true = code:add_patha(Beam)
    end.

parse_args(Opts, []) ->
    maps:merge(#{command => undefined, tls => false}, Opts);
parse_args(Opts, ["--host", Host | Rest]) ->
    parse_args(Opts#{host => Host}, Rest);
parse_args(Opts, ["--port", Port | Rest]) ->
    parse_args(Opts#{port => list_to_integer(Port)}, Rest);
parse_args(Opts, ["--key-hex", Hex | Rest]) ->
    parse_args(Opts#{key_hex => Hex}, Rest);
parse_args(Opts, ["--value-hex", Hex | Rest]) ->
    parse_args(Opts#{value_hex => Hex}, Rest);
parse_args(Opts, ["--expire-at-ns", Expire | Rest]) ->
    parse_args(Opts#{expire_at_ns => list_to_integer(Expire)}, Rest);
parse_args(Opts, ["--burst", Burst | Rest]) ->
    parse_args(Opts#{burst => list_to_integer(Burst)}, Rest);
parse_args(Opts, ["--tls" | Rest]) ->
    parse_args(Opts#{tls => true}, Rest);
parse_args(Opts, ["--tls-ca", Path | Rest]) ->
    parse_args(Opts#{tls_ca => Path}, Rest);
parse_args(Opts, ["--tls-cert", Path | Rest]) ->
    parse_args(Opts#{tls_cert => Path}, Rest);
parse_args(Opts, ["--tls-key", Path | Rest]) ->
    parse_args(Opts#{tls_key => Path}, Rest);
parse_args(Opts, ["--server-name", Name | Rest]) ->
    parse_args(Opts#{server_name => Name}, Rest);
parse_args(Opts, ["--insecure-skip-verify" | Rest]) ->
    parse_args(Opts#{insecure_skip_verify => true}, Rest);
parse_args(Opts, [Command | Rest]) ->
    parse_args(Opts#{command => Command}, Rest).

run(#{command := undefined}) ->
    usage(),
    halt(2);
run(#{command := Command} = Opts) ->
    Host = maps:get(host, Opts, "127.0.0.1"),
    Port = maps:get(port, Opts, undefined),
    case Port of
        undefined ->
            usage(),
            halt(2);
        _ ->
            ok
    end,
    Key = parse_hex(maps:get(key_hex, Opts, "")),
    Value = parse_hex(maps:get(value_hex, Opts, "")),
    Expire = maps:get(expire_at_ns, Opts, 0),
    Burst = maps:get(burst, Opts, 32),
    case Burst >= 1 andalso Burst =< 10000 of
        true -> ok;
        false -> usage(), halt(2)
    end,
    Config0 = #{host => Host, port => Port},
    Config1 = maybe_tls(Config0, Opts),
    Config = glifistore_util:merge_config(Config1),
    case glifistore_client:connect(Config) of
        {ok, Client} ->
            try
                dispatch(Client, Command, Key, Value, Expire, Config, Burst)
            after
                glifistore_client:close(Client)
            end;
        {error, Err} ->
            fail(Err)
    end.

maybe_tls(Config, #{tls := true} = Opts) ->
    TLS0 = #{enable => true},
    TLS1 = case maps:get(tls_ca, Opts, undefined) of
               undefined -> TLS0;
               CA -> TLS0#{ca_file => CA}
           end,
    TLS2 = case maps:get(tls_cert, Opts, undefined) of
               undefined -> TLS1;
               Cert -> TLS1#{cert_file => Cert}
           end,
    TLS3 = case maps:get(tls_key, Opts, undefined) of
               undefined -> TLS2;
               Key -> TLS2#{key_file => Key}
           end,
    TLS4 = case maps:get(server_name, Opts, undefined) of
               undefined -> TLS3;
               Name -> TLS3#{server_name => Name}
           end,
    TLS5 = case maps:get(insecure_skip_verify, Opts, false) of
               true -> TLS4#{insecure_skip_verify => true};
               false -> TLS4
           end,
    Config#{tls => TLS5};
maybe_tls(Config, _Opts) ->
    Config.

dispatch(Client, "put", Key, Value, Expire, _Config, _Burst) ->
    case glifistore_client:put(Client, Key, Value, #{expire_at_ns => Expire}) of
        #{outcome := committed} -> halt(0);
        Other -> fail(Other)
    end;
dispatch(Client, "get", Key, _Value, _Expire, _Config, _Burst) ->
    case glifistore_client:get(Client, Key) of
        {ok, Payload} ->
            io:format("~s", [to_hex(Payload)]),
            halt(0);
        {error, Err} ->
            fail(Err)
    end;
dispatch(Client, "erase", Key, _Value, _Expire, _Config, _Burst) ->
    case glifistore_client:erase(Client, Key) of
        #{outcome := committed} -> halt(0);
        Other -> fail(Other)
    end;
dispatch(Client, "pipeline-put-get", Key, Value, _Expire, _Config, _Burst) ->
    case glifistore_client:execute_pipeline(Client, [
        #{opcode => put, key => Key, value => Value},
        #{opcode => get, key => Key}
    ]) of
        {ok, [#{outcome := succeeded}, #{outcome := succeeded, value := Value}]} ->
            io:format("~s", [to_hex(Value)]),
            halt(0);
        Other ->
            fail(Other)
    end;
dispatch(Client, "worker-pipelines-put-get", Key, Value, _Expire, _Config, _Burst) ->
    WorkerCount = glifistore_client:worker_count(Client),
    true = is_integer(WorkerCount) andalso WorkerCount > 0,
    Keys = discover_owned_keys(Client, Key, WorkerCount, 0, maps:new()),
    {Wave, Values} = build_worker_wave(Keys, Value, WorkerCount, 0, [], []),
    case glifistore_client:execute_worker_pipelines(Client, Wave) of
        {ok, All} when length(All) =:= WorkerCount ->
            ok = verify_worker_results(All, Values, 0),
            io:format("ok~n"),
            halt(0);
        Other ->
            fail(Other)
    end;
dispatch(Client, "expect-not-found", Key, _Value, _Expire, _Config, _Burst) ->
    case glifistore_client:get(Client, Key) of
        {error, Err} ->
            case maps:get(category, Err) =:= not_found andalso maps:get(retryability, Err) =:= new_attempt of
                true -> halt(0);
                false -> fail(Err)
            end;
        {ok, _} ->
            io:format(standard_error, "GET unexpectedly found the key~n", []),
            halt(1)
    end;
dispatch(Client, "expect-permission-denied", Key, Value, Expire, _Config, _Burst) ->
    case glifistore_client:put(Client, Key, Value, #{expire_at_ns => Expire}) of
        #{outcome := rejected,
          error := #{category := permission_denied, retryability := never}} ->
            halt(0);
        Other ->
            fail(Other)
    end;
dispatch(Client, "burst-expect-overloaded", Key, Value, Expire, _Config, Burst) ->
    case burst_expect_overloaded(Client, Key, Value, Expire, Burst) of
        ok -> halt(0);
        Error -> fail(Error)
    end;
dispatch(Client, "expect-frame-limit", _Key, _Value, _Expire, Config, _Burst) ->
    Oversize = binary:copy(<<165>>, maps:get(maximum_frame_bytes, Config)),
    case glifistore_client:put(Client, <<"limit">>, Oversize) of
        #{outcome := rejected, error := Err} ->
            case maps:get(category, Err) =:= invalid_argument
                andalso maps:get(bytes_sent, Err, 0) =:= 0
                andalso maps:get(retryability, Err) =:= never
            of
                true -> halt(0);
                false -> fail(Err)
            end;
        Other ->
            fail(Other)
    end;
dispatch(_Client, Command, _K, _V, _E, _C, _Burst) ->
    io:format(standard_error, "unknown command: ~s~n", [Command]),
    halt(2).

burst_expect_overloaded(_Client, _Key, _Value, _Expire, 0) ->
    {error, overloaded_not_observed};
burst_expect_overloaded(Client, Key, Value, Expire, Remaining) ->
    case glifistore_client:put(Client, Key, Value, #{expire_at_ns => Expire}) of
        #{outcome := rejected,
          error := #{category := overloaded, retryability := never}} ->
            ok;
        #{outcome := committed} ->
            burst_expect_overloaded(Client, Key, Value, Expire, Remaining - 1);
        Other ->
            {error, {unexpected_result_before_overloaded, Other}}
    end.

discover_owned_keys(_Client, _Prefix, WorkerCount, _Candidate, Acc)
  when map_size(Acc) =:= WorkerCount ->
    [maps:get(Owner, Acc) || Owner <- lists:seq(0, WorkerCount - 1)];
discover_owned_keys(_Client, _Prefix, _WorkerCount, Candidate, _Acc) when Candidate > 1000000 ->
    fail(failed_to_discover_worker_keys);
discover_owned_keys(Client, Prefix, WorkerCount, Candidate, Acc) ->
    Probe = <<Prefix/binary, "-", (integer_to_binary(Candidate))/binary>>,
    {ok, Owner} = glifistore_client:worker_for(Client, Probe),
    Acc1 = case maps:is_key(Owner, Acc) of
               true -> Acc;
               false -> Acc#{Owner => Probe}
           end,
    discover_owned_keys(Client, Prefix, WorkerCount, Candidate + 1, Acc1).

build_worker_wave(_Keys, _Value, WorkerCount, Worker, WaveAcc, ValueAcc) when Worker =:= WorkerCount ->
    {lists:reverse(WaveAcc), lists:reverse(ValueAcc)};
build_worker_wave(Keys, Value, WorkerCount, Worker, WaveAcc, ValueAcc) ->
    Owned = lists:nth(Worker + 1, Keys),
    Payload = case Value of
                  <<>> -> <<"v", (Worker rem 10)>>;
                  _ -> Value
              end,
    Slot = [
        #{opcode => put, key => Owned, value => Payload},
        #{opcode => get, key => Owned}
    ],
    build_worker_wave(Keys, Value, WorkerCount, Worker + 1, [Slot | WaveAcc], [Payload | ValueAcc]).

verify_worker_results([], [], _Worker) ->
    ok;
verify_worker_results(
  [[#{outcome := succeeded}, #{outcome := succeeded, value := Got}] | RestSlots],
  [Expected | RestValues],
  Worker
) when Got =:= Expected ->
    verify_worker_results(RestSlots, RestValues, Worker + 1);
verify_worker_results(_Slots, _Values, Worker) ->
    fail({worker_pipeline_failed, Worker}).

parse_hex("") -> <<>>;
parse_hex(Hex) ->
    Clean = re:replace(Hex, "\\s+", "", [global, {return, binary}]),
    true = (byte_size(Clean) rem 2) =:= 0,
    binary:decode_hex(Clean).

to_hex(Bin) ->
    %% OTP 24+: binary:encode_hex/1. OTP 26+: encode_hex/2 with case option.
    %% Ubuntu 24.04 ships Erlang/OTP 25 — use /1 + lowercase.
    case erlang:function_exported(binary, encode_hex, 2) of
        true -> binary:encode_hex(Bin, lowercase);
        false -> string:lowercase(binary:encode_hex(Bin))
    end.

fail(Term) ->
    io:format(standard_error, "~p~n", [Term]),
    halt(1).

usage() ->
    io:format(standard_error,
              "Usage: glifistore-interop --port N [--tls --tls-ca PATH --server-name NAME] "
              "<put|get|erase|pipeline-put-get|worker-pipelines-put-get|expect-not-found|"
              "expect-permission-denied|burst-expect-overloaded|expect-frame-limit>~n",
              []).
