use strict;
use warnings;
use ExtUtils::MakeMaker 7.12;
use Config;

die "GlifiStore requires a Perl with 64-bit integers\n"
    unless defined($Config{use64bitint}) && $Config{use64bitint} eq 'define'
        && ($Config{uvsize} // 0) >= 8;

my $version = '0.1.0';

WriteMakefile(
    NAME             => 'GlifiStore',
    VERSION_FROM     => 'lib/GlifiStore.pm',
    ABSTRACT_FROM    => 'lib/GlifiStore.pm',
    AUTHOR           => 'Giacomo Picchiarelli',
    # MetaCPAN / CPAN::Meta recognize 'bsd' as the BSD family; the authoritative
    # text in LICENSE is BSD-3-Clause (same grant as the repository root).
    LICENSE          => 'bsd',
    MIN_PERL_VERSION => '5.032',
    CONFIGURE_REQUIRES => {
        'ExtUtils::MakeMaker' => '7.12',
        'Config'              => '0',
    },
    TEST_REQUIRES => {
        'Test::More'       => '0.98',
        'FindBin'          => '0',
        'IO::Select'       => '0',
        'IO::Socket::INET' => '0',
        'POSIX'            => '0',
    },
    PREREQ_PM => {
        'Config'           => '0',
        'Errno'            => '0',
        'Exporter'         => '0',
        'IO::Select'       => '0',
        'IO::Socket::INET' => '0',
        'Socket'           => '0',
        'Time::HiRes'      => '0',
    },
    META_MERGE => {
        'meta-spec' => { version => 2 },
        release_status => 'testing',
        dynamic_config => 0,
        no_index => {
            directory => [ 'benchmarks', 't' ],
        },
        resources => {
            license    => ['https://opensource.org/licenses/BSD-3-Clause'],
            bugtracker => {
                web => 'https://github.com/gpicchiarelli/GlifiStore/issues',
            },
            repository => {
                type => 'git',
                url  => 'https://github.com/gpicchiarelli/GlifiStore.git',
                web  => 'https://github.com/gpicchiarelli/GlifiStore',
            },
            homepage => 'https://github.com/gpicchiarelli/GlifiStore',
        },
        x_copyright => 'Copyright (c) 2026 Giacomo Picchiarelli',
        prereqs => {
            runtime => {
                requires => {
                    'perl'             => '5.032',
                    'Errno'            => '0',
                    'Exporter'         => '0',
                    'IO::Select'       => '0',
                    'IO::Socket::INET' => '0',
                    'Socket'           => '0',
                    'Time::HiRes'      => '0',
                },
            },
            test => {
                requires => {
                    'Test::More'       => '0.98',
                    'FindBin'          => '0',
                    'IO::Select'       => '0',
                    'IO::Socket::INET' => '0',
                    'POSIX'            => '0',
                },
            },
        },
        provides => {
            'GlifiStore' => {
                file    => 'lib/GlifiStore.pm',
                version => $version,
            },
            'GlifiStore::Client' => {
                file    => 'lib/GlifiStore/Client.pm',
                version => $version,
            },
            'GlifiStore::Protocol' => {
                file    => 'lib/GlifiStore/Protocol.pm',
                version => $version,
            },
            'GlifiStore::Error' => {
                file    => 'lib/GlifiStore/Error.pm',
                version => $version,
            },
            'GlifiStore::SendFailure' => {
                file    => 'lib/GlifiStore/SendFailure.pm',
                version => $version,
            },
        },
        x_static_install => 1,
    },
    dist  => { COMPRESS => 'gzip -9f', SUFFIX => 'gz' },
    clean => { FILES => 'GlifiStore-*' },
);
