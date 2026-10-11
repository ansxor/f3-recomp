package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"
)

// startServer runs a server on ephemeral localhost ports with a temp log dir.
// mut may adjust the config before New.
func startServer(t *testing.T, mut func(*Config)) (*Server, string) {
	t.Helper()
	dir := t.TempDir()
	cfg := Config{
		TCPAddr:      "127.0.0.1:0",
		UDPAddr:      "127.0.0.1:0",
		LogDir:       dir,
		MaxRooms:     64,
		HelloTimeout: 5 * time.Second,
		LobbyIdle:    time.Minute,
		RoomTTL:      time.Minute,
		WriteTimeout: 5 * time.Second,
	}
	if mut != nil {
		mut(&cfg)
	}
	srv, err := New(cfg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- srv.Serve(ctx) }()
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			t.Error("Serve did not return after cancel")
		}
	})
	return srv, dir
}

func testIdent() identity {
	id := identity{gameID: "f3-test", romCRCs: [numROMCRCs]uint32{1, 2, 3, 4, 5, 6, 7}}
	for i := range id.buildHash {
		id.buildHash[i] = 0xAB
	}
	return id
}

func helloPayload(ver uint16, name string, ident identity) []byte {
	b := binary.BigEndian.AppendUint16(nil, ver)
	b = appendStr(b, ident.gameID)
	for _, c := range ident.romCRCs {
		b = binary.BigEndian.AppendUint32(b, c)
	}
	b = append(b, ident.buildHash[:]...)
	return appendStr(b, name)
}

// tcpClient is a test-side control connection.
type tcpClient struct {
	t    *testing.T
	conn net.Conn
	br   *bufio.Reader
}

