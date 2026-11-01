package GlifiStore::Client;

use v5.32;
use strict;
use warnings;
use Errno qw(EAGAIN EWOULDBLOCK EINTR);
use IO::Select;
use IO::Socket::INET;
use Socket qw(IPPROTO_TCP TCP_NODELAY SOCK_STREAM);
use Time::HiRes qw(clock_gettime CLOCK_MONOTONIC);

sub _now {
    return clock_gettime(CLOCK_MONOTONIC);
}

use GlifiStore::Error;
use GlifiStore::Protocol qw(
    MAX_FRAME_BYTES NO_WORKER RESPONSE_HEADER_BYTES
    OP_INIT OP_PING OP_GET OP_PUT OP_ERASE OP_BIND_WORKER
    OP_HEALTH OP_READY OP_STATS OP_BACKUP
    STATUS_OK STATUS_INVALID_REQUEST STATUS_UNSUPPORTED STATUS_INTERNAL_ERROR
    STATUS_NOT_FOUND STATUS_OVERLOADED STATUS_WRONG_OWNER STATUS_NOT_BOUND
    STATUS_PERMISSION_DENIED
    encode_request_parts encode_request_hot
);
use GlifiStore::SendFailure;

our $VERSION = '0.1.0';

use constant DEFAULT_PORT => 7379;
use constant {
    RESPONSE_STATUS        => 0,
    RESPONSE_REQUEST_ID    => 1,
    RESPONSE_OWNER_WORKER  => 2,
    RESPONSE_WORKER_COUNT  => 3,
    RESPONSE_ROUTING_EPOCH => 4,
    RESPONSE_VALUE         => 5,
};

sub _error {
    my ($category, $message, $fields) = @_;
    return GlifiStore::Error->new($category, $message, $fields);
}
sub _throw { die _error($_[0], $_[1], $_[2]) }

sub _plain_message {
    my ($error) = @_;
    return $error->message if ref($error) && eval { $error->can('message') };
    return "$error" if ref($error);
    $error =~ s/\s+at \S+ line \d+\.?\n?\z//;
    chomp $error;
    return $error;
}

sub connect {
    my ($class, %config) = @_;
    my $self = bless {
        host                      => $config{host} // '127.0.0.1',
        port                      => $config{port} // DEFAULT_PORT,
        unix_socket_path          => $config{unix_socket_path},
        connect_timeout           => $config{connect_timeout} // 3.0,
        request_timeout           => $config{request_timeout} // 5.0,
        maximum_frame_bytes       => $config{maximum_frame_bytes} // MAX_FRAME_BYTES,
        maximum_pipeline_requests => $config{maximum_pipeline_requests} // 256,
        maximum_pipeline_bytes    => $config{maximum_pipeline_bytes} // 1024 * 1024,
        tls                       => $config{tls} ? 1 : 0,
        tls_ca                    => $config{tls_ca} // $config{ca_file},
        cert_file                 => $config{cert_file},
        key_file                  => $config{key_file},
        server_name               => $config{server_name},
        insecure_skip_verify      => $config{insecure_skip_verify} ? 1 : 0,
        worker_count              => 0,
        routing_epoch             => 0,
        routing                   => { algorithm => 1, seed => 0 },
        next_request_id           => 1,
        healthy                   => 1,
        connections               => [],
    }, $class;
    $self->_validate_config;
    my $ok = eval {
        my $first = $self->_new_connection(0);
        my ($worker_count, $routing_epoch, $routing) = $self->_bootstrap($first, undef);
        $self->{worker_count} = $worker_count;
        $self->{routing_epoch} = $routing_epoch;
        $self->{routing} = $routing;
        push @{$self->{connections}}, $first;
        for my $worker (1 .. $worker_count - 1) {
            my $connection = $self->_new_connection($worker);
            $self->_bootstrap($connection, [$worker_count, $routing_epoch, $routing]);
            push @{$self->{connections}}, $connection;
        }
        1;
    };
    if (!$ok) {
        my $error = $@;
        $self->close;
        die $error;
    }
    return $self;
}

sub _validate_config {
    my ($self) = @_;
    my $unix = defined($self->{unix_socket_path}) && length($self->{unix_socket_path});
    if ($unix) {
        _throw('invalid_argument', 'TLS is not supported on AF_UNIX clients') if $self->{tls};
    }
    else {
        _throw('invalid_argument', 'client configuration is outside protocol limits')
            if !length($self->{host})
            || $self->{port} < 1
            || $self->{port} > 65_535;
    }
    _throw('invalid_argument', 'client configuration is outside protocol limits')
        if $self->{connect_timeout} <= 0
        || $self->{request_timeout} <= 0
        || $self->{maximum_frame_bytes} < RESPONSE_HEADER_BYTES
        || $self->{maximum_frame_bytes} > MAX_FRAME_BYTES
        || $self->{maximum_pipeline_requests} < 1
        || $self->{maximum_pipeline_bytes} < 40;
    my $has_cert = defined($self->{cert_file}) && length($self->{cert_file});
    my $has_key  = defined($self->{key_file})  && length($self->{key_file});
    if ($self->{tls} && ($has_cert xor $has_key)) {
        _throw('invalid_argument', 'TLS mTLS requires both cert_file and key_file (fail closed)');
    }
    if (($has_cert || $has_key) && !$self->{tls}) {
        _throw('invalid_argument', 'cert_file/key_file require tls => 1 (fail closed)');
    }
    if ($self->{tls}) {
        eval { require IO::Socket::SSL; 1 }
            or _throw('invalid_argument',
            'TLS was requested but IO::Socket::SSL is not installed (fail closed)');
    }
    return;
}

sub _new_connection {
    my ($self, $worker) = @_;
    return {
        worker       => $worker,
        socket       => undef,
        selector     => undef,
        input        => '',
        input_offset => 0,
    };
}

sub worker_count  { return $_[0]->{worker_count} }
sub routing_epoch { return $_[0]->{routing_epoch} }
sub routing       { return $_[0]->{routing} }
sub healthy       { return $_[0]->{healthy} ? 1 : 0 }

sub worker_for {
    my ($self, $key) = @_;
    return GlifiStore::Protocol::hash_key_routing_prevalidated($key, $self->{routing})
        % $self->{worker_count};
}

sub _next_request_id {
    my ($self) = @_;
    my $current = $self->{next_request_id};
    # Match Python/C++: wrap after the maximum unsigned 64-bit value, skipping 0.
    $self->{next_request_id} = ($current == ~0) ? 1 : $current + 1;
    return $current;
}

