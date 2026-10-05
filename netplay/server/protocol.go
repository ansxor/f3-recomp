package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
)

const (
	Magic           uint32 = 0x46334E50 // 'F', '3', 'N', 'P'
	ProtocolVersion uint8  = 2

	HeaderSize   = 20
	IdentitySize = 64 // ROM CRCs (28), build hash (32), state format (4)

	MaxPacketSize = 1400
	MaxInputs     = 128
	MaxChecksums  = 32

	InputMask uint16 = 0x07FF // 11 active-high bits: 0..3 dir, 4..6 btn, 7 start, 8 coin, 9 service, 10 test
)

// Packet Types
const (
	PktJoinReq         uint8 = 1
	PktJoinWait        uint8 = 2
	PktJoinAck         uint8 = 3
	PktJoinReject      uint8 = 4
	PktMatchStart      uint8 = 5
	PktGameData        uint8 = 6
	PktHeartbeat       uint8 = 7
	PktLeave           uint8 = 8
	PktMatchTerminated uint8 = 9
	PktMatchComplete   uint8 = 10
	PktSnapshotMeta    uint8 = 11
	PktSnapshotChunk   uint8 = 12
	PktSnapshotAck     uint8 = 13
	PktSnapshotLoaded  uint8 = 14
	PktBarrierStart    uint8 = 15
)

// Reject Reasons
const (
	RejectProtocolMismatch    uint8 = 1
	RejectRoomFull            uint8 = 2
	RejectSlotTaken           uint8 = 3
	RejectRomCrcMismatch      uint8 = 4
	RejectBuildHashMismatch   uint8 = 5
	RejectStateFormatMismatch uint8 = 6
	RejectInvalidRoom         uint8 = 10
	RejectRateLimited         uint8 = 11
	RejectMatchInProgress     uint8 = 12
	RejectInvalidIdentity     uint8 = 13
	RejectRoleConflict        uint8 = 14
)

// GameData Flags
const (
	FlagFinishReq uint16 = 1 << 0
	FlagFinishAck uint16 = 1 << 1
)

func RejectReasonString(code uint8) string {
	switch code {
	case RejectProtocolMismatch:
		return "protocol mismatch"
	case RejectRoomFull:
		return "room is full (max 2 players)"
	case RejectSlotTaken:
		return "requested slot is already taken"
	case RejectRomCrcMismatch:
		return "ROM CRC mismatch"
	case RejectBuildHashMismatch:
		return "build hash mismatch"
	case RejectStateFormatMismatch:
		return "snapshot state format mismatch"
	case RejectRoleConflict:
		return "room requires one host and one guest"
	case RejectInvalidRoom:
		return "invalid room name"
	case RejectRateLimited:
		return "rate limited"
	case RejectMatchInProgress:
		return "match already in progress"
	case RejectInvalidIdentity:
		return "invalid identity payload"
	default:
		return fmt.Sprintf("unknown rejection (%d)", code)
	}
}

// Identity matches the C++ struct f3rt::netplay::Identity
type Identity struct {
	RomCRC      [7]uint32
	BuildHash   [32]uint8
	StateFormat uint32
}

func (id Identity) Equal(other Identity) bool {
	return id == other
}

func (id Identity) Marshal() []byte {
	buf := make([]byte, IdentitySize)
	for i := range 7 {
		binary.BigEndian.PutUint32(buf[i*4:(i+1)*4], id.RomCRC[i])
	}
	copy(buf[28:60], id.BuildHash[:])
	binary.BigEndian.PutUint32(buf[60:64], id.StateFormat)
	return buf
}

func UnmarshalIdentity(data []byte) (Identity, error) {
	var id Identity
	if len(data) < IdentitySize {
		return id, errors.New("identity buffer too small")
	}
	for i := range 7 {
		id.RomCRC[i] = binary.BigEndian.Uint32(data[i*4 : (i+1)*4])
	}
	copy(id.BuildHash[:], data[28:60])
	id.StateFormat = binary.BigEndian.Uint32(data[60:64])
	return id, nil
}

