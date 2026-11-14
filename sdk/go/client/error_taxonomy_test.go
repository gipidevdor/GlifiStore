package client

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/protocol"
)

type taxonomyFixture struct {
	Cases []struct {
		ID                   string `json:"id"`
		WireStatus           uint16 `json:"wire_status"`
		Category             string `json:"category"`
		ReadRetryability     string `json:"read_retryability"`
		MutationOutcome      string `json:"mutation_outcome"`
		MutationRetryability string `json:"mutation_retryability"`
		Unhealthy            bool   `json:"unhealthy"`
	} `json:"cases"`
}

func loadTaxonomyFixture(t *testing.T) taxonomyFixture {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	candidates := []string{
		filepath.Join(filepath.Dir(file), "..", "testdata", "error_taxonomy_v1.json"),
		filepath.Join(filepath.Dir(file), "..", "..", "..", "tests", "fixtures", "error_taxonomy_v1.json"),
	}
	var raw []byte
	var err error
	for _, path := range candidates {
		raw, err = os.ReadFile(path)
		if err == nil {
			break
		}
	}
	if err != nil {
		t.Fatalf("error_taxonomy_v1.json: %v", err)
	}
	var fixture taxonomyFixture
	if err := json.Unmarshal(raw, &fixture); err != nil {
		t.Fatalf("decode fixture: %v", err)
	}
	return fixture
}

func TestErrorTaxonomyWireStatusMatrix(t *testing.T) {
	fixture := loadTaxonomyFixture(t)
	for _, caseItem := range fixture.Cases {
		caseItem := caseItem
		t.Run(caseItem.ID, func(t *testing.T) {
			err := statusError(protocol.Status(caseItem.WireStatus))
			if string(err.Category) != caseItem.Category {
				t.Fatalf("category=%q want %q", err.Category, caseItem.Category)
			}
			if err.WireStatus == nil || uint16(*err.WireStatus) != caseItem.WireStatus {
				t.Fatalf("wire_status=%v want %d", err.WireStatus, caseItem.WireStatus)
			}
			if string(err.Retryability) != caseItem.ReadRetryability {
				t.Fatalf("read retryability=%q want %q", err.Retryability, caseItem.ReadRetryability)
			}

			outcome := MutationRejected
			if caseItem.MutationOutcome == "indeterminate" {
				outcome = MutationIndeterminate
			} else if caseItem.MutationOutcome == "committed" {
				outcome = MutationCommitted
			}
			mutated := err.withMutation(outcome).withBytesSent(1)
			if string(mutated.Retryability) != caseItem.MutationRetryability {
				t.Fatalf("mutation retryability=%q want %q", mutated.Retryability, caseItem.MutationRetryability)
			}
			wantUnhealthy := caseItem.WireStatus == uint16(protocol.StatusWrongOwner) ||
				caseItem.WireStatus == uint16(protocol.StatusNotBound)
			if caseItem.Unhealthy != wantUnhealthy {
				t.Fatalf("unhealthy=%v want %v", caseItem.Unhealthy, wantUnhealthy)
			}
		})
	}
}

func TestIndeterminateEnrichIgnoresZeroBytesSent(t *testing.T) {
	// Receive-after-send paths historically omitted bytes_sent; transport+0 must not
	// advertise same_request when mutation_outcome is already indeterminate.
	err := transport("socket closed").withMutation(MutationIndeterminate)
	if err.BytesSent != 0 {
		t.Fatalf("bytes_sent=%d", err.BytesSent)
	}
	if err.Retryability != RetryReconcileFirst {
		t.Fatalf("retryability=%s after withMutation", err.Retryability)
	}
	err = err.withBytesSent(0)
	if err.Retryability != RetryReconcileFirst {
		t.Fatalf("retryability=%s after withBytesSent(0)", err.Retryability)
	}
}