func dialTCP(t *testing.T, srv *Server) *tcpClient {
	t.Helper()
	conn, err := net.DialTimeout("tcp", srv.TCPAddr().String(), 5*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return &tcpClient{t: t, conn: conn, br: bufio.NewReader(conn)}
}

func (c *tcpClient) send(typ byte, payload []byte) {
	c.t.Helper()
	c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if _, err := c.conn.Write(encode(typ, payload)); err != nil {
		c.t.Fatalf("write: %v", err)
	}
}

func (c *tcpClient) recv() (byte, []byte) {
	c.t.Helper()
	c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	typ, p, err := readFrame(c.br)
	if err != nil {
		c.t.Fatalf("read frame: %v", err)
	}
	return typ, p
}

func (c *tcpClient) expect(typ byte) []byte {
	c.t.Helper()
	got, p := c.recv()
	if got != typ {
		c.t.Fatalf("message %#x, want %#x (payload %v)", got, typ, p)
	}
	return p
}

func (c *tcpClient) expectErr(code uint16) {
	c.t.Helper()
	p := c.expect(msgError)
	r := &reader{b: p}
	got := r.u16()
	if r.err != nil || got != code {
		c.t.Fatalf("ERROR code %d (err %v), want %d", got, r.err, code)
	}
}

func (c *tcpClient) expectClosed() {
	c.t.Helper()
	c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, _, err := readFrame(c.br); err == nil {
		c.t.Fatal("connection still open, want closed")
	}
}

// expectSilent fails if any frame arrives within d.
func (c *tcpClient) expectSilent(d time.Duration) {
	c.t.Helper()
	c.conn.SetReadDeadline(time.Now().Add(d))
	if typ, _, err := readFrame(c.br); err == nil {
		c.t.Fatalf("message %#x before the deadline, want none", typ)
	}
}

func (c *tcpClient) hello(name string, ident identity) {
	c.t.Helper()
	c.send(msgHello, helloPayload(protoVersion, name, ident))
	p := c.expect(msgWelcome)
	if len(p) != 2 || binary.BigEndian.Uint16(p) != protoVersion {
		c.t.Fatalf("bad WELCOME payload %v", p)
	}
}

func (c *tcpClient) create(delay uint8) string {
	c.t.Helper()
	return c.createFlags(delay, 0)
}

func (c *tcpClient) createFlags(delay, flags uint8) string {
	c.t.Helper()
	c.send(msgCreateRoom, []byte{delay, flags})
	return string(c.expect(msgRoomCreated))
}

func (c *tcpClient) join(code string) {
	c.t.Helper()
	c.send(msgJoinRoom, []byte(code))
}

func readU64(r *reader) uint64 {
	return uint64(r.u32())<<32 | uint64(r.u32())
}

type matchInfo struct {
	sid     uint64
	key     [32]byte
	seed    uint64
	index   uint8
	delay   uint8
	udpPort uint16
	peer    string
}

func parseMatchStart(t *testing.T, p []byte) matchInfo {
	t.Helper()
	r := &reader{b: p}
	var m matchInfo
	m.sid = readU64(r)
	copy(m.key[:], r.bytes(32))
	m.seed = readU64(r)
	m.index = r.u8()
	m.delay = r.u8()
	m.udpPort = r.u16()
	m.peer = r.str()
	if err := r.finish(); err != nil {
		t.Fatalf("MATCH_START payload: %v", err)
	}
	return m
}

// match is a host and joiner connected through the lobby.
type match struct {
	srv          *Server
	host, joiner *tcpClient
	hm, jm       matchInfo
	hu, ju       *udpPeer
	code         string
}

// openMatch runs HELLO, CREATE_ROOM and JOIN_ROOM, and reads both MATCH_START
// messages. The host creates with input delay 2 and names are alice and bob.
func openMatch(t *testing.T, srv *Server) *match {
	t.Helper()
	return openMatchFlags(t, srv, 0)
}

// openMatchFlags is openMatch with CREATE_ROOM flags.
func openMatchFlags(t *testing.T, srv *Server, flags uint8) *match {
	t.Helper()
	m := &match{srv: srv}
	m.host = dialTCP(t, srv)
	m.host.hello("alice", testIdent())
	m.code = m.host.createFlags(2, flags)
	m.joiner = dialTCP(t, srv)
	m.joiner.hello("bob", testIdent())
	m.joiner.join(m.code)
	m.hm = parseMatchStart(t, m.host.expect(msgMatchStart))
	m.jm = parseMatchStart(t, m.joiner.expect(msgMatchStart))
	return m
}

// registerPeers registers both players' UDP endpoints and expects the
// REGISTER replies. It does not expect GO.
func (m *match) registerPeers(t *testing.T) {
	t.Helper()
	m.hu = newUDPPeer(t, m.hm, 0)
	m.ju = newUDPPeer(t, m.jm, 1)
	m.hu.send(kindRegister, nil)
	m.hu.expectReply(kindRegister)
	m.ju.send(kindRegister, nil)
	m.ju.expectReply(kindRegister)
}

// registerUDP registers both players' UDP endpoints and expects GO on both
// control connections.
func (m *match) registerUDP(t *testing.T) {
	t.Helper()
	m.registerPeers(t)
	m.host.expect(msgGo)
	m.joiner.expect(msgGo)
}

// buildPacket is the test-side encoder. It is written independently of the
// server so that it checks the wire format.
func buildPacket(key [32]byte, sid uint64, player, kind byte, body []byte) []byte {
	b := []byte("N3P1")
	b = binary.BigEndian.AppendUint64(b, sid)
	b = append(b, player, kind)
	b = append(b, body...)
	m := hmac.New(sha256.New, key[:])
	m.Write(b)
	return append(b, m.Sum(nil)[:16]...)
}

func verifyPacket(key [32]byte, pkt []byte) bool {
	if len(pkt) < udpHeaderLen+udpTagLen {
		return false
	}
	m := hmac.New(sha256.New, key[:])
	m.Write(pkt[:len(pkt)-16])
	return hmac.Equal(m.Sum(nil)[:16], pkt[len(pkt)-16:])
}

func inputsBody(first uint32, vals ...uint16) []byte {
	b := binary.BigEndian.AppendUint32(nil, first)
	b = append(b, byte(len(vals)))
	for _, v := range vals {
		b = binary.BigEndian.AppendUint16(b, v)
	}
	return b
}

type udpPeer struct {
	t      *testing.T
	conn   *net.UDPConn
	dst    *net.UDPAddr
	sid    uint64
	key    [32]byte
	player byte
}

func newUDPPeer(t *testing.T, mi matchInfo, player byte) *udpPeer {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("listen udp: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	dst := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: int(mi.udpPort)}
	return &udpPeer{t: t, conn: conn, dst: dst, sid: mi.sid, key: mi.key, player: player}
}

func (p *udpPeer) send(kind byte, body []byte) {
	p.t.Helper()
	p.sendRaw(buildPacket(p.key, p.sid, p.player, kind, body))
}

func (p *udpPeer) sendRaw(pkt []byte) {
	p.t.Helper()
	if _, err := p.conn.WriteToUDP(pkt, p.dst); err != nil {
		p.t.Fatalf("udp write: %v", err)
	}
}

// recv returns the next datagram, or false if none arrives within d.
func (p *udpPeer) recv(d time.Duration) ([]byte, bool) {
	buf := make([]byte, maxUDPPacket+1)
	p.conn.SetReadDeadline(time.Now().Add(d))
	n, err := p.conn.Read(buf)
	if err != nil {
		return nil, false
	}
	return buf[:n], true
}

func (p *udpPeer) expectReply(kind byte) {
	p.t.Helper()
	pkt, ok := p.recv(5 * time.Second)
	if !ok {
		p.t.Fatal("no REGISTER reply")
	}
	if !verifyPacket(p.key, pkt) || string(pkt[:4]) != "N3P1" ||
		binary.BigEndian.Uint64(pkt[4:12]) != p.sid || pkt[12] != p.player || pkt[13] != kind {
		p.t.Fatalf("bad REGISTER reply %x", pkt)
	}
}

func crcPayload(frame, crc uint32) []byte {
	return binary.BigEndian.AppendUint32(binary.BigEndian.AppendUint32(nil, frame), crc)
}

func resultPayload(frame uint32, winner uint8, crc uint32) []byte {
	b := binary.BigEndian.AppendUint32(nil, frame)
	b = append(b, winner)
	return binary.BigEndian.AppendUint32(b, crc)
}

type logView struct {
	EndReason      string    `json:"end_reason"`
	EndedBy        int       `json:"ended_by"`
	Inputs         [2][]*int `json:"inputs"`
	ProtocolErrors []string  `json:"protocol_errors"`
	CRC            struct {
		Compared      int `json:"compared"`
		MismatchCount int `json:"mismatch_count"`
	} `json:"crc"`
	ResultAck *struct {
		Agreed bool  `json:"agreed"`
		Winner uint8 `json:"winner"`
	} `json:"result_ack"`
	SyncBlob    *string    `json:"sync_blob"`
	Chat        []chatView `json:"chat"`
	ChatDropped [2]int     `json:"chat_dropped"`
}

// chatView is one entry of the log's chat array.
type chatView struct {
	Player uint8  `json:"player"`
	TMs    int64  `json:"t_ms"`
	Text   string `json:"text"`
}

// waitLog polls dir for the single session log written after session end.
func waitLog(t *testing.T, dir string) logView {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		paths, _ := filepath.Glob(filepath.Join(dir, "*.json"))
		if len(paths) > 0 {
			b, err := os.ReadFile(paths[0])
			if err != nil {
				t.Fatal(err)
			}
			var v logView
			if err := json.Unmarshal(b, &v); err != nil {
				t.Fatalf("log JSON: %v", err)
			}
			return v
		}
		if time.Now().After(deadline) {
			t.Fatalf("no log file in %s", dir)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func logValues(s []*int) []int {
	out := []int{}
	for _, p := range s {
		if p == nil {
			out = append(out, -1)
		} else {
			out = append(out, *p)
		}
	}
	return out
}

func TestMatchStartFields(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)

	if m.hm.index != 0 || m.jm.index != 1 {
		t.Fatalf("player indices host=%d joiner=%d, want 0 and 1", m.hm.index, m.jm.index)
	}
	if m.hm.sid != m.jm.sid || m.hm.key != m.jm.key || m.hm.seed != m.jm.seed {
		t.Fatal("session id, key or seed differ between players")
	}
	if m.hm.sid == 0 || m.hm.key == [32]byte{} {
		t.Fatal("session id or key is zero")
	}
	if m.hm.delay != 2 || m.jm.delay != 2 {
		t.Fatalf("input_delay host=%d joiner=%d, want 2", m.hm.delay, m.jm.delay)
	}
	if int(m.hm.udpPort) != srv.UDPAddr().Port || int(m.jm.udpPort) != srv.UDPAddr().Port {
		t.Fatalf("udp_port %d, want %d", m.hm.udpPort, srv.UDPAddr().Port)
	}
	if m.hm.peer != "bob" || m.jm.peer != "alice" {
		t.Fatalf("peer names host=%q joiner=%q", m.hm.peer, m.jm.peer)
	}
}

func TestIdentityMismatch(t *testing.T) {
	cases := map[string]func(*identity){
		"game_id":    func(id *identity) { id.gameID = "other" },
		"rom_crc":    func(id *identity) { id.romCRCs[3]++ },
		"build_hash": func(id *identity) { id.buildHash[31] ^= 1 },
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			srv, _ := startServer(t, nil)
			host := dialTCP(t, srv)
			host.hello("alice", testIdent())
			code := host.create(1)

			bad := testIdent()
			mutate(&bad)
			joiner := dialTCP(t, srv)
			joiner.hello("bob", bad)
			joiner.join(code)
			joiner.expectErr(errIdentityMismatch)
			joiner.expectClosed()
		})
	}
}

func TestRegisterThenGo(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)
	m.registerUDP(t)
}

