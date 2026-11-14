package client

// Test-only helpers for client_test (same package visibility during `go test`).

func (c *Client) ResetWorkerConnection(worker uint32) {
	if int(worker) >= len(c.connections) {
		return
	}
	conn := c.connections[worker]
	conn.mu.Lock()
	defer conn.mu.Unlock()
	conn.reset()
}
