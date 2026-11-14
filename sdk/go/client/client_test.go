package client_test

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/pem"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/client"
	"github.com/gpicchiarelli/GlifiStore/sdk/go/protocol"
)

func mustEncodeInitIdentity(routing protocol.WorkerRouting) []byte {
	identity, err := protocol.EncodeInitIdentity(routing)
	if err != nil {
		panic(err)
	}
	return identity
}

type fakeServer struct {
	workerCount            uint32
	disconnectOnPut        bool
	internalErrorOnPut     bool
	internalErrorOnBackup  bool
	wrongRequestIdOnBackup bool
	stallOnGet             bool
	failRebindWorkers      map[uint32]struct{}
	bindCounts             map[uint32]int
	bindMu                 sync.Mutex
	routing                protocol.WorkerRouting
	tlsConfig              *tls.Config
	ln                     net.Listener
	backupRequests         atomic.Uint32
	values                 map[string][]byte
	valuesMu               sync.Mutex
	wg                     sync.WaitGroup
	stop                   chan struct{}
}

func startFakeServer(t *testing.T, workerCount uint32, disconnectOnPut, internalErrorOnPut bool) *fakeServer {
	t.Helper()
	return startFakeServerTLS(t, workerCount, disconnectOnPut, internalErrorOnPut, nil)
}