func TestInputsForwardedAndLogged(t *testing.T) {
	srv, dir := startServer(t, nil)
	m := openMatch(t, srv)
	m.registerUDP(t)

	valid := buildPacket(m.hm.key, m.hm.sid, 0, kindInputs, inputsBody(0, 5, 6))
	m.hu.sendRaw(valid)
	got, ok := m.ju.recv(5 * time.Second)
	if !ok || !bytes.Equal(got, valid) {
		t.Fatalf("joiner got %x (ok=%v), want identical %x", got, ok, valid)
	}

	// Bad tag: dropped silently, not forwarded.
	tampered := bytes.Clone(buildPacket(m.hm.key, m.hm.sid, 0, kindInputs, inputsBody(2, 8)))
	tampered[len(tampered)-1] ^= 0xFF
	m.hu.sendRaw(tampered)

	// Conflicting re-send of frame 0: logged and dropped, not forwarded.
	m.hu.send(kindInputs, inputsBody(0, 9))

	// Next valid packet is the first thing the joiner sees after the drops.
	third := buildPacket(m.hm.key, m.hm.sid, 0, kindInputs, inputsBody(2, 7))
	m.hu.sendRaw(third)
	got, ok = m.ju.recv(5 * time.Second)
	if !ok || !bytes.Equal(got, third) {
		t.Fatalf("joiner got %x (ok=%v) after drops, want %x", got, ok, third)
	}

	m.host.send(msgBye, nil)
	m.joiner.expect(msgPeerLeft)
	m.host.expectClosed()

	v := waitLog(t, dir)
	if v.EndReason != "bye" || v.EndedBy != 0 {
		t.Fatalf("end_reason %q ended_by %d, want bye and 0", v.EndReason, v.EndedBy)
	}
	if !slices.Equal(logValues(v.Inputs[0]), []int{5, 6, 7}) {
		t.Fatalf("logged inputs[0] = %v, want [5 6 7]", logValues(v.Inputs[0]))
	}
	if len(v.Inputs[1]) != 0 {
		t.Fatalf("logged inputs[1] = %v, want empty", logValues(v.Inputs[1]))
	}
	if len(v.ProtocolErrors) != 1 {
		t.Fatalf("protocol_errors = %v, want exactly the conflict", v.ProtocolErrors)
	}
}

