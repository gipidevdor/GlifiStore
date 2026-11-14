-module(glifistore_fake_server).
-export([start/1, stop/1, port/1, control/2, request_ids/1]).

-include("glifistore_protocol.hrl").

start(Opts) ->
    Parent = self(),
    Tab = ets:new(glifistore_fake_server, [public, set]),
    ets:insert(Tab, {cfg, normalize_opts(Opts)}),
    ets:insert(Tab, {request_ids, []}),
    ets:insert(Tab, {release_all, false}),
    ets:insert(Tab, {store_ops, 0}),
    ets:insert(Tab, {bind_counts, #{}}),
    Pid = spawn(fun() -> server(Parent, Tab) end),
    receive
        {glifistore_fake_server, ready, Port} ->
            {ok, #{pid => Pid, port => Port, tab => Tab}}
    end.

stop(#{pid := Pid, tab := Tab}) ->
    exit(Pid, shutdown),
    catch ets:delete(Tab),
    ok;
stop(#{pid := Pid}) ->
    exit(Pid, shutdown),
    ok.

port(#{port := Port}) -> Port.

control(#{tab := Tab}, Msg) ->
    handle_control(Tab, Msg).

request_ids(Server) ->
    control(Server, get_request_ids).

normalize_opts(Opts) ->
    #{
        workers => maps:get(workers, Opts, 1),
        internal => maps:get(internal_error_on_put, Opts, false),
        drop => maps:get(drop_after_mutation, Opts, false),
        stall_get => maps:get(stall_on_get, Opts, false),
        status_on_get => maps:get(status_on_get, Opts, undefined),
        routing_epoch => maps:get(routing_epoch, Opts, 9),
        hold_gets => maps:get(hold_gets, Opts, false),
        hold_puts => maps:get(hold_puts, Opts, false),
        record_ids => maps:get(record_ids, Opts, false),
        wrong_request_id => maps:get(wrong_request_id, Opts, false),
        wrong_owner => maps:get(wrong_owner, Opts, false),
        oversized_response => maps:get(oversized_response, Opts, false),
        mutate_epoch_after => maps:get(mutate_epoch_after, Opts, undefined),
        mutate_workers_after => maps:get(mutate_workers_after, Opts, undefined),
        internal_backup => maps:get(internal_error_on_backup, Opts, false),
        fail_rebind_workers => maps:get(fail_rebind_workers, Opts, [])
    }.

handle_control(Tab, get_request_ids) ->
    [{request_ids, Ids}] = ets:lookup(Tab, request_ids),
    lists:reverse(Ids);
handle_control(Tab, release_held) ->
    %% Publish the release before collecting registrations. A connection that
    %% races with this control call rechecks release_all after registering, so
    %% it cannot miss the release and remain blocked.
    ets:insert(Tab, {release_all, true}),
    Held = ets:match_object(Tab, {{held, '_'}, '_'}),
    lists:foreach(fun({{held, Pid}, true}) -> Pid ! {release_hold, ok} end, Held),
    ets:match_delete(Tab, {{held, '_'}, '_'}),
    ok;
handle_control(Tab, {set_routing_epoch, Epoch}) ->
    [{cfg, Cfg}] = ets:lookup(Tab, cfg),
    ets:insert(Tab, {cfg, Cfg#{routing_epoch => Epoch}}),
    ok;
handle_control(Tab, {set_workers, WC}) ->
    [{cfg, Cfg}] = ets:lookup(Tab, cfg),
    ets:insert(Tab, {cfg, Cfg#{workers => WC}}),
    ok;
handle_control(Tab, {set_wrong_request_id, Flag}) ->
    [{cfg, Cfg}] = ets:lookup(Tab, cfg),
    ets:insert(Tab, {cfg, Cfg#{wrong_request_id => Flag}}),
    ok;
handle_control(Tab, {set_wrong_owner, Flag}) ->
    [{cfg, Cfg}] = ets:lookup(Tab, cfg),
    ets:insert(Tab, {cfg, Cfg#{wrong_owner => Flag}}),
    ok;
handle_control(_Tab, Msg) ->
    {error, {unknown_control, Msg}}.

server(Parent, Tab) ->
    {ok, Listen} = gen_tcp:listen(0, [binary, {active, false}, {packet, 0}, {reuseaddr, true}]),
    {ok, {_Addr, Port}} = inet:sockname(Listen),
    Parent ! {glifistore_fake_server, ready, Port},
    accept_loop(Listen, Tab).

accept_loop(Listen, Tab) ->
    case gen_tcp:accept(Listen) of
        {ok, Socket} ->
            spawn(fun() -> client_loop(Socket, Tab, undefined, #{}) end),
            accept_loop(Listen, Tab);
        {error, closed} ->
            ok
    end.

client_loop(Socket, Tab, Bound, Store) ->
    case read_frame(Socket) of
        {ok, Frame} ->
            {ok, Request} = glifistore_protocol:decode_request(Frame, glifistore_protocol:max_frame_bytes()),
            handle_request(Socket, Tab, Bound, Store, Request);
        _ ->
            catch gen_tcp:close(Socket)
    end.

cfg(Tab) ->
    [{cfg, Cfg}] = ets:lookup(Tab, cfg),
    Cfg.

note_id(Tab, Id) ->
    Cfg = cfg(Tab),
    case maps:get(record_ids, Cfg) of
        true ->
            [{request_ids, Ids}] = ets:lookup(Tab, request_ids),
            ets:insert(Tab, {request_ids, [Id | Ids]});
        false ->
            ok
    end,
    [{store_ops, N}] = ets:lookup(Tab, store_ops),
    ets:insert(Tab, {store_ops, N + 1}).

ops(Tab) ->
    [{store_ops, N}] = ets:lookup(Tab, store_ops),
    N.

maybe_hold_get(Tab) ->
    maybe_hold(Tab, hold_gets).

maybe_hold_put(Tab) ->
    maybe_hold(Tab, hold_puts).

maybe_hold(Tab, Flag) ->
    Cfg = cfg(Tab),
    case maps:get(Flag, Cfg) of
        true ->
            case ets:lookup(Tab, release_all) of
                [{release_all, true}] ->
                    ok;
                _ ->
                    HeldKey = {held, self()},
                    ets:insert(Tab, {HeldKey, true}),
                    case ets:lookup(Tab, release_all) of
                        [{release_all, true}] ->
                            ets:delete(Tab, HeldKey),
                            ok;
                        _ ->
                            receive
                                {release_hold, ok} -> ok
                            after 30000 ->
                                error(hold_release_timeout)
                            end,
                            ets:delete(Tab, HeldKey)
                    end
            end;
        false ->
            ok
    end.

handle_request(Socket, Tab, Bound, Store, Request) ->
    note_id(Tab, maps:get(request_id, Request)),
    Cfg = cfg(Tab),
    Workers = maps:get(workers, Cfg),
    Epoch = maps:get(routing_epoch, Cfg),
    Ops = ops(Tab),
    Epoch1 =
        case maps:get(mutate_epoch_after, Cfg) of
            EpochAfter when is_integer(EpochAfter), Ops > EpochAfter -> Epoch + 1;
            _ -> Epoch
        end,
    Workers1 =
        case maps:get(mutate_workers_after, Cfg) of
            WorkersAfter when is_integer(WorkersAfter), Ops > WorkersAfter -> Workers + 1;
            _ -> Workers
        end,
    Meta = #{
        worker_count => Workers1,
        routing_epoch => Epoch1,
        owner_worker => case Bound of
            undefined -> 0;
            _ -> Bound
        end
    },
    case maps:get(opcode, Request) of
        ?GS_OP_INIT ->
            {ok, Identity} = glifistore_protocol:encode_init_identity(
                maps:get(routing, Cfg, glifistore_protocol:default_routing())
            ),
            reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                value => Identity,
                owner_worker => glifistore_protocol:no_worker()
            }),
            client_loop(Socket, Tab, Bound, Store);
        ?GS_OP_HEALTH ->
            reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                value => <<"GlifiStore/live">>
            }),
            client_loop(Socket, Tab, Bound, Store);
        ?GS_OP_READY ->
            reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                value => <<"GlifiStore/ready">>
            }),
            client_loop(Socket, Tab, Bound, Store);
        ?GS_OP_STATS ->
            reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                value => <<"GlifiStore/stats\n">>
            }),
            client_loop(Socket, Tab, Bound, Store);
        ?GS_OP_BIND ->
            NewBound = maps:get(target_worker, Request),
            [{bind_counts, Counts}] = ets:lookup(Tab, bind_counts),
            Count = maps:get(NewBound, Counts, 0) + 1,
            ets:insert(Tab, {bind_counts, Counts#{NewBound => Count}}),
            FailWorkers = maps:get(fail_rebind_workers, Cfg, []),
            case lists:member(NewBound, FailWorkers) andalso Count > 1 of
                true ->
                    %% Second+ BIND for this Worker: force reconnect failure (litmus).
                    reply(Socket, Cfg, glifistore_protocol:status_overloaded(), Request, Meta#{
                        owner_worker => NewBound
                    }),
                    client_loop(Socket, Tab, Bound, Store);
                false ->
                    reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                        owner_worker => NewBound
                    }),
                    client_loop(Socket, Tab, NewBound, Store)
            end;
        ?GS_OP_PUT ->
            case maps:get(drop, Cfg) of
                true ->
                    catch gen_tcp:close(Socket);
                false ->
                    case maps:get(internal, Cfg) of
                        true ->
                            reply(Socket, Cfg, glifistore_protocol:status_internal_error(), Request, Meta#{
                                owner_worker => Bound
                            }),
                            client_loop(Socket, Tab, Bound, Store);
                        false ->
                            maybe_hold_put(Tab),
                            Key = maps:get(key, Request),
                            Value = maps:get(value, Request),
                            reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                                owner_worker => Bound
                            }),
                            client_loop(Socket, Tab, Bound, Store#{Key => Value})
                    end
            end;
        ?GS_OP_GET ->
            case maps:get(stall_get, Cfg) of
                true ->
                    timer:sleep(3600000),
                    catch gen_tcp:close(Socket);
                false ->
                    maybe_hold_get(Tab),
                    case maps:get(status_on_get, Cfg) of
                        undefined ->
                            Key = maps:get(key, Request),
                            case maps:get(Key, Store, undefined) of
                                undefined ->
                                    reply(
                                        Socket,
                                        Cfg,
                                        glifistore_protocol:status_not_found(),
                                        Request,
                                        Meta#{owner_worker => Bound}
                                    ),
                                    client_loop(Socket, Tab, Bound, Store);
                                Value ->
                                    reply(
                                        Socket,
                                        Cfg,
                                        glifistore_protocol:status_ok(),
                                        Request,
                                        Meta#{owner_worker => Bound, value => Value}
                                    ),
                                    client_loop(Socket, Tab, Bound, Store)
                            end;
                        Status ->
                            reply(Socket, Cfg, Status, Request, Meta#{owner_worker => Bound}),
                            client_loop(Socket, Tab, Bound, Store)
                    end
            end;
        ?GS_OP_ERASE ->
            Key = maps:get(key, Request),
            case maps:is_key(Key, Store) of
                true ->
                    reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{owner_worker => Bound}),
                    client_loop(Socket, Tab, Bound, maps:remove(Key, Store));
                false ->
                    reply(
                        Socket,
                        Cfg,
                        glifistore_protocol:status_not_found(),
                        Request,
                        Meta#{owner_worker => Bound}
                    ),
                    client_loop(Socket, Tab, Bound, Store)
            end;
        ?GS_OP_PING ->
            reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                value => maps:get(value, Request)
            }),
            client_loop(Socket, Tab, Bound, Store);
        ?GS_OP_BACKUP ->
            case maps:get(internal_backup, Cfg) of
                true ->
                    reply(Socket, Cfg, glifistore_protocol:status_internal_error(), Request, Meta#{
                        value => <<"report failed">>,
                        owner_worker => Bound
                    }),
                    client_loop(Socket, Tab, Bound, Store);
                false ->
                    reply(Socket, Cfg, glifistore_protocol:status_ok(), Request, Meta#{
                        value => <<"status=ok files=0 bytes=0">>,
                        owner_worker => Bound
                    }),
                    client_loop(Socket, Tab, Bound, Store)
            end;
        _ ->
            reply(Socket, Cfg, glifistore_protocol:status_unsupported(), Request, Meta),
            client_loop(Socket, Tab, Bound, Store)
    end.

