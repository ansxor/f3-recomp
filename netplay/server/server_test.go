package main

import (
	"bytes"
	"encoding/binary"
	"net"
	"testing"
	"time"
)

func createTestClient(t *testing.T, srvAddr *net.UDPAddr) *net.UDPConn {
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatalf("listen client udp: %v", err)
	}
	return conn
}

func sendJoin(t *testing.T, conn *net.UDPConn, host bool, srvAddr *net.UDPAddr, room string, slot uint8, delay uint8, nonce uint64, id Identity) {
	hdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktJoinReq,
		SenderSlot: 0xFF,
	}
	payload := JoinReqPayload{
		ClientNonce:   nonce,
		RequestedSlot: slot,
		Delay:         delay,
		RoomName:      room,
		Identity:      id,
		Host:          host,
	}
	data := append(hdr.Marshal(), payload.Marshal()...)
	_, err := conn.WriteToUDP(data, srvAddr)
	if err != nil {
		t.Fatalf("send join: %v", err)
	}
}

func recvPacket(t *testing.T, conn *net.UDPConn, timeout time.Duration) (Header, []byte) {
	_ = conn.SetReadDeadline(time.Now().Add(timeout))
	buf := make([]byte, 2048)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("recv packet failed: %v", err)
	}
	hdr, err := UnmarshalHeader(buf[:n])
	if err != nil {
		t.Fatalf("unmarshal header failed: %v", err)
	}
	return hdr, buf[HeaderSize:n]
}

func TestRoomMatchAndGameDataFlow(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()

	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()

	id := sampleIdentity()

	// Client 1 joins "match1"
	sendJoin(t, c1, true, srvAddr, "match1", 0, 2, 0x1111, id)

	// Client 1 should get JoinWait
	h1, p1 := recvPacket(t, c1, 2*time.Second)
	if h1.Type != PktJoinWait {
		t.Fatalf("expected PktJoinWait, got %d", h1.Type)
	}
	waitPayload, err := UnmarshalJoinWaitPayload(p1)
	if err != nil {
		t.Fatalf("unmarshal wait payload: %v", err)
	}
	if waitPayload.AssignedSlot != 0 {
		t.Fatalf("expected assigned slot 0, got %d", waitPayload.AssignedSlot)
	}

	// Client 2 joins "match1"
	sendJoin(t, c2, false, srvAddr, "match1", 0, 2, 0x2222, id)

	// Both should receive MatchStart
	hStart1, pStart1 := recvPacket(t, c1, 2*time.Second)
	if hStart1.Type != PktMatchStart {
		t.Fatalf("client 1 expected MatchStart, got %d", hStart1.Type)
	}
	ms1, err := UnmarshalMatchStartPayload(pStart1)
	if err != nil {
		t.Fatalf("unmarshal ms1: %v", err)
	}
	if ms1.AssignedSlot != 0 {
		t.Fatalf("client 1 expected slot 0, got %d", ms1.AssignedSlot)
	}

	hStart2, pStart2 := recvPacket(t, c2, 2*time.Second)
	if hStart2.Type != PktMatchStart {
		t.Fatalf("client 2 expected MatchStart, got %d", hStart2.Type)
	}
	ms2, err := UnmarshalMatchStartPayload(pStart2)
	if err != nil {
		t.Fatalf("unmarshal ms2: %v", err)
	}
	if ms2.AssignedSlot != 1 {
		t.Fatalf("client 2 expected slot 1, got %d", ms2.AssignedSlot)
	}

	sessionID := hStart1.SessionID
	if sessionID == 0 || sessionID != hStart2.SessionID {
		t.Fatalf("session ID mismatch: %d vs %d", sessionID, hStart2.SessionID)
	}
	completeHandoff(t, c1, c2, srvAddr, sessionID)

	// Now client 1 sends GameData
	gd1 := GameDataPayload{
		SimulatedFrame:   10,
		AckFrame:         0xFFFFFFFF,
		AckChecksumFrame: 0xFFFFFFFF,
		PingID:           1,
		InputStartFrame:  2,
		Inputs:           []uint16{0x0001, 0x0002},
	}
	hdrGD1 := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktGameData,
		SessionID:  sessionID,
		SenderSlot: 0,
	}
	dataGD1 := append(hdrGD1.Marshal(), gd1.Marshal()...)
	_, err = c1.WriteToUDP(dataGD1, srvAddr)
	if err != nil {
		t.Fatalf("c1 send game data: %v", err)
	}

	// Client 2 should receive it
	hRecv2, pRecv2 := recvPacket(t, c2, 2*time.Second)
	if hRecv2.Type != PktGameData {
		t.Fatalf("client 2 expected GameData, got %d", hRecv2.Type)
	}
	gdRecv2, err := UnmarshalGameDataPayload(pRecv2)
	if err != nil {
		t.Fatalf("unmarshal game data on c2: %v", err)
	}
	if gdRecv2.SimulatedFrame != 10 || len(gdRecv2.Inputs) != 2 || gdRecv2.Inputs[0] != 0x0001 {
		t.Fatalf("unexpected game data received on c2: %+v", gdRecv2)
	}
}