func TestCRCDesyncOnce(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)

	m.host.send(msgCRC, crcPayload(10, 1))
	m.joiner.send(msgCRC, crcPayload(10, 2))
	for _, c := range []*tcpClient{m.host, m.joiner} {
		r := &reader{b: c.expect(msgDesync)}
		frame, c1, c2 := r.u32(), r.u32(), r.u32()
		if r.finish() != nil || frame != 10 || c1 != 1 || c2 != 2 {
			t.Fatalf("DESYNC frame=%d crc_p1=%d crc_p2=%d, want 10 1 2", frame, c1, c2)
		}
	}

	// A second mismatch must not send another DESYNC. The RESULT exchange
	// below would read a DESYNC first if one were sent.
	m.host.send(msgCRC, crcPayload(11, 3))
	m.joiner.send(msgCRC, crcPayload(11, 4))
	m.host.send(msgResult, resultPayload(100, 0, 0xAB))
	m.joiner.send(msgResult, resultPayload(100, 0, 0xAB))
	m.host.expect(msgResultAck)
	m.joiner.expect(msgResultAck)
}

func TestResultAgree(t *testing.T) {
	srv, dir := startServer(t, nil)
	m := openMatch(t, srv)
	m.host.send(msgResult, resultPayload(100, 0, 0xABCD))
	m.joiner.send(msgResult, resultPayload(100, 0, 0xABCD))
	for _, c := range []*tcpClient{m.host, m.joiner} {
		p := c.expect(msgResultAck)
		if len(p) != 2 || p[0] != 1 || p[1] != 0 {
			t.Fatalf("RESULT_ACK %v, want agreed=1 winner=0", p)
		}
	}
	m.host.send(msgBye, nil)
	m.joiner.expect(msgPeerLeft)
	if v := waitLog(t, dir); v.ResultAck == nil {
		t.Fatal("log has no result_ack")
	}
}