// Header is the 20-byte packet header
type Header struct {
	Magic      uint32
	Version    uint8
	Type       uint8
	Flags      uint16
	SessionID  uint64
	SenderSlot uint8
	Reserved   [3]uint8
}

func (h Header) Marshal() []byte {
	buf := make([]byte, HeaderSize)
	binary.BigEndian.PutUint32(buf[0:4], h.Magic)
	buf[4] = h.Version
	buf[5] = h.Type
	binary.BigEndian.PutUint16(buf[6:8], h.Flags)
	binary.BigEndian.PutUint64(buf[8:16], h.SessionID)
	buf[16] = h.SenderSlot
	copy(buf[17:20], h.Reserved[:])
	return buf
}

func UnmarshalHeader(data []byte) (Header, error) {
	var h Header
	if len(data) < HeaderSize {
		return h, errors.New("header truncated")
	}
	h.Magic = binary.BigEndian.Uint32(data[0:4])
	if h.Magic != Magic {
		return h, fmt.Errorf("invalid magic: 0x%08X", h.Magic)
	}
	h.Version = data[4]
	if h.Version != ProtocolVersion {
		return h, fmt.Errorf("unsupported protocol version: %d", h.Version)
	}
	h.Type = data[5]
	h.Flags = binary.BigEndian.Uint16(data[6:8])
	h.SessionID = binary.BigEndian.Uint64(data[8:16])
	h.SenderSlot = data[16]
	copy(h.Reserved[:], data[17:20])
	return h, nil
}

// JoinReqPayload
type JoinReqPayload struct {
	ClientNonce   uint64
	RequestedSlot uint8 // 0: auto, 1: slot 0, 2: slot 1
	Delay         uint8
	RoomName      string
	Identity      Identity
	Host          bool
}

func (p JoinReqPayload) Marshal() []byte {
	buf := make([]byte, 108)
	binary.BigEndian.PutUint64(buf[0:8], p.ClientNonce)
	buf[8] = p.RequestedSlot
	buf[9] = p.Delay
	roomBytes := []byte(p.RoomName)
	if len(roomBytes) > 32 {
		roomBytes = roomBytes[:32]
	}
	buf[10] = uint8(len(roomBytes))
	copy(buf[11:11+len(roomBytes)], roomBytes)
	copy(buf[43:43+IdentitySize], p.Identity.Marshal())
	if p.Host {
		buf[107] = 1
	}
	return buf
}

func UnmarshalJoinReqPayload(data []byte) (JoinReqPayload, error) {
	var p JoinReqPayload
	if len(data) != 108 {
		return p, errors.New("join req payload must be 108 bytes")
	}
	p.ClientNonce = binary.BigEndian.Uint64(data[0:8])
	p.RequestedSlot = data[8]
	p.Delay = data[9]
	if p.RequestedSlot > 2 || data[107] > 1 {
		return p, errors.New("invalid slot or host role")
	}
	p.Host = data[107] == 1
	rLen := int(data[10])
	if rLen == 0 || rLen > 32 {
		return p, errors.New("invalid room name length: must be between 1 and 32 characters")
	}
	// Trim null bytes
	rBytes := data[11 : 11+rLen]
	p.RoomName = string(bytes.TrimRight(rBytes, "\x00"))
	id, err := UnmarshalIdentity(data[43 : 43+IdentitySize])
	if err != nil {
		return p, err
	}
	p.Identity = id
	return p, nil
}

// JoinWaitPayload
type JoinWaitPayload struct {
	ClientNonce  uint64
	AssignedSlot uint8
	Message      string
}

func (p JoinWaitPayload) Marshal() []byte {
	buf := make([]byte, 8+1+32)
	binary.BigEndian.PutUint64(buf[0:8], p.ClientNonce)
	buf[8] = p.AssignedSlot
	msg := []byte(p.Message)
	if len(msg) > 32 {
		msg = msg[:32]
	}
	copy(buf[9:9+len(msg)], msg)
	return buf
}