sub _encode_request {
    my ($self, $opcode, $request_id, $key, $value, $expire_at_ns, $target_worker) = @_;
    $key //= '';
    $value //= '';
    $expire_at_ns //= 0;
    $target_worker //= NO_WORKER;
    # Hot path: request IDs come from the native counter; expire 0 is the common case.
    if ($expire_at_ns eq '0' || $expire_at_ns == 0) {
        return encode_request_hot($opcode, $request_id, $key, $value, 0, $target_worker);
    }
    return encode_request_parts($opcode, $request_id, $key, $value, $expire_at_ns, $target_worker);
}

sub _encode_request_fragments {
    my ($self, $opcode, $request_id, $key, $value, $expire_at_ns, $target_worker) = @_;
    $key //= '';
    $value //= '';
    $expire_at_ns //= 0;
    $target_worker //= NO_WORKER;
    if ($expire_at_ns eq '0' || $expire_at_ns == 0) {
        return GlifiStore::Protocol::request_fragments_hot_internal($opcode, $request_id, $key,
            $value, 0, $target_worker);
    }
    return GlifiStore::Protocol::request_fragments_parts_internal($opcode, $request_id, $key,
        $value, $expire_at_ns, $target_worker);
}

sub _reset_connection {
    my ($self, $connection) = @_;
    if (my $socket = delete $connection->{socket}) {
        close($socket) or die "close failed: $!\n";
    }
    $connection->{selector} = undef;
    $connection->{input} = '';
    $connection->{input_offset} = 0;
    return;
}

sub close {
    my ($self) = @_;
    $self->{healthy} = 0;
    $self->_reset_connection($_) for @{$self->{connections}};
    return;
}

sub DESTROY {
    my ($self) = @_;
    $self->close if $self->{connections};
    return;
}

sub _open_socket {
    my ($self) = @_;
    if (defined($self->{unix_socket_path}) && length($self->{unix_socket_path})) {
        require IO::Socket::UNIX;
        my $socket = IO::Socket::UNIX->new(
            Peer    => $self->{unix_socket_path},
            Type    => SOCK_STREAM,
            Timeout => $self->{connect_timeout},
        );
        _throw('unavailable', "could not connect to GlifiStore: $!") if !$socket;
        $socket->blocking(0);
        return $socket;
    }
    if (!$self->{tls}) {
        my $socket = IO::Socket::INET->new(
            PeerAddr => $self->{host},
            PeerPort => $self->{port},
            Proto    => 'tcp',
            Timeout  => $self->{connect_timeout},
        );
        _throw('unavailable', "could not connect to GlifiStore: $!") if !$socket;
        setsockopt($socket, IPPROTO_TCP, TCP_NODELAY, pack('i', 1));
        $socket->blocking(0);
        return $socket;
    }

    require IO::Socket::SSL;
    my $server_name
        = (defined($self->{server_name}) && length($self->{server_name}))
        ? $self->{server_name}
        : $self->{host};
    my %ssl_args = (
        PeerHost        => $self->{host},
        PeerPort        => $self->{port},
        Proto           => 'tcp',
        Timeout         => $self->{connect_timeout},
        SSL_version     => 'TLSv1_3',
        SSL_hostname    => $server_name,
        SSL_verifycn_name => $server_name,
    );
    if ($self->{insecure_skip_verify}) {
        $ssl_args{SSL_verify_mode} = IO::Socket::SSL::SSL_VERIFY_NONE();
    }
    else {
        $ssl_args{SSL_verify_mode} = IO::Socket::SSL::SSL_VERIFY_PEER();
        $ssl_args{SSL_ca_file} = $self->{tls_ca}
            if defined($self->{tls_ca}) && length($self->{tls_ca});
    }
    if (defined($self->{cert_file}) && length($self->{cert_file})) {
        $ssl_args{SSL_cert_file} = $self->{cert_file};
        $ssl_args{SSL_key_file}  = $self->{key_file};
    }
    my $socket = IO::Socket::SSL->new(%ssl_args);
    if (!$socket) {
        my $err = $IO::Socket::SSL::SSL_ERROR // $!;
        _throw('unavailable', "could not connect to GlifiStore over TLS: $err");
    }
    setsockopt($socket, IPPROTO_TCP, TCP_NODELAY, pack('i', 1));
    $socket->blocking(0);
    return $socket;
}

sub _remaining {
    my ($deadline) = @_;
    my $remaining = $deadline - _now();
    _throw('transport', 'request deadline expired') if $remaining <= 0;
    return $remaining;
}

sub _wait_io {
    my ($selector, $mode, $deadline) = @_;
    while (1) {
        my $remaining = _remaining($deadline);
        my @ready
            = $mode eq 'write'
            ? $selector->can_write($remaining)
            : $selector->can_read($remaining);
        return 1 if @ready;
        next if $! == EINTR;
        return 0;
    }
    return;
}

sub _selector {
    my ($connection) = @_;
    return $connection->{selector} if $connection->{selector};
    return $connection->{selector} = IO::Select->new($connection->{socket});
}

sub _send {
    my ($self, $connection, $frame, $deadline) = @_;
    local $SIG{PIPE} = 'IGNORE';
    my $socket = $connection->{socket};
    my $selector = _selector($connection);
    $deadline //=  _now() + $self->{request_timeout};
    my $total = length($frame);
    my $sent = 0;
    while ($sent < $total) {
        if (!_wait_io($selector, 'write', $deadline)) {
            die GlifiStore::SendFailure->new(_error('transport', 'request send deadline expired'),
                $sent);
        }
        my $written = syswrite($socket, $frame, $total - $sent, $sent);
        if (defined($written) && $written > 0) {
            $sent += $written;
            next;
        }
        next if !defined($written) && ($! == EINTR || $! == EAGAIN || $! == EWOULDBLOCK);
        my $message = defined($written) ? 'socket closed during send' : "request send failed: $!";
        die GlifiStore::SendFailure->new(_error('transport', $message), $sent);
    }
    return $sent;
}