func TestResultDisagree(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)
	m.host.send(msgResult, resultPayload(100, 0, 0xABCD))
	m.joiner.send(msgResult, resultPayload(100, 1, 0xABCD))
	for _, c := range []*tcpClient{m.host, m.joiner} {
		p := c.expect(msgResultAck)
		if len(p) != 2 || p[0] != 0 || p[1] != 0xFE {
			t.Fatalf("RESULT_ACK %v, want agreed=0 winner=0xFE", p)
		}
	}
}

func TestDisconnectPeerLeftAndLog(t *testing.T) {
	srv, dir := startServer(t, nil)
	m := openMatch(t, srv)
	m.host.conn.Close()
	m.joiner.expect(msgPeerLeft)

	v := waitLog(t, dir)
	if v.EndReason != "disconnect" || v.EndedBy != 0 {
		t.Fatalf("end_reason %q ended_by %d, want disconnect and 0", v.EndReason, v.EndedBy)
	}
}

func TestFirstMessageMustBeHello(t *testing.T) {
	srv, _ := startServer(t, nil)
	c := dialTCP(t, srv)
	c.send(msgCreateRoom, []byte{2})
	c.expectErr(errBadMessage)
	c.expectClosed()
}

func TestWrongProtocolVersion(t *testing.T) {
	srv, _ := startServer(t, nil)
	c := dialTCP(t, srv)
	c.send(msgHello, helloPayload(2, "alice", testIdent()))
	c.expectErr(errBadVersion)
	c.expectClosed()
}

func TestOversizeFrameClosedWithoutError(t *testing.T) {
	srv, _ := startServer(t, nil)
	c := dialTCP(t, srv)
	// Length 4097 counts type and payload, so it exceeds the 4096 limit.
	c.conn.Write([]byte{0x00, 0x00, 0x10, 0x01})
	c.expectClosed()
}

func TestRoomNotFound(t *testing.T) {
	srv, _ := startServer(t, nil)
	c := dialTCP(t, srv)
	c.hello("bob", testIdent())
	c.join("ZZZZZZ")
	c.expectErr(errRoomNotFound)
}

func TestRoomFull(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)
	third := dialTCP(t, srv)
	third.hello("carol", testIdent())
	third.join(m.code)
	third.expectErr(errRoomFull)
}

func TestServerFullAtMaxRooms(t *testing.T) {
	srv, _ := startServer(t, func(c *Config) { c.MaxRooms = 1 })
	first := dialTCP(t, srv)
	first.hello("alice", testIdent())
	first.create(1)

	second := dialTCP(t, srv)
	second.hello("bob", testIdent())
	second.send(msgCreateRoom, []byte{1, 0})
	second.expectErr(7)
}

