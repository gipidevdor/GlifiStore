package client_test

import (
	"fmt"
	"os"
	"testing"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/client"
)

// Benchmarks against a live glifistored. Set GLIFISTORE_BENCH_PORT to enable.
func BenchmarkPutGetPipeline(b *testing.B) {
	port := os.Getenv("GLIFISTORE_BENCH_PORT")
	if port == "" {
		b.Skip("set GLIFISTORE_BENCH_PORT to run against a live server")
	}
	var p int
	if _, err := fmt.Sscanf(port, "%d", &p); err != nil || p <= 0 {
		b.Fatalf("bad GLIFISTORE_BENCH_PORT=%q", port)
	}
	c, err := client.Connect(client.Config{Port: p})
	if err != nil {
		b.Fatal(err)
	}
	defer c.Close()

	key := []byte("bench-key")
	value := []byte("bench-value-0123456789")
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		reqs := []client.PipelineRequest{
			{Opcode: client.PipelinePut, Key: key, Value: value},
			{Opcode: client.PipelineGet, Key: key},
		}
		responses, err := c.ExecutePipeline(reqs)
		if err != nil || len(responses) != 2 || !responses[1].Succeeded() {
			b.Fatalf("pipeline failed: %v %+v", err, responses)
		}
	}
}

func BenchmarkPutGetBatch(b *testing.B) {
	port := os.Getenv("GLIFISTORE_BENCH_PORT")
	if port == "" {
		b.Skip("set GLIFISTORE_BENCH_PORT to run against a live server")
	}
	var p int
	if _, err := fmt.Sscanf(port, "%d", &p); err != nil || p <= 0 {
		b.Fatalf("bad GLIFISTORE_BENCH_PORT=%q", port)
	}
	c, err := client.Connect(client.Config{Port: p})
	if err != nil {
		b.Fatal(err)
	}
	defer c.Close()

	keys := make([][]byte, c.WorkerCount())
	for candidate := 0; ; candidate++ {
		key := []byte(fmt.Sprintf("batch-bench-%012d", candidate))
		owner, err := c.WorkerFor(key)
		if err != nil {
			b.Fatal(err)
		}
		if keys[owner] == nil {
			keys[owner] = key
		}
		complete := true
		for _, found := range keys {
			complete = complete && found != nil
		}
		if complete {
			break
		}
	}

	value := []byte("batch-bench-value-0123456789")
	requests := make([]client.PipelineRequest, 0, len(keys)*16)
	for pair := 0; pair < 8; pair++ {
		for _, key := range keys {
			requests = append(requests,
				client.PipelineRequest{Opcode: client.PipelinePut, Key: key, Value: value},
				client.PipelineRequest{Opcode: client.PipelineGet, Key: key},
			)
		}
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		responses, err := c.ExecuteBatch(requests)
		if err != nil || len(responses) != len(requests) {
			b.Fatalf("batch failed: %v", err)
		}
		for _, response := range responses {
			if !response.Succeeded() {
				b.Fatalf("batch slot failed: %+v", response)
			}
		}
	}
}
