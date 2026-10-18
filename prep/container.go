// Package prep prepares privately owned GameMaker data for Butterscotch Pocket.
// Its conversion path uses no native libraries or child processes.
package prep

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"unicode/utf8"
)

// MaxInputBytes bounds individual input files and buffers on wasm32.
const MaxInputBytes = 512 << 20

type Container struct {
	Data   []byte
	chunks map[string][]byte
}

func Parse(data []byte) (*Container, error) {
	if len(data) < 8 || len(data) > MaxInputBytes || string(data[:4]) != "FORM" || uint64(binary.LittleEndian.Uint32(data[4:])) != uint64(len(data)-8) {
		return nil, fmt.Errorf("not a complete GameMaker FORM data file (maximum %d MiB)", MaxInputBytes>>20)
	}
	c := &Container{Data: data, chunks: map[string][]byte{}}
	for pos := 8; pos < len(data); {
		if len(data)-pos < 8 {
			return nil, fmt.Errorf("truncated chunk header")
		}
		name := string(data[pos : pos+4])
		n := uint64(binary.LittleEndian.Uint32(data[pos+4:]))
		if n > uint64(len(data)-pos-8) {
			return nil, fmt.Errorf("truncated %s chunk", name)
		}
		if _, exists := c.chunks[name]; exists {
			return nil, fmt.Errorf("duplicate %s chunk", name)
		}
		end := pos + 8 + int(n)
		c.chunks[name] = data[pos+8 : end]
		pos = end
	}
	return c, nil
}
func (c *Container) Chunk(name string) ([]byte, error) {
	b, ok := c.chunks[name]
	if !ok {
		return nil, fmt.Errorf("missing %s chunk", name)
	}
	return b, nil
}
func (c *Container) U32(offset int) (uint32, error) {
	if offset < 0 || offset > len(c.Data)-4 {
		return 0, fmt.Errorf("invalid 32-bit offset %d", offset)
	}
	return binary.LittleEndian.Uint32(c.Data[offset:]), nil
}
func (c *Container) String(offset uint32) (string, error) {
	if offset == 0 {
		return "", nil
	}
	if uint64(offset) >= uint64(len(c.Data)) {
		return "", fmt.Errorf("invalid string offset %d", offset)
	}
	b := c.Data[int(offset):]
	n := bytes.IndexByte(b, 0)
	if n < 0 || !utf8.Valid(b[:n]) {
		return "", fmt.Errorf("invalid UTF-8 string at %d", offset)
	}
	return string(b[:n]), nil
}