func TestIdentityMismatchRejection(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()

	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()

	baseID := sampleIdentity()

	// C1 joins
	sendJoin(t, c1, true, srvAddr, "room_mismatch", 0, 2, 0x1010, baseID)
	recvPacket(t, c1, 2*time.Second) // consume JoinWait

	// C2 joins with different BuildHash
	badBuildID := baseID
	badBuildID.BuildHash[0] ^= 0xFF
	sendJoin(t, c2, false, srvAddr, "room_mismatch", 0, 2, 0x2020, badBuildID)

	hRej, pRej := recvPacket(t, c2, 2*time.Second)
	if hRej.Type != PktJoinReject {
		t.Fatalf("expected PktJoinReject, got %d", hRej.Type)
	}
	rejPayload, err := UnmarshalJoinRejectPayload(pRej)
	if err != nil {
		t.Fatalf("unmarshal reject payload: %v", err)
	}
	if rejPayload.RejectCode != RejectBuildHashMismatch {
		t.Fatalf("expected RejectBuildHashMismatch, got %d", rejPayload.RejectCode)
	}

	// Guest delay does not override the host's choice.
	sendJoin(t, c2, false, srvAddr, "room_mismatch", 0, 4, 0x2021, baseID)
	hStart, paired := recvPacket(t, c2, 2*time.Second)
	start, err := UnmarshalMatchStartPayload(paired)
	if hStart.Type != PktMatchStart || err != nil || start.Delay != 2 {
		t.Fatalf("host delay was not negotiated: %+v, %v", start, err)
	}
}

func TestRoomCapacityAndSlotConflict(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()

	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()
	c3 := createTestClient(t, srvAddr)
	defer c3.Close()

	id := sampleIdentity()

	// C1 requests slot 1 (maps to internal slot 0)
	sendJoin(t, c1, true, srvAddr, "room_slots", 1, 2, 0x3030, id)
	recvPacket(t, c1, 2*time.Second) // JoinWait

	// C2 also requests slot 1 -> should be rejected with RejectSlotTaken
	sendJoin(t, c2, false, srvAddr, "room_slots", 1, 2, 0x4040, id)
	hRej, pRej := recvPacket(t, c2, 2*time.Second)
	if hRej.Type != PktJoinReject {
		t.Fatalf("expected Reject, got %d", hRej.Type)
	}
	rejPayload, _ := UnmarshalJoinRejectPayload(pRej)
	if rejPayload.RejectCode != RejectSlotTaken {
		t.Fatalf("expected RejectSlotTaken, got %d", rejPayload.RejectCode)
	}

	// C2 joins with auto slot -> succeeds, match starts
	sendJoin(t, c2, false, srvAddr, "room_slots", 0, 2, 0x4041, id)
	recvPacket(t, c1, 2*time.Second) // c1 MatchStart
	hStart2, _ := recvPacket(t, c2, 2*time.Second)
	if hStart2.Type != PktMatchStart {
		t.Fatalf("c2 expected MatchStart, got %d", hStart2.Type)
	}

	// C3 tries to join active room -> rejected with RejectMatchInProgress
	sendJoin(t, c3, false, srvAddr, "room_slots", 0, 2, 0x5050, id)
	hRej3, pRej3 := recvPacket(t, c3, 2*time.Second)
	if hRej3.Type != PktJoinReject {
		t.Fatalf("expected Reject for 3rd player, got %d", hRej3.Type)
	}
	rejPayload3, _ := UnmarshalJoinRejectPayload(pRej3)
	if rejPayload3.RejectCode != RejectMatchInProgress {
		t.Fatalf("expected RejectMatchInProgress, got %d", rejPayload3.RejectCode)
	}
}

