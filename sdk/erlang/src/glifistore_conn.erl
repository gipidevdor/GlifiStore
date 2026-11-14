-module(glifistore_conn).
-behaviour(gen_server).

-export([start_link/2, exchange/3, send/3, receive_response/2, reset/1, dial/1, inject_send_failure/2, run_pipeline/4]).
-export([init/1, handle_call/3, handle_cast/2, terminate/2]).

-record(state, {
    worker :: non_neg_integer(),
    config :: glifistore_client:config(),
    socket :: term() | undefined,
    use_ssl = false :: boolean(),
    input = <<>> :: binary(),
    test_send_failure :: undefined | before_any | {after_bytes, non_neg_integer()}
}).

start_link(Worker, Config) ->
    gen_server:start_link(?MODULE, {Worker, Config}, []).

exchange(Pid, Frame, Deadline) ->
    gen_server:call(Pid, {exchange, Frame, Deadline}, infinity).

send(Pid, Frame, Deadline) ->
    gen_server:call(Pid, {send, Frame, Deadline}, infinity).

receive_response(Pid, Deadline) ->
    gen_server:call(Pid, {receive_response, Deadline}, infinity).

%% Atomic pipeline on one connection: send all frames then collect Count responses.
%% Prevents interleaving when multiple callers share the same Worker conn.
run_pipeline(Pid, Frames, Count, Deadline) ->
    gen_server:call(Pid, {run_pipeline, Frames, Count, Deadline}, infinity).

reset(Pid) ->
    gen_server:cast(Pid, reset).

dial(Pid) ->
    gen_server:call(Pid, dial, infinity).

%% Test-only: inject a send failure on subsequent sends (after bootstrap).
inject_send_failure(Pid, Spec) ->
    gen_server:call(Pid, {inject_send_failure, Spec}).

