package client

import (
	"testing"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/protocol"
)

func TestOverloadedRetryabilityIsNever(t *testing.T) {
	err := statusError(protocol.StatusOverloaded)
	if err.Category != CategoryOverloaded {
		t.Fatalf("category=%v", err.Category)
	}
	if err.Retryability != RetryNever {
		t.Fatalf("retryability=%v want never", err.Retryability)
	}
	if got := retryabilityFor(CategoryOverloaded, false, false); got != RetryNever {
		t.Fatalf("retryabilityFor=%v want never", got)
	}
}
