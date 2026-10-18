package main

import (
	"bytes"
	"testing"
)

func TestRunRequiresFlags(t *testing.T) {
	for _, args := range [][]string{nil, {"--game", "unknown", "--source", "missing", "--output", "new"}, {"--bogus"}} {
		var out, err bytes.Buffer
		if run(args, &out, &err) == 0 || err.Len() == 0 {
			t.Fatal("expected actionable failure")
		}
	}
}
