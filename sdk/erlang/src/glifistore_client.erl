-module(glifistore_client).
-behaviour(gen_server).

-include("glifistore_protocol.hrl").
-include("glifistore_client_internal.hrl").

-export([
    connect/1,
    close/1,
    healthy/1,
    worker_count/1,
    routing_epoch/1,
    routing/1,
    worker_for/2,
    get/2,
    get/3,
    ping/2,
    ping/3,
    health/1,
    health/2,
    ready/1,
    ready/2,
    stats/1,
    stats/2,
    backup/2,
    backup/3,
    put/3,
    put/4,
    erase/2,
    erase/3,
    execute_pipeline/2,
    execute_pipeline/3,
    execute_batch/2,
    execute_batch/3,
    execute_worker_pipelines/2,
    execute_worker_pipelines/3,
    build_tls_options/2
]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2]).

-export_type([config/0, client/0, mutation_result/0, pipeline_request/0, pipeline_response/0]).

%% Concurrency model (preferred strategy):
%% glifistore_client is a coordinator only. handle_call resolves Worker, Conn,
%% request ID, and deadline, then delegates socket I/O to a monitored process that
%% calls glifistore_conn. The coordinator returns {noreply, State} so callers
%% targeting different Workers proceed in parallel on the BEAM. Each glifistore_conn
%% still serializes request/response on its single TCP/TLS stream.
%%
%% Request IDs are allocated only inside this gen_server (never copied to concurrent
%% callers). Wire protocol v2 treats request_id as correlation-only with no server
%% deduplication; we still keep a single client-wide counter so in-flight IDs stay
%% unique across Workers and wrap 16#FFFFFFFFFFFFFFFF -> 1.
%%
%% OTP supervision: glifistore_conn_sup (linked from this process) owns one
%% temporary child per Worker with intensity 0 — never auto-restart. Reconnect is
%% explicit in ensure_connected (re-INIT+BIND, verify epoch/count, fail-closed).
%% Conn crashes are monitored; they do not kill the coordinator via link.
%%
%% close/1 is synchronous: rejects new work, lets in-flight ops finish (or fail
%% after socket reset), then stops Worker conns. Late io_result/DOWN messages are ignored.

-type config() :: #{
    host => string() | binary(),
    port => pos_integer(),
    unix_socket_path => string() | binary(),
    connect_timeout => float(),
    request_timeout => float(),
    maximum_frame_bytes => pos_integer(),
    maximum_pipeline_requests => pos_integer(),
    maximum_pipeline_bytes => pos_integer(),
    tls => map()
}.

-type client() :: pid().
-type mutation_result() :: #{outcome := committed | rejected | indeterminate, error => glifistore_error:error()}.
-type pipeline_request() :: #{
    opcode := get | put | erase,
    key := binary(),
    value => binary(),
    expire_at_ns => non_neg_integer()
}.
-type pipeline_response() :: #{
    outcome := succeeded | failed | indeterminate,
    value => binary(),
    error => glifistore_error:error()
}.

connect(Config0) ->
    Config = glifistore_util:merge_config(Config0),
    case glifistore_util:validate_config(Config) of
        ok ->
            %% Use start/3 (not start_link) so bootstrap failures cannot kill the caller via link.
            case gen_server:start(?MODULE, Config, []) of
                {ok, Pid} ->
                    {ok, Pid};
                {error, {shutdown, Err}} ->
                    {error, Err};
                {error, Err} ->
                    {error, Err}
            end;
        {error, Err} ->
            {error, Err}
    end.

%% Synchronous close (sibling SDKs wait until sockets are released).
close(Client) ->
    try gen_server:call(Client, close, infinity) of
        ok -> ok
    catch
        exit:{noproc, _} -> ok;
        exit:{{noproc, _}, _} -> ok;
        exit:{normal, _} -> ok;
        exit:{{normal, _}, _} -> ok
    end.
healthy(Client) -> gen_server:call(Client, healthy).
worker_count(Client) -> gen_server:call(Client, worker_count).
routing_epoch(Client) -> gen_server:call(Client, routing_epoch).
routing(Client) -> gen_server:call(Client, routing).
worker_for(Client, Key) -> gen_server:call(Client, {worker_for, Key}).