func UnmarshalJoinWaitPayload(data []byte) (JoinWaitPayload, error) {
	var p JoinWaitPayload
	if len(data) < 8+1+32 {
		return p, errors.New("join wait payload truncated")
	}
	p.ClientNonce = binary.BigEndian.Uint64(data[0:8])
	p.AssignedSlot = data[8]
	p.Message = string(bytes.TrimRight(data[9:41], "\x00"))
	return p, nil
}

// JoinRejectPayload
type JoinRejectPayload struct {
	ClientNonce uint64
	RejectCode  uint8
	Message     string
}

func (p JoinRejectPayload) Marshal() []byte {
	buf := make([]byte, 8+1+64)
	binary.BigEndian.PutUint64(buf[0:8], p.ClientNonce)
	buf[8] = p.RejectCode
	msg := []byte(p.Message)
	if len(msg) > 64 {
		msg = msg[:64]
	}
	copy(buf[9:9+len(msg)], msg)
	return buf
}

func UnmarshalJoinRejectPayload(data []byte) (JoinRejectPayload, error) {
	var p JoinRejectPayload
	if len(data) < 8+1+64 {
		return p, errors.New("join reject payload truncated")
	}
	p.ClientNonce = binary.BigEndian.Uint64(data[0:8])
	p.RejectCode = data[8]
	p.Message = string(bytes.TrimRight(data[9:73], "\x00"))
	return p, nil
}

// MatchStartPayload
type MatchStartPayload struct {
	ClientNonce  uint64
	AssignedSlot uint8
	Delay        uint8
	PeerIdentity Identity
}

func (p MatchStartPayload) Marshal() []byte {
	buf := make([]byte, 8+1+1+2+IdentitySize)
	binary.BigEndian.PutUint64(buf[0:8], p.ClientNonce)
	buf[8] = p.AssignedSlot
	buf[9] = p.Delay
	// buf[10:12] reserved
	copy(buf[12:12+IdentitySize], p.PeerIdentity.Marshal())
	return buf
}

func UnmarshalMatchStartPayload(data []byte) (MatchStartPayload, error) {
	var p MatchStartPayload
	if len(data) < 8+1+1+2+IdentitySize {
		return p, errors.New("match start payload truncated")
	}
	p.ClientNonce = binary.BigEndian.Uint64(data[0:8])
	p.AssignedSlot = data[8]
	p.Delay = data[9]
	id, err := UnmarshalIdentity(data[12 : 12+IdentitySize])
	if err != nil {
		return p, err
	}
	p.PeerIdentity = id
	return p, nil
}

// InputItem
type InputItem struct {
	Frame uint32
	Word  uint16
}

// ChecksumItem
type ChecksumItem struct {
	Frame uint32
	CRC   uint32
}

const (
	LeaveAbort          uint8 = 0
	LeaveNormalFinished uint8 = 1
)

// GameDataPayload
type GameDataPayload struct {
	SimulatedFrame   uint32
	AckFrame         uint32 // 0xFFFFFFFF if none
	AckChecksumFrame uint32 // 0xFFFFFFFF if none
	PingID           uint32
	PongID           uint32
	Flags            uint16
	InputStartFrame  uint32
	Inputs           []uint16
	Checksums        []ChecksumItem
	FinishFrame      uint32
	FinishCRC        uint32
}

