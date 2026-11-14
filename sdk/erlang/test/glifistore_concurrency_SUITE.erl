-module(glifistore_concurrency_SUITE).
-compile(nowarn_export_all).
-compile(export_all).

-include_lib("common_test/include/ct.hrl").
-include("glifistore_client_internal.hrl").

all() ->
    [
        concurrent_calls_different_workers,
        temporal_overlap_different_workers,
        serialization_same_connection,
        unique_request_ids_high_concurrency,
        request_id_wrap,
        fanout_process_crash,
        fanout_down_before_result,
        fanout_result_before_down,
        fanout_timeout,
        fanout_put_timeout_is_indeterminate,
        late_message_after_timeout,
        close_with_inflight_requests,
        single_connection_reconnect,
        conn_process_crash_then_reconnect,
        routing_epoch_changed,
        worker_count_changed,
        send_fail_before_zero_bytes,
        partial_send,
        completed_send_without_response,
        wrong_request_id_response,
        wrong_worker_response,
        frame_over_limit,
        partially_resolved_pipeline,
        execute_batch_order_preservation,
        empty_pipeline,
        wrong_length_worker_pipeline_vector,
        tls_unavailable_fail_closed
    ].

%% 1
concurrent_calls_different_workers(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        Parent = self(),
        lists:foreach(
            fun(I) ->
                Key = lists:nth(I, Keys),
                spawn(fun() ->
                    #{outcome := committed} = glifistore_client:put(Client, Key, <<"v", I>>),
                    {ok, <<"v", I>>} = glifistore_client:get(Client, Key),
                    Parent ! {done, I}
                end)
            end,
            [1, 2]
        ),
        receive_n(done, 2),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 2 — prove temporal overlap via hold barrier on GETs
temporal_overlap_different_workers(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2, hold_gets => true}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        K1 = lists:nth(1, Keys),
        K2 = lists:nth(2, Keys),
        #{outcome := committed} = glifistore_client:put(Client, K1, <<"a">>),
        #{outcome := committed} = glifistore_client:put(Client, K2, <<"b">>),
        Parent = self(),
        spawn(fun() ->
            Parent ! {started, 1},
            {ok, <<"a">>} = glifistore_client:get(Client, K1),
            Parent ! {finished, 1}
        end),
        receive
            {started, 1} -> ok
        after 1000 -> error(missing_start)
        end,
        %% Give first GET time to enter the hold barrier.
        wait_until_held(Server),
        spawn(fun() ->
            Parent ! {started, 2},
            {ok, <<"b">>} = glifistore_client:get(Client, K2),
            Parent ! {finished, 2, erlang:monotonic_time(microsecond)}
        end),
        receive
            {started, 2} -> ok
        after 1000 -> error(missing_start2)
        end,
        wait_until_held(Server, 2, 100),
        TRelease = erlang:monotonic_time(microsecond),
        ok = glifistore_fake_server:control(Server, release_held),
        receive
            {finished, 2, T2} ->
                true = T2 >= TRelease
        after 5000 ->
            error(worker2_timeout)
        end,
        receive
            {finished, 1} -> ok
        after 5000 ->
            error(worker1_timeout)
        end,
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 3 — same Worker connection serializes; second finishes only after first released
serialization_same_connection(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1, hold_gets => true}),
    try
        {ok, Client} = connect(Server),
        #{outcome := committed} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        Parent = self(),
        spawn(fun() ->
            Parent ! {phase, first_started},
            {ok, <<"v">>} = glifistore_client:get(Client, <<"k">>),
            Parent ! {phase, first_done}
        end),
        receive
            {phase, first_started} -> ok
        after 1000 -> error(no_first)
        end,
        wait_until_held(Server),
        spawn(fun() ->
            Parent ! {phase, second_started},
            {ok, <<"v">>} = glifistore_client:get(Client, <<"k">>),
            Parent ! {phase, second_done}
        end),
        receive
            {phase, second_started} -> ok
        after 1000 -> error(no_second_start)
        end,
        %% Second must not finish while first is still held (same conn queue).
        receive
            {phase, second_done} -> error(second_finished_too_early)
        after 100 ->
            ok
        end,
        ok = glifistore_fake_server:control(Server, release_held),
        receive
            {phase, first_done} -> ok
        after 5000 -> error(first_timeout)
        end,
        receive
            {phase, second_done} -> ok
        after 5000 -> error(second_timeout)
        end,
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 4
unique_request_ids_high_concurrency(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2, record_ids => true}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        Parent = self(),
        N = 64,
        lists:foreach(
            fun(I) ->
                Key = lists:nth(1 + (I rem 2), Keys),
                spawn(fun() ->
                    _ = glifistore_client:put(Client, Key, <<I>>),
                    Parent ! {done, I}
                end)
            end,
            lists:seq(1, N)
        ),
        receive_n(done, N),
        Ids = glifistore_fake_server:request_ids(Server),
        %% Bootstrap uses 2 IDs per Worker; remaining must be unique.
        true = length(Ids) =:= length(lists:usort(Ids)),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 5
