package vorbis

import (
	"testing"
)

type boundsBits struct {
	b []byte
	p uint
}

func (w *boundsBits) put(v uint32, n uint) {
	for i := uint(0); i < n; i++ {
		if w.p/8 >= uint(len(w.b)) {
			w.b = append(w.b, 0)
		}
		w.b[w.p/8] |= byte((v>>i)&1) << (w.p % 8)
		w.p++
	}
}
func TestRejectEmptyCodebook(t *testing.T) {
	var w boundsBits
	w.put(codebookPattern, 24)
	w.put(1, 16)
	w.put(2, 24)
	w.put(0, 1)
	w.put(1, 1)
	w.put(0, 1)
	w.put(0, 1)
	w.put(0, 4)
	var c codebook
	if e := c.ReadFrom(newBitReader(w.b)); e == nil {
		t.Fatal("accepted empty sparse codebook")
	}
}
func TestRejectReversedResidue(t *testing.T) {
	var w boundsBits
	w.put(0, 16)
	w.put(32, 24)
	w.put(0, 24)
	w.put(0, 24)
	w.put(0, 6)
	w.put(0, 8)
	w.put(0, 3)
	w.put(0, 1)
	var x residue
	if e := x.ReadFrom(newBitReader(w.b)); e == nil {
		t.Fatal("accepted end before begin")
	}
}
func TestMissingHuffmanBranchTerminates(t *testing.T) {
	defer func() {
		if recover() == nil {
			t.Fatal("missing branch did not fail")
		}
	}()
	huffmanCode{0, 1}.Lookup(newBitReader([]byte{0}))
}

func TestLookup1ValuesExactRoots(t *testing.T) {
	for dimensions := uint32(1); dimensions <= 16; dimensions++ {
		for base := 1; base <= 64; base++ {
			entries := 1
			for i := uint32(0); i < dimensions; i++ {
				entries *= base
				if entries > 65536 {
					break
				}
			}
			if entries > 65536 {
				break
			}
			if got := lookup1Values(entries, dimensions); got != base {
				t.Fatalf("lookup1Values(%d,%d)=%d want %d", entries, dimensions, got, base)
			}
		}
	}
}
