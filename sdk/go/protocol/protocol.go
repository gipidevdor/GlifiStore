// Package protocol implements GlifiStore wire protocol v2 encoding, decoding,
// and Worker routing (FNV-1a / SipHash-2-4 per ADR 0030).
package protocol

import (
	"encoding/binary"
	"errors"
	"fmt"
)

const (
	Version                     = 2
	RequestHeaderBytes          = 40
	ResponseHeaderBytes         = 40
	MaxFrameBytes               = 2 * 1024 * 1024
	NoWorker                    = 0xFFFF_FFFF
	Identity                    = "GlifiStore/2"
	RoutingAlgFNV1a64V1         = 1
	RoutingAlgSipHash24V1       = 2
	WorkerRoutingSipKey1Xor     = 0x6a09e667f3bcc909
	InitIdentityExtendedBytes   = len(Identity) + 1 + 4 + 8
)

// WorkerRouting is the session Worker ownership algorithm (ADR 0030).
type WorkerRouting struct {
	Algorithm uint32
	Seed      uint64
}

// Keyed reports whether SipHash-2-4 routing is active.
func (r WorkerRouting) Keyed() bool {
	return r.Algorithm == RoutingAlgSipHash24V1
}

// Opcode is a wire-protocol v2 request opcode.
type Opcode uint8

const (
	OpcodeInit       Opcode = 1
	OpcodePing       Opcode = 2
	OpcodeGet        Opcode = 3
	OpcodePut        Opcode = 4
	OpcodeErase      Opcode = 5
	OpcodeBindWorker Opcode = 6
	OpcodeHealth     Opcode = 7
	OpcodeReady      Opcode = 8
	OpcodeStats      Opcode = 9
	OpcodeBackup     Opcode = 10
)

// Status is a wire-protocol v2 response status.
type Status uint16

const (
	StatusOK             Status = 0
	StatusInvalidRequest Status = 1
	StatusUnsupported    Status = 2
	StatusInternalError  Status = 3
	StatusNotFound       Status = 4
	StatusOverloaded     Status = 5
	StatusWrongOwner     Status = 6
	StatusNotBound      Status = 7
	StatusPermissionDenied Status = 8
)

// Request is a decoded protocol-v2 request.
type Request struct {
	Opcode       Opcode
	RequestID    uint64
	ExpireAtNs   uint64
	TargetWorker uint32
	Key          []byte
	Value        []byte
}

// Response is a decoded protocol-v2 response.
type Response struct {
	Status       Status
	RequestID    uint64
	OwnerWorker  uint32
	WorkerCount  uint32
	RoutingEpoch uint64
	Value        []byte
}

var (
	errShortRequest  = errors.New("request is shorter than its header")
	errShortResponse = errors.New("response is shorter than its header")
)

// RequestFrameSize returns the encoded size of a request with the given payloads.
func RequestFrameSize(key, value []byte) int {
	return RequestHeaderBytes + len(key) + len(value)
}

// EncodeRequest encodes one canonical protocol-v2 request frame.
func EncodeRequest(
	opcode Opcode,
	requestID uint64,
	key, value []byte,
	expireAtNs uint64,
	targetWorker uint32,
) ([]byte, error) {
	size := RequestFrameSize(key, value)
	buf := make([]byte, size)
	n, err := EncodeRequestInto(buf, opcode, requestID, key, value, expireAtNs, targetWorker)
	if err != nil {
		return nil, err
	}
	return buf[:n], nil
}

