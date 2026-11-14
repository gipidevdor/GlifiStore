-module(glifistore_version).
-export([version/0]).

-spec version() -> binary().
version() ->
    <<"0.1.0">>.
