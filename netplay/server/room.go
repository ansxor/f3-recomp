package main

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"net"
	"sync"
	"time"
)

type RoomState int

const (
	RoomWaiting RoomState = iota
	RoomActive
	RoomFinished
	RoomTerminated
)

const RetiredNonceLifetime = 120 * time.Second

type retiredNonce struct {
	Nonce uint64
	Until time.Time
}

type ClientSlot struct {
	Occupied    bool
	Addr        *net.UDPAddr
	Nonce       uint64
	LastActive  time.Time
	Finished    bool
	FinishFrame uint32
	FinishCRC   uint32
	Host        bool
}

type Room struct {
	mu sync.RWMutex

	Name             string
	State            RoomState
	SessionID        uint64
	Delay            uint8
	Identity         Identity
	Clients          [2]ClientSlot
	FinalFrame       uint32
	FinalCRC         uint32
	SnapshotMeta     *SnapshotMetaPayload
	BarrierReleased  bool
	BarrierConfirmed [2]bool
	Retired          [64]retiredNonce

	CreatedAt  time.Time
	LastActive time.Time
}

func NewRoom(name string) *Room {
	now := time.Now()
	return &Room{
		Name:       name,
		State:      RoomWaiting,
		CreatedAt:  now,
		LastActive: now,
	}
}

func generateSessionID() uint64 {
	var buf [8]byte
	_, err := rand.Read(buf[:])
	if err != nil {
		return uint64(time.Now().UnixNano())
	}
	id := binary.BigEndian.Uint64(buf[:])
	if id == 0 {
		return 1
	}
	return id
}

// JoinResult contains the outcome of a join request
type JoinResult struct {
	RejectCode   uint8
	AssignedSlot uint8
	SessionID    uint64
	OldSessionID uint64
	MatchStarted bool
	PeerAddr     *net.UDPAddr
	PeerSlot     uint8
	PeerNonce    uint64
	Delay        uint8
	PeerIdentity Identity
}

