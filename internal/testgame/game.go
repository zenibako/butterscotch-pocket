// Package testgame builds tiny original fixtures without commercial game data.
package testgame

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
)

func u32(n int) []byte         { b := make([]byte, 4); binary.LittleEndian.PutUint32(b, uint32(n)); return b }
func wrap(parts []byte) []byte { return append(append([]byte("FORM"), u32(len(parts))...), parts...) }
func Game(version byte, group bool) (data, bank []byte) {
	parts := []byte{}
	add := func(name string, b []byte) {
		parts = append(parts, []byte(name)...)
		parts = append(parts, u32(len(b))...)
		parts = append(parts, b...)
	}
	add("GEN8", []byte{0, version, 0, 0, 0, 0, 0, 0})
	img := image.NewNRGBA(image.Rect(0, 0, 65, 2))
	for y := 0; y < 2; y++ {
		for x := 0; x < 65; x++ {
			img.SetNRGBA(x, y, color.NRGBA{uint8(x * 3), uint8(y * 80), 255, 255})
		}
	}
	var buf bytes.Buffer
	_ = png.Encode(&buf, img)
	offset := 8 + len(parts)
	tx := append(u32(1), u32(offset+16)...)
	tx = append(tx, u32(0)...)
	tx = append(tx, u32(offset+24)...)
	tx = append(tx, buf.Bytes()...)
	add("TXTR", tx)
	offset = 8 + len(parts)
	entry := offset + 16
	fields := []int{entry + 36, 3, 0, 0, 0, 0, 0, 0, 0}
	if group {
		fields[7] = 1
	}
	sond := append(u32(1), u32(entry)...)
	for _, n := range fields {
		sond = append(sond, u32(n)...)
	}
	sond = append(sond, []byte("tone\x00")...)
	add("SOND", sond)
	pcm := []byte{}
	for i := 0; i < 128; i++ {
		pcm = append(pcm, byte(i*3), byte(i))
	}
	wav := []byte("RIFF")
	wav = append(wav, u32(36+len(pcm))...)
	wav = append(wav, []byte("WAVEfmt ")...)
	wav = append(wav, u32(16)...)
	wav = append(wav, 1, 0, 1, 0)
	wav = append(wav, u32(32000)...)
	wav = append(wav, u32(64000)...)
	wav = append(wav, 2, 0, 16, 0)
	wav = append(wav, []byte("data")...)
	wav = append(wav, u32(len(pcm))...)
	wav = append(wav, pcm...)
	audo := func(offset int) []byte {
		b := append(u32(1), u32(offset+16)...)
		b = append(b, u32(len(wav))...)
		return append(b, wav...)
	}
	if group {
		add("AUDO", u32(0))
	} else {
		add("AUDO", audo(8+len(parts)))
	}
	bankParts := append([]byte("AUDO"), u32(len(wav)+12)...)
	bankParts = append(bankParts, audo(8)...)
	return wrap(parts), wrap(bankParts)
}

func Write(root, game string) error {
	if err := os.MkdirAll(root, 0755); err != nil {
		return err
	}
	group := game == "deltarune"
	version := byte(16)
	if group {
		version = 17
	}
	data, bank := Game(version, group)
	if err := os.WriteFile(filepath.Join(root, "data.win"), data, 0644); err != nil {
		return err
	}
	if group {
		if err := os.WriteFile(filepath.Join(root, "audiogroup1.dat"), bank, 0644); err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Join(root, "lang"), 0755); err != nil {
			return err
		}
		return os.WriteFile(filepath.Join(root, "lang/lang_en.json"), []byte("{\"hello\":\"Hello\"}"), 0644)
	}
	return nil
}
