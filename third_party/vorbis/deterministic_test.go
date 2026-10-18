package vorbis

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math"
	"testing"
)

func TestIMDCTDeterministicRounding(t *testing.T) {
	const n = 256
	var lookup imdctLookup
	generateIMDCTLookup(n, &lookup)
	in := make([]float32, n/2)
	var state uint32 = 1
	for i := range in {
		state ^= state << 13
		state ^= state >> 17
		state ^= state << 5
		in[i] = float32(int16(state)) / 32768
	}
	out := make([]float32, n)
	imdct(&lookup, in, out)
	encoded := make([]byte, n*4)
	for i, v := range out {
		binary.LittleEndian.PutUint32(encoded[i*4:], math.Float32bits(v))
	}
	got := fmt.Sprintf("%x", sha256.Sum256(encoded))
	t.Logf("IMDCT checksum %s", got)
	const want = "12e34059db33d8893b5c5af2f2073a9513a8e558ae19ff46c044c6377c4f8642"
	if got != want {
		t.Fatalf("IMDCT checksum %s, want %s", got, want)
	}
}