// ProcessJoin processes an incoming JoinReqPayload
func (r *Room) ProcessJoin(addr *net.UDPAddr, req JoinReqPayload) JoinResult {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := time.Now()
	for i := range r.Retired {
		if !r.Retired[i].Until.After(now) {
			r.Retired[i] = retiredNonce{}
		} else if r.Retired[i].Nonce == req.ClientNonce {
			return JoinResult{RejectCode: RejectInvalidIdentity}
		}
	}

	if r.State == RoomActive || r.State == RoomFinished {
		for i := range 2 {
			if r.Clients[i].Occupied && r.Clients[i].Nonce == req.ClientNonce {
				// Retries must retain role and the host's negotiated delay.
				if !req.Identity.Equal(r.Identity) || req.Host != r.Clients[i].Host || (req.Host && req.Delay != r.Delay) {
					return JoinResult{RejectCode: RejectInvalidIdentity}
				}
				r.LastActive = now
				r.Clients[i].Addr = addr
				r.Clients[i].LastActive = now
				peerSlot := 1 - i
				return JoinResult{
					AssignedSlot: uint8(i),
					SessionID:    r.SessionID,
					MatchStarted: true,
					Delay:        r.Delay,
					PeerIdentity: r.Identity,
					PeerAddr:     r.Clients[peerSlot].Addr,
					PeerSlot:     uint8(peerSlot),
					PeerNonce:    r.Clients[peerSlot].Nonce,
				}
			}
		}
		return JoinResult{
			RejectCode: RejectMatchInProgress,
		}
	}

	var oldSessionID uint64
	if r.State == RoomTerminated {
		oldSessionID = r.SessionID
		r.State = RoomWaiting
		r.Clients[0] = ClientSlot{}
		r.Clients[1] = ClientSlot{}
		r.SessionID = 0
		r.SnapshotMeta = nil
		r.BarrierReleased = false
		r.BarrierConfirmed = [2]bool{}
		r.FinalFrame, r.FinalCRC = 0, 0
	}

	// In RoomWaiting:
	occupiedCount := 0
	for i := range 2 {
		if r.Clients[i].Occupied {
			occupiedCount++
			// Check if client is retransmitting JoinReq while waiting
			if r.Clients[i].Nonce == req.ClientNonce {
				if !req.Identity.Equal(r.Identity) || req.Host != r.Clients[i].Host || (req.Host && req.Delay != r.Delay) {
					return JoinResult{RejectCode: RejectInvalidIdentity}
				}
				r.LastActive = now
				r.Clients[i].Addr = addr
				r.Clients[i].LastActive = now
				return JoinResult{
					AssignedSlot: uint8(i),
					SessionID:    0,
					MatchStarted: false,
					Delay:        r.Delay,
				}
			}
		}
	}

	if occupiedCount >= 2 {
		return JoinResult{RejectCode: RejectRoomFull}
	}
	// Reserve two tombstone positions for the occupants of this session.
	retiredCount := 0
	for _, retired := range r.Retired {
		if retired.Until.After(now) {
			retiredCount++
		}
	}
	if retiredCount >= len(r.Retired)-2 {
		return JoinResult{RejectCode: RejectRateLimited}
	}

	// Pair only compatible builds with complementary roles.
	if occupiedCount == 1 {
		for i := range 2 {
			if r.Clients[i].Occupied && r.Clients[i].Host == req.Host {
				return JoinResult{RejectCode: RejectRoleConflict}
			}
		}
		if req.Identity.RomCRC != r.Identity.RomCRC {
			return JoinResult{RejectCode: RejectRomCrcMismatch}
		}
		if req.Identity.BuildHash != r.Identity.BuildHash {
			return JoinResult{RejectCode: RejectBuildHashMismatch}
		}
		if req.Identity.StateFormat != r.Identity.StateFormat {
			return JoinResult{RejectCode: RejectStateFormatMismatch}
		}
	}

	// Slot assignment:
	// RequestedSlot: 0=auto, 1=slot 0, 2=slot 1
	var targetSlot int = -1
	if req.RequestedSlot == 1 {
		if r.Clients[0].Occupied {
			return JoinResult{RejectCode: RejectSlotTaken}
		}
		targetSlot = 0
	} else if req.RequestedSlot == 2 {
		if r.Clients[1].Occupied {
			return JoinResult{RejectCode: RejectSlotTaken}
		}
		targetSlot = 1
	} else {
		// Auto
		if !r.Clients[0].Occupied {
			targetSlot = 0
		} else if !r.Clients[1].Occupied {
			targetSlot = 1
		} else {
			return JoinResult{RejectCode: RejectRoomFull}
		}
	}

	// Assign slot
	r.LastActive = now
	r.Clients[targetSlot] = ClientSlot{
		Occupied:   true,
		Addr:       addr,
		Nonce:      req.ClientNonce,
		LastActive: now,
		Host:       req.Host,
	}

	if req.Host {
		r.Delay = req.Delay
	}
	if occupiedCount == 0 {
		r.CreatedAt = now
		// First player in room sets the room baseline
		r.Identity = req.Identity
		if req.Host {
			r.Delay = req.Delay
		}
		return JoinResult{
			RejectCode:   0,
			AssignedSlot: uint8(targetSlot),
			SessionID:    0,
			OldSessionID: oldSessionID,
			MatchStarted: false,
			Delay:        r.Delay,
		}
	}

	// Second player joined and passed all validation checks!
	// Match starts!
	r.SessionID = generateSessionID()
	r.State = RoomActive

	peerSlot := 1 - targetSlot
	return JoinResult{
		RejectCode:   0,
		AssignedSlot: uint8(targetSlot),
		SessionID:    r.SessionID,
		OldSessionID: oldSessionID,
		MatchStarted: true,
		Delay:        r.Delay,
		PeerIdentity: r.Identity,
		PeerAddr:     r.Clients[peerSlot].Addr,
		PeerSlot:     uint8(peerSlot),
		PeerNonce:    r.Clients[peerSlot].Nonce,
	}
}

// GetPeer returns the peer address and slot for a given sender slot, validating session ID and endpoint
func (r *Room) GetPeer(slot uint8, sessionID uint64, addr *net.UDPAddr) (*net.UDPAddr, error) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.State != RoomActive && r.State != RoomFinished {
		return nil, errors.New("room is not active")
	}
	if r.SessionID != sessionID {
		return nil, errors.New("invalid session ID")
	}
	if slot > 1 {
		return nil, errors.New("invalid slot index")
	}
	if !r.Clients[slot].Occupied || r.Clients[slot].Addr == nil || r.Clients[slot].Addr.String() != addr.String() {
		return nil, errors.New("sender endpoint mismatch for slot (anti-spoofing)")
	}

	now := time.Now()
	r.Clients[slot].LastActive = now
	r.LastActive = now

	peerSlot := 1 - slot
	if !r.Clients[peerSlot].Occupied || r.Clients[peerSlot].Addr == nil {
		// If room is finished, peer may have gracefully exited after completion;
		// return peer's last known address so lingering completion/finish packets can be delivered
		if r.State == RoomFinished && r.Clients[peerSlot].Addr != nil {
			return r.Clients[peerSlot].Addr, nil
		}
		return nil, errors.New("peer not connected")
	}
	return r.Clients[peerSlot].Addr, nil
}

