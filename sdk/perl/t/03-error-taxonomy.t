#!/usr/bin/env perl
use v5.32;
use strict;
use warnings;
use FindBin;
use JSON::PP qw(decode_json);
use Test::More;

use GlifiStore::Client;
use GlifiStore::Error;
use GlifiStore::Protocol qw(
    STATUS_INVALID_REQUEST STATUS_UNSUPPORTED STATUS_INTERNAL_ERROR STATUS_NOT_FOUND
    STATUS_OVERLOADED STATUS_WRONG_OWNER STATUS_NOT_BOUND STATUS_PERMISSION_DENIED
);

sub fixture_path {
    my ($name) = @_;
    my $local = "$FindBin::Bin/fixtures/$name";
    return $local if -f $local;
    return "$FindBin::Bin/../../../tests/fixtures/$name";
}

my $path = fixture_path('error_taxonomy_v1.json');
ok(-f $path, 'error taxonomy fixture present');
open my $fh, '<:raw', $path or die "open $path: $!";
my $raw = do { local $/; <$fh> };
close $fh;
my $fixture = decode_json($raw);

for my $case (@{ $fixture->{cases} }) {
    subtest $case->{id} => sub {
        my $error = GlifiStore::Client::_status_error($case->{wire_status});
        is($error->category, $case->{category}, 'category');
        is($error->wire_status, $case->{wire_status}, 'wire_status');
        is($error->retryability, $case->{read_retryability}, 'read retryability');

        my $enriched = GlifiStore::Client::_status_error($case->{wire_status})->enrich(
            bytes_sent       => 1,
            mutation_outcome => $case->{mutation_outcome},
        );
        is($enriched->retryability, $case->{mutation_retryability}, 'mutation retryability');

        my $want_unhealthy =
             $case->{wire_status} == STATUS_WRONG_OWNER
          || $case->{wire_status} == STATUS_NOT_BOUND;
        ok(($case->{unhealthy} ? 1 : 0) == ($want_unhealthy ? 1 : 0), 'unhealthy flag');
    };
}

subtest 'indeterminate enrich ignores zero bytes_sent' => sub {
    # Receive-after-send paths historically omitted bytes_sent; transport+0 must not
    # advertise same_request when mutation_outcome is already indeterminate.
    my $error = GlifiStore::Error->new('transport', 'socket closed')->enrich(
        mutation_outcome => 'indeterminate',
    );
    is($error->bytes_sent, 0, 'bytes_sent stays zero when omitted');
    is($error->retryability, 'reconcile_first', 'indeterminate beats transport+0 same_request');
    is($error->mutation_outcome, 'indeterminate', 'mutation_outcome preserved');
};

done_testing();
