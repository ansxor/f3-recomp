package main

import (
	"errors"
	"fmt"
	"net"
	"sync"
	"time"
)

const (
	MaxRooms            = 1024
	ClientTimeout       = 8 * time.Second
	WaitingRoomTimeout  = 60 * time.Second
	FinishedRoomTimeout = 30 * time.Second

	MaxTrackedIPs         = 4096
	RateLimitTokensPerSec = 500.0
	RateLimitBurst        = 250.0
)

type ServerConfig struct {
	Addr       string
	Impairment ImpairmentConfig
}

type ipRateLimiter struct {
	tokens   float64
	lastTime time.Time
}

func (rl *ipRateLimiter) allow(now time.Time) bool {
	elapsed := now.Sub(rl.lastTime).Seconds()
	rl.tokens += elapsed * RateLimitTokensPerSec
	if rl.tokens > RateLimitBurst {
		rl.tokens = RateLimitBurst
	}
	rl.lastTime = now
	if rl.tokens >= 1.0 {
		rl.tokens -= 1.0
		return true
	}
	return false
}

type Server struct {
	conn       *net.UDPConn
	cfg        ServerConfig
	impairer   *Impairer
	mu         sync.RWMutex
	rooms      map[string]*Room
	sessions   map[uint64]*Room
	limiters   map[string]*ipRateLimiter
	limiterMu  sync.Mutex
	stopCh     chan struct{}
	wg         sync.WaitGroup
	listenAddr *net.UDPAddr
}

func NewServer(cfg ServerConfig) (*Server, error) {
	laddr, err := net.ResolveUDPAddr("udp", cfg.Addr)
	if err != nil {
		return nil, fmt.Errorf("resolve udp addr: %w", err)
	}

	conn, err := net.ListenUDP("udp", laddr)
	if err != nil {
		return nil, fmt.Errorf("listen udp: %w", err)
	}

	s := &Server{
		conn:       conn,
		cfg:        cfg,
		rooms:      make(map[string]*Room),
		sessions:   make(map[uint64]*Room),
		limiters:   make(map[string]*ipRateLimiter),
		stopCh:     make(chan struct{}),
		listenAddr: conn.LocalAddr().(*net.UDPAddr),
	}

	// Impairer uses s.directSend to send delayed/jittered packets
	s.impairer = NewImpairer(cfg.Impairment, s.directSend)

	s.wg.Add(2)
	go s.readLoop()
	go s.janitorLoop()

	return s, nil
}

func (s *Server) LocalAddr() *net.UDPAddr {
	return s.listenAddr
}

func (s *Server) directSend(addr *net.UDPAddr, data []byte) error {
	_, err := s.conn.WriteToUDP(data, addr)
	return err
}

func (s *Server) checkRateLimit(ip string) bool {
	s.limiterMu.Lock()
	defer s.limiterMu.Unlock()

	now := time.Now()
	rl, ok := s.limiters[ip]
	if !ok {
		if len(s.limiters) >= MaxTrackedIPs {
			for k := range s.limiters {
				delete(s.limiters, k)
				break
			}
		}
		rl = &ipRateLimiter{
			tokens:   RateLimitBurst - 1.0,
			lastTime: now,
		}
		s.limiters[ip] = rl
		return true
	}
	return rl.allow(now)
}

func (s *Server) readLoop() {
	defer s.wg.Done()

	buf := make([]byte, 2048)
	for {
		select {
		case <-s.stopCh:
			return
		default:
		}

		_ = s.conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, remoteAddr, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}
			select {
			case <-s.stopCh:
				return
			default:
				continue
			}
		}

		if n < HeaderSize || n > MaxPacketSize {
			continue // Drop malformed / oversized packet
		}

		// Rate limiting by client IP
		if !s.checkRateLimit(remoteAddr.IP.String()) {
			continue
		}

		packetData := make([]byte, n)
		copy(packetData, buf[:n])
		s.handlePacket(remoteAddr, packetData)
	}
}

func (s *Server) handlePacket(remoteAddr *net.UDPAddr, data []byte) {
	magic, version, pktType, nonce, err := PeekHeader(data)
	if err != nil || magic != Magic {
		return
	}

	// Clear rejection on protocol version mismatch
	if version != ProtocolVersion {
		if pktType == PktJoinReq {
			s.sendReject(remoteAddr, nonce, RejectProtocolMismatch, "unsupported protocol version")
		}
		return
	}

	hdr, err := UnmarshalHeader(data)
	if err != nil {
		return
	}

	switch hdr.Type {
	case PktJoinReq:
		s.handleJoinReq(remoteAddr, data[HeaderSize:])
	case PktGameData:
		s.handleGameData(remoteAddr, hdr, data)
	case PktHeartbeat:
		s.handleHeartbeat(remoteAddr, hdr, data)
	case PktLeave:
		s.handleLeave(remoteAddr, hdr, data[HeaderSize:])
	}
}