// MarkFinish records finish data and returns completion details if both finished matching
func (r *Room) MarkFinish(slot uint8, addr *net.UDPAddr, frame uint32, crc uint32) (bothFinished bool, finalFrame uint32, finalCRC uint32, addr0 *net.UDPAddr, addr1 *net.UDPAddr) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if slot > 1 || !r.BarrierReleased || !r.Clients[slot].Occupied {
		return false, 0, 0, nil, nil
	}
	// Anti-spoofing: sender endpoint must match registered slot endpoint
	if r.Clients[slot].Addr == nil || r.Clients[slot].Addr.String() != addr.String() {
		return false, 0, 0, nil, nil
	}
	r.Clients[slot].Finished = true
	r.Clients[slot].FinishFrame = frame
	r.Clients[slot].FinishCRC = crc

	peerSlot := 1 - slot
	if r.Clients[peerSlot].Finished {
		if r.Clients[peerSlot].FinishCRC == crc && r.Clients[peerSlot].FinishFrame == frame {
			r.State = RoomFinished
			r.FinalFrame = frame
			r.FinalCRC = crc
			return true, frame, crc, r.Clients[0].Addr, r.Clients[1].Addr
		}
	}
	if r.State == RoomFinished {
		return true, r.FinalFrame, r.FinalCRC, r.Clients[0].Addr, r.Clients[1].Addr
	}
	return false, 0, 0, nil, nil
}

func (r *Room) GetFinishVerdict(addr *net.UDPAddr) (bool, uint32, uint32) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if r.State == RoomFinished {
		if addr != nil {
			for i := range 2 {
				if r.Clients[i].Addr != nil && r.Clients[i].Addr.String() == addr.String() {
					return true, r.FinalFrame, r.FinalCRC
				}
			}
		} else {
			return true, r.FinalFrame, r.FinalCRC
		}
	}
	return false, 0, 0
}

type RoomSnapshot struct {
	Name       string
	State      RoomState
	CreatedAt  time.Time
	LastActive time.Time
	SessionID  uint64
	FinalFrame uint32
	FinalCRC   uint32
}

func (r *Room) Snapshot() RoomSnapshot {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return RoomSnapshot{
		Name:       r.Name,
		State:      r.State,
		CreatedAt:  r.CreatedAt,
		LastActive: r.LastActive,
		SessionID:  r.SessionID,
		FinalFrame: r.FinalFrame,
		FinalCRC:   r.FinalCRC,
	}
}

// HandleLeave validates session and endpoint before processing leave.
// If the match is already finished and leaveCode is LeaveNormalFinished, peer is not terminated.
func (r *Room) HandleLeave(slot uint8, sessionID uint64, addr *net.UDPAddr, leaveCode uint8) (*net.UDPAddr, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if slot > 1 || !r.Clients[slot].Occupied || r.Clients[slot].Addr == nil {
		return nil, false
	}
	if r.SessionID != sessionID {
		return nil, false
	}
	if r.Clients[slot].Addr.String() != addr.String() {
		return nil, false // Invalid sender endpoint
	}

	r.Clients[slot].Occupied = false
	r.retireNonce(r.Clients[slot].Nonce, time.Now())

	if r.State == RoomFinished && leaveCode == LeaveNormalFinished {
		// Graceful exit after successful completion does NOT terminate the match or the other peer!
		if !r.Clients[0].Occupied && !r.Clients[1].Occupied {
			r.State = RoomTerminated
			return nil, true
		}
		return nil, false
	}

	r.State = RoomTerminated
	r.retireNonce(r.Clients[1-slot].Nonce, time.Now())
	peerSlot := 1 - slot
	if r.Clients[peerSlot].Occupied && r.Clients[peerSlot].Addr != nil {
		return r.Clients[peerSlot].Addr, true
	}
	return nil, true
}

func (r *Room) IsFinished() (bool, uint32, uint32) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if r.State == RoomFinished {
		return true, r.Clients[0].FinishFrame, r.Clients[0].FinishCRC
	}
	return false, 0, 0
}

// CheckTimeouts retires silent active or completed sessions, including lost final Leave packets.
func (r *Room) CheckTimeouts(clientTimeout time.Duration) (timedOutSlot int, notifyAddr *net.UDPAddr) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.State != RoomActive && r.State != RoomFinished {
		return -1, nil
	}

	now := time.Now()
	for i := range 2 {
		if r.Clients[i].Occupied {
			if now.Sub(r.Clients[i].LastActive) > clientTimeout {
				r.State = RoomTerminated
				r.retireNonce(r.Clients[0].Nonce, now)
				r.retireNonce(r.Clients[1].Nonce, now)
				peerSlot := 1 - i
				if r.Clients[peerSlot].Occupied {
					return i, r.Clients[peerSlot].Addr
				}
				return i, nil
			}
		}
	}
	return -1, nil
}