sub _receive_response {
    my ($self, $connection, $deadline, $selector, $socket_ready) = @_;
    my $socket = $connection->{socket};
    $selector //= _selector($connection);
    $deadline //=  _now() + $self->{request_timeout};
    my $max_frame = $self->{maximum_frame_bytes};
    while (1) {
        my $input = $connection->{input};
        my $offset = $connection->{input_offset};
        my $available = length($input) - $offset;
        if ($available >= 4) {
            my $frame_size = unpack('V', substr($input, $offset, 4));
            _throw('protocol', 'server response size is outside client limits')
                if $frame_size < RESPONSE_HEADER_BYTES || $frame_size > $max_frame;
            if ($available >= $frame_size) {
                my $frame = substr($input, $offset, $frame_size);
                $offset += $frame_size;
                if ($offset == length($input)) {
                    $connection->{input} = '';
                    $connection->{input_offset} = 0;
                }
                else {
                    $connection->{input_offset} = $offset;
                }
                my $response= eval {
                    GlifiStore::Protocol::decode_response_fields_internal($frame, $max_frame)
                };
                _throw('protocol', 'invalid server response: ' . _plain_message($@)) if !$response;
                return $response;
            }
        }
        if ($offset) {
            substr($connection->{input}, 0, $offset, '');
            $connection->{input_offset} = 0;
        }
        if ($socket_ready) {
            $socket_ready = 0;
        }
        else {
            _throw('transport', 'response receive deadline expired')
                if !_wait_io($selector, 'read', $deadline);
        }
        my $received
            = sysread($socket,$connection->{input},256 * 1024,length($connection->{input}),);
        if (defined($received) && $received > 0) {
            next;
        }
        next if !defined($received) && ($! == EINTR || $! == EAGAIN || $! == EWOULDBLOCK);
        _throw('transport',
            defined($received)? 'server closed the connection' : "response receive failed: $!");
    }
    return;
}

sub _exchange {
    my ($self, $connection, $frame, $deadline) = @_;
    $deadline //=  _now() + $self->{request_timeout};
    $self->_send($connection, $frame, $deadline);
    return $self->_receive_response($connection, $deadline);
}

sub _bootstrap {
    my ($self, $connection, $expected) = @_;
    $self->_reset_connection($connection);
    $connection->{socket} = $self->_open_socket;
    my $result = eval {
        my $init_id = $self->_next_request_id;
        my $response = $self->_exchange($connection, $self->_encode_request(OP_INIT, $init_id));
        _throw('protocol', 'server INIT response is inconsistent')
            if $response->[RESPONSE_STATUS] != STATUS_OK
            || $response->[RESPONSE_REQUEST_ID] != $init_id
            || $response->[RESPONSE_WORKER_COUNT] < 1
            || $response->[RESPONSE_WORKER_COUNT] > 256
            || !$response->[RESPONSE_ROUTING_EPOCH];
        my $routing
            = eval { GlifiStore::Protocol::decode_init_identity($response->[RESPONSE_VALUE]) };
        _throw('protocol', 'server INIT response is inconsistent') if !$routing;
        my @metadata
            = ($response->[RESPONSE_WORKER_COUNT], $response->[RESPONSE_ROUTING_EPOCH], $routing);
        if ($expected) {
            _throw('unavailable', 'server routing metadata changed during bootstrap')
                if $metadata[0] != $expected->[0]
                || $metadata[1] != $expected->[1]
                || $metadata[2]->{algorithm} != $expected->[2]->{algorithm}
                || $metadata[2]->{seed} != $expected->[2]->{seed};
        }
        my $bind_id = $self->_next_request_id;
        my $bound = $self->_exchange($connection,
            $self->_encode_request(OP_BIND_WORKER, $bind_id, '', '', 0, $connection->{worker}));
        _throw('protocol', 'server BIND_WORKER response is inconsistent')
            if $bound->[RESPONSE_STATUS] != STATUS_OK
            || $bound->[RESPONSE_REQUEST_ID] != $bind_id
            || $bound->[RESPONSE_OWNER_WORKER] != $connection->{worker}
            || $bound->[RESPONSE_WORKER_COUNT] != $metadata[0]
            || $bound->[RESPONSE_ROUTING_EPOCH] != $metadata[1];
        \@metadata;
    };
    if (!$result) {
        my $error = $@;
        $self->_reset_connection($connection);
        if (ref($error) eq 'GlifiStore::SendFailure') {
            _throw('unavailable', $error->{error}->{message});
        }
        if (ref($error) eq 'GlifiStore::Error' && $error->{category} eq 'transport') {
            _throw('unavailable', $error->{message});
        }
        die $error;
    }
    return @$result;
}

sub _ensure_connected {
    my ($self, $connection) = @_;
    return if $connection->{socket};
    $self->_bootstrap($connection,
        [$self->{worker_count}, $self->{routing_epoch}, $self->{routing}]);
    return;
}

sub _validate_response {
    my ($self, $response, $request_id, $worker) = @_;
    _throw('protocol', 'server response request ID does not match')
        if $response->[RESPONSE_REQUEST_ID] != $request_id;
    if ($response->[RESPONSE_WORKER_COUNT] != $self->{worker_count}
        || $response->[RESPONSE_ROUTING_EPOCH] != $self->{routing_epoch})
    {
        $self->{healthy} = 0;
        _throw('unavailable', 'server routing metadata changed');
    }
    if ($response->[RESPONSE_OWNER_WORKER] != $worker
        && $response->[RESPONSE_STATUS] != STATUS_WRONG_OWNER)
    {
        $self->{healthy} = 0;
        _throw('protocol', 'server response came from the wrong Worker');
    }
    return;
}

sub _status_error {
    my ($status) = @_;
    my $error;
    $error = _error('not_found', 'key was not found') if $status == STATUS_NOT_FOUND;
    $error = _error('overloaded', 'server is overloaded') if $status == STATUS_OVERLOADED;
    $error = _error('unavailable', 'server connection is not bound') if $status == STATUS_NOT_BOUND;
    $error = _error('permission_denied', 'server denied the request')
        if $status == STATUS_PERMISSION_DENIED;
    $error = _error('protocol', 'server rejected Worker routing') if $status == STATUS_WRONG_OWNER;
    $error = _error('invalid_argument', 'server rejected the request')
        if $status == STATUS_INVALID_REQUEST || $status == STATUS_UNSUPPORTED;
    $error = _error('internal', 'server reported an internal error')
        if $status == STATUS_INTERNAL_ERROR || $status == STATUS_OK;
    $error //= _error('protocol', 'server returned an unknown status');
    return $error->enrich(wire_status => $status);
}

sub get {
    my ($self, $key, %options) = @_;
    return $self->_read(OP_GET, $key, '', $options{timeout});
}