// EncodeRequestInto writes a request into dst (must be large enough) and returns
// the number of bytes written.
func EncodeRequestInto(
	dst []byte,
	opcode Opcode,
	requestID uint64,
	key, value []byte,
	expireAtNs uint64,
	targetWorker uint32,
) (int, error) {
	if err := validateRequestFields(opcode, key, value, expireAtNs, targetWorker); err != nil {
		return 0, err
	}
	frameSize := RequestFrameSize(key, value)
	if frameSize > MaxFrameBytes {
		return 0, errors.New("request exceeds the protocol frame limit")
	}
	if len(dst) < frameSize {
		return 0, errors.New("destination buffer is too small for request frame")
	}
	binary.LittleEndian.PutUint32(dst[0:4], uint32(frameSize))
	binary.LittleEndian.PutUint16(dst[4:6], Version)
	dst[6] = byte(opcode)
	dst[7] = 0
	binary.LittleEndian.PutUint64(dst[8:16], requestID)
	binary.LittleEndian.PutUint32(dst[16:20], uint32(len(key)))
	binary.LittleEndian.PutUint32(dst[20:24], uint32(len(value)))
	binary.LittleEndian.PutUint64(dst[24:32], expireAtNs)
	binary.LittleEndian.PutUint32(dst[32:36], targetWorker)
	binary.LittleEndian.PutUint32(dst[36:40], 0)
	copy(dst[RequestHeaderBytes:], key)
	copy(dst[RequestHeaderBytes+len(key):], value)
	return frameSize, nil
}

// AppendRequest appends an encoded request to dst.
func AppendRequest(
	dst []byte,
	opcode Opcode,
	requestID uint64,
	key, value []byte,
	expireAtNs uint64,
	targetWorker uint32,
) ([]byte, error) {
	size := RequestFrameSize(key, value)
	start := len(dst)
	dst = growSlice(dst, size)
	n, err := EncodeRequestInto(dst[start:start+size], opcode, requestID, key, value, expireAtNs, targetWorker)
	if err != nil {
		return nil, err
	}
	return dst[: start+n], nil
}

func validateRequestFields(
	opcode Opcode,
	key, value []byte,
	expireAtNs uint64,
	targetWorker uint32,
) error {
	switch opcode {
	case OpcodeInit:
		if len(key) != 0 || len(value) != 0 || expireAtNs != 0 || targetWorker != NoWorker {
			return errors.New("INIT request cannot carry key, value, expiry, or target_worker")
		}
	case OpcodeHealth, OpcodeReady, OpcodeStats:
		if len(key) != 0 || len(value) != 0 || expireAtNs != 0 || targetWorker != NoWorker {
			return errors.New("lifecycle probe cannot carry key, value, expiry, or target_worker")
		}
	case OpcodePing:
		if len(key) != 0 || expireAtNs != 0 || targetWorker != NoWorker {
			return errors.New("PING request cannot carry key, expiry, or target_worker")
		}
	case OpcodeGet:
		if len(key) == 0 || len(value) != 0 || expireAtNs != 0 || targetWorker != NoWorker {
			return errors.New("GET request requires a key and cannot carry value, expiry, or target_worker")
		}
	case OpcodePut:
		if len(key) == 0 || targetWorker != NoWorker {
			return errors.New("PUT request requires a key and cannot carry target_worker")
		}
	case OpcodeErase:
		if len(key) == 0 || len(value) != 0 || expireAtNs != 0 || targetWorker != NoWorker {
			return errors.New("ERASE request requires a key and cannot carry value, expiry, or target_worker")
		}
	case OpcodeBindWorker:
		if len(key) != 0 || len(value) != 0 || expireAtNs != 0 {
			return errors.New("BIND_WORKER request cannot carry key, value, or expiry")
		}
		if targetWorker == NoWorker {
			return errors.New("BIND_WORKER request requires an explicit target_worker")
		}
	case OpcodeBackup:
		if len(key) == 0 || len(value) != 0 || expireAtNs != 0 || targetWorker != NoWorker {
			return errors.New("BACKUP requires a destination path key and no value, expiry, or target_worker")
		}
	default:
		return errors.New("opcode is not defined by wire protocol v2")
	}
	return nil
}