func TestDroppedCompletionRetryAfterOnePeerLeave(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()
	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()

	id := sampleIdentity()
	sendJoin(t, c1, true, srvAddr, "persist_finish", 0, 2, 0xA1, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, false, srvAddr, "persist_finish", 0, 2, 0xA2, id)
	hStart1, _ := recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	sessionID := hStart1.SessionID
	completeHandoff(t, c1, c2, srvAddr, sessionID)

	// C1 finishes
	fin1 := GameDataPayload{
		SimulatedFrame: 500,
		Flags:          FlagFinishReq,
		FinishFrame:    500,
		FinishCRC:      0x99887766,
	}
	hdr1 := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktGameData,
		SessionID:  sessionID,
		SenderSlot: 0,
		Flags:      FlagFinishReq,
	}
	c1.WriteToUDP(append(hdr1.Marshal(), fin1.Marshal()...), srvAddr)

	// C2 finishes
	fin2 := GameDataPayload{
		SimulatedFrame: 500,
		Flags:          FlagFinishReq | FlagFinishAck,
		FinishFrame:    500,
		FinishCRC:      0x99887766,
	}
	hdr2 := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktGameData,
		SessionID:  sessionID,
		SenderSlot: 1,
		Flags:      FlagFinishReq | FlagFinishAck,
	}
	c2.WriteToUDP(append(hdr2.Marshal(), fin2.Marshal()...), srvAddr)

	// C1 sends graceful leave (NormalFinished)
	leaveHdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktLeave,
		SessionID:  sessionID,
		SenderSlot: 0,
	}
	leavePayload := LeavePayload{
		LeaveCode: LeaveNormalFinished,
		Message:   "finished",
	}
	c1.WriteToUDP(append(leaveHdr.Marshal(), leavePayload.Marshal()...), srvAddr)

	// C2 now retransmits its finish packet (simulating dropped PktMatchComplete retry)
	c2.WriteToUDP(append(hdr2.Marshal(), fin2.Marshal()...), srvAddr)

	// C2 must receive PktMatchComplete from server despite C1 having left!
	foundComplete := false
	for range 5 {
		h, p := recvPacket(t, c2, 2*time.Second)
		if h.Type == PktMatchComplete {
			cmp, err := UnmarshalMatchCompletePayload(p)
			if err != nil {
				t.Fatalf("unmarshal match complete: %v", err)
			}
			if cmp.FinalFrame == 500 && cmp.FinalCRC == 0x99887766 {
				foundComplete = true
				break
			}
		}
	}
	if !foundComplete {
		t.Fatal("C2 did not receive persistent PktMatchComplete verdict after peer left")
	}
}

func TestStaleLeaveAfterNewSession(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()
	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()

	id := sampleIdentity()
	sendJoin(t, c1, true, srvAddr, "room_rejoin", 0, 2, 0xB1, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, false, srvAddr, "room_rejoin", 0, 2, 0xB2, id)
	hStart1, _ := recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	oldSessionID := hStart1.SessionID

	// Abort old match by leaving
	abortHdr := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktLeave,
		SessionID:  oldSessionID,
		SenderSlot: 0,
	}
	c1.WriteToUDP(append(abortHdr.Marshal(), LeavePayload{LeaveCode: LeaveAbort}.Marshal()...), srvAddr)
	recvPacket(t, c2, 2*time.Second) // consume PktMatchTerminated

	// Start fresh match in same room
	sendJoin(t, c1, true, srvAddr, "room_rejoin", 0, 2, 0xB3, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, false, srvAddr, "room_rejoin", 0, 2, 0xB4, id)
	hStart2, _ := recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	newSessionID := hStart2.SessionID
	if newSessionID == oldSessionID {
		t.Fatal("expected new session ID to differ from old session ID")
	}
	completeHandoff(t, c1, c2, srvAddr, newSessionID)

	// Now inject delayed stale leave with oldSessionID
	staleLeave := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktLeave,
		SessionID:  oldSessionID,
		SenderSlot: 0,
	}
	c1.WriteToUDP(append(staleLeave.Marshal(), LeavePayload{LeaveCode: LeaveAbort}.Marshal()...), srvAddr)

	// Verify new match is STILL alive by sending GameData on newSessionID
	gd := GameDataPayload{SimulatedFrame: 10}
	hdrGD := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktGameData,
		SessionID:  newSessionID,
		SenderSlot: 0,
	}
	c1.WriteToUDP(append(hdrGD.Marshal(), gd.Marshal()...), srvAddr)

	hRecv, _ := recvPacket(t, c2, 2*time.Second)
	if hRecv.Type != PktGameData {
		t.Fatalf("expected active new session GameData, got packet type %d", hRecv.Type)
	}
}

func TestSlotEndpointSpoofing(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()
	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()
	cRogue := createTestClient(t, srvAddr)
	defer cRogue.Close()

	id := sampleIdentity()
	sendJoin(t, c1, true, srvAddr, "anti_spoof", 0, 2, 0xC1, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, false, srvAddr, "anti_spoof", 0, 2, 0xC2, id)
	hStart1, _ := recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	sessionID := hStart1.SessionID
	completeHandoff(t, c1, c2, srvAddr, sessionID)

	// Rogue client attempts to send packet claiming Slot 0 (C1's slot)
	gdSpoof := GameDataPayload{SimulatedFrame: 999}
	hdrSpoof := Header{
		Magic:      Magic,
		Version:    ProtocolVersion,
		Type:       PktGameData,
		SessionID:  sessionID,
		SenderSlot: 0, // Claiming slot 0 from rogue IP/port
	}
	cRogue.WriteToUDP(append(hdrSpoof.Marshal(), gdSpoof.Marshal()...), srvAddr)

	// C2 should NOT receive this spoofed packet (read deadline times out)
	c2.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
	buf := make([]byte, 2048)
	n, _, err := c2.ReadFromUDP(buf)
	if err == nil {
		t.Fatalf("expected spoofed packet to be dropped, but C2 received %d bytes", n)
	}
}

