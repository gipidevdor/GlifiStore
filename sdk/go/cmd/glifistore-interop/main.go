// Command glifistore-interop is the Go helper for scripts/test-sdk-interop.sh.
package main

import (
	"bytes"
	"encoding/hex"
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/client"
	"github.com/gpicchiarelli/GlifiStore/sdk/go/protocol"
)

func main() {
	host := flag.String("host", "127.0.0.1", "server host")
	port := flag.Int("port", 0, "server port")
	keyHex := flag.String("key-hex", "", "key as hex")
	valueHex := flag.String("value-hex", "", "value as hex")
	dest := flag.String("dest", "", "UTF-8 destination path for backup")
	expireAtNs := flag.Uint64("expire-at-ns", 0, "absolute expire_at_ns")
	tlsEnable := flag.Bool("tls", false, "opt-in TLS 1.3")
	tlsCA := flag.String("tls-ca", "", "PEM CA / trust anchor")
	tlsCert := flag.String("tls-cert", "", "client certificate for mTLS")
	tlsKey := flag.String("tls-key", "", "client private key for mTLS")
	serverName := flag.String("server-name", "", "SNI / hostname verification name")
	insecure := flag.Bool("insecure-skip-verify", false, "lab escape: skip cert/hostname verify")
	burst := flag.Int("burst", 32, "PUT attempts for burst-expect-overloaded")
	flag.Parse()
	if *port == 0 || flag.NArg() != 1 || *burst < 1 || *burst > 10000 {
		fmt.Fprintln(os.Stderr, "usage: glifistore-interop --port N <put|get|erase|health|ready|stats|backup|pipeline-put-get|worker-pipelines-put-get|expect-not-found|expect-permission-denied|burst-expect-overloaded|expect-frame-limit> [tls flags...]")
		os.Exit(2)
	}
	command := flag.Arg(0)
	key, err := parseHex(*keyHex)
	if err != nil {
		fail(err)
	}
	value, err := parseHex(*valueHex)
	if err != nil {
		fail(err)
	}

	cfg := client.Config{
		Host: *host,
		Port: *port,
		TLS: client.TLSConfig{
			Enable:             *tlsEnable,
			CAFile:             *tlsCA,
			CertFile:           *tlsCert,
			KeyFile:            *tlsKey,
			ServerName:         *serverName,
			InsecureSkipVerify: *insecure,
		},
	}
	c, err := client.Connect(cfg)
	if err != nil {
		fail(err)
	}
	defer c.Close()

	switch command {
	case "put":
		result := c.Put(key, value, *expireAtNs)
		if !result.Committed() {
			fail(fmt.Errorf("put not committed: %v", result.Err))
		}
	case "get":
		got, err := c.Get(key)
		if err != nil {
			fail(err)
		}
		fmt.Println(hex.EncodeToString(got))
	case "erase":
		result := c.Erase(key)
		if !result.Committed() {
			fail(fmt.Errorf("erase not committed: %v", result.Err))
		}
	case "health":
		payload, err := c.Health()
		if err != nil {
			fail(err)
		}
		fmt.Print(string(payload))
	case "ready":
		payload, err := c.Ready()
		if err != nil {
			fail(err)
		}
		fmt.Print(string(payload))
	case "stats":
		payload, err := c.Stats()
		if err != nil {
			fail(err)
		}
		fmt.Print(string(payload))
	case "backup":
		if *dest == "" {
			fail(fmt.Errorf("backup requires --dest PATH"))
		}
		report, err := c.Backup(*dest)
		if err != nil {
			fail(err)
		}
		fmt.Print(string(report))
	case "pipeline-put-get":
		responses, err := c.ExecutePipeline([]client.PipelineRequest{
			{Opcode: client.PipelinePut, Key: key, Value: value},
			{Opcode: client.PipelineGet, Key: key},
		})
		if err != nil {
			fail(err)
		}
		if len(responses) != 2 || !responses[0].Succeeded() || !responses[1].Succeeded() {
			fail(fmt.Errorf("pipeline outcomes failed"))
		}
		if string(responses[1].Value) != string(value) {
			fail(fmt.Errorf("pipeline value mismatch"))
		}
		fmt.Println(hex.EncodeToString(responses[1].Value))
	case "worker-pipelines-put-get":
		workerCount := int(c.WorkerCount())
		keys := make([][]byte, workerCount)
		remaining := workerCount
		for candidate := 0; remaining > 0 && candidate < 1_000_000; candidate++ {
			probe := append(append([]byte{}, key...), []byte(fmt.Sprintf("-%d", candidate))...)
			owner, err := c.WorkerFor(probe)
			if err != nil {
				fail(err)
			}
			if int(owner) >= workerCount || keys[owner] != nil {
				continue
			}
			keys[owner] = probe
			remaining--
		}
		if remaining != 0 {
			fail(fmt.Errorf("failed to discover a key for every Worker"))
		}
		batches := make([][]client.PipelineRequest, workerCount)
		values := make([][]byte, workerCount)
		for worker, owned := range keys {
			payload := value
			if len(payload) == 0 {
				payload = []byte{byte('v'), byte('0' + (worker % 10))}
			}
			values[worker] = payload
			batches[worker] = []client.PipelineRequest{
				{Opcode: client.PipelinePut, Key: owned, Value: payload},
				{Opcode: client.PipelineGet, Key: owned},
			}
		}
		results, err := c.ExecuteWorkerPipelines(batches)
		if err != nil {
			fail(err)
		}
		if len(results) != workerCount {
			fail(fmt.Errorf("worker pipelines returned unexpected slot count"))
		}
		for worker, slot := range results {
			if len(slot) != 2 || !slot[0].Succeeded() || !slot[1].Succeeded() ||
				string(slot[1].Value) != string(values[worker]) {
				fail(fmt.Errorf("worker %d pipeline outcomes failed", worker))
			}
		}
		fmt.Println("ok")
	case "expect-not-found":
		_, err := c.Get(key)
		structured, ok := err.(*client.Error)
		if !ok || structured.Category != client.CategoryNotFound ||
			structured.Retryability != client.RetryNewAttempt {
			fail(fmt.Errorf("GET did not produce structured not_found: %v", err))
		}
	case "expect-permission-denied":
		result := c.Put(key, value, *expireAtNs)
		structured, ok := result.Err.(*client.Error)
		if result.Outcome != client.MutationRejected || !ok ||
			structured.Category != client.CategoryPermissionDenied ||
			structured.Retryability != client.RetryNever {
			fail(fmt.Errorf("PUT did not produce structured permission_denied: %v", result.Err))
		}
	case "burst-expect-overloaded":
		for index := 0; index < *burst; index++ {
			result := c.Put(key, value, *expireAtNs)
			structured, ok := result.Err.(*client.Error)
			if result.Outcome == client.MutationRejected && ok &&
				structured.Category == client.CategoryOverloaded &&
				structured.Retryability == client.RetryNever {
				return
			}
			if !result.Committed() {
				fail(fmt.Errorf("PUT produced an unexpected result before OVERLOADED: %v", result.Err))
			}
		}
		fail(fmt.Errorf("burst of %d PUTs did not observe OVERLOADED", *burst))
	case "expect-frame-limit":
		value := bytes.Repeat([]byte{0xA5}, protocol.MaxFrameBytes)
		result := c.Put([]byte("limit"), value, 0)
		structured, ok := result.Err.(*client.Error)
		if result.Outcome != client.MutationRejected || !ok ||
			structured.Category != client.CategoryInvalidArgument || structured.BytesSent != 0 ||
			structured.Retryability != client.RetryNever {
			fail(fmt.Errorf("oversized PUT did not produce the expected local rejection: %v", result.Err))
		}
	default:
		fail(fmt.Errorf("unknown command %q", command))
	}
}

func parseHex(text string) ([]byte, error) {
	cleaned := strings.Join(strings.Fields(text), "")
	if cleaned == "" {
		return nil, nil
	}
	if len(cleaned)%2 != 0 {
		return nil, fmt.Errorf("odd hex length")
	}
	return hex.DecodeString(cleaned)
}

func fail(err error) {
	fmt.Fprintf(os.Stderr, "error: %v\n", err)
	os.Exit(1)
}
