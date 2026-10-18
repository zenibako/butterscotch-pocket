package prep

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"math"

	"github.com/jfreymuth/oggvorbis"
)

// These limits keep one decoded track and its resampling buffers bounded on wasm32.
const maxAudioSamples = 32 << 20
const musicSampleRate = 32000

func decodeAudio(blob []byte) ([]int16, int, int, error) {
	if len(blob) > MaxInputBytes {
		return nil, 0, 0, fmt.Errorf("audio input exceeds limit")
	}
	if len(blob) >= 4 && string(blob[:4]) == "OggS" {
		return decodeVorbis(blob)
	}
	if len(blob) < 12 || string(blob[:4]) != "RIFF" || string(blob[8:12]) != "WAVE" || uint64(binary.LittleEndian.Uint32(blob[4:])) != uint64(len(blob)-8) {
		return nil, 0, 0, fmt.Errorf("invalid or truncated WAV")
	}
	channels, rate, bits := 0, 0, 0
	var samples []byte
	haveFmt, haveData := false, false
	for p := 12; p < len(blob); {
		if len(blob)-p < 8 {
			return nil, 0, 0, fmt.Errorf("truncated WAV chunk")
		}
		n := uint64(binary.LittleEndian.Uint32(blob[p+4:]))
		if n > uint64(len(blob)-p-8) {
			return nil, 0, 0, fmt.Errorf("truncated WAV body")
		}
		body := blob[p+8 : p+8+int(n)]
		switch string(blob[p : p+4]) {
		case "fmt ":
			if haveFmt || len(body) < 16 || binary.LittleEndian.Uint16(body) != 1 {
				return nil, 0, 0, fmt.Errorf("unsupported WAV format")
			}
			haveFmt = true
			channels = int(binary.LittleEndian.Uint16(body[2:]))
			rate = int(binary.LittleEndian.Uint32(body[4:]))
			bits = int(binary.LittleEndian.Uint16(body[14:]))
			if channels < 1 || channels > 8 || rate < 1 || rate > 384000 || (bits != 8 && bits != 16) || int(binary.LittleEndian.Uint16(body[12:])) != channels*bits/8 {
				return nil, 0, 0, fmt.Errorf("invalid WAV parameters")
			}
		case "data":
			if haveData {
				return nil, 0, 0, fmt.Errorf("duplicate WAV data")
			}
			haveData = true
			samples = body
		}
		p += 8 + int(n)
		if n&1 != 0 && p < len(blob) {
			p++
		}
	}
	if !haveFmt || !haveData || len(samples) == 0 || len(samples)%(channels*bits/8) != 0 || len(samples)/(bits/8) > maxAudioSamples {
		return nil, 0, 0, fmt.Errorf("invalid WAV sample count")
	}
	out := make([]int16, len(samples)/(bits/8))
	for i := range out {
		if bits == 8 {
			out[i] = int16((int(samples[i]) - 128) * 256)
		} else {
			out[i] = int16(binary.LittleEndian.Uint16(samples[i*2:]))
		}
	}
	return out, channels, rate, nil
}