func TestProtocolMismatchRejection(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()
	c := createTestClient(t, srvAddr)
	defer c.Close()

	hdr := Header{
		Magic:      Magic,
		Version:    99, // Unsupported protocol version
		Type:       PktJoinReq,
		SenderSlot: 0xFF,
	}
	payload := JoinReqPayload{
		ClientNonce: 0xDEADBEEF,
		RoomName:    "mismatch_proto",
		Identity:    sampleIdentity(),
	}
	c.WriteToUDP(append(hdr.Marshal(), payload.Marshal()...), srvAddr)

	hRej, pRej := recvPacket(t, c, 2*time.Second)
	if hRej.Type != PktJoinReject {
		t.Fatalf("expected PktJoinReject on version mismatch, got %d", hRej.Type)
	}
	rej, _ := UnmarshalJoinRejectPayload(pRej)
	if rej.RejectCode != RejectProtocolMismatch {
		t.Fatalf("expected RejectProtocolMismatch, got %d", rej.RejectCode)
	}
}

func TestIdentityMismatchesAllFields(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()
	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()

	baseID := sampleIdentity()

	// Snapshot representation compatibility remains mandatory.
	sendJoin(t, c1, true, srvAddr, "room_format", 0, 2, 0x1, baseID)
	recvPacket(t, c1, 2*time.Second)
	badFormat := baseID
	badFormat.StateFormat ^= 0xFF
	sendJoin(t, c2, false, srvAddr, "room_format", 0, 2, 0x2, badFormat)
	h, p := recvPacket(t, c2, 2*time.Second)
	if h.Type != PktJoinReject {
		t.Fatalf("expected reject, got %d", h.Type)
	}
	rej, _ := UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectStateFormatMismatch {
		t.Fatalf("expected RejectStateFormatMismatch, got %d", rej.RejectCode)
	}

	// Test RomCRC mismatch
	sendJoin(t, c1, true, srvAddr, "room_romcrc", 0, 2, 0x3, baseID)
	recvPacket(t, c1, 2*time.Second)
	badRom := baseID
	badRom.RomCRC[0] ^= 0xFF
	sendJoin(t, c2, false, srvAddr, "room_romcrc", 0, 2, 0x4, badRom)
	h, p = recvPacket(t, c2, 2*time.Second)
	rej, _ = UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectRomCrcMismatch {
		t.Fatalf("expected RejectRomCrcMismatch, got %d", rej.RejectCode)
	}

}

