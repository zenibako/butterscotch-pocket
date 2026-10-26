//go:build wasip1

package main

import (
	"fmt"
	"strings"

	"github.com/extism/go-pdk"
	"github.com/zenibako/butterscotch-pocket/src/openfpga/tools/pocket-data/plugin"
)

//go:wasmimport extism:host/user print_msg
func printMsg(uint64) uint64

var audioTracks int

func progress(message string) {
	// Pocket Sync appends every message to a flexbox log. Hundreds of rows
	// can shrink its menu controls out of view, so keep GUI progress compact.
	if strings.HasPrefix(message, "Converted texture page ") {
		return
	}
	if message == "Converting audio" {
		audioTracks = 0
	}
	if strings.HasPrefix(message, "Audio: ") {
		audioTracks++
		if audioTracks != 1 && audioTracks%64 != 0 {
			return
		}
		message = fmt.Sprintf("Audio: processing track %d", audioTracks)
	}
	memory := pdk.AllocateString(message + "\n")
	defer memory.Free()
	printMsg(memory.Offset())
}

var session = plugin.New(pocketRoot(), progress)

func output(data []byte, err error) int32 {
	if err != nil {
		pdk.SetError(err)
		return 1
	}
	pdk.Output(data)
	return 0
}

//go:wasmexport start
func start() int32 { return output(session.Start()) }

//go:wasmexport handle_response
func handleResponse() int32 { return output(session.Handle(pdk.Input())) }

func main() {}

// TinyGo normalizes Pocket Sync's relative preopens into absolute guest paths.
// Keep the mount explicit instead of depending on which preopen becomes cwd.
func pocketRoot() string { return "/pocket" }