func startFakeServerTLS(t *testing.T, workerCount uint32, disconnectOnPut, internalErrorOnPut bool, tlsConfig *tls.Config) *fakeServer {
	t.Helper()
	if workerCount == 0 {
		workerCount = 1
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	if tlsConfig != nil {
		ln = tls.NewListener(ln, tlsConfig)
	}
	s := &fakeServer{
		workerCount:        workerCount,
		disconnectOnPut:    disconnectOnPut,
		internalErrorOnPut: internalErrorOnPut,
		tlsConfig:          tlsConfig,
		ln:                 ln,
		values:             make(map[string][]byte),
		bindCounts:         make(map[uint32]int),
		failRebindWorkers:  make(map[uint32]struct{}),
		stop:               make(chan struct{}),
	}
	s.wg.Add(1)
	go s.accept()
	return s
}

func (s *fakeServer) port() int {
	return s.ln.Addr().(*net.TCPAddr).Port
}

func (s *fakeServer) close() {
	close(s.stop)
	_ = s.ln.Close()
	s.wg.Wait()
}

func (s *fakeServer) accept() {
	defer s.wg.Done()
	for {
		conn, err := s.ln.Accept()
		if err != nil {
			select {
			case <-s.stop:
				return
			default:
				return
			}
		}
		s.wg.Add(1)
		go func(c net.Conn) {
			defer s.wg.Done()
			s.serve(c)
		}(conn)
	}
}

func (s *fakeServer) serve(conn net.Conn) {
	defer conn.Close()
	var bound *uint32
	buf := make([]byte, 0, 8192)
	for {
		for {
			if len(buf) >= 4 {
				size := int(binary.LittleEndian.Uint32(buf[:4]))
				if len(buf) >= size {
					frame := buf[:size]
					buf = buf[size:]
					req, err := protocol.DecodeRequest(frame, protocol.MaxFrameBytes)
					if err != nil {
						return
					}
					if !s.handle(conn, &bound, req) {
						return
					}
					continue
				}
			}
			break
		}
		chunk := make([]byte, 64*1024)
		n, err := conn.Read(chunk)
		if n > 0 {
			buf = append(buf, chunk[:n]...)
		}
		if err != nil {
			return
		}
	}
}

func (s *fakeServer) send(conn net.Conn, status protocol.Status, requestID uint64, owner uint32, value []byte) bool {
	frame, err := protocol.EncodeResponse(status, requestID, value, owner, s.workerCount, 9)
	if err != nil {
		return false
	}
	_, err = conn.Write(frame)
	return err == nil
}

func (s *fakeServer) handle(conn net.Conn, bound **uint32, req protocol.Request) bool {
	switch req.Opcode {
	case protocol.OpcodeInit:
		return s.send(conn, protocol.StatusOK, req.RequestID, 0, mustEncodeInitIdentity(s.routing))
	case protocol.OpcodeHealth:
		owner := uint32(0)
		if *bound != nil {
			owner = **bound
		}
		return s.send(conn, protocol.StatusOK, req.RequestID, owner, []byte("GlifiStore/live"))
	case protocol.OpcodeReady:
		owner := uint32(0)
		if *bound != nil {
			owner = **bound
		}
		return s.send(conn, protocol.StatusOK, req.RequestID, owner, []byte("GlifiStore/ready"))
	case protocol.OpcodeStats:
		owner := uint32(0)
		if *bound != nil {
			owner = **bound
		}
		return s.send(conn, protocol.StatusOK, req.RequestID, owner, []byte("GlifiStore/stats\n"))
	case protocol.OpcodeBindWorker:
		if req.TargetWorker >= s.workerCount {
			return s.send(conn, protocol.StatusInvalidRequest, req.RequestID, 0, nil)
		}
		s.bindMu.Lock()
		s.bindCounts[req.TargetWorker]++
		count := s.bindCounts[req.TargetWorker]
		_, failRebind := s.failRebindWorkers[req.TargetWorker]
		s.bindMu.Unlock()
		if failRebind && count > 1 {
			return false
		}
		w := req.TargetWorker
		*bound = &w
		return s.send(conn, protocol.StatusOK, req.RequestID, w, nil)
	}
	if *bound == nil {
		return s.send(conn, protocol.StatusNotBound, req.RequestID, 0, nil)
	}
	ownerBound := **bound
	switch req.Opcode {
	case protocol.OpcodePing:
		return s.send(conn, protocol.StatusOK, req.RequestID, ownerBound, req.Value)
	case protocol.OpcodePut:
		owner, _ := protocol.WorkerFor(req.Key, s.workerCount, s.routing)
		if ownerBound != owner {
			return s.send(conn, protocol.StatusWrongOwner, req.RequestID, owner, nil)
		}
		s.valuesMu.Lock()
		s.values[string(req.Key)] = append([]byte(nil), req.Value...)
		s.valuesMu.Unlock()
		if s.disconnectOnPut {
			return false
		}
		if s.internalErrorOnPut {
			return s.send(conn, protocol.StatusInternalError, req.RequestID, ownerBound, nil)
		}
		return s.send(conn, protocol.StatusOK, req.RequestID, ownerBound, nil)
	case protocol.OpcodeGet:
		if s.stallOnGet {
			select {
			case <-s.stop:
			case <-time.After(time.Hour):
			}
			return false
		}
		owner, _ := protocol.WorkerFor(req.Key, s.workerCount, s.routing)
		if ownerBound != owner {
			return s.send(conn, protocol.StatusWrongOwner, req.RequestID, owner, nil)
		}
		s.valuesMu.Lock()
		value, ok := s.values[string(req.Key)]
		s.valuesMu.Unlock()
		if !ok {
			return s.send(conn, protocol.StatusNotFound, req.RequestID, ownerBound, nil)
		}
		return s.send(conn, protocol.StatusOK, req.RequestID, ownerBound, value)
	case protocol.OpcodeErase:
		owner, _ := protocol.WorkerFor(req.Key, s.workerCount, s.routing)
		if ownerBound != owner {
			return s.send(conn, protocol.StatusWrongOwner, req.RequestID, owner, nil)
		}
		s.valuesMu.Lock()
		_, ok := s.values[string(req.Key)]
		if ok {
			delete(s.values, string(req.Key))
		}
		s.valuesMu.Unlock()
		status := protocol.StatusOK
		if !ok {
			status = protocol.StatusNotFound
		}
		return s.send(conn, status, req.RequestID, ownerBound, nil)
	case protocol.OpcodeBackup:
		s.backupRequests.Add(1)
		if s.internalErrorOnBackup {
			return s.send(conn, protocol.StatusInternalError, req.RequestID, ownerBound, []byte("report failed"))
		}
		replyID := req.RequestID
		if s.wrongRequestIdOnBackup {
			replyID ^= 1
		}
		return s.send(conn, protocol.StatusOK, replyID, ownerBound, []byte("status=ok files=0 bytes=0"))
	default:
		return s.send(conn, protocol.StatusUnsupported, req.RequestID, ownerBound, nil)
	}
}

func TestBinaryPutGetPingAndErase(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	if c.WorkerCount() != 1 || c.RoutingEpoch() != 9 {
		t.Fatalf("bootstrap metadata unexpected")
	}
	owner, err := c.WorkerFor([]byte("binary\x00key"))
	if err != nil || owner != 0 {
		t.Fatalf("worker_for: %v %d", err, owner)
	}
	if !c.Put([]byte("binary\x00key"), []byte("value\x00\xff"), 0).Committed() {
		t.Fatal("put failed")
	}
	got, err := c.Get([]byte("binary\x00key"))
	if err != nil || string(got) != "value\x00\xff" {
		t.Fatalf("get: %v %q", err, got)
	}
	pong, err := c.Ping([]byte("hello"))
	if err != nil || string(pong) != "hello" {
		t.Fatalf("ping: %v %q", err, pong)
	}
	if !c.Erase([]byte("binary\x00key")).Committed() {
		t.Fatal("erase failed")
	}
	_, err = c.Get([]byte("binary\x00key"))
	if err == nil {
		t.Fatal("expected not found")
	}
	ge, ok := err.(*client.Error)
	if !ok || ge.Category != client.CategoryNotFound {
		t.Fatalf("expected not_found, got %v", err)
	}
}

func TestEraseAbsentIsRejected(t *testing.T) {
	// client-semantics §3: wire NOT_FOUND on standalone ERASE → rejected.
	server := startFakeServer(t, 1, false, false)
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	result := c.Erase([]byte("missing-key"))
	if result.Outcome != client.MutationRejected {
		t.Fatalf("outcome=%v", result.Outcome)
	}
	ge, ok := result.Err.(*client.Error)
	if !ok || ge.Category != client.CategoryNotFound || ge.MutationOutcome != client.MutationRejected {
		t.Fatalf("err=%v", result.Err)
	}
	if ge.Retryability != client.RetryNewAttempt {
		t.Fatalf("retryability=%v", ge.Retryability)
	}
}

func TestPipelineEraseAbsentIsFailedRejected(t *testing.T) {
	// client-semantics §3: wire NOT_FOUND on pipeline ERASE → failed + rejected.
	server := startFakeServer(t, 1, false, false)
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	responses, err := c.ExecutePipeline([]client.PipelineRequest{
		{Opcode: client.PipelineErase, Key: []byte("missing-key")},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(responses) != 1 || responses[0].Outcome != client.PipelineFailed {
		t.Fatalf("responses=%v", responses)
	}
	ge, ok := responses[0].Err.(*client.Error)
	if !ok || ge.Category != client.CategoryNotFound || ge.MutationOutcome != client.MutationRejected {
		t.Fatalf("err=%v", responses[0].Err)
	}
	if ge.Retryability != client.RetryNewAttempt {
		t.Fatalf("retryability=%v", ge.Retryability)
	}
	if ge.WireStatus == nil || *ge.WireStatus != protocol.StatusNotFound {
		t.Fatalf("wire_status=%v", ge.WireStatus)
	}
}

func TestBatchEraseAbsentIsFailedRejected(t *testing.T) {
	// client-semantics §3/§5: execute_batch absent ERASE → failed + rejected.
	server := startFakeServer(t, 1, false, false)
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	responses, err := c.ExecuteBatch([]client.PipelineRequest{
		{Opcode: client.PipelineErase, Key: []byte("missing-key")},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(responses) != 1 || responses[0].Outcome != client.PipelineFailed {
		t.Fatalf("responses=%v", responses)
	}
	ge, ok := responses[0].Err.(*client.Error)
	if !ok || ge.Category != client.CategoryNotFound || ge.MutationOutcome != client.MutationRejected {
		t.Fatalf("err=%v", responses[0].Err)
	}
	if ge.Retryability != client.RetryNewAttempt {
		t.Fatalf("retryability=%v", ge.Retryability)
	}
	if ge.WireStatus == nil || *ge.WireStatus != protocol.StatusNotFound {
		t.Fatalf("wire_status=%v", ge.WireStatus)
	}
}

func TestWorkerPipelinesEraseAbsentIsFailedRejected(t *testing.T) {
	// client-semantics §3/§5: pre-sharded worker pipelines inherit pipeline ERASE mapping.
	server := startFakeServer(t, 1, false, false)
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	results, err := c.ExecuteWorkerPipelines([][]client.PipelineRequest{
		{{Opcode: client.PipelineErase, Key: []byte("missing-key")}},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 || len(results[0]) != 1 || results[0][0].Outcome != client.PipelineFailed {
		t.Fatalf("results=%v", results)
	}
	ge, ok := results[0][0].Err.(*client.Error)
	if !ok || ge.Category != client.CategoryNotFound || ge.MutationOutcome != client.MutationRejected {
		t.Fatalf("err=%v", results[0][0].Err)
	}
	if ge.Retryability != client.RetryNewAttempt {
		t.Fatalf("retryability=%v", ge.Retryability)
	}
	if ge.WireStatus == nil || *ge.WireStatus != protocol.StatusNotFound {
		t.Fatalf("wire_status=%v", ge.WireStatus)
	}
}

func TestHealthReadyStatsAndRouting(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	if !c.Healthy() {
		t.Fatal("expected healthy client")
	}
	_ = c.Routing()
	live, err := c.Health()
	if err != nil || string(live) != "GlifiStore/live" {
		t.Fatalf("health: %v %q", err, live)
	}
	ready, err := c.Ready()
	if err != nil || string(ready) != "GlifiStore/ready" {
		t.Fatalf("ready: %v %q", err, ready)
	}
	stats, err := c.Stats()
	if err != nil || !strings.HasPrefix(string(stats), "GlifiStore/stats") {
		t.Fatalf("stats: %v %q", err, stats)
	}
}

func TestUnixSocketRefusesTLS(t *testing.T) {
	_, err := client.Connect(client.Config{
		UnixSocketPath: "/tmp/glifistore-go-uds-tls-refuse.sock",
		TLS:            client.TLSConfig{Enable: true},
	})
	if err == nil {
		t.Fatal("expected invalid_argument for TLS over AF_UNIX")
	}
	ge, ok := err.(*client.Error)
	if !ok || ge.Category != client.CategoryInvalidArgument {
		t.Fatalf("expected invalid_argument, got %v", err)
	}
}

func TestInternalErrorMutationIsIndeterminate(t *testing.T) {
	server := startFakeServer(t, 1, false, true)
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	result := c.Put([]byte("key"), []byte("value"), 0)
	if result.Outcome != client.MutationIndeterminate {
		t.Fatalf("outcome=%s", result.Outcome)
	}
	ge, ok := result.Err.(*client.Error)
	if !ok || ge.Retryability != client.RetryReconcileFirst ||
		ge.MutationOutcome != client.MutationIndeterminate || ge.BytesSent == 0 {
		t.Fatalf("enrichment: %+v", result.Err)
	}
}

func TestKeyedSipHashInitAndRouting(t *testing.T) {
	server := startFakeServer(t, 8, false, false)
	server.routing = protocol.WorkerRouting{Algorithm: protocol.RoutingAlgSipHash24V1, Seed: 0x1111222233334444}
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if !c.Routing().Keyed() || c.Routing().Seed != 0x1111222233334444 {
		t.Fatalf("routing=%+v", c.Routing())
	}
	owner, err := c.WorkerFor([]byte("tenant-a/orders/1"))
	if err != nil {
		t.Fatal(err)
	}
	if owner != 6 {
		t.Fatalf("owner=%d", owner)
	}
	result := c.Put([]byte("tenant-a/orders/1"), []byte("v"), 0)
	if !result.Committed() {
		t.Fatalf("%v", result.Err)
	}
	got, err := c.Get([]byte("tenant-a/orders/1"))
	if err != nil || string(got) != "v" {
		t.Fatalf("%q %v", got, err)
	}
}

func TestDisconnectAfterMutationIsIndeterminate(t *testing.T) {
	server := startFakeServer(t, 1, true, false)
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	result := c.Put([]byte("key"), []byte("value"), 0)
	if result.Outcome != client.MutationIndeterminate {
		t.Fatalf("outcome=%s err=%v", result.Outcome, result.Err)
	}
	ge, ok := result.Err.(*client.Error)
	if !ok {
		t.Fatalf("expected *client.Error, got %T", result.Err)
	}
	if ge.BytesSent <= 0 {
		t.Fatalf("bytes_sent=%d want > 0", ge.BytesSent)
	}
	if ge.Retryability != client.RetryReconcileFirst {
		t.Fatalf("retryability=%s", ge.Retryability)
	}
	if ge.MutationOutcome != client.MutationIndeterminate {
		t.Fatalf("mutation_outcome=%s", ge.MutationOutcome)
	}
}

func TestPipelinePreservesOrder(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	var requests []client.PipelineRequest
	var expected [][]byte
	for i := 0; i < 64; i++ {
		value := []byte("pipeline-" + strconv.Itoa(i))
		expected = append(expected, value)
		requests = append(requests,
			client.PipelineRequest{Opcode: client.PipelinePut, Key: []byte("key"), Value: value},
			client.PipelineRequest{Opcode: client.PipelineGet, Key: []byte("key")},
		)
	}
	responses, err := c.ExecutePipeline(requests)
	if err != nil {
		t.Fatal(err)
	}
	if len(responses) != len(requests) {
		t.Fatalf("len=%d", len(responses))
	}
	for i, value := range expected {
		if !responses[i*2].Succeeded() || !responses[i*2+1].Succeeded() {
			t.Fatalf("pipeline outcomes failed at %d", i)
		}
		if string(responses[i*2+1].Value) != string(value) {
			t.Fatalf("ordered value mismatch at %d", i)
		}
	}
}

func TestPipelineDisconnectClassifiesEachRequest(t *testing.T) {
	server := startFakeServer(t, 1, true, false)
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	responses, err := c.ExecutePipeline([]client.PipelineRequest{
		{Opcode: client.PipelinePut, Key: []byte("key"), Value: []byte("value")},
		{Opcode: client.PipelineGet, Key: []byte("key")},
		{Opcode: client.PipelineErase, Key: []byte("key")},
	})
	if err != nil {
		t.Fatal(err)
	}
	if responses[0].Outcome != client.PipelineIndeterminate ||
		responses[1].Outcome != client.PipelineFailed ||
		responses[2].Outcome != client.PipelineIndeterminate {
		t.Fatalf("unexpected outcomes: %+v", responses)
	}
	ge0, ok := responses[0].Err.(*client.Error)
	if !ok || ge0.BytesSent <= 0 || ge0.Retryability != client.RetryReconcileFirst ||
		ge0.MutationOutcome != client.MutationIndeterminate {
		t.Fatalf("PUT error enrichment: %+v", responses[0].Err)
	}
	ge2, ok := responses[2].Err.(*client.Error)
	if !ok || ge2.Retryability != client.RetryReconcileFirst ||
		ge2.MutationOutcome != client.MutationIndeterminate {
		t.Fatalf("ERASE error enrichment: %+v", responses[2].Err)
	}
}

func TestPipelineInternalErrorMutationIsReconcileFirst(t *testing.T) {
	server := startFakeServer(t, 1, false, true)
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	responses, err := c.ExecutePipeline([]client.PipelineRequest{
		{Opcode: client.PipelinePut, Key: []byte("key"), Value: []byte("value")},
	})
	if err != nil {
		t.Fatal(err)
	}
	if responses[0].Outcome != client.PipelineIndeterminate {
		t.Fatalf("outcome=%s", responses[0].Outcome)
	}
	ge, ok := responses[0].Err.(*client.Error)
	if !ok || ge.Retryability != client.RetryReconcileFirst ||
		ge.MutationOutcome != client.MutationIndeterminate || ge.BytesSent == 0 {
		t.Fatalf("enrichment: %+v", responses[0].Err)
	}
}

func TestPipelineLimitsFailBeforeTransmission(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port(), MaximumPipelineRequests: 1})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	_, err = c.ExecutePipeline([]client.PipelineRequest{
		{Opcode: client.PipelineGet, Key: []byte("key")},
		{Opcode: client.PipelineGet, Key: []byte("key")},
	})
	ge, ok := err.(*client.Error)
	if !ok || ge.Category != client.CategoryInvalidArgument {
		t.Fatalf("expected invalid_argument, got %v", err)
	}
}

func TestMultiWorkerBootstrapAndBatch(t *testing.T) {
	server := startFakeServer(t, 2, false, false)
	defer server.close()
	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	if c.WorkerCount() != 2 {
		t.Fatalf("worker_count=%d", c.WorkerCount())
	}

	keys := make([][]byte, 0, 64)
	owners := map[uint32]bool{}
	for i := 0; i < 64; i++ {
		key := []byte("batch-" + strconv.Itoa(i))
		keys = append(keys, key)
		owner, err := c.WorkerFor(key)
		if err != nil {
			t.Fatal(err)
		}
		owners[owner] = true
	}
	if len(owners) != 2 {
		t.Fatalf("keys did not span both Workers: %v", owners)
	}

	var requests []client.PipelineRequest
	for _, key := range keys {
		rev := reverse(key)
		requests = append(requests, client.PipelineRequest{Opcode: client.PipelinePut, Key: key, Value: rev})
	}
	for _, key := range keys {
		requests = append(requests, client.PipelineRequest{Opcode: client.PipelineGet, Key: key})
	}
	responses, err := c.ExecuteBatch(requests)
	if err != nil {
		t.Fatal(err)
	}
	if len(responses) != len(requests) {
		t.Fatalf("len=%d", len(responses))
	}
	for i, key := range keys {
		if !responses[i].Succeeded() || !responses[len(keys)+i].Succeeded() {
			t.Fatalf("batch outcomes failed at %d", i)
		}
		if string(responses[len(keys)+i].Value) != string(reverse(key)) {
			t.Fatalf("batch order mismatch at %d", i)
		}
	}

	c.Close()
	limited, err := client.Connect(client.Config{Port: server.port(), MaximumPipelineRequests: 1})
	if err != nil {
		t.Fatal(err)
	}
	defer limited.Close()
	var key0 []byte
	for _, key := range keys {
		owner, _ := limited.WorkerFor(key)
		if owner == 0 {
			key0 = key
			break
		}
	}
	_, err = limited.ExecuteBatch([]client.PipelineRequest{
		{Opcode: client.PipelineGet, Key: key0},
		{Opcode: client.PipelineGet, Key: key0},
	})
	ge, ok := err.(*client.Error)
	if !ok || ge.Category != client.CategoryInvalidArgument {
		t.Fatalf("expected invalid_argument, got %v", err)
	}
}

func TestExecuteWorkerPipelinesPreShardedFanout(t *testing.T) {
	server := startFakeServer(t, 2, false, false)
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	keys := make([][]byte, 2)
	for candidate := 0; keys[0] == nil || keys[1] == nil; candidate++ {
		key := []byte("wp-" + strconv.Itoa(candidate))
		owner, err := c.WorkerFor(key)
		if err != nil {
			t.Fatal(err)
		}
		if keys[owner] == nil {
			keys[owner] = key
		}
	}
	results, err := c.ExecuteWorkerPipelines([][]client.PipelineRequest{
		{
			{Opcode: client.PipelinePut, Key: keys[0], Value: []byte("a")},
			{Opcode: client.PipelineGet, Key: keys[0]},
		},
		{
			{Opcode: client.PipelinePut, Key: keys[1], Value: []byte("b")},
			{Opcode: client.PipelineGet, Key: keys[1]},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 2 || len(results[0]) != 2 || len(results[1]) != 2 {
		t.Fatalf("unexpected shape: %#v", results)
	}
	if !results[0][0].Succeeded() || string(results[0][1].Value) != "a" {
		t.Fatalf("worker 0 results: %#v", results[0])
	}
	if !results[1][0].Succeeded() || string(results[1][1].Value) != "b" {
		t.Fatalf("worker 1 results: %#v", results[1])
	}
	sparse, err := c.ExecuteWorkerPipelines([][]client.PipelineRequest{
		{},
		{{Opcode: client.PipelineGet, Key: keys[1]}},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(sparse[0]) != 0 || !sparse[1][0].Succeeded() {
		t.Fatalf("sparse results: %#v", sparse)
	}
	_, err = c.ExecuteWorkerPipelines([][]client.PipelineRequest{
		{{Opcode: client.PipelineGet, Key: keys[1]}},
		{},
	})
	ge, ok := err.(*client.Error)
	if !ok || ge.Category != client.CategoryInvalidArgument {
		t.Fatalf("expected misroute invalid_argument, got %v", err)
	}
	_, err = c.ExecuteWorkerPipelines([][]client.PipelineRequest{
		{{Opcode: client.PipelineGet, Key: keys[0]}},
	})
	ge, ok = err.(*client.Error)
	if !ok || ge.Category != client.CategoryInvalidArgument {
		t.Fatalf("expected length invalid_argument, got %v", err)
	}
}

func TestBatchPreservesSiblingResultsWhenOneWorkerFails(t *testing.T) {
	server := startFakeServer(t, 2, false, false)
	server.failRebindWorkers[1] = struct{}{}
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	keys := make([][]byte, 2)
	for candidate := 0; keys[0] == nil || keys[1] == nil; candidate++ {
		key := []byte("sib-" + strconv.Itoa(candidate))
		owner, err := c.WorkerFor(key)
		if err != nil {
			t.Fatal(err)
		}
		if keys[owner] == nil {
			keys[owner] = key
		}
	}
	c.ResetWorkerConnection(1)
	responses, err := c.ExecuteBatch([]client.PipelineRequest{
		{Opcode: client.PipelinePut, Key: keys[0], Value: []byte("a")},
		{Opcode: client.PipelinePut, Key: keys[1], Value: []byte("b")},
	})
	if err != nil {
		t.Fatalf("batch must return per-slot results, got error: %v", err)
	}
	if !responses[0].Succeeded() {
		t.Fatalf("worker0 slot: %+v", responses[0])
	}
	if responses[1].Outcome != client.PipelineFailed || responses[1].Err == nil {
		t.Fatalf("worker1 slot: %+v", responses[1])
	}
	ge, ok := responses[1].Err.(*client.Error)
	if !ok {
		t.Fatalf("worker1 error type: %T", responses[1].Err)
	}
	if ge.MutationOutcome != client.MutationRejected || ge.BytesSent != 0 {
		t.Fatalf("worker1 rejected enrich: outcome=%s bytes=%d", ge.MutationOutcome, ge.BytesSent)
	}
	got, err := c.Get(keys[0])
	if err != nil || string(got) != "a" {
		t.Fatalf("sibling committed value: %q %v", got, err)
	}
}

func TestSingleWorkerBatchConvertsAdmissionFailureToSlots(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	server.failRebindWorkers[0] = struct{}{}
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.ResetWorkerConnection(0)

	responses, err := c.ExecuteBatch([]client.PipelineRequest{
		{Opcode: client.PipelinePut, Key: []byte("single"), Value: []byte("value")},
	})
	if err != nil {
		t.Fatalf("batch must return positional failure, got %v", err)
	}
	if len(responses) != 1 || responses[0].Outcome != client.PipelineFailed {
		t.Fatalf("responses=%+v", responses)
	}
	ge, ok := responses[0].Err.(*client.Error)
	if !ok || ge.BytesSent != 0 || ge.MutationOutcome != client.MutationRejected {
		t.Fatalf("enrichment=%+v", responses[0].Err)
	}
}

func TestPerCallTimeoutOverridesConfig(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	server.stallOnGet = true
	defer server.close()

	c, err := client.Connect(client.Config{Port: server.port(), RequestTimeout: 5 * time.Second})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	started := time.Now()
	_, err = c.Get([]byte("key"), client.CallOptions{Timeout: 50 * time.Millisecond})
	elapsed := time.Since(started)
	if err == nil {
		t.Fatal("expected timeout")
	}
	ge, ok := err.(*client.Error)
	if !ok || ge.Category != client.CategoryTransport {
		t.Fatalf("expected transport, got %v", err)
	}
	if elapsed > 2*time.Second {
		t.Fatalf("timeout took too long: %v", elapsed)
	}
	_, err = c.Get([]byte("key"), client.CallOptions{Timeout: -1})
	if err == nil {
		t.Fatal("expected invalid timeout")
	}
}

func TestRejectsInvalidConfig(t *testing.T) {
	_, err := client.Connect(client.Config{
		Host:                    "127.0.0.1",
		Port:                    1,
		ConnectTimeout:          time.Second,
		RequestTimeout:          time.Second,
		MaximumFrameBytes:       1,
		MaximumPipelineRequests: 1,
		MaximumPipelineBytes:    40,
	})
	if err == nil {
		t.Fatal("expected invalid config")
	}
}

func TestStructuredErrorFieldsOnInternalPut(t *testing.T) {
	server := startFakeServer(t, 1, false, true)
	defer server.close()
	c, err := client.Connect(client.Config{
		Host:           "127.0.0.1",
		Port:           server.port(),
		ConnectTimeout: time.Second,
		RequestTimeout: time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	result := c.Put([]byte("key"), []byte("value"), 0)
	if result.Outcome != client.MutationIndeterminate {
		t.Fatalf("outcome=%v", result.Outcome)
	}
	ge, ok := result.Err.(*client.Error)
	if !ok {
		t.Fatalf("expected *client.Error, got %T", result.Err)
	}
	if ge.Category != client.CategoryInternal {
		t.Fatalf("category=%v", ge.Category)
	}
	if ge.WireStatus == nil || *ge.WireStatus != protocol.StatusInternalError {
		t.Fatalf("wire_status=%v", ge.WireStatus)
	}
	if ge.Operation != "put" {
		t.Fatalf("operation=%q", ge.Operation)
	}
	if ge.Retryability != client.RetryReconcileFirst {
		t.Fatalf("retryability=%v", ge.Retryability)
	}
	if ge.MutationOutcome != client.MutationIndeterminate {
		t.Fatalf("mutation_outcome=%v", ge.MutationOutcome)
	}
}

func TestBackupInternalErrorIsReconcileFirst(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	server.internalErrorOnBackup = true
	defer server.close()
	c, err := client.Connect(client.Config{
		Host:           "127.0.0.1",
		Port:           server.port(),
		ConnectTimeout: time.Second,
		RequestTimeout: time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	_, err = c.Backup("/tmp/glifistore-go-backup-internal")
	if err == nil {
		t.Fatal("expected backup INTERNAL_ERROR")
	}
	ge, ok := err.(*client.Error)
	if !ok {
		t.Fatalf("expected *client.Error, got %T", err)
	}
	if ge.Retryability != client.RetryReconcileFirst {
		t.Fatalf("retryability=%v", ge.Retryability)
	}
	if ge.MutationOutcome != client.MutationIndeterminate {
		t.Fatalf("mutation_outcome=%v", ge.MutationOutcome)
	}
	if server.backupRequests.Load() != 1 {
		t.Fatalf("backup_requests=%d", server.backupRequests.Load())
	}
}

func TestBackupValidateFailureIsReconcileFirst(t *testing.T) {
	server := startFakeServer(t, 1, false, false)
	server.wrongRequestIdOnBackup = true
	defer server.close()
	c, err := client.Connect(client.Config{
		Host:           "127.0.0.1",
		Port:           server.port(),
		ConnectTimeout: time.Second,
		RequestTimeout: time.Second,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	_, err = c.Backup("/tmp/glifistore-go-backup-wrong-id")
	if err == nil {
		t.Fatal("expected backup validate failure")
	}
	ge, ok := err.(*client.Error)
	if !ok {
		t.Fatalf("expected *client.Error, got %T", err)
	}
	if ge.Retryability != client.RetryReconcileFirst {
		t.Fatalf("retryability=%v", ge.Retryability)
	}
	if ge.MutationOutcome != client.MutationIndeterminate {
		t.Fatalf("mutation_outcome=%v", ge.MutationOutcome)
	}
	if ge.BytesSent == 0 {
		t.Fatal("expected bytes_sent > 0")
	}
	if server.backupRequests.Load() != 1 {
		t.Fatalf("backup_requests=%d", server.backupRequests.Load())
	}
}

func reverse(in []byte) []byte {
	out := make([]byte, len(in))
	for i := range in {
		out[len(in)-1-i] = in[i]
	}
	return out
}

func TestConnectTLSPing(t *testing.T) {
	dir := t.TempDir()
	certPEM, keyPEM := mustSelfSigned(t)
	certPath := filepath.Join(dir, "server.crt")
	keyPath := filepath.Join(dir, "server.key")
	if err := os.WriteFile(certPath, certPEM, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
		t.Fatal(err)
	}
	cert, err := tls.LoadX509KeyPair(certPath, keyPath)
	if err != nil {
		t.Fatal(err)
	}
	server := startFakeServerTLS(t, 1, false, false, &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS13,
	})
	defer server.close()

	c, err := client.Connect(client.Config{
		Host:           "127.0.0.1",
		Port:           server.port(),
		ConnectTimeout: time.Second,
		RequestTimeout: time.Second,
		TLS: client.TLSConfig{
			Enable:     true,
			CAFile:     certPath,
			ServerName: "localhost",
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	payload := []byte("tls-ping")
	got, err := c.Ping(payload)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(payload) {
		t.Fatalf("got %q", got)
	}
}

func TestTLSConfigRequiresCertAndKeyPair(t *testing.T) {
	_, err := client.Connect(client.Config{
		Host: "127.0.0.1",
		Port: 1,
		TLS: client.TLSConfig{
			Enable:   true,
			CertFile: "only-cert.pem",
		},
	})
	if err == nil {
		t.Fatal("expected incomplete mTLS config to fail")
	}
}

func mustSelfSigned(t *testing.T) (certPEM, keyPEM []byte) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"localhost"},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	certPEM = pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyBytes, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	keyPEM = pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyBytes})
	return certPEM, keyPEM
}
