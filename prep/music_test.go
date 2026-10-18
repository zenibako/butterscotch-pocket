package prep

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func musicU32(b []byte, p int, v uint32) { binary.LittleEndian.PutUint32(b[p:], v) }
func musicFixture(group uint32, blob []byte) ([]byte, []byte) {
	b := make([]byte, 92+len(blob))
	copy(b, "FORM")
	musicU32(b, 4, uint32(len(b)-8))
	copy(b[8:], "SOND")
	musicU32(b, 12, 56)
	musicU32(b, 16, 1)
	musicU32(b, 20, 24)
	musicU32(b, 24, 60)
	musicU32(b, 28, 1)
	musicU32(b, 52, group)
	copy(b[60:], "Snd_Test\x00")
	copy(b[72:], "AUDO")
	musicU32(b, 76, uint32(len(b)-80))
	musicU32(b, 80, 1)
	musicU32(b, 84, 88)
	musicU32(b, 88, uint32(len(blob)))
	copy(b[92:], blob)
	g := make([]byte, 28+len(blob))
	copy(g, "FORM")
	musicU32(g, 4, uint32(len(g)-8))
	copy(g[8:], "AUDO")
	musicU32(g, 12, uint32(len(g)-16))
	musicU32(g, 16, 1)
	musicU32(g, 20, 24)
	musicU32(g, 24, uint32(len(blob)))
	copy(g[28:], blob)
	return b, g
}
func TestMusicPackGroupAndRaw(t *testing.T) {
	b, g := musicFixture(1, audioWAV(8, 1, 32000, []byte{128, 129, 127, 128}))
	got, e := MusicPack(b, nil, map[int][]byte{1: g}, map[string][]byte{"LANG_EN.JSON": []byte("{}")}, nil)
	if e != nil {
		t.Fatal(e)
	}
	if string(got[:4]) != "UTM1" || binary.LittleEndian.Uint32(got[4:]) != 2 || string(bytes.TrimRight(got[8:40], "\x00")) != "lang_en.json" || string(bytes.TrimRight(got[56:88], "\x00")) != "snd_test" {
		t.Fatalf("bad records %x", got)
	}
	if binary.LittleEndian.Uint32(got[40:]) != 104 || binary.LittleEndian.Uint32(got[48:]) != 0 || !bytes.Equal(got[104:106], []byte("{}")) || binary.LittleEndian.Uint32(got[96:]) != 32000 {
		t.Fatalf("bad layout %x", got)
	}
}
func TestMusicPackRejectMissingAndDuplicates(t *testing.T) {
	b, _ := musicFixture(1, audioWAV(8, 1, 32000, []byte{128, 128}))
	if _, e := MusicPack(b, nil, nil, nil, nil); e == nil {
		t.Fatal("missing group accepted")
	}
	b, _ = musicFixture(0, audioWAV(8, 1, 32000, []byte{128, 128}))
	if _, e := MusicPack(b, nil, nil, map[string][]byte{"SND_TEST": []byte("x")}, nil); e == nil {
		t.Fatal("duplicate accepted")
	}
	if _, e := MusicPack([]byte("FORM\x00\x00\x00\x00"), nil, nil, nil, nil); e == nil {
		t.Fatal("empty accepted")
	}
}

// Golden was emitted by the unchanged mkmusic.c for stereo 44.1 kHz PCM.
func TestMusicPackPCMReference(t *testing.T) {
	samples := []int16{0, 1, 1000, -1000, 32767, -32768, 1234, 4321, -1, 0, -2000, 3000, 999, -999, 42, 43}
	pcm := make([]byte, len(samples)*2)
	for i, s := range samples {
		binary.LittleEndian.PutUint16(pcm[i*2:], uint16(s))
	}
	b, _ := musicFixture(0, audioWAV(16, 2, 44100, pcm))
	got, err := MusicPack(b, nil, nil, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	want := make([]byte, 58)
	copy(want, "UTM1")
	musicU32(want, 4, 1)
	copy(want[8:], "snd_test")
	musicU32(want, 40, 56)
	musicU32(want, 44, 4)
	musicU32(want, 48, 32000)
	want[56] = 0
	want[57] = 0x77
	if !bytes.Equal(got, want) {
		t.Fatalf("PCM C parity: got %x want %x", got, want)
	}
}

func TestMusicExternalPrecedenceAndFailures(t *testing.T) {
	ogg, err := os.ReadFile("testdata/audio/sine.ogg")
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "stream.ogg")
	if err = os.WriteFile(path, ogg, 0600); err != nil {
		t.Fatal(err)
	}
	// A valid streamed file overrides an embedded reference to an absent group.
	b, _ := musicFixture(15, []byte("not audio"))
	got, err := MusicPack(b, map[string]string{"SND_TEST.ogg": path}, nil, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if binary.LittleEndian.Uint32(got[44:]) != 3204 {
		t.Fatalf("unexpected output samples %d", binary.LittleEndian.Uint32(got[44:]))
	}
	for _, ext := range []map[string]string{{"snd_test.ogg": path, "SND_TEST.ogg": path}, {"bad.wav": path}, {"snd_test.ogg": path + "-absent"}, {"../snd_test.ogg": path}} {
		if _, err := MusicPack(b, ext, nil, nil, nil); err == nil {
			t.Fatal("accepted invalid external source")
		}
	}
}
func TestMusicRejectTruncatedTablesAndNames(t *testing.T) {
	base, _ := musicFixture(0, audioWAV(8, 1, 32000, []byte{128, 128}))
	for _, mutate := range []func([]byte){func(b []byte) { musicU32(b, 16, 65537) }, func(b []byte) { musicU32(b, 20, 0xffffffff) }, func(b []byte) { musicU32(b, 84, 0xffffffff) }, func(b []byte) { musicU32(b, 88, 0xffffffff) }, func(b []byte) { b[60] = '/' }} {
		b := bytes.Clone(base)
		mutate(b)
		if _, err := MusicPack(b, nil, nil, nil, nil); err == nil {
			t.Fatal("accepted malformed music source")
		}
	}
	for _, name := range []string{"", strings.Repeat("a", 32), "../bad", "snow雪"} {
		if _, err := MusicPack([]byte("FORM\x00\x00\x00\x00"), nil, nil, map[string][]byte{name: []byte("x")}, nil); err == nil {
			t.Fatalf("accepted name %q", name)
		}
	}
	if _, err := MusicPack(base, nil, map[int][]byte{16: base}, nil, nil); err == nil {
		t.Fatal("accepted invalid group index")
	}
}

func TestMusicPackRejectRuntimeTrackLimit(t *testing.T) {
	raw := make(map[string][]byte, 4097)
	for i := 0; i < 4097; i++ {
		raw[fmt.Sprintf("file%04d", i)] = []byte("x")
	}
	if _, err := MusicPack([]byte("FORM\x00\x00\x00\x00"), nil, nil, raw, nil); err == nil {
		t.Fatal("accepted more than runtime's 4096 tracks")
	}
}
