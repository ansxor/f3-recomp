// Package main is the protocol v1 netplay relay. TCP carries rooms, match
// setup, CRC reports and results. UDP carries authenticated input packets that
// are relayed unchanged. See docs/site/developer/netplay/protocol.md.
package main

import (
	"bufio"
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/netip"
	"strings"
	"sync"
	"time"
)

// Config holds listener addresses, limits and timers. Zero timers take the
// protocol defaults; tests shorten them.
type Config struct {
	TCPAddr  string
	UDPAddr  string
	LogDir   string
	MaxRooms int

	HelloTimeout time.Duration // idle limit before HELLO
	LobbyIdle    time.Duration // idle limit for lobby connections
	RoomTTL      time.Duration // lifetime of an unjoined room
	WriteTimeout time.Duration
}

const (
	defaultMaxRooms     = 64
	defaultLogDir       = "./netplay-logs"
	defaultHelloTimeout = 30 * time.Second
	defaultLobbyIdle    = 10 * time.Minute
	defaultRoomTTL      = 10 * time.Minute
	defaultWriteTimeout = 10 * time.Second
	outQueueLen         = 64
)

func (c Config) withDefaults() Config {
	if c.MaxRooms <= 0 {
		c.MaxRooms = defaultMaxRooms
	}
	if c.LogDir == "" {
		c.LogDir = defaultLogDir
	}
	if c.HelloTimeout <= 0 {
		c.HelloTimeout = defaultHelloTimeout
	}
	if c.LobbyIdle <= 0 {
		c.LobbyIdle = defaultLobbyIdle
	}
	if c.RoomTTL <= 0 {
		c.RoomTTL = defaultRoomTTL
	}
	if c.WriteTimeout <= 0 {
		c.WriteTimeout = defaultWriteTimeout
	}
	return c
}

// identity is what both players must agree on to play.
type identity struct {
	gameID    string
	romCRCs   [numROMCRCs]uint32
	buildHash [32]byte
}

// room is a lobby entry before a joiner arrives, and the session afterwards.
type room struct {
	code         string
	id           identity
	inputDelay   uint8
	syncRequired bool
	host         *client
	sess         *session
	timer        *time.Timer
}

// client is one TCP connection. Fields after stop are guarded by Server.mu.
// conn is safe for the goroutines that use it.
type client struct {
	conn net.Conn
	out  chan []byte // encoded frames for the writer goroutine
	stop chan struct{}

	helloDone bool
	ident     identity
	name      string
	room      *room
	role      int // 0 host, 1 joiner; valid while room != nil
	detached  bool
	chat      chatBucket // CHAT rate limit
}

func (c *client) session() *session {
	if c.room == nil {
		return nil
	}
	return c.room.sess
}

// CHAT rate limit: a bucket of chatBurst tokens refilled at chatRefillPerSec.
const (
	chatBurst        = 5
	chatRefillPerSec = 1.0
)

// chatBucket is a per-client token bucket for CHAT. The zero value fills on
// the first allow call.
type chatBucket struct {
	tokens float64
	last   time.Time
}

// allow spends one token if one is available.
func (b *chatBucket) allow(now time.Time) bool {
	if b.last.IsZero() {
		b.tokens = chatBurst
	} else {
		b.tokens = min(chatBurst, b.tokens+now.Sub(b.last).Seconds()*chatRefillPerSec)
	}
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// effects collects work that must run after Server.mu is released.
type effects struct {
	logs []*logFile
	udp  []outPacket
}

type outPacket struct {
	data []byte
	to   netip.AddrPort
}

// Server accepts TCP control connections and relays UDP input packets.
type Server struct {
	cfg     Config
	tcp     net.Listener
	udp     *net.UDPConn
	udpPort uint16

	mu       sync.Mutex
	closing  bool
	rooms    map[string]*room
	sessions map[uint64]*session
	clients  map[*client]struct{}
}

// New binds the TCP and UDP listeners. Call Serve to run the server.
func New(cfg Config) (*Server, error) {
	cfg = cfg.withDefaults()
	tcp, err := net.Listen("tcp", cfg.TCPAddr)
	if err != nil {
		return nil, fmt.Errorf("tcp listen: %w", err)
	}
	ua, err := net.ResolveUDPAddr("udp", cfg.UDPAddr)
	if err != nil {
		tcp.Close()
		return nil, fmt.Errorf("udp address: %w", err)
	}
	udp, err := net.ListenUDP("udp", ua)
	if err != nil {
		tcp.Close()
		return nil, fmt.Errorf("udp listen: %w", err)
	}
	return &Server{
		cfg:      cfg,
		tcp:      tcp,
		udp:      udp,
		udpPort:  uint16(udp.LocalAddr().(*net.UDPAddr).Port),
		rooms:    make(map[string]*room),
		sessions: make(map[uint64]*session),
		clients:  make(map[*client]struct{}),
	}, nil
}

// TCPAddr is the bound control address.
func (s *Server) TCPAddr() net.Addr { return s.tcp.Addr() }

// UDPAddr is the bound input address.
func (s *Server) UDPAddr() *net.UDPAddr { return s.udp.LocalAddr().(*net.UDPAddr) }

// Serve runs until ctx is cancelled or the TCP listener fails. On return every
// connection is closed and every open session has been logged.
func (s *Server) Serve(ctx context.Context) error {
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			s.shutdown()
		case <-done:
		}
	}()

	udpDone := make(chan struct{})
	go func() {
		defer close(udpDone)
		s.udpLoop()
	}()

	var wg sync.WaitGroup
	var acceptErr error
	for {
		conn, err := s.tcp.Accept()
		if err != nil {
			if !errors.Is(err, net.ErrClosed) {
				acceptErr = err
			}
			break
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			s.serveConn(conn)
		}()
	}
	s.shutdown()
	wg.Wait()
	<-udpDone
	return acceptErr
}

