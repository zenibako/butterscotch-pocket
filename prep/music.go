package prep

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const maxMusicTracks = 4096
const maxAudioRecords = 65536
const musicRecordBytes = 48

type musicSource struct {
	name, path string
	blob       []byte
	raw        bool
}

func musicName(name string) (string, error) {
	if len(name) == 0 || len(name) >= 32 {
		return "", fmt.Errorf("music name must contain 1 to 31 ASCII bytes")
	}
	for _, c := range []byte(name) {
		if c < 32 || c > 126 || c == '/' || c == '\\' {
			return "", fmt.Errorf("unsupported music name")
		}
	}
	return strings.ToLower(name), nil
}
func musicChunkStart(c *Container, name string) int {
	for p := 8; p < len(c.Data); {
		n := int(binary.LittleEndian.Uint32(c.Data[p+4:]))
		if string(c.Data[p:p+4]) == name {
			return p + 8
		}
		p += 8 + n
	}
	return -1
}
func musicTable(b []byte) (int, error) {
	if len(b) < 4 {
		return 0, fmt.Errorf("truncated audio table")
	}
	n := uint64(binary.LittleEndian.Uint32(b))
	if n > maxAudioRecords || n*4 > uint64(len(b)-4) {
		return 0, fmt.Errorf("audio table count exceeds bounds")
	}
	return int(n), nil
}