func decodeVorbis(blob []byte) (pcm []int16, channels, sampleRate int, err error) {
	// Malformed codec headers must not let a dependency panic escape the converter.
	defer func() {
		if recover() != nil {
			pcm = nil
			channels = 0
			sampleRate = 0
			err = fmt.Errorf("malformed Vorbis stream")
		}
	}()
	// Require complete pages and an end-of-stream page: EOF alone can conceal truncation.
	eos := false
	headerCount := 0
	var header []byte
	var headerChannels int
	var finalGranule uint64
	for p := 0; p < len(blob); {
		if len(blob)-p < 27 || string(blob[p:p+4]) != "OggS" || blob[p+4] != 0 {
			return nil, 0, 0, fmt.Errorf("truncated Ogg page")
		}
		n := int(blob[p+26])
		if n == 0 || len(blob)-p < 27+n {
			return nil, 0, 0, fmt.Errorf("truncated Ogg lacing")
		}
		size := 27 + n
		for _, v := range blob[p+27 : p+27+n] {
			size += int(v)
		}
		if size > len(blob)-p {
			return nil, 0, 0, fmt.Errorf("truncated Ogg body")
		}
		if headerCount < 3 {
			body := p + 27 + n
			for _, segment := range blob[p+27 : p+27+n] {
				end := body + int(segment)
				if headerCount < 3 {
					if len(header)+int(segment) > 1<<20 {
						return nil, 0, 0, fmt.Errorf("Vorbis header exceeds limit")
					}
					header = append(header, blob[body:end]...)
					if segment < 255 {
						if headerCount == 0 {
							if len(header) < 30 || string(header[:7]) != "\x01vorbis" {
								return nil, 0, 0, fmt.Errorf("invalid Vorbis identification")
							}
							headerChannels = int(header[11])
							sr := binary.LittleEndian.Uint32(header[12:])
							if headerChannels < 1 || headerChannels > 8 || sr < 1 || sr > 384000 {
								return nil, 0, 0, fmt.Errorf("Vorbis parameters exceed limit")
							}
						}
						headerCount++
						header = nil
					}
				}
				body = end
			}
		}
		finalGranule = binary.LittleEndian.Uint64(blob[p+6:])
		eos = blob[p+5]&4 != 0
		p += size
		if eos && p != len(blob) {
			return nil, 0, 0, fmt.Errorf("chained Ogg streams unsupported")
		}
	}
	if !eos || headerCount != 3 || headerChannels == 0 || finalGranule == 0 || finalGranule > uint64(maxAudioSamples/headerChannels) {
		return nil, 0, 0, fmt.Errorf("missing Ogg end of stream")
	}
	r, err := oggvorbis.NewReader(bytes.NewReader(blob))
	if err != nil {
		return nil, 0, 0, fmt.Errorf("decode Vorbis: %w", err)
	}
	ch, rate := r.Channels(), r.SampleRate()
	if ch < 1 || ch > 8 || rate < 1 || rate > 384000 || r.Length() <= 0 || r.Length() > int64(maxAudioSamples/ch) {
		return nil, 0, 0, fmt.Errorf("Vorbis parameters exceed limit")
	}
	out := make([]int16, 0, int(r.Length())*ch)
	buf := make([]float32, 4096*ch)
	for {
		n, e := r.Read(buf)
		if len(out)+n > maxAudioSamples {
			return nil, 0, 0, fmt.Errorf("Vorbis samples exceed limit")
		}
		for _, s := range buf[:n] {
			v := math.RoundToEven(float64(s) * 32768)
			if v > 32767 {
				v = 32767
			}
			if v < -32768 {
				v = -32768
			}
			if math.IsNaN(v) {
				return nil, 0, 0, fmt.Errorf("invalid Vorbis sample")
			}
			out = append(out, int16(v))
		}
		if e == io.EOF {
			break
		}
		if e != nil {
			return nil, 0, 0, fmt.Errorf("decode Vorbis: %w", e)
		}
		if n == 0 {
			return nil, 0, 0, io.ErrNoProgress
		}
	}
	// Ogg granules are end positions and may include a positive starting
	// offset. They need not equal the number of decoded frames (including
	// streams accepted by stb_vorbis). Complete framing/EOS and codec errors
	// above detect truncation; do not manufacture samples to fill that offset.
	if len(out) == 0 || len(out)%ch != 0 {
		return nil, 0, 0, fmt.Errorf("incomplete Vorbis samples")
	}
	return out, ch, rate, nil
}

func monoAudio(pcm []int16, channels, rate int) ([]int16, error) {
	if channels < 1 || channels > 8 || rate < 1 || rate > 384000 || len(pcm) == 0 || len(pcm)%channels != 0 || len(pcm) > maxAudioSamples {
		return nil, fmt.Errorf("invalid audio parameters")
	}
	frames := len(pcm) / channels
	ratio := float64(rate) / musicSampleRate
	count := int(float64(frames)/ratio) & ^1
	if count < 2 || count > maxAudioSamples {
		return nil, fmt.Errorf("resampled audio sample count outside limits")
	}
	out := make([]int16, count)
	for i := range out {
		// Preserve the C reference's rounded window start before adding
		// ratio. An explicit conversion prevents Go from fusing both steps.
		start := float64(float64(i) * ratio)
		end := start + ratio
		sum, weight := 0.0, 0.0
		if ratio > 1 {
			for f := int(start); f < frames && f < int(math.Ceil(end)); f++ {
				w := math.Min(float64(f+1), end) - math.Max(float64(f), start)
				if w <= 0 {
					continue
				}
				s := 0.0
				for c := 0; c < channels; c++ {
					s += float64(pcm[f*channels+c])
				}
				sum += float64((s / float64(channels)) * w)
				weight += w
			}
		} else {
			f0 := int(start)
			f1 := f0
			if f0+1 < frames {
				f1++
			}
			t := start - float64(f0)
			s0, s1 := 0.0, 0.0
			for c := 0; c < channels; c++ {
				s0 += float64(pcm[f0*channels+c])
				s1 += float64(pcm[f1*channels+c])
			}
			sum = (float64(s0*(1-t)) + float64(s1*t)) / float64(channels)
			weight = 1
		}
		v := 0.0
		if weight > 0 {
			v = sum / weight
		}
		v = math.Max(-32768, math.Min(32767, v))
		out[i] = int16(math.RoundToEven(v))
	}
	return out, nil
}

var audioIndexTable = [16]int{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8}
var audioStepTable = [89]int{7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767}

func encodeADPCM(samples []int16) []byte {
	out := make([]byte, len(samples)/2)
	predictor, index := 0, 0
	for i, s := range samples {
		step := audioStepTable[index]
		diff := int(s) - predictor
		code := 0
		if diff < 0 {
			code = 8
			diff = -diff
		}
		if diff >= step {
			code |= 4
			diff -= step
		}
		if diff >= step>>1 {
			code |= 2
			diff -= step >> 1
		}
		if diff >= step>>2 {
			code |= 1
		}
		delta := step >> 3
		if code&4 != 0 {
			delta += step
		}
		if code&2 != 0 {
			delta += step >> 1
		}
		if code&1 != 0 {
			delta += step >> 2
		}
		if code&8 != 0 {
			delta = -delta
		}
		predictor += delta
		predictor = max(-32768, min(32767, predictor))
		index = max(0, min(88, index+audioIndexTable[code]))
		out[i/2] |= byte(code) << uint((i&1)*4)
	}
	return out
}