func (p GameDataPayload) Marshal() []byte {
	nInputs := len(p.Inputs)
	nChecksums := len(p.Checksums)
	totalLen := 4 + 4 + 4 + 4 + 4 + 2 + 4 + 2 + nInputs*2 + 2 + nChecksums*8
	if p.Flags&FlagFinishReq != 0 {
		totalLen += 8
	}
	buf := make([]byte, totalLen)
	offset := 0
	binary.BigEndian.PutUint32(buf[offset:offset+4], p.SimulatedFrame)
	offset += 4
	binary.BigEndian.PutUint32(buf[offset:offset+4], p.AckFrame)
	offset += 4
	binary.BigEndian.PutUint32(buf[offset:offset+4], p.AckChecksumFrame)
	offset += 4
	binary.BigEndian.PutUint32(buf[offset:offset+4], p.PingID)
	offset += 4
	binary.BigEndian.PutUint32(buf[offset:offset+4], p.PongID)
	offset += 4
	binary.BigEndian.PutUint16(buf[offset:offset+2], p.Flags)
	offset += 2
	binary.BigEndian.PutUint32(buf[offset:offset+4], p.InputStartFrame)
	offset += 4
	binary.BigEndian.PutUint16(buf[offset:offset+2], uint16(nInputs))
	offset += 2
	for _, w := range p.Inputs {
		binary.BigEndian.PutUint16(buf[offset:offset+2], w)
		offset += 2
	}
	binary.BigEndian.PutUint16(buf[offset:offset+2], uint16(nChecksums))
	offset += 2
	for _, cs := range p.Checksums {
		binary.BigEndian.PutUint32(buf[offset:offset+4], cs.Frame)
		offset += 4
		binary.BigEndian.PutUint32(buf[offset:offset+4], cs.CRC)
		offset += 4
	}
	if p.Flags&FlagFinishReq != 0 {
		binary.BigEndian.PutUint32(buf[offset:offset+4], p.FinishFrame)
		offset += 4
		binary.BigEndian.PutUint32(buf[offset:offset+4], p.FinishCRC)
		offset += 4
	}
	return buf
}

func UnmarshalGameDataPayload(data []byte) (GameDataPayload, error) {
	var p GameDataPayload
	minLen := 4 + 4 + 4 + 4 + 4 + 2 + 4 + 2 + 2
	if len(data) < minLen {
		return p, errors.New("game data payload truncated")
	}
	offset := 0
	p.SimulatedFrame = binary.BigEndian.Uint32(data[offset : offset+4])
	offset += 4
	p.AckFrame = binary.BigEndian.Uint32(data[offset : offset+4])
	offset += 4
	p.AckChecksumFrame = binary.BigEndian.Uint32(data[offset : offset+4])
	offset += 4
	p.PingID = binary.BigEndian.Uint32(data[offset : offset+4])
	offset += 4
	p.PongID = binary.BigEndian.Uint32(data[offset : offset+4])
	offset += 4
	p.Flags = binary.BigEndian.Uint16(data[offset : offset+2])
	offset += 2
	if p.Flags&^(FlagFinishReq|FlagFinishAck) != 0 {
		return p, errors.New("unknown game-data flags")
	}
	p.InputStartFrame = binary.BigEndian.Uint32(data[offset : offset+4])
	offset += 4
	nInputs := int(binary.BigEndian.Uint16(data[offset : offset+2]))
	offset += 2
	if nInputs > MaxInputs {
		return p, fmt.Errorf("too many inputs: %d > %d", nInputs, MaxInputs)
	}
	if uint64(p.InputStartFrame)+uint64(nInputs) > 0xffffffff {
		return p, errors.New("input frame range overflow")
	}
	if offset+nInputs*2 > len(data) {
		return p, errors.New("truncated inputs")
	}
	p.Inputs = make([]uint16, nInputs)
	for i := range nInputs {
		p.Inputs[i] = binary.BigEndian.Uint16(data[offset : offset+2])
		if p.Inputs[i]&^InputMask != 0 {
			return p, errors.New("invalid input bits")
		}
		offset += 2
	}
	if offset+2 > len(data) {
		return p, errors.New("truncated checksum count")
	}
	nChecksums := int(binary.BigEndian.Uint16(data[offset : offset+2]))
	offset += 2
	if nChecksums > MaxChecksums {
		return p, fmt.Errorf("too many checksums: %d > %d", nChecksums, MaxChecksums)
	}
	if offset+nChecksums*8 > len(data) {
		return p, errors.New("truncated checksums")
	}
	p.Checksums = make([]ChecksumItem, nChecksums)
	for i := range nChecksums {
		p.Checksums[i].Frame = binary.BigEndian.Uint32(data[offset : offset+4])
		offset += 4
		p.Checksums[i].CRC = binary.BigEndian.Uint32(data[offset : offset+4])
		offset += 4
	}
	if p.Flags&FlagFinishReq != 0 {
		if offset+8 > len(data) {
			return p, errors.New("truncated finish info")
		}
		p.FinishFrame = binary.BigEndian.Uint32(data[offset : offset+4])
		offset += 4
		p.FinishCRC = binary.BigEndian.Uint32(data[offset : offset+4])
		offset += 4
	}
	if offset != len(data) {
		return p, errors.New("trailing game-data bytes")
	}
	return p, nil
}

