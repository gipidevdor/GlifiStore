#!/usr/bin/env escript
%%! -noshell -noinput
-mode(compile).

main([]) ->
    add_beam_path(),
    io:format("~s", [glifistore_version:version()]),
    halt(0);
main(_) ->
    halt(1).

add_beam_path() ->
    Script = escript:script_name(),
    Root = filename:dirname(filename:dirname(Script)),
    Beam = filename:join([Root, "_build", "default", "lib", "glifistore", "ebin"]),
    true = code:add_patha(Beam).