read_frame(Socket) ->
    case gen_tcp:recv(Socket, 4, 5000) of
        {ok, <<Size:32/little>>} ->
            case gen_tcp:recv(Socket, Size - 4, 5000) of
                {ok, Rest} -> {ok, <<Size:32/little, Rest/binary>>};
                Err -> Err
            end;
        Err ->
            Err
    end.

reply(Socket, Cfg, Status, Request, Fields) ->
    case maps:get(oversized_response, Cfg) of
        true ->
            gen_tcp:send(Socket, <<(16#7fffffff):32/little, 0:(36 * 8)>>);
        false ->
            RequestId =
                case maps:get(wrong_request_id, Cfg) of
                    true -> maps:get(request_id, Request) bxor 1;
                    false -> maps:get(request_id, Request)
                end,
            Owner =
                case maps:get(wrong_owner, Cfg) of
                    true -> (maps:get(owner_worker, Fields) + 1) rem 256;
                    false -> maps:get(owner_worker, Fields)
                end,
            {ok, Frame} = glifistore_protocol:encode_response(
                Status,
                RequestId,
                maps:get(value, Fields, <<>>),
                Owner,
                maps:get(worker_count, Fields),
                maps:get(routing_epoch, Fields)
            ),
            gen_tcp:send(Socket, Frame)
    end.
