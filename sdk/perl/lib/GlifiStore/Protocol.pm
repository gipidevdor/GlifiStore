package GlifiStore::Protocol;

use v5.32;
use strict;
use warnings;
use Config;
use Exporter 'import';

our $VERSION = '0.1.0';

BEGIN {
    die "GlifiStore::Protocol requires a Perl with 64-bit integers (pack 'Q<')\n"
        unless defined($Config{use64bitint})
        && $Config{use64bitint} eq 'define'
        && ($Config{uvsize} // 0) >= 8;
}

our @EXPORT_OK = qw(
    PROTOCOL_VERSION REQUEST_HEADER_BYTES RESPONSE_HEADER_BYTES MAX_FRAME_BYTES NO_WORKER
    IDENTITY ROUTING_ALG_FNV1A64_V1 ROUTING_ALG_SIPHASH24_V1 WORKER_ROUTING_SIP_KEY1_XOR
    INIT_IDENTITY_EXTENDED_BYTES
    OP_INIT OP_PING OP_GET OP_PUT OP_ERASE OP_BIND_WORKER OP_HEALTH OP_READY OP_STATS OP_BACKUP
    STATUS_OK STATUS_INVALID_REQUEST STATUS_UNSUPPORTED STATUS_INTERNAL_ERROR
    STATUS_NOT_FOUND STATUS_OVERLOADED STATUS_WRONG_OWNER STATUS_NOT_BOUND STATUS_PERMISSION_DENIED
    encode_request encode_request_parts encode_request_hot decode_request encode_response
    decode_response fnv1a64 siphash24 hash_key_routing encode_init_identity decode_init_identity
    worker_for
);

use constant PROTOCOL_VERSION      => 2;
use constant REQUEST_HEADER_BYTES  => 40;
use constant RESPONSE_HEADER_BYTES => 40;
use constant MAX_FRAME_BYTES       => 2 * 1024 * 1024;
use constant NO_WORKER             => 4_294_967_295;
use constant IDENTITY              => 'GlifiStore/2';
use constant ROUTING_ALG_FNV1A64_V1 => 1;
use constant ROUTING_ALG_SIPHASH24_V1 => 2;
use constant WORKER_ROUTING_SIP_KEY1_XOR => 7_640_891_576_956_012_809; # 0x6a09e667f3bcc909
use constant INIT_IDENTITY_EXTENDED_BYTES => 25;
use constant FNV1A_OFFSET          => 14_695_981_039_346_656_037;
use constant FNV1A_PRIME           => 1_099_511_628_211;
use constant U64_MAX               => '18446744073709551615';
use constant SIP_C0                => 8_317_987_319_222_330_741; # 0x736f6d6570736575
use constant SIP_C1                => 7_237_128_888_997_146_477; # 0x646f72616e646f6d
use constant SIP_C2                => 7_816_392_313_619_706_465; # 0x6c7967656e657261
use constant SIP_C3                => 8_387_220_255_154_660_723; # 0x7465646279746573
use constant U32_MASK              => 4_294_967_295;
use constant SIP_ROTATE_V1_FIRST   => 13;
use constant SIP_ROTATE_V3_SECOND  => 21;
use constant SIP_ROTATE_V1_SECOND  => 17;
use constant SIP_LENGTH_SHIFT      => 56;
use constant SIP_FINAL_XOR         => 255;

use constant OP_INIT        => 1;
use constant OP_PING        => 2;
use constant OP_GET         => 3;
use constant OP_PUT         => 4;
use constant OP_ERASE       => 5;
use constant OP_BIND_WORKER => 6;
use constant OP_HEALTH      => 7;
use constant OP_READY       => 8;
use constant OP_STATS       => 9;
use constant OP_BACKUP      => 10;

use constant STATUS_OK              => 0;
use constant STATUS_INVALID_REQUEST => 1;
use constant STATUS_UNSUPPORTED     => 2;
use constant STATUS_INTERNAL_ERROR  => 3;
use constant STATUS_NOT_FOUND       => 4;
use constant STATUS_OVERLOADED      => 5;
use constant STATUS_WRONG_OWNER     => 6;
use constant STATUS_NOT_BOUND       => 7;
use constant STATUS_PERMISSION_DENIED => 8;

# Native little-endian u64 fields (requires use64bitint).
my $REQUEST_FORMAT  = 'VvCC Q< VV Q< VV';
my $RESPONSE_FORMAT = 'Vvv Q< VVVV Q<';

sub _require_bytes {
    my ($value, $field) = @_;
    return '' unless defined $value;
    # Hot path: already an octet string — return without copying.
    return $value unless utf8::is_utf8($value);
    my $copy = $value;
    utf8::downgrade($copy, 1)
        or die "$field contains wide characters; encode to octets before use\n";
    return $copy;
}

# Accept digit strings and native non-negative integers; return a UV suitable for pack 'Q<'.
sub _as_u64 {
    my ($value, $field) = @_;
    die "$field must be a non-negative integer\n"
        if !defined($value) || $value !~ /\A(?:0|[1-9][0-9]*)\z/;
    die "$field is outside unsigned 64-bit range\n"
        if length($value) > 20 || (length($value) == 20 && $value gt U64_MAX);
    # pack/unpack keeps full unsigned range without NV rounding above 2^53.
    return unpack('Q<', pack('Q<', $value));
}

sub encode_request_parts {
    my ($opcode, $request_id, $key, $value, $expire_at_ns, $target_worker) = @_;
    die "unknown protocol-v2 opcode\n"
        if !defined($opcode) || $opcode < OP_INIT || $opcode > OP_BACKUP;

    $key   = _require_bytes($key, 'key');
    $value = _require_bytes($value, 'value');
    my $expire = _as_u64($expire_at_ns // 0, 'expire_at_ns');
    my $rid    = _as_u64($request_id, 'request_id');
    $target_worker = NO_WORKER unless defined $target_worker;
    die "target_worker is outside unsigned 32-bit range\n"
        if $target_worker < 0 || $target_worker > NO_WORKER;

    return _pack_request($opcode, $rid, $key, $value, $expire, $target_worker);
}

sub request_fragments_parts_internal {
    my ($opcode, $request_id, $key, $value, $expire_at_ns, $target_worker) = @_;
    die "unknown protocol-v2 opcode\n"
        if !defined($opcode) || $opcode < OP_INIT || $opcode > OP_BACKUP;
    $key   = _require_bytes($key, 'key');
    $value = _require_bytes($value, 'value');
    my $expire = _as_u64($expire_at_ns // 0, 'expire_at_ns');
    my $rid    = _as_u64($request_id, 'request_id');
    $target_worker = NO_WORKER unless defined $target_worker;
    die "target_worker is outside unsigned 32-bit range\n"
        if $target_worker < 0 || $target_worker > NO_WORKER;
    return _request_fragments($opcode, $rid, $key, $value, $expire, $target_worker);
}

# Client hot path: request_id / expire_at_ns are already native UVs in range.
sub encode_request_hot {
    my ($opcode, $request_id, $key, $value, $expire_at_ns, $target_worker) = @_;
    die "unknown protocol-v2 opcode\n"
        if !defined($opcode) || $opcode < OP_INIT || $opcode > OP_BACKUP;
    $key   = _require_bytes($key, 'key');
    $value = _require_bytes($value, 'value');
    $expire_at_ns = 0 unless defined $expire_at_ns;
    $target_worker = NO_WORKER unless defined $target_worker;
    return _pack_request($opcode, $request_id, $key, $value, $expire_at_ns, $target_worker);
}

sub request_fragments_hot_internal {
    my ($opcode, $request_id, $key, $value, $expire_at_ns, $target_worker) = @_;
    die "unknown protocol-v2 opcode\n"
        if !defined($opcode) || $opcode < OP_INIT || $opcode > OP_BACKUP;
    $key   = _require_bytes($key, 'key');
    $value = _require_bytes($value, 'value');
    $expire_at_ns = 0 unless defined $expire_at_ns;
    $target_worker = NO_WORKER unless defined $target_worker;
    return _request_fragments($opcode, $request_id, $key, $value, $expire_at_ns, $target_worker);
}

sub _validate_data_request_fields {
    my ($opcode, $key_len, $value_len, $expire, $target_worker) = @_;
    if ($opcode == OP_GET && (!$key_len || $value_len || $expire || $target_worker != NO_WORKER)) {
        die "GET request requires a key and cannot carry value, expiry, or target_worker\n";
    }
    if ($opcode == OP_PUT && (!$key_len || $target_worker != NO_WORKER)) {
        die "PUT request requires a key and cannot carry target_worker\n";
    }
    if ($opcode == OP_ERASE
        && (!$key_len || $value_len || $expire || $target_worker != NO_WORKER))
    {
        die "ERASE request requires a key and cannot carry value, expiry, or target_worker\n";
    }
    return;
}

sub _validate_control_request_fields {
    my ($opcode, $key_len, $value_len, $expire, $target_worker) = @_;
    if ($opcode == OP_INIT && ($key_len || $value_len || $expire || $target_worker != NO_WORKER)) {
        die "INIT request cannot carry key, value, expiry, or target_worker\n";
    }
    if ($opcode == OP_PING && ($key_len || $expire || $target_worker != NO_WORKER)) {
        die "PING request cannot carry key, expiry, or target_worker\n";
    }
    if ($opcode == OP_BIND_WORKER && ($key_len || $value_len || $expire)) {
        die "BIND_WORKER request cannot carry key, value, or expiry\n";
    }
    if ($opcode == OP_BIND_WORKER && $target_worker == NO_WORKER) {
        die "BIND_WORKER request requires an explicit target_worker\n";
    }
    if (($opcode == OP_HEALTH || $opcode == OP_READY || $opcode == OP_STATS)
        && ($key_len || $value_len || $expire || $target_worker != NO_WORKER))
    {
        die "lifecycle probe cannot carry key, value, expiry, or target_worker\n";
    }
    if ($opcode == OP_BACKUP && (!$key_len || $value_len || $expire || $target_worker != NO_WORKER))
    {
        die "BACKUP requires a destination path key and no value, expiry, or target_worker\n";
    }
    return;
}

sub _validate_request_fields {
    my ($opcode, $key_len, $value_len, $expire, $target_worker) = @_;
    if ($opcode >= OP_GET && $opcode <= OP_ERASE) {
        _validate_data_request_fields($opcode, $key_len, $value_len, $expire, $target_worker);
        return;
    }
    _validate_control_request_fields($opcode, $key_len, $value_len, $expire, $target_worker);
    return;
}

sub _pack_request {
    my ($opcode, $rid, $key, $value, $expire, $target_worker) = @_;
    my ($header, $wire_key, $wire_value)
        =_request_fragments($opcode, $rid, $key, $value, $expire, $target_worker);
    return $header . $wire_key . $wire_value;
}

sub _request_fragments {
    my ($opcode, $rid, $key, $value, $expire, $target_worker) = @_;
    my $key_len = length($key);
    my $value_len = length($value);
    _validate_request_fields($opcode, $key_len, $value_len, $expire, $target_worker);

    my $frame_size = REQUEST_HEADER_BYTES + $key_len + $value_len;
    die "request exceeds the protocol frame limit\n" if $frame_size > MAX_FRAME_BYTES;
    my $header = pack($REQUEST_FORMAT,
        $frame_size, PROTOCOL_VERSION, $opcode, 0,$rid,$key_len, $value_len,$expire,
        $target_worker, 0,);
    return ($header, $key, $value, $frame_size);
}

sub encode_request {
    my (%request) = @_;
    return encode_request_parts($request{opcode},$request{request_id},$request{key},$request{value},
        $request{expire_at_ns},$request{target_worker},);
}

sub decode_request {
    my ($frame, $maximum) = @_;
    $maximum //= MAX_FRAME_BYTES;
    die "request is shorter than its header\n" if length($frame) < REQUEST_HEADER_BYTES;
    my (
        $frame_size, $version, $opcode, $flags,$request_id,
        $key_size, $value_size,$expire_at_ns, $target_worker, $reserved
    ) = unpack($REQUEST_FORMAT, $frame);
    die "request frame extent is invalid\n"
        if $frame_size != length($frame) || $frame_size > $maximum;
    die "request protocol version is unsupported\n" if $version != PROTOCOL_VERSION;
    die "request canonical fields are invalid\n" if $flags != 0 || $reserved != 0;
    die "unknown protocol-v2 opcode\n" if $opcode < OP_INIT || $opcode > OP_BACKUP;
    die "request payload extent is invalid\n"
        if REQUEST_HEADER_BYTES + $key_size + $value_size != $frame_size;
    my $key = substr($frame, REQUEST_HEADER_BYTES, $key_size);
    my $value = substr($frame, REQUEST_HEADER_BYTES + $key_size, $value_size);
    _validate_request_fields($opcode, $key_size, $value_size, $expire_at_ns, $target_worker);
    return {
        opcode         => $opcode,
        request_id     => $request_id,
        expire_at_ns   => $expire_at_ns,
        target_worker  => $target_worker,
        key            => $key,
        value          => $value,
    };
}

sub encode_response {
    my (%response) = @_;
    my $status = $response{status};
    die "unknown protocol-v2 status\n"
        if !defined($status) || $status < STATUS_OK || $status > STATUS_PERMISSION_DENIED;
    my $request_id = _as_u64($response{request_id}, 'request_id');
    my $value = _require_bytes($response{value}, 'value');
    my $owner_worker = $response{owner_worker} // NO_WORKER;
    my $worker_count = $response{worker_count} // 0;
    my $routing_epoch = _as_u64($response{routing_epoch} // 0, 'routing_epoch');
    die "owner_worker is outside unsigned 32-bit range\n"
        if $owner_worker < 0 || $owner_worker > NO_WORKER;
    die "worker_count is outside unsigned 32-bit range\n"
        if $worker_count < 0 || $worker_count > NO_WORKER;
    my $frame_size = RESPONSE_HEADER_BYTES + length($value);
    die "response exceeds the protocol frame limit\n" if $frame_size > MAX_FRAME_BYTES;
    return pack($RESPONSE_FORMAT,
        $frame_size, PROTOCOL_VERSION, $status,$request_id,length($value),
        $owner_worker, $worker_count, 0,$routing_epoch,)
        . $value;
}

# Client-only compact result shape. This is intentionally not exported: public codec callers keep
# the named hash returned by decode_response, while the hot client path avoids a transient hash.
sub decode_response_fields_internal {
    my ($frame, $maximum) = @_;
    $maximum //= MAX_FRAME_BYTES;
    die "response is shorter than its header\n" if length($frame) < RESPONSE_HEADER_BYTES;
    my (
        $frame_size, $version, $status,$request_id, $value_size,
        $owner_worker,$worker_count, $reserved, $routing_epoch
    ) = unpack($RESPONSE_FORMAT, $frame);
    die "response frame extent is invalid\n"
        if $frame_size != length($frame) || $frame_size > $maximum;
    die "response protocol version is unsupported\n" if $version != PROTOCOL_VERSION;
    die "response reserved field is noncanonical\n" if $reserved != 0;
    die "unknown protocol-v2 status\n" if $status < STATUS_OK || $status > STATUS_PERMISSION_DENIED;
    die "response value extent is invalid\n"
        if RESPONSE_HEADER_BYTES + $value_size != $frame_size;
    return [
        $status, $request_id, $owner_worker, $worker_count, $routing_epoch,
        substr($frame, RESPONSE_HEADER_BYTES, $value_size),
    ];
}

sub decode_response {
    my ($frame, $maximum) = @_;
    my $fields = decode_response_fields_internal($frame, $maximum);
    return {
        status        => $fields->[0],
        request_id    => $fields->[1],
        owner_worker  => $fields->[2],
        worker_count  => $fields->[3],
        routing_epoch => $fields->[4],
        value         => $fields->[5],
    };
}

sub fnv1a64 {
    my ($key) = @_;
    $key = _require_bytes($key, 'key');
    my $hash;
    {
        use integer;
        $hash = FNV1A_OFFSET;
        for my $byte (unpack('C*', $key)) {
            $hash ^= $byte;
            $hash *= FNV1A_PRIME;
        }
    }
    return unpack('Q<', pack('q<', $hash));
}

# Force a native value into unsigned 64-bit (matches C++ uint64_t wraps).
sub _u64 {
    my ($value) = @_;
    use integer;
    return unpack('Q<', pack('q<', $value));
}

sub _add64 {
    my ($a, $b) = @_;
    use integer;
    return unpack('Q<', pack('q<', unpack('q<', pack('Q<', $a)) + unpack('q<', pack('Q<', $b))));
}

sub _xor64 {
    my ($a, $b) = @_;
    use integer;
    return unpack('Q<', pack('q<', unpack('q<', pack('Q<', $a)) ^ unpack('q<', pack('Q<', $b))));
}

sub _rotl64 {
    my ($value, $shift) = @_;
    $shift %= 64;
    return unpack('Q<', pack('Q<', $value)) if $shift == 0;
    my ($lo, $hi) = unpack('V2', pack('Q<', $value));
    if ($shift >= 32) {
        ($lo, $hi) = ($hi, $lo);
        $shift -= 32;
        return unpack('Q<', pack('V2', $lo, $hi)) if $shift == 0;
    }
    my $new_hi = (($hi << $shift) | ($lo >> (32 - $shift))) & U32_MASK;
    my $new_lo = (($lo << $shift) | ($hi >> (32 - $shift))) & U32_MASK;
    return unpack('Q<', pack('V2', $new_lo, $new_hi));
}

sub _sipround {
    my ($v0, $v1, $v2, $v3) = @_;
    $v0 = _add64($v0, $v1);
    $v1 = _rotl64($v1, SIP_ROTATE_V1_FIRST);
    $v1 = _xor64($v1, $v0);
    $v0 = _rotl64($v0, 32);
    $v2 = _add64($v2, $v3);
    $v3 = _rotl64($v3, 16);
    $v3 = _xor64($v3, $v2);
    $v0 = _add64($v0, $v3);
    $v3 = _rotl64($v3, SIP_ROTATE_V3_SECOND);
    $v3 = _xor64($v3, $v0);
    $v2 = _add64($v2, $v1);
    $v1 = _rotl64($v1, SIP_ROTATE_V1_SECOND);
    $v1 = _xor64($v1, $v2);
    $v2 = _rotl64($v2, 32);
    return ($v0, $v1, $v2, $v3);
}

# SipHash-2-4 (Aumasson/Bernstein). Independent implementation for siphash24-v1 routing;
# see repository THIRD_PARTY_NOTICES.md. Not a copy of a third-party source tree.
sub siphash24 {
    my ($key, $k0, $k1) = @_;
    $key = _require_bytes($key, 'key');
    $k0 = _u64($k0);
    $k1 = _u64($k1);
    my $v0 = _xor64($k0, SIP_C0);
    my $v1 = _xor64($k1, SIP_C1);
    my $v2 = _xor64($k0, SIP_C2);
    my $v3 = _xor64($k1, SIP_C3);
    my $length = length($key);
    my $offset = 0;

    while ($offset + 8 <= $length) {
        my $message = unpack('Q<', substr($key, $offset, 8));
        $v3 = _xor64($v3, $message);
        ($v0, $v1, $v2, $v3) = _sipround($v0, $v1, $v2, $v3);
        ($v0, $v1, $v2, $v3) = _sipround($v0, $v1, $v2, $v3);
        $v0 = _xor64($v0, $message);
        $offset += 8;
    }
    my $message = _u64($length << SIP_LENGTH_SHIFT);
    my $i = 0;
    while ($offset + $i < $length) {
        $message = _xor64($message, _u64(ord(substr($key, $offset + $i, 1)) << (8 * $i)));
        $i += 1;
    }
    $v3 = _xor64($v3, $message);
    ($v0, $v1, $v2, $v3) = _sipround($v0, $v1, $v2, $v3);
    ($v0, $v1, $v2, $v3) = _sipround($v0, $v1, $v2, $v3);
    $v0 = _xor64($v0, $message);
    $v2 = _xor64($v2, SIP_FINAL_XOR);
    for (1 .. 4) {
        ($v0, $v1, $v2, $v3) = _sipround($v0, $v1, $v2, $v3);
    }
    return _xor64(_xor64($v0, $v1), _xor64($v2, $v3));
}

sub _normalize_routing {
    my ($routing) = @_;
    $routing //= {};
    my $algorithm = $routing->{algorithm} // ROUTING_ALG_FNV1A64_V1;
    my $seed = $routing->{seed} // 0;
    if ($algorithm == ROUTING_ALG_FNV1A64_V1) {
        die "fnv1a64-v1 Worker routing requires a zero hash seed\n" if $seed != 0;
    }
    elsif ($algorithm != ROUTING_ALG_SIPHASH24_V1) {
        die "unsupported Worker routing algorithm\n";
    }
    return { algorithm => $algorithm, seed => $seed };
}

sub hash_key_routing {
    my ($key, $routing) = @_;
    my $state = _normalize_routing($routing);
    return hash_key_routing_prevalidated($key, $state);
}

# Client bootstrap already validates and freezes this state. This internal fast-path entry point
# avoids allocating a fresh normalized routing hash for every request; callers must not expose an
# unvalidated state through it.
sub hash_key_routing_prevalidated {
    my ($key, $state) = @_;
    if ($state->{algorithm} == ROUTING_ALG_SIPHASH24_V1) {
        return siphash24($key, $state->{seed}, _xor64($state->{seed}, WORKER_ROUTING_SIP_KEY1_XOR));
    }
    return fnv1a64($key);
}

sub encode_init_identity {
    my ($routing) = @_;
    my $state = _normalize_routing($routing);
    return IDENTITY if $state->{algorithm} != ROUTING_ALG_SIPHASH24_V1;
    return IDENTITY . "\0" . pack('L<Q<', $state->{algorithm}, $state->{seed});
}

sub decode_init_identity {
    my ($value) = @_;
    $value = _require_bytes($value, 'value');
    if ($value eq IDENTITY) {
        return { algorithm => ROUTING_ALG_FNV1A64_V1, seed => 0 };
    }
    die "server INIT identity value has unexpected length\n"
        if length($value) != INIT_IDENTITY_EXTENDED_BYTES;
    die "server INIT identity prefix is invalid\n"
        if substr($value, 0, length(IDENTITY)) ne IDENTITY
        || substr($value, length(IDENTITY), 1) ne "\0";
    my ($algorithm, $seed) = unpack('L<Q<', substr($value, length(IDENTITY) + 1));
    my $state = _normalize_routing({ algorithm => $algorithm, seed => $seed });
    die "server INIT extended identity must use siphash24-v1 routing\n"
        if $state->{algorithm} != ROUTING_ALG_SIPHASH24_V1;
    return $state;
}

sub worker_for {
    my ($key, $worker_count, $routing) = @_;
    die "worker_count must be positive\n" if !$worker_count || $worker_count < 0;
    return hash_key_routing($key, $routing) % $worker_count;
}

1;

__END__

=encoding utf8

=head1 NAME

GlifiStore::Protocol - wire-protocol v2 codec and Worker routing

=head1 VERSION

Version 0.1.0

=head1 SYNOPSIS

    use GlifiStore::Protocol qw(
        OP_PUT encode_request encode_request_parts decode_response worker_for
    );

    my $frame = encode_request_parts(OP_PUT, 1, "k", "v", 0, 0xFFFF_FFFF);

=head1 DESCRIPTION

Canonical little-endian framing for GlifiStore wire protocol v2. Keys and
values must be B<octet strings> (UTF8 flag off). Wide characters are rejected.

Requires a Perl built with 64-bit integers. Hot paths use native C<pack 'Q<'>,
avoid copying octet strings, and compute FNV-1a with wrapped 64-bit arithmetic.

=head1 SUBROUTINES/METHODS

Exported helpers include C<encode_request>, C<encode_request_parts>,
C<encode_request_hot>, C<decode_request>, C<encode_response>, C<decode_response>,
and C<worker_for>. Opcode and status constants are listed in C<@EXPORT_OK>.

=head1 DIAGNOSTICS

Invalid frames, opcodes, or field ranges cause C<die> with a short message.
Wide-character keys or values are rejected before encoding.

=head1 CONFIGURATION AND ENVIRONMENT

Requires C<use64bitint> and at least 8-byte C<UV> from the Perl build (see
C<Config>).

=head1 DEPENDENCIES

Core modules only: C<Config>, C<Exporter>.

=head1 INCOMPATIBILITIES

Perl builds without 64-bit integers cannot load this module.

=head1 BUGS AND LIMITATIONS

None known.

=head1 AUTHOR

Giacomo Picchiarelli

=head1 LICENSE AND COPYRIGHT

Copyright (c) 2026, Giacomo Picchiarelli.

BSD 3-Clause. See the distribution F<LICENSE>.

=cut
