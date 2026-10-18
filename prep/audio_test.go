package prep

import (
	"encoding/binary"
	"github.com/jfreymuth/vorbis"
	"math"
	"os"
	"reflect"
	"testing"
	"time"
)

func audioWAV(bits, channels, rate int, data []byte) []byte {
	b := make([]byte, 44+len(data))
	copy(b, "RIFF")
	binary.LittleEndian.PutUint32(b[4:], uint32(len(b)-8))
	copy(b[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(b[16:], 16)
	binary.LittleEndian.PutUint16(b[20:], 1)
	binary.LittleEndian.PutUint16(b[22:], uint16(channels))
	binary.LittleEndian.PutUint32(b[24:], uint32(rate))
	binary.LittleEndian.PutUint32(b[28:], uint32(rate*channels*bits/8))
	binary.LittleEndian.PutUint16(b[32:], uint16(channels*bits/8))
	binary.LittleEndian.PutUint16(b[34:], uint16(bits))
	copy(b[36:], "data")
	binary.LittleEndian.PutUint32(b[40:], uint32(len(data)))
	copy(b[44:], data)
	return b
}
func TestAudioPCMAndResampling(t *testing.T) {
	p, c, r, e := decodeAudio(audioWAV(8, 1, 16000, []byte{0, 128, 255, 128}))
	if e != nil || c != 1 || r != 16000 || !reflect.DeepEqual(p, []int16{-32768, 0, 32512, 0}) {
		t.Fatalf("decode %v %d %d %v", p, c, r, e)
	}
	for _, tc := range []struct {
		p    []int16
		c, r int
		want []int16
	}{{[]int16{0, 1, 1, 2, 2, 3, 3, 4}, 2, 32000, []int16{0, 2, 2, 4}}, {[]int16{0, 100, 200, 300}, 1, 64000, []int16{50, 250}}, {[]int16{0, 100, 200}, 1, 16000, []int16{0, 50, 100, 150, 200, 200}}} {
		got, e := monoAudio(tc.p, tc.c, tc.r)
		if e != nil || !reflect.DeepEqual(got, tc.want) {
			t.Fatalf("resample %v want %v err %v", got, tc.want, e)
		}
	}
}
func TestAudioRejectMalformed(t *testing.T) {
	for _, b := range [][]byte{nil, []byte("OggS"), audioWAV(24, 1, 32000, []byte{0, 0, 0}), audioWAV(16, 0, 32000, nil), audioWAV(16, 1, 32000, []byte{0}), audioWAV(16, 1, 32000, nil)} {
		if _, _, _, e := decodeAudio(b); e == nil {
			t.Fatalf("accepted %x", b)
		}
	}
	b := audioWAV(16, 1, 32000, []byte{1, 0, 2, 0})
	if _, _, _, e := decodeAudio(b[:len(b)-1]); e == nil {
		t.Fatal("truncated WAV accepted")
	}
}
func TestAudioADPCMReference(t *testing.T) {
	got := encodeADPCM([]int16{0, 1000, -1000, 32767, -32768, 0, 123, -456})
	want := []byte{0x70, 0x7f, 0x2f, 0xf3}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %x want %x", got, want)
	}
}

func TestAudioVorbisSynthetic(t *testing.T) {
	b, e := os.ReadFile("testdata/audio/sine.ogg")
	if e != nil {
		t.Fatal(e)
	}
	pcm, ch, rate, e := decodeAudio(b)
	if e != nil {
		t.Fatal(e)
	}
	if ch != 2 || rate != 44100 || len(pcm) != 8832 {
		t.Fatalf("Vorbis shape %d %d %d", ch, rate, len(pcm))
	}
	sum := 0.0
	for _, s := range pcm {
		sum += float64(s) * float64(s)
	}
	rms := math.Sqrt(sum / float64(len(pcm)))
	if rms < 1900 || rms > 2200 {
		t.Fatalf("Vorbis RMS %f", rms)
	}
	for _, n := range []int{1, 20, len(b) - 1, len(b) / 2} {
		if _, _, _, e := decodeAudio(b[:n]); e == nil {
			t.Fatalf("accepted truncated Ogg length %d", n)
		}
	}
}

func TestAudioRejectZeroSegmentOgg(t *testing.T) {
	b := make([]byte, 27)
	copy(b, "OggS")
	b[5] = 4
	if _, _, _, err := decodeAudio(b); err == nil {
		t.Fatal("accepted empty Ogg page")
	}
}

func TestAudioVorbisSetupLimits(t *testing.T) {
	// A tiny setup header claiming 16 million codebook entries must fail before decoder allocation.
	b := []byte{5, 'v', 'o', 'r', 'b', 'i', 's', 0, 'B', 'C', 'V', 1, 0, 255, 255, 255}
	if err := new(vorbis.Decoder).ReadHeader(b); err == nil {
		t.Fatal("accepted enormous codebook")
	}
	if err := new(vorbis.Decoder).ReadHeader([]byte{5, 'v', 'o', 'r', 'b', 'i', 's', 0}); err == nil {
		t.Fatal("accepted incomplete setup")
	}
}

func TestAudioVorbisCommentLimits(t *testing.T) {
	b := []byte{3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 255, 255, 255, 255, 1}
	if err := new(vorbis.Decoder).ReadHeader(b); err == nil {
		t.Fatal("accepted enormous comment table")
	}
}

func reviewPage(packet []byte, seq uint32, flag byte) []byte {
	b := make([]byte, 28+len(packet))
	copy(b, "OggS")
	b[5] = flag
	binary.LittleEndian.PutUint32(b[14:], 1)
	binary.LittleEndian.PutUint32(b[18:], seq)
	b[26] = 1
	b[27] = byte(len(packet))
	copy(b[28:], packet)
	var crc uint32
	for _, v := range b {
		crc ^= uint32(v) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = crc<<1 ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	binary.LittleEndian.PutUint32(b[22:], crc)
	return b
}
func reviewOgg(t *testing.T, comment, setup []byte) []byte {
	orig, e := os.ReadFile("testdata/audio/sine.ogg")
	if e != nil {
		t.Fatal(e)
	}
	ident := orig[28:58]
	b := reviewPage(ident, 0, 2)
	b = append(b, reviewPage(comment, 1, 0)...)
	return append(b, reviewPage(setup, 2, 4)...)
}
func TestAudioMalformedSetupTerminates(t *testing.T) {
	comment := append([]byte{3, 'v', 'o', 'r', 'b', 'i', 's'}, make([]byte, 9)...)
	comment[len(comment)-1] = 1
	setup := []byte{5, 'v', 'o', 'r', 'b', 'i', 's', 0, 0x42, 0x43, 0x56, 1, 0, 2, 0, 0, 1}
	b := reviewOgg(t, comment, setup)
	done := make(chan bool)
	go func() { _, _, _, e := decodeAudio(b); t.Logf("decode error: %v", e); close(done) }()
	select {
	case <-done:
	case <-time.After(200 * time.Millisecond):
		t.Fatal("decodeAudio hangs on a complete CRC-correct malformed Ogg setup packet")
	}
}

type reviewBits struct {
	b []byte
	p uint
}

func (w *reviewBits) put(v uint32, n uint) {
	for i := uint(0); i < n; i++ {
		if w.p/8 >= uint(len(w.b)) {
			w.b = append(w.b, 0)
		}
		w.b[w.p/8] |= byte((v>>i)&1) << (w.p % 8)
		w.p++
	}
}
func TestAudioEmptyHuffmanTerminates(t *testing.T) {
	var w reviewBits
	p := w.put
	p(0x564342, 24)
	p(1, 16)
	p(2, 24)
	p(0, 1)
	p(1, 1)
	p(0, 1)
	p(0, 1)
	p(0, 4)
	p(0, 6)
	p(0, 16)
	p(0, 6)
	p(1, 16)
	p(0, 5)
	p(0, 3)
	p(0, 2)
	p(0, 8)
	p(0, 2)
	p(6, 4)
	p(0, 6)
	p(0, 16)
	p(0, 24)
	p(32, 24)
	p(0, 24)
	p(0, 6)
	p(0, 8)
	p(0, 3)
	p(0, 1)
	p(0, 6)
	p(0, 16)
	p(0, 1)
	p(0, 1)
	p(0, 2)
	p(0, 8)
	p(0, 8)
	p(0, 8)
	p(0, 6)
	p(0, 1)
	p(0, 16)
	p(0, 16)
	p(0, 8)
	p(1, 1)
	setup := append([]byte{5, 'v', 'o', 'r', 'b', 'i', 's', 0}, w.b...)
	comment := append([]byte{3, 'v', 'o', 'r', 'b', 'i', 's'}, make([]byte, 9)...)
	comment[len(comment)-1] = 1
	orig, e := os.ReadFile("testdata/audio/sine.ogg")
	if e != nil {
		t.Fatal(e)
	}
	ident := append([]byte(nil), orig[28:58]...)
	ident[11] = 1
	b := reviewPage(ident, 0, 2)
	b = append(b, reviewPage(comment, 1, 0)...)
	b = append(b, reviewPage(setup, 2, 0)...)
	last := reviewPage([]byte{2, 0, 0}, 3, 4)
	binary.LittleEndian.PutUint64(last[6:], 1)
	for i := 22; i < 26; i++ {
		last[i] = 0
	}
	var crc uint32
	for _, v := range last {
		crc ^= uint32(v) << 24
		for j := 0; j < 8; j++ {
			if crc&0x80000000 != 0 {
				crc = crc<<1 ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	binary.LittleEndian.PutUint32(last[22:], crc)
	b = append(b, last...)
	done := make(chan bool)
	go func() { _, _, _, e := decodeAudio(b); t.Logf("decode error %v", e); close(done) }()
	select {
	case <-done:
	case <-time.After(200 * time.Millisecond):
		t.Fatal("decodeAudio hangs on empty Huffman table")
	}
}

func TestAudioVorbisGranuleBeyondDecodedSamples(t *testing.T) {
	// An Ogg granule is a PCM end position, not necessarily the number of
	// frames returned. Reproduce a 128-frame positive offset using our tone;
	// the C/stb reference returns 5120 frames for this complete packet set.
	b, err := os.ReadFile("testdata/audio/sine.ogg")
	if err != nil {
		t.Fatal(err)
	}
	last := 0
	for p := 0; p < len(b); {
		last = p
		n := int(b[p+26])
		size := 27 + n
		for _, segment := range b[p+27 : p+27+n] {
			size += int(segment)
		}
		p += size
	}
	setGranule := func(granule uint64) {
		binary.LittleEndian.PutUint64(b[last+6:], granule)
		clear(b[last+22 : last+26])
		var crc uint32
		for _, v := range b[last:] {
			crc ^= uint32(v) << 24
			for i := 0; i < 8; i++ {
				if crc&0x80000000 != 0 {
					crc = crc<<1 ^ 0x04c11db7
				} else {
					crc <<= 1
				}
			}
		}
		binary.LittleEndian.PutUint32(b[last+22:], crc)
	}
	setGranule(5120 + 128)
	pcm, ch, rate, err := decodeAudio(b)
	if err != nil {
		t.Fatal(err)
	}
	if ch != 2 || rate != 44100 || len(pcm) != 5120*2 {
		t.Fatalf("Vorbis shape %d %d %d", ch, rate, len(pcm))
	}
	for _, n := range []int{last, len(b) - 1} {
		if _, _, _, err := decodeAudio(b[:n]); err == nil {
			t.Fatalf("accepted truncated offset stream at %d", n)
		}
	}
	b[len(b)-1] ^= 1
	if _, _, _, err := decodeAudio(b); err == nil {
		t.Fatal("accepted corrupt offset stream checksum")
	}
	b[len(b)-1] ^= 1
	setGranule(maxAudioSamples/2 + 1)
	if _, _, _, err := decodeAudio(b); err == nil {
		t.Fatal("accepted excessive granule")
	}
}

func TestAudioResamplingRoundsWindowStartBeforeEnd(t *testing.T) {
	// Force a half-integer output at 44.1 -> 32 kHz. Fusing i*ratio+ratio
	// moves the end boundary across that tie and disagrees with the C oracle.
	pcm := make([]int16, 9118*2)
	pcm[9116*2], pcm[9116*2+1] = -3673, -3674
	pcm[9117*2], pcm[9117*2+1] = -3012, -3012
	mono, err := monoAudio(pcm, 2, 44100)
	if err != nil {
		t.Fatal(err)
	}
	if mono[6615] != -3350 {
		t.Fatalf("resampled boundary = %d, want C reference -3350", mono[6615])
	}
}

func TestAudioVorbisEndUsesPreviousPagePosition(t *testing.T) {
	b, err := os.ReadFile("testdata/audio/sine.ogg")
	if err != nil {
		t.Fatal(err)
	}
	// Split the synthetic tone's six audio packets into two pages. The
	// earlier page position is 128 frames ahead of its decoded sample count,
	// reproducing page positions that occur at Vorbis window transitions.
	p := 0
	for b[p+5]&4 == 0 {
		n := int(b[p+26])
		size := 27 + n
		for _, segment := range b[p+27 : p+27+n] {
			size += int(segment)
		}
		p += size
	}
	n := int(b[p+26])
	body := p + 27 + n
	var packets [][]byte
	var packet []byte
	for _, segment := range b[p+27 : p+27+n] {
		packet = append(packet, b[body:body+int(segment)]...)
		body += int(segment)
		if segment < 255 {
			packets = append(packets, packet)
			packet = nil
		}
	}
	if len(packets) != 6 {
		t.Fatalf("synthetic packet count = %d", len(packets))
	}
	page := func(packets [][]byte, seq uint32, flag byte, granule uint64) []byte {
		out := make([]byte, 27)
		copy(out, b[p:p+27])
		out[5] = flag
		binary.LittleEndian.PutUint64(out[6:], granule)
		binary.LittleEndian.PutUint32(out[18:], seq)
		clear(out[22:26])
		for _, packet := range packets {
			length := len(packet)
			for length >= 255 {
				out = append(out, 255)
				length -= 255
			}
			out = append(out, byte(length))
		}
		out[26] = byte(len(out) - 27)
		for _, packet := range packets {
			out = append(out, packet...)
		}
		var crc uint32
		for _, v := range out {
			crc ^= uint32(v) << 24
			for i := 0; i < 8; i++ {
				if crc&0x80000000 != 0 {
					crc = crc<<1 ^ 0x04c11db7
				} else {
					crc <<= 1
				}
			}
		}
		binary.LittleEndian.PutUint32(out[22:], crc)
		return out
	}
	for _, tc := range []struct {
		name            string
		split           int
		previous, final uint64
		frames          int
	}{
		{"partial-final-packet", 5, 4096 + 128, 5120, 4992},
		{"discard-exceeds-final-packet", 4, 3072 + 2304, 6144, 4096},
	} {
		t.Run(tc.name, func(t *testing.T) {
			stream := append([]byte(nil), b[:p]...)
			stream = append(stream, page(packets[:tc.split], 2, 0, tc.previous)...)
			stream = append(stream, page(packets[tc.split:], 3, 4, tc.final)...)
			pcm, ch, rate, err := decodeAudio(stream)
			if err != nil {
				t.Fatal(err)
			}
			// Both expected lengths were emitted by the unchanged C/stb decoder.
			if len(pcm) != tc.frames*2 || ch != 2 || rate != 44100 {
				t.Fatalf("Vorbis shape %d %d %d, want %d 2 44100", len(pcm), ch, rate, tc.frames*2)
			}
		})
	}
}
