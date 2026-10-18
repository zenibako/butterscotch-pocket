package prep

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"image"
	"image/color"
	"image/gif"
	"image/jpeg"
	"image/png"
	"testing"
)

func qoiFixture(w, h int, ops ...byte) []byte {
	b := make([]byte, 12)
	copy(b, "fioq")
	binary.LittleEndian.PutUint16(b[4:], uint16(w))
	binary.LittleEndian.PutUint16(b[6:], uint16(h))
	binary.LittleEndian.PutUint32(b[8:], uint32(len(ops)))
	return append(b, ops...)
}
func TestImageStraightAlpha(t *testing.T) {
	im := image.NewNRGBA(image.Rect(0, 0, 2, 1))
	im.SetNRGBA(0, 0, color.NRGBA{240, 80, 32, 127})
	im.SetNRGBA(1, 0, color.NRGBA{10, 30, 90, 0})
	var b bytes.Buffer
	if err := png.Encode(&b, im); err != nil {
		t.Fatal(err)
	}
	p, w, h, err := decodeTextureImage(b.Bytes(), false)
	if err != nil {
		t.Fatal(err)
	}
	if w != 2 || h != 1 || !bytes.Equal(p, im.Pix) {
		t.Fatalf("RGBA changed: %v %dx%d", p, w, h)
	}
}
func TestImageCustomQOI(t *testing.T) {
	b := qoiFixture(7, 1, 0xff, 100, 110, 120, 130, 48, 0x40, 0x95, 0xc1, 0x23, 0xe0, 0x84, 0x21, 0xf8, 9)
	p, w, h, err := decodeTextureImage(b, false)
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{100, 110, 120, 130, 100, 110, 120, 130, 100, 110, 120, 130, 101, 111, 121, 130, 102, 113, 124, 130, 103, 114, 125, 131, 9, 114, 125, 131}
	if w != 7 || h != 1 || !bytes.Equal(p, want) {
		t.Fatalf("got %v %dx%d", p, w, h)
	}
}
func TestImageRejectMalformed(t *testing.T) {
	for _, b := range [][]byte{nil, qoiFixture(0, 1), qoiFixture(65535, 65535), qoiFixture(1, 1, 0xff, 1), qoiFixture(1, 1), qoiFixture(1, 1, 0x41)} {
		if _, _, _, err := decodeTextureImage(b, false); err == nil {
			t.Fatalf("accepted %x", b)
		}
	}
}

func TestImageCompressedQOIHeaders(t *testing.T) {
	stream, _ := hex.DecodeString("425a6839314159265359c8537b72000005d784f1004000400000100120a00080400000a000220693464f4100000300e96c4f7d7f272b45dc914e14243214dedc80")
	for _, newer := range []bool{false, true} {
		header := 8
		if newer {
			header = 12
		}
		b := make([]byte, header)
		copy(b, "2zoq")
		binary.LittleEndian.PutUint16(b[4:], 2)
		binary.LittleEndian.PutUint16(b[6:], 1)
		if newer {
			binary.LittleEndian.PutUint32(b[8:], 18)
		}
		b = append(b, stream...)
		p, w, h, err := decodeTextureImage(b, newer)
		if err != nil {
			t.Fatal(err)
		}
		if w != 2 || h != 1 || !bytes.Equal(p, []byte{200, 32, 90, 127, 200, 32, 90, 127}) {
			t.Fatalf("decoded %v %dx%d", p, w, h)
		}
		b[4] = 3
		if _, _, _, err = decodeTextureImage(b, newer); err == nil {
			t.Fatal("accepted inconsistent dimensions")
		}
	}
}
func TestImageJPEGAndGIF(t *testing.T) {
	im := image.NewNRGBA(image.Rect(0, 0, 2, 1))
	im.SetNRGBA(0, 0, color.NRGBA{200, 40, 80, 255})
	for _, format := range []string{"jpeg", "gif"} {
		var b bytes.Buffer
		var err error
		if format == "jpeg" {
			err = jpeg.Encode(&b, im, nil)
		} else {
			err = gif.Encode(&b, im, nil)
		}
		if err != nil {
			t.Fatal(err)
		}
		p, w, h, err := decodeTextureImage(b.Bytes(), false)
		if err != nil || w != 2 || h != 1 || len(p) != 8 {
			t.Fatalf("%s: %v %dx%d %v", format, p, w, h, err)
		}
	}
}

func TestImagePNG16PreservesStraightChannels(t *testing.T) {
	im := image.NewNRGBA64(image.Rect(0, 0, 2, 1))
	im.SetNRGBA64(0, 0, color.NRGBA64{R: 0xffff, G: 0x8000, B: 0x4000, A: 0})
	im.SetNRGBA64(1, 0, color.NRGBA64{R: 0x8000, G: 0x4000, B: 0x2000, A: 0x8000})
	var b bytes.Buffer
	if err := png.Encode(&b, im); err != nil {
		t.Fatal(err)
	}
	p, _, _, err := decodeTextureImage(b.Bytes(), false)
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{255, 128, 64, 0, 128, 64, 32, 128}
	if !bytes.Equal(p, want) {
		t.Fatalf("got %v want straight %v", p, want)
	}
}

// TXTR image extents include alignment bytes after the bzip2 stream.
func TestImageCompressedQOIAlignment(t *testing.T) {
	stream, err := hex.DecodeString("425a6839314159265359c8537b72000005d784f1004000400000100120a00080400000a000220693464f4100000300e96c4f7d7f272b45dc914e14243214dedc80")
	if err != nil {
		t.Fatal(err)
	}
	for _, newer := range []bool{false, true} {
		for _, padding := range []int{0, 1, 2, 73, 127} {
			header := 8
			if newer {
				header = 12
			}
			blob := make([]byte, header)
			copy(blob, "2zoq")
			binary.LittleEndian.PutUint16(blob[4:], 2)
			binary.LittleEndian.PutUint16(blob[6:], 1)
			if newer {
				binary.LittleEndian.PutUint32(blob[8:], 18)
			}
			blob = append(blob, stream...)
			blob = append(blob, make([]byte, padding)...)
			p, w, h, err := decodeTextureImage(blob, newer)
			if err != nil {
				t.Errorf("newer=%v padding=%d: %v", newer, padding, err)
				continue
			}
			if w != 2 || h != 1 || !bytes.Equal(p, []byte{200, 32, 90, 127, 200, 32, 90, 127}) {
				t.Fatalf("decoded %v %dx%d", p, w, h)
			}
			for removed := 1; removed <= 12; removed++ {
				truncated := append(append([]byte(nil), blob[:header+len(stream)-removed]...), make([]byte, padding)...)
				if _, _, _, err := decodeTextureImage(truncated, newer); err == nil {
					t.Fatalf("accepted stream missing %d bytes with %d padding", removed, padding)
				}
			}
			for _, corruptAt := range []int{10, len(stream) - 2} {
				corrupt := append([]byte(nil), blob...)
				corrupt[header+corruptAt] ^= 1
				if _, _, _, err := decodeTextureImage(corrupt, newer); err == nil {
					t.Fatalf("accepted corrupt stream at %d with %d padding", corruptAt, padding)
				}
			}
		}
	}
}