func TestNonceMigrationRevalidation(t *testing.T) {
	srv, err := NewServer(ServerConfig{
		Addr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatalf("new server: %v", err)
	}
	defer srv.Close()

	srvAddr := srv.LocalAddr()
	c1 := createTestClient(t, srvAddr)
	defer c1.Close()
	c2 := createTestClient(t, srvAddr)
	defer c2.Close()

	id := sampleIdentity()
	sendJoin(t, c1, true, srvAddr, "room_reval", 0, 2, 0x7777, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, false, srvAddr, "room_reval", 0, 2, 0x8888, id)
	recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	// c1 attempts nonce migration with tampered identity
	tamperedID := id
	tamperedID.BuildHash[0] ^= 0xEE
	sendJoin(t, c1, true, srvAddr, "room_reval", 0, 2, 0x7777, tamperedID)
	h, p := recvPacket(t, c1, 2*time.Second)
	if h.Type != PktJoinReject {
		t.Fatalf("expected reject on tampered identity during migration, got %d", h.Type)
	}
	rej, _ := UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectInvalidIdentity {
		t.Fatalf("expected RejectInvalidIdentity, got %d", rej.RejectCode)
	}
}

func sendSessionPacket(t *testing.T, conn *net.UDPConn, server *net.UDPAddr, session uint64, slot uint8, kind uint8, payload []byte) {
	t.Helper()
	header := Header{Magic: Magic, Version: ProtocolVersion, Type: kind, SessionID: session, SenderSlot: slot}
	if _, err := conn.WriteToUDP(append(header.Marshal(), payload...), server); err != nil {
		t.Fatal(err)
	}
}

func completeHandoff(t *testing.T, host, guest *net.UDPConn, server *net.UDPAddr, session uint64) {
	t.Helper()
	meta := SnapshotMetaPayload{TransferID: 1, RawSize: 1, CompressedSize: 1, RawCRC: 0x1234, CompressedCRC: 0x5678}
	sendSessionPacket(t, host, server, session, 0, PktSnapshotMeta, meta.Marshal())
	if h, _ := recvPacket(t, guest, 2*time.Second); h.Type != PktSnapshotMeta {
		t.Fatalf("expected metadata, got %d", h.Type)
	}
	sendSessionPacket(t, guest, server, session, 1, PktSnapshotLoaded, (SnapshotReceiptPayload{TransferID: 1, RawCRC: meta.RawCRC}).Marshal())
	for _, conn := range []*net.UDPConn{host, guest} {
		h, payload := recvPacket(t, conn, 2*time.Second)
		receipt, err := UnmarshalSnapshotReceiptPayload(payload)
		if h.Type != PktBarrierStart || h.SessionID != session || err != nil || receipt.RawCRC != meta.RawCRC {
			t.Fatalf("invalid start barrier: %+v %+v %v", h, receipt, err)
		}
	}
}

func TestSnapshotRelayAuthorityBoundsAndBarrier(t *testing.T) {
	srv, err := NewServer(ServerConfig{Addr: "127.0.0.1:0"})
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	a := createTestClient(t, srv.LocalAddr())
	defer a.Close()
	b := createTestClient(t, srv.LocalAddr())
	defer b.Close()
	rogue := createTestClient(t, srv.LocalAddr())
	defer rogue.Close()
	address := func(conn *net.UDPConn) *net.UDPAddr { return conn.LocalAddr().(*net.UDPAddr) }
	join := func(conn *net.UDPConn, host bool, nonce uint64, delay uint8) {
		header := Header{Magic: Magic, Version: ProtocolVersion, Type: PktJoinReq, SenderSlot: 0xFF}
		req := JoinReqPayload{ClientNonce: nonce, Delay: delay, RoomName: "snapshot", Identity: sampleIdentity(), Host: host}
		srv.handlePacket(address(conn), append(header.Marshal(), req.Marshal()...))
	}
	// Guest joins first, occupies slot zero, and cannot choose delay.
	join(a, false, 1, 7)
	recvPacket(t, a, time.Second)
	join(b, false, 2, 9)
	h, payload := recvPacket(t, b, time.Second)
	reject, err := UnmarshalJoinRejectPayload(payload)
	if h.Type != PktJoinReject || err != nil || reject.RejectCode != RejectRoleConflict {
		t.Fatalf("same-role pair accepted: %+v %+v %v", h, reject, err)
	}
	join(b, true, 2, 3)
	h, payload = recvPacket(t, a, time.Second)
	start, err := UnmarshalMatchStartPayload(payload)
	if h.Type != PktMatchStart || err != nil || start.Delay != 3 || start.AssignedSlot != 0 {
		t.Fatalf("host-second negotiation failed: %+v %v", start, err)
	}
	session := h.SessionID
	recvPacket(t, b, time.Second)
	deliver := func(conn *net.UDPConn, slot uint8, kind uint8, body []byte) {
		header := Header{Magic: Magic, Version: ProtocolVersion, Type: kind, SessionID: session, SenderSlot: slot}
		srv.handlePacket(address(conn), append(header.Marshal(), body...))
	}
	silent := func(conn *net.UDPConn) {
		t.Helper()
		conn.SetReadDeadline(time.Now().Add(10 * time.Millisecond))
		var buffer [MaxPacketSize]byte
		_, _, err := conn.ReadFromUDP(buffer[:])
		if networkErr, ok := err.(net.Error); !ok || !networkErr.Timeout() {
			t.Fatalf("invalid traffic was delivered: %v", err)
		}
	}
	// Pairing alone cannot release gameplay, including finish packets.
	deliver(a, 0, PktGameData, (GameDataPayload{Flags: FlagFinishReq, FinishFrame: 1, FinishCRC: 2}).Marshal())
	silent(b)
	meta := SnapshotMetaPayload{TransferID: 1, RawSize: 2048, CompressedSize: 1025, RawCRC: 11, CompressedCRC: 22}
	for _, invalid := range []SnapshotMetaPayload{
		{TransferID: 2, RawSize: 1, CompressedSize: 1},
		{TransferID: 1, RawSize: 0, CompressedSize: 1},
		{TransferID: 1, RawSize: MaxSnapshotSize + 1, CompressedSize: 1},
		{TransferID: 1, RawSize: 1, CompressedSize: 0},
		{TransferID: 1, RawSize: 1, CompressedSize: MaxSnapshotSize + 1},
	} {
		deliver(b, 1, PktSnapshotMeta, invalid.Marshal())
		silent(a)
	}
	deliver(a, 0, PktSnapshotMeta, meta.Marshal())
	silent(b)
	deliver(rogue, 1, PktSnapshotMeta, meta.Marshal())
	silent(a)
	deliver(b, 1, PktSnapshotMeta, append(meta.Marshal(), 0))
	silent(a)
	deliver(b, 1, PktSnapshotMeta, meta.Marshal())
	if h, _ := recvPacket(t, a, time.Second); h.Type != PktSnapshotMeta {
		t.Fatal("valid host metadata not relayed")
	}
	for _, mutate := range []func(*SnapshotMetaPayload){
		func(p *SnapshotMetaPayload) { p.RawCRC++ },
		func(p *SnapshotMetaPayload) { p.CompressedCRC++ },
		func(p *SnapshotMetaPayload) { p.CompressedSize++ },
		func(p *SnapshotMetaPayload) { p.RawSize++ },
	} {
		changed := meta
		mutate(&changed)
		deliver(b, 1, PktSnapshotMeta, changed.Marshal())
		silent(a)
	}
	chunk := func(index uint32, size int) []byte {
		body := make([]byte, 8+size)
		binary.BigEndian.PutUint32(body[:4], 1)
		binary.BigEndian.PutUint32(body[4:8], index)
		return body
	}
	for _, invalid := range [][]byte{chunk(2, 1), chunk(0, 1023), chunk(0, 1025), chunk(1, 2), chunk(^uint32(0), 1)} {
		deliver(b, 1, PktSnapshotChunk, invalid)
		silent(a)
	}
	deliver(a, 0, PktSnapshotChunk, chunk(1, 1))
	silent(b)
	// Reordered and duplicate valid chunks remain relayable.
	for _, body := range [][]byte{chunk(1, 1), chunk(0, 1024), chunk(1, 1)} {
		deliver(b, 1, PktSnapshotChunk, body)
		h, received := recvPacket(t, a, time.Second)
		if h.Type != PktSnapshotChunk || !bytes.Equal(received, body) {
			t.Fatal("valid chunk corrupted or not relayed")
		}
	}
	deliver(b, 1, PktSnapshotAck, chunk(0, 0))
	silent(a)
	deliver(a, 0, PktSnapshotAck, chunk(2, 0))
	silent(b)
	deliver(a, 0, PktSnapshotAck, chunk(0, 1))
	silent(b)
	deliver(a, 0, PktSnapshotAck, chunk(0, 0))
	if h, _ := recvPacket(t, b, time.Second); h.Type != PktSnapshotAck {
		t.Fatal("valid guest ack not relayed")
	}
	deliver(a, 0, PktSnapshotLoaded, (SnapshotReceiptPayload{TransferID: 1, RawCRC: 12}).Marshal())
	silent(a)
	silent(b)
	deliver(b, 1, PktSnapshotLoaded, (SnapshotReceiptPayload{TransferID: 1, RawCRC: 11}).Marshal())
	silent(a)
	deliver(a, 0, PktSnapshotLoaded, (SnapshotReceiptPayload{TransferID: 1, RawCRC: 11}).Marshal())
	for _, conn := range []*net.UDPConn{a, b} {
		h, body := recvPacket(t, conn, time.Second)
		receipt, err := UnmarshalSnapshotReceiptPayload(body)
		if h.Type != PktBarrierStart || h.SenderSlot != 0xFF || h.SessionID != session || err != nil || receipt.RawCRC != 11 {
			t.Fatalf("invalid barrier: %+v %+v %v", h, receipt, err)
		}
	}
	// Dropped barrier retries on heartbeat; a heartbeat cannot acknowledge it.
	deliver(b, 1, PktHeartbeat, (HeartbeatPayload{}).Marshal())
	recvPacket(t, a, time.Second) // relayed heartbeat
	for _, conn := range []*net.UDPConn{a, b} {
		if h, _ := recvPacket(t, conn, time.Second); h.Type != PktBarrierStart {
			t.Fatal("barrier was not retried")
		}
	}
	deliver(a, 0, PktGameData, (GameDataPayload{SimulatedFrame: 5}).Marshal())
	if h, _ := recvPacket(t, b, time.Second); h.Type != PktGameData {
		t.Fatal("released game data not relayed")
	}
	deliver(b, 1, PktSnapshotMeta, meta.Marshal())
	if h, _ := recvPacket(t, a, time.Second); h.Type != PktSnapshotMeta {
		t.Fatal("metadata retry not relayed")
	}
	if h, _ := recvPacket(t, b, time.Second); h.Type != PktBarrierStart {
		t.Fatal("unconfirmed host barrier not retried")
	}
	silent(a) // Guest traffic already acknowledged its barrier.
}

func TestCleanFinishedRoomReuseAndStaleSessionIsolation(t *testing.T) {
	srv, err := NewServer(ServerConfig{Addr: "127.0.0.1:0"})
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	a := createTestClient(t, srv.LocalAddr())
	defer a.Close()
	b := createTestClient(t, srv.LocalAddr())
	defer b.Close()
	addrA := a.LocalAddr().(*net.UDPAddr)
	addrB := b.LocalAddr().(*net.UDPAddr)
	room := NewRoom("reuse_finished")
	room.ProcessJoin(addrA, JoinReqPayload{ClientNonce: 1, Host: true, Delay: 2, Identity: sampleIdentity()})
	paired := room.ProcessJoin(addrB, JoinReqPayload{ClientNonce: 2, Identity: sampleIdentity()})
	oldSession := paired.SessionID
	meta := SnapshotMetaPayload{TransferID: 1, RawSize: 1, CompressedSize: 1, RawCRC: 5}
	if !room.RelaySnapshot(0, PktSnapshotMeta, meta.Marshal()) ||
		!room.RelaySnapshot(1, PktSnapshotLoaded, (SnapshotReceiptPayload{TransferID: 1, RawCRC: 5}).Marshal()) {
		t.Fatal("snapshot was not accepted")
	}
	room.MarkFinish(0, addrA, 60, 100)
	if complete, _, _, _, _ := room.MarkFinish(1, addrB, 60, 100); !complete {
		t.Fatal("matching finish failed")
	}
	srv.mu.Lock()
	srv.rooms[room.Name] = room
	srv.sessions[oldSession] = room
	srv.mu.Unlock()
	srv.handleLeave(addrA, Header{SessionID: oldSession, SenderSlot: 0}, (LeavePayload{LeaveCode: LeaveNormalFinished}).Marshal())
	// Completion remains available while the remaining peer has not left.
	if finished, frame, crc := room.GetFinishVerdict(addrB); !finished || frame != 60 || crc != 100 {
		t.Fatal("first clean leave erased persistent verdict")
	}
	srv.handleLeave(addrB, Header{SessionID: oldSession, SenderSlot: 1}, (LeavePayload{LeaveCode: LeaveNormalFinished}).Marshal())
	srv.mu.RLock()
	_, staleRegistered := srv.sessions[oldSession]
	srv.mu.RUnlock()
	if staleRegistered {
		t.Fatal("both clean leaves retained old session routing")
	}
	if staleJoin := room.ProcessJoin(addrA, JoinReqPayload{ClientNonce: 1, Host: true, Identity: sampleIdentity()}); staleJoin.RejectCode == 0 {
		t.Fatal("departed JoinReq was allowed to reoccupy reused room")
	}
	fresh := room.ProcessJoin(addrA, JoinReqPayload{ClientNonce: 3, Host: true, Delay: 4, Identity: sampleIdentity()})
	if fresh.RejectCode != 0 || fresh.MatchStarted {
		t.Fatalf("finished room cannot be reused: %+v", fresh)
	}
	next := room.ProcessJoin(addrB, JoinReqPayload{ClientNonce: 4, Delay: 9, Identity: sampleIdentity()})
	if !next.MatchStarted || next.SessionID == oldSession || next.Delay != 4 || room.SnapshotMeta != nil || room.BarrierReleased {
		t.Fatalf("new session inherited old transfer or delay: %+v", next)
	}
	srv.mu.Lock()
	srv.sessions[next.SessionID] = room
	srv.mu.Unlock()
	// Old-session loaded, finish, leave, and heartbeat traffic cannot mutate
	// the replacement room even though both endpoints are reused.
	for _, kind := range []uint8{PktSnapshotLoaded, PktGameData, PktLeave, PktHeartbeat} {
		var body []byte
		switch kind {
		case PktSnapshotLoaded:
			body = (SnapshotReceiptPayload{TransferID: 1, RawCRC: 5}).Marshal()
		case PktGameData:
			body = (GameDataPayload{Flags: FlagFinishReq, FinishFrame: 60, FinishCRC: 100}).Marshal()
		case PktLeave:
			body = (LeavePayload{LeaveCode: LeaveAbort}).Marshal()
		case PktHeartbeat:
			body = (HeartbeatPayload{}).Marshal()
		}
		header := Header{Magic: Magic, Version: ProtocolVersion, Type: kind, SessionID: oldSession, SenderSlot: 1}
		srv.handlePacket(addrB, append(header.Marshal(), body...))
	}
	if room.State != RoomActive || room.BarrierReleased || room.Clients[1].Finished || !room.Clients[1].Occupied {
		t.Fatal("stale session mutated replacement match")
	}
}

func TestWaitingCancellationAuthenticatesNonceAndEndpoint(t *testing.T) {
	srv, err := NewServer(ServerConfig{Addr: "127.0.0.1:0"})
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	a := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 23456}
	rogue := &net.UDPAddr{IP: a.IP, Port: 23457}
	room := NewRoom("cancel_wait")
	req := JoinReqPayload{ClientNonce: 10, Host: true, Delay: 2, Identity: sampleIdentity()}
	room.ProcessJoin(a, req)
	srv.mu.Lock()
	srv.rooms[room.Name] = room
	srv.mu.Unlock()
	cancel := func(addr *net.UDPAddr, nonce uint64) {
		payload := make([]byte, 65)
		payload[0] = LeaveAbort
		binary.BigEndian.PutUint64(payload[1:9], nonce)
		srv.handleLeave(addr, Header{SenderSlot: 0xFF}, payload)
	}
	cancel(rogue, 10)
	cancel(a, 11)
	if !room.Clients[0].Occupied {
		t.Fatal("unauthenticated waiting cancellation removed peer")
	}
	cancel(a, 10)
	if room.Clients[0].Occupied {
		t.Fatal("authenticated waiting cancellation retained peer")
	}
	if staleJoin := room.ProcessJoin(a, JoinReqPayload{ClientNonce: 10, Host: true, Identity: sampleIdentity()}); staleJoin.RejectCode == 0 {
		t.Fatal("cancelled JoinReq was accepted again")
	}
	req.ClientNonce = 20
	room.ProcessJoin(a, req)
	cancel(a, 10)
	if !room.Clients[0].Occupied || room.Clients[0].Nonce != 20 {
		t.Fatal("stale cancellation removed replacement request")
	}
	room.ProcessJoin(rogue, JoinReqPayload{ClientNonce: 21, Identity: sampleIdentity()})
	cancel(a, 20)
	if room.State != RoomActive || !room.Clients[0].Occupied {
		t.Fatal("session-zero cancellation affected paired match")
	}
}

