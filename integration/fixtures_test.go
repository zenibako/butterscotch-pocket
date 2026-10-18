package integration

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// Include an original generated Vorbis track in full-pipeline comparisons.
func addVorbis(t *testing.T, source string) {
	t.Helper()
	b, err := os.ReadFile("../prep/testdata/audio/sine.ogg")
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(filepath.Join(source, "sine.ogg"), b, 0644); err != nil {
		t.Fatal(err)
	}
	dataPath := filepath.Join(source, "data.win")
	data, err := os.ReadFile(dataPath)
	if err != nil {
		t.Fatal(err)
	}
	payload := []byte("sine.ogg\x00")
	header := make([]byte, 8)
	copy(header, "STRG")
	binary.LittleEndian.PutUint32(header[4:], uint32(len(payload)))
	data = append(data, header...)
	data = append(data, payload...)
	binary.LittleEndian.PutUint32(data[4:], uint32(len(data)-8))
	if err = os.WriteFile(dataPath, data, 0644); err != nil {
		t.Fatal(err)
	}
}