// DecodeRequest decodes one complete request and rejects noncanonical fields.
func DecodeRequest(frame []byte, maximumFrameBytes int) (Request, error) {
	if maximumFrameBytes <= 0 {
		maximumFrameBytes = MaxFrameBytes
	}
	if len(frame) < RequestHeaderBytes {
		return Request{}, errShortRequest
	}
	frameSize := int(binary.LittleEndian.Uint32(frame[0:4]))
	if frameSize != len(frame) || frameSize > maximumFrameBytes {
		return Request{}, errors.New("request frame extent is invalid")
	}
	version := binary.LittleEndian.Uint16(frame[4:6])
	if version != Version {
		return Request{}, errors.New("request protocol version is unsupported")
	}
	flags := frame[7]
	reserved := binary.LittleEndian.Uint32(frame[36:40])
	if flags != 0 || reserved != 0 {
		return Request{}, errors.New("request canonical fields are invalid")
	}
	keySize := int(binary.LittleEndian.Uint32(frame[16:20]))
	valueSize := int(binary.LittleEndian.Uint32(frame[20:24]))
	if RequestHeaderBytes+keySize+valueSize != frameSize {
		return Request{}, errors.New("request payload extent is invalid")
	}
	opcode := Opcode(frame[6])
	if !validOpcode(opcode) {
		return Request{}, errors.New("request opcode is unknown")
	}
	keyStart := RequestHeaderBytes
	valueStart := keyStart + keySize
	key := append([]byte(nil), frame[keyStart:valueStart]...)
	value := append([]byte(nil), frame[valueStart:valueStart+valueSize]...)
	expireAtNs := binary.LittleEndian.Uint64(frame[24:32])
	targetWorker := binary.LittleEndian.Uint32(frame[32:36])
	if err := validateRequestFields(opcode, key, value, expireAtNs, targetWorker); err != nil {
		return Request{}, err
	}
	return Request{
		Opcode:       opcode,
		RequestID:    binary.LittleEndian.Uint64(frame[8:16]),
		ExpireAtNs:   expireAtNs,
		TargetWorker: targetWorker,
		Key:          key,
		Value:        value,
	}, nil
}

// EncodeResponse encodes one canonical protocol-v2 response frame.
func EncodeResponse(
	status Status,
	requestID uint64,
	value []byte,
	ownerWorker, workerCount uint32,
	routingEpoch uint64,
) ([]byte, error) {
	if !validStatus(status) {
		return nil, errors.New("status is not defined by wire protocol v2")
	}
	frameSize := ResponseHeaderBytes + len(value)
	if frameSize > MaxFrameBytes {
		return nil, errors.New("response exceeds the protocol frame limit")
	}
	buf := make([]byte, frameSize)
	binary.LittleEndian.PutUint32(buf[0:4], uint32(frameSize))
	binary.LittleEndian.PutUint16(buf[4:6], Version)
	binary.LittleEndian.PutUint16(buf[6:8], uint16(status))
	binary.LittleEndian.PutUint64(buf[8:16], requestID)
	binary.LittleEndian.PutUint32(buf[16:20], uint32(len(value)))
	binary.LittleEndian.PutUint32(buf[20:24], ownerWorker)
	binary.LittleEndian.PutUint32(buf[24:28], workerCount)
	binary.LittleEndian.PutUint32(buf[28:32], 0)
	binary.LittleEndian.PutUint64(buf[32:40], routingEpoch)
	copy(buf[ResponseHeaderBytes:], value)
	return buf, nil
}

// DecodeResponseView decodes one complete response. Value aliases frame and is
// only valid while frame remains unchanged; use OwnBytes before retaining it.
func DecodeResponseView(frame []byte, maximumFrameBytes int) (Response, error) {
	if maximumFrameBytes <= 0 {
		maximumFrameBytes = MaxFrameBytes
	}
	if len(frame) < ResponseHeaderBytes {
		return Response{}, errShortResponse
	}
	frameSize := int(binary.LittleEndian.Uint32(frame[0:4]))
	if frameSize != len(frame) || frameSize > maximumFrameBytes {
		return Response{}, errors.New("response frame extent is invalid")
	}
	version := binary.LittleEndian.Uint16(frame[4:6])
	if version != Version {
		return Response{}, errors.New("response protocol version is unsupported")
	}
	reserved := binary.LittleEndian.Uint32(frame[28:32])
	if reserved != 0 {
		return Response{}, errors.New("response reserved field is noncanonical")
	}
	valueSize := int(binary.LittleEndian.Uint32(frame[16:20]))
	if ResponseHeaderBytes+valueSize != frameSize {
		return Response{}, errors.New("response value extent is invalid")
	}
	status := Status(binary.LittleEndian.Uint16(frame[6:8]))
	if !validStatus(status) {
		return Response{}, errors.New("response status is unknown")
	}
	var value []byte
	if valueSize > 0 {
		value = frame[ResponseHeaderBytes:frameSize]
	}
	return Response{
		Status:       status,
		RequestID:    binary.LittleEndian.Uint64(frame[8:16]),
		OwnerWorker:  binary.LittleEndian.Uint32(frame[20:24]),
		WorkerCount:  binary.LittleEndian.Uint32(frame[24:28]),
		RoutingEpoch: binary.LittleEndian.Uint64(frame[32:40]),
		Value:        value,
	}, nil
}

