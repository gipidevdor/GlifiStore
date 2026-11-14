-module(glifistore_error).
-export([
    new/2,
    invalid_argument/1,
    unavailable/1,
    transport/1,
    protocol/1,
    not_found/1,
    overloaded/1,
    internal/1,
    permission_denied/1,
    from_status/1,
    retryability_for/3,
    enrich/2,
    format/1
]).

-export_type([error/0, category/0, retryability/0, mutation_outcome/0]).

-type category() ::
    invalid_argument
    | not_found
    | overloaded
    | unavailable
    | transport
    | protocol
    | internal
    | permission_denied.

-type retryability() :: never | same_request | new_attempt | reconcile_first.

-type mutation_outcome() :: committed | rejected | indeterminate.

-type error() :: #{
    category := category(),
    message := binary(),
    wire_status => non_neg_integer() | undefined,
    bytes_sent => non_neg_integer(),
    request_id => non_neg_integer() | undefined,
    worker => non_neg_integer() | undefined,
    routing_epoch => non_neg_integer() | undefined,
    retryability := retryability(),
    operation => binary() | undefined,
    mutation_outcome => mutation_outcome() | undefined
}.

-spec new(category(), binary()) -> error().
new(Category, Message) ->
    #{
        category => Category,
        message => Message,
        bytes_sent => 0,
        retryability => retryability_for(Category, false, false)
    }.

-spec invalid_argument(binary()) -> error().
invalid_argument(Message) -> new(invalid_argument, Message).

-spec unavailable(binary()) -> error().
unavailable(Message) ->
    Err = new(unavailable, Message),
    Err#{retryability => never}.

-spec transport(binary()) -> error().
transport(Message) ->
    Err = new(transport, Message),
    Err#{retryability => same_request}.

-spec protocol(binary()) -> error().
protocol(Message) -> new(protocol, Message).

-spec not_found(binary()) -> error().
not_found(Message) ->
    Err = new(not_found, Message),
    Err#{retryability => new_attempt}.

-spec overloaded(binary()) -> error().
overloaded(Message) ->
    Err = new(overloaded, Message),
    Err#{retryability => never}.

-spec internal(binary()) -> error().
internal(Message) -> new(internal, Message).

-spec permission_denied(binary()) -> error().
permission_denied(Message) ->
    Err = new(permission_denied, Message),
    Err#{retryability => never}.

-spec from_status(non_neg_integer()) -> error().
from_status(4) ->
    from_status_with(4, not_found(<<"key was not found">>));
from_status(5) ->
    from_status_with(5, overloaded(<<"server is overloaded">>));
from_status(7) ->
    from_status_with(7, unavailable(<<"server connection is not bound">>));
from_status(6) ->
    from_status_with(6, protocol(<<"server rejected Worker routing">>));
from_status(8) ->
    from_status_with(8, permission_denied(<<"server denied the request">>));
from_status(1) ->
    from_status_with(1, invalid_argument(<<"server rejected the request">>));
from_status(2) ->
    from_status_with(2, invalid_argument(<<"server rejected the request">>));
from_status(3) ->
    from_status_with(3, internal(<<"server reported an internal error">>));
from_status(0) ->
    from_status_with(0, internal(<<"unexpected successful response mapping">>));
from_status(Status) when is_integer(Status), Status > 8 ->
    from_status_with(Status, protocol(<<"server returned an unknown status">>));
from_status(Status) ->
    from_status_with(Status, protocol(<<"server returned an unknown status">>)).

from_status_with(Status, Err) ->
    Err#{wire_status => Status}.

-spec retryability_for(category(), boolean(), boolean()) -> retryability().
retryability_for(_Category, _MutationSent, true) ->
    reconcile_first;
retryability_for(invalid_argument, false, false) ->
    never;
retryability_for(transport, false, false) ->
    same_request;
retryability_for(overloaded, _, _) ->
    never;
retryability_for(permission_denied, _, _) ->
    never;
retryability_for(not_found, _, _) ->
    new_attempt;
retryability_for(unavailable, _, _) ->
    never;
retryability_for(_Category, true, false) ->
    reconcile_first;
retryability_for(_Category, false, false) ->
    new_attempt.

-spec enrich(error(), map()) -> error().
enrich(Err, Fields) ->
    Err1 = maps:merge(Err, maps:with([wire_status, bytes_sent, request_id, worker, routing_epoch, operation, mutation_outcome], Fields)),
    Category = maps:get(category, Err1),
    BytesSent = maps:get(bytes_sent, Err1, 0),
    MutationOutcome = maps:get(mutation_outcome, Err1, undefined),
    Indeterminate = MutationOutcome =:= indeterminate,
    MutationSent = (BytesSent > 0) andalso (MutationOutcome =/= undefined),
    %% Indeterminate always means reconcile_first — including transport failures after a
    %% full mutation send where bytes_sent was omitted (must not fall through to same_request).
    Retry =
        case {Category, BytesSent, MutationOutcome} of
            {_, _, indeterminate} ->
                reconcile_first;
            {transport, 0, _} ->
                same_request;
            {transport, _, _} when MutationOutcome =/= undefined ->
                reconcile_first;
            _ ->
                retryability_for(Category, MutationSent, Indeterminate)
        end,
    Err1#{retryability => Retry}.

-spec format(error()) -> binary().
format(Err) ->
    maps:get(message, Err).
