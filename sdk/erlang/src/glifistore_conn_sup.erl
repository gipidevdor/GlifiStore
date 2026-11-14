%% @doc Minimal OTP supervisor for per-Worker `glifistore_conn` processes.
%%
%% Restart policy (fail-closed):
%% - `one_for_one` with `intensity = 0` — never auto-restart children.
%% - Conn children are `temporary` — a crash does not recreate the socket.
%% - Reconnect is explicit in `glifistore_client:ensure_connected/3`, which
%%   re-`INIT`+`BIND_WORKER` and verifies worker_count / routing_epoch.
%% - Permanent auto-restart is intentionally rejected: it could hide an epoch
%%   change or resume with obsolete routing metadata.
%%
%% The supervisor is started linked from the client coordinator. When the client
%% exits, all Worker connections exit with it.
-module(glifistore_conn_sup).
-behaviour(supervisor).

-export([start_link/0, start_conn/3, replace_conn/3]).
-export([init/1]).

-spec start_link() -> {ok, pid()} | {error, term()}.
start_link() ->
    supervisor:start_link(?MODULE, []).

-spec start_conn(pid(), non_neg_integer(), glifistore_client:config()) ->
    {ok, pid()} | {error, term()}.
start_conn(Sup, Worker, Config) when is_pid(Sup), is_integer(Worker), Worker >= 0 ->
    supervisor:start_child(Sup, conn_child_spec(Worker, Config)).

%% Replace a dead/exited Worker conn child. Deletes any lingering temporary
%% child id, then starts a fresh conn under the same supervisor.
-spec replace_conn(pid(), non_neg_integer(), glifistore_client:config()) ->
    {ok, pid()} | {error, term()}.
replace_conn(Sup, Worker, Config) when is_pid(Sup), is_integer(Worker), Worker >= 0 ->
    Id = {conn, Worker},
    _ = supervisor:terminate_child(Sup, Id),
    _ = supervisor:delete_child(Sup, Id),
    start_conn(Sup, Worker, Config).

init([]) ->
    SupFlags = #{
        strategy => one_for_one,
        intensity => 0,
        period => 1
    },
    {ok, {SupFlags, []}}.

conn_child_spec(Worker, Config) ->
    #{
        id => {conn, Worker},
        start => {glifistore_conn, start_link, [Worker, Config]},
        restart => temporary,
        shutdown => 2000,
        type => worker,
        modules => [glifistore_conn]
    }.