func (s *Server) handleJoinReq(addr *net.UDPAddr, payload []byte) {
	req, err := UnmarshalJoinReqPayload(payload)
	if err != nil {
		s.sendReject(addr, req.ClientNonce, RejectInvalidIdentity, "malformed join payload")
		return
	}

	if len(req.RoomName) == 0 || len(req.RoomName) > 32 {
		s.sendReject(addr, req.ClientNonce, RejectInvalidRoom, "room name must be 1 to 32 characters")
		return
	}
	s.mu.Lock()
	room, exists := s.rooms[req.RoomName]
	if !exists {
		if len(s.rooms) >= MaxRooms {
			s.mu.Unlock()
			s.sendReject(addr, req.ClientNonce, RejectRoomFull, "server full")
			return
		}
		room = NewRoom(req.RoomName)
		s.rooms[req.RoomName] = room
	}
	s.mu.Unlock()

	result := room.ProcessJoin(addr, req)
	if result.OldSessionID != 0 {
		s.mu.Lock()
		delete(s.sessions, result.OldSessionID)
		s.mu.Unlock()
	}
	if result.RejectCode != 0 {
		s.sendReject(addr, req.ClientNonce, result.RejectCode, RejectReasonString(result.RejectCode))
		return
	}

	if !result.MatchStarted {
		// Waiting for second player
		s.sendJoinWait(addr, req.ClientNonce, result.AssignedSlot, "waiting for opponent")
		return
	}

	// Match started! Register session ID
	s.mu.Lock()
	s.sessions[result.SessionID] = room
	s.mu.Unlock()
	// Send MatchStart to joining player
	s.sendMatchStart(addr, req.ClientNonce, result.AssignedSlot, result.Delay, result.SessionID, result.PeerIdentity)

	// Send MatchStart to waiting player (using result.PeerNonce safely from JoinResult)
	s.sendMatchStart(result.PeerAddr, result.PeerNonce, result.PeerSlot, result.Delay, result.SessionID, room.Identity)
}

func (s *Server) handleGameData(addr *net.UDPAddr, hdr Header, data []byte) {
	s.mu.RLock()
	room, exists := s.sessions[hdr.SessionID]
	s.mu.RUnlock()

	if !exists {
		return // Stale or invalid session
	}

	// Canonical full-pass validation before state mutation or relay
	payload, err := UnmarshalGameDataPayload(data[HeaderSize:])
	if err != nil {
		return
	}
	if payload.Flags&^(FlagFinishReq|FlagFinishAck) != 0 {
		return
	}
	for _, w := range payload.Inputs {
		if w&^InputMask != 0 {
			return
		}
	}

	// Check if this packet carries finish info to update room state
	if payload.Flags&FlagFinishReq != 0 {
		bothFinished, finalFrame, finalCRC, addr0, addr1 := room.MarkFinish(hdr.SenderSlot, addr, payload.FinishFrame, payload.FinishCRC)
		if bothFinished {
			// Persistent completion: send PktMatchComplete to both clients
			if addr0 != nil {
				s.sendMatchComplete(addr0, hdr.SessionID, finalFrame, finalCRC)
			}
			if addr1 != nil {
				s.sendMatchComplete(addr1, hdr.SessionID, finalFrame, finalCRC)
			}
		} else if isFin, fFrame, fCRC := room.GetFinishVerdict(addr); isFin {
			// Room already finished, repeat verdict to this sender
			s.sendMatchComplete(addr, hdr.SessionID, fFrame, fCRC)
		}
	} else if isFin, fFrame, fCRC := room.GetFinishVerdict(addr); isFin {
		// Room already finished, repeat verdict to sender
		s.sendMatchComplete(addr, hdr.SessionID, fFrame, fCRC)
	}

	peerAddr, err := room.GetPeer(hdr.SenderSlot, hdr.SessionID, addr)
	if err != nil {
		return // Peer not connected or sender endpoint mismatch
	}

	// Relay packet to peer via impairment engine
	s.impairer.Send(peerAddr, data)
}

func (s *Server) handleHeartbeat(addr *net.UDPAddr, hdr Header, data []byte) {
	s.mu.RLock()
	room, exists := s.sessions[hdr.SessionID]
	s.mu.RUnlock()

	if !exists {
		return
	}

	peerAddr, err := room.GetPeer(hdr.SenderSlot, hdr.SessionID, addr)
	if err != nil {
		return
	}

	s.impairer.Send(peerAddr, data)
}

