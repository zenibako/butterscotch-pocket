package vorbis

import (
	"encoding/binary"
	"errors"
)

const (
	headerTypeIdentification = 1
	headerTypeComment        = 3
	headerTypeSetup          = 5
)

func (d *Decoder) readIdentificationHeader(h []byte) error {
	if len(h) <= 22 {
		return errors.New("vorbis: short identification header")
	}
	le := binary.LittleEndian
	version := le.Uint32(h)
	if version != 0 {
		return errors.New("vorbis: unsupported Vorbis version")
	}
	d.channels = int(h[4])
	d.sampleRate = int(le.Uint32(h[5:]))
	d.Bitrate.Maximum = int(le.Uint32(h[9:]))
	d.Bitrate.Nominal = int(le.Uint32(h[13:]))
	d.Bitrate.Minimum = int(le.Uint32(h[17:]))
	d.blocksize[0] = 1 << (h[21] & 0x0F)
	d.blocksize[1] = 1 << (h[21] >> 4)
	if d.channels < 1 || d.channels > 8 || d.sampleRate < 1 || d.sampleRate > 384000 || d.blocksize[0] < 64 || d.blocksize[1] < d.blocksize[0] || d.blocksize[1] > 8192 {
		return errors.New("vorbis: invalid identification parameters")
	}
	if h[22]&1 == 0 {
		return errors.New("vorbis: missing identification framing")
	}
	return nil
}

func (d *Decoder) readCommentHeader(h []byte) (err error) {
	defer func() {
		if recover() != nil {
			err = errors.New("vorbis: malformed comment header")
		}
	}()
	le := binary.LittleEndian
	vendorLen := le.Uint32(h)
	h = h[4:]
	d.Vendor = string(h[:vendorLen])
	h = h[vendorLen:]
	numComments := int(le.Uint32(h))
	if numComments > 65536 || numComments > (len(h)-4)/4 {
		return errors.New("vorbis: comment count exceeds limit")
	}
	d.Comments = make([]string, numComments)
	h = h[4:]
	for i := 0; i < numComments; i++ {
		commentLen := le.Uint32(h)
		h = h[4:]
		d.Comments[i] = string(h[:commentLen])
		h = h[commentLen:]
	}
	if len(h) < 1 || h[0]&1 == 0 {
		return errors.New("vorbis: missing comment framing")
	}
	return err
}
