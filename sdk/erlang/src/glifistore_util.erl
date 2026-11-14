-module(glifistore_util).
-export([
    monotonic_seconds/0,
    remaining_timeout/1,
    timeout_ms/1,
    default_config/0,
    merge_config/1,
    validate_config/1
]).

-spec monotonic_seconds() -> float().
monotonic_seconds() ->
    erlang:convert_time_unit(erlang:monotonic_time(), native, 1000000000) / 1000000000.

-spec remaining_timeout(float()) -> {ok, float()} | {error, glifistore_error:error()}.
remaining_timeout(Deadline) ->
    Left = Deadline - monotonic_seconds(),
    case Left =< 0 of
        true ->
            {error, glifistore_error:transport(<<"request deadline expired">>)};
        false ->
            {ok, Left}
    end.

-spec timeout_ms(float()) -> non_neg_integer().
timeout_ms(Seconds) when Seconds =< 0 ->
    0;
timeout_ms(Seconds) ->
    max(1, round(Seconds * 1000)).

-spec default_config() -> glifistore_client:config().
default_config() ->
    #{
        host => "127.0.0.1",
        port => 7379,
        unix_socket_path => undefined,
        connect_timeout => 3.0,
        request_timeout => 5.0,
        maximum_frame_bytes => glifistore_protocol:max_frame_bytes(),
        maximum_pipeline_requests => 256,
        maximum_pipeline_bytes => 1024 * 1024,
        tls => #{enable => false}
    }.

-spec merge_config(map() | undefined) -> glifistore_client:config().
merge_config(undefined) ->
    default_config();
merge_config(Config) ->
    maps:fold(
        fun
            (_, undefined, Acc) -> Acc;
            (port, 0, Acc) -> Acc;
            (host, <<>>, Acc) -> Acc;
            (host, "", Acc) -> Acc;
            (unix_socket_path, <<>>, Acc) -> Acc;
            (unix_socket_path, "", Acc) -> Acc;
            (K, V, Acc) -> Acc#{K => V}
        end,
        default_config(),
        Config
    ).

-spec validate_config(glifistore_client:config()) -> ok | {error, glifistore_error:error()}.
validate_config(Config) ->
    Host = maps:get(host, Config),
    Port = maps:get(port, Config),
    UnixPath = maps:get(unix_socket_path, Config, undefined),
    ConnectTimeout = maps:get(connect_timeout, Config),
    RequestTimeout = maps:get(request_timeout, Config),
    MaxFrame = maps:get(maximum_frame_bytes, Config),
    MaxPipeReq = maps:get(maximum_pipeline_requests, Config),
    MaxPipeBytes = maps:get(maximum_pipeline_bytes, Config),
    TLS = maps:get(tls, Config, #{enable => false}),
    UnixOk =
        case UnixPath of
            undefined -> false;
            <<>> -> false;
            "" -> false;
            Path when is_binary(Path) -> byte_size(Path) > 0;
            Path when is_list(Path) -> Path =/= ""
        end,
    case UnixOk andalso maps:get(enable, TLS, false) of
        true ->
            {error, glifistore_error:invalid_argument(<<"TLS is not supported on AF_UNIX clients">>)};
        false ->
            HostOk = (is_binary(Host) andalso Host =/= <<>>) orelse (is_list(Host) andalso Host =/= ""),
            EndpointOk =
                case UnixOk of
                    true -> true;
                    false -> HostOk andalso is_integer(Port) andalso Port > 0 andalso Port =< 65535
                end,
            case EndpointOk
                andalso ConnectTimeout > 0
                andalso RequestTimeout > 0
                andalso MaxFrame >= glifistore_protocol:response_header_bytes()
                andalso MaxFrame =< glifistore_protocol:max_frame_bytes()
                andalso MaxPipeReq > 0
                andalso MaxPipeBytes >= 40
            of
                true ->
                    ok;
                false ->
                    {error, glifistore_error:invalid_argument(<<"client configuration is outside protocol limits">>)}
            end
    end.
