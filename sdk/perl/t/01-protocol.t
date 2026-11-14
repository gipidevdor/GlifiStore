use v5.32;
use strict;
use warnings;
use FindBin;
use Test::More;

use lib "$FindBin::Bin/../lib";
use GlifiStore::Protocol qw(
    OP_INIT OP_PING OP_GET OP_PUT OP_ERASE OP_BIND_WORKER OP_HEALTH OP_READY OP_STATS OP_BACKUP
    INIT_IDENTITY_EXTENDED_BYTES
    encode_request decode_request encode_response decode_response worker_for
);

sub fixture {
    my ($name) = @_;
    my $path = "$FindBin::Bin/fixtures/$name";
    open(my $handle, '<', $path) or die "cannot read $path: $!";
    local $/;
    my $text = <$handle>;
    close($handle);
    return pack('C*', map { hex($_) } split(/\s+/, $text));
}

sub frames {
    my ($corpus) = @_;
    my @frames;
    my $offset = 0;
    while ($offset < length($corpus)) {
        my $size = unpack('V', substr($corpus, $offset, 4));
        push @frames, substr($corpus, $offset, $size);
        $offset += $size;
    }
    return @frames;
}

my @expected_requests = frames(fixture('wire_requests_v2.hex'));
my @encoded_requests = (
    encode_request(opcode => OP_INIT, request_id => 1),
    encode_request(opcode => OP_PING, request_id => 2, value => "\x00ping\xff"),
    encode_request(opcode => OP_GET, request_id => 3, key => "get\x00key"),
    encode_request(opcode => OP_PUT, request_id => 4, key => "put\x00key",
        value => "\x10\x20\xff", expire_at_ns => 123_456_789),
    encode_request(opcode => OP_ERASE, request_id => 5, key => 'erase-key'),
    encode_request(opcode => OP_BIND_WORKER, request_id => 6, target_worker => 2),
    encode_request(opcode => OP_HEALTH, request_id => 7),
    encode_request(opcode => OP_READY, request_id => 8),
    encode_request(opcode => OP_STATS, request_id => 9),
    encode_request(opcode => OP_BACKUP, request_id => 10, key => '/tmp/glifistore-backup'),
);
is_deeply(\@encoded_requests, \@expected_requests, 'request encoder matches canonical corpus');

my @decoded_requests = map { decode_request($_) } @expected_requests;
is_deeply([map { $_->{opcode} } @decoded_requests], [1 .. 10], 'request decoder covers every opcode');
is($decoded_requests[1]->{value}, "\x00ping\xff", 'PING payload is binary exact');
is($decoded_requests[3]->{expire_at_ns}, 123_456_789, 'PUT expiry is preserved');
my @reencoded_requests = map {
    encode_request(
        opcode => $_->{opcode},
        request_id => $_->{request_id},
        key => $_->{key},
        value => $_->{value},
        expire_at_ns => $_->{expire_at_ns},
        target_worker => $_->{target_worker},
    )
} @decoded_requests;
is_deeply(\@reencoded_requests, \@expected_requests, 'request codec round-trips fixtures');

my @expected_responses = frames(fixture('wire_responses_v2.hex'));
my @responses = map { decode_response($_) } @expected_responses;
is_deeply([map { $_->{status} } @responses], [0 .. 8], 'response decoder covers every status');
is($responses[0]->{value}, 'GlifiStore/2', 'identity value is binary exact');
is($responses[6]->{owner_worker}, 2, 'wrong-owner fixture preserves owner');
my @reencoded_responses = map {
    encode_response(
        status => $_->{status},
        request_id => $_->{request_id},
        value => $_->{value},
        owner_worker => $_->{owner_worker},
        worker_count => $_->{worker_count},
        routing_epoch => $_->{routing_epoch},
    )
} @responses;
is_deeply(\@reencoded_responses, \@expected_responses, 'response codec round-trips fixtures');

is(worker_for('', 4), 1, 'empty-key FNV routing matches canonical implementation');
is(worker_for("key\x00\xff", 17), worker_for("key\x00\xff", 17), 'binary routing is stable');

use GlifiStore::Protocol qw(
    ROUTING_ALG_SIPHASH24_V1 siphash24 hash_key_routing encode_init_identity decode_init_identity
);
# Paper vectors from Aumasson & Bernstein, SipHash: a fast short-input PRF.
is(siphash24('', 506_097_522_914_230_528, 1_084_818_905_618_843_912), 8_246_050_544_436_514_353,
    'siphash24 empty paper vector (Aumasson/Bernstein)');
is(siphash24("\x00\x01\x02", 506_097_522_914_230_528, 1_084_818_905_618_843_912), 9_612_764_727_700_323_885,
    'siphash24 3-byte paper vector (Aumasson/Bernstein)');
my $keyed = { algorithm => ROUTING_ALG_SIPHASH24_V1, seed => 1_229_801_703_532_086_340 };
is(hash_key_routing('tenant-a/orders/1', $keyed), 8_155_964_797_755_082_054, 'keyed digest fixture');
is(worker_for('tenant-a/orders/1', 8, $keyed), 6, 'keyed worker_for fixture');
isnt(hash_key_routing('tenant-a/orders/1', $keyed), hash_key_routing('tenant-a/orders/1'),
    'siphash differs from FNV');
is(encode_init_identity(), 'GlifiStore/2', 'plain INIT identity');
my $extended = encode_init_identity({ algorithm => ROUTING_ALG_SIPHASH24_V1, seed => 12_379_813_738_877_118_345 });
is(length($extended), INIT_IDENTITY_EXTENDED_BYTES, 'extended INIT length');
is_deeply(decode_init_identity($extended),
    { algorithm => ROUTING_ALG_SIPHASH24_V1, seed => 12_379_813_738_877_118_345 },
    'extended INIT round-trip');
ok(!eval { decode_init_identity("GlifiStore/2\x00bad"); 1 }, 'malformed extended INIT rejected');
ok(!eval { decode_init_identity('GlifiStore/3'); 1 }, 'wrong INIT prefix rejected');

my $noncanonical = $expected_requests[0];
substr($noncanonical, 36, 1, "\x01");
ok(!eval { decode_request($noncanonical); 1 }, 'decoder rejects noncanonical reserved fields');

ok(!eval { encode_request(opcode => OP_PUT, request_id => -1, key => 'k', value => 'v'); 1 },
    'request_id rejects negatives');
ok(!eval { encode_request(opcode => OP_PUT, request_id => 1, key => 'k', value => 'v',
    expire_at_ns => '18446744073709551616'); 1 },
    'expire_at_ns rejects values above u64 max');

is($GlifiStore::Protocol::VERSION, '0.1.0', 'module VERSION is not shadowed by wire constant');
is(GlifiStore::Protocol::PROTOCOL_VERSION(), 2, 'wire protocol version constant');

ok(!eval {
    encode_request(opcode => OP_PUT, request_id => 1, key => "\x{100}", value => 'v'); 1
}, 'wide-character keys are rejected');

my $big = encode_request(
    opcode => OP_PUT,
    request_id => '9007199254740993',
    key => 'k',
    value => 'v',
    expire_at_ns => '18446744073709551615',
);
my $decoded_big = decode_request($big);
is($decoded_big->{request_id}, 9_007_199_254_740_993, 'u64 request_id above 2^53 round-trips');
is($decoded_big->{expire_at_ns}, '18446744073709551615' + 0, 'u64 max expire_at_ns round-trips');

done_testing;