// RelaySnapshot validates authority and bounds before forwarding. Identical
// metadata retries are accepted; a transfer cannot be replaced within a session.
func (r *Room) RelaySnapshot(slot uint8, packetType uint8, payload []byte) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.State != RoomActive || slot > 1 || !r.Clients[slot].Occupied {
		return false
	}
	host := r.Clients[slot].Host
	switch packetType {
	case PktSnapshotMeta:
		if !host {
			return false
		}
		meta, err := UnmarshalSnapshotMetaPayload(payload)
		if err != nil || (r.SnapshotMeta != nil && *r.SnapshotMeta != meta) {
			return false
		}
		if r.SnapshotMeta == nil {
			r.SnapshotMeta = &meta
		}
		return true
	case PktSnapshotChunk, PktSnapshotAck:
		if (packetType == PktSnapshotChunk) != host || r.SnapshotMeta == nil || len(payload) < 8 {
			return false
		}
		transferID := binary.BigEndian.Uint32(payload[0:4])
		index := binary.BigEndian.Uint32(payload[4:8])
		meta := r.SnapshotMeta
		count := (meta.CompressedSize + SnapshotChunkSize - 1) / SnapshotChunkSize
		if transferID != meta.TransferID || index >= count {
			return false
		}
		if packetType == PktSnapshotAck {
			return len(payload) == 8
		}
		size := min(SnapshotChunkSize, meta.CompressedSize-index*SnapshotChunkSize)
		return len(payload) == 8+int(size)
	case PktSnapshotLoaded:
		if host || r.SnapshotMeta == nil {
			return false
		}
		loaded, err := UnmarshalSnapshotReceiptPayload(payload)
		if err != nil || loaded.TransferID != r.SnapshotMeta.TransferID || loaded.RawCRC != r.SnapshotMeta.RawCRC {
			return false
		}
		r.BarrierReleased = true
		return true
	}
	return false
}

// BarrierRetries returns only peers whose game traffic has not yet proved they
// received start. Heartbeats also run before start and cannot acknowledge it.
func (r *Room) BarrierRetries() (uint64, SnapshotReceiptPayload, [2]*net.UDPAddr) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	var addresses [2]*net.UDPAddr
	var receipt SnapshotReceiptPayload
	if r.BarrierReleased && r.SnapshotMeta != nil && r.State == RoomActive {
		receipt = SnapshotReceiptPayload{TransferID: r.SnapshotMeta.TransferID, RawCRC: r.SnapshotMeta.RawCRC}
		for i := range 2 {
			if r.Clients[i].Occupied && !r.BarrierConfirmed[i] {
				addresses[i] = r.Clients[i].Addr
			}
		}
	}
	return r.SessionID, receipt, addresses
}

func (r *Room) AcceptGameData(slot uint8) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if !r.BarrierReleased || slot > 1 || !r.Clients[slot].Occupied {
		return false
	}
	r.BarrierConfirmed[slot] = true
	return true
}

// CancelWaiting authenticates session-zero cancellation by nonce and endpoint.
func (r *Room) CancelWaiting(addr *net.UDPAddr, nonce uint64) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.State != RoomWaiting {
		return false
	}
	for i := range 2 {
		client := &r.Clients[i]
		if client.Occupied && client.Nonce == nonce && client.Addr != nil && client.Addr.String() == addr.String() {
			r.retireNonce(client.Nonce, time.Now())
			*client = ClientSlot{}
			r.Identity = Identity{}
			r.Delay = 0
			r.LastActive = time.Now()
			return true
		}
	}
	return false
}

// Caller holds r.mu. Capacity is reserved before admitting new occupants.
func (r *Room) retireNonce(nonce uint64, now time.Time) {
	for i := range r.Retired {
		entry := &r.Retired[i]
		if entry.Nonce == nonce && entry.Until.After(now) {
			return
		}
	}
	for i := range r.Retired {
		if !r.Retired[i].Until.After(now) {
			r.Retired[i] = retiredNonce{Nonce: nonce, Until: now.Add(RetiredNonceLifetime)}
			return
		}
	}
}

// Preserve bounded tombstones beyond normal room expiry.
func (r *Room) CanRemove(now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	eligible := (r.State == RoomWaiting && now.Sub(r.CreatedAt) > WaitingRoomTimeout) ||
		((r.State == RoomFinished || r.State == RoomTerminated) && now.Sub(r.LastActive) > FinishedRoomTimeout)
	if !eligible {
		return false
	}
	for i := range 2 {
		if r.Clients[i].Occupied {
			r.retireNonce(r.Clients[i].Nonce, now)
			r.Clients[i].Occupied = false
			r.State = RoomTerminated
		}
	}
	for _, entry := range r.Retired {
		if entry.Until.After(now) {
			return false
		}
	}
	return true
}