func TestRetiredNonceCapacityAndExpiry(t *testing.T) {
	room := NewRoom("bounded_retirement")
	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 12345}
	for nonce := uint64(1); nonce <= 62; nonce++ {
		result := room.ProcessJoin(addr, JoinReqPayload{ClientNonce: nonce, Host: true, Identity: sampleIdentity()})
		if result.RejectCode != 0 || !room.CancelWaiting(addr, nonce) {
			t.Fatalf("unexpected rejection before bounded capacity at nonce %d: %+v", nonce, result)
		}
	}
	if result := room.ProcessJoin(addr, JoinReqPayload{ClientNonce: 63, Host: true, Identity: sampleIdentity()}); result.RejectCode != RejectRateLimited {
		t.Fatal("tombstone capacity did not bound admission")
	}
	room.CreatedAt = time.Now().Add(-WaitingRoomTimeout - time.Second)
	if room.CanRemove(time.Now()) {
		t.Fatal("room expiry erased recent retired nonces")
	}
	for i := range room.Retired {
		room.Retired[i].Until = time.Now().Add(-time.Second)
	}
	result := room.ProcessJoin(addr, JoinReqPayload{ClientNonce: 63, Host: true, Identity: sampleIdentity()})
	if result.RejectCode != 0 {
		t.Fatalf("expired tombstones still block room admission: %+v", result)
	}
	if room.CanRemove(time.Now()) {
		t.Fatal("fresh waiting peer inherited expired room age")
	}
}