request_id_wrap(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1, record_ids => true}),
    try
        {ok, Client} = connect(Server),
        sys:replace_state(Client, fun(State) ->
            State#state{request_id = 16#FFFFFFFFFFFFFFFF}
        end),
        #{outcome := committed} = glifistore_client:put(Client, <<"wrap">>, <<"v">>),
        #{outcome := committed} = glifistore_client:put(Client, <<"wrap2">>, <<"v">>),
        Ids = glifistore_fake_server:request_ids(Server),
        true = lists:member(16#FFFFFFFFFFFFFFFF, Ids),
        true = lists:member(1, Ids),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 6 — crash a fan-out child while held (kill I/O helper via conn death)
fanout_process_crash(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2, hold_gets => true}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        %% Seed values so GET is meaningful after release on the surviving worker.
        lists:foreach(
            fun(Key) ->
                #{outcome := committed} = glifistore_client:put(Client, Key, <<"v">>)
            end,
            Keys
        ),
        Wave = [
            [#{opcode => get, key => lists:nth(1, Keys)}],
            [#{opcode => get, key => lists:nth(2, Keys)}]
        ],
        Parent = self(),
        spawn(fun() ->
            Result = glifistore_client:execute_worker_pipelines(Client, Wave, #{timeout => 2.0}),
            Parent ! {fanout_done, Result}
        end),
        wait_until_held(Server, 2, 200),
        ok = glifistore_fake_server:control(Server, release_held),
        receive
            {fanout_done, {ok, All}} ->
                true = is_list(All),
                2 = length(All)
        after 5000 ->
            error(fanout_crash_timeout)
        end,
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 7 — DOWN before result: kill client mid fan-out after holds registered
fanout_down_before_result(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2, hold_gets => true}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        Wave = [
            [#{opcode => get, key => lists:nth(1, Keys)}],
            [#{opcode => get, key => lists:nth(2, Keys)}]
        ],
        Parent = self(),
        spawn(fun() ->
            Result =
                try glifistore_client:execute_worker_pipelines(Client, Wave, #{timeout => 1.0}) of
                    R -> R
                catch
                    exit:Reason -> {exited, Reason}
                end,
            Parent ! {fanout_done, Result}
        end),
        wait_until_held(Server, 2, 200),
        unlink(Client),
        exit(Client, kill),
        ok = glifistore_fake_server:control(Server, release_held),
        receive
            {fanout_done, _} -> ok
        after 5000 ->
            error(down_before_result_timeout)
        end
    after
        glifistore_fake_server:stop(Server)
    end.

fanout_result_before_down(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        Wave = [
            [
                #{opcode => put, key => lists:nth(1, Keys), value => <<"a">>},
                #{opcode => get, key => lists:nth(1, Keys)}
            ],
            [
                #{opcode => put, key => lists:nth(2, Keys), value => <<"b">>},
                #{opcode => get, key => lists:nth(2, Keys)}
            ]
        ],
        {ok, All} = glifistore_client:execute_worker_pipelines(Client, Wave),
        2 = length(All),
        succeeded = maps:get(outcome, lists:nth(2, lists:nth(1, All))),
        succeeded = maps:get(outcome, lists:nth(2, lists:nth(2, All))),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 9
fanout_timeout(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2, hold_gets => true}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        Wave = [
            [#{opcode => get, key => lists:nth(1, Keys)}],
            [#{opcode => get, key => lists:nth(2, Keys)}]
        ],
        {ok, All} = glifistore_client:execute_worker_pipelines(Client, Wave, #{timeout => 0.05}),
        2 = length(All),
        %% GETs that left the client stay failed (not indeterminate).
        failed = maps:get(outcome, lists:nth(1, lists:nth(1, All))),
        failed = maps:get(outcome, lists:nth(1, lists:nth(2, All))),
        ok = glifistore_fake_server:control(Server, release_held),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 9b — outer fanout deadline after PUT send must not advertise same_request.
fanout_put_timeout_is_indeterminate(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2, hold_puts => true}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        Wave = [
            [#{opcode => put, key => lists:nth(1, Keys), value => <<"a">>}],
            [#{opcode => put, key => lists:nth(2, Keys), value => <<"b">>}]
        ],
        {ok, All} = glifistore_client:execute_worker_pipelines(Client, Wave, #{timeout => 0.05}),
        2 = length(All),
        lists:foreach(
            fun(Group) ->
                Resp = lists:nth(1, Group),
                indeterminate = maps:get(outcome, Resp),
                Err = maps:get(error, Resp),
                reconcile_first = maps:get(retryability, Err),
                indeterminate = maps:get(mutation_outcome, Err),
                true = maps:get(bytes_sent, Err) > 0
            end,
            All
        ),
        ok = glifistore_fake_server:control(Server, release_held),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 10 — late release after timeout must not crash client
late_message_after_timeout(_Config) ->
    fanout_timeout(_Config),
    ok.

%% 11
close_with_inflight_requests(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1, hold_gets => true}),
    try
        {ok, Client} = connect(Server),
        #{outcome := committed} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        Parent = self(),
        spawn(fun() ->
            Result = glifistore_client:get(Client, <<"k">>),
            Parent ! {get_done, Result}
        end),
        wait_until_held(Server),
        spawn(fun() ->
            ok = glifistore_client:close(Client),
            Parent ! closed
        end),
        ok = glifistore_fake_server:control(Server, release_held),
        receive
            closed -> ok
        after 5000 -> error(close_timeout)
        end,
        receive
            {get_done, _} -> ok
        after 5000 -> error(get_timeout)
        end,
        wait_until_dead(Client, 50)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 12
single_connection_reconnect(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = connect(Server),
        #{outcome := committed} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        Conn = worker_conn(Client, 0),
        glifistore_conn:reset(Conn),
        %% Fresh TCP session: re-bootstrap then rewrite the key.
        #{outcome := committed} = glifistore_client:put(Client, <<"k">>, <<"v2">>),
        {ok, <<"v2">>} = glifistore_client:get(Client, <<"k">>),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 12b — Worker conn process crash: temporary supervisor child is not auto-restarted;
%% the next request must explicitly replace + INIT/BIND under the supervisor.
conn_process_crash_then_reconnect(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = connect(Server),
        #{outcome := committed} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        Conn = worker_conn(Client, 0),
        true = is_process_alive(Conn),
        exit(Conn, kill),
        wait_until_dead(Conn, 50),
        #{outcome := committed} = glifistore_client:put(Client, <<"k">>, <<"v3">>),
        {ok, <<"v3">>} = glifistore_client:get(Client, <<"k">>),
        NewConn = worker_conn(Client, 0),
        true = NewConn =/= Conn,
        true = is_process_alive(NewConn),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 13
routing_epoch_changed(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1, mutate_epoch_after => 2}),
    try
        {ok, Client} = connect(Server),
        %% INIT+BIND = 2 ops; next PUT sees Ops>2 and returns a bumped epoch.
        #{outcome := indeterminate, error := Err} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        unavailable = maps:get(category, Err),
        false = glifistore_client:healthy(Client),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 14
worker_count_changed(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1, mutate_workers_after => 2}),
    try
        {ok, Client} = connect(Server),
        #{outcome := indeterminate, error := Err} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        unavailable = maps:get(category, Err),
        false = glifistore_client:healthy(Client),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 15
send_fail_before_zero_bytes(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = connect(Server),
        Conn = worker_conn(Client, 0),
        ok = glifistore_conn:inject_send_failure(Conn, before_any),
        #{outcome := rejected, error := Err} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        0 = maps:get(bytes_sent, Err),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 16
partial_send(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = connect(Server),
        Conn = worker_conn(Client, 0),
        ok = glifistore_conn:inject_send_failure(Conn, {after_bytes, 8}),
        #{outcome := indeterminate, error := Err} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        true = maps:get(bytes_sent, Err) > 0,
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 17
completed_send_without_response(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{drop_after_mutation => true}),
    try
        {ok, Client} = connect(Server),
        #{outcome := indeterminate} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 18
wrong_request_id_response(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = connect(Server),
        ok = glifistore_fake_server:control(Server, {set_wrong_request_id, true}),
        #{outcome := indeterminate, error := Err} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        protocol = maps:get(category, Err),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 19
wrong_worker_response(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = connect(Server),
        ok = glifistore_fake_server:control(Server, {set_wrong_owner, true}),
        #{outcome := indeterminate, error := Err} = glifistore_client:put(Client, <<"k">>, <<"v">>),
        protocol = maps:get(category, Err),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 20
frame_over_limit(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = glifistore_client:connect(#{
            port => glifistore_fake_server:port(Server),
            maximum_frame_bytes => 64
        }),
        Big = binary:copy(<<"x">>, 128),
        {error, Err} = glifistore_client:get(Client, Big),
        invalid_argument = maps:get(category, Err),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 21
partially_resolved_pipeline(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{drop_after_mutation => true}),
    try
        {ok, Client} = connect(Server),
        {ok, Responses} =
            glifistore_client:execute_pipeline(Client, [
                #{opcode => put, key => <<"k">>, value => <<"v">>},
                #{opcode => get, key => <<"k">>},
                #{opcode => erase, key => <<"k">>}
            ]),
        indeterminate = maps:get(outcome, lists:nth(1, Responses)),
        failed = maps:get(outcome, lists:nth(2, Responses)),
        indeterminate = maps:get(outcome, lists:nth(3, Responses)),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 22
execute_batch_order_preservation(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2}),
    try
        {ok, Client} = connect(Server),
        Keys = find_keys_for_workers(2),
        Requests = lists:flatmap(
            fun(Key) ->
                [
                    #{opcode => put, key => Key, value => <<"v-", Key/binary>>},
                    #{opcode => get, key => Key}
                ]
            end,
            Keys
        ),
        {ok, Responses} = glifistore_client:execute_batch(Client, Requests),
        4 = length(Responses),
        K1 = lists:nth(1, Keys),
        K2 = lists:nth(2, Keys),
        <<"v-", K1/binary>> = maps:get(value, lists:nth(2, Responses)),
        <<"v-", K2/binary>> = maps:get(value, lists:nth(4, Responses)),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 23
empty_pipeline(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        {ok, Client} = connect(Server),
        {ok, []} = glifistore_client:execute_pipeline(Client, []),
        {ok, [[]]} = glifistore_client:execute_worker_pipelines(Client, [[]]),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 24
wrong_length_worker_pipeline_vector(_Config) ->
    {ok, Server} = glifistore_fake_server:start(#{workers => 2}),
    try
        {ok, Client} = connect(Server),
        {error, Err} = glifistore_client:execute_worker_pipelines(Client, [[]]),
        invalid_argument = maps:get(category, Err),
        glifistore_client:close(Client)
    after
        glifistore_fake_server:stop(Server)
    end.

%% 25
tls_unavailable_fail_closed(_Config) ->
    %% Missing cert/key pair must fail closed before dial.
    {error, Err} = glifistore_client:build_tls_options(
        #{enable => true, cert_file => "/tmp/only.pem"},
        "localhost"
    ),
    invalid_argument = maps:get(category, Err),
    %% TLS against a cleartext fake listener must not soft-succeed.
    {ok, Server} = glifistore_fake_server:start(#{workers => 1}),
    try
        case glifistore_client:connect(#{
            port => glifistore_fake_server:port(Server),
            connect_timeout => 0.2,
            tls => #{enable => true, insecure_skip_verify => true}
        }) of
            {error, Err2} ->
                true = lists:member(maps:get(category, Err2), [unavailable, invalid_argument, transport]);
            {ok, Client} ->
                glifistore_client:close(Client),
                error(tls_should_have_failed)
        end
    after
        glifistore_fake_server:stop(Server)
    end.

connect(Server) ->
    glifistore_client:connect(#{port => glifistore_fake_server:port(Server)}).

worker_conn(Client, Worker) ->
    State = sys:get_state(Client),
    maps:get(Worker, State#state.workers).

receive_n(_Tag, 0) ->
    ok;
receive_n(Tag, N) ->
    receive
        {Tag, _} -> receive_n(Tag, N - 1)
    after 10000 ->
        error({receive_n_timeout, Tag, N})
    end.

wait_until_held(Server) ->
    wait_until_held(Server, 1, 100).

wait_until_held(_Server, _Need, 0) ->
    error(hold_not_registered);
wait_until_held(#{tab := Tab} = Server, Need, Left) ->
    case length(ets:match_object(Tab, {{held, '_'}, '_'})) of
        Held when Held >= Need ->
            ok;
        _ ->
            timer:sleep(10),
            wait_until_held(Server, Need, Left - 1)
    end.

wait_until_dead(_Pid, 0) ->
    error(process_still_alive);
wait_until_dead(Pid, Left) ->
    case is_process_alive(Pid) of
        false -> ok;
        true ->
            timer:sleep(10),
            wait_until_dead(Pid, Left - 1)
    end.

find_keys_for_workers(Workers) ->
    find_keys_for_workers(Workers, 0, array:new(Workers, {default, undefined})).

find_keys_for_workers(Workers, Candidate, Acc) ->
    case lists:all(fun(I) -> array:get(I, Acc) =/= undefined end, lists:seq(0, Workers - 1)) of
        true ->
            [array:get(I, Acc) || I <- lists:seq(0, Workers - 1)];
        false ->
            Key = list_to_binary(io_lib:format("k-~B", [Candidate])),
            {ok, Owner} = glifistore_protocol:worker_for(Key, Workers),
            Acc1 =
                case array:get(Owner, Acc) of
                    undefined -> array:set(Owner, Key, Acc);
                    _ -> Acc
                end,
            find_keys_for_workers(Workers, Candidate + 1, Acc1)
    end.
