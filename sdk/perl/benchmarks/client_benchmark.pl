#!/usr/bin/env perl
use v5.32;
use strict;
use warnings;
use FindBin;
use Getopt::Long qw(GetOptions);
use Time::HiRes qw(time);

use lib "$FindBin::Bin/../lib";
use GlifiStore;
use GlifiStore::Client;

my %options = (
    host       => '127.0.0.1',
    workers    => 1,
    ops        => 100_000,
    pipeline   => 128,
    warmup     => 1,
    repeats    => 7,
    concurrent => undef,
    batch      => 0,
);
GetOptions(
    'host=s'      => \$options{host},
    'port=i'      => \$options{port},
    'workers=i'   => \$options{workers},
    'ops=i'       => \$options{ops},
    'pipeline=i'  => \$options{pipeline},
    'warmup=i'    => \$options{warmup},
    'repeats=i'   => \$options{repeats},
    'concurrent!' => \$options{concurrent},
    'batch!'      => \$options{batch},
) or die "invalid benchmark arguments\n";
die "--port is required\n" if !$options{port};
die "numeric arguments are outside benchmark limits\n"
    if $options{workers} < 1 || $options{ops} < 1 || $options{pipeline} < 1
    || $options{warmup} < 0 || $options{repeats} < 1;

# Default: overlap Workers when workers>1 unless the caller forces --no-concurrent.
my $use_concurrent = defined($options{concurrent})
    ? ($options{concurrent} ? 1 : 0)
    : ($options{workers} > 1 ? 1 : 0);

my $client = GlifiStore::Client->connect(
    host                      => $options{host},
    port                      => $options{port},
    maximum_pipeline_requests => $options{pipeline} * 2,
);
die "server Worker count does not match --workers\n"
    if $client->worker_count != $options{workers};

my @requests = map { [] } 1 .. $options{workers};
my @remaining = map {
    int($options{ops} / $options{workers})
        + ($_ < $options{ops} % $options{workers} ? 1 : 0)
} 0 .. $options{workers} - 1;
my $candidate = 0;
while (grep { $_ } @remaining) {
    my $key = sprintf('perl-bench-%012d', $candidate);
    my $owner = $client->worker_for($key);
    if ($remaining[$owner]) {
        my $value = chr($candidate & 0xFF) x 64;
        push @{$requests[$owner]}, { opcode => 'put', key => $key, value => $value };
        push @{$requests[$owner]}, { opcode => 'get', key => $key };
        --$remaining[$owner];
    }
    ++$candidate;
}
my $batch_frames = $options{pipeline} * 2;
my @batches;
my $max_rounds = 0;
for my $worker_requests (@requests) {
    my @worker_batches;
    for (my $offset = 0; $offset < @$worker_requests; $offset += $batch_frames) {
        my $last = $offset + $batch_frames - 1;
        $last = $#$worker_requests if $last > $#$worker_requests;
        push @worker_batches, [@$worker_requests[$offset .. $last]];
    }
    $max_rounds = @worker_batches if @worker_batches > $max_rounds;
    push @batches, \@worker_batches;
}

sub validate_batch {
    my ($batch, $responses) = @_;
    die "pipeline response count mismatch\n" if @$responses != @$batch;
    for my $index (0 .. $#$batch) {
        die "pipeline request failed\n"
            if $responses->[$index]->{outcome} ne 'succeeded';
        if ($batch->[$index]->{opcode} eq 'get') {
            die "pipeline GET value mismatch\n"
                if $responses->[$index]->{value} ne $batch->[$index - 1]->{value};
        }
    }
}

sub run_once_sequential {
    my $started = time;
    for my $worker_batches (@batches) {
        for my $batch (@$worker_batches) {
            validate_batch($batch, $client->execute_pipeline($batch));
        }
    }
    return time - $started;
}

sub run_once_concurrent {
    my $started = time;
    for my $round (0 .. $max_rounds - 1) {
        my @wave = map {
            my $worker_batches = $batches[$_];
            ($round < @$worker_batches) ? $worker_batches->[$round] : [];
        } 0 .. $options{workers} - 1;
        my $all = $client->execute_worker_pipelines(\@wave);
        for my $worker (0 .. $options{workers} - 1) {
            next if !@{$wave[$worker]};
            validate_batch($wave[$worker], $all->[$worker]);
        }
    }
    return time - $started;
}

sub run_once_batch {
    my $started = time;
    for my $round (0 .. $max_rounds - 1) {
        my @wave = map {
            my $worker_batches = $batches[$_];
            ($round < @$worker_batches) ? $worker_batches->[$round] : [];
        } 0 .. $options{workers} - 1;
        my @mixed;
        my $largest = 0;
        for my $batch (@wave) {
            $largest = @$batch if @$batch > $largest;
        }
        for (my $offset = 0; $offset < $largest; $offset += 2) {
            for my $batch (@wave) {
                next if $offset >= @$batch;
                push @mixed, @$batch[$offset, $offset + 1];
            }
        }
        validate_batch(\@mixed, $client->execute_batch(\@mixed));
    }
    return time - $started;
}

my $run_once
    = $options{batch} ? \&run_once_batch
    : $use_concurrent ? \&run_once_concurrent
    :                   \&run_once_sequential;
my $execution
    = $options{batch} ? 'single-process-batch'
    : $use_concurrent ? 'single-process-worker-concurrent'
    :                   'single-process-worker-sequential';

sub median {
    my @sorted = sort { $a <=> $b } @_;
    return $sorted[int(@sorted / 2)] if @sorted % 2;
    return ($sorted[@sorted / 2 - 1] + $sorted[@sorted / 2]) / 2;
}

$run_once->() for 1 .. $options{warmup};
my @samples = map { $run_once->() } 1 .. $options{repeats};
$client->close;

my $sdk_version = $GlifiStore::VERSION;
my $operation_count = $options{ops} * 2;
my @rates = map { $operation_count / $_ } @samples;
printf "# glifistore Perl client benchmark\n";
printf "# sdk_version=%s runtime=sync execution=%s "
    . "workers=%d pipeline_pairs=%d operations=%d\n",
    $sdk_version, $execution, $options{workers}, $options{pipeline}, $operation_count;
printf "name=perl_client_pipeline_read_after_write sdk_version=%s runtime=sync "
    . "execution=%s workers=%d pipeline_pairs=%d "
    . "operations=%d samples=%d median_seconds=%.9f min_seconds=%.9f max_seconds=%.9f "
    . "median_ops_per_second=%.3f min_ops_per_second=%.3f max_ops_per_second=%.3f\n",
    $sdk_version, $execution, $options{workers}, $options{pipeline}, $operation_count,
    scalar(@samples), median(@samples), (sort { $a <=> $b } @samples)[0],
    (sort { $b <=> $a } @samples)[0], median(@rates),
    (sort { $a <=> $b } @rates)[0], (sort { $b <=> $a } @rates)[0];