func TestRejectedJoinDoesNotPinFinishedRoom(t *testing.T) {
	room := NewRoom("finished_expiry")
	a := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 12001}
	b := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 12002}
	room.ProcessJoin(a, JoinReqPayload{ClientNonce: 1, Host: true, Identity: sampleIdentity()})
	room.ProcessJoin(b, JoinReqPayload{ClientNonce: 2, Identity: sampleIdentity()})
	room.BarrierReleased = true
	room.MarkFinish(0, a, 60, 100)
	if complete, _, _, _, _ := room.MarkFinish(1, b, 60, 100); !complete {
		t.Fatal("matching completion was not retained")
	}
	now := time.Now()
	room.LastActive = now.Add(-FinishedRoomTimeout - time.Second)
	fresh := JoinReqPayload{ClientNonce: 3, Host: true, Identity: sampleIdentity()}
	if result := room.ProcessJoin(a, fresh); result.RejectCode != RejectMatchInProgress {
		t.Fatalf("expected occupied finished room: %+v", result)
	}
	room.CanRemove(now)
	if result := room.ProcessJoin(a, fresh); result.RejectCode != 0 || result.MatchStarted {
		t.Fatalf("rejected traffic prevented expired room reuse: %+v", result)
	}
}

func TestLostFinishedLeaveEventuallyAllowsRematch(t *testing.T) {
	room := NewRoom("lost_leave")
	a := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 12003}
	b := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 12004}
	room.ProcessJoin(a, JoinReqPayload{ClientNonce: 1, Host: true, Identity: sampleIdentity()})
	room.ProcessJoin(b, JoinReqPayload{ClientNonce: 2, Identity: sampleIdentity()})
	room.BarrierReleased = true
	room.MarkFinish(0, a, 60, 100)
	room.MarkFinish(1, b, 60, 100)
	room.HandleLeave(0, room.SessionID, a, LeaveNormalFinished)
	room.Clients[1].LastActive = time.Now().Add(-2 * time.Second)
	if slot, _ := room.CheckTimeouts(time.Second); slot != 1 {
		t.Fatal("lost final Leave retained a dead occupant")
	}
	if result := room.ProcessJoin(a, JoinReqPayload{ClientNonce: 3, Host: true, Identity: sampleIdentity()}); result.RejectCode != 0 {
		t.Fatalf("dead completed session still blocks rematch: %+v", result)
	}
}