func (s *Server) handleLeave(addr *net.UDPAddr, hdr Header, payload []byte) {
	s.mu.RLock()
	room, exists := s.sessions[hdr.SessionID]
	s.mu.RUnlock()

	if !exists {
		return
	}

	leavePayload, err := UnmarshalLeavePayload(payload)
	leaveCode := LeaveAbort
	if err == nil {
		leaveCode = leavePayload.LeaveCode
	}

	peerAddr, shouldTerminate := room.HandleLeave(hdr.SenderSlot, hdr.SessionID, addr, leaveCode)
	if shouldTerminate {
		s.mu.Lock()
		delete(s.sessions, hdr.SessionID)
		s.mu.Unlock()
		if peerAddr != nil {
			s.sendTerminated(peerAddr, hdr.SessionID, 0, "opponent left match")
		}
	}
}

func (s *Server) sendReject(addr *net.UDPAddr, nonce uint64, code uint8, msg string) {
	hdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktJoinReject,
		SenderSlot: 0xFF,
	}
	p := JoinRejectPayload{
		ClientNonce: nonce,
		RejectCode:  code,
		Message:     msg,
	}
	buf := append(hdr.Marshal(), p.Marshal()...)
	s.impairer.Send(addr, buf)
}

func (s *Server) sendJoinWait(addr *net.UDPAddr, nonce uint64, slot uint8, msg string) {
	hdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktJoinWait,
		SenderSlot: slot,
	}
	p := JoinWaitPayload{
		ClientNonce:  nonce,
		AssignedSlot: slot,
		Message:      msg,
	}
	buf := append(hdr.Marshal(), p.Marshal()...)
	s.impairer.Send(addr, buf)
}

func (s *Server) sendMatchStart(addr *net.UDPAddr, nonce uint64, slot uint8, delay uint8, sessionID uint64, peerId Identity) {
	hdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktMatchStart,
		SessionID:  sessionID,
		SenderSlot: slot,
	}
	p := MatchStartPayload{
		ClientNonce:  nonce,
		AssignedSlot: slot,
		Delay:        delay,
		PeerIdentity: peerId,
	}
	buf := append(hdr.Marshal(), p.Marshal()...)
	s.impairer.Send(addr, buf)
}

func (s *Server) sendTerminated(addr *net.UDPAddr, sessionID uint64, code uint8, msg string) {
	hdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktMatchTerminated,
		SessionID:  sessionID,
		SenderSlot: 0xFF,
	}
	p := TerminatePayload{
		ReasonCode: code,
		Message:    msg,
	}
	buf := append(hdr.Marshal(), p.Marshal()...)
	s.impairer.Send(addr, buf)
}

func (s *Server) sendMatchComplete(addr *net.UDPAddr, sessionID uint64, finalFrame uint32, finalCRC uint32) {
	hdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktMatchComplete,
		SessionID:  sessionID,
		SenderSlot: 0xFF,
	}
	p := MatchCompletePayload{
		FinalFrame: finalFrame,
		FinalCRC:   finalCRC,
	}
	buf := append(hdr.Marshal(), p.Marshal()...)
	s.impairer.Send(addr, buf)
}

func (s *Server) janitorLoop() {
	defer s.wg.Done()

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-s.stopCh:
			return
		case <-ticker.C:
			s.runJanitor()
		}
	}
}

func (s *Server) runJanitor() {
	now := time.Now()

	s.mu.Lock()
	type item struct {
		name string
		room *Room
		snap RoomSnapshot
	}
	items := make([]item, 0, len(s.rooms))
	for name, room := range s.rooms {
		items = append(items, item{name: name, room: room, snap: room.Snapshot()})
	}
	s.mu.Unlock()

	for _, it := range items {
		// Check client timeouts in active rooms
		timedOutSlot, notifyAddr := it.room.CheckTimeouts(ClientTimeout)
		if timedOutSlot >= 0 && notifyAddr != nil {
			s.sendTerminated(notifyAddr, it.snap.SessionID, 0, "opponent connection timed out")
		}

		// Clean up old rooms
		s.mu.Lock()
		if it.snap.State == RoomWaiting && now.Sub(it.snap.CreatedAt) > WaitingRoomTimeout {
			delete(s.rooms, it.name)
			if it.snap.SessionID != 0 {
				delete(s.sessions, it.snap.SessionID)
			}
		} else if (it.snap.State == RoomFinished || it.snap.State == RoomTerminated) && now.Sub(it.snap.LastActive) > FinishedRoomTimeout {
			delete(s.rooms, it.name)
			if it.snap.SessionID != 0 {
				delete(s.sessions, it.snap.SessionID)
			}
		}
		s.mu.Unlock()
	}

	// Clean up inactive rate limiters
	s.limiterMu.Lock()
	for ip, rl := range s.limiters {
		if now.Sub(rl.lastTime) > 60*time.Second {
			delete(s.limiters, ip)
		}
	}
	s.limiterMu.Unlock()
}

func (s *Server) Close() error {
	close(s.stopCh)
	err := s.conn.Close()
	s.impairer.Close()
	s.wg.Wait()
	return err
}
