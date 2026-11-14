use v5.32;
use strict;
use warnings;
use FindBin;
use File::Temp;
use IO::Select;
use IO::Socket::INET;
use POSIX qw(_exit);
use Socket qw(AF_UNIX PF_UNSPEC SOCK_STREAM);
use Test::More;

use lib "$FindBin::Bin/../lib";
use GlifiStore::Client;
use GlifiStore::Error;
use GlifiStore::Protocol qw(
    OP_INIT OP_PING OP_GET OP_PUT OP_ERASE OP_BIND_WORKER OP_HEALTH OP_READY OP_STATS OP_BACKUP
    STATUS_OK STATUS_NOT_FOUND STATUS_NOT_BOUND STATUS_INVALID_REQUEST STATUS_WRONG_OWNER
    STATUS_INTERNAL_ERROR
    decode_request encode_response worker_for
);

sub read_exact {
    my ($socket, $size) = @_;
    my $output = '';
    while (length($output) < $size) {
        my $received = sysread($socket, my $chunk, $size - length($output));
        die "peer closed" if !$received;
        $output .= $chunk;
    }
    return $output;
}

sub receive_request {
    my ($socket) = @_;
    my $prefix = read_exact($socket, 4);
    my $size = unpack('V', $prefix);
    return decode_request($prefix . read_exact($socket, $size - 4));
}

sub send_response {
    my ($socket, %response) = @_;
    my $frame = encode_response(%response);
    my $offset = 0;
    while ($offset < length($frame)) {
        my $written = syswrite($socket, $frame, length($frame) - $offset, $offset);
        die "send failed: $!" if !$written;
        $offset += $written;
    }
}

