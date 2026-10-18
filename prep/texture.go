package prep

import (
	"encoding/binary"
	"fmt"
	"sort"
)

const textureTileSize = 64
const maxTexturePackBytes = 256 * 1024 * 1024
const maxTexturePages = 65536

// TexturePack converts TXTR pages one at a time into the renderer's UTX2 format.
func TexturePack(data []byte, progress func(string)) ([]byte, error) {
	c, err := Parse(data)
	if err != nil {
		return nil, err
	}
	txtr, err := c.Chunk("TXTR")
	if err != nil {
		return nil, err
	}
	if len(txtr) < 4 {
		return nil, fmt.Errorf("truncated TXTR count")
	}
	rawCount := uint64(binary.LittleEndian.Uint32(txtr))
	if rawCount == 0 || rawCount > maxTexturePages || rawCount > uint64((len(txtr)-4)/4) {
		return nil, fmt.Errorf("invalid TXTR page count")
	}
	count := int(rawCount)
	// Locate the payload using the container's original slice, without relying on a private parser field.
	txtrStart := 0
	for pos := 8; pos+8 <= len(data); {
		n := int(binary.LittleEndian.Uint32(data[pos+4:]))
		if string(data[pos:pos+4]) == "TXTR" {
			txtrStart = pos + 8
			break
		}
		pos += 8 + n
	}
	txtrEnd := txtrStart + len(txtr)
	entrySize := uint32(8)
	if count >= 2 {
		a, b := binary.LittleEndian.Uint32(txtr[4:]), binary.LittleEndian.Uint32(txtr[8:])
		if b < a {
			return nil, fmt.Errorf("invalid TXTR entry order")
		}
		entrySize = b - a
	}
	if entrySize < 8 || entrySize > 64 {
		return nil, fmt.Errorf("unsupported TXTR entry size %d", entrySize)
	}
	offsets := make([]int, count)
	sorted := make([]int, 0, count)
	for i := range offsets {
		entry := uint64(binary.LittleEndian.Uint32(txtr[4+4*i:]))
		if entry < uint64(txtrStart+4+4*count) || entry+uint64(entrySize) > uint64(txtrEnd) {
			return nil, fmt.Errorf("TXTR page %d entry outside chunk", i)
		}
		off := int(binary.LittleEndian.Uint32(data[int(entry)+int(entrySize)-4:]))
		if off != 0 && (off < txtrStart+4+4*count || off >= txtrEnd) {
			return nil, fmt.Errorf("TXTR page %d blob outside chunk", i)
		}
		offsets[i] = off
		if off != 0 {
			sorted = append(sorted, off)
		}
	}
	sort.Ints(sorted)
	pages := make([]byte, count*8)
	tiles := make([]byte, 0)
	payload := make([]byte, 0)
	tileCount := 0
	for i, off := range offsets {
		if off == 0 {
			continue
		}
		end := txtrEnd
		j := sort.SearchInts(sorted, off+1)
		if j < len(sorted) {
			end = sorted[j]
		}
		rgba, w, h, e := decodeTextureImage(data[off:end], entrySize >= 16)
		if e != nil && entrySize == 16 {
			rgba, w, h, e = decodeTextureImage(data[off:end], false)
		}
		if e != nil {
			return nil, fmt.Errorf("texture page %d: %w", i, e)
		}
		binary.LittleEndian.PutUint16(pages[i*8:], uint16(w))
		binary.LittleEndian.PutUint16(pages[i*8+2:], uint16(h))
		binary.LittleEndian.PutUint32(pages[i*8+4:], uint32(tileCount))
		for y := 0; y < h; y += textureTileSize {
			for x := 0; x < w; x += textureTileSize {
				tw, th := min(textureTileSize, w-x), min(textureTileSize, h-y)
				pixels := make([]uint16, tw*th)
				empty := true
				for row := 0; row < th; row++ {
					for col := 0; col < tw; col++ {
						p := ((y+row)*w + x + col) * 4
						v := uint16(rgba[p+2]>>3) | uint16(rgba[p+1]>>3)<<5 | uint16(rgba[p]>>3)<<10 | uint16(rgba[p+3]>>7)<<15
						pixels[row*tw+col] = v
						empty = empty && v == 0
					}
				}
				if 16+len(pages)+len(tiles)+8+len(payload) > maxTexturePackBytes {
					return nil, fmt.Errorf("texture pack exceeds %d bytes", maxTexturePackBytes)
				}
				record := make([]byte, 8)
				if !empty {
					encoded := encodeTextureTile(pixels)
					if len(payload)+len(encoded)+16+len(pages)+len(tiles)+8 > maxTexturePackBytes {
						return nil, fmt.Errorf("texture pack exceeds %d bytes", maxTexturePackBytes)
					}
					binary.LittleEndian.PutUint32(record, uint32(len(payload)))
					binary.LittleEndian.PutUint32(record[4:], uint32(len(encoded)))
					payload = append(payload, encoded...)
				}
				tiles = append(tiles, record...)
				tileCount++
			}
		}
		if progress != nil {
			progress(fmt.Sprintf("Converted texture page %d/%d", i+1, count))
		}
	}
	start := 16 + len(pages) + len(tiles)
	if start+len(payload) > maxTexturePackBytes {
		return nil, fmt.Errorf("texture pack exceeds bound")
	}
	for i := 0; i < len(tiles); i += 8 {
		if binary.LittleEndian.Uint32(tiles[i+4:]) != 0 {
			binary.LittleEndian.PutUint32(tiles[i:], uint32(start)+binary.LittleEndian.Uint32(tiles[i:]))
		}
	}
	out := make([]byte, 16, start+len(payload))
	copy(out, "UTX2")
	binary.LittleEndian.PutUint32(out[4:], uint32(count))
	binary.LittleEndian.PutUint32(out[8:], textureTileSize)
	binary.LittleEndian.PutUint32(out[12:], uint32(tileCount))
	out = append(out, pages...)
	out = append(out, tiles...)
	return append(out, payload...), nil
}
func encodeTextureTile(pixels []uint16) []byte {
	out := make([]byte, 0, len(pixels)*2+2)
	word := func(v uint16) { out = append(out, byte(v), byte(v>>8)) }
	for i := 0; i < len(pixels); {
		run := 1
		for i+run < len(pixels) && pixels[i+run] == pixels[i] && run < 32768 {
			run++
		}
		if run >= 3 {
			word(0x8000 | uint16(run-1))
			word(pixels[i])
			i += run
			continue
		}
		start := i
		for i < len(pixels) && i-start < 32768 {
			if i+2 < len(pixels) && pixels[i] == pixels[i+1] && pixels[i] == pixels[i+2] {
				break
			}
			i++
		}
		word(uint16(i - start - 1))
		for _, v := range pixels[start:i] {
			word(v)
		}
	}
	return out
}