init({Worker, Config}) ->
    {ok, #state{worker = Worker, config = Config, test_send_failure = undefined}}.

handle_call(dial, _From, State) ->
    case do_dial(State) of
        {ok, State1} ->
            {reply, ok, State1};
        {error, Reason, State1} ->
            {reply, {error, Reason}, State1}
    end;
handle_call(connected, _From, #state{socket = undefined} = State) ->
    {reply, false, State};
handle_call(connected, _From, State) ->
    {reply, true, State};
handle_call({inject_send_failure, Spec}, _From, State) ->
    {reply, ok, State#state{test_send_failure = Spec}};
handle_call({exchange, Frame, Deadline}, _From, State) ->
    case send_frame(State, Frame, Deadline) of
        {ok, State1} ->
            case receive_one(State1, Deadline) of
                {ok, Response, State2} ->
                    {reply, {ok, Response}, State2};
                {error, Reason, State2} ->
                    {reply, {error, Reason}, State2}
            end;
        {error, Reason, State1} ->
            {reply, {error, Reason}, State1}
    end;
handle_call({send, Frame, Deadline}, _From, State) ->
    case send_frame(State, Frame, Deadline) of
        {ok, State1} ->
            {reply, ok, State1};
        {error, Reason, State1} ->
            {reply, {error, Reason}, State1}
    end;
handle_call({receive_response, Deadline}, _From, State) ->
    case receive_one(State, Deadline) of
        {ok, Response, State1} ->
            {reply, {ok, Response}, State1};
        {error, Reason, State1} ->
            {reply, {error, Reason}, State1}
    end;
handle_call({run_pipeline, Frames, Count, Deadline}, _From, State) ->
    case send_frame(State, Frames, Deadline) of
        {ok, State1} ->
            case collect_n(State1, Count, Deadline, []) of
                {ok, Responses, State2} ->
                    {reply, {ok, Responses}, State2};
                {error, Reason, Partial, State2} ->
                    {reply, {recv_error, Reason, lists:reverse(Partial)}, State2}
            end;
        {error, Reason, State1} ->
            {reply, {send_error, Reason}, State1}
    end;
handle_call(_Request, _From, State) ->
    {reply, {error, glifistore_error:internal(<<"unexpected call">>)}, State}.

handle_cast(reset, State) ->
    {noreply, reset_state(State)};
handle_cast(_Msg, State) ->
    {noreply, State}.

terminate(_Reason, State) ->
    close_socket(State),
    ok.

do_dial(State) ->
    Config = State#state.config,
    TimeoutMs = glifistore_util:timeout_ms(maps:get(connect_timeout, Config)),
    TLS = maps:get(tls, Config, #{enable => false}),
    case maps:get(unix_socket_path, Config, undefined) of
        undefined ->
            dial_host(Config, TimeoutMs, TLS, State);
        <<>> ->
            dial_host(Config, TimeoutMs, TLS, State);
        "" ->
            dial_host(Config, TimeoutMs, TLS, State);
        UnixPath ->
            case maps:get(enable, TLS, false) of
                true ->
                    {error, glifistore_error:invalid_argument(<<"TLS is not supported on AF_UNIX clients">>), State};
                false ->
                    dial_unix(UnixPath, TimeoutMs, State)
            end
    end.

dial_host(Config, TimeoutMs, TLS, State) ->
    Host = maps:get(host, Config),
    Port = maps:get(port, Config),
    case maps:get(enable, TLS, false) of
        true ->
            dial_tls(Host, Port, TimeoutMs, TLS, State);
        false ->
            dial_tcp(Host, Port, TimeoutMs, State)
    end.

dial_unix(UnixPath0, TimeoutMs, State) ->
    Path =
        case UnixPath0 of
            Bin when is_binary(Bin) -> binary_to_list(Bin);
            List when is_list(List) -> List
        end,
    Opts = [binary, {active, false}, {packet, 0}],
    case gen_tcp:connect({local, Path}, 0, Opts, TimeoutMs) of
        {ok, Socket} ->
            {ok, State#state{socket = Socket, use_ssl = false}};
        {error, Reason} ->
            Msg = iolist_to_binary([
                <<"could not connect to GlifiStore: ">>,
                format_inet_reason(Reason)
            ]),
            {error, glifistore_error:unavailable(Msg), State}
    end.

dial_tcp(Host, Port, TimeoutMs, State) ->
    Opts = [binary, {active, false}, {packet, 0}, {nodelay, true}],
    case gen_tcp:connect(Host, Port, Opts, TimeoutMs) of
        {ok, Socket} ->
            {ok, State#state{socket = Socket, use_ssl = false}};
        {error, Reason} ->
            Msg = iolist_to_binary([
                <<"could not connect to GlifiStore: ">>,
                format_inet_reason(Reason)
            ]),
            {error, glifistore_error:unavailable(Msg), State}
    end.

dial_tls(Host, Port, TimeoutMs, TLS, State) ->
    case code:ensure_loaded(ssl) of
        {module, ssl} ->
            case application:ensure_all_started(ssl) of
                {ok, _} ->
                    do_dial_tls(Host, Port, TimeoutMs, TLS, State);
                _ ->
                    {error,
                        glifistore_error:unavailable(<<"TLS requires the ssl application">>),
                        State}
            end;
        _ ->
            {error, glifistore_error:unavailable(<<"TLS requires the ssl application">>), State}
    end.

do_dial_tls(Host, Port, TimeoutMs, TLS, State) ->
    Opts = [binary, {active, false}, {packet, 0}, {nodelay, true}],
    case gen_tcp:connect(Host, Port, Opts, TimeoutMs) of
        {ok, Tcp} ->
            case glifistore_client:build_tls_options(TLS, Host) of
                {ok, SSLOpts} ->
                    case ssl:connect(Tcp, SSLOpts, TimeoutMs) of
                        {ok, Socket} ->
                            {ok, State#state{socket = Socket, use_ssl = true}};
                        {error, Reason} ->
                            gen_tcp:close(Tcp),
                            {error,
                                glifistore_error:unavailable(
                                    iolist_to_binary([
                                        <<"TLS handshake failed: ">>,
                                        format_inet_reason(Reason)
                                    ])
                                ),
                                State}
                    end;
                {error, Err} ->
                    gen_tcp:close(Tcp),
                    {error, Err, State}
            end;
        {error, Reason} ->
            Msg = iolist_to_binary([
                <<"could not connect to GlifiStore: ">>,
                format_inet_reason(Reason)
            ]),
            {error, glifistore_error:unavailable(Msg), State}
    end.

send_frame(#state{socket = undefined} = State, _Frame, _Deadline) ->
    {error, glifistore_error:transport(<<"socket is not connected">>), State};
send_frame(#state{test_send_failure = before_any} = State, _Frame, _Deadline) ->
    {error, send_failure(0, transport(<<"injected send failure before any bytes">>)), reset_state(State)};
send_frame(#state{test_send_failure = {after_bytes, N}} = State, Frame, _Deadline) when is_integer(N), N >= 0 ->
    Total =
        case is_binary(Frame) of
            true -> byte_size(Frame);
            false -> iolist_size(Frame)
        end,
    Sent = min(N, Total),
    {error, send_failure(Sent, transport(<<"injected send failure after partial bytes">>)), reset_state(State)};
send_frame(State, Frame, Deadline) when is_binary(Frame) ->
    send_loop(State, Frame, 0, byte_size(Frame), Deadline);
send_frame(State, Frame, Deadline) ->
    send_loop(State, Frame, 0, iolist_size(Frame), Deadline).

send_loop(State, Frame, Sent, Total, Deadline) ->
    case glifistore_util:remaining_timeout(Deadline) of
        {ok, Left} ->
            TimeoutMs = glifistore_util:timeout_ms(Left),
            case send_chunk(State, Frame, Sent, Total, TimeoutMs) of
                {ok, Sent1} when Sent1 >= Total ->
                    {ok, State};
                {ok, Sent1} ->
                    send_loop(State, Frame, Sent1, Total, Deadline);
                {error, closed, Sent1} ->
                    {error, send_failure(Sent1, transport(<<"socket closed during send">>)), reset_state(State)};
                {error, Reason, Sent1} ->
                    {error, send_failure(Sent1, Reason), reset_state(State)}
            end;
        {error, Err} ->
            {error, send_failure(Sent, Err), State}
    end.

%% ssl:send/2 only — timeouts are enforced via remaining_timeout before each chunk
%% and ssl:setopts send_timeout when the OTP ssl application supports it.
send_chunk(#state{socket = Socket, use_ssl = true}, Frame, Sent, Total, TimeoutMs) ->
    _ = catch ssl:setopts(Socket, [{send_timeout, TimeoutMs}]),
    Chunk = iolist_or_part(Frame, Sent, Total - Sent),
    case ssl:send(Socket, Chunk) of
        ok -> {ok, Total};
        {error, closed} -> {error, closed, Sent};
        {error, timeout} -> {error, transport(<<"request deadline expired">>), Sent};
        {error, Reason} ->
            {error, transport(iolist_to_binary([<<"request send failed: ">>, format_inet_reason(Reason)])), Sent}
    end;
send_chunk(#state{socket = Socket, use_ssl = false}, Frame, Sent, Total, TimeoutMs) ->
    ok = inet:setopts(Socket, [{send_timeout, TimeoutMs}]),
    Chunk = iolist_or_part(Frame, Sent, Total - Sent),
    case gen_tcp:send(Socket, Chunk) of
        ok -> {ok, Total};
        {error, closed} -> {error, closed, Sent};
        {error, timeout} -> {error, transport(<<"request deadline expired">>), Sent};
        {error, Reason} ->
            {error, transport(iolist_to_binary([<<"request send failed: ">>, format_inet_reason(Reason)])), Sent}
    end.

%% Prefer zero-copy send of a full binary/iolist; only slice when retrying a partial send.
iolist_or_part(Data, 0, _Len) when is_binary(Data); is_list(Data) ->
    Data;
iolist_or_part(Data, Sent, Len) when is_binary(Data) ->
    binary_part(Data, Sent, Len);
iolist_or_part(Data, Sent, Len) ->
    binary_part(iolist_to_binary(Data), Sent, Len).

receive_one(State, Deadline) ->
    read_response(State, Deadline).

collect_n(State, 0, _Deadline, Acc) ->
    {ok, lists:reverse(Acc), State};
collect_n(State, N, Deadline, Acc) ->
    case receive_one(State, Deadline) of
        {ok, Response, State1} ->
            collect_n(State1, N - 1, Deadline, [Response | Acc]);
        {error, Reason, State1} ->
            {error, Reason, Acc, State1}
    end.

read_response(State, Deadline) ->
    Input = State#state.input,
    MaxFrame = maps:get(maximum_frame_bytes, State#state.config),
    case frame_ready(Input, MaxFrame) of
        {ready, Frame, Rest} ->
            case glifistore_protocol:decode_response(Frame, MaxFrame) of
                {ok, Response} ->
                    {ok, Response, State#state{input = Rest}};
                {error, {invalid_argument, Msg}} ->
                    {error, glifistore_error:protocol(Msg), State}
            end;
        {need, _} ->
            recv_more(State, Deadline);
        {invalid, _} ->
            {error, glifistore_error:protocol(<<"server response size is outside client limits">>), State}
    end.

frame_ready(Input, MaxFrame) when byte_size(Input) >= 4 ->
    <<Size:32/little, _/binary>> = Input,
    Min = glifistore_protocol:response_header_bytes(),
    case Size >= Min andalso Size =< MaxFrame of
        true ->
            case byte_size(Input) >= Size of
                true ->
                    <<Frame:Size/binary, Rest/binary>> = Input,
                    {ready, Frame, Rest};
                false ->
                    {need, Size - byte_size(Input)}
            end;
        false ->
            {invalid, Size}
    end;
frame_ready(Input, _MaxFrame) ->
    {need, 4 - byte_size(Input)}.

recv_more(State, Deadline) ->
    case glifistore_util:remaining_timeout(Deadline) of
        {ok, Left} ->
            TimeoutMs = glifistore_util:timeout_ms(Left),
            Recv =
                case State#state.use_ssl of
                    true -> ssl:recv(State#state.socket, 0, TimeoutMs);
                    false -> gen_tcp:recv(State#state.socket, 0, TimeoutMs)
                end,
            case Recv of
                {ok, Chunk} ->
                    Input1 = append_input(State#state.input, Chunk),
                    read_response(State#state{input = Input1}, Deadline);
                {error, closed} ->
                    {error, glifistore_error:transport(<<"server closed the connection">>), reset_state(State)};
                {error, timeout} ->
                    {error, glifistore_error:transport(<<"request deadline expired">>), State};
                {error, Reason} ->
                    Msg = iolist_to_binary([
                        <<"response receive failed: ">>,
                        format_inet_reason(Reason)
                    ]),
                    {error, glifistore_error:transport(Msg), State}
            end;
        {error, Err} ->
            {error, Err, State}
    end.

%% Avoid an empty-binary concat on the first chunk of a frame.
append_input(<<>>, Chunk) -> Chunk;
append_input(Input, Chunk) -> <<Input/binary, Chunk/binary>>.

%% ssl/gen_tcp reasons are atoms or nested tuples (e.g. {tls_alert, ...}).
format_inet_reason(Reason) when is_atom(Reason) ->
    atom_to_binary(Reason, utf8);
format_inet_reason(Reason) ->
    iolist_to_binary(io_lib:format("~0p", [Reason])).

send_failure(BytesSent, Err) ->
    #{send_failure => true, bytes_sent => BytesSent, error => Err}.

transport(Msg) ->
    glifistore_error:transport(Msg).

reset_state(State) ->
    close_socket(State),
    State#state{socket = undefined, input = <<>>}.

close_socket(#state{socket = undefined}) ->
    ok;
close_socket(#state{socket = Socket, use_ssl = true}) ->
    catch ssl:close(Socket);
close_socket(#state{socket = Socket, use_ssl = false}) ->
    catch gen_tcp:close(Socket).
