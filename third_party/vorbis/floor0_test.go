package vorbis

import (
	"fmt"
	"math"
	"testing"
)

// Golden maps and gains use the equations in the Xiph Vorbis I specification,
// sections 6.2.2 and 6.2.3, evaluated independently in double precision with
// float32 LSP coefficients. No encoded audio fixture is needed.
// https://xiph.org/vorbis/doc/Vorbis_I_spec.html
func TestFloor0BarkMap(t *testing.T) {
	for _, tc := range []struct {
		rate, size uint16
		want       []int
	}{
		{8000, 64, []int{0, 8, 17, 24, 31, 36, 41, 45, 48, 51, 53, 55, 57, 59, 61, 62}},
		{48000, 256, []int{0, 157, 196, 217, 230, 239, 245, 251}},
		{8000, 1, []int{0}},
	} {
		f := floor0{rate: tc.rate, barkMapSize: tc.size}
		n := uint32(len(tc.want))
		for i, want := range tc.want {
			if got := f.mapResult(uint32(i), n); got != want {
				t.Errorf("rate=%d size=%d bin=%d: got %d, want %d", tc.rate, tc.size, i, got, want)
			}
		}
		if got := f.mapResult(n, n); got != -1 {
			t.Errorf("end sentinel = %d, want -1", got)
		}
	}
}

func TestFloor0Synthesis(t *testing.T) {
	bins := []int{0, 1, 4, 8, 15}
	for _, tc := range []struct {
		coefficients []float32
		gains        []float64
	}{
		{[]float32{.7}, []float64{.04168561101, .008303367533, .002041843254, .001711142948, .001644796343}},
		{[]float32{.5, 1.4}, []float64{.03597132117, .01727489755, .002062597079, .001542401034, .001456018654}},
		{[]float32{.4, 1.3, 2.4}, []float64{.02448135428, .005730278324, .001839403529, .001886635786, .002366063418}},
		{[]float32{.3, .9, 1.8, 2.7}, []float64{.05464718118, .006904780865, .001690455829, .00175606634, .003793660784}},
	} {
		t.Run(fmt.Sprintf("order%d", len(tc.coefficients)), func(t *testing.T) {
			defer func() {
				if r := recover(); r != nil {
					t.Fatalf("valid floor panicked: %v", r)
				}
			}()
			f := floor0{order: uint8(len(tc.coefficients)), rate: 8000, barkMapSize: 64, amplitudeBits: 6, amplitudeOffset: 60}
			out := make([]float32, 16)
			for i := range out {
				out[i] = .25
				if i%2 == 1 {
					out[i] = -.5
				}
			}
			f.Apply(out, floor0Data{amplitude: 8, coefficients: tc.coefficients})
			for j, i := range bins {
				residue := .25
				if i%2 == 1 {
					residue = -.5
				}
				want := residue * tc.gains[j]
				got := float64(out[i])
				if math.IsNaN(got) || math.IsInf(got, 0) || math.Abs(got-want) > math.Abs(want)*1e-6 {
					t.Errorf("bin %d: got %.10g, want %.10g", i, got, want)
				}
			}
		})
	}
}

func TestFloor0CoefficientAccumulation(t *testing.T) {
	for _, tc := range []struct {
		vector, want []float32
	}{
		{[]float32{.25}, []float32{.25, .5, .75, 1}},
		{[]float32{.125, .25}, []float32{.125, .25, .375, .5, .625}},
	} {
		f := floor0{order: uint8(len(tc.want)), amplitudeBits: 6, bookList: []uint8{0}}
		var packet boundsBits
		packet.put(8, 6) // amplitude
		packet.put(0, 1) // floor book number
		for i := 0; i < len(tc.want); i += len(tc.vector) {
			packet.put(0, 1) // single-entry Huffman book
		}
		books := []codebook{{dimensions: uint32(len(tc.vector)), entries: huffmanCode{1, 1}, values: tc.vector}}
		got := f.Decode(newBitReader(packet.b), books, 16).(floor0Data)
		for i, want := range tc.want {
			if got.coefficients[i] != want {
				t.Errorf("dimension=%d coefficient=%d: got %g, want %g", len(tc.vector), i, got.coefficients[i], want)
			}
		}
	}
}