// HeartbeatPayload
type HeartbeatPayload struct {
	PingID         uint32
	PongID         uint32
	SimulatedFrame uint32
	AckFrame       uint32
}

func (p HeartbeatPayload) Marshal() []byte {
	buf := make([]byte, 16)
	binary.BigEndian.PutUint32(buf[0:4], p.PingID)
	binary.BigEndian.PutUint32(buf[4:8], p.PongID)
	binary.BigEndian.PutUint32(buf[8:12], p.SimulatedFrame)
	binary.BigEndian.PutUint32(buf[12:16], p.AckFrame)
	return buf
}

func UnmarshalHeartbeatPayload(data []byte) (HeartbeatPayload, error) {
	var p HeartbeatPayload
	if len(data) < 16 {
		return p, errors.New("heartbeat payload truncated")
	}
	p.PingID = binary.BigEndian.Uint32(data[0:4])
	p.PongID = binary.BigEndian.Uint32(data[4:8])
	p.SimulatedFrame = binary.BigEndian.Uint32(data[8:12])
	p.AckFrame = binary.BigEndian.Uint32(data[12:16])
	return p, nil
}

// TerminatePayload
type TerminatePayload struct {
	ReasonCode uint8
	Message    string
}

func (p TerminatePayload) Marshal() []byte {
	buf := make([]byte, 1+64)
	buf[0] = p.ReasonCode
	msg := []byte(p.Message)
	if len(msg) > 64 {
		msg = msg[:64]
	}
	copy(buf[1:1+len(msg)], msg)
	return buf
}

func UnmarshalTerminatePayload(data []byte) (TerminatePayload, error) {
	var p TerminatePayload
	if len(data) < 1+64 {
		return p, errors.New("terminate payload truncated")
	}
	p.ReasonCode = data[0]
	p.Message = string(bytes.TrimRight(data[1:65], "\x00"))
	return p, nil
}

// LeavePayload
type LeavePayload struct {
	LeaveCode uint8
	Message   string
}

func (p LeavePayload) Marshal() []byte {
	buf := make([]byte, 1+64)
	buf[0] = p.LeaveCode
	msg := []byte(p.Message)
	if len(msg) > 64 {
		msg = msg[:64]
	}
	copy(buf[1:1+len(msg)], msg)
	return buf
}

func UnmarshalLeavePayload(data []byte) (LeavePayload, error) {
	var p LeavePayload
	if len(data) < 1 {
		return p, errors.New("leave payload truncated")
	}
	p.LeaveCode = data[0]
	if len(data) > 1 {
		msgLen := len(data) - 1
		if msgLen > 64 {
			msgLen = 64
		}
		p.Message = string(bytes.TrimRight(data[1:1+msgLen], "\x00"))
	}
	return p, nil
}

// PeekHeader inspects raw packet bytes for magic, version, packet type and nonce before full unmarshaling
func PeekHeader(data []byte) (magic uint32, version uint8, pktType uint8, nonce uint64, err error) {
	if len(data) < HeaderSize {
		return 0, 0, 0, 0, errors.New("packet truncated")
	}
	magic = binary.BigEndian.Uint32(data[0:4])
	version = data[4]
	pktType = data[5]
	if len(data) >= HeaderSize+8 {
		nonce = binary.BigEndian.Uint64(data[HeaderSize : HeaderSize+8])
	}
	return magic, version, pktType, nonce, nil
}