sub start_server {
    my (%options) = @_;
    my $worker_count = $options{worker_count} // 1;
    my $routing = $options{routing};
    my $listener = IO::Socket::INET->new(
        LocalAddr => '127.0.0.1', LocalPort => 0, Proto => 'tcp',
        Listen => $worker_count, ReuseAddr => 1)
        or die "listen failed: $!";
    my $port = $listener->sockport;
    my $pid = fork();
    die "fork failed: $!" if !defined($pid);
    if (!$pid) {
        my %values;
        eval {
            my $selector = IO::Select->new($listener);
            my %bound;
            my $accepted = 0;
            while ($selector->count) {
                my @ready = $selector->can_read(2)
                    or last;
                for my $handle (@ready) {
                    if (fileno($handle) == fileno($listener)) {
                        my $socket = $listener->accept or next;
                        $selector->add($socket);
                        ++$accepted;
                        $selector->remove($listener) if $accepted >= $worker_count;
                        next;
                    }
                    my $request = eval { receive_request($handle) };
                    if (!$request) {
                        $selector->remove($handle);
                        close($handle);
                        next;
                    }
                    my $opcode = $request->{opcode};
                    my $owner = $bound{$handle} // 0;
                    if ($opcode == OP_INIT) {
                        send_response($handle,
                            status => STATUS_OK,
                            request_id => $request->{request_id},
                            value => GlifiStore::Protocol::encode_init_identity($routing),
                            owner_worker => 0,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_HEALTH) {
                        send_response($handle,
                            status => STATUS_OK,
                            request_id => $request->{request_id},
                            value => 'GlifiStore/live',
                            owner_worker => $bound{$handle} // 0,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_READY) {
                        send_response($handle,
                            status => STATUS_OK,
                            request_id => $request->{request_id},
                            value => 'GlifiStore/ready',
                            owner_worker => $bound{$handle} // 0,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_STATS) {
                        send_response($handle,
                            status => STATUS_OK,
                            request_id => $request->{request_id},
                            value => "GlifiStore/stats\n",
                            owner_worker => $bound{$handle} // 0,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_BIND_WORKER) {
                        if ($request->{target_worker} >= $worker_count) {
                            send_response($handle,
                                status => STATUS_INVALID_REQUEST,
                                request_id => $request->{request_id},
                                owner_worker => 0,
                                worker_count => $worker_count,
                                routing_epoch => 9);
                            next;
                        }
                        $bound{$handle} = $request->{target_worker};
                        send_response($handle,
                            status => STATUS_OK,
                            request_id => $request->{request_id},
                            owner_worker => $request->{target_worker},
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif (!exists $bound{$handle}) {
                        send_response($handle,
                            status => STATUS_NOT_BOUND,
                            request_id => $request->{request_id},
                            owner_worker => 0,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_PING) {
                        send_response($handle,
                            status => STATUS_OK,
                            request_id => $request->{request_id},
                            value => $request->{value},
                            owner_worker => $owner,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_PUT) {
                        my $owner = worker_for($request->{key}, $worker_count, $routing);
                        if ($owner != $bound{$handle}) {
                            send_response($handle,
                                status => STATUS_WRONG_OWNER,
                                request_id => $request->{request_id},
                                owner_worker => $owner,
                                worker_count => $worker_count,
                                routing_epoch => 9);
                            next;
                        }
                        $values{$request->{key}} = $request->{value};
                        if ($options{disconnect_on_put}) {
                            $selector->remove($handle);
                            close($handle);
                            next;
                        }
                        if ($options{internal_error_on_put}) {
                            send_response($handle,
                                status => STATUS_INTERNAL_ERROR,
                                request_id => $request->{request_id},
                                owner_worker => $owner,
                                worker_count => $worker_count,
                                routing_epoch => 9);
                            next;
                        }
                        send_response($handle,
                            status => STATUS_OK,
                            request_id => $request->{request_id},
                            owner_worker => $owner,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_GET) {
                        my $owner = worker_for($request->{key}, $worker_count, $routing);
                        if ($owner != $bound{$handle}) {
                            send_response($handle,
                                status => STATUS_WRONG_OWNER,
                                request_id => $request->{request_id},
                                owner_worker => $owner,
                                worker_count => $worker_count,
                                routing_epoch => 9);
                            next;
                        }
                        send_response($handle,
                            status => exists($values{$request->{key}}) ? STATUS_OK : STATUS_NOT_FOUND,
                            request_id => $request->{request_id},
                            value => $values{$request->{key}} // '',
                            owner_worker => $owner,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_ERASE) {
                        my $owner = worker_for($request->{key}, $worker_count, $routing);
                        if ($owner != $bound{$handle}) {
                            send_response($handle,
                                status => STATUS_WRONG_OWNER,
                                request_id => $request->{request_id},
                                owner_worker => $owner,
                                worker_count => $worker_count,
                                routing_epoch => 9);
                            next;
                        }
                        my $found = exists($values{$request->{key}});
                        delete $values{$request->{key}};
                        send_response($handle,
                            status => $found ? STATUS_OK : STATUS_NOT_FOUND,
                            request_id => $request->{request_id},
                            owner_worker => $owner,
                            worker_count => $worker_count,
                            routing_epoch => 9);
                    } elsif ($opcode == OP_BACKUP) {
                        if ($options{internal_error_on_backup}) {
                            send_response($handle,
                                status => STATUS_INTERNAL_ERROR,
                                request_id => $request->{request_id},
                                value => 'report failed',
                                owner_worker => $owner,
                                worker_count => $worker_count,
                                routing_epoch => 9);
                        } else {
                            my $reply_id = $options{wrong_request_id_on_backup}
                                ? ($request->{request_id} ^ 1)
                                : $request->{request_id};
                            send_response($handle,
                                status => STATUS_OK,
                                request_id => $reply_id,
                                value => 'status=ok files=0 bytes=0',
                                owner_worker => $owner,
                                worker_count => $worker_count,
                                routing_epoch => 9);
                        }
                    }
                }
            }
            close($_) for grep { $_ != $listener } $selector->handles;
        };
        close($listener);
        _exit(0);
    }
    close($listener);
    return ($port, $pid);
}

{
    socketpair(my $reader, my $writer, AF_UNIX, SOCK_STREAM, PF_UNSPEC)
        or die "socketpair failed: $!";
    my $frame = encode_response(
        status => STATUS_OK, request_id => 41, value => 'partial-ready', owner_worker => 0,
        worker_count => 1, routing_epoch => 9,
    );
    is(syswrite($writer, $frame, 4), 4, 'readiness test writes response prefix');
    my $fragment_pid = fork();
    die "fork failed: $!" if !defined($fragment_pid);
    if (!$fragment_pid) {
        select undef, undef, undef, 0.05;
        my $offset = 4;
        while ($offset < length($frame)) {
            my $written = syswrite($writer, $frame, length($frame) - $offset, $offset);
            _exit(1) if !$written;
            $offset += $written;
        }
        close($reader);
        close($writer);
        _exit(0);
    }
    close($writer);

    my $probe = bless {
        request_timeout     => 1,
        maximum_frame_bytes => GlifiStore::Protocol::MAX_FRAME_BYTES(),
        connections         => [],
    }, 'GlifiStore::Client';
    my $connection = {
        socket => $reader, selector => undef, input => '', input_offset => 0,
    };
    my $wait_io = \&GlifiStore::Client::_wait_io;
    my $waits = 0;
    my $response;
    {
        no warnings 'redefine';
        local *GlifiStore::Client::_wait_io = sub {
            ++$waits;
            return $wait_io->(@_);
        };
        $response = $probe->_receive_response(
            $connection, GlifiStore::Client::_now() + 1, undef, 1
        );
    }
    is($response->[GlifiStore::Client::RESPONSE_VALUE()], 'partial-ready',
        'readiness fast path preserves a fragmented response');
    is($waits, 1, 'readiness skips only the first wait and partial input waits with its deadline');
    close($reader);
    waitpid($fragment_pid, 0);
    is($?, 0, 'fragmented response writer exits cleanly');
}

my ($port, $pid) = start_server();
my $client = GlifiStore::Client->connect(port => $port);
is($client->worker_count, 1, 'client discovers Worker count');
is($client->routing_epoch, 9, 'client records routing epoch');
is($client->worker_for("binary\x00key"), 0, 'client routes binary key');
is($client->put("binary\x00key", "value\x00\xff")->{outcome}, 'committed', 'PUT commits');
is($client->get("binary\x00key"), "value\x00\xff", 'GET preserves binary value');
is($client->ping('hello'), 'hello', 'PING echoes payload');
ok($client->healthy, 'client is healthy after bootstrap');
ok(defined $client->routing, 'client exposes routing metadata');
is($client->health(), 'GlifiStore/live', 'HEALTH returns wire live payload');
is($client->ready(), 'GlifiStore/ready', 'READY returns wire ready payload');
like($client->stats(), qr/^GlifiStore\/stats/, 'STATS returns wire stats report');

my $rejected = $client->put('bad-expiry', 'value', expire_at_ns => -1);
is($rejected->{outcome}, 'rejected', 'negative expire_at_ns is rejected before send');
unlike("$rejected->{error}", qr/ at .* line /, 'rejected errors omit Perl location noise');

my (@pipeline, @expected);
for my $index (0 .. 63) {
    my $value = "pipeline-$index";
    push @expected, $value;
    push @pipeline, { opcode => 'put', key => 'key', value => $value };
    push @pipeline, { opcode => 'get', key => 'key' };
}
my $responses = $client->execute_pipeline(\@pipeline);
is(scalar(@$responses), scalar(@pipeline), 'pipeline returns one positional response');
for my $index (0 .. $#expected) {
    is($responses->[$index * 2]->{outcome}, 'succeeded', "pipeline PUT $index succeeds");
    is($responses->[$index * 2 + 1]->{value}, $expected[$index], "pipeline GET $index ordered");
}
is($client->erase("binary\x00key")->{outcome}, 'committed', 'ERASE commits');
my $loaded = eval { $client->get("binary\x00key") };
ok(!$loaded && ref($@) eq 'GlifiStore::Error' && $@->category eq 'not_found',
    'missing GET raises categorized error');
is($@->wire_status, STATUS_NOT_FOUND, 'not_found exposes wire_status');
is($@->retryability, 'new_attempt', 'not_found retryability');

my $absent = $client->erase('missing-key');
is($absent->{outcome}, 'rejected', 'ERASE absent is rejected');
ok(ref($absent->{error}) eq 'GlifiStore::Error', 'ERASE absent error is structured');
is($absent->{error}->category, 'not_found', 'ERASE absent category');
is($absent->{error}->mutation_outcome, 'rejected', 'ERASE absent mutation_outcome');
is($absent->{error}->retryability, 'new_attempt', 'ERASE absent retryability');

# client-semantics §3: wire NOT_FOUND on pipeline ERASE → failed + rejected.
my $pipe_absent = $client->execute_pipeline([
    { opcode => 'erase', key => 'still-missing' },
]);
is(scalar @$pipe_absent, 1, 'pipeline ERASE absent returns one slot');
is($pipe_absent->[0]->{outcome}, 'failed', 'pipeline ERASE absent is failed');
ok(ref($pipe_absent->[0]->{error}) eq 'GlifiStore::Error', 'pipeline ERASE absent error is structured');
is($pipe_absent->[0]->{error}->category, 'not_found', 'pipeline ERASE absent category');
is($pipe_absent->[0]->{error}->mutation_outcome, 'rejected', 'pipeline ERASE absent mutation_outcome');
is($pipe_absent->[0]->{error}->retryability, 'new_attempt', 'pipeline ERASE absent retryability');

# client-semantics §3/§5: execute_batch absent ERASE → failed + rejected.
my $batch_absent = $client->execute_batch([
    { opcode => 'erase', key => 'batch-missing' },
]);
is(scalar @$batch_absent, 1, 'batch ERASE absent returns one slot');
is($batch_absent->[0]->{outcome}, 'failed', 'batch ERASE absent is failed');
ok(ref($batch_absent->[0]->{error}) eq 'GlifiStore::Error', 'batch ERASE absent error is structured');
is($batch_absent->[0]->{error}->category, 'not_found', 'batch ERASE absent category');
is($batch_absent->[0]->{error}->mutation_outcome, 'rejected', 'batch ERASE absent mutation_outcome');
is($batch_absent->[0]->{error}->retryability, 'new_attempt', 'batch ERASE absent retryability');

# client-semantics §3/§5: pre-sharded worker pipelines inherit pipeline ERASE mapping.
my $worker_absent = $client->execute_worker_pipelines([
    [{ opcode => 'erase', key => 'worker-missing' }],
]);
is(scalar @$worker_absent, 1, 'worker-pipelines ERASE absent returns one Worker');
is(scalar @{$worker_absent->[0]}, 1, 'worker-pipelines ERASE absent returns one slot');
is($worker_absent->[0][0]->{outcome}, 'failed', 'worker-pipelines ERASE absent is failed');
ok(ref($worker_absent->[0][0]->{error}) eq 'GlifiStore::Error', 'worker-pipelines ERASE absent error');
is($worker_absent->[0][0]->{error}->category, 'not_found', 'worker-pipelines ERASE absent category');
is($worker_absent->[0][0]->{error}->mutation_outcome, 'rejected', 'worker-pipelines ERASE absent mutation_outcome');
is($worker_absent->[0][0]->{error}->retryability, 'new_attempt', 'worker-pipelines ERASE absent retryability');

$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(disconnect_on_put => 1);
$client = GlifiStore::Client->connect(port => $port);
my $indeterminate = $client->put('key', 'value');
is($indeterminate->{outcome}, 'indeterminate', 'standalone PUT disconnect is indeterminate');
ok(ref($indeterminate->{error}) eq 'GlifiStore::Error', 'disconnect error is structured');
ok($indeterminate->{error}->bytes_sent > 0, 'disconnect after send exposes bytes_sent');
is($indeterminate->{error}->operation, 'put', 'disconnect error exposes operation');
is($indeterminate->{error}->retryability, 'reconcile_first', 'partial send is reconcile_first');
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(disconnect_on_put => 1);
$client = GlifiStore::Client->connect(port => $port);
$responses = $client->execute_pipeline([
    { opcode => 'put', key => 'key', value => 'value' },
    { opcode => 'get', key => 'key' },
    { opcode => 'erase', key => 'key' },
]);
is($responses->[0]->{outcome}, 'indeterminate', 'sent PUT is indeterminate after disconnect');
is($responses->[1]->{outcome}, 'failed', 'unresolved GET is failed');
is($responses->[2]->{outcome}, 'indeterminate', 'sent ERASE is indeterminate');
ok($responses->[0]->{error}->bytes_sent > 0, 'pipeline PUT exposes bytes_sent');
is($responses->[0]->{error}->retryability, 'reconcile_first', 'pipeline PUT is reconcile_first');
is($responses->[0]->{error}->mutation_outcome, 'indeterminate', 'pipeline PUT mutation_outcome');
is($responses->[2]->{error}->retryability, 'reconcile_first', 'pipeline ERASE is reconcile_first');
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(internal_error_on_put => 1);
$client = GlifiStore::Client->connect(port => $port);
my $ie = $client->put('key', 'value');
is($ie->{outcome}, 'indeterminate', 'PUT INTERNAL_ERROR is indeterminate');
ok(ref($ie->{error}) eq 'GlifiStore::Error', 'INTERNAL_ERROR error is structured');
is($ie->{error}->mutation_outcome, 'indeterminate', 'PUT INTERNAL_ERROR mutation_outcome');
is($ie->{error}->retryability, 'reconcile_first', 'PUT INTERNAL_ERROR is reconcile_first');
ok($ie->{error}->bytes_sent > 0, 'PUT INTERNAL_ERROR exposes bytes_sent');
is($ie->{error}->wire_status, STATUS_INTERNAL_ERROR, 'PUT INTERNAL_ERROR wire_status');
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(worker_count => 2, internal_error_on_put => 1);
$client = GlifiStore::Client->connect(port => $port);
my @ie_keys = (undef, undef);
my $candidate = 0;
while (!defined($ie_keys[0]) || !defined($ie_keys[1])) {
    my $key = sprintf('ie%08d', $candidate++);
    my $w = $client->worker_for($key);
    $ie_keys[$w] //= $key;
}
my $ie_batch = $client->execute_batch([
    { opcode => 'put', key => $ie_keys[0], value => 'a' },
    { opcode => 'put', key => $ie_keys[1], value => 'b' },
]);
is(scalar(@$ie_batch), 2, 'INTERNAL_ERROR batch length');
for my $slot (@$ie_batch) {
    is($slot->{outcome}, 'indeterminate', 'batch INTERNAL_ERROR slot indeterminate');
    is($slot->{error}->mutation_outcome, 'indeterminate', 'batch INTERNAL_ERROR mutation_outcome');
    is($slot->{error}->retryability, 'reconcile_first', 'batch INTERNAL_ERROR reconcile_first');
    ok($slot->{error}->bytes_sent > 0, 'batch INTERNAL_ERROR bytes_sent');
}
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server();
$client = GlifiStore::Client->connect(
    port => $port,
    maximum_pipeline_requests => 1,
);
ok(!eval {
    $client->execute_pipeline([
        { opcode => 'get', key => 'key' },
        { opcode => 'get', key => 'key' },
    ]);
    1;
} && ref($@) eq 'GlifiStore::Error' && $@->category eq 'invalid_argument',
    'pipeline request limit fails before transmission');
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(worker_count => 2);
$client = GlifiStore::Client->connect(port => $port);
is($client->worker_count, 2, 'multi-worker bootstrap discovers both Workers');
my @keys = map { sprintf('mw-%02d', $_) } 0 .. 31;
my %owners = map { $_ => $client->worker_for($_) } @keys;
my %unique_owners = map { $_ => 1 } values %owners;
is_deeply([sort { $a <=> $b } keys %unique_owners], [0, 1], 'keys span both Workers');
for my $key (@keys) {
    my $value = scalar reverse $key;
    is($client->put($key, $value)->{outcome}, 'committed', "multi-worker PUT $key");
}
for my $key (@keys) {
    my $value = scalar reverse $key;
    is($client->get($key), $value, "multi-worker GET $key");
    is(worker_for($key, 2), $owners{$key}, "routing stays stable for $key");
}
$client->close;
waitpid($pid, 0);

my $keyed_routing = {
    algorithm => GlifiStore::Protocol::ROUTING_ALG_SIPHASH24_V1(),
    seed => 1_229_801_703_532_086_340,
};
($port, $pid) = start_server(worker_count => 2, routing => $keyed_routing);
$client = GlifiStore::Client->connect(port => $port);
my $keyed_key = 'tenant-a/orders/1';
is($client->worker_for($keyed_key), worker_for($keyed_key, 2, $keyed_routing),
    'client Worker routing reuses the validated keyed INIT identity');
is($client->put($keyed_key, 'keyed-value')->{outcome}, 'committed',
    'keyed-routing client sends PUT to the selected Worker');
is($client->get($keyed_key), 'keyed-value', 'keyed-routing client reads from the selected Worker');
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(worker_count => 2);
$client = GlifiStore::Client->connect(port => $port);
{
    my @wave = ([], []);
    for my $key (@keys) {
        my $owner = $owners{$key};
        my $value = scalar reverse $key;
        push @{$wave[$owner]}, { opcode => 'put', key => $key, value => "$value-c" };
        push @{$wave[$owner]}, { opcode => 'get', key => $key };
    }
    my $all = $client->execute_worker_pipelines(\@wave);
    is(scalar(@$all), 2, 'concurrent worker pipelines return one slot per Worker');
    for my $worker (0, 1) {
        my $responses = $all->[$worker];
        is(scalar(@$responses), scalar(@{$wave[$worker]}), "worker $worker response count");
        for (my $index = 0; $index < @{$wave[$worker]}; $index += 2) {
            is($responses->[$index]->{outcome}, 'succeeded', "concurrent PUT worker $worker");
            is($responses->[$index + 1]->{value}, $wave[$worker][$index]{value},
                "concurrent GET worker $worker ordered");
        }
    }

    my $worker_zero_key = (grep { $owners{$_} == 0 } @keys)[0];
    my $misrouted = $client->execute_worker_pipelines(
        [[], [{ opcode => 'get', key => $worker_zero_key }]]);
    is($misrouted->[1][0]{outcome}, 'failed',
        'public worker pipeline still rejects a key assigned to another Worker');
    is($misrouted->[1][0]{error}->category, 'invalid_argument',
        'misrouted public worker pipeline preserves its error category');
}
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(worker_count => 2);
$client = GlifiStore::Client->connect(port => $port);
{
    my @batch;
    for my $key (@keys) {
        my $value = scalar reverse $key;
        push @batch, { opcode => 'put', key => $key, value => "$value-b" };
    }
    for my $key (@keys) {
        push @batch, { opcode => 'get', key => $key };
    }
    my $ordered = $client->execute_batch(\@batch);
    is(scalar(@$ordered), scalar(@batch), 'execute_batch restores caller order length');
    for my $index (0 .. $#keys) {
        is($ordered->[$index]->{outcome}, 'succeeded', "batch PUT $keys[$index]");
        is($ordered->[scalar(@keys) + $index]->{value}, (scalar reverse $keys[$index]) . '-b',
            "batch GET $keys[$index] ordered");
    }
}
$client->close;
waitpid($pid, 0);

($port, $pid) = start_server(worker_count => 2);
$client = GlifiStore::Client->connect(
    port => $port,
    maximum_pipeline_requests => 1,
);
{
    my $key0 = (grep { $owners{$_} == 0 } @keys)[0];
    ok(!eval {
        $client->execute_batch([
            { opcode => 'get', key => $key0 },
            { opcode => 'get', key => $key0 },
        ]);
        1;
    } && ref($@) eq 'GlifiStore::Error' && $@->category eq 'invalid_argument',
        'batch per-Worker limit fails before transmission');
}
$client->close;
waitpid($pid, 0);

{
    my $err;
    eval {
        GlifiStore::Client->connect(port => 1, tls => 1, cert_file => 'only-cert.pem');
        1;
    } or $err = $@;
    ok(ref($err) eq 'GlifiStore::Error' && $err->category eq 'invalid_argument',
        'TLS mTLS requires both cert_file and key_file');
}

SKIP: {
    eval { require IO::Socket::SSL; 1 }
        or skip 'IO::Socket::SSL not installed', 1;
    my $tmpdir = File::Temp::tempdir(CLEANUP => 1);
    my $cert = "$tmpdir/server.crt";
    my $key  = "$tmpdir/server.key";
    my $rc = system(
        "openssl req -x509 -newkey rsa:2048 -nodes -keyout '$key' -out '$cert' -days 1 -subj '/CN=localhost' >/dev/null 2>&1"
    );
    skip 'openssl CLI unavailable for ephemeral certs', 1 if $rc != 0;

    my $listener = IO::Socket::INET->new(
        LocalAddr => '127.0.0.1', LocalPort => 0, Proto => 'tcp',
        Listen => 1, ReuseAddr => 1)
        or die "listen failed: $!";
    my $tls_port = $listener->sockport;
    my $tls_pid = fork();
    die "fork failed: $!" if !defined($tls_pid);
    if (!$tls_pid) {
        require IO::Socket::SSL;
        my $plain = $listener->accept;
        close($listener);
        my $ssl = IO::Socket::SSL->start_SSL(
            $plain,
            SSL_server    => 1,
            SSL_cert_file => $cert,
            SSL_key_file  => $key,
            SSL_version   => 'TLSv1_3',
        ) or _exit(1);
        my $bound;
        eval {
            while (1) {
                my $request = receive_request($ssl);
                my $opcode = $request->{opcode};
                if ($opcode == OP_INIT) {
                    send_response(
                        $ssl,
                        status        => STATUS_OK,
                        request_id    => $request->{request_id},
                        owner_worker  => 0,
                        worker_count  => 1,
                        routing_epoch => 9,
                        value         => GlifiStore::Protocol::encode_init_identity()
                    );
                }
                elsif ($opcode == OP_BIND_WORKER) {
                    $bound = $request->{target_worker};
                    send_response(
                        $ssl,
                        status        => STATUS_OK,
                        request_id    => $request->{request_id},
                        owner_worker  => $request->{target_worker},
                        worker_count  => 1,
                        routing_epoch => 9
                    );
                }
                elsif ($opcode == OP_PING) {
                    send_response(
                        $ssl,
                        status        => STATUS_OK,
                        request_id    => $request->{request_id},
                        owner_worker  => $bound // 0,
                        worker_count  => 1,
                        routing_epoch => 9,
                        value         => $request->{value}
                    );
                }
                else {
                    last;
                }
            }
        };
        close($ssl);
        _exit(0);
    }
    close($listener);
    my $tls_client = GlifiStore::Client->connect(
        host        => '127.0.0.1',
        port        => $tls_port,
        tls         => 1,
        tls_ca      => $cert,
        server_name => 'localhost',
    );
    is($tls_client->ping('tls-ping'), 'tls-ping', 'TLS client ping over IO::Socket::SSL');
    $tls_client->close;
    waitpid($tls_pid, 0);
}

{
    my $overloaded = GlifiStore::Error->new('overloaded', 'server is overloaded');
    is($overloaded->retryability, 'never', 'overloaded retryability is never');
}

{
    my $ok = eval {
        GlifiStore::Client->connect(
            unix_socket_path => '/tmp/glifistore-perl-uds-tls-refuse.sock',
            tls              => 1,
        );
        1;
    };
    my $error = $@;
    ok(!$ok, 'AF_UNIX + TLS is refused');
    isa_ok($error, 'GlifiStore::Error');
    is($error->category, 'invalid_argument', 'AF_UNIX + TLS is invalid_argument');
    like("$error", qr/AF_UNIX/, 'AF_UNIX + TLS mentions AF_UNIX');
}

{
    my ($backup_port, $backup_pid) = start_server(internal_error_on_backup => 1);
    my $backup_client = GlifiStore::Client->connect(port => $backup_port);
    my $ok = eval { $backup_client->backup('/tmp/glifistore-perl-backup-internal'); 1 };
    my $error = $@;
    ok(!$ok, 'BACKUP INTERNAL_ERROR raises');
    isa_ok($error, 'GlifiStore::Error');
    is($error->mutation_outcome, 'indeterminate', 'BACKUP INTERNAL_ERROR is indeterminate');
    is($error->retryability, 'reconcile_first', 'BACKUP INTERNAL_ERROR is reconcile_first');
    is($error->wire_status, STATUS_INTERNAL_ERROR, 'BACKUP INTERNAL_ERROR wire_status');
    ok($error->bytes_sent > 0, 'BACKUP INTERNAL_ERROR exposes bytes_sent');
    $backup_client->close;
    kill 'TERM', $backup_pid;
    waitpid($backup_pid, 0);
}

{
    my ($backup_port, $backup_pid) = start_server(wrong_request_id_on_backup => 1);
    my $backup_client = GlifiStore::Client->connect(port => $backup_port);
    my $ok = eval { $backup_client->backup('/tmp/glifistore-perl-backup-wrong-id'); 1 };
    my $error = $@;
    ok(!$ok, 'BACKUP validate failure raises');
    isa_ok($error, 'GlifiStore::Error');
    is($error->mutation_outcome, 'indeterminate', 'BACKUP validate failure is indeterminate');
    is($error->retryability, 'reconcile_first', 'BACKUP validate failure is reconcile_first');
    ok($error->bytes_sent > 0, 'BACKUP validate failure exposes bytes_sent');
    $backup_client->close;
    kill 'TERM', $backup_pid;
    waitpid($backup_pid, 0);
}

done_testing;
