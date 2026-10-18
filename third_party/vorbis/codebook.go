package vorbis

import (
	"errors"
	"math"
)

const codebookPattern = 0x564342 //"BCV"

type codebook struct {
	dimensions uint32
	entries    huffmanCode
	values     []float32
}

func (c *codebook) ReadFrom(r *bitReader) error {
	if r.Read32(24) != codebookPattern {
		return errors.New("vorbis: invalid codebook sync")
	}
	c.dimensions = r.Read32(16)
	numEntries := r.Read32(24)
	if c.dimensions == 0 || c.dimensions > 256 || numEntries == 0 || numEntries > 65536 || uint64(c.dimensions)*uint64(numEntries) > 1<<20 {
		return errors.New("vorbis: codebook allocation exceeds limit")
	}
	treeSize := numEntries*2 - 2
	if treeSize < 2 {
		treeSize = 2
	}
	entries := newHuffmanBuilder(treeSize)
	used := uint32(0)
	ordered := r.ReadBool()
	if !ordered {
		sparse := r.ReadBool()
		for i := uint32(0); i < numEntries; i++ {
			if !sparse || r.ReadBool() {
				entries.Put(i, r.Read8(5)+1)
				used++
			}
		}
	} else {
		currentEntry := uint32(0)
		currentLength := r.Read8(5) + 1
		for currentEntry < numEntries {
			if currentLength > 32 {
				return errors.New("vorbis: invalid ordered codebook")
			}
			num := r.Read32(ilog(int(numEntries - currentEntry)))
			if num > numEntries-currentEntry {
				return errors.New("vorbis: invalid ordered entries")
			}
			used += num
			for i := currentEntry; i < currentEntry+num; i++ {
				entries.Put(i, currentLength)
			}
			currentEntry += num
			currentLength++
		}
	}
	if used == 0 {
		return errors.New("vorbis: empty codebook")
	}
	c.entries = entries.code

	lookupType := r.Read8(4)
	if lookupType == 0 {
		return nil
	}
	if lookupType > 2 {
		return errors.New("vorbis: invalid codebook lookup type")
	}
	minimumValue := float32Unpack(r.Read32(32))
	deltaValue := float32Unpack(r.Read32(32))
	valueBits := r.Read8(4) + 1
	sequenceP := r.ReadBool()
	var multiplicands []uint32
	if lookupType == 1 {
		multiplicands = make([]uint32, lookup1Values(int(numEntries), c.dimensions))
	} else {
		multiplicands = make([]uint32, int(numEntries)*int(c.dimensions))
	}
	for i := range multiplicands {
		multiplicands[i] = r.Read32(uint(valueBits))
	}
	c.values = make([]float32, numEntries*c.dimensions)
	for entry := 0; entry < int(numEntries); entry++ {
		index := entry * int(c.dimensions)
		if lookupType == 1 {
			last := float32(0)
			indexDivisor := 1
			for i := 0; i < int(c.dimensions); i++ {
				multiplicandOffset := (entry / indexDivisor) % len(multiplicands)
				c.values[index+i] = float32(float32(multiplicands[multiplicandOffset])*deltaValue) + minimumValue + last
				if sequenceP {
					last = c.values[index+i]
				}
				indexDivisor *= len(multiplicands)
			}
		} else if lookupType == 2 {
			last := float32(0)
			for i := 0; i < int(c.dimensions); i++ {
				c.values[index+i] = float32(float32(multiplicands[index+i])*deltaValue) + minimumValue + last
				if sequenceP {
					last = c.values[index+i]
				}
			}
		}
	}
	return nil
}

func (c *codebook) DecodeScalar(r *bitReader) uint32 {
	return c.entries.Lookup(r)
}

func (c *codebook) DecodeVector(r *bitReader) []float32 {
	index := c.entries.Lookup(r) * c.dimensions
	return c.values[index : index+c.dimensions]
}

func ilog(x int) uint {
	var r uint
	for x > 0 {
		r++
		x >>= 1
	}
	return r
}

func lookup1Values(entries int, dim uint32) int {
	// lookup1_values is an integer root. Floating-point Pow can land below
	// a perfect power (for example cbrt(8)), consuming the wrong number of bits.
	low, high := 1, entries
	for low < high {
		candidate := (low + high + 1) / 2
		product := uint64(1)
		for d := uint32(0); d < dim; d++ {
			product *= uint64(candidate)
			if product > uint64(entries) {
				break
			}
		}
		if product <= uint64(entries) {
			low = candidate
		} else {
			high = candidate - 1
		}
	}
	return low
}

func float32Unpack(x uint32) float32 {
	mantissa := float64(x & 0x1fffff)
	if x&0x80000000 != 0 {
		mantissa = -mantissa
	}
	exponent := (x & 0x7fe00000) >> 21
	return float32(math.Ldexp(mantissa, int(exponent)-788))
}
