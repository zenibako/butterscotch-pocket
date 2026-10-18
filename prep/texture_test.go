package prep

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"testing"
)

func textureFORM(blobs ...[]byte) []byte {
	n := len(blobs)
	b := make([]byte, 20+4*n+8*n)
	copy(b, "FORM")
	copy(b[8:], "TXTR")
	binary.LittleEndian.PutUint32(b[16:], uint32(n))
	for i, blob := range blobs {
		entry := 20 + 4*n + 8*i
		binary.LittleEndian.PutUint32(b[20+4*i:], uint32(entry))
		if blob != nil {
			binary.LittleEndian.PutUint32(b[entry+4:], uint32(len(b)))
			b = append(b, blob...)
		}
	}
	binary.LittleEndian.PutUint32(b[4:], uint32(len(b)-8))
	binary.LittleEndian.PutUint32(b[12:], uint32(len(b)-16))
	return b
}
func TestTexturePackExact(t *testing.T) {
	b := textureFORM(qoiFixture(3, 1, 0xff, 255, 0, 0, 255, 0x41), nil, qoiFixture(1, 1, 0xff, 0, 0, 0, 0))
	got, err := TexturePack(b, nil)
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{'U', 'T', 'X', '2', 3, 0, 0, 0, 64, 0, 0, 0, 2, 0, 0, 0, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 56, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 128, 0, 252}
	if !bytes.Equal(got, want) {
		t.Fatalf("got %x want %x", got, want)
	}
}
func TestTexturePackEdges(t *testing.T) {
	got, err := TexturePack(textureFORM(qoiFixture(65, 65, 0x70, 0x60)), nil)
	if err != nil {
		t.Fatal(err)
	}
	if binary.LittleEndian.Uint32(got[12:]) != 4 {
		t.Fatal("missing clipped tiles")
	}
	for i, count := range []uint16{4096, 64, 64, 1} {
		offset := binary.LittleEndian.Uint32(got[24+i*8:])
		control := binary.LittleEndian.Uint16(got[offset:])
		if count >= 3 {
			if control != 0x8000|(count-1) {
				t.Fatalf("tile %d: %x", i, control)
			}
		} else if control != 0 {
			t.Fatal("bad single literal")
		}
	}
}
func TestTexturePackRejectMalformed(t *testing.T) {
	valid := textureFORM(qoiFixture(1, 1, 0xff, 0, 0, 0, 255))
	cases := [][]byte{nil, textureFORM()}
	for _, off := range []int{16, 20, 28} {
		b := bytes.Clone(valid)
		binary.LittleEndian.PutUint32(b[off:], 0xffffffff)
		cases = append(cases, b)
	}
	for _, b := range cases {
		if _, err := TexturePack(b, nil); err == nil {
			t.Fatalf("accepted malformed %x", b)
		}
	}
}

// Hashes were independently produced by the existing C mktexpack binary, not
// this encoder. The synthetic images contain no commercial game assets.
func TestTexturePackReferenceGolden(t *testing.T) {
	im := image.NewNRGBA(image.Rect(0, 0, 65, 65))
	alphas := []uint8{0, 127, 128, 255}
	for y := 0; y < 65; y++ {
		for x := 0; x < 65; x++ {
			im.SetNRGBA(x, y, color.NRGBA{uint8(x * 31), uint8(y * 17), uint8(x + y), alphas[(x+y)%4]})
		}
	}
	var pngBytes bytes.Buffer
	if err := png.Encode(&pngBytes, im); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name string
		data []byte
		hash string
	}{
		{"PNG", textureFORM(pngBytes.Bytes(), pngBytes.Bytes()), "1e3935c14a0fc2fc859f4a5e5f4ca332eacf627c45651bc3484d9ef769a0faee"},
		{"QOI", textureFORM(qoiFixture(7, 1, 255, 100, 110, 120, 130, 48, 64, 149, 193, 35, 224, 132, 33, 248, 9), nil, qoiFixture(65, 65, 112, 96)), "d36ff619e2270f04a3584e3976fa303c79fd9c373a64bf4cee33ce10155e81fc"},
	}
	for _, tt := range cases {
		t.Run(tt.name, func(t *testing.T) {
			p, err := TexturePack(tt.data, nil)
			if err != nil {
				t.Fatal(err)
			}
			if got := fmt.Sprintf("%x", sha256.Sum256(p)); got != tt.hash {
				t.Fatalf("C reference hash %s, Go hash %s", tt.hash, got)
			}
		})
	}
}
func TestTextureTruncationsReturnErrors(t *testing.T) {
	data := textureFORM(qoiFixture(3, 1, 255, 255, 0, 0, 255, 65))
	for n := 0; n < len(data); n++ {
		b := bytes.Clone(data[:n])
		if n >= 16 {
			binary.LittleEndian.PutUint32(b[4:], uint32(n-8))
			binary.LittleEndian.PutUint32(b[12:], uint32(n-16))
		}
		if _, err := TexturePack(b, nil); err == nil {
			t.Fatalf("accepted truncation at %d", n)
		}
	}
}

func TestTextureRejectHighBitPageCounts(t *testing.T) {
	for _, count := range []uint32{0x80000000, 0xffffffff} {
		b := textureFORM(qoiFixture(1, 1, 255, 0, 0, 0, 255))
		binary.LittleEndian.PutUint32(b[16:], count)
		if _, err := TexturePack(b, nil); err == nil {
			t.Fatalf("accepted count %x", count)
		}
	}
}
