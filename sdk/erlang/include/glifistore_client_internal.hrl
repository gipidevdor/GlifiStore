%% Internal coordinator state shared with white-box Common Test suites.
%% Keep access field-based so adding state does not silently corrupt test setup.
-record(state, {
    config = #{} :: map(),
    worker_count = 0 :: non_neg_integer(),
    routing_epoch = 0 :: non_neg_integer(),
    routing = #{algorithm => 1, seed => 0} :: map(),
    request_id = 1 :: non_neg_integer(),
    healthy = true :: boolean(),
    workers = #{} :: #{non_neg_integer() => pid()},
    pending = #{} :: #{reference() => map()},
    mon_index = #{} :: #{reference() => reference()},
    closing = false :: boolean(),
    close_from :: gen_server:from() | undefined,
    sup :: pid() | undefined,
    conn_mons = #{} :: #{reference() => non_neg_integer()}
}).
