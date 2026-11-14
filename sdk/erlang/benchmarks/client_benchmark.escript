#!/usr/bin/env escript
%%! -noshell -noinput +S 4:4
%% Reproducible external-server benchmark for the Erlang GlifiStore SDK.
%% Workload matches Python/Perl/Go/Ruby: ordered PUT/GET pipeline read-after-write.
%% Fixed +S 4:4 (not 1:1) so execute_worker_pipelines fan-out can use multiple
%% schedulers; still capped for cross-machine comparability.
-mode(compile).

main(Args) ->
    add_beam_path(),
    Opts = parse_args(#{
        host => "127.0.0.1",
        workers => 4,
        ops => 100000,
        pipeline => 64,
        warmup => 1,
        repeats => 7,
        concurrent => undefined
    }, Args),
    run(Opts).

add_beam_path() ->
    Script = escript:script_name(),
    Root = filename:dirname(filename:dirname(Script)),
    Beam = filename:join([Root, "_build", "default", "lib", "glifistore", "ebin"]),
    true = code:add_patha(Beam).

parse_args(Opts, []) ->
    Opts;
parse_args(Opts, ["--host", Host | Rest]) ->
    parse_args(Opts#{host => Host}, Rest);
parse_args(Opts, ["--port", Port | Rest]) ->
    parse_args(Opts#{port => list_to_integer(Port)}, Rest);
parse_args(Opts, ["--workers", N | Rest]) ->
    parse_args(Opts#{workers => list_to_integer(N)}, Rest);
parse_args(Opts, ["--ops", N | Rest]) ->
    parse_args(Opts#{ops => list_to_integer(N)}, Rest);
parse_args(Opts, ["--pipeline", N | Rest]) ->
    parse_args(Opts#{pipeline => list_to_integer(N)}, Rest);
parse_args(Opts, ["--warmup", N | Rest]) ->
    parse_args(Opts#{warmup => list_to_integer(N)}, Rest);
parse_args(Opts, ["--repeats", N | Rest]) ->
    parse_args(Opts#{repeats => list_to_integer(N)}, Rest);
parse_args(Opts, ["--concurrent" | Rest]) ->
    parse_args(Opts#{concurrent => true}, Rest);
parse_args(Opts, ["--no-concurrent" | Rest]) ->
    parse_args(Opts#{concurrent => false}, Rest);
parse_args(Opts, ["--callers", N | Rest]) ->
    parse_args(Opts#{callers => list_to_integer(N)}, Rest);
parse_args(Opts, ["--mode", Mode | Rest]) ->
    parse_args(Opts#{mode => list_to_atom(Mode)}, Rest);
parse_args(_Opts, [Unknown | _]) ->
    io:format(standard_error, "unknown argument: ~s~n", [Unknown]),
    usage(),
    halt(2).

usage() ->
    io:format(standard_error,
        "Usage: client_benchmark.escript --port N [--host H] [--workers N] [--ops N] "
        "[--pipeline N] [--warmup N] [--repeats N] [--callers N] "
        "[--mode pipeline|batch|worker_pipelines] [--concurrent|--no-concurrent]~n",
        []).

run(Opts) ->
    Port = maps:get(port, Opts, undefined),
    Workers = maps:get(workers, Opts),
    Ops = maps:get(ops, Opts),
    Pipeline = maps:get(pipeline, Opts),
    Warmup = maps:get(warmup, Opts),
    Repeats = maps:get(repeats, Opts),
    Callers = maps:get(callers, Opts, 1),
    Mode = maps:get(mode, Opts, worker_pipelines),
    case Port =:= undefined orelse Workers < 1 orelse Ops < 1 orelse Pipeline < 1
        orelse Warmup < 0 orelse Repeats < 1 orelse Callers < 1 of
        true ->
            io:format(standard_error, "numeric arguments are outside benchmark limits~n", []),
            halt(2);
        false ->
            ok
    end,
    UseConcurrent =
        case maps:get(concurrent, Opts) of
            undefined -> Workers > 1;
            Flag -> Flag
        end,
    Batches = material(Ops, Workers, Pipeline),
    BatchFrames = Pipeline * 2,
    Config = glifistore_util:merge_config(#{
        host => maps:get(host, Opts),
        port => Port,
        maximum_pipeline_requests => BatchFrames,
        request_timeout => 120.0
    }),
    case glifistore_client:connect(Config) of
        {ok, Client} ->
            try
                case glifistore_client:worker_count(Client) of
                    Workers ->
                        ok;
                    Other ->
                        io:format(standard_error,
                            "server Worker count ~p does not match --workers ~p~n",
                            [Other, Workers]),
                        halt(1)
                end,
                RunOnce = make_runner(Client, Batches, UseConcurrent, Callers, Mode, Workers, Ops, Pipeline),
                lists:foreach(fun(_) -> ok = RunOnce() end, lists:seq(1, Warmup)),
                Samples = [begin
                    T0 = erlang:monotonic_time(nanosecond),
                    ok = RunOnce(),
                    T1 = erlang:monotonic_time(nanosecond),
                    (T1 - T0) / 1.0e9
                end || _ <- lists:seq(1, Repeats)],
                report(UseConcurrent, Callers, Mode, Workers, Pipeline, Ops * 2, Samples)
            after
                glifistore_client:close(Client)
            end;
        {error, Err} ->
            io:format(standard_error, "~p~n", [Err]),
            halt(1)
    end.

make_runner(Client, Batches, UseConcurrent, Callers, Mode, Workers, Ops, Pipeline) ->
    case {Callers > 1, Mode, UseConcurrent} of
        {true, _, _} ->
            fun() -> run_multi_callers(Client, Callers, UseConcurrent, Ops, Workers, Pipeline) end;
        {false, batch, _} ->
            fun() -> run_batch_mode(Client, Batches) end;
        {false, pipeline, _} ->
            fun() -> run_sequential(Client, Batches) end;
        {false, worker_pipelines, true} when Workers > 1 ->
            fun() -> run_concurrent(Client, Batches) end;
        {false, _, _} ->
            fun() -> run_sequential(Client, Batches) end
    end.

run_multi_callers(Client, Callers, UseConcurrent, Ops, Workers, Pipeline) ->
    Parent = self(),
    PerCaller = max(1, Ops div Callers),
    lists:foreach(
        fun(I) ->
            spawn(fun() ->
                try
                    Batches = material(PerCaller, Workers, Pipeline, I),
                    case UseConcurrent of
                        true -> ok = run_concurrent(Client, Batches);
                        false -> ok = run_sequential(Client, Batches)
                    end,
                    Parent ! {caller_done, I, ok}
                catch
                    Class:Reason ->
                        Parent ! {caller_done, I, {error, Class, Reason}}
                end
            end)
        end,
        lists:seq(1, Callers)
    ),
    collect_callers(Callers).

collect_callers(0) ->
    ok;
collect_callers(N) ->
    receive
        {caller_done, _, ok} ->
            collect_callers(N - 1);
        {caller_done, _, {error, Class, Reason}} ->
            error({caller_failed, Class, Reason})
    after 600000 ->
        error(caller_timeout)
    end.

run_batch_mode(Client, Batches) ->
    MaxRounds = lists:max([0 | [length(WB) || WB <- Batches]]),
    lists:foreach(
        fun(Round) ->
            Wave = [
                case Round < length(WB) of
                    true -> lists:nth(Round + 1, WB);
                    false -> []
                end
             || WB <- Batches
            ],
            Flat = lists:append(Wave),
            case Flat of
                [] -> ok;
                _ -> validate_batch(Flat, glifistore_client:execute_batch(Client, Flat))
            end
        end,
        lists:seq(0, MaxRounds - 1)
    ),
    ok.

material(Operations, Workers, Pipeline) ->
    material(Operations, Workers, Pipeline, 0).

material(Operations, Workers, Pipeline, CallerId) ->
    Quotas0 = [Operations div Workers || _ <- lists:seq(1, Workers)],
    Rem = Operations rem Workers,
    Quotas = [lists:nth(I, Quotas0) + case I =< Rem of true -> 1; false -> 0 end
              || I <- lists:seq(1, Workers)],
    Requests = fill_requests(Quotas, 0, [[] || _ <- lists:seq(1, Workers)], CallerId),
    BatchFrames = Pipeline * 2,
    [chunk(WorkerReqs, BatchFrames) || WorkerReqs <- Requests].

fill_requests(Quotas, _Candidate, Acc, _CallerId) ->
    case lists:any(fun(Q) -> Q > 0 end, Quotas) of
        false ->
            Acc;
        true ->
            fill_requests_step(Quotas, 0, Acc, _CallerId)
    end.

fill_requests_step(Quotas, Candidate, Acc, CallerId) ->
    case lists:any(fun(Q) -> Q > 0 end, Quotas) of
        false ->
            Acc;
        true ->
            Key = list_to_binary(io_lib:format("erlang-bench-~B-~12..0B", [CallerId, Candidate])),
            {ok, Owner} = glifistore_protocol:worker_for(Key, length(Quotas)),
            Quota = lists:nth(Owner + 1, Quotas),
            case Quota of
                0 ->
                    fill_requests_step(Quotas, Candidate + 1, Acc, CallerId);
                _ ->
                    Value = binary:copy(<< (Candidate band 16#FF) >>, 64),
                    Put = #{opcode => put, key => Key, value => Value},
                    Get = #{opcode => get, key => Key},
                    Acc1 = set_nth(Acc, Owner + 1, lists:nth(Owner + 1, Acc) ++ [Put, Get]),
                    Quotas1 = set_nth(Quotas, Owner + 1, Quota - 1),
                    fill_requests_step(Quotas1, Candidate + 1, Acc1, CallerId)
            end
    end.

chunk(List, Size) ->
    chunk(List, Size, []).

chunk([], _Size, Acc) ->
    lists:reverse(Acc);
chunk(List, Size, Acc) ->
    {Batch, Rest} = split_at(List, Size),
    chunk(Rest, Size, [Batch | Acc]).

split_at(List, N) when length(List) =< N ->
    {List, []};
split_at(List, N) ->
    {lists:sublist(List, N), lists:nthtail(N, List)}.

set_nth(List, Idx, Value) ->
    lists:sublist(List, 1, Idx - 1) ++ [Value] ++ lists:nthtail(Idx, List).

run_sequential(Client, Batches) ->
    lists:foreach(
        fun(WorkerBatches) ->
            lists:foreach(
                fun(Batch) ->
                    validate_batch(Batch, glifistore_client:execute_pipeline(Client, Batch))
                end,
                WorkerBatches
            )
        end,
        Batches
    ),
    ok.

run_concurrent(Client, Batches) ->
    MaxRounds = lists:max([0 | [length(WB) || WB <- Batches]]),
    lists:foreach(
        fun(Round) ->
            Wave = [
                case Round < length(WB) of
                    true -> lists:nth(Round + 1, WB);
                    false -> []
                end
             || WB <- Batches
            ],
            case lists:all(fun(B) -> B =:= [] end, Wave) of
                true ->
                    ok;
                false ->
                    {ok, All} = glifistore_client:execute_worker_pipelines(Client, Wave),
                    lists:foreach(
                        fun({Batch, Responses}) ->
                            case Batch of
                                [] -> ok;
                                _ -> validate_batch(Batch, {ok, Responses})
                            end
                        end,
                        lists:zip(Wave, All)
                    )
            end
        end,
        lists:seq(0, MaxRounds - 1)
    ),
    ok.

validate_batch(Batch, {ok, Responses}) ->
    case length(Responses) =:= length(Batch) of
        false ->
            error(pipeline_response_count_mismatch);
        true ->
            lists:foreach(
                fun({Idx, Req}) ->
                    Resp = lists:nth(Idx, Responses),
                    case maps:get(outcome, Resp) of
                        succeeded ->
                            case maps:get(opcode, Req) of
                                get ->
                                    Expected = maps:get(value, lists:nth(Idx - 1, Batch)),
                                    case maps:get(value, Resp) of
                                        Expected -> ok;
                                        _ -> error(pipeline_get_value_mismatch)
                                    end;
                                _ ->
                                    ok
                            end;
                        _ ->
                            error(pipeline_request_failed)
                    end
                end,
                lists:zip(lists:seq(1, length(Batch)), Batch)
            )
    end;
validate_batch(_Batch, {error, Err}) ->
    error({pipeline_failed, Err}).

report(UseConcurrent, Callers, Mode, Workers, Pipeline, OperationCount, Samples) ->
    Rates = [OperationCount * Callers / S || S <- Samples],
    Execution =
        case {Callers > 1, UseConcurrent} of
            {true, true} -> "multi-caller-worker-concurrent";
            {true, false} -> "multi-caller-worker-sequential";
            {false, true} -> "single-process-worker-concurrent";
            {false, false} -> "single-process-worker-sequential"
        end,
    Version = binary_to_list(glifistore_version:version()),
    SortedS = lists:sort(Samples),
    SortedR = lists:sort(Rates),
    io:format("# glifistore Erlang client benchmark~n"),
    io:format(
        "# sdk_version=~s runtime=sync execution=~s mode=~s callers=~B workers=~B "
        "pipeline_pairs=~B operations=~B~n",
        [Version, Execution, Mode, Callers, Workers, Pipeline, OperationCount * Callers]
    ),
    io:format(
        "name=erlang_client_pipeline_read_after_write sdk_version=~s runtime=sync execution=~s "
        "workers=~B pipeline_pairs=~B operations=~B samples=~B "
        "median_seconds=~.9f min_seconds=~.9f max_seconds=~.9f "
        "median_ops_per_second=~.3f min_ops_per_second=~.3f max_ops_per_second=~.3f "
        "mode=~s callers=~B p50_seconds=~.9f p95_seconds=~.9f p99_seconds=~.9f "
        "errors=0 indeterminates=0~n",
        [
            Version,
            Execution,
            Workers,
            Pipeline,
            OperationCount * Callers,
            length(Samples),
            median(SortedS),
            hd(SortedS),
            lists:last(SortedS),
            median(SortedR),
            hd(SortedR),
            lists:last(SortedR),
            Mode,
            Callers,
            percentile(SortedS, 0.50),
            percentile(SortedS, 0.95),
            percentile(SortedS, 0.99)
        ]
    ).

median([X]) ->
    X;
median(Sorted) ->
    percentile(Sorted, 0.50).

percentile([X], _P) ->
    X;
percentile(Sorted, P) ->
    N = length(Sorted),
    Idx = max(1, min(N, ceil(P * N))),
    lists:nth(Idx, Sorted).