func TestRoomExpiresAfterTTL(t *testing.T) {
	srv, _ := startServer(t, func(c *Config) { c.RoomTTL = 150 * time.Millisecond })
	host := dialTCP(t, srv)
	host.hello("alice", testIdent())
	code := host.create(1)
	host.expectClosed()

	joiner := dialTCP(t, srv)
	joiner.hello("bob", testIdent())
	joiner.join(code)
	joiner.expectErr(errRoomNotFound)
}

// syncBlobPayload is the SYNC_BLOB payload: u16 length, then the blob.
func syncBlobPayload(blob []byte) []byte {
	return append(binary.BigEndian.AppendUint16(nil, uint16(len(blob))), blob...)
}

func TestSyncBlobRelayedToJoinerOnly(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatchFlags(t, srv, flagSyncBlob)
	payload := syncBlobPayload([]byte{1, 2, 3, 4, 5})
	m.host.send(msgSyncBlob, payload)
	if got := m.joiner.expect(msgSyncBlobFwd); string(got) != string(payload) {
		t.Fatalf("relayed payload %v, want %v", got, payload)
	}
	m.registerPeers(t)
	// The host's next message is GO, so it was never sent the relayed blob.
	m.host.expect(msgGo)
	m.joiner.expect(msgGo)
}

func TestGoWaitsForSyncBlob(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatchFlags(t, srv, flagSyncBlob)
	m.registerPeers(t)
	m.host.expectSilent(300 * time.Millisecond)
	m.joiner.expectSilent(300 * time.Millisecond)
	m.host.send(msgSyncBlob, syncBlobPayload([]byte{9}))
	m.joiner.expect(msgSyncBlobFwd)
	m.host.expect(msgGo)
	m.joiner.expect(msgGo)
}

func TestSyncBlobMaxLengthAccepted(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatchFlags(t, srv, flagSyncBlob)
	payload := syncBlobPayload(make([]byte, maxSyncBlob))
	m.host.send(msgSyncBlob, payload)
	if got := m.joiner.expect(msgSyncBlobFwd); len(got) != len(payload) {
		t.Fatalf("relayed %d bytes, want %d", len(got), len(payload))
	}
}

func TestSyncBlobMalformedIsRejected(t *testing.T) {
	cases := map[string][]byte{
		"empty blob":     {0, 0},
		"too long":       syncBlobPayload(make([]byte, maxSyncBlob+1)),
		"short body":     {0, 5, 1, 2},
		"trailing bytes": append(syncBlobPayload([]byte{1}), 0xFF),
	}
	for name, payload := range cases {
		t.Run(name, func(t *testing.T) {
			srv, _ := startServer(t, nil)
			m := openMatchFlags(t, srv, flagSyncBlob)
			m.host.send(msgSyncBlob, payload)
			m.host.expectErr(errBadMessage)
			m.joiner.expect(msgPeerLeft)
		})
	}
}

func TestSyncBlobFromJoinerIsRejected(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatchFlags(t, srv, flagSyncBlob)
	m.joiner.send(msgSyncBlob, syncBlobPayload([]byte{1}))
	m.joiner.expectErr(errBadMessage)
	m.joiner.expectClosed()
	m.host.expect(msgPeerLeft)
}

func TestSyncBlobWithoutFlagIsRejected(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)
	m.host.send(msgSyncBlob, syncBlobPayload([]byte{1}))
	m.host.expectErr(errBadMessage)
	m.joiner.expect(msgPeerLeft)
}

func TestDuplicateSyncBlobIsRejected(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatchFlags(t, srv, flagSyncBlob)
	m.host.send(msgSyncBlob, syncBlobPayload([]byte{1}))
	m.host.send(msgSyncBlob, syncBlobPayload([]byte{2}))
	m.host.expectErr(errBadMessage)
	m.joiner.expect(msgSyncBlobFwd)
	m.joiner.expect(msgPeerLeft)
}