func (s *Server) shutdown() {
	s.mu.Lock()
	if s.closing {
		s.mu.Unlock()
		return
	}
	s.closing = true
	var e effects
	list := make([]*client, 0, len(s.clients))
	for c := range s.clients {
		list = append(list, c)
	}
	for _, c := range list {
		s.leaveLocked(c, "disconnect", &e)
	}
	s.mu.Unlock()
	s.flush(&e)
	s.tcp.Close()
	s.udp.Close()
}

func (s *Server) serveConn(conn net.Conn) {
	c := &client{conn: conn, out: make(chan []byte, outQueueLen), stop: make(chan struct{})}

	s.mu.Lock()
	if s.closing {
		s.mu.Unlock()
		conn.Close()
		return
	}
	s.clients[c] = struct{}{}
	s.applyDeadlineLocked(c)
	s.mu.Unlock()

	go s.writeLoop(c)

	br := bufio.NewReader(conn)
	reason := "disconnect"
	for {
		typ, payload, err := readFrame(br)
		if err != nil {
			if errors.Is(err, errFrameSize) {
				reason = "protocol_error"
			}
			break
		}
		if !s.handle(c, typ, payload) {
			break
		}
	}

	var e effects
	s.mu.Lock()
	s.leaveLocked(c, reason, &e)
	s.mu.Unlock()
	s.flush(&e)
}

// writeLoop sends queued frames. After stop closes it drains what is queued,
// then closes the connection. Only this goroutine writes to conn.
func (s *Server) writeLoop(c *client) {
	defer c.conn.Close()
	write := func(b []byte) bool {
		c.conn.SetWriteDeadline(time.Now().Add(s.cfg.WriteTimeout))
		_, err := c.conn.Write(b)
		return err == nil
	}
	for {
		select {
		case b := <-c.out:
			if !write(b) {
				return
			}
		case <-c.stop:
			for {
				select {
				case b := <-c.out:
					if !write(b) {
						return
					}
				default:
					return
				}
			}
		}
	}
}

// handle processes one TCP message and returns false when the connection
// must stop reading.
func (s *Server) handle(c *client, typ byte, p []byte) bool {
	var e effects
	s.mu.Lock()
	keep := s.dispatchLocked(c, typ, p, &e)
	if keep {
		s.applyDeadlineLocked(c)
	}
	s.mu.Unlock()
	s.flush(&e)
	return keep
}

func (s *Server) dispatchLocked(c *client, typ byte, p []byte, e *effects) bool {
	if c.detached {
		return false
	}
	if typ == msgHello {
		return s.onHelloLocked(c, p, e)
	}
	if !c.helloDone {
		return s.failLocked(c, errBadMessage, "HELLO must be first", e)
	}
	switch typ {
	case msgCreateRoom:
		return s.onCreateLocked(c, p, e)
	case msgJoinRoom:
		return s.onJoinLocked(c, p, e)
	case msgCRC:
		return s.onCRCLocked(c, p, e)
	case msgResult:
		return s.onResultLocked(c, p, e)
	case msgSyncBlob:
		return s.onSyncBlobLocked(c, p, e)
	case msgChat:
		return s.onChatLocked(c, p, e)
	case msgBye:
		if len(p) != 0 {
			return s.failLocked(c, errBadMessage, "BYE has a payload", e)
		}
		s.leaveLocked(c, "bye", e)
		return false
	}
	return s.failLocked(c, errBadMessage, "unknown message type", e)
}

