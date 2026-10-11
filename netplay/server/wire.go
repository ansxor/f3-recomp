// Protocol v1 framing and field codecs. The normative description is
// docs/site/developer/netplay/protocol.md.
package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"io"
	"unicode/utf8"
)

const (
	protoVersion = 1

	// maxFrameLen bounds the TCP length field, which counts the type byte and payload.
	maxFrameLen = 4096

	maxUDPPacket = 512
	udpHeaderLen = 4 + 8 + 1 + 1 // magic, session_id, player_index, kind
	udpTagLen    = 16
	maxInputs    = 32

	maxNameLen   = 24
	numROMCRCs   = 7
	roomCodeLen  = 6
	roomAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789" // 32 symbols, so index with b&31

	minInputDelay = 1
	maxInputDelay = 8

	// CREATE_ROOM flags. Bits outside flagsKnownMask make the message malformed.
	flagSyncBlob   = 0x01
	flagsKnownMask = flagSyncBlob
	minSyncBlob    = 1
	maxSyncBlob    = 2048

	winnerP1      = 0
	winnerP2      = 1
	winnerDraw    = 0xFF
	winnerInvalid = 0xFE // RESULT_ACK winner when the two players disagree
)

// TCP message types.
const (
	msgHello      = 0x01
	msgCreateRoom = 0x02
	msgJoinRoom   = 0x03
	msgCRC        = 0x04
	msgResult     = 0x05
	msgBye        = 0x06
	msgSyncBlob   = 0x07
	msgChat       = 0x08

	msgWelcome     = 0x81
	msgRoomCreated = 0x82
	msgMatchStart  = 0x83
	msgGo          = 0x84
	msgDesync      = 0x85
	msgResultAck   = 0x86
	msgPeerLeft    = 0x87
	msgError       = 0x88
	msgSyncBlobFwd = 0x89
	msgChatFwd     = 0x8A
)

// ERROR codes.
const (
	errBadVersion       = 1
	errBadMessage       = 2
	errRoomNotFound     = 3
	errRoomFull         = 4
	errIdentityMismatch = 5
	errNotInLobby       = 6
	errServerFull       = 7
)

// UDP packet kinds.
const (
	kindRegister = 0
	kindInputs   = 1
)

var (
	errMalformed = errors.New("malformed message")
	errFrameSize = errors.New("frame length out of range")
)

// reader decodes a big-endian payload. The first failure is sticky: later
// reads return zero values and finish reports the error.
type reader struct {
	b   []byte
	err error
}

func (r *reader) bytes(n int) []byte {
	if r.err != nil {
		return nil
	}
	if n > len(r.b) {
		r.err = errMalformed
		return nil
	}
	v := r.b[:n:n]
	r.b = r.b[n:]
	return v
}

func (r *reader) u8() uint8 {
	b := r.bytes(1)
	if r.err != nil {
		return 0
	}
	return b[0]
}

func (r *reader) u16() uint16 {
	b := r.bytes(2)
	if r.err != nil {
		return 0
	}
	return binary.BigEndian.Uint16(b)
}

func (r *reader) u32() uint32 {
	b := r.bytes(4)
	if r.err != nil {
		return 0
	}
	return binary.BigEndian.Uint32(b)
}

// str reads a u8 length followed by UTF-8 bytes.
func (r *reader) str() string {
	n := int(r.u8())
	b := r.bytes(n)
	if r.err != nil {
		return ""
	}
	if !utf8.Valid(b) {
		r.err = errMalformed
		return ""
	}
	return string(b)
}

// finish reports the first error, or an error if bytes remain unread.
func (r *reader) finish() error {
	if r.err == nil && len(r.b) != 0 {
		r.err = errMalformed
	}
	return r.err
}

// appendStr writes a u8 length and the bytes of s. Callers keep len(s) <= 255.
func appendStr(b []byte, s string) []byte {
	b = append(b, byte(len(s)))
	return append(b, s...)
}

// CHAT limits. The text is 1 to 256 bytes of UTF-8 with no bytes below 0x20
// and no 0x7F.
const (
	minChatLen = 1
	maxChatLen = 256
)

// parseChat decodes a client CHAT payload: u16 length, then the text. It
// reports false for a bad length, invalid UTF-8, or a control byte.
func parseChat(p []byte) (string, bool) {
	r := &reader{b: p}
	n := int(r.u16())
	if r.err != nil || n < minChatLen || n > maxChatLen {
		return "", false
	}
	b := r.bytes(n)
	if r.finish() != nil || !utf8.Valid(b) {
		return "", false
	}
	for _, c := range b {
		if c < 0x20 || c == 0x7F {
			return "", false
		}
	}
	return string(b), true
}

// chatFwdPayload builds a relayed CHAT payload: u8 from_player, u16 length,
// then the text.
func chatFwdPayload(from uint8, text string) []byte {
	b := append([]byte{from}, binary.BigEndian.AppendUint16(nil, uint16(len(text)))...)
	return append(b, text...)
}

// encode builds one TCP frame: u32 length (type plus payload), type, payload.
func encode(typ byte, payload []byte) []byte {
	out := make([]byte, 4, 5+len(payload))
	binary.BigEndian.PutUint32(out, uint32(1+len(payload)))
	out = append(out, typ)
	return append(out, payload...)
}

// readFrame reads one TCP frame. It rejects lengths of zero or above maxFrameLen.
func readFrame(br *bufio.Reader) (byte, []byte, error) {
	var hdr [4]byte
	if _, err := io.ReadFull(br, hdr[:]); err != nil {
		return 0, nil, err
	}
	n := binary.BigEndian.Uint32(hdr[:])
	if n == 0 || n > maxFrameLen {
		return 0, nil, errFrameSize
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(br, buf); err != nil {
		return 0, nil, err
	}
	return buf[0], buf[1:], nil
}