func TestSyncBlobLogged(t *testing.T) {
	srv, dir := startServer(t, nil)
	m := openMatchFlags(t, srv, flagSyncBlob)
	m.host.send(msgSyncBlob, syncBlobPayload([]byte{0xDE, 0xAD}))
	m.joiner.expect(msgSyncBlobFwd)
	m.registerPeers(t)
	m.host.expect(msgGo)
	m.joiner.expect(msgGo)
	m.host.send(msgBye, nil)
	m.joiner.expect(msgPeerLeft)
	v := waitLog(t, dir)
	if v.SyncBlob == nil || *v.SyncBlob != "dead" {
		t.Fatalf("sync_blob = %v, want \"dead\"", v.SyncBlob)
	}
}

func TestCreateRoomPayloadValidated(t *testing.T) {
	cases := map[string][]byte{
		"one byte":     {2},
		"unknown flag": {2, 0x02},
	}
	for name, payload := range cases {
		t.Run(name, func(t *testing.T) {
			srv, _ := startServer(t, nil)
			c := dialTCP(t, srv)
			c.hello("alice", testIdent())
			c.send(msgCreateRoom, payload)
			c.expectErr(errBadMessage)
			c.expectClosed()
		})
	}
}

// chatPayload is the client CHAT payload: u16 length, then the text.
func chatPayload(text []byte) []byte {
	return append(binary.BigEndian.AppendUint16(nil, uint16(len(text))), text...)
}

// expectChat reads one relayed CHAT frame and checks its sender and text.
func expectChat(t *testing.T, c *tcpClient, from byte, text string) {
	t.Helper()
	p := c.expect(msgChatFwd)
	if len(p) < 3 || p[0] != from || int(binary.BigEndian.Uint16(p[1:3])) != len(text) || string(p[3:]) != text {
		t.Fatalf("CHAT payload %v, want from %d text %q", p, from, text)
	}
}

func TestChatRelayedToPeerOnly(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)
	m.host.send(msgChat, chatPayload([]byte("hi")))
	expectChat(t, m.joiner, 0, "hi")
	m.host.expectSilent(200 * time.Millisecond)
	m.joiner.send(msgChat, chatPayload([]byte("yo")))
	expectChat(t, m.host, 1, "yo")
	m.joiner.expectSilent(200 * time.Millisecond)
}

func TestChatMaxLengthAccepted(t *testing.T) {
	srv, _ := startServer(t, nil)
	m := openMatch(t, srv)
	text := make([]byte, maxChatLen)
	for i := range text {
		text[i] = 'a'
	}
	m.host.send(msgChat, chatPayload(text))
	expectChat(t, m.joiner, 0, string(text))
}

func TestChatBeforeMatchStartIsRejected(t *testing.T) {
	srv, _ := startServer(t, nil)

	idle := dialTCP(t, srv)
	idle.hello("carol", testIdent())
	idle.send(msgChat, chatPayload([]byte("hi")))
	idle.expectErr(errNotInLobby)
	idle.expectClosed()

	// A host in the lobby is also outside a match. Its room closes with it.
	host := dialTCP(t, srv)
	host.hello("alice", testIdent())
	code := host.create(2)
	host.send(msgChat, chatPayload([]byte("hi")))
	host.expectErr(errNotInLobby)
	host.expectClosed()

	joiner := dialTCP(t, srv)
	joiner.hello("bob", testIdent())
	joiner.join(code)
	joiner.expectErr(errRoomNotFound)
}

func TestChatMalformedIsRejected(t *testing.T) {
	cases := map[string][]byte{
		"zero length":    {0, 0},
		"oversize":       chatPayload(make([]byte, maxChatLen+1)),
		"short body":     {0, 3, 'a'},
		"trailing bytes": append(chatPayload([]byte("a")), 0xFF),
		"invalid utf-8":  chatPayload([]byte{0xFF}),
		"control 0x01":   chatPayload([]byte("a\x01b")),
		"newline":        chatPayload([]byte("a\nb")),
		"delete 0x7f":    chatPayload([]byte{'a', 0x7F}),
	}
	for name, payload := range cases {
		t.Run(name, func(t *testing.T) {
			srv, _ := startServer(t, nil)
			m := openMatch(t, srv)
			m.host.send(msgChat, payload)
			m.host.expectErr(errBadMessage)
			m.joiner.expect(msgPeerLeft)
		})
	}
}

