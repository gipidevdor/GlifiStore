package client

import (
	"time"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/protocol"
)

// MutationOutcome classifies standalone PUT/ERASE results.
type MutationOutcome string

const (
	MutationCommitted     MutationOutcome = "committed"
	MutationRejected      MutationOutcome = "rejected"
	MutationIndeterminate MutationOutcome = "indeterminate"
)

// MutationResult is returned by Put and Erase (never panics on outcome).
type MutationResult struct {
	Outcome MutationOutcome
	Err     error
}

// Committed reports whether the mutation is known applied.
func (r MutationResult) Committed() bool {
	return r.Outcome == MutationCommitted
}

// PipelineOpcode is a Store operation allowed inside a pipeline/batch.
type PipelineOpcode uint8

const (
	PipelineGet   PipelineOpcode = PipelineOpcode(protocol.OpcodeGet)
	PipelinePut   PipelineOpcode = PipelineOpcode(protocol.OpcodePut)
	PipelineErase PipelineOpcode = PipelineOpcode(protocol.OpcodeErase)
)

// PipelineRequest is one ordered pipeline/batch entry.
type PipelineRequest struct {
	Opcode     PipelineOpcode
	Key        []byte
	Value      []byte
	ExpireAtNs uint64
}

// PipelineOutcome classifies a positional pipeline/batch response.
type PipelineOutcome string

const (
	PipelineSucceeded     PipelineOutcome = "succeeded"
	PipelineFailed        PipelineOutcome = "failed"
	PipelineIndeterminate PipelineOutcome = "indeterminate"
)

// PipelineResponse is one positional pipeline/batch result.
type PipelineResponse struct {
	Outcome PipelineOutcome
	Value   []byte
	Err     error
}

// Succeeded reports whether the position completed with a valid OK.
func (r PipelineResponse) Succeeded() bool {
	return r.Outcome == PipelineSucceeded
}

// Config configures a Client.
type Config struct {
	Host                    string
	Port                    int
	// UnixSocketPath, when set, dials AF_UNIX instead of TCP Host:Port (ADR 0029).
	// Mutually exclusive with TLS.
	UnixSocketPath          string
	ConnectTimeout          time.Duration
	RequestTimeout          time.Duration
	MaximumFrameBytes       int
	MaximumPipelineRequests int
	MaximumPipelineBytes    int
	// TLS is opt-in (cleartext remains the default). When TLS.Enable is true the
	// client fails closed: there is no cleartext fallback.
	TLS TLSConfig
}

// TLSConfig configures optional TLS 1.3 for the secure profile (ADR 0020).
type TLSConfig struct {
	Enable bool
	// CAFile is an optional PEM CA bundle. Empty uses the system trust store
	// unless InsecureSkipVerify is set.
	CAFile string
	// CertFile and KeyFile enable mTLS when both are set.
	CertFile string
	KeyFile  string
	// ServerName overrides Host for SNI and hostname verification.
	ServerName string
	// InsecureSkipVerify disables certificate and hostname verification (lab only).
	InsecureSkipVerify bool
}

// CallOptions optionally overrides the client-wide request timeout for one call.
// Timeout == 0 means use Config.RequestTimeout. Negative values are invalid.
type CallOptions struct {
	Timeout time.Duration
}

// DefaultConfig returns production-oriented defaults matching the other SDKs.
func DefaultConfig() Config {
	return Config{
		Host:                    "127.0.0.1",
		Port:                    7379,
		ConnectTimeout:          3 * time.Second,
		RequestTimeout:          5 * time.Second,
		MaximumFrameBytes:       protocol.MaxFrameBytes,
		MaximumPipelineRequests: 256,
		MaximumPipelineBytes:    1024 * 1024,
	}
}
