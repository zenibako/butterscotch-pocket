package prep

import (
	"bytes"
	"compress/bzip2"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"io"
)

// Bound allocations before invoking decoders, including on wasm32 hosts.
const maxTexturePixels = 16 * 1024 * 1024

func textureDimensions(w, h int) error {
	if w <= 0 || h <= 0 || w > 65535 || h > 65535 || uint64(w)*uint64(h) > maxTexturePixels {
		return fmt.Errorf("unsupported texture dimensions %dx%d", w, h)
	}
	return nil
}
func decodeTextureImage(blob []byte, newer bool) ([]byte, int, int, error) {
	if len(blob) < 4 {
		return nil, 0, 0, fmt.Errorf("truncated texture image")
	}
	if string(blob[:4]) == "fioq" {
		return decodeTextureQOI(blob)
	}
	if string(blob[:4]) == "2zoq" {
		header := 8
		if newer {
			header = 12
		}
		if len(blob) < header {
			return nil, 0, 0, fmt.Errorf("truncated compressed QOI header")
		}
		w, h := int(binary.LittleEndian.Uint16(blob[4:])), int(binary.LittleEndian.Uint16(blob[6:]))
		if err := textureDimensions(w, h); err != nil {
			return nil, 0, 0, err
		}
		limit := int64(12 + w*h*5)
		raw, err := decodeTextureBzip2(blob[header:], limit)
		if err != nil {
			return nil, 0, 0, fmt.Errorf("QOI bzip2: %w", err)
		}
		if int64(len(raw)) > limit {
			return nil, 0, 0, fmt.Errorf("compressed QOI exceeds image bound")
		}
		p, qw, qh, err := decodeTextureQOI(raw)
		if err != nil {
			return nil, 0, 0, err
		}
		if qw != w || qh != h {
			return nil, 0, 0, fmt.Errorf("compressed QOI dimensions disagree")
		}
		return p, w, h, nil
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(blob))
	if err != nil {
		return nil, 0, 0, fmt.Errorf("texture image header: %w", err)
	}
	if err = textureDimensions(cfg.Width, cfg.Height); err != nil {
		return nil, 0, 0, err
	}
	im, _, err := image.Decode(bytes.NewReader(blob))
	if err != nil {
		return nil, 0, 0, fmt.Errorf("texture image: %w", err)
	}
	if im.Bounds().Dx() != cfg.Width || im.Bounds().Dy() != cfg.Height {
		return nil, 0, 0, fmt.Errorf("texture dimensions changed")
	}
	raw := make([]byte, cfg.Width*cfg.Height*4)
	bounds := im.Bounds()
	for y := 0; y < cfg.Height; y++ {
		for x := 0; x < cfg.Width; x++ {
			var c color.NRGBA
			if straight, ok := im.(*image.NRGBA64); ok {
				p := straight.NRGBA64At(bounds.Min.X+x, bounds.Min.Y+y)
				c = color.NRGBA{R: uint8(p.R >> 8), G: uint8(p.G >> 8), B: uint8(p.B >> 8), A: uint8(p.A >> 8)}
			} else {
				c = color.NRGBAModel.Convert(im.At(bounds.Min.X+x, bounds.Min.Y+y)).(color.NRGBA)
			}
			i := (y*cfg.Width + x) * 4
			raw[i], raw[i+1], raw[i+2], raw[i+3] = c.R, c.G, c.B, c.A
		}
	}
	return raw, cfg.Width, cfg.Height, nil
}

// decodeTextureBzip2 validates the complete stream, including both CRCs. TXTR
// blob extents can include alignment padding, whereas Go's reader expects any
// following bytes to begin another bzip2 stream. Its io.ByteReader input avoids
// read-ahead, so an invalid continuation consumes only one or two padding bytes.
// Retry those exact boundaries and require clean EOF; never suppress the initial
// error merely because enough QOI pixels were produced (that skips CRC checks).
func decodeTextureBzip2(data []byte, limit int64) ([]byte, error) {
	source := bytes.NewReader(data)
	raw, err := io.ReadAll(io.LimitReader(bzip2.NewReader(source), limit+1))
	if err == nil {
		return raw, nil
	}
	consumed := len(data) - source.Len()
	for _, trim := range []int{2, 1} {
		if trim > consumed {
			continue
		}
		end := consumed - trim
		// Only TXTR's zero alignment padding warrants this fallback.
		if len(bytes.Trim(data[end:], "\x00")) != 0 {
			continue
		}
		retry, retryErr := io.ReadAll(io.LimitReader(bzip2.NewReader(bytes.NewReader(data[:end])), limit+1))
		if retryErr == nil && int64(len(retry)) <= limit {
			return retry, nil
		}
	}
	return nil, err
}

func decodeTextureQOI(data []byte) ([]byte, int, int, error) {
	bad := func() ([]byte, int, int, error) { return nil, 0, 0, fmt.Errorf("malformed custom QOI") }
	if len(data) < 12 || string(data[:4]) != "fioq" {
		return bad()
	}
	w, h := int(binary.LittleEndian.Uint16(data[4:])), int(binary.LittleEndian.Uint16(data[6:]))
	if err := textureDimensions(w, h); err != nil {
		return nil, 0, 0, err
	}
	length := uint64(binary.LittleEndian.Uint32(data[8:]))
	if length > uint64(len(data)-12) {
		return bad()
	}
	ops := data[12 : 12+int(length)]
	raw := make([]byte, w*h*4)
	var index [64][4]byte
	px := [4]byte{0, 0, 0, 255}
	pos, run := 0, 0
	signed := func(v uint32, bits uint) byte { mask := uint32(1) << (bits - 1); return byte((v ^ mask) - mask) }
	for i := 0; i < w*h; i++ {
		if run > 0 {
			run--
		} else {
			if pos >= len(ops) {
				return bad()
			}
			b := ops[pos]
			pos++
			switch {
			case b&0xc0 == 0:
				px = index[b&63]
			case b&0xe0 == 0x40:
				run = int(b & 31)
			case b&0xe0 == 0x60:
				if pos >= len(ops) {
					return bad()
				}
				run = (int(b&31)<<8 | int(ops[pos])) + 32
				pos++
			case b&0xc0 == 0x80:
				px[0] += signed(uint32(b>>4&3), 2)
				px[1] += signed(uint32(b>>2&3), 2)
				px[2] += signed(uint32(b&3), 2)
			case b&0xe0 == 0xc0:
				if pos >= len(ops) {
					return bad()
				}
				v := uint32(b)<<8 | uint32(ops[pos])
				pos++
				px[0] += signed(v>>8&31, 5)
				px[1] += signed(v>>4&15, 4)
				px[2] += signed(v&15, 4)
			case b&0xf0 == 0xe0:
				if len(ops)-pos < 2 {
					return bad()
				}
				v := uint32(b)<<16 | uint32(ops[pos])<<8 | uint32(ops[pos+1])
				pos += 2
				px[0] += signed(v>>15&31, 5)
				px[1] += signed(v>>10&31, 5)
				px[2] += signed(v>>5&31, 5)
				px[3] += signed(v&31, 5)
			default:
				for channel, flag := range []byte{8, 4, 2, 1} {
					if b&flag != 0 {
						if pos >= len(ops) {
							return bad()
						}
						px[channel] = ops[pos]
						pos++
					}
				}
			}
			if run > w*h-i-1 {
				return bad()
			}
			index[(px[0]^px[1]^px[2]^px[3])&63] = px
		}
		copy(raw[4*i:], px[:])
	}
	return raw, w, h, nil
}
