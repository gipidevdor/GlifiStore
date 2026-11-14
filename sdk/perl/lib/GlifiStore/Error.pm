package GlifiStore::Error;

use v5.32;
use strict;
use warnings;

our $VERSION = '0.1.0';

use overload '""' => sub { $_[0]->{message} }, fallback => 1;

sub new {
    my ($class, $category, $message, $fields) = @_;
    $fields //= {};
    my $self = bless {
        category         => $category,
        message          => $message,
        wire_status      => $fields->{wire_status},
        mutation_outcome => $fields->{mutation_outcome},
        bytes_sent       => $fields->{bytes_sent} // 0,
        request_id       => $fields->{request_id},
        worker           => $fields->{worker},
        routing_epoch    => $fields->{routing_epoch},
        operation        => $fields->{operation},
        retryability     => $fields->{retryability},
    }, $class;
    $self->{retryability} //= _retryability_for(
        $self->{category},
        ($self->{bytes_sent} // 0) > 0 && defined $self->{mutation_outcome},
        defined $self->{mutation_outcome} && $self->{mutation_outcome} eq 'indeterminate',
    );
    return $self;
}

sub _retryability_for {
    my ($category, $mutation_sent, $indeterminate) = @_;
    return 'reconcile_first' if $indeterminate;
    return 'never' if $category eq 'invalid_argument' && !$mutation_sent;
    return 'same_request' if $category eq 'transport' && !$mutation_sent;
    return 'never' if $category eq 'overloaded';
    return 'never' if $category eq 'permission_denied';
    return 'new_attempt' if $category eq 'not_found';
    return 'never' if $category eq 'unavailable';
    return 'reconcile_first' if $mutation_sent;
    return 'new_attempt';
}

sub category         { return $_[0]->{category} }
sub message          { return $_[0]->{message} }
sub wire_status      { return $_[0]->{wire_status} }
sub mutation_outcome { return $_[0]->{mutation_outcome} }
sub bytes_sent       { return $_[0]->{bytes_sent} // 0 }
sub request_id       { return $_[0]->{request_id} }
sub worker           { return $_[0]->{worker} }
sub routing_epoch    { return $_[0]->{routing_epoch} }
sub retryability     { return $_[0]->{retryability} }
sub operation        { return $_[0]->{operation} }

sub enrich {
    my ($self, %fields) = @_;
    for my $key (
        qw(wire_status mutation_outcome bytes_sent request_id worker routing_epoch operation))
    {
        $self->{$key} = $fields{$key} if exists $fields{$key};
    }
    my $mutation_sent =($self->{bytes_sent} // 0) > 0
        && defined $self->{mutation_outcome};
    my $indeterminate
        =defined $self->{mutation_outcome} && $self->{mutation_outcome} eq 'indeterminate';
    # Indeterminate always means reconcile_first — including transport failures after a
    # full mutation send where bytes_sent was omitted (must not fall through to same_request).
    if ($indeterminate) {
        $self->{retryability} = 'reconcile_first';
    }
    elsif (defined $self->{mutation_outcome}
        && ($self->{bytes_sent} // 0) > 0
        && $self->{category} eq 'transport')
    {
        $self->{retryability} = 'reconcile_first';
    }
    elsif (($self->{bytes_sent} // 0) == 0 && $self->{category} eq 'transport') {
        $self->{retryability} = 'same_request';
    }
    else {
        $self->{retryability} =_retryability_for($self->{category}, $mutation_sent, $indeterminate);
    }
    return $self;
}

1;

__END__

=encoding utf8

=head1 NAME

GlifiStore::Error - structured client exception

=head1 VERSION

Version 0.1.0

=head1 SYNOPSIS

    eval { GlifiStore::Client->connect(host => '127.0.0.1', port => 7379) };
    if (my $error = $@) {
        warn $error->category, ': ', $error->message if ref($error);
        warn $error->retryability if $error->can('retryability');
    }

=head1 DESCRIPTION

Exception object thrown by L<GlifiStore::Client> for transport, protocol, and
routing failures. Stringifies to the human-readable C<message>.

Fields follow client-semantics v1 section 2.1: C<category>, C<message>, and when
known C<wire_status>, C<mutation_outcome>, C<bytes_sent>, C<request_id>,
C<worker>, C<routing_epoch>, C<retryability>, and C<operation>.

=head1 SUBROUTINES/METHODS

=over 4

=item new($category, $message, \%fields?)

Construct an error. Optional C<%fields> may include structured section 2.1 keys.

=item category / message / wire_status / mutation_outcome / bytes_sent / request_id / worker / routing_epoch / retryability / operation

Accessors. Unset optional fields return C<undef> (C<bytes_sent> defaults to 0).

=item enrich(%fields)

Mutate and return C<$self> after applying known structured fields and refreshing
C<retryability>.

=back

=head1 DIAGNOSTICS

Thrown via C<die> / C<eval> from L<GlifiStore::Client> read and connect paths.
Mutations and pipeline entries may embed an instance under C<error> instead of
throwing.

=head1 CONFIGURATION AND ENVIRONMENT

None.

=head1 DEPENDENCIES

None beyond core Perl.

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