// MusicPack emits sorted UTM1 tracks, decoding only one audio track at a time.
// External names include .ogg and override embedded sounds with the same stem.
func MusicPack(data []byte, external map[string]string, groups map[int][]byte, raw map[string][]byte, progress func(string)) ([]byte, error) {
	c, err := Parse(data)
	if err != nil {
		return nil, err
	}
	sources := make(map[string]musicSource)
	externalNames := make(map[string]bool)
	for filename, path := range external {
		if !strings.EqualFold(filepath.Ext(filename), ".ogg") || filepath.Base(filename) != filename {
			return nil, fmt.Errorf("invalid external audio filename")
		}
		name, e := musicName(filename[:len(filename)-4])
		if e != nil {
			return nil, e
		}
		if _, ok := sources[name]; ok {
			return nil, fmt.Errorf("duplicate music name %s", name)
		}
		sources[name] = musicSource{name: name, path: path}
		externalNames[name] = true
	}
	containers := map[int]*Container{0: c}
	for id, b := range groups {
		if id < 1 || id >= 16 {
			return nil, fmt.Errorf("audio group outside 1..15")
		}
		g, e := Parse(b)
		if e != nil {
			return nil, fmt.Errorf("audio group %d: %w", id, e)
		}
		containers[id] = g
	}
	if sond, e := c.Chunk("SOND"); e == nil {
		n, e := musicTable(sond)
		if e != nil {
			return nil, e
		}
		seen := map[string]bool{}
		sondStart := musicChunkStart(c, "SOND")
		for i := 0; i < n; i++ {
			offset := uint64(binary.LittleEndian.Uint32(sond[4+i*4:]))
			if offset < uint64(sondStart+4+n*4) || offset+36 > uint64(sondStart+len(sond)) {
				return nil, fmt.Errorf("sound record outside SOND")
			}
			record := c.Data[int(offset) : int(offset)+36]
			name, e := c.String(binary.LittleEndian.Uint32(record))
			if e != nil {
				return nil, e
			}
			name, e = musicName(name)
			if e != nil {
				return nil, e
			}
			if seen[name] {
				return nil, fmt.Errorf("duplicate embedded music name %s", name)
			}
			seen[name] = true
			flags := binary.LittleEndian.Uint32(record[4:])
			file := int32(binary.LittleEndian.Uint32(record[32:]))
			if flags&3 == 0 || file < 0 {
				continue
			}
			if externalNames[name] {
				continue
			}
			group := binary.LittleEndian.Uint32(record[28:])
			g := containers[int(group)]
			if group >= 16 || g == nil {
				return nil, fmt.Errorf("sound %s: missing audio group %d", name, group)
			}
			audo, e := g.Chunk("AUDO")
			if e != nil {
				return nil, e
			}
			count, e := musicTable(audo)
			if e != nil {
				return nil, e
			}
			if uint64(file) >= uint64(count) {
				return nil, fmt.Errorf("sound %s: missing AUDO record", name)
			}
			start := musicChunkStart(g, "AUDO")
			off := uint64(binary.LittleEndian.Uint32(audo[4+int(file)*4:]))
			if off < uint64(start+4+4*count) || off+4 > uint64(start+len(audo)) {
				return nil, fmt.Errorf("audio record outside AUDO")
			}
			length := uint64(binary.LittleEndian.Uint32(g.Data[int(off):]))
			if length == 0 || off+4+length > uint64(start+len(audo)) {
				return nil, fmt.Errorf("truncated embedded audio")
			}
			sources[name] = musicSource{name: name, blob: g.Data[int(off)+4 : int(off+4+length)]}
		}
	}
	for filename, b := range raw {
		if filepath.Base(filename) != filename {
			return nil, fmt.Errorf("raw music filename must be a basename")
		}
		name, e := musicName(filename)
		if e != nil {
			return nil, e
		}
		if _, ok := sources[name]; ok {
			return nil, fmt.Errorf("duplicate music name %s", name)
		}
		if len(b) > MaxInputBytes {
			return nil, fmt.Errorf("raw file exceeds limit")
		}
		sources[name] = musicSource{name: name, blob: b, raw: true}
	}
	if len(sources) == 0 || len(sources) > maxMusicTracks {
		return nil, fmt.Errorf("music track count outside limits")
	}
	names := make([]string, 0, len(sources))
	for name := range sources {
		names = append(names, name)
	}
	sort.Strings(names)
	out := make([]byte, 8+musicRecordBytes*len(names))
	copy(out, "UTM1")
	binary.LittleEndian.PutUint32(out[4:], uint32(len(names)))
	for i, name := range names {
		s := sources[name]
		if progress != nil {
			progress("Audio: " + name)
		}
		record := out[8+i*musicRecordBytes : 8+(i+1)*musicRecordBytes]
		copy(record, name)
		binary.LittleEndian.PutUint32(record[32:], uint32(len(out)))
		count, rate := len(s.blob), 0
		encoded := s.blob
		if !s.raw {
			blob := s.blob
			if s.path != "" {
				f, e := os.Open(s.path)
				if e != nil {
					return nil, fmt.Errorf("audio %s: cannot open source", name)
				}
				stat, e := f.Stat()
				if e != nil || stat.Size() < 1 || stat.Size() > MaxInputBytes {
					f.Close()
					return nil, fmt.Errorf("audio %s: source size outside limits", name)
				}
				blob, e = io.ReadAll(io.LimitReader(f, MaxInputBytes+1))
				f.Close()
				if e != nil || len(blob) > MaxInputBytes {
					return nil, fmt.Errorf("audio %s: source read failed", name)
				}
			}
			pcm, ch, sr, e := decodeAudio(blob)
			if e != nil {
				return nil, fmt.Errorf("audio %s: %w", name, e)
			}
			mono, e := monoAudio(pcm, ch, sr)
			if e != nil {
				return nil, fmt.Errorf("audio %s: %w", name, e)
			}
			count, rate = len(mono), musicSampleRate
			encoded = encodeADPCM(mono)
		}
		if len(encoded) > MaxInputBytes-len(out) {
			return nil, fmt.Errorf("music pack exceeds %d MiB", MaxInputBytes>>20)
		}
		binary.LittleEndian.PutUint32(record[36:], uint32(count))
		binary.LittleEndian.PutUint32(record[40:], uint32(rate))
		out = append(out, encoded...)
	}
	return out, nil
}
