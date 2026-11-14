// Command glifistore-version prints the official Go client version for packaging checks.
package main

import (
	"fmt"

	"github.com/gpicchiarelli/GlifiStore/sdk/go/client"
)

func main() {
	fmt.Print(client.Version)
}