// OwnBytes returns a retained copy of b, or nil when b is empty.
func OwnBytes(b []byte) []byte {
	if len(b) == 0 {
		return nil
	}
	out := make([]byte, len(b))
	copy(out, b)
	return out
}

// DecodeResponse decodes one complete response and returns an owned Value copy.
func DecodeResponse(frame []byte, maximumFrameBytes int) (Response, error) {
	response, err := DecodeResponseView(frame, maximumFrameBytes)
	if err != nil {
		return Response{}, err
	}
	response.Value = OwnBytes(response.Value)
	return response, nil
}

// FNV1a64 returns the canonical 64-bit FNV-1a hash used for default Worker routing.
func FNV1a64(key []byte) uint64 {
	const (
		offset = 14695981039346656037
		prime  = 1099511628211
	)
	value := uint64(offset)
	for _, b := range key {
		value ^= uint64(b)
		value *= prime
	}
	return value
}

func rotl64(value uint64, shift uint) uint64 {
	return (value << shift) | (value >> (64 - shift))
}

func sipRound(v0, v1, v2, v3 *uint64) {
	*v0 += *v1
	*v1 = rotl64(*v1, 13)
	*v1 ^= *v0
	*v0 = rotl64(*v0, 32)
	*v2 += *v3
	*v3 = rotl64(*v3, 16)
	*v3 ^= *v2
	*v0 += *v3
	*v3 = rotl64(*v3, 21)
	*v3 ^= *v0
	*v2 += *v1
	*v1 = rotl64(*v1, 17)
	*v1 ^= *v2
	*v2 = rotl64(*v2, 32)
}

// SipHash24 matches the C++ core SipHash-2-4 (Aumasson/Bernstein).
func SipHash24(key []byte, k0, k1 uint64) uint64 {
	v0 := k0 ^ 0x736f6d6570736575
	v1 := k1 ^ 0x646f72616e646f6d
	v2 := k0 ^ 0x6c7967656e657261
	v3 := k1 ^ 0x7465646279746573
	length := len(key)
	offset := 0
	for offset+8 <= length {
		message := binary.LittleEndian.Uint64(key[offset : offset+8])
		v3 ^= message
		sipRound(&v0, &v1, &v2, &v3)
		sipRound(&v0, &v1, &v2, &v3)
		v0 ^= message
		offset += 8
	}
	message := uint64(length) << 56
	for i := 0; offset+i < length; i++ {
		message |= uint64(key[offset+i]) << (8 * uint(i))
	}
	v3 ^= message
	sipRound(&v0, &v1, &v2, &v3)
	sipRound(&v0, &v1, &v2, &v3)
	v0 ^= message
	v2 ^= 0xff
	sipRound(&v0, &v1, &v2, &v3)
	sipRound(&v0, &v1, &v2, &v3)
	sipRound(&v0, &v1, &v2, &v3)
	sipRound(&v0, &v1, &v2, &v3)
	return v0 ^ v1 ^ v2 ^ v3
}

// ValidateWorkerRouting rejects unsupported algorithm/seed combinations.
// Algorithm 0 is treated as fnv1a64-v1 so the Go zero value matches C++ defaults.
func ValidateWorkerRouting(routing WorkerRouting) error {
	algorithm := routing.Algorithm
	if algorithm == 0 {
		algorithm = RoutingAlgFNV1a64V1
	}
	switch algorithm {
	case RoutingAlgFNV1a64V1:
		if routing.Seed != 0 {
			return errors.New("fnv1a64-v1 Worker routing requires a zero hash seed")
		}
		return nil
	case RoutingAlgSipHash24V1:
		return nil
	default:
		return errors.New("unsupported Worker routing algorithm")
	}
}