func (s *Server) onHelloLocked(c *client, p []byte, e *effects) bool {
	r := &reader{b: p}
	ver := r.u16()
	if r.err == nil && ver != protoVersion {
		return s.failLocked(c, errBadVersion, "unsupported protocol version", e)
	}
	gid := r.str()
	var crcs [numROMCRCs]uint32
	for i := range crcs {
		crcs[i] = r.u32()
	}
	bh := r.bytes(32)
	name := r.str()
	if err := r.finish(); err != nil {
		return s.failLocked(c, errBadMessage, "malformed HELLO", e)
	}
	if len(name) < 1 || len(name) > maxNameLen {
		return s.failLocked(c, errBadMessage, "player name must be 1 to 24 bytes", e)
	}
	if c.helloDone {
		return s.failLocked(c, errBadMessage, "duplicate HELLO", e)
	}
	c.helloDone = true
	c.ident = identity{gameID: gid, romCRCs: crcs}
	copy(c.ident.buildHash[:], bh)
	c.name = name
	s.enqueueLocked(c, encode(msgWelcome, binary.BigEndian.AppendUint16(nil, protoVersion)))
	return true
}

func (s *Server) onCreateLocked(c *client, p []byte, e *effects) bool {
	if len(p) != 2 {
		return s.failLocked(c, errBadMessage, "CREATE_ROOM payload must be 2 bytes", e)
	}
	delay, flags := p[0], p[1]
	if flags&^flagsKnownMask != 0 {
		return s.failLocked(c, errBadMessage, "unknown CREATE_ROOM flags", e)
	}
	if delay < minInputDelay || delay > maxInputDelay {
		return s.failLocked(c, errBadMessage, "input_delay out of range", e)
	}
	if c.room != nil {
		return s.failLocked(c, errNotInLobby, "already in a room", e)
	}
	if len(s.rooms) >= s.cfg.MaxRooms {
		return s.failLocked(c, errServerFull, "server full", e)
	}
	code := s.newCodeLocked()
	r := &room{code: code, id: c.ident, inputDelay: delay, syncRequired: flags&flagSyncBlob != 0, host: c}
	s.rooms[code] = r
	c.room = r
	c.role = 0
	r.timer = time.AfterFunc(s.cfg.RoomTTL, func() { s.expireRoom(r) })
	s.enqueueLocked(c, encode(msgRoomCreated, []byte(code)))
	return true
}

func (s *Server) onJoinLocked(c *client, p []byte, e *effects) bool {
	if len(p) != roomCodeLen {
		return s.failLocked(c, errBadMessage, "room code must be 6 bytes", e)
	}
	for _, b := range p {
		if strings.IndexByte(roomAlphabet, b) < 0 {
			return s.failLocked(c, errBadMessage, "room code has invalid characters", e)
		}
	}
	if c.room != nil {
		return s.failLocked(c, errNotInLobby, "already in a room", e)
	}
	r := s.rooms[string(p)]
	if r == nil {
		return s.failLocked(c, errRoomNotFound, "room not found", e)
	}
	if r.sess != nil {
		return s.failLocked(c, errRoomFull, "room full", e)
	}
	if r.id != c.ident {
		return s.failLocked(c, errIdentityMismatch, "game, ROM or build differs from host", e)
	}

	host := r.host
	sess := newSession(s.newSessionIDLocked(), r, host, c)
	s.sessions[sess.id] = sess
	r.sess = sess
	r.timer.Stop()
	c.room = r
	c.role = 1

	s.enqueueLocked(host, encode(msgMatchStart, matchStartPayload(sess, 0, c.name, s.udpPort)))
	s.enqueueLocked(c, encode(msgMatchStart, matchStartPayload(sess, 1, host.name, s.udpPort)))
	s.applyDeadlineLocked(host)
	s.applyDeadlineLocked(c)
	return true
}