sub ping {
    my ($self, $payload, %options) = @_;
    return $self->_read(OP_PING, '', $payload // '', $options{timeout});
}

sub health {
    my ($self, %options) = @_;
    return $self->_read(OP_HEALTH, '', '', $options{timeout});
}

sub ready {
    my ($self, %options) = @_;
    return $self->_read(OP_READY, '', '', $options{timeout});
}

sub stats {
    my ($self, %options) = @_;
    return $self->_read(OP_STATS, '', '', $options{timeout});
}

sub backup {
    my ($self, $destination, %options) = @_;
    _throw('invalid_argument', 'backup destination must be non-empty')
        if !defined $destination || $destination eq '';
    # Online fenced BACKUP (opcode 10): not hot zero-impact; admin under secure authz.
    return $self->_read(OP_BACKUP, $destination, '', $options{timeout});
}

sub put {
    my ($self, $key, $value, %options) = @_;
    return $self->_mutate(OP_PUT, $key, $value, $options{expire_at_ns} // 0, $options{timeout});
}

sub erase {
    my ($self, $key, %options) = @_;
    return $self->_mutate(OP_ERASE, $key, '', 0, $options{timeout});
}

sub _request_deadline {
    my ($self, $timeout) = @_;
    if (defined $timeout) {
        _throw('invalid_argument', 'request timeout must be positive') if $timeout <= 0;
        return _now() + $timeout;
    }
    return _now() + $self->{request_timeout};
}

sub _read {
    my ($self, $opcode, $key, $value, $timeout) = @_;
    _throw('unavailable', 'client is closed or routing metadata changed') if !$self->{healthy};
    my $deadline = $self->_request_deadline($timeout);
    my $worker
        = ($opcode == OP_PING
            || $opcode == OP_BACKUP
            || $opcode == OP_HEALTH
            || $opcode == OP_READY
            || $opcode == OP_STATS) ? 0 : $self->worker_for($key);
    my $connection = $self->{connections}->[$worker];
    my $last_error = _error('unavailable', 'request was not attempted');
    for (1 .. 2) {
        my $frame_bytes = 0;
        my $response = eval {
            $self->_ensure_connected($connection);
            my $request_id = $self->_next_request_id;
            my $frame = eval {$self->_encode_request($opcode, $request_id, $key, $value)};
            _throw('invalid_argument', _plain_message($@)) if !$frame;
            _throw('invalid_argument', 'request exceeds the configured frame limit')
                if length($frame) > $self->{maximum_frame_bytes};
            $frame_bytes = length($frame);
            my $received = $self->_exchange($connection, $frame, $deadline);
            $self->_validate_response($received, $request_id, $worker);
            [$received, $request_id];
        };
        if (!$response) {
            my $error = $@;
            $self->_reset_connection($connection);
            if (ref($error) eq 'GlifiStore::SendFailure') {
                my $bytes_sent = $error->{bytes_sent} // 0;
                if ($opcode == OP_BACKUP && $bytes_sent > 0) {
                    die $error->{error}->enrich(
                        operation        => 'backup',
                        bytes_sent       => $bytes_sent,
                        mutation_outcome => 'indeterminate',
                    );
                }
                $last_error = $error->{error};
                next;
            }
            if (ref($error) eq 'GlifiStore::Error') {
                if ($opcode == OP_BACKUP && $frame_bytes > 0) {
                    # Validate / receive failure after send — do not blind-retry BACKUP.
                    die $error->enrich(
                        operation        => 'backup',
                        bytes_sent       => $frame_bytes,
                        mutation_outcome => 'indeterminate',
                    );
                }
                if ($error->{category} eq 'transport') {
                    $last_error = $error;
                    next;
                }
                if ($error->{category} eq 'unavailable') {
                    die $error if !$self->{healthy};
                    $last_error = $error;
                    next;
                }
            }
            die $error;
        }
        my $received   = $response->[0];
        my $request_id = $response->[1];
        if ($received->[RESPONSE_STATUS] != STATUS_OK) {
            $self->{healthy} = 0
                if $received->[RESPONSE_STATUS] == STATUS_WRONG_OWNER
                || $received->[RESPONSE_STATUS] == STATUS_NOT_BOUND;
            my $error = _status_error($received->[RESPONSE_STATUS]);
            if ($opcode == OP_BACKUP && $received->[RESPONSE_STATUS] == STATUS_INTERNAL_ERROR) {
                # Fenced copy may already be committed — same polarity as C++.
                die $error->enrich(
                    operation        => 'backup',
                    request_id       => $request_id,
                    worker           => $worker,
                    routing_epoch    => $self->{routing_epoch},
                    bytes_sent       => $frame_bytes,
                    mutation_outcome => 'indeterminate',
                );
            }
            die $error;
        }
        return $received->[RESPONSE_VALUE];
    }
    die $last_error;
}

sub _mutate {
    my ($self, $opcode, $key, $value, $expire_at_ns, $timeout) = @_;
    return {
        outcome => 'rejected',
        error   => _error('unavailable', 'client is closed or routing metadata changed'),
        }
        if !$self->{healthy};
    my $deadline = eval { $self->_request_deadline($timeout) };
    if (!$deadline) {
        my $error = $@;
        return {
            outcome => 'rejected',
            error   => ref($error) eq 'GlifiStore::Error'
            ? $error
            : _error('invalid_argument', _plain_message($error)),
        };
    }
    my $worker = $self->worker_for($key);
    my $connection = $self->{connections}->[$worker];
    for my $attempt (0 .. 1) {
        my $connected = eval { $self->_ensure_connected($connection); 1 };
        if (!$connected) {
            my $error = $@;
            return {
                outcome => 'rejected',
                error   => ref($error) eq 'GlifiStore::Error'
                ? $error
                : _error('unavailable', _plain_message($error)),
            };
        }
        my $request_id = $self->_next_request_id;
        my $frame= eval {$self->_encode_request($opcode, $request_id, $key, $value, $expire_at_ns)};
        return {
            outcome => 'rejected',
            error   => _error('invalid_argument', _plain_message($@)),
            }
            if !$frame;
        return {
            outcome => 'rejected',
            error   => _error('invalid_argument', 'request exceeds the configured frame limit'),
            }
            if length($frame) > $self->{maximum_frame_bytes};
        my $response = eval {
            my $received = $self->_exchange($connection, $frame, $deadline);
            $self->_validate_response($received, $request_id, $worker);
            $received;
        };
        if (!$response) {
            my $error = $@;
            $self->_reset_connection($connection);
            if (ref($error) eq 'GlifiStore::SendFailure') {
                if (!$error->{bytes_sent}) {
                    next if !$attempt;
                    return {
                        outcome => 'rejected',
                        error   => $error->{error}->enrich(
                            bytes_sent       => 0,
                            request_id       => $request_id,
                            worker           => $worker,
                            routing_epoch    => $self->{routing_epoch},
                            mutation_outcome => 'rejected',
                            operation        => ($opcode == OP_PUT ? 'put' : 'erase'),
                        ),
                    };
                }
                return {
                    outcome => 'indeterminate',
                    error   => $error->{error}->enrich(
                        bytes_sent       => $error->{bytes_sent},
                        request_id       => $request_id,
                        worker           => $worker,
                        routing_epoch    => $self->{routing_epoch},
                        mutation_outcome => 'indeterminate',
                        operation        => ($opcode == OP_PUT ? 'put' : 'erase'),
                    ),
                };
            }
            return {
                outcome => 'indeterminate',
                error   => (
                    ref($error) eq 'GlifiStore::Error'
                    ? $error
                    : _error('transport', _plain_message($error))
                )->enrich(
                    bytes_sent       => length($frame),
                    request_id       => $request_id,
                    worker           => $worker,
                    routing_epoch    => $self->{routing_epoch},
                    mutation_outcome => 'indeterminate',
                    operation        => ($opcode == OP_PUT ? 'put' : 'erase'),
                ),
            };
        }
        if ($response->[RESPONSE_STATUS] == STATUS_OK) {
            if (length($response->[RESPONSE_VALUE])) {
                $self->_reset_connection($connection);
                return {
                    outcome => 'indeterminate',
                    error   => _error('protocol', 'mutation response value must be empty')->enrich(
                        bytes_sent       => length($frame),
                        request_id       => $request_id,
                        worker           => $worker,
                        routing_epoch    => $self->{routing_epoch},
                        mutation_outcome => 'indeterminate',
                        operation        => ($opcode == OP_PUT ? 'put' : 'erase'),
                    ),
                };
            }
            return { outcome => 'committed', error => undef };
        }
        my $error = _status_error($response->[RESPONSE_STATUS]);
        my $op = $opcode == OP_PUT ? 'put' : 'erase';
        if ($response->[RESPONSE_STATUS] == STATUS_INTERNAL_ERROR) {
            return {
                outcome => 'indeterminate',
                error   => $error->enrich(
                    bytes_sent       => length($frame),
                    request_id       => $request_id,
                    worker           => $worker,
                    routing_epoch    => $self->{routing_epoch},
                    mutation_outcome => 'indeterminate',
                    operation        => $op,
                    wire_status      => $response->[RESPONSE_STATUS],
                ),
            };
        }
        $self->{healthy} = 0
            if $response->[RESPONSE_STATUS] == STATUS_WRONG_OWNER
            || $response->[RESPONSE_STATUS] == STATUS_NOT_BOUND;
        return {
            outcome => 'rejected',
            error   => $error->enrich(
                bytes_sent       => length($frame),
                request_id       => $request_id,
                worker           => $worker,
                routing_epoch    => $self->{routing_epoch},
                mutation_outcome => 'rejected',
                operation        => $op,
                wire_status      => $response->[RESPONSE_STATUS],
            ),
        };
    }
    return { outcome => 'rejected', error => _error('unavailable', 'could not send mutation') };
}

sub _mark_unresolved_pipeline_responses {
    my ($responses, $normalized, $metadata, $first, $error, $bytes_sent, $worker, $routing_epoch)
        = @_;
    if (ref($error) ne 'GlifiStore::Error') {
        $error = _error('transport', _plain_message($error));
    }
    for my $index ($first .. $#$normalized) {
        my $op = $normalized->[$index];
        my $is_mutation = ($op eq 'put' || $op eq 'erase');
        my $mutation_may_have_arrived = $is_mutation && $bytes_sent > $metadata->[$index][1];
        my $pos_bytes
            = $bytes_sent > $metadata->[$index][1] ? $bytes_sent - $metadata->[$index][1] : 0;
        my %fields = (
            operation     => $op,
            request_id    => $metadata->[$index][0],
            worker        => $worker,
            routing_epoch => $routing_epoch,
            bytes_sent    => $pos_bytes,
        );
        if ($is_mutation) {
            $fields{mutation_outcome}= $mutation_may_have_arrived ? 'indeterminate' : 'rejected';
        }
        my $enriched = GlifiStore::Error->new(
            $error->category,
            $error->message,
            {
                wire_status => $error->wire_status,
            }
        )->enrich(%fields);
        $responses->[$index] = {
            outcome => $mutation_may_have_arrived ? 'indeterminate' : 'failed',
            value   => '',
            error   => $enriched,
        };
    }
    return;
}

sub _apply_pipeline_response {
    my ($self, $index, $response, $normalized, $worker, $responses, $metadata, $output_len) = @_;
    if ($response->[RESPONSE_STATUS] == STATUS_OK) {
        if (($normalized->[$index] eq 'put' || $normalized->[$index] eq 'erase')
            && length($response->[RESPONSE_VALUE]))
        {
            return _error('protocol', 'mutation response value must be empty');
        }
        $responses->[$index] = {
            outcome => 'succeeded',
            value   => $response->[RESPONSE_VALUE],
            error   => undef,
        };
        return;
    }
    my $op = $normalized->[$index];
    my $is_mutation = ($op eq 'put' || $op eq 'erase');
    my $indeterminate= $is_mutation && $response->[RESPONSE_STATUS] == STATUS_INTERNAL_ERROR;
    my %fields = (
        operation     => $op,
        request_id    => $metadata->[$index][0],
        worker        => $worker,
        routing_epoch => $self->{routing_epoch},
        bytes_sent    => $output_len - $metadata->[$index][1],
        wire_status   => $response->[RESPONSE_STATUS],
    );
    if ($is_mutation) {
        $fields{mutation_outcome} = $indeterminate ? 'indeterminate' : 'rejected';
    }
    my $error = _status_error($response->[RESPONSE_STATUS])->enrich(%fields);
    $responses->[$index] = {
        outcome => $indeterminate ? 'indeterminate' : 'failed',
        value => '',
        error => $error,
    };
    $self->{healthy} = 0
        if $response->[RESPONSE_STATUS] == STATUS_WRONG_OWNER
        || $response->[RESPONSE_STATUS] == STATUS_NOT_BOUND;
    return;
}

sub execute_pipeline {
    my ($self, $requests, %options) = @_;
    _throw('invalid_argument', 'pipeline requests must be an array reference')
        if ref($requests) ne 'ARRAY';
    return [] if !@$requests;
    _throw('unavailable', 'client is closed or routing metadata changed') if !$self->{healthy};
    my $deadline = $self->_request_deadline($options{timeout});

    my $first_key = $requests->[0]{key} // '';
    my $worker = $self->worker_for($first_key);
    my $encoded = $self->_encode_pipeline_batch($worker, $requests);
    my $normalized = $encoded->{normalized};
    my $metadata = $encoded->{metadata};
    my $output = $encoded->{output};

    my @responses = (undef) x scalar(@$requests);
    my $connection = $self->{connections}->[$worker];
    _throw('unavailable', 'client closed before pipeline admission') if !$self->{healthy};
    $self->_ensure_connected($connection);

    my $sent = eval { $self->_send($connection, $output, $deadline) };
    if (!defined($sent)) {
        my $failure = $@;
        $self->_reset_connection($connection);
        my $error = ref($failure) eq 'GlifiStore::SendFailure'? $failure->{error} : $failure;
        my $bytes_sent = ref($failure) eq 'GlifiStore::SendFailure'? $failure->{bytes_sent} : 0;
        _mark_unresolved_pipeline_responses(\@responses, $normalized, $metadata, 0, $error,
            $bytes_sent, $worker, $self->{routing_epoch});
        return \@responses;
    }

    my $receive_selector = _selector($connection);
    for my $index (0 .. $#$normalized) {
        my $response = eval {
            my $received = $self->_receive_response($connection, $deadline, $receive_selector);
            $self->_validate_response($received, $metadata->[$index][0], $worker);
            $received;
        };
        if (!$response) {
            my $error = $@;
            $self->_reset_connection($connection);
            _mark_unresolved_pipeline_responses(\@responses, $normalized, $metadata, $index,
                $error, length($output), $worker, $self->{routing_epoch});
            return \@responses;
        }
        my $protocol_error
            = $self->_apply_pipeline_response($index, $response, $normalized, $worker, \@responses,
            $metadata, length($output));
        if ($protocol_error) {
            $self->_reset_connection($connection);
            _mark_unresolved_pipeline_responses(\@responses, $normalized, $metadata, $index,
                $protocol_error, length($output), $worker, $self->{routing_epoch});
            return \@responses;
        }
    }
    return \@responses;
}

sub execute_batch {
    my ($self, $requests, %options) = @_;
    _throw('invalid_argument', 'batch requests must be an array reference')
        if ref($requests) ne 'ARRAY';
    return [] if !@$requests;
    _throw('unavailable', 'client is closed or routing metadata changed') if !$self->{healthy};
    my $deadline = $self->_request_deadline($options{timeout});

    my @batches = map { [] } 0 .. $self->{worker_count} - 1;
    my @original_indices = map { [] } 0 .. $self->{worker_count} - 1;
    for my $index (0 .. $#$requests) {
        my $request = $requests->[$index];
        _throw('invalid_argument', 'batch request must be a hash reference')
            if ref($request) ne 'HASH';
        my $name = $request->{opcode} // '';
        _throw('invalid_argument', 'batch request contains an invalid opcode')
            if $name ne 'get' && $name ne 'put' && $name ne 'erase';
        my $key = $request->{key} // '';
        my $worker = $self->worker_for($key);
        if (@{$batches[$worker]} >= $self->{maximum_pipeline_requests}) {
            _throw('invalid_argument', 'batch exceeds the configured per-Worker request limit');
        }
        push @{$batches[$worker]}, $request;
        push @{$original_indices[$worker]}, $index;
    }

    my $worker_results = $self->_execute_worker_pipelines(\@batches, $deadline, 1);
    my @responses = (undef) x scalar(@$requests);
    for my $worker (0 .. $self->{worker_count} - 1) {
        my $group = $worker_results->[$worker] // [];
        my $indices = $original_indices[$worker];
        for my $offset (0 .. $#$indices) {
            $responses[$indices->[$offset]] = $group->[$offset]
                if defined $group->[$offset];
        }
    }
    return \@responses;
}

# Drive one pipeline batch per Worker concurrently via a shared select loop.
# $batches is an arrayref indexed by Worker; each element is an arrayref of requests (or undef/[]).
sub _pipeline_failed_responses {
    my ($self, $requests, $error, $fallback_category) = @_;
    my $base
        =ref($error) eq 'GlifiStore::Error'
        ? $error
        : _error($fallback_category, _plain_message($error));
    my @responses;
    for my $req (@$requests) {
        my $name = $req->{opcode} // '';
        my %fields = (
            operation     => $name eq '' ? 'get' : $name,
            bytes_sent    => 0,
            routing_epoch => $self->{routing_epoch},
        );
        if ($name eq 'put' || $name eq 'erase') {
            my $worker = eval { $self->worker_for($req->{key} // '') };
            $fields{worker} = $worker if $@ eq '';
            $fields{mutation_outcome} = 'rejected';
        }
        elsif (defined $req->{key}) {
            my $worker = eval { $self->worker_for($req->{key}) };
            $fields{worker} = $worker if $@ eq '';
        }
        push @responses,
            {
            outcome => 'failed',
            value   => '',
            error   => GlifiStore::Error->new(
                $base->category,
                $base->message,
                {
                    wire_status => $base->wire_status,
                }
            )->enrich(%fields),
            };
    }
    return \@responses;
}

sub _abort_active_worker_pipelines {
    my ($self, $active, $results, $error) = @_;
    for my $state (@$active) {
        $self->_reset_connection($state->{connection});
        $self->_fail_worker_pipeline_state($state, $error, $state->{sent});
        $results->[$state->{worker}] = $state->{responses};
    }
    return;
}

sub _remove_worker_pipeline_state {
    my ($state, $socket, $write_set, $read_set, $by_fd, $active, $results) = @_;
    my $fd = fileno($socket);
    $write_set->remove($socket);
    $read_set->remove($socket);
    delete $by_fd->{$fd};
    $results->[$state->{worker}] = $state->{responses};
    @$active = grep { $_ != $state } @$active;
    return;
}

sub _write_worker_pipeline_socket {
    my ($self, $socket, $state, $write_set, $read_set, $by_fd, $active, $results) = @_;
    my $written = syswrite($socket,$state->{output},length($state->{output}) - $state->{sent},
        $state->{sent},);
    if (defined($written) && $written > 0) {
        $state->{sent} += $written;
        if ($state->{sent} >= length($state->{output})) {
            $write_set->remove($socket);
            $read_set->add($socket);
        }
        return 1;
    }
    return 1 if !defined($written) && ($! == EINTR || $! == EAGAIN || $! == EWOULDBLOCK);
    $self->_reset_connection($state->{connection});
    $self->_fail_worker_pipeline_state(
        $state,
        defined($written)
        ? _error('transport', 'socket closed during send')
        : _error('transport', "request send failed: $!"),
        $state->{sent},
    );
    _remove_worker_pipeline_state($state, $socket, $write_set, $read_set, $by_fd, $active,$results);
    return 0;
}

sub _read_worker_pipeline_socket {
    my ($self, $socket, $state, $deadline, $write_set, $read_set, $by_fd, $active, $results)= @_;
    my $progress = eval {
        $self->_drive_worker_pipeline_read($state, $deadline);
        1;
    };
    if (!$progress) {
        my $error = $@;
        $self->_reset_connection($state->{connection});
        $self->_fail_worker_pipeline_state($state, $error, length($state->{output}));
        _remove_worker_pipeline_state($state, $socket, $write_set, $read_set, $by_fd, $active,
            $results);
        return 0;
    }
    if ($state->{next_index} >= @{$state->{normalized}}) {
        _remove_worker_pipeline_state($state, $socket, $write_set, $read_set, $by_fd, $active,
            $results);
    }
    return 1;
}

sub execute_worker_pipelines {
    my ($self, $batches, %options) = @_;
    _throw('invalid_argument', 'worker pipelines must be an array reference')
        if ref($batches) ne 'ARRAY';
    _throw('unavailable', 'client is closed or routing metadata changed') if !$self->{healthy};
    my $worker_count = $self->{worker_count};
    _throw('invalid_argument', 'worker pipeline vector does not match Worker count')
        if @$batches != $worker_count;

    my $deadline
        = defined $options{deadline}
        ? $options{deadline}
        : $self->_request_deadline($options{timeout});
    return $self->_execute_worker_pipelines($batches, $deadline, 0);
}

sub _execute_worker_pipelines {
    my ($self, $batches, $deadline, $routing_validated) = @_;
    my $worker_count = $self->{worker_count};
    my @results = map { undef } 0 .. $worker_count - 1;
    my (@active, %by_fd);
    local $SIG{PIPE} = 'IGNORE';
    my $write_set = IO::Select->new;
    my $read_set = IO::Select->new;

    for my $worker (0 .. $worker_count - 1) {
        my $requests = $batches->[$worker];
        next if !defined($requests) || !@$requests;
        my $encoded= eval { $self->_encode_pipeline_batch($worker, $requests, $routing_validated) };
        if (!$encoded) {
            $results[$worker]= $self->_pipeline_failed_responses($requests, $@, 'invalid_argument');
            next;
        }
        my $connection = $self->{connections}->[$worker];
        my $ok = eval { $self->_ensure_connected($connection); 1 };
        if (!$ok) {
            $results[$worker] = $self->_pipeline_failed_responses($requests, $@, 'unavailable');
            next;
        }
        my $socket = $connection->{socket};
        my $state = {
            worker     => $worker,
            connection => $connection,
            normalized => $encoded->{normalized},
            metadata   => $encoded->{metadata},
            output     => $encoded->{output},
            sent       => 0,
            next_index => 0,
            responses  => [(undef) x scalar(@{$encoded->{normalized}})],
        };
        push @active, $state;
        $by_fd{fileno($socket)} = $state;
        $write_set->add($socket);
    }

    return [ map { $_ // [] } @results ] if !@active;

    my $deadline_error = _error('transport', 'request deadline expired');
    while (@active) {
        my $remaining = $deadline - _now();
        if ($remaining <= 0) {
            _abort_active_worker_pipelines($self, \@active, \@results, $deadline_error);
            last;
        }
        my ($readers, $writers) = IO::Select->select(
            $read_set->count ? $read_set : undef,
            $write_set->count ? $write_set : undef,
            undef,$remaining,
        );
        next if $! == EINTR && !$readers && !$writers;
        if (!$readers && !$writers) {
            _abort_active_worker_pipelines($self, \@active, \@results, $deadline_error);
            last;
        }

        for my $socket (@{$writers // []}) {
            my $state = $by_fd{fileno($socket)} // next;
            _write_worker_pipeline_socket($self, $socket, $state, $write_set, $read_set,\%by_fd,
                \@active, \@results);
        }

        for my $socket (@{$readers // []}) {
            my $state = $by_fd{fileno($socket)} // next;
            _read_worker_pipeline_socket(
                $self, $socket, $state, $deadline, $write_set,
                $read_set,\%by_fd, \@active, \@results
            );
        }
    }

    return [ map { $_ // [] } @results ];
}

sub _encode_pipeline_batch {
    my ($self, $worker, $requests, $routing_validated) = @_;
    _throw('invalid_argument', 'pipeline exceeds the configured request limit')
        if @$requests > $self->{maximum_pipeline_requests};
    state %wire_opcode = (get => OP_GET, put => OP_PUT, erase => OP_ERASE);
    my (@normalized, @metadata);
    my $output = '';
    my $output_size = 0;
    my $max_frame = $self->{maximum_frame_bytes};
    my $max_pipeline = $self->{maximum_pipeline_bytes};

    for my $request (@$requests) {
        _throw('invalid_argument', 'pipeline request must be a hash reference')
            if ref($request) ne 'HASH';
        my $name = $request->{opcode} // '';
        my $opcode = $wire_opcode{$name};
        _throw('invalid_argument', 'pipeline request contains an invalid opcode')
            if !defined $opcode;
        my $key = $request->{key} // '';
        my $value = $request->{value} // '';
        my $expire_at_ns = $request->{expire_at_ns} // 0;
        if (!$routing_validated) {
            my $owner = $self->worker_for($key);
            _throw('invalid_argument', 'every pipeline key must route to the same Worker')
                if $owner != $worker;
        }
        _throw('invalid_argument', 'GET and ERASE pipeline requests cannot carry PUT fields')
            if ($name eq 'get' || $name eq 'erase')
            && (length($value) || ($expire_at_ns ne '0' && $expire_at_ns != 0));
        my $request_id = $self->_next_request_id;
        my ($header, $wire_key, $wire_value, $frame_len)
            =$self->_encode_request_fragments($opcode, $request_id, $key, $value, $expire_at_ns);
        _throw('invalid_argument', 'pipeline request exceeds the configured frame limit')
            if $frame_len > $max_frame;
        _throw('invalid_argument', 'pipeline exceeds the configured aggregate byte limit')
            if $frame_len > $max_pipeline - $output_size;
        push @normalized, $name;
        push @metadata, [$request_id, $output_size];
        $output .= $header;
        $output .= $wire_key;
        $output .= $wire_value;
        $output_size += $frame_len;
    }
    return {
        normalized => \@normalized,
        metadata   => \@metadata,
        output     => $output,
    };
}

sub _fail_worker_pipeline_state {
    my ($self, $state, $error, $bytes_sent) = @_;
    if (ref($error) ne 'GlifiStore::Error') {
        if (ref($error) eq 'GlifiStore::SendFailure') {
            $bytes_sent = $error->{bytes_sent};
            $error = $error->{error};
        }
        else {
            $error = _error('transport', _plain_message($error));
        }
    }
    # Match single-Worker execute_pipeline: enrich per-slot bytes_sent / mutation_outcome
    # so indeterminate slots advertise reconcile_first (not bare same_request / new_attempt).
    _mark_unresolved_pipeline_responses(
        $state->{responses},$state->{normalized},$state->{metadata},$state->{next_index},$error,
        $bytes_sent // 0,
        $state->{worker},$self->{routing_epoch},
    );
    return;
}

sub _record_worker_pipeline_response {
    my ($self, $state, $index, $received) = @_;
    my $name = $state->{normalized}[$index];
    if ($received->[RESPONSE_STATUS] == STATUS_OK) {
        if (($name eq 'put' || $name eq 'erase') && length($received->[RESPONSE_VALUE])) {
            _throw('protocol', 'mutation response value must be empty');
        }
        $state->{responses}[$index] = {
            outcome => 'succeeded',
            value   => $received->[RESPONSE_VALUE],
            error   => undef,
        };
        return;
    }
    my $is_mutation = ($name eq 'put' || $name eq 'erase');
    my $indeterminate= $is_mutation && $received->[RESPONSE_STATUS] == STATUS_INTERNAL_ERROR;
    my $output_len = length($state->{output} // '');
    my %fields = (
        operation     => $name,
        request_id    => $state->{metadata}[$index][0],
        worker        => $state->{worker},
        routing_epoch => $self->{routing_epoch},
        bytes_sent    => $output_len - $state->{metadata}[$index][1],
        wire_status   => $received->[RESPONSE_STATUS],
    );
    if ($is_mutation) {
        $fields{mutation_outcome} = $indeterminate ? 'indeterminate' : 'rejected';
    }
    my $error = _status_error($received->[RESPONSE_STATUS])->enrich(%fields);
    $state->{responses}[$index] = {
        outcome => $indeterminate ? 'indeterminate' : 'failed',
        value   => '',
        error   => $error,
    };
    $self->{healthy} = 0
        if $received->[RESPONSE_STATUS] == STATUS_WRONG_OWNER
        || $received->[RESPONSE_STATUS] == STATUS_NOT_BOUND;
    return;
}

sub _drive_worker_pipeline_read {
    my ($self, $state, $deadline) = @_;
    my $connection = $state->{connection};
    my $index = $state->{next_index};
    return if $index >= @{$state->{normalized}};
    # The shared select loop dispatched this state from its readable set. Consume that readiness
    # once instead of issuing a duplicate select before the first sysread; partial reads still
    # return through the normal deadline-aware wait path.
    my $received = $self->_receive_response($connection, $deadline, undef, 1);
    $self->_validate_response($received, $state->{metadata}[$index][0], $state->{worker});
    $self->_record_worker_pipeline_response($state, $index, $received);
    $state->{next_index} = $index + 1;

    while ($state->{next_index} < @{$state->{normalized}}) {
        my $available = length($connection->{input}) - $connection->{input_offset};
        last if $available < 4;
        my $frame_size = unpack('V', substr($connection->{input}, $connection->{input_offset}, 4));
        last if $available < $frame_size;
        $received = $self->_receive_response($connection, $deadline);
        $index = $state->{next_index};
        $self->_validate_response($received, $state->{metadata}[$index][0], $state->{worker});
        $self->_record_worker_pipeline_response($state, $index, $received);
        $state->{next_index} = $index + 1;
    }
    return;
}

1;

__END__

=encoding utf8

=head1 NAME

GlifiStore::Client - synchronous TCP client for GlifiStore

=head1 VERSION

Version 0.1.0

=head1 SYNOPSIS

    use GlifiStore::Client;

    my $cache = GlifiStore::Client->connect(port => 7379);
    my $put = $cache->put("key", "value");
    my $value = $cache->get("key") if $put->{outcome} eq 'committed';
    my $responses = $cache->execute_pipeline([
        { opcode => 'put', key => 'key', value => 'next' },
        { opcode => 'get', key => 'key' },
    ]);
    $cache->close;

=head1 DESCRIPTION

C<GlifiStore::Client> opens one bound TCP connection per Worker advertised by
the server C<INIT> response. Keys and values are opaque byte strings. Mutations
return a hash reference with C<outcome> set to C<committed>, C<rejected>, or
C<indeterminate>. Reads throw L<GlifiStore::Error> objects with a C<category>
and C<message>.

Request deadlines use a monotonic clock (C<CLOCK_MONOTONIC> via L<Time::HiRes>).
Perl ithreads are not a shared-client concurrency model; use one client per
process or thread. Do not reuse sockets across C<fork>. For multi-Worker throughput in one process, prefer
C<execute_worker_pipelines>.

=head1 SUBROUTINES/METHODS

=over 4

=item connect(%config)

=item get($key)

=item put($key, $value, %options)

=item erase($key)

=item ping($payload?)

=item execute_pipeline(\@requests)

=item execute_batch(\@requests)

Group requests by Worker, run one pipeline per Worker (overlapping I/O when multiple Workers are
used), and restore caller order. Not an atomic transaction.

=item execute_worker_pipelines(\@batches_by_worker)

Run one pipeline batch per Worker concurrently (shared C<select> loop). C<@batches_by_worker>
is indexed by Worker id; empty slots are skipped.

=item worker_for($key)

=item worker_count

=item routing_epoch

=item healthy

=item close

=back

=head1 DIAGNOSTICS

Reads throw L<GlifiStore::Error> objects. Mutations and pipeline entries return
hash references with C<outcome> and optional C<error>.

=head1 CONFIGURATION AND ENVIRONMENT

C<connect> accepts C<host>, C<port>, C<unix_socket_path> (AF_UNIX; mutually exclusive
with TLS), C<connect_timeout>, C<request_timeout>, C<maximum_frame_bytes>,
C<maximum_pipeline_requests>, and C<maximum_pipeline_bytes>.

=head1 DEPENDENCIES

L<GlifiStore::Protocol>, L<GlifiStore::Error>, L<GlifiStore::SendFailure>, and
core modules: C<Errno>, C<IO::Select>, C<IO::Socket::INET>, C<IO::Socket::UNIX>,
C<Socket>, C<Time::HiRes>.
Optional TLS requires C<IO::Socket::SSL> (fail closed if C<tls =E<gt> 1> without it).

=head1 INCOMPATIBILITIES

Perl ithreads do not share one client safely; use one client per process or thread.

=head1 BUGS AND LIMITATIONS

None known.

=head1 AUTHOR

Giacomo Picchiarelli

=head1 LICENSE AND COPYRIGHT

Copyright (c) 2026, Giacomo Picchiarelli.

BSD 3-Clause. See the distribution F<LICENSE>.

=cut