// MatchCompletePayload (Type 10: Server-origin verified match verdict)
type MatchCompletePayload struct {
	FinalFrame uint32
	FinalCRC   uint32
}

func (p MatchCompletePayload) Marshal() []byte {
	buf := make([]byte, 8)
	binary.BigEndian.PutUint32(buf[0:4], p.FinalFrame)
	binary.BigEndian.PutUint32(buf[4:8], p.FinalCRC)
	return buf
}

func UnmarshalMatchCompletePayload(data []byte) (MatchCompletePayload, error) {
	var p MatchCompletePayload
	if len(data) < 8 {
		return p, errors.New("match complete payload truncated")
	}
	p.FinalFrame = binary.BigEndian.Uint32(data[0:4])
	p.FinalCRC = binary.BigEndian.Uint32(data[4:8])
	return p, nil
}

// IsValidRoomName checks length 1..32 and characters [a-zA-Z0-9_-]
func IsValidRoomName(name string) bool {
	if len(name) == 0 || len(name) > 32 {
		return false
	}
	for i := range len(name) {
		c := name[i]
		if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
			return false
		}
	}
	return true
}

const (
	SnapshotTransferID uint32 = 1
	MaxSnapshotSize    uint32 = 16 * 1024 * 1024
	SnapshotChunkSize  uint32 = 1024
)

// SnapshotMetaPayload bounds the relay without retaining snapshot bytes.
type SnapshotMetaPayload struct {
	TransferID     uint32
	RawSize        uint32
	CompressedSize uint32
	RawCRC         uint32
	CompressedCRC  uint32
}

func (p SnapshotMetaPayload) Marshal() []byte {
	buf := make([]byte, 20)
	binary.BigEndian.PutUint32(buf[0:4], p.TransferID)
	binary.BigEndian.PutUint32(buf[4:8], p.RawSize)
	binary.BigEndian.PutUint32(buf[8:12], p.CompressedSize)
	binary.BigEndian.PutUint32(buf[12:16], p.RawCRC)
	binary.BigEndian.PutUint32(buf[16:20], p.CompressedCRC)
	return buf
}

func UnmarshalSnapshotMetaPayload(data []byte) (SnapshotMetaPayload, error) {
	var p SnapshotMetaPayload
	if len(data) != 20 {
		return p, errors.New("snapshot metadata must be 20 bytes")
	}
	p.TransferID = binary.BigEndian.Uint32(data[0:4])
	p.RawSize = binary.BigEndian.Uint32(data[4:8])
	p.CompressedSize = binary.BigEndian.Uint32(data[8:12])
	p.RawCRC = binary.BigEndian.Uint32(data[12:16])
	p.CompressedCRC = binary.BigEndian.Uint32(data[16:20])
	if p.TransferID != SnapshotTransferID || p.RawSize == 0 || p.RawSize > MaxSnapshotSize || p.CompressedSize == 0 || p.CompressedSize > MaxSnapshotSize {
		return p, errors.New("invalid snapshot transfer or sizes")
	}
	return p, nil
}

// SnapshotReceiptPayload is used for Loaded and BarrierStart.
type SnapshotReceiptPayload struct {
	TransferID uint32
	RawCRC     uint32
}

func (p SnapshotReceiptPayload) Marshal() []byte {
	buf := make([]byte, 8)
	binary.BigEndian.PutUint32(buf[0:4], p.TransferID)
	binary.BigEndian.PutUint32(buf[4:8], p.RawCRC)
	return buf
}

func UnmarshalSnapshotReceiptPayload(data []byte) (SnapshotReceiptPayload, error) {
	if len(data) != 8 {
		return SnapshotReceiptPayload{}, errors.New("snapshot receipt must be 8 bytes")
	}
	return SnapshotReceiptPayload{
		TransferID: binary.BigEndian.Uint32(data[0:4]),
		RawCRC:     binary.BigEndian.Uint32(data[4:8]),
	}, nil
}