// onSyncBlobLocked relays the host's sync blob to the joiner. It is accepted
// once, from the host, before GO, and only when the room requires one. The
// joiner receives the payload byte-for-byte.
func (s *Server) onSyncBlobLocked(c *client, p []byte, e *effects) bool {
	sess := c.session()
	if sess == nil {
		return s.failLocked(c, errBadMessage, "SYNC_BLOB outside a match", e)
	}
	if c.role != 0 {
		return s.failLocked(c, errBadMessage, "only the host may send SYNC_BLOB", e)
	}
	if !sess.syncRequired {
		return s.failLocked(c, errBadMessage, "room does not require a sync blob", e)
	}
	if sess.goSent {
		return s.failLocked(c, errBadMessage, "SYNC_BLOB after GO", e)
	}
	if sess.syncSeen {
		return s.failLocked(c, errBadMessage, "duplicate SYNC_BLOB", e)
	}
	r := &reader{b: p}
	n := int(r.u16())
	if r.err != nil || n < minSyncBlob || n > maxSyncBlob {
		return s.failLocked(c, errBadMessage, "SYNC_BLOB length must be 1 to 2048", e)
	}
	blob := r.bytes(n)
	if err := r.finish(); err != nil {
		return s.failLocked(c, errBadMessage, "malformed SYNC_BLOB", e)
	}
	sess.syncSeen = true
	sess.syncBlob = append([]byte(nil), blob...)
	s.enqueueLocked(sess.clients[1], encode(msgSyncBlobFwd, p))
	s.maybeGoLocked(sess)
	return true
}

// maybeGoLocked sends GO to both players once both UDP endpoints are
// registered and, if the room requires one, the sync blob has been relayed.
func (s *Server) maybeGoLocked(sess *session) {
	if sess.goSent || !sess.registered[0] || !sess.registered[1] {
		return
	}
	if sess.syncRequired && !sess.syncSeen {
		return
	}
	sess.goSent = true
	msg := encode(msgGo, nil)
	for _, c := range sess.clients {
		s.enqueueLocked(c, msg)
	}
}

// onChatLocked relays one CHAT line to the other player. CHAT is accepted
// only inside a match; a client without one gets ERROR 6. Malformed text ends
// the session with ERROR 2. Lines over the rate limit are dropped silently and
// counted in the session log.
func (s *Server) onChatLocked(c *client, p []byte, e *effects) bool {
	sess := c.session()
	if sess == nil {
		return s.failLocked(c, errNotInLobby, "CHAT before MATCH_START", e)
	}
	text, ok := parseChat(p)
	if !ok {
		return s.failLocked(c, errBadMessage, "malformed CHAT", e)
	}
	now := time.Now()
	if !c.chat.allow(now) {
		sess.chatDropped[c.role]++
		return true
	}
	sess.chat = append(sess.chat, chatRec{
		Player: uint8(c.role),
		TMs:    now.Sub(sess.started).Milliseconds(),
		Text:   text,
	})
	s.enqueueLocked(sess.clients[1-c.role], encode(msgChatFwd, chatFwdPayload(uint8(c.role), text)))
	return true
}

func (s *Server) onCRCLocked(c *client, p []byte, e *effects) bool {
	sess := c.session()
	if len(p) != 8 || sess == nil {
		return s.failLocked(c, errBadMessage, "CRC outside a match or bad length", e)
	}
	frame := binary.BigEndian.Uint32(p[0:4])
	crc := binary.BigEndian.Uint32(p[4:8])
	if frame >= maxSessionFrames {
		return true
	}
	if d := sess.recordCRC(c.role, frame, crc); d != nil {
		payload := binary.BigEndian.AppendUint32(nil, d.frame)
		payload = binary.BigEndian.AppendUint32(payload, d.crc1)
		payload = binary.BigEndian.AppendUint32(payload, d.crc2)
		msg := encode(msgDesync, payload)
		for _, pc := range sess.clients {
			s.enqueueLocked(pc, msg)
		}
	}
	return true
}

func (s *Server) onResultLocked(c *client, p []byte, e *effects) bool {
	sess := c.session()
	if len(p) != 9 || sess == nil {
		return s.failLocked(c, errBadMessage, "RESULT outside a match or bad length", e)
	}
	winner := p[4]
	if winner != winnerP1 && winner != winnerP2 && winner != winnerDraw {
		return s.failLocked(c, errBadMessage, "invalid winner", e)
	}
	if sess.results[c.role] != nil {
		return true // only the first RESULT per player counts
	}
	sess.results[c.role] = &resultRec{
		Frame:    binary.BigEndian.Uint32(p[0:4]),
		Winner:   winner,
		FinalCRC: binary.BigEndian.Uint32(p[5:9]),
	}
	if sess.results[0] == nil || sess.results[1] == nil || sess.ack != nil {
		return true
	}
	r0, r1 := sess.results[0], sess.results[1]
	agreed := r0.Frame == r1.Frame && r0.Winner == r1.Winner && r0.FinalCRC == r1.FinalCRC
	ackWinner := uint8(winnerInvalid)
	if agreed {
		ackWinner = r0.Winner
	}
	sess.ack = &ackRec{Agreed: agreed, Winner: ackWinner}
	msg := encode(msgResultAck, []byte{boolByte(agreed), ackWinner})
	for _, pc := range sess.clients {
		s.enqueueLocked(pc, msg)
	}
	return true
}