func TestChatBucket(t *testing.T) {
	var b chatBucket
	t0 := time.Unix(1000, 0)
	for i := 0; i < chatBurst; i++ {
		if !b.allow(t0) {
			t.Fatalf("message %d denied within the burst", i+1)
		}
	}
	if b.allow(t0) {
		t.Fatal("6th message in the same instant allowed")
	}
	if b.allow(t0.Add(999 * time.Millisecond)) {
		t.Fatal("message allowed before one token refilled")
	}
	if !b.allow(t0.Add(1999 * time.Millisecond)) {
		t.Fatal("message denied after one token refilled")
	}

	// An idle bucket refills to its burst and no further.
	var idle chatBucket
	idle.allow(t0)
	later := t0.Add(time.Minute)
	for i := 0; i < chatBurst; i++ {
		if !idle.allow(later) {
			t.Fatalf("idle message %d denied", i+1)
		}
	}
	if idle.allow(later) {
		t.Fatal("idle bucket refilled past its burst")
	}
}

func TestChatRateLimitDropsSixth(t *testing.T) {
	srv, dir := startServer(t, nil)
	m := openMatch(t, srv)
	texts := []string{"m1", "m2", "m3", "m4", "m5", "m6"}
	for _, s := range texts {
		m.host.send(msgChat, chatPayload([]byte(s)))
	}
	for _, s := range texts[:chatBurst] {
		expectChat(t, m.joiner, 0, s)
	}
	m.joiner.expectSilent(200 * time.Millisecond)
	m.host.send(msgBye, nil)
	m.joiner.expect(msgPeerLeft)

	v := waitLog(t, dir)
	if v.ChatDropped != [2]int{1, 0} {
		t.Fatalf("chat_dropped = %v, want [1 0]", v.ChatDropped)
	}
	if len(v.Chat) != chatBurst {
		t.Fatalf("logged %d chat lines, want %d", len(v.Chat), chatBurst)
	}
}

func TestChatRateLimitIsPerClient(t *testing.T) {
	srv, dir := startServer(t, nil)
	m := openMatch(t, srv)
	for _, s := range []string{"a1", "a2", "a3", "a4", "a5"} {
		m.host.send(msgChat, chatPayload([]byte(s)))
	}
	for _, s := range []string{"a1", "a2", "a3", "a4", "a5"} {
		expectChat(t, m.joiner, 0, s)
	}
	for _, s := range []string{"b1", "b2", "b3", "b4", "b5"} {
		m.joiner.send(msgChat, chatPayload([]byte(s)))
	}
	for _, s := range []string{"b1", "b2", "b3", "b4", "b5"} {
		expectChat(t, m.host, 1, s)
	}
	m.host.send(msgBye, nil)
	m.joiner.expect(msgPeerLeft)

	v := waitLog(t, dir)
	if v.ChatDropped != [2]int{0, 0} {
		t.Fatalf("chat_dropped = %v, want [0 0]", v.ChatDropped)
	}
}

func TestChatInSessionLog(t *testing.T) {
	srv, dir := startServer(t, nil)
	m := openMatch(t, srv)
	m.host.send(msgChat, chatPayload([]byte("hello")))
	expectChat(t, m.joiner, 0, "hello")
	m.joiner.send(msgChat, chatPayload([]byte("reply")))
	expectChat(t, m.host, 1, "reply")
	m.host.send(msgBye, nil)
	m.joiner.expect(msgPeerLeft)

	v := waitLog(t, dir)
	if len(v.Chat) != 2 {
		t.Fatalf("logged %d chat lines, want 2", len(v.Chat))
	}
	if v.Chat[0].Player != 0 || v.Chat[0].Text != "hello" {
		t.Fatalf("chat[0] = %+v", v.Chat[0])
	}
	if v.Chat[1].Player != 1 || v.Chat[1].Text != "reply" {
		t.Fatalf("chat[1] = %+v", v.Chat[1])
	}
	if v.Chat[0].TMs < 0 || v.Chat[1].TMs < v.Chat[0].TMs {
		t.Fatalf("t_ms out of order: %d, %d", v.Chat[0].TMs, v.Chat[1].TMs)
	}
	if v.ChatDropped != [2]int{0, 0} {
		t.Fatalf("chat_dropped = %v, want [0 0]", v.ChatDropped)
	}
}