// HashKeyRouting hashes key under the given routing state (default FNV).
func HashKeyRouting(key []byte, routing WorkerRouting) (uint64, error) {
	if err := ValidateWorkerRouting(routing); err != nil {
		return 0, err
	}
	if routing.Algorithm == RoutingAlgSipHash24V1 {
		return SipHash24(key, routing.Seed, routing.Seed^WorkerRoutingSipKey1Xor), nil
	}
	return FNV1a64(key), nil
}

// EncodeInitIdentity encodes plain FNV or extended SipHash INIT identity bytes.
func EncodeInitIdentity(routing WorkerRouting) ([]byte, error) {
	if err := ValidateWorkerRouting(routing); err != nil {
		return nil, err
	}
	if !routing.Keyed() {
		return []byte(Identity), nil
	}
	out := make([]byte, InitIdentityExtendedBytes)
	copy(out, Identity)
	out[len(Identity)] = 0
	binary.LittleEndian.PutUint32(out[len(Identity)+1:], routing.Algorithm)
	binary.LittleEndian.PutUint64(out[len(Identity)+1+4:], routing.Seed)
	return out, nil
}

// DecodeInitIdentity parses plain FNV or extended SipHash INIT identity (fail closed).
func DecodeInitIdentity(value []byte) (WorkerRouting, error) {
	if string(value) == Identity {
		return WorkerRouting{Algorithm: RoutingAlgFNV1a64V1}, nil
	}
	if len(value) != InitIdentityExtendedBytes {
		return WorkerRouting{}, errors.New("server INIT identity value has unexpected length")
	}
	if string(value[:len(Identity)]) != Identity || value[len(Identity)] != 0 {
		return WorkerRouting{}, errors.New("server INIT identity prefix is invalid")
	}
	algorithm := binary.LittleEndian.Uint32(value[len(Identity)+1:])
	seed := binary.LittleEndian.Uint64(value[len(Identity)+1+4:])
	state := WorkerRouting{Algorithm: algorithm, Seed: seed}
	if err := ValidateWorkerRouting(state); err != nil {
		return WorkerRouting{}, err
	}
	if !state.Keyed() {
		return WorkerRouting{}, errors.New("server INIT extended identity must use siphash24-v1 routing")
	}
	return state, nil
}

// WorkerFor returns the owning Worker index for key under workerCount and optional routing.
func WorkerFor(key []byte, workerCount uint32, routing ...WorkerRouting) (uint32, error) {
	if workerCount == 0 {
		return 0, errors.New("worker_count must be positive")
	}
	state := WorkerRouting{Algorithm: RoutingAlgFNV1a64V1}
	if len(routing) > 0 {
		state = routing[0]
	}
	digest, err := HashKeyRouting(key, state)
	if err != nil {
		return 0, err
	}
	return uint32(digest % uint64(workerCount)), nil
}

func validOpcode(opcode Opcode) bool {
	switch opcode {
	case OpcodeInit, OpcodePing, OpcodeGet, OpcodePut, OpcodeErase, OpcodeBindWorker,
		OpcodeHealth, OpcodeReady, OpcodeStats, OpcodeBackup:
		return true
	default:
		return false
	}
}

func validStatus(status Status) bool {
	return status <= StatusPermissionDenied
}

func growSlice(dst []byte, n int) []byte {
	if cap(dst)-len(dst) >= n {
		return dst[: len(dst)+n]
	}
	out := make([]byte, len(dst)+n)
	copy(out, dst)
	return out
}

// FormatStatus returns a stable name for diagnostics.
func FormatStatus(status Status) string {
	switch status {
	case StatusOK:
		return "OK"
	case StatusInvalidRequest:
		return "INVALID_REQUEST"
	case StatusUnsupported:
		return "UNSUPPORTED"
	case StatusInternalError:
		return "INTERNAL_ERROR"
	case StatusNotFound:
		return "NOT_FOUND"
	case StatusOverloaded:
		return "OVERLOADED"
	case StatusWrongOwner:
		return "WRONG_OWNER"
	case StatusNotBound:
		return "NOT_BOUND"
	case StatusPermissionDenied:
		return "PERMISSION_DENIED"
	default:
		return fmt.Sprintf("STATUS_%d", status)
	}
}