func boolByte(b bool) byte {
	if b {
		return 1
	}
	return 0
}

// failLocked sends ERROR, then closes the connection. The writer drains the
// queued ERROR before it closes.
func (s *Server) failLocked(c *client, code uint16, msg string, e *effects) bool {
	payload := appendStr(binary.BigEndian.AppendUint16(nil, code), msg)
	s.enqueueLocked(c, encode(msgError, payload))
	s.leaveLocked(c, "protocol_error", e)
	return false
}

// enqueueLocked queues a frame for c. A full queue closes the connection.
func (s *Server) enqueueLocked(c *client, frame []byte) {
	if c.detached {
		return
	}
	select {
	case c.out <- frame:
	default:
		c.conn.Close()
	}
}

// leaveLocked detaches c. A lobby host takes its room with it. A session ends
// for both players: the peer gets PEER_LEFT and is detached too. Logs are
// appended to e so they are written after the lock is released.
func (s *Server) leaveLocked(c *client, reason string, e *effects) {
	if c.detached {
		return
	}
	c.detached = true
	delete(s.clients, c)
	close(c.stop)

	r := c.room
	if r == nil {
		return
	}
	if r.sess == nil {
		s.removeRoomLocked(r)
		return
	}
	sess := r.sess
	if sess.ended {
		return
	}
	s.endSessionLocked(sess, reason, c.role, e)
	if peer := sess.clients[1-c.role]; peer != nil {
		s.enqueueLocked(peer, encode(msgPeerLeft, nil))
		s.leaveLocked(peer, reason, e)
	}
}

func (s *Server) endSessionLocked(sess *session, reason string, endedBy int, e *effects) {
	sess.end(reason, endedBy)
	delete(s.sessions, sess.id)
	s.removeRoomLocked(sess.room)
	e.logs = append(e.logs, sess.snapshot())
}

func (s *Server) removeRoomLocked(r *room) {
	if s.rooms[r.code] == r {
		delete(s.rooms, r.code)
	}
	if r.timer != nil {
		r.timer.Stop()
	}
}

// expireRoom closes an unjoined room after RoomTTL. The host connection is
// closed without an ERROR.
func (s *Server) expireRoom(r *room) {
	var e effects
	s.mu.Lock()
	if s.rooms[r.code] == r && r.sess == nil && !r.host.detached {
		s.leaveLocked(r.host, "expired", &e)
	}
	s.mu.Unlock()
	s.flush(&e)
}

// applyDeadlineLocked sets the read deadline for c's next frame.
func (s *Server) applyDeadlineLocked(c *client) {
	var d time.Time
	switch {
	case !c.helloDone:
		d = time.Now().Add(s.cfg.HelloTimeout)
	case c.room != nil && c.room.sess != nil:
		// Matches have no idle timeout.
	default:
		d = time.Now().Add(s.cfg.LobbyIdle)
	}
	c.conn.SetReadDeadline(d)
}

func (s *Server) newCodeLocked() string {
	var b [roomCodeLen]byte
	out := make([]byte, roomCodeLen)
	for {
		mustRand(b[:])
		for i := range b {
			out[i] = roomAlphabet[b[i]&31]
		}
		code := string(out)
		if s.rooms[code] == nil {
			return code
		}
	}
}

func (s *Server) newSessionIDLocked() uint64 {
	var b [8]byte
	for {
		mustRand(b[:])
		id := binary.BigEndian.Uint64(b[:])
		if s.sessions[id] == nil {
			return id
		}
	}
}

func matchStartPayload(sess *session, index uint8, peer string, udpPort uint16) []byte {
	p := binary.BigEndian.AppendUint64(nil, sess.id)
	p = append(p, sess.key[:]...)
	p = binary.BigEndian.AppendUint64(p, sess.seed)
	p = append(p, index, sess.inputDelay)
	p = binary.BigEndian.AppendUint16(p, udpPort)
	return appendStr(p, peer)
}

// flush performs the work collected under the lock. It must run without it.
func (s *Server) flush(e *effects) {
	for _, p := range e.udp {
		s.udp.WriteToUDPAddrPort(p.data, p.to)
	}
	for _, l := range e.logs {
		if err := writeLog(s.cfg.LogDir, l); err != nil {
			log.Printf("netplay: write session log: %v", err)
		}
	}
}

// mustRand fills b from crypto/rand. Failure means the OS entropy source is
// unusable, which cannot be recovered from.
func mustRand(b []byte) {
	if _, err := io.ReadFull(rand.Reader, b); err != nil {
		panic(err)
	}
}
