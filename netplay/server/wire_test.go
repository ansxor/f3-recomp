package main

import (
	"bufio"
	"bytes"
	"errors"
	"net/netip"
	"testing"
	"time"
)

func TestReaderTrailingBytesRejected(t *testing.T) {
	r := &reader{b: []byte{0x01, 0x02}}
	if v := r.u8(); v != 0x01 {
		t.Fatalf("u8 = %#x", v)
	}
	if err := r.finish(); !errors.Is(err, errMalformed) {
		t.Fatalf("finish with trailing byte: err = %v, want errMalformed", err)
	}
}

func TestReaderShortRead(t *testing.T) {
	r := &reader{b: []byte{0x00}}
	_ = r.u32()
	if !errors.Is(r.err, errMalformed) {
		t.Fatalf("short u32: err = %v, want errMalformed", r.err)
	}
}

func TestReaderStrRejectsInvalidUTF8(t *testing.T) {
	r := &reader{b: []byte{0x02, 0xff, 0xfe}}
	_ = r.str()
	if !errors.Is(r.err, errMalformed) {
		t.Fatalf("invalid utf-8: err = %v, want errMalformed", r.err)
	}
}

func TestReaderStrValid(t *testing.T) {
	r := &reader{b: appendStr(nil, "héllo")}
	if got := r.str(); got != "héllo" {
		t.Fatalf("str = %q", got)
	}
	if err := r.finish(); err != nil {
		t.Fatalf("finish: %v", err)
	}
}

func TestEncodeReadFrameRoundTrip(t *testing.T) {
	payload := []byte{1, 2, 3, 4, 5}
	frame := encode(msgCRC, payload)
	typ, got, err := readFrame(bufio.NewReader(bytes.NewReader(frame)))
	if err != nil {
		t.Fatalf("readFrame: %v", err)
	}
	if typ != msgCRC || !bytes.Equal(got, payload) {
		t.Fatalf("got type %#x payload %v", typ, got)
	}
}

func TestReadFrameRejectsBadLength(t *testing.T) {
	cases := map[string][]byte{
		"zero":     {0, 0, 0, 0},
		"too long": {0, 0, 0x10, 0x01}, // 4097
	}
	for name, hdr := range cases {
		_, _, err := readFrame(bufio.NewReader(bytes.NewReader(hdr)))
		if !errors.Is(err, errFrameSize) {
			t.Errorf("%s: err = %v, want errFrameSize", name, err)
		}
	}
}

func TestReadFrameAcceptsMaxLength(t *testing.T) {
	frame := encode(msgPeerLeft, make([]byte, maxFrameLen-1))
	if _, _, err := readFrame(bufio.NewReader(bytes.NewReader(frame))); err != nil {
		t.Fatalf("max-length frame: %v", err)
	}
}

func TestRoomAlphabet(t *testing.T) {
	if len(roomAlphabet) != 32 {
		t.Fatalf("alphabet length %d, want 32", len(roomAlphabet))
	}
	seen := map[byte]bool{}
	for i := range len(roomAlphabet) {
		if seen[roomAlphabet[i]] {
			t.Fatalf("duplicate symbol %q", roomAlphabet[i])
		}
		seen[roomAlphabet[i]] = true
	}
}

func TestRateLimiter(t *testing.T) {
	l := newRateLimiter()
	src := netip.MustParseAddrPort("192.0.2.1:9000")
	now := time.Unix(1000, 0)
	for i := range udpBurst {
		if !l.allow(src, now) {
			t.Fatalf("packet %d denied within burst", i)
		}
	}
	if l.allow(src, now) {
		t.Fatal("packet beyond burst allowed")
	}
	// 300 tokens/s: 100 ms refills 30 tokens.
	later := now.Add(100 * time.Millisecond)
	for i := range 30 {
		if !l.allow(src, later) {
			t.Fatalf("refill packet %d denied", i)
		}
	}
	if l.allow(src, later) {
		t.Fatal("refill exceeded rate")
	}
}

func TestRateLimiterPerSource(t *testing.T) {
	l := newRateLimiter()
	a := netip.MustParseAddrPort("192.0.2.1:9000")
	b := netip.MustParseAddrPort("192.0.2.2:9000")
	now := time.Unix(1000, 0)
	for range udpBurst {
		l.allow(a, now)
	}
	if !l.allow(b, now) {
		t.Fatal("second source limited by first source's bucket")
	}
}
