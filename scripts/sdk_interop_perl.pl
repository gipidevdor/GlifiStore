#!/usr/bin/env perl
use v5.32;
use strict;
use warnings;
use FindBin;
use Getopt::Long qw(GetOptions);
use Cwd qw(abs_path);
BEGIN {
    unshift @INC, "$FindBin::Bin/../sdk/perl/lib"
      if ($ENV{GLIFISTORE_INTEROP_USE_INSTALLED} // '0') ne '1';
}
use GlifiStore::Client;

if (($ENV{GLIFISTORE_INTEROP_USE_INSTALLED} // '0') eq '1') {
    my $source_root = abs_path($ENV{GLIFISTORE_SOURCE_ROOT} // '');
    my $loaded_from = abs_path($INC{'GlifiStore/Client.pm'} // '');
    die "installed-artifact mode loaded Perl SDK from source: $loaded_from\n"
      if !$source_root || !$loaded_from || index($loaded_from, "$source_root/") == 0;
}

sub parse_hex {
    my ($text) = @_;
    $text //= '';
    $text =~ s/\s+//g;
    return '' if $text eq '';
    die "odd hex length\n" if length($text) % 2;
    return pack('H*', $text);
}

sub to_hex {
    return unpack('H*', $_[0] // '');
}

my %options = (
    host                 => '127.0.0.1',
    expire_at_ns         => 0,
    tls                  => 0,
    insecure_skip_verify => 0,
    burst                => 32,
);
GetOptions(
    'host=s'                 => \$options{host},
    'port=i'                 => \$options{port},
    'key-hex=s'              => \$options{key_hex},
    'value-hex=s'            => \$options{value_hex},
    'expire-at-ns=i'         => \$options{expire_at_ns},
    'tls!'                   => \$options{tls},
    'tls-ca=s'               => \$options{tls_ca},
    'tls-cert=s'             => \$options{tls_cert},
    'tls-key=s'              => \$options{tls_key},
    'server-name=s'          => \$options{server_name},
    'insecure-skip-verify!'  => \$options{insecure_skip_verify},
    'burst=i'                => \$options{burst},
) or die "invalid arguments\n";
my $command = shift @ARGV // '';
die "--port and command are required\n"
  if !$options{port}
  || $command !~ /\A(?:put|get|erase|pipeline-put-get|worker-pipelines-put-get|expect-not-found|expect-permission-denied|burst-expect-overloaded|expect-frame-limit)\z/
  || $options{burst} < 1
  || $options{burst} > 10_000;

my $key   = parse_hex($options{key_hex});
my $value = parse_hex($options{value_hex});
my %connect = (
    host                 => $options{host},
    port                 => $options{port},
    tls                  => $options{tls} ? 1 : 0,
    insecure_skip_verify => $options{insecure_skip_verify} ? 1 : 0,
);
$connect{tls_ca}       = $options{tls_ca}       if defined $options{tls_ca} && length $options{tls_ca};
$connect{cert_file}    = $options{tls_cert}     if defined $options{tls_cert} && length $options{tls_cert};
$connect{key_file}     = $options{tls_key}      if defined $options{tls_key} && length $options{tls_key};
$connect{server_name}  = $options{server_name}  if defined $options{server_name} && length $options{server_name};

my $client = GlifiStore::Client->connect(%connect);

if ($command eq 'put') {
    my $result = $client->put($key, $value, expire_at_ns => $options{expire_at_ns});
    die "put not committed\n" if $result->{outcome} ne 'committed';
}
elsif ($command eq 'get') {
    print to_hex($client->get($key)), "\n";
}
elsif ($command eq 'erase') {
    my $result = $client->erase($key);
    die "erase not committed\n" if $result->{outcome} ne 'committed';
}
elsif ($command eq 'expect-not-found') {
    my $found = eval {
        $client->get($key);
        1;
    };
    die "GET unexpectedly found the key\n" if $found;
    my $error = $@;
    die "GET did not produce structured not_found\n"
      if ref($error) ne 'GlifiStore::Error'
      || $error->category ne 'not_found'
      || $error->retryability ne 'new_attempt';
}
elsif ($command eq 'expect-permission-denied') {
    my $result = $client->put($key, $value, expire_at_ns => $options{expire_at_ns});
    die "PUT did not produce structured permission_denied\n"
      if $result->{outcome} ne 'rejected'
      || ref($result->{error}) ne 'GlifiStore::Error'
      || $result->{error}->category ne 'permission_denied'
      || $result->{error}->retryability ne 'never';
}
elsif ($command eq 'burst-expect-overloaded') {
    for (1 .. $options{burst}) {
        my $result = $client->put($key, $value, expire_at_ns => $options{expire_at_ns});
        if ($result->{outcome} eq 'rejected'
            && ref($result->{error}) eq 'GlifiStore::Error'
            && $result->{error}->category eq 'overloaded'
            && $result->{error}->retryability eq 'never')
        {
            $client->close;
            exit 0;
        }
        die "PUT produced an unexpected result before OVERLOADED\n"
          if $result->{outcome} ne 'committed';
    }
    die "PUT burst did not produce structured OVERLOADED\n";
}
elsif ($command eq 'expect-frame-limit') {
    my $result = $client->put('limit', "\xA5" x $client->{maximum_frame_bytes});
    die "oversized PUT did not produce the expected local rejection\n"
      if $result->{outcome} ne 'rejected'
      || ref($result->{error}) ne 'GlifiStore::Error'
      || $result->{error}->category ne 'invalid_argument'
      || $result->{error}->bytes_sent != 0
      || $result->{error}->retryability ne 'never';
}
elsif ($command eq 'worker-pipelines-put-get') {
    my $worker_count = $client->worker_count;
    my @keys = (undef) x $worker_count;
    my $remaining = $worker_count;
    my $candidate = 0;
    while ($remaining > 0 && $candidate < 1_000_000) {
        my $probe = $key . "-$candidate";
        my $owner = $client->worker_for($probe);
        if (!defined $keys[$owner]) {
            $keys[$owner] = $probe;
            $remaining--;
        }
        $candidate++;
    }
    die "failed to discover a key for every Worker\n" if $remaining != 0;
    my @wave;
    my @values;
    for my $worker (0 .. $worker_count - 1) {
        my $payload = length($value) ? $value : ("v" . ($worker % 10));
        $values[$worker] = $payload;
        $wave[$worker] = [
            { opcode => 'put', key => $keys[$worker], value => $payload },
            { opcode => 'get', key => $keys[$worker] },
        ];
    }
    my $all = $client->execute_worker_pipelines(\@wave);
    die "worker pipelines returned unexpected slot count\n" if @$all != $worker_count;
    for my $worker (0 .. $worker_count - 1) {
        my $slot = $all->[$worker];
        die "worker $worker pipeline outcomes failed\n"
          if @$slot != 2
          || $slot->[0]{outcome} ne 'succeeded'
          || $slot->[1]{outcome} ne 'succeeded'
          || $slot->[1]{value} ne $values[$worker];
    }
    print "ok\n";
}
elsif ($command eq 'pipeline-put-get') {
    my $responses = $client->execute_pipeline(
        [
            { opcode => 'put', key => $key, value => $value },
            { opcode => 'get', key => $key },
        ]
    );
    die "pipeline outcomes failed\n"
      if @$responses != 2
      || $responses->[0]{outcome} ne 'succeeded'
      || $responses->[1]{outcome} ne 'succeeded';
    die "pipeline value mismatch\n" if $responses->[1]{value} ne $value;
    print to_hex($responses->[1]{value}), "\n";
}
else {
    die "unknown command: $command\n";
}

$client->close;
