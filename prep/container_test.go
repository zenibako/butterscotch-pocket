package prep

import (
	"encoding/binary"
	"testing"
)

func form(parts ...[]byte) []byte {
	b := []byte("FORM\x00\x00\x00\x00")
	for _, p := range parts {
		b = append(b, p...)
	}
	binary.LittleEndian.PutUint32(b[4:], uint32(len(b)-8))
	return b
}
func chunk(name string, data []byte) []byte {
	b := make([]byte, 8)
	copy(b, name)
	binary.LittleEndian.PutUint32(b[4:], uint32(len(data)))
	return append(b, data...)
}
func TestContainer(t *testing.T) {
	b := form(chunk("GEN8", []byte{0, 16}), chunk("STRG", []byte("hello\x00")))
	c, e := Parse(b)
	if e != nil {
		t.Fatal(e)
	}
	p, e := c.Chunk("GEN8")
	if e != nil || len(p) != 2 || p[1] != 16 {
		t.Fatalf("chunk %v %v", p, e)
	}
	s, e := c.String(26)
	if e != nil || s != "hello" {
		t.Fatalf("string %q %v", s, e)
	}
	for _, bad := range [][]byte{nil, b[:len(b)-1], append(append([]byte{}, b...), 0), form(chunk("AAAA", nil), chunk("AAAA", nil))} {
		if _, e := Parse(bad); e == nil {
			t.Fatalf("accepted bad container %x", bad)
		}
	}
	if _, e := c.U32(-1); e == nil {
		t.Fatal("negative read")
	}
	if _, e := c.U32(len(b) - 2); e == nil {
		t.Fatal("short read")
	}
	if _, e := c.String(^uint32(0)); e == nil {
		t.Fatal("bad string")
	}
}
