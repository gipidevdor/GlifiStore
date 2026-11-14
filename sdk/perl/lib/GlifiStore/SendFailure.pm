package GlifiStore::SendFailure;

use v5.32;
use strict;
use warnings;

our $VERSION = '0.1.0';

use GlifiStore::Error;

use overload '""' => sub { "$_[0]->{error}" }, fallback => 1;

sub new {
    my ($class, $error, $bytes_sent) = @_;
    return bless { error => $error, bytes_sent => $bytes_sent }, $class;
}

1;

__END__

=encoding utf8

=head1 NAME

GlifiStore::SendFailure - partial send failure marker

=head1 VERSION

Version 0.1.0

=head1 SYNOPSIS

Internal to L<GlifiStore::Client>; not part of the public API.

=head1 DESCRIPTION

Raised when a request frame could not be fully written. Carries the underlying
L<GlifiStore::Error> and how many bytes were accepted before failure.

=head1 SUBROUTINES/METHODS

=over 4

=item new($error, $bytes_sent)

Construct a send-failure object. C<$error> is a L<GlifiStore::Error>; C<$bytes_sent>
is the number of octets accepted before the failure.

=back

=head1 DIAGNOSTICS

Thrown via C<die> from L<GlifiStore::Client> send paths; does not set C<$!>.

=head1 CONFIGURATION AND ENVIRONMENT

None.

=head1 DEPENDENCIES

L<GlifiStore::Error>.

=head1 INCOMPATIBILITIES

None known.

=head1 BUGS AND LIMITATIONS

None known.

=head1 AUTHOR

Giacomo Picchiarelli

=head1 LICENSE AND COPYRIGHT

Copyright (c) 2026, Giacomo Picchiarelli.

BSD 3-Clause. See the distribution F<LICENSE>.

=cut