get(Client, Key) -> get(Client, Key, #{}).
get(Client, Key, Opts) -> gen_server:call(Client, {read, get, Key, <<>>, Opts}, infinity).

ping(Client, Payload) -> ping(Client, Payload, #{}).
ping(Client, Payload, Opts) -> gen_server:call(Client, {read, ping, <<>>, Payload, Opts}, infinity).

health(Client) -> health(Client, #{}).
health(Client, Opts) -> gen_server:call(Client, {read, health, <<>>, <<>>, Opts}, infinity).

ready(Client) -> ready(Client, #{}).
ready(Client, Opts) -> gen_server:call(Client, {read, ready, <<>>, <<>>, Opts}, infinity).

stats(Client) -> stats(Client, #{}).
stats(Client, Opts) -> gen_server:call(Client, {read, stats, <<>>, <<>>, Opts}, infinity).

%% Online fenced BACKUP (opcode 10): not hot zero-impact; admin under secure authz.
backup(Client, Destination) -> backup(Client, Destination, #{}).
backup(Client, Destination, Opts) when is_binary(Destination), Destination =/= <<>> ->
    gen_server:call(Client, {read, backup, Destination, <<>>, Opts}, infinity);
backup(_Client, _Destination, _Opts) ->
    {error, glifistore_error:invalid_argument(<<"backup destination must be non-empty">>)}.

put(Client, Key, Value) -> put(Client, Key, Value, #{}).
put(Client, Key, Value, Opts) ->
    Expire = maps:get(expire_at_ns, Opts, 0),
    CallOpts = maps:without([expire_at_ns], Opts),
    gen_server:call(Client, {mutate, put, Key, Value, Expire, CallOpts}, infinity).

erase(Client, Key) -> erase(Client, Key, #{}).
erase(Client, Key, Opts) -> gen_server:call(Client, {mutate, erase, Key, <<>>, 0, Opts}, infinity).

execute_pipeline(Client, Requests) -> execute_pipeline(Client, Requests, #{}).
execute_pipeline(Client, Requests, Opts) ->
    gen_server:call(Client, {execute_pipeline, Requests, Opts}, infinity).

execute_batch(Client, Requests) -> execute_batch(Client, Requests, #{}).
execute_batch(Client, Requests, Opts) ->
    gen_server:call(Client, {execute_batch, Requests, Opts}, infinity).

%% Run one pipeline vector per Worker concurrently (Perl execute_worker_pipelines parity).
%% Batches must be a list of length worker_count(); empty lists are skipped.
execute_worker_pipelines(Client, Batches) -> execute_worker_pipelines(Client, Batches, #{}).
execute_worker_pipelines(Client, Batches, Opts) ->
    gen_server:call(Client, {execute_worker_pipelines, Batches, Opts}, infinity).

init(Config) ->
    case glifistore_conn_sup:start_link() of
        {ok, Sup} ->
            case bootstrap_all(Sup, Config, 1) of
                {ok, Workers, WorkerCount, RoutingEpoch, Routing, NextId, ConnMons} ->
                    {ok,
                        #state{
                            config = Config,
                            workers = Workers,
                            worker_count = WorkerCount,
                            routing_epoch = RoutingEpoch,
                            routing = Routing,
                            request_id = NextId,
                            sup = Sup,
                            conn_mons = ConnMons
                        }};
                {error, Err} ->
                    stop_conn_sup(Sup),
                    %% Use {shutdown, Err} so start_link callers are not killed by an abnormal exit reason.
                    {stop, {shutdown, Err}}
            end;
        {error, Err} ->
            {stop, {shutdown, glifistore_error:internal(iolist_to_binary(io_lib:format("~p", [Err])))}}
    end.

handle_call(healthy, _From, State) -> {reply, State#state.healthy, State};
handle_call(worker_count, _From, State) -> {reply, State#state.worker_count, State};
handle_call(routing_epoch, _From, State) -> {reply, State#state.routing_epoch, State};
handle_call(routing, _From, State) -> {reply, State#state.routing, State};
handle_call({worker_for, _Key}, _From, #state{worker_count = 0} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is not connected">>)}, State};
handle_call({worker_for, Key}, _From, #state{worker_count = WC, routing = Routing} = State) ->
    {reply, glifistore_protocol:worker_for(Key, WC, Routing), State};
handle_call({read, Op, Key, Value, Opts}, From, State) ->
    dispatch_read(Op, Key, Value, Opts, From, State);
handle_call({mutate, Op, Key, Value, Expire, Opts}, From, State) ->
    dispatch_mutate(Op, Key, Value, Expire, Opts, From, State);
handle_call({execute_pipeline, Requests, Opts}, From, State) ->
    dispatch_pipeline(Requests, Opts, From, State);
handle_call({execute_batch, Requests, Opts}, From, State) ->
    dispatch_batch(Requests, Opts, From, State);
handle_call({execute_worker_pipelines, Batches, Opts}, From, State) ->
    dispatch_worker_pipelines(Batches, Opts, From, State);
handle_call(close, From, State) ->
    begin_close(From, State);
handle_call(_Req, _From, State) ->
    {reply, {error, glifistore_error:internal(<<"unexpected call">>)}, State}.

handle_cast(_Msg, State) ->
    {noreply, State}.

handle_info({io_result, Tag, Result}, State) ->
    handle_io_result(Tag, Result, State);
handle_info({fanout_result, Tag, Mon, Worker, Items, GroupResponses, Healthy}, State) ->
    handle_fanout_result(Tag, Mon, Worker, Items, GroupResponses, Healthy, State);
handle_info({'DOWN', Mon, process, Pid, Reason}, State) ->
    case maps:take(Mon, State#state.conn_mons) of
        {Worker, ConnMons} ->
            handle_conn_down(Worker, Pid, Reason, State#state{conn_mons = ConnMons});
        error ->
            handle_down(Mon, Reason, State)
    end;
handle_info({timeout, TimerRef, {op_deadline, Tag}}, State) ->
    handle_op_timeout(Tag, TimerRef, State);
handle_info(_Msg, State) ->
    {noreply, State}.

terminate(_Reason, State) ->
    maps:foreach(
        fun(_Tag, Pending) ->
            cancel_pending_timer(Pending),
            case maps:get(pid, Pending, undefined) of
                undefined -> ok;
                Pid -> exit(Pid, kill)
            end,
            maps:foreach(
                fun(_Mon, Child) ->
                    exit(maps:get(pid, Child), kill)
                end,
                maps:get(children, Pending, #{})
            )
        end,
        State#state.pending
    ),
    maps:foreach(fun(_Mon, _) -> ok end, State#state.conn_mons),
    stop_conn_sup(State#state.sup),
    ok.

stop_conn_sup(undefined) ->
    ok;
stop_conn_sup(Sup) when is_pid(Sup) ->
    case is_process_alive(Sup) of
        true ->
            unlink(Sup),
            exit(Sup, shutdown),
            ok;
        false ->
            ok
    end.

%% Conn process exited. Do not auto-reconnect (fail-closed for in-flight work).
%% Next request path recreates via ensure_connected → replace_conn + INIT/BIND.
handle_conn_down(Worker, Pid, _Reason, State) ->
    case maps:get(Worker, State#state.workers, undefined) of
        Pid ->
            {noreply, State#state{workers = maps:remove(Worker, State#state.workers)}};
        _ ->
            %% Stale DOWN after an explicit replace_conn — ignore.
            {noreply, State}
    end.

bootstrap_all(Sup, Config, NextId) ->
    case glifistore_conn_sup:start_conn(Sup, 0, Config) of
        {ok, Conn0} ->
            Mon0 = monitor(process, Conn0),
            case bootstrap_conn(Conn0, 0, undefined, Config, NextId) of
                {ok, WC, Epoch, Routing, NextId1} ->
                    bootstrap_rest(Sup, 1, WC, Epoch, Routing, Config, #{0 => Conn0}, #{Mon0 => 0}, NextId1);
                {error, Err} ->
                    demonitor(Mon0, [flush]),
                    {error, Err}
            end;
        {error, Err} ->
            {error, glifistore_error:internal(iolist_to_binary(io_lib:format("conn start: ~p", [Err])))}
    end.

bootstrap_rest(_Sup, W, WC, Epoch, Routing, _Config, Workers, ConnMons, NextId) when W >= WC ->
    {ok, Workers, WC, Epoch, Routing, NextId, ConnMons};
bootstrap_rest(Sup, W, WC, Epoch, Routing, Config, Workers, ConnMons, NextId) ->
    case glifistore_conn_sup:start_conn(Sup, W, Config) of
        {ok, Conn} ->
            Mon = monitor(process, Conn),
            case bootstrap_conn(Conn, W, {WC, Epoch, Routing}, Config, NextId) of
                {ok, WC, Epoch, Routing, NextId1} ->
                    bootstrap_rest(
                        Sup,
                        W + 1,
                        WC,
                        Epoch,
                        Routing,
                        Config,
                        Workers#{W => Conn},
                        ConnMons#{Mon => W},
                        NextId1
                    );
                {error, Err} ->
                    demonitor(Mon, [flush]),
                    {error, Err}
            end;
        {error, Err} ->
            {error, glifistore_error:internal(iolist_to_binary(io_lib:format("conn start: ~p", [Err])))}
    end.

bootstrap_conn(Conn, Worker, Expected, Config, NextId) ->
    glifistore_conn:reset(Conn),
    case glifistore_conn:dial(Conn) of
        ok ->
            Deadline = glifistore_util:monotonic_seconds() + maps:get(request_timeout, Config),
            InitId = NextId,
            NextAfterInit = advance_id(InitId),
            case encode_init(InitId) of
                {ok, Frame} ->
                    case glifistore_conn:exchange(Conn, Frame, Deadline) of
                        {ok, Response} ->
                            case validate_init(Response, InitId, Expected) of
                                {ok, Routing} ->
                                    bind_worker(Conn, Worker, Response, Routing, Deadline, NextAfterInit);
                                {error, Err} ->
                                    glifistore_conn:reset(Conn),
                                    {error, Err}
                            end;
                        {error, SF = #{send_failure := true}} ->
                            glifistore_conn:reset(Conn),
                            {error, promote_send(SF)};
                        {error, Err} ->
                            glifistore_conn:reset(Conn),
                            {error, maybe_unavailable(Err)}
                    end;
                {error, Err} ->
                    glifistore_conn:reset(Conn),
                    {error, Err}
            end;
        {error, Err} ->
            {error, Err}
    end.

bind_worker(Conn, Worker, InitResponse, Routing, Deadline, BindId) ->
    case glifistore_protocol:encode_request(
        glifistore_protocol:opcode_bind_worker(), BindId, <<>>, <<>>, 0, Worker
    ) of
        {ok, Frame} ->
            case glifistore_conn:exchange(Conn, Frame, Deadline) of
                {ok, Bound} ->
                    case validate_bind(Bound, BindId, Worker, InitResponse) of
                        ok ->
                            {ok, maps:get(worker_count, InitResponse), maps:get(routing_epoch, InitResponse),
                                Routing, advance_id(BindId)};
                        {error, Err} ->
                            glifistore_conn:reset(Conn),
                            {error, Err}
                    end;
                {error, SF = #{send_failure := true}} ->
                    glifistore_conn:reset(Conn),
                    {error, promote_send(SF)};
                {error, Err} ->
                    glifistore_conn:reset(Conn),
                    {error, maybe_unavailable(Err)}
            end;
        {error, {invalid_argument, Msg}} ->
            {error, glifistore_error:invalid_argument(Msg)}
    end.

advance_id(Id) ->
    case Id of
        16#FFFFFFFFFFFFFFFF -> 1;
        N -> N + 1
    end.

bump_id(State) ->
    Id = State#state.request_id,
    {Id, State#state{request_id = advance_id(Id)}}.

%% ---------------------------------------------------------------------------
%% Dispatch: resolve quickly, then delegate I/O off the coordinator callback.
%% ---------------------------------------------------------------------------

dispatch_read(_Op, _Key, _Value, _Opts, _From, #state{healthy = false} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_read(_Op, _Key, _Value, _Opts, _From, #state{closing = true} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_read(Op, Key, Value, Opts, From, State) ->
    case resolve_deadline(State, Opts) of
        {ok, Deadline} ->
            {Opcode, OpName, RouteKey} = read_op(Op, Key),
            WorkerPick =
                case Op of
                    ping -> worker_index_fixed(State, 0);
                    health -> worker_index_fixed(State, 0);
                    ready -> worker_index_fixed(State, 0);
                    stats -> worker_index_fixed(State, 0);
                    backup -> worker_index_fixed(State, 0);
                    _ -> worker_index(State, RouteKey)
                end,
            case WorkerPick of
                {ok, Worker, Conn} ->
                    launch_read(Opcode, OpName, Key, Value, Deadline, Worker, Conn, From, State, 2);
                {error, Err} ->
                    {reply, {error, Err}, State}
            end;
        {error, Err} ->
            {reply, {error, Err}, State}
    end.

launch_read(_Opcode, _OpName, _Key, _Value, _Deadline, _Worker, _Conn, From, State, 0) ->
    finish_reply(From, {error, glifistore_error:unavailable(<<"request was not attempted">>)}, State);
launch_read(Opcode, OpName, Key, Value, Deadline, Worker, Conn, From, State, Attempts) ->
    case ensure_connected(Conn, Worker, State) of
        {ok, State1} ->
            Conn1 = maps:get(Worker, State1#state.workers),
            {RequestId, State2} = bump_id(State1),
            case encode_request(Opcode, RequestId, Key, Value, 0, State2) of
                {ok, Frame, State3} ->
                    Meta = snapshot_meta(State3),
                    start_io(
                        From,
                        State3,
                        Deadline,
                        fun() -> glifistore_conn:exchange(Conn1, Frame, Deadline) end,
                        #{
                            type => read,
                            opcode => Opcode,
                            op_name => OpName,
                            key => Key,
                            value => Value,
                            deadline => Deadline,
                            worker => Worker,
                            conn => Conn1,
                            request_id => RequestId,
                            attempts => Attempts,
                            frame_bytes => byte_size(Frame),
                            meta => Meta
                        }
                    );
                {{error, Err}, State3} ->
                    finish_reply(From, {error, Err}, State3)
            end;
        {{error, Err}, State1} ->
            case Attempts > 1 andalso retryable_read(Err, State1) of
                true ->
                    launch_read(Opcode, OpName, Key, Value, Deadline, Worker, Conn, From, State1, Attempts - 1);
                false ->
                    finish_reply(From, {error, Err}, State1)
            end
    end.

dispatch_mutate(Op, _Key, _Value, _Expire, _Opts, _From, #state{healthy = false} = State) ->
    {reply, mutation_rejected(Op, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>), State), State};
dispatch_mutate(Op, _Key, _Value, _Expire, _Opts, _From, #state{closing = true} = State) ->
    {reply, mutation_rejected(Op, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>), State), State};
dispatch_mutate(Op, Key, Value, Expire, Opts, From, State) ->
    OpName = atom_to_binary(Op, utf8),
    case resolve_deadline(State, Opts) of
        {ok, Deadline} ->
            case worker_index(State, Key) of
                {ok, Worker, Conn} ->
                    launch_mutate(Op, OpName, Key, Value, Expire, Deadline, Worker, Conn, From, State, 2);
                {error, Err} ->
                    {reply, mutation_rejected(Op, Err, State), State}
            end;
        {error, Err} ->
            {reply, mutation_rejected(Op, Err, State), State}
    end.

launch_mutate(_Op, OpName, _Key, _Value, _Expire, _Deadline, Worker, _Conn, From, State, 0) ->
    Err = enrich_mutation(
        annotate(glifistore_error:unavailable(<<"could not send mutation">>), OpName, undefined, Worker, State),
        rejected
    ),
    finish_reply(From, #{outcome => rejected, error => Err}, State);
launch_mutate(Op, OpName, Key, Value, Expire, Deadline, Worker, Conn, From, State, Attempts) ->
    case ensure_connected(Conn, Worker, State) of
        {ok, State1} ->
            Conn1 = maps:get(Worker, State1#state.workers),
            {RequestId, State2} = bump_id(State1),
            Opcode = mutate_opcode(Op),
            case encode_request(Opcode, RequestId, Key, Value, Expire, State2) of
                {ok, Frame, State3} ->
                    Meta = snapshot_meta(State3),
                    start_io(
                        From,
                        State3,
                        Deadline,
                        fun() -> glifistore_conn:exchange(Conn1, Frame, Deadline) end,
                        #{
                            type => mutate,
                            op => Op,
                            op_name => OpName,
                            key => Key,
                            value => Value,
                            expire => Expire,
                            deadline => Deadline,
                            worker => Worker,
                            conn => Conn1,
                            request_id => RequestId,
                            attempts => Attempts,
                            frame_bytes => byte_size(Frame),
                            meta => Meta
                        }
                    );
                {{error, Err}, State3} ->
                    finish_reply(
                        From,
                        mutation_rejected(Op, annotate(Err, OpName, RequestId, Worker, State3), State3),
                        State3
                    )
            end;
        {{error, Err}, State1} ->
            finish_reply(
                From,
                mutation_rejected(Op, annotate(Err, OpName, undefined, Worker, State1), State1),
                State1
            )
    end.

dispatch_pipeline([], _Opts, _From, State) ->
    {reply, {ok, []}, State};
dispatch_pipeline(_R, _O, _From, #state{healthy = false} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_pipeline(_R, _O, _From, #state{closing = true} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_pipeline(Requests, Opts, From, State) ->
    case resolve_deadline(State, Opts) of
        {ok, Deadline} ->
            case plan_pipeline(Requests, State) of
                {ok, Worker, Conn, Plan, State1} ->
                    launch_pipeline(Plan, Deadline, Worker, Conn, From, State1, length(Requests));
                {error, Err} ->
                    {reply, {error, Err}, State}
            end;
        {error, Err} ->
            {reply, {error, Err}, State}
    end.

launch_pipeline(Plan, Deadline, Worker, Conn, From, State, Count) ->
    case ensure_connected(Conn, Worker, State) of
        {ok, State1} ->
            Conn1 = maps:get(Worker, State1#state.workers),
            Meta = snapshot_meta(State1),
            start_io(
                From,
                State1,
                Deadline,
                fun() -> run_pipeline_io(Plan, Deadline, Worker, Conn1, Meta, Count) end,
                #{
                    type => pipeline,
                    deadline => Deadline,
                    worker => Worker,
                    conn => Conn1,
                    meta => Meta,
                    plan => Plan
                }
            );
        {{error, Err}, State1} ->
            Responses = array:new(Count, {default, failed_response()}),
            finish_reply(
                From,
                {ok, array:to_list(mark_unresolved(Responses, 0, Err, 0, metadata_from_plan(Plan)))},
                State1
            )
    end.

dispatch_batch([], _Opts, _From, State) ->
    {reply, {ok, []}, State};
dispatch_batch(_R, _O, _From, #state{healthy = false} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_batch(_R, _O, _From, #state{closing = true} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_batch(Requests, Opts, From, State) ->
    case resolve_deadline(State, Opts) of
        {ok, Deadline} ->
            case group_batch(Requests, State) of
                {ok, Groups} ->
                    launch_batch(Groups, Deadline, From, State, length(Requests));
                {error, Err} ->
                    {reply, {error, Err}, State}
            end;
        {error, Err} ->
            {reply, {error, Err}, State}
    end.

launch_batch(Groups, Deadline, From, State, Count) ->
    Responses0 = array:new(Count, {default, failed_response()}),
    Merge = fun(_Worker, Items, GroupResponses, Acc) ->
        merge_group_by_index(Acc, Items, GroupResponses)
    end,
    case prepare_batch_groups(Groups, State, []) of
        {ok, Prepared, State1} ->
            %% Per-Worker connect like Python/Go execute_pipeline: a rebind failure
            %% stamps only that Worker's slots and still fans out siblings.
            {Ready, Acc1, State2} = connect_prepared_groups(Prepared, Responses0, Merge, State1),
            fanout_prepared(Ready, Deadline, From, State2, Merge, Acc1, batch);
        {error, Err} ->
            {reply, {error, Err}, State}
    end.

dispatch_worker_pipelines(_Batches, _Opts, _From, #state{healthy = false} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_worker_pipelines(_Batches, _Opts, _From, #state{closing = true} = State) ->
    {reply, {error, glifistore_error:unavailable(<<"client is closed or routing metadata changed">>)}, State};
dispatch_worker_pipelines(Batches, Opts, From, State) when is_list(Batches) ->
    WC = State#state.worker_count,
    case length(Batches) =:= WC of
        false ->
            {reply, {error, glifistore_error:invalid_argument(<<"worker pipeline vector does not match Worker count">>)}, State};
        true ->
            case resolve_deadline(State, Opts) of
                {ok, Deadline} ->
                    case plan_worker_pipelines(Batches, 0, State, []) of
                        {ok, Prepared, State1} ->
                            Empty = array:new(WC, {default, []}),
                            Merge = fun(Worker, _Items, GroupResponses, Acc) ->
                                array:set(Worker, GroupResponses, Acc)
                            end,
                            {Ready, Acc1, State2} = connect_prepared_groups(
                                Prepared, Empty, Merge, State1
                            ),
                            fanout_prepared(
                                Ready, Deadline, From, State2, Merge, Acc1, worker_pipelines
                            );
                        {error, Err} ->
                            {reply, {error, Err}, State}
                    end;
                {error, Err} ->
                    {reply, {error, Err}, State}
            end
    end;
dispatch_worker_pipelines(_Batches, _Opts, _From, State) ->
    {reply, {error, glifistore_error:invalid_argument(<<"worker pipelines must be a list">>)}, State}.

%% ---------------------------------------------------------------------------
%% Monitored I/O helpers
%% ---------------------------------------------------------------------------

start_io(From, State, Deadline, IoFun, Pending0) ->
    Client = self(),
    Tag = make_ref(),
    {Pid, Mon} = spawn_monitor(fun() ->
        Result =
            try IoFun() of
                R -> {ok, R}
            catch
                Class:Reason:Stack ->
                    {crash, Class, Reason, Stack}
            end,
        Client ! {io_result, Tag, Result}
    end),
    Timer = start_deadline_timer(Tag, Deadline),
    Pending = Pending0#{
        from => From,
        pid => Pid,
        mon => Mon,
        tag => Tag,
        timer => Timer
    },
    State1 = track_pending(Tag, Mon, Pending, State),
    {noreply, State1}.

track_pending(Tag, Mon, Pending, State) ->
    State#state{
        pending = maps:put(Tag, Pending, State#state.pending),
        mon_index = maps:put(Mon, Tag, State#state.mon_index)
    }.

untrack_pending(Tag, State) ->
    case maps:take(Tag, State#state.pending) of
        {Pending, PendingMap} ->
            Mon = maps:get(mon, Pending, undefined),
            cancel_pending_timer(Pending),
            MonIndex =
                case Mon of
                    undefined -> State#state.mon_index;
                    _ -> maps:remove(Mon, State#state.mon_index)
                end,
            Children = maps:get(children, Pending, #{}),
            MonIndex1 = maps:fold(
                fun(ChildMon, _, Acc) -> maps:remove(ChildMon, Acc) end,
                MonIndex,
                Children
            ),
            {Pending, State#state{pending = PendingMap, mon_index = MonIndex1}};
        error ->
            error
    end.

start_deadline_timer(Tag, Deadline) ->
    case glifistore_util:remaining_timeout(Deadline) of
        {ok, Left} ->
            erlang:start_timer(glifistore_util:timeout_ms(Left), self(), {op_deadline, Tag});
        {error, _} ->
            erlang:start_timer(1, self(), {op_deadline, Tag})
    end.

cancel_pending_timer(Pending) ->
    case maps:get(timer, Pending, undefined) of
        undefined -> ok;
        TimerRef -> erlang:cancel_timer(TimerRef), ok
    end.

finish_reply(From, Reply, State) ->
    gen_server:reply(From, Reply),
    maybe_finish_close(State).

snapshot_meta(State) ->
    #{
        config => State#state.config,
        worker_count => State#state.worker_count,
        routing_epoch => State#state.routing_epoch,
        healthy => State#state.healthy
    }.

meta_state(Meta) ->
    #state{
        config = maps:get(config, Meta),
        worker_count = maps:get(worker_count, Meta),
        routing_epoch = maps:get(routing_epoch, Meta),
        healthy = maps:get(healthy, Meta, true)
    }.

begin_close(From, State) ->
    State1 = State#state{closing = true, healthy = false, close_from = From},
    maps:foreach(fun(_W, Pid) -> glifistore_conn:reset(Pid) end, State1#state.workers),
    case maps:size(State1#state.pending) of
        0 ->
            {stop, normal, ok, State1#state{close_from = undefined}};
        _ ->
            {noreply, State1}
    end.

maybe_finish_close(State) ->
    case State#state.closing andalso maps:size(State#state.pending) =:= 0 of
        true ->
            case State#state.close_from of
                undefined ->
                    {stop, normal, State};
                From ->
                    gen_server:reply(From, ok),
                    {stop, normal, State#state{close_from = undefined}}
            end;
        false ->
            {noreply, State}
    end.

handle_io_result(Tag, Result, State) ->
    case untrack_pending(Tag, State) of
        error ->
            {noreply, State};
        {Pending, State1} ->
            _ = erlang:demonitor(maps:get(mon, Pending), [flush]),
            case maps:get(type, Pending) of
                read -> complete_read(Pending, Result, State1);
                mutate -> complete_mutate(Pending, Result, State1);
                pipeline -> complete_pipeline(Pending, Result, State1)
            end
    end.

handle_down(Mon, Reason, State) ->
    case maps:get(Mon, State#state.mon_index, undefined) of
        undefined ->
            {noreply, State};
        Tag ->
            case maps:get(Tag, State#state.pending, undefined) of
                undefined ->
                    {noreply, State#state{mon_index = maps:remove(Mon, State#state.mon_index)}};
                Pending ->
                    case maps:get(type, Pending) of
                        fanout ->
                            handle_fanout_down(Tag, Mon, Reason, Pending, State);
                        _ ->
                            case Reason of
                                normal ->
                                    %% Result message should arrive (or already did). Keep pending.
                                    {noreply, State};
                                _ ->
                                    case untrack_pending(Tag, State) of
                                        error ->
                                            {noreply, State};
                                        {P, State1} ->
                                            fail_pending_crash(P, Reason, State1)
                                    end
                            end
                    end
            end
    end.

handle_op_timeout(Tag, TimerRef, State) ->
    case maps:get(Tag, State#state.pending, undefined) of
        undefined ->
            {noreply, State};
        Pending ->
            case maps:get(timer, Pending, undefined) of
                TimerRef ->
                    case maps:get(type, Pending) of
                        fanout ->
                            timeout_fanout(Tag, Pending, State);
                        _ ->
                            case untrack_pending(Tag, State) of
                                error ->
                                    {noreply, State};
                                {P, State1} ->
                                    kill_pending_pid(P),
                                    maybe_reset_conn(P),
                                    fail_pending_timeout(P, State1)
                            end
                    end;
                _ ->
                    {noreply, State}
            end
    end.

kill_pending_pid(Pending) ->
    case maps:get(pid, Pending, undefined) of
        undefined -> ok;
        Pid -> exit(Pid, kill)
    end.

maybe_reset_conn(Pending) ->
    case maps:get(conn, Pending, undefined) of
        undefined -> ok;
        Conn -> glifistore_conn:reset(Conn)
    end.

fail_pending_crash(Pending, _Reason, State) ->
    maybe_reset_conn(Pending),
    From = maps:get(from, Pending),
    case maps:get(type, Pending) of
        read ->
            Ann0 = annotate(
                glifistore_error:transport(<<"worker I/O process crashed">>),
                maps:get(op_name, Pending),
                maps:get(request_id, Pending, undefined),
                maps:get(worker, Pending),
                State
            ),
            %% Match timeout / send-failure: BACKUP after launch is not same-destination
            %% idempotent — crash mid-receive must not advertise same_request.
            BackupOpcode = glifistore_protocol:opcode_backup(),
            Ann = case maps:get(opcode, Pending, undefined) of
                BackupOpcode ->
                    FrameBytes = maps:get(frame_bytes, Pending, 1),
                    enrich_mutation(glifistore_error:enrich(Ann0, #{bytes_sent => FrameBytes}), indeterminate);
                _ ->
                    Ann0
            end,
            finish_reply(From, {error, Ann}, State);
        mutate ->
            FrameBytes = maps:get(frame_bytes, Pending, 1),
            Err = enrich_mutation(
                glifistore_error:enrich(
                    annotate(glifistore_error:transport(<<"worker I/O process crashed">>), maps:get(op_name, Pending), maps:get(request_id, Pending, undefined), maps:get(worker, Pending), State),
                    #{bytes_sent => FrameBytes}
                ),
                indeterminate
            ),
            finish_reply(From, #{outcome => indeterminate, error => Err}, State);
        pipeline ->
            finish_reply(
                From,
                pipeline_outer_failure_reply(
                    Pending,
                    glifistore_error:transport(<<"worker I/O process crashed">>)
                ),
                State
            )
    end.

fail_pending_timeout(Pending, State) ->
    From = maps:get(from, Pending),
    Err = glifistore_error:transport(<<"request deadline expired">>),
    case maps:get(type, Pending) of
        read ->
            Ann0 = annotate(Err, maps:get(op_name, Pending), maps:get(request_id, Pending, undefined), maps:get(worker, Pending), State),
            BackupOpcode = glifistore_protocol:opcode_backup(),
            Ann = case maps:get(opcode, Pending, undefined) of
                BackupOpcode ->
                    FrameBytes = maps:get(frame_bytes, Pending, 1),
                    enrich_mutation(glifistore_error:enrich(Ann0, #{bytes_sent => FrameBytes}), indeterminate);
                _ ->
                    Ann0
            end,
            finish_reply(From, {error, Ann}, State);
        mutate ->
            %% Timeout while waiting: bytes may have been sent → indeterminate.
            FrameBytes = maps:get(frame_bytes, Pending, 1),
            Ann = enrich_mutation(
                glifistore_error:enrich(
                    annotate(Err, maps:get(op_name, Pending), maps:get(request_id, Pending, undefined), maps:get(worker, Pending), State),
                    #{bytes_sent => FrameBytes}
                ),
                indeterminate
            ),
            finish_reply(From, #{outcome => indeterminate, error => Ann}, State);
        pipeline ->
            finish_reply(From, pipeline_outer_failure_reply(Pending, Err), State)
    end.

complete_read(Pending, {crash, _C, _R, _S}, State) ->
    fail_pending_crash(Pending, crash, State);
complete_read(Pending, {ok, IoResult}, State) ->
    OpName = maps:get(op_name, Pending),
    RequestId = maps:get(request_id, Pending),
    Worker = maps:get(worker, Pending),
    Conn = maps:get(conn, Pending),
    Opcode = maps:get(opcode, Pending),
    Key = maps:get(key, Pending),
    Value = maps:get(value, Pending),
    Deadline = maps:get(deadline, Pending),
    Attempts = maps:get(attempts, Pending),
    From = maps:get(from, Pending),
    FrameBytes = maps:get(frame_bytes, Pending, 1),
    case IoResult of
        {ok, Response} ->
            {Reply, State1} = handle_read_response(Response, OpName, RequestId, Worker, Conn, FrameBytes, State),
            finish_reply(From, Reply, State1);
        {error, SF = #{send_failure := true}} ->
            glifistore_conn:reset(Conn),
            Bytes = maps:get(bytes_sent, SF, 0),
            IsBackup = Opcode =:= glifistore_protocol:opcode_backup(),
            case IsBackup andalso Bytes > 0 of
                true ->
                    Err = enrich_mutation(promote_send_sf(SF, OpName, RequestId, Worker, State, true), indeterminate),
                    finish_reply(From, {error, Err}, State);
                false ->
                    Err = promote_send_sf(SF, OpName, RequestId, Worker, State, false),
                    case Attempts > 1 andalso retryable_read(Err, State) of
                        true ->
                            launch_read(Opcode, OpName, Key, Value, Deadline, Worker, Conn, From, State, Attempts - 1);
                        false ->
                            finish_reply(From, {error, Err}, State)
                    end
            end;
        {error, Err} ->
            glifistore_conn:reset(Conn),
            IsBackup = Opcode =:= glifistore_protocol:opcode_backup(),
            case IsBackup of
                true ->
                    %% Receive loss after send — do not blind-retry BACKUP.
                    Ann = enrich_mutation(
                        glifistore_error:enrich(
                            annotate(Err, OpName, RequestId, Worker, State),
                            #{bytes_sent => FrameBytes}
                        ),
                        indeterminate
                    ),
                    finish_reply(From, {error, Ann}, State);
                false ->
                    Ann = annotate(Err, OpName, RequestId, Worker, State),
                    case Attempts > 1 andalso retryable_read(Ann, State) of
                        true ->
                            launch_read(Opcode, OpName, Key, Value, Deadline, Worker, Conn, From, State, Attempts - 1);
                        false ->
                            finish_reply(From, {error, Ann}, State)
                    end
            end
    end.

complete_mutate(Pending, {crash, _C, _R, _S}, State) ->
    fail_pending_crash(Pending, crash, State);
complete_mutate(Pending, {ok, IoResult}, State) ->
    Op = maps:get(op, Pending),
    OpName = maps:get(op_name, Pending),
    RequestId = maps:get(request_id, Pending),
    Worker = maps:get(worker, Pending),
    Conn = maps:get(conn, Pending),
    Key = maps:get(key, Pending),
    Value = maps:get(value, Pending),
    Expire = maps:get(expire, Pending),
    Deadline = maps:get(deadline, Pending),
    Attempts = maps:get(attempts, Pending),
    From = maps:get(from, Pending),
    FrameBytes = maps:get(frame_bytes, Pending, 1),
    case IoResult of
        {ok, Response} ->
            {Reply, State1} = handle_mutate_response(Response, OpName, RequestId, Worker, Conn, FrameBytes, State),
            finish_reply(From, Reply, State1);
        {error, SF = #{send_failure := true}} ->
            glifistore_conn:reset(Conn),
            case maps:get(bytes_sent, SF, 0) of
                0 when Attempts > 1 ->
                    launch_mutate(Op, OpName, Key, Value, Expire, Deadline, Worker, Conn, From, State, Attempts - 1);
                0 ->
                    Err = enrich_mutation(promote_send_sf(SF, OpName, RequestId, Worker, State, true), rejected),
                    finish_reply(From, #{outcome => rejected, error => Err}, State);
                _ ->
                    Err = enrich_mutation(promote_send_sf(SF, OpName, RequestId, Worker, State, true), indeterminate),
                    finish_reply(From, #{outcome => indeterminate, error => Err}, State)
            end;
        {error, Err} ->
            glifistore_conn:reset(Conn),
            Ann = enrich_mutation(
                glifistore_error:enrich(annotate(Err, OpName, RequestId, Worker, State), #{bytes_sent => FrameBytes}),
                indeterminate
            ),
            finish_reply(From, #{outcome => indeterminate, error => Ann}, State)
    end.

complete_pipeline(Pending, {crash, _C, _R, _S}, State) ->
    fail_pending_crash(Pending, crash, State);
complete_pipeline(Pending, {ok, {ok, Responses, Healthy}}, State) ->
    State1 =
        case Healthy of
            true -> State;
            false -> State#state{healthy = false}
        end,
    finish_reply(maps:get(from, Pending), {ok, Responses}, State1);
complete_pipeline(Pending, {ok, {error, Err}}, State) ->
    maybe_reset_conn(Pending),
    %% I/O may have started; classify per-slot like an outer kill (§6.1).
    finish_reply(maps:get(from, Pending), pipeline_outer_failure_reply(Pending, Err), State).

%% ---------------------------------------------------------------------------
%% Pipeline I/O (runs in delegated process; returns {ok, Responses, Healthy})
%% ---------------------------------------------------------------------------

run_pipeline_io(Plan, Deadline, Worker, Conn, Meta, Count) ->
    Responses = array:new(Count, {default, failed_response()}),
    Output = [maps:get(frame, I) || I <- Plan],
    TotalBytes = iolist_size(Output),
    Metadata = [{maps:get(request_id, I), maps:get(req, I), maps:get(begin_offset, I)} || I <- Plan],
    State0 = meta_state(Meta),
    case glifistore_conn:run_pipeline(Conn, Output, Count, Deadline) of
        {ok, RawResponses} ->
            fold_pipeline_responses(RawResponses, Metadata, 0, Responses, Worker, Conn, State0, TotalBytes);
        {send_error, SF = #{send_failure := true}} ->
            glifistore_conn:reset(Conn),
            {ok,
                array:to_list(
                    mark_unresolved(
                        Responses,
                        0,
                        promote_send_sf(SF, undefined, undefined, Worker, State0, false),
                        maps:get(bytes_sent, SF, 0),
                        Metadata
                    )
                ),
                State0#state.healthy};
        {send_error, Err} ->
            glifistore_conn:reset(Conn),
            {ok, array:to_list(mark_unresolved(Responses, 0, Err, 0, Metadata)), State0#state.healthy};
        {recv_error, Err, Partial} ->
            glifistore_conn:reset(Conn),
            {ok, PartialList, Healthy} =
                fold_pipeline_responses(
                    Partial,
                    lists:sublist(Metadata, length(Partial)),
                    0,
                    Responses,
                    Worker,
                    Conn,
                    State0,
                    TotalBytes
                ),
            PartialArr = array:from_list(PartialList),
            Start = length(Partial),
            {ok,
                array:to_list(mark_unresolved(PartialArr, Start, Err, TotalBytes, lists:nthtail(Start, Metadata))),
                Healthy}
    end.

fold_pipeline_responses([], [], _Idx, Responses, _Worker, _Conn, State, _Sent) ->
    {ok, array:to_list(Responses), State#state.healthy};
fold_pipeline_responses([Response | RestResp], [{RequestId, Req, Begin} | RestMeta], Idx, Responses, Worker, Conn, State, SentBytes) ->
    case validate_response(Response, RequestId, Worker, State) of
        ok ->
            case maps:get(status, Response) of
                ?GS_ST_OK ->
                    case pipeline_ok(Req, Response) of
                        ok ->
                            Responses1 = array:set(
                                Idx,
                                #{outcome => succeeded, value => maps:get(value, Response)},
                                Responses
                            ),
                            fold_pipeline_responses(
                                RestResp, RestMeta, Idx + 1, Responses1, Worker, Conn, State, SentBytes
                            );
                        {error, Err} ->
                            glifistore_conn:reset(Conn),
                            {ok,
                                array:to_list(
                                    mark_unresolved(Responses, Idx, Err, SentBytes, [
                                        {RequestId, Req, Begin} | RestMeta
                                    ])
                                ),
                                State#state.healthy}
                    end;
                Status ->
                    OpName = pipeline_op(Req),
                    Outcome = pipeline_status_outcome(Req, Status),
                    IsMut = maps:get(op, Req) =:= put orelse maps:get(op, Req) =:= erase,
                    Fields0 = #{bytes_sent => SentBytes - Begin},
                    Fields =
                        case IsMut of
                            true ->
                                Fields0#{
                                    mutation_outcome =>
                                        case Outcome of
                                            indeterminate -> indeterminate;
                                            _ -> rejected
                                        end
                                };
                            false ->
                                Fields0
                        end,
                    Err = glifistore_error:enrich(
                        status_err(Status, OpName, RequestId, Worker, State), Fields
                    ),
                    Responses1 = array:set(Idx, #{outcome => Outcome, error => Err}, Responses),
                    State1 = maybe_unhealthy_state(Status, State),
                    fold_pipeline_responses(
                        RestResp, RestMeta, Idx + 1, Responses1, Worker, Conn, State1, SentBytes
                    )
            end;
        {error, Err} ->
            glifistore_conn:reset(Conn),
            {ok,
                array:to_list(
                    mark_unresolved(Responses, Idx, Err, SentBytes, [{RequestId, Req, Begin} | RestMeta])
                ),
                State#state.healthy}
    end.

%% ---------------------------------------------------------------------------
%% Fan-out with spawn_monitor (batch / worker pipelines)
%% ---------------------------------------------------------------------------

fanout_prepared([], _Deadline, _From, State, _Merge, Acc, Kind) ->
    Reply =
        case Kind of
            batch -> {ok, array:to_list(Acc)};
            worker_pipelines -> {ok, array:to_list(Acc)}
        end,
    {reply, Reply, State};
fanout_prepared([{Worker, Items, Plan}], Deadline, From, State, Merge, Acc, Kind) ->
    %% Single Worker: one monitored I/O process (still off the coordinator callback).
    Conn = maps:get(Worker, State#state.workers),
    Meta = snapshot_meta(State),
    start_io(
        From,
        State,
        Deadline,
        fun() ->
            {ok, GroupResponses, Healthy} = run_pipeline_io(Plan, Deadline, Worker, Conn, Meta, length(Plan)),
            Acc1 = Merge(Worker, Items, GroupResponses, Acc),
            case Kind of
                batch -> {ok, array:to_list(Acc1), Healthy};
                worker_pipelines -> {ok, array:to_list(Acc1), Healthy}
            end
        end,
        #{
            type => pipeline,
            deadline => Deadline,
            worker => Worker,
            conn => Conn,
            meta => Meta,
            plan => Plan,
            items => Items,
            merge => Merge,
            acc => Acc,
            kind => Kind
        }
    );
fanout_prepared(Prepared, Deadline, From, State, Merge, Acc0, Kind) ->
    Client = self(),
    Tag = make_ref(),
    Meta = snapshot_meta(State),
    {Children, MonIndex} = lists:foldl(
        fun({Worker, Items, Plan}, {ChAcc, MonAcc}) ->
            Conn = maps:get(Worker, State#state.workers),
            {Pid, Mon} = spawn_monitor(fun() ->
                receive
                    {start, ResultMon} ->
                        {ok, GroupResponses, Healthy} =
                            run_pipeline_io(Plan, Deadline, Worker, Conn, Meta, length(Plan)),
                        Client !
                            {fanout_result, Tag, ResultMon, Worker, Items, GroupResponses, Healthy}
                end
            end),
            Pid ! {start, Mon},
            {
                ChAcc#{
                    Mon => #{
                        worker => Worker,
                        items => Items,
                        plan => Plan,
                        pid => Pid,
                        conn => Conn
                    }
                },
                MonAcc#{Mon => Tag}
            }
        end,
        {#{}, State#state.mon_index},
        Prepared
    ),
    Timer = start_deadline_timer(Tag, Deadline),
    Pending = #{
        type => fanout,
        kind => Kind,
        from => From,
        tag => Tag,
        children => Children,
        merge => Merge,
        acc => Acc0,
        timer => Timer,
        deadline => Deadline
    },
    State1 = State#state{
        pending = maps:put(Tag, Pending, State#state.pending),
        mon_index = MonIndex
    },
    {noreply, State1}.

handle_fanout_result(Tag, Mon, Worker, Items, GroupResponses, Healthy, State) ->
    case maps:get(Tag, State#state.pending, undefined) of
        undefined ->
            {noreply, State};
        Pending ->
            Children0 = maps:get(children, Pending),
            case maps:take(Mon, Children0) of
                error ->
                    {noreply, State};
                {_Child, Children1} ->
                    _ = erlang:demonitor(Mon, [flush]),
                    Acc1 = (maps:get(merge, Pending))(Worker, Items, GroupResponses, maps:get(acc, Pending)),
                    StateH =
                        case Healthy of
                            true -> State;
                            false -> State#state{healthy = false}
                        end,
                    Pending1 = Pending#{children := Children1, acc := Acc1},
                    State1 = StateH#state{
                        pending = maps:put(Tag, Pending1, StateH#state.pending),
                        mon_index = maps:remove(Mon, StateH#state.mon_index)
                    },
                    case maps:size(Children1) of
                        0 ->
                            finish_fanout(Tag, Pending1, State1);
                        _ ->
                            {noreply, State1}
                    end
            end
    end.

handle_fanout_down(Tag, Mon, Reason, Pending, State) ->
    Children0 = maps:get(children, Pending),
    case maps:take(Mon, Children0) of
        error ->
            {noreply, State};
        {Child, Children1} ->
            case Reason of
                normal ->
                    %% Result should follow; keep waiting unless already removed.
                    {noreply, State};
                _ ->
                    glifistore_conn:reset(maps:get(conn, Child)),
                    Worker = maps:get(worker, Child),
                    Items = maps:get(items, Child),
                    Plan = maps:get(plan, Child),
                    CrashResponses = classify_pipeline_after_io(
                        Plan,
                        glifistore_error:transport(<<"worker I/O process crashed">>)
                    ),
                    Acc1 = (maps:get(merge, Pending))(Worker, Items, CrashResponses, maps:get(acc, Pending)),
                    Pending1 = Pending#{children := Children1, acc := Acc1},
                    State1 = State#state{
                        pending = maps:put(Tag, Pending1, State#state.pending),
                        mon_index = maps:remove(Mon, State#state.mon_index)
                    },
                    case maps:size(Children1) of
                        0 -> finish_fanout(Tag, Pending1, State1);
                        _ -> {noreply, State1}
                    end
            end
    end.

timeout_fanout(Tag, Pending, State) ->
    maps:foreach(
        fun(Mon, Child) ->
            exit(maps:get(pid, Child), kill),
            glifistore_conn:reset(maps:get(conn, Child)),
            _ = erlang:demonitor(Mon, [flush])
        end,
        maps:get(children, Pending)
    ),
    case untrack_pending(Tag, State) of
        error ->
            {noreply, State};
        {P, State1} ->
            Reply1 = fanout_timeout_reply(P, maps:get(acc, P)),
            finish_reply(maps:get(from, P), Reply1, State1)
    end.

fanout_timeout_reply(Pending, Acc) ->
    Err = glifistore_error:transport(<<"request deadline expired">>),
    Kind = maps:get(kind, Pending),
    Children = maps:get(children, Pending),
    Acc1 = maps:fold(
        fun(_Mon, Child, A) ->
            Worker = maps:get(worker, Child),
            Items = maps:get(items, Child),
            Plan = maps:get(plan, Child),
            Classified = classify_pipeline_after_io(Plan, Err),
            (maps:get(merge, Pending))(Worker, Items, Classified, A)
        end,
        Acc,
        Children
    ),
    case Kind of
        batch -> {ok, array:to_list(Acc1)};
        worker_pipelines -> {ok, array:to_list(Acc1)}
    end.

finish_fanout(Tag, _Pending, State) ->
    case untrack_pending(Tag, State) of
        error ->
            {noreply, State};
        {P, State1} ->
            Kind = maps:get(kind, P),
            Acc = maps:get(acc, P),
            Reply =
                case Kind of
                    batch -> {ok, array:to_list(Acc)};
                    worker_pipelines -> {ok, array:to_list(Acc)}
                end,
            finish_reply(maps:get(from, P), Reply, State1)
    end.

handle_read_response(Response, OpName, RequestId, Worker, Conn, FrameBytes, State) ->
    case validate_response(Response, RequestId, Worker, State) of
        ok ->
            case maps:get(status, Response) of
                ?GS_ST_OK ->
                    {{ok, maps:get(value, Response)}, State};
                Status ->
                    Err = status_err(Status, OpName, RequestId, Worker, State),
                    {State1, Final} = apply_unhealthy(Status, Err, State),
                    %% BACKUP INTERNAL_ERROR may mean the fenced copy already committed.
                    Final1 =
                        case {OpName, Status} of
                            {<<"backup">>, ?GS_ST_INTERNAL} ->
                                enrich_mutation(
                                    glifistore_error:enrich(Final, #{bytes_sent => FrameBytes}),
                                    indeterminate
                                );
                            _ ->
                                Final
                        end,
                    {{error, Final1}, State1}
            end;
        {error, Err} ->
            glifistore_conn:reset(Conn),
            State1 = mark_unhealthy_if_metadata(Err, State),
            Ann0 = annotate(Err, OpName, RequestId, Worker, State1),
            %% BACKUP validate failure after a framed response — fenced copy may exist.
            Ann =
                case OpName of
                    <<"backup">> ->
                        enrich_mutation(
                            glifistore_error:enrich(Ann0, #{bytes_sent => FrameBytes}),
                            indeterminate
                        );
                    _ ->
                        Ann0
                end,
            {{error, Ann}, State1}
    end.

handle_mutate_response(Response, OpName, RequestId, Worker, Conn, FrameBytes, State) ->
    case validate_response(Response, RequestId, Worker, State) of
        ok ->
            case maps:get(status, Response) of
                ?GS_ST_OK ->
                    case maps:get(value, Response) of
                        <<>> -> {#{outcome => committed}, State};
                        _ ->
                            glifistore_conn:reset(Conn),
                            Err = enrich_mutation(
                                glifistore_error:enrich(
                                    annotate(
                                        glifistore_error:protocol(<<"mutation response value must be empty">>),
                                        OpName,
                                        RequestId,
                                        Worker,
                                        State
                                    ),
                                    #{bytes_sent => FrameBytes}
                                ),
                                indeterminate
                            ),
                            {#{outcome => indeterminate, error => Err}, State}
                    end;
                ?GS_ST_INTERNAL ->
                    Err = enrich_mutation(
                        glifistore_error:enrich(
                            status_err(?GS_ST_INTERNAL, OpName, RequestId, Worker, State),
                            #{bytes_sent => FrameBytes}
                        ),
                        indeterminate
                    ),
                    {#{outcome => indeterminate, error => Err}, State};
                Status ->
                    Err = enrich_mutation(
                        glifistore_error:enrich(
                            status_err(Status, OpName, RequestId, Worker, State),
                            #{bytes_sent => FrameBytes}
                        ),
                        rejected
                    ),
                    {State1, Final} = apply_unhealthy(Status, Err, State),
                    {#{outcome => rejected, error => Final}, State1}
            end;
        {error, Err} ->
            glifistore_conn:reset(Conn),
            State1 = mark_unhealthy_if_metadata(Err, State),
            Ann = enrich_mutation(
                glifistore_error:enrich(annotate(Err, OpName, RequestId, Worker, State1), #{bytes_sent => FrameBytes}),
                indeterminate
            ),
            {#{outcome => indeterminate, error => Ann}, State1}
    end.

mark_unhealthy_if_metadata(Err, State) ->
    case maps:get(category, Err) of
        unavailable -> State#state{healthy = false};
        _ -> State
    end.

plan_pipeline(Requests, State) ->
    MaxReq = maps:get(maximum_pipeline_requests, State#state.config),
    MaxBytes = maps:get(maximum_pipeline_bytes, State#state.config),
    case length(Requests) > MaxReq of
        true ->
            {error, glifistore_error:invalid_argument(<<"pipeline exceeds the configured request limit">>)};
        false ->
            plan_pipeline_items(Requests, State, undefined, 0, [], MaxBytes, State)
    end.

plan_pipeline_items([], _State, Worker, _Need, Acc, _MaxBytes, StateAcc) ->
    Conn = maps:get(Worker, StateAcc#state.workers),
    {ok, Worker, Conn, lists:reverse(Acc), StateAcc};
plan_pipeline_items([Req | Rest], State, Worker0, Need, Acc, MaxBytes, StateAcc) ->
    case normalize_pipeline_req(Req) of
        {error, Err} ->
            {error, Err};
        {ok, Norm} ->
            case worker_index(StateAcc, maps:get(key, Norm)) of
                {error, Err} ->
                    {error, Err};
                {ok, Owner, _Conn} ->
                    case Worker0 =:= undefined orelse Worker0 =:= Owner of
                        false ->
                            {error, glifistore_error:invalid_argument(<<"every pipeline key must route to the same Worker">>)};
                        true ->
                            Key = maps:get(key, Norm),
                            Value = maps:get(value, Norm, <<>>),
                            FrameLen = glifistore_protocol:request_frame_size(Key, Value),
                            MaxFrame = maps:get(maximum_frame_bytes, StateAcc#state.config),
                            case FrameLen =< MaxFrame andalso FrameLen =< MaxBytes - Need of
                                true ->
                                    {RequestId, State1} = bump_id(StateAcc),
                                    case encode_request(
                                        maps:get(opcode, Norm),
                                        RequestId,
                                        Key,
                                        Value,
                                        maps:get(expire_at_ns, Norm, 0),
                                        State1
                                    ) of
                                        {ok, Frame, State2} ->
                                            Item = #{
                                                req => Norm,
                                                request_id => RequestId,
                                                frame => Frame,
                                                begin_offset => Need
                                            },
                                            plan_pipeline_items(
                                                Rest, State, Owner, Need + FrameLen, [Item | Acc], MaxBytes, State2
                                            );
                                        {{error, Err2}, _} ->
                                            {error, Err2}
                                    end;
                                false ->
                                    {error, frame_limit_err(FrameLen, MaxFrame, MaxBytes, Need)}
                            end
                    end
            end
    end.

group_batch(Requests, State) ->
    group_batch(Requests, State, 0, #{}).

group_batch([], _State, _Idx, Groups) ->
    {ok, maps:to_list(Groups)};
group_batch([Req | Rest], State, Idx, Groups) ->
    case normalize_pipeline_req(Req) of
        {ok, Norm} ->
            case worker_index(State, maps:get(key, Norm)) of
                {ok, Worker, _Conn} ->
                    Bucket = maps:get(Worker, Groups, []),
                    MaxReq = maps:get(maximum_pipeline_requests, State#state.config),
                    case length(Bucket) >= MaxReq of
                        true ->
                            {error, glifistore_error:invalid_argument(<<"batch exceeds the configured per-Worker request limit">>)};
                        false ->
                            group_batch(Rest, State, Idx + 1, Groups#{Worker => [{Idx, Norm} | Bucket]})
                    end;
                {error, Err} ->
                    {error, Err}
            end;
        {error, Err} ->
            {error, Err}
    end.

prepare_batch_groups([], State, Acc) ->
    {ok, lists:reverse(Acc), State};
prepare_batch_groups([{Worker, Items} | Rest], State, Acc) ->
    Ordered = lists:reverse(Items),
    case build_group_plan(Ordered, State) of
        {ok, Plan, State1} ->
            prepare_batch_groups(Rest, State1, [{Worker, Ordered, Plan} | Acc]);
        {error, Err} ->
            {error, Err}
    end.

plan_worker_pipelines([], _Worker, State, Acc) ->
    {ok, lists:reverse(Acc), State};
plan_worker_pipelines([[] | Rest], Worker, State, Acc) ->
    plan_worker_pipelines(Rest, Worker + 1, State, Acc);
plan_worker_pipelines([Requests | Rest], Worker, State, Acc) when is_list(Requests) ->
    case plan_pipeline_for_worker(Requests, Worker, State) of
        {ok, Plan, State1} ->
            plan_worker_pipelines(Rest, Worker + 1, State1, [{Worker, Requests, Plan} | Acc]);
        {error, Err} ->
            {error, Err}
    end;
plan_worker_pipelines([_ | _], _Worker, _State, _Acc) ->
    {error, glifistore_error:invalid_argument(<<"each worker pipeline must be a list">>)}.

plan_pipeline_for_worker(Requests, ExpectedWorker, State) ->
    MaxReq = maps:get(maximum_pipeline_requests, State#state.config),
    MaxBytes = maps:get(maximum_pipeline_bytes, State#state.config),
    case length(Requests) > MaxReq of
        true ->
            {error, glifistore_error:invalid_argument(<<"pipeline exceeds the configured request limit">>)};
        false ->
            plan_pipeline_for_worker_items(Requests, ExpectedWorker, State, 0, [], MaxBytes)
    end.

plan_pipeline_for_worker_items([], _ExpectedWorker, StateAcc, _Need, Acc, _MaxBytes) ->
    {ok, lists:reverse(Acc), StateAcc};
plan_pipeline_for_worker_items([Req | Rest], ExpectedWorker, StateAcc, Need, Acc, MaxBytes) ->
    case normalize_pipeline_req(Req) of
        {error, Err} ->
            {error, Err};
        {ok, Norm} ->
            case worker_index(StateAcc, maps:get(key, Norm)) of
                {error, Err} ->
                    {error, Err};
                {ok, Owner, _Conn} when Owner =/= ExpectedWorker ->
                    {error, glifistore_error:invalid_argument(<<"pipeline key does not route to the expected Worker">>)};
                {ok, Owner, _Conn} ->
                    Key = maps:get(key, Norm),
                    Value = maps:get(value, Norm, <<>>),
                    FrameLen = glifistore_protocol:request_frame_size(Key, Value),
                    MaxFrame = maps:get(maximum_frame_bytes, StateAcc#state.config),
                    case FrameLen =< MaxFrame andalso FrameLen =< MaxBytes - Need of
                        true ->
                            {RequestId, State1} = bump_id(StateAcc),
                            case encode_request(
                                maps:get(opcode, Norm),
                                RequestId,
                                Key,
                                Value,
                                maps:get(expire_at_ns, Norm, 0),
                                State1
                            ) of
                                {ok, Frame, State2} ->
                                    Item = #{
                                        req => Norm,
                                        request_id => RequestId,
                                        frame => Frame,
                                        begin_offset => Need
                                    },
                                    plan_pipeline_for_worker_items(
                                        Rest, Owner, State2, Need + FrameLen, [Item | Acc], MaxBytes
                                    );
                                {{error, Err2}, _} ->
                                    {error, Err2}
                            end;
                        false ->
                            {error, frame_limit_err(FrameLen, MaxFrame, MaxBytes, Need)}
                    end
            end
    end.

build_group_plan(Items, State) ->
    build_group_plan(Items, State, 0, []).

build_group_plan([], State, _Need, Acc) ->
    {ok, lists:reverse(Acc), State};
build_group_plan([{Idx, Norm} | Rest], State, Need, Acc) ->
    {RequestId, State1} = bump_id(State),
    Key = maps:get(key, Norm),
    Value = maps:get(value, Norm, <<>>),
    Expire = maps:get(expire_at_ns, Norm, 0),
    case encode_request(maps:get(opcode, Norm), RequestId, Key, Value, Expire, State1) of
        {ok, Frame, State2} ->
            Item = #{
                req => Norm,
                request_id => RequestId,
                frame => Frame,
                begin_offset => Need,
                index => Idx
            },
            build_group_plan(Rest, State2, Need + byte_size(Frame), [Item | Acc]);
        {{error, Err}, _} ->
            {error, Err}
    end.

merge_group_by_index(Responses, Items, GroupResponses) ->
    lists:foldl(
        fun({{Idx, _Norm}, Resp}, Acc) ->
            array:set(Idx, Resp, Acc)
        end,
        Responses,
        lists:zip(Items, GroupResponses)
    ).

%% Per-Worker connect/bootstrap before fan-out. A failure stamps only that
%% Worker's planned slots (bytes_sent=0 → failed/rejected) and leaves siblings
%% ready to send — matching Python/Go/Ruby execute_batch group isolation.
connect_prepared_groups(Prepared, Acc0, MergeFun, State0) ->
    {ReadyRev, Acc, State} =
        lists:foldl(
            fun({Worker, Items, Plan}, {ReadyAcc, AccIn, St}) ->
                Conn = maps:get(Worker, St#state.workers, undefined),
                case ensure_connected(Conn, Worker, St) of
                    {ok, St1} ->
                        {[{Worker, Items, Plan} | ReadyAcc], AccIn, St1};
                    {{error, Err}, St1} ->
                        Group = array:to_list(
                            mark_unresolved(
                                array:new(length(Plan), {default, failed_response()}),
                                0,
                                Err,
                                0,
                                metadata_from_plan(Plan)
                            )
                        ),
                        AccOut = MergeFun(Worker, Items, Group, AccIn),
                        {ReadyAcc, AccOut, St1}
                end
            end,
            {[], Acc0, State0},
            Prepared
        ),
    {lists:reverse(ReadyRev), Acc, State}.

read_op(get, Key) -> {glifistore_protocol:opcode_get(), <<"get">>, Key};
read_op(ping, _) -> {glifistore_protocol:opcode_ping(), <<"ping">>, <<>>};
read_op(health, _) -> {glifistore_protocol:opcode_health(), <<"health">>, <<>>};
read_op(ready, _) -> {glifistore_protocol:opcode_ready(), <<"ready">>, <<>>};
read_op(stats, _) -> {glifistore_protocol:opcode_stats(), <<"stats">>, <<>>};
read_op(backup, _) -> {glifistore_protocol:opcode_backup(), <<"backup">>, <<>>}.

mutate_opcode(put) -> glifistore_protocol:opcode_put();
mutate_opcode(erase) -> glifistore_protocol:opcode_erase().

encode_init(InitId) ->
    case glifistore_protocol:encode_request(glifistore_protocol:opcode_init(), InitId, <<>>, <<>>, 0, glifistore_protocol:no_worker()) of
        {ok, Frame} -> {ok, Frame};
        {error, {invalid_argument, Msg}} -> {error, glifistore_error:invalid_argument(Msg)}
    end.

encode_request(Opcode, RequestId, Key, Value, Expire, State) ->
    case glifistore_protocol:encode_request(Opcode, RequestId, Key, Value, Expire, glifistore_protocol:no_worker()) of
        {ok, Frame} ->
            Max = maps:get(maximum_frame_bytes, State#state.config),
            case byte_size(Frame) =< Max of
                true -> {ok, Frame, State};
                false ->
                    {{error, glifistore_error:invalid_argument(<<"request exceeds the configured frame limit">>)}, State}
            end;
        {error, {invalid_argument, Msg}} ->
            {{error, glifistore_error:invalid_argument(Msg)}, State}
    end.

validate_init(Response, InitId, Expected) ->
    Base = maps:get(status, Response) =:= glifistore_protocol:status_ok()
        andalso maps:get(request_id, Response) =:= InitId
        andalso maps:get(worker_count, Response) > 0
        andalso maps:get(worker_count, Response) =< 256
        andalso maps:get(routing_epoch, Response) > 0,
    case Base of
        false -> {error, glifistore_error:protocol(<<"server INIT response is inconsistent">>)};
        true ->
            case glifistore_protocol:decode_init_identity(maps:get(value, Response)) of
                {ok, Routing} ->
                    case Expected of
                        undefined -> {ok, Routing};
                        {WC, Epoch, ExpRouting} ->
                            case maps:get(worker_count, Response) =:= WC
                                andalso maps:get(routing_epoch, Response) =:= Epoch
                                andalso Routing =:= ExpRouting
                            of
                                true -> {ok, Routing};
                                false -> {error, glifistore_error:unavailable(<<"server routing metadata changed during bootstrap">>)}
                            end
                    end;
                {error, _} ->
                    {error, glifistore_error:protocol(<<"server INIT response is inconsistent">>)}
            end
    end.

validate_bind(Bound, BindId, Worker, InitResponse) ->
    case maps:get(status, Bound) =:= glifistore_protocol:status_ok()
        andalso maps:get(request_id, Bound) =:= BindId
        andalso maps:get(owner_worker, Bound) =:= Worker
        andalso maps:get(worker_count, Bound) =:= maps:get(worker_count, InitResponse)
        andalso maps:get(routing_epoch, Bound) =:= maps:get(routing_epoch, InitResponse)
    of
        true -> ok;
        false -> {error, glifistore_error:protocol(<<"server BIND_WORKER response is inconsistent">>)}
    end.

resolve_deadline(State, Opts) ->
    Budget = maps:get(request_timeout, State#state.config),
    Timeout = maps:get(timeout, Opts, Budget),
    case Timeout > 0 of
        true -> {ok, glifistore_util:monotonic_seconds() + Timeout};
        false -> {error, glifistore_error:invalid_argument(<<"request timeout must be positive">>)}
    end.

worker_index(#state{worker_count = 0}, _Key) ->
    {error, glifistore_error:unavailable(<<"client is not connected">>)};
worker_index(#state{worker_count = WC, routing = Routing} = State, Key) ->
    case glifistore_protocol:worker_for(Key, WC, Routing) of
        {ok, Worker} ->
            worker_index_fixed(State, Worker);
        {error, {invalid_argument, Msg}} ->
            {error, glifistore_error:invalid_argument(Msg)}
    end.

worker_index_fixed(#state{worker_count = 0}, _Worker) ->
    {error, glifistore_error:unavailable(<<"client is not connected">>)};
worker_index_fixed(#state{workers = Workers}, Worker) ->
    case maps:find(Worker, Workers) of
        {ok, Conn} ->
            {ok, Worker, Conn};
        error ->
            %% Conn process died; ensure_connected will replace under the supervisor.
            {ok, Worker, undefined}
    end.

ensure_connected(undefined, Worker, State) ->
    replace_and_bootstrap(Worker, State);
ensure_connected(Conn, Worker, State) ->
    case is_process_alive(Conn) of
        false ->
            replace_and_bootstrap(Worker, State);
        true ->
            case gen_server:call(Conn, connected, 5000) of
                true ->
                    {ok, State};
                false ->
                    WC = State#state.worker_count,
                    Epoch = State#state.routing_epoch,
                    Routing = State#state.routing,
                    case bootstrap_conn(Conn, Worker, {WC, Epoch, Routing}, State#state.config, State#state.request_id) of
                        {ok, WC, Epoch, Routing, NextId} ->
                            {ok, State#state{request_id = NextId}};
                        {error, Err} ->
                            {{error, Err}, State}
                    end
            end
    end.

%% Recreate a Worker conn under glifistore_conn_sup, then INIT+BIND with epoch checks.
replace_and_bootstrap(_Worker, #state{sup = undefined} = State) ->
    {{error, glifistore_error:unavailable(<<"client connection supervisor is not running">>)}, State};
replace_and_bootstrap(Worker, State) ->
    Sup = State#state.sup,
    Config = State#state.config,
    State0 = drop_worker_monitors(Worker, State),
    case glifistore_conn_sup:replace_conn(Sup, Worker, Config) of
        {ok, Conn} ->
            Mon = monitor(process, Conn),
            State1 = State0#state{
                workers = maps:put(Worker, Conn, State0#state.workers),
                conn_mons = maps:put(Mon, Worker, State0#state.conn_mons)
            },
            WC = State1#state.worker_count,
            Epoch = State1#state.routing_epoch,
            Routing = State1#state.routing,
            case bootstrap_conn(Conn, Worker, {WC, Epoch, Routing}, Config, State1#state.request_id) of
                {ok, WC, Epoch, Routing, NextId} ->
                    {ok, State1#state{request_id = NextId}};
                {error, Err} ->
                    {{error, Err}, State1}
            end;
        {error, Reason} ->
            Msg = iolist_to_binary(io_lib:format("cannot replace worker ~B connection: ~p", [Worker, Reason])),
            {{error, glifistore_error:unavailable(Msg)}, State0}
    end.

drop_worker_monitors(Worker, State) ->
    {Drop, Keep} = maps:fold(
        fun(Mon, W, {D, K}) ->
            case W =:= Worker of
                true -> {[Mon | D], K};
                false -> {D, K#{Mon => W}}
            end
        end,
        {[], #{}},
        State#state.conn_mons
    ),
    lists:foreach(fun(Mon) -> demonitor(Mon, [flush]) end, Drop),
    State#state{conn_mons = Keep}.

validate_response(Response, RequestId, Worker, #state{worker_count = WC, routing_epoch = Epoch}) ->
    case maps:get(request_id, Response) =:= RequestId of
        false -> {error, glifistore_error:protocol(<<"server response request ID does not match">>)};
        true ->
            case maps:get(worker_count, Response) =:= WC andalso maps:get(routing_epoch, Response) =:= Epoch of
                false -> {error, glifistore_error:unavailable(<<"server routing metadata changed">>)};
                true ->
                    case maps:get(owner_worker, Response) =:= Worker orelse maps:get(status, Response) =:= glifistore_protocol:status_wrong_owner() of
                        true -> ok;
                        false -> {error, glifistore_error:protocol(<<"server response came from the wrong Worker">>)}
                    end
            end
    end.

status_err(Status, OpName, RequestId, Worker, State) ->
    annotate(glifistore_error:from_status(Status), OpName, RequestId, Worker, State).

annotate(Err, OpName, RequestId, Worker, State) ->
    glifistore_error:enrich(Err, #{
        operation => OpName,
        request_id => RequestId,
        worker => Worker,
        routing_epoch => State#state.routing_epoch
    }).

enrich_mutation(Err, Outcome) ->
    glifistore_error:enrich(Err, #{mutation_outcome => Outcome}).

promote_send(SF) ->
    maps:get(error, SF).

promote_send_sf(SF, OpName, RequestId, Worker, State, Mutation) ->
    Err = maps:get(error, SF),
    Bytes = maps:get(bytes_sent, SF, 0),
    Outcome = case Mutation of
        true when Bytes =:= 0 -> rejected;
        true -> indeterminate;
        false -> undefined
    end,
    E1 = glifistore_error:enrich(Err, #{bytes_sent => Bytes, operation => OpName, request_id => RequestId, worker => Worker, routing_epoch => State#state.routing_epoch}),
    case Outcome of
        undefined -> E1;
        O -> enrich_mutation(E1, O)
    end.

maybe_unavailable(Err) ->
    case maps:get(category, Err, undefined) of
        transport -> glifistore_error:unavailable(maps:get(message, Err));
        _ -> Err
    end.

mutation_rejected(Op, Err, State) ->
    OpName = atom_to_binary(Op, utf8),
    #{outcome => rejected, error => enrich_mutation(annotate(Err, OpName, undefined, undefined, State), rejected)}.

retryable_read(Err, #state{healthy = true}) ->
    Cat = maps:get(category, Err),
    Cat =:= transport orelse Cat =:= unavailable;
retryable_read(_, _) -> false.

apply_unhealthy(Status, Err, State) ->
    case Status of
        ?GS_ST_WRONG_OWNER ->
            {State#state{healthy = false}, Err};
        ?GS_ST_NOT_BOUND ->
            {State#state{healthy = false}, Err};
        _ ->
            {State, Err}
    end.

maybe_unhealthy_state(Status, State) ->
    case Status of
        ?GS_ST_WRONG_OWNER ->
            State#state{healthy = false};
        ?GS_ST_NOT_BOUND ->
            State#state{healthy = false};
        _ ->
            State
    end.

normalize_pipeline_req(Req) ->
    Op = maps:get(opcode, Req),
    Key = maps:get(key, Req),
    Value = maps:get(value, Req, <<>>),
    Expire = maps:get(expire_at_ns, Req, 0),
    Opcode = case Op of
        get -> glifistore_protocol:opcode_get();
        put -> glifistore_protocol:opcode_put();
        erase -> glifistore_protocol:opcode_erase();
        _ -> invalid
    end,
    case Opcode of
        invalid -> {error, glifistore_error:invalid_argument(<<"pipeline request contains an invalid opcode">>)};
        _ ->
            case (Op =:= get orelse Op =:= erase) andalso (Value =/= <<>> orelse Expire =/= 0) of
                true -> {error, glifistore_error:invalid_argument(<<"GET and ERASE pipeline requests cannot carry PUT fields">>)};
                false -> {ok, #{opcode => Opcode, key => Key, value => Value, expire_at_ns => Expire, op => Op}}
            end
    end.

failed_response() -> #{outcome => failed}.

frame_limit_err(FrameLen, MaxFrame, _MaxBytes, _Need) ->
    case FrameLen > MaxFrame of
        true -> glifistore_error:invalid_argument(<<"pipeline request exceeds the configured frame limit">>);
        false -> glifistore_error:invalid_argument(<<"pipeline exceeds the configured aggregate byte limit">>)
    end.

pipeline_ok(Req, Response) ->
    case maps:get(op, Req) of
        Op when Op =:= put; Op =:= erase ->
            case maps:get(value, Response) of
                <<>> -> ok;
                _ -> {error, glifistore_error:protocol(<<"mutation response value must be empty">>)}
            end;
        _ -> ok
    end.

pipeline_status_outcome(Req, Status) ->
    case {maps:get(op, Req), Status} of
        {Op, ?GS_ST_INTERNAL} when Op =:= put; Op =:= erase ->
            indeterminate;
        _ ->
            failed
    end.

pipeline_op(Req) ->
    atom_to_binary(maps:get(op, Req), utf8).

%% Match C++: per-request classification with enriched retryability / mutation_outcome.
%% Mutation indeterminate only when bytes_sent > that request's begin_offset.
mark_unresolved(Responses, Start, Err, BytesSent, Metadata) ->
    mark_unresolved_from(Start, Metadata, Responses, Err, BytesSent).

mark_unresolved_from(_Idx, [], Responses, _Err, _BytesSent) ->
    Responses;
mark_unresolved_from(Idx, [{Id, Req, Begin} | Rest], Responses, Err, BytesSent) ->
    Op = maps:get(op, Req),
    IsMut = Op =:= put orelse Op =:= erase,
    Arrived = IsMut andalso BytesSent > Begin,
    Outcome =
        case Arrived of
            true -> indeterminate;
            false -> failed
        end,
    PosBytes =
        case BytesSent > Begin of
            true -> BytesSent - Begin;
            false -> 0
        end,
    Fields0 = #{
        operation => atom_to_binary(Op, utf8),
        request_id => Id,
        bytes_sent => PosBytes
    },
    Fields =
        case IsMut of
            true ->
                Fields0#{
                    mutation_outcome =>
                        case Arrived of
                            true -> indeterminate;
                            false -> rejected
                        end
                };
            false ->
                Fields0
        end,
    Enriched = glifistore_error:enrich(Err, Fields),
    mark_unresolved_from(
        Idx + 1,
        Rest,
        array:set(Idx, #{outcome => Outcome, error => Enriched}, Responses),
        Err,
        BytesSent
    ).

metadata_from_plan(Plan) ->
    [{maps:get(request_id, I), maps:get(req, I), maps:get(begin_offset, I)} || I <- Plan].

pipeline_plan_bytes(Plan) ->
    lists:sum([byte_size(maps:get(frame, I)) || I <- Plan]).

%% Outer kill / deadline after I/O may have started: treat the full planned payload as
%% may-have-sent so mutations become indeterminate / reconcile_first (§6.1).
classify_pipeline_after_io(Plan, Err) ->
    Count = length(Plan),
    Responses = array:new(Count, {default, failed_response()}),
    array:to_list(
        mark_unresolved(
            Responses,
            0,
            Err,
            pipeline_plan_bytes(Plan),
            metadata_from_plan(Plan)
        )
    ).

pipeline_outer_failure_reply(Pending, Err) ->
    Plan = maps:get(plan, Pending, []),
    GroupResponses = classify_pipeline_after_io(Plan, Err),
    case maps:is_key(merge, Pending) of
        true ->
            Acc1 = (maps:get(merge, Pending))(
                maps:get(worker, Pending),
                maps:get(items, Pending),
                GroupResponses,
                maps:get(acc, Pending)
            ),
            case maps:get(kind, Pending) of
                batch -> {ok, array:to_list(Acc1)};
                worker_pipelines -> {ok, array:to_list(Acc1)}
            end;
        false ->
            {ok, GroupResponses}
    end.

build_tls_options(TLS, Host) ->
    case code:ensure_loaded(ssl) of
        {module, ssl} ->
            SN = maps:get(server_name, TLS, Host),
            Verify = case maps:get(insecure_skip_verify, TLS, false) of true -> verify_none; false -> verify_peer end,
            Opts0 = [{verify, Verify}, {server_name_indication, SN}, {versions, ['tlsv1.3']}],
            case maps:get(ca_file, TLS, undefined) of
                undefined ->
                    finish_tls_options(TLS, Opts0);
                CAFile ->
                    %% Prefer cacertfile over decoded DER: OTP ssl accepts a
                    %% self-signed leaf trust anchor more reliably this way
                    %% (interop TLS uses the server cert as --tls-ca).
                    finish_tls_options(TLS, [{cacertfile, CAFile} | Opts0])
            end;
        _ -> {error, glifistore_error:unavailable(<<"TLS requires the ssl application">>)}
    end.

finish_tls_options(TLS, Opts0) ->
    Cert = maps:get(cert_file, TLS, undefined),
    Key = maps:get(key_file, TLS, undefined),
    case {Cert, Key} of
        {undefined, undefined} ->
            {ok, Opts0};
        {undefined, _} ->
            {error, glifistore_error:invalid_argument(<<"TLS mTLS requires both cert_file and key_file">>)};
        {_, undefined} ->
            {error, glifistore_error:invalid_argument(<<"TLS mTLS requires both cert_file and key_file">>)};
        {CertFile, KeyFile} ->
            {ok, [{certfile, CertFile}, {keyfile, KeyFile} | Opts0]}
    end.
