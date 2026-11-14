package client

import (
	"github.com/gpicchiarelli/GlifiStore/sdk/go/protocol"
)

// Category is the portable client-semantics v1 error category.
type Category string

const (
	CategoryInvalidArgument   Category = "invalid_argument"
	CategoryNotFound          Category = "not_found"
	CategoryOverloaded        Category = "overloaded"
	CategoryUnavailable       Category = "unavailable"
	CategoryTransport         Category = "transport"
	CategoryProtocol          Category = "protocol"
	CategoryInternal          Category = "internal"
	CategoryPermissionDenied  Category = "permission_denied"
)

// Retryability classifies whether an automatic or application retry is safe.
type Retryability string

const (
	RetryNever          Retryability = "never"
	RetrySameRequest    Retryability = "same_request"
	RetryNewAttempt     Retryability = "new_attempt"
	RetryReconcileFirst Retryability = "reconcile_first"
)

// Error is a structured GlifiStore client failure (§2.1).
type Error struct {
	Category        Category
	Message         string
	WireStatus      *protocol.Status
	BytesSent       int
	RequestID       uint64
	Worker          uint32
	RoutingEpoch    uint64
	Retryability    Retryability
	Operation       string
	MutationOutcome MutationOutcome
	hasMutation     bool
}

func (e *Error) Error() string {
	if e == nil {
		return ""
	}
	return e.Message
}

func (e *Error) Is(target error) bool {
	other, ok := target.(*Error)
	if !ok || e == nil || other == nil {
		return false
	}
	return e.Category == other.Category
}

func (e *Error) clone() *Error {
	if e == nil {
		return nil
	}
	out := *e
	if e.WireStatus != nil {
		status := *e.WireStatus
		out.WireStatus = &status
	}
	return &out
}

func (e *Error) withOp(op string) *Error {
	out := e.clone()
	out.Operation = op
	return out
}

func (e *Error) withRequest(requestID uint64, worker uint32, epoch uint64) *Error {
	out := e.clone()
	out.RequestID = requestID
	out.Worker = worker
	out.RoutingEpoch = epoch
	return out
}

func (e *Error) withBytesSent(n int) *Error {
	out := e.clone()
	out.BytesSent = n
	indeterminate := out.hasMutation && out.MutationOutcome == MutationIndeterminate
	// Indeterminate always means reconcile_first — including transport failures after a
	// full mutation send where bytes_sent was omitted (must not fall through to same_request).
	if indeterminate {
		out.Retryability = RetryReconcileFirst
	} else if out.hasMutation && n > 0 && out.Category == CategoryTransport {
		out.Retryability = RetryReconcileFirst
	} else if n == 0 && out.Category == CategoryTransport {
		out.Retryability = RetrySameRequest
	} else {
		out.Retryability = retryabilityFor(out.Category, n > 0 && out.hasMutation, indeterminate)
	}
	return out
}

func (e *Error) withMutation(outcome MutationOutcome) *Error {
	out := e.clone()
	out.MutationOutcome = outcome
	out.hasMutation = true
	indeterminate := outcome == MutationIndeterminate
	out.Retryability = retryabilityFor(out.Category, out.BytesSent > 0, indeterminate)
	return out
}

func (e *Error) withSession(worker uint32, epoch uint64) *Error {
	out := e.clone()
	out.Worker = worker
	out.RoutingEpoch = epoch
	return out
}

func promoteSendFailure(sf *sendFailure, op string, requestID uint64, worker uint32, epoch uint64, mutation bool) *Error {
	out := sf.err.clone()
	out.BytesSent = sf.bytesSent
	out.Operation = op
	out.RequestID = requestID
	out.Worker = worker
	out.RoutingEpoch = epoch
	out.hasMutation = mutation
	if mutation {
		if sf.bytesSent == 0 {
			out.MutationOutcome = MutationRejected
			out.Retryability = RetrySameRequest
		} else {
			out.MutationOutcome = MutationIndeterminate
			out.Retryability = RetryReconcileFirst
		}
	} else if sf.bytesSent == 0 {
		out.Retryability = RetrySameRequest
	} else {
		out.Retryability = RetryReconcileFirst
	}
	return out
}

func newError(category Category, message string) *Error {
	return &Error{
		Category:     category,
		Message:      message,
		Retryability: retryabilityFor(category, false, false),
	}
}

func retryabilityFor(category Category, mutationSent bool, indeterminate bool) Retryability {
	switch {
	case indeterminate:
		return RetryReconcileFirst
	case category == CategoryInvalidArgument && !mutationSent:
		return RetryNever
	case category == CategoryTransport && !mutationSent:
		return RetrySameRequest
	case category == CategoryOverloaded:
		// Wire OVERLOADED collapses admission and capacity exhaustion; fail closed.
		return RetryNever
	case category == CategoryPermissionDenied:
		return RetryNever
	case category == CategoryNotFound:
		return RetryNewAttempt
	case category == CategoryUnavailable:
		return RetryNever
	default:
		if mutationSent {
			return RetryReconcileFirst
		}
		return RetryNewAttempt
	}
}

func invalidArgument(message string) *Error {
	return newError(CategoryInvalidArgument, message)
}

func unavailable(message string) *Error {
	err := newError(CategoryUnavailable, message)
	err.Retryability = RetryNever
	return err
}

func transport(message string) *Error {
	err := newError(CategoryTransport, message)
	err.Retryability = RetrySameRequest
	return err
}

func protocolErr(message string) *Error {
	return newError(CategoryProtocol, message)
}

func notFound(message string) *Error {
	err := newError(CategoryNotFound, message)
	err.Retryability = RetryNewAttempt
	return err
}

func overloaded(message string) *Error {
	err := newError(CategoryOverloaded, message)
	err.Retryability = RetryNever
	return err
}

func permissionDenied(message string) *Error {
	err := newError(CategoryPermissionDenied, message)
	err.Retryability = RetryNever
	return err
}

func internalErr(message string) *Error {
	return newError(CategoryInternal, message)
}

func statusError(status protocol.Status) *Error {
	var err *Error
	switch status {
	case protocol.StatusNotFound:
		err = notFound("key was not found")
	case protocol.StatusOverloaded:
		err = overloaded("server is overloaded")
	case protocol.StatusNotBound:
		err = unavailable("server connection is not bound")
	case protocol.StatusPermissionDenied:
		err = permissionDenied("server denied the request")
	case protocol.StatusWrongOwner:
		err = protocolErr("server rejected Worker routing")
	case protocol.StatusInvalidRequest, protocol.StatusUnsupported:
		err = invalidArgument("server rejected the request")
	case protocol.StatusInternalError, protocol.StatusOK:
		err = internalErr("server reported an internal error")
	default:
		err = protocolErr("server returned an unknown status")
	}
	s := status
	err.WireStatus = &s
	return err
}

func annotate(err *Error, op string, requestID uint64, worker uint32, epoch uint64) *Error {
	if err == nil {
		return nil
	}
	return err.withOp(op).withRequest(requestID, worker, epoch)
}
