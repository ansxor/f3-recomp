package main

import (
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

func sendJoin(t *testing.T, conn *net.UDPConn, srvAddr *net.UDPAddr, room string, slot uint8, delay uint8, nonce uint64, id Identity) {
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
	sendJoin(t, c1, srvAddr, "match1", 0, 2, 0x1111, id)

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
	sendJoin(t, c2, srvAddr, "match1", 0, 2, 0x2222, id)

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
	sendJoin(t, c1, srvAddr, "room_mismatch", 0, 2, 0x1010, baseID)
	recvPacket(t, c1, 2*time.Second) // consume JoinWait

	// C2 joins with different BuildHash
	badBuildID := baseID
	badBuildID.BuildHash[0] ^= 0xFF
	sendJoin(t, c2, srvAddr, "room_mismatch", 0, 2, 0x2020, badBuildID)

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

	// C2 joins with different Delay
	sendJoin(t, c2, srvAddr, "room_mismatch", 0, 4, 0x2021, baseID)
	hRej2, pRej2 := recvPacket(t, c2, 2*time.Second)
	if hRej2.Type != PktJoinReject {
		t.Fatalf("expected PktJoinReject for delay mismatch, got %d", hRej2.Type)
	}
	rejPayload2, _ := UnmarshalJoinRejectPayload(pRej2)
	if rejPayload2.RejectCode != RejectDelayMismatch {
		t.Fatalf("expected RejectDelayMismatch, got %d", rejPayload2.RejectCode)
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
	sendJoin(t, c1, srvAddr, "room_slots", 1, 2, 0x3030, id)
	recvPacket(t, c1, 2*time.Second) // JoinWait

	// C2 also requests slot 1 -> should be rejected with RejectSlotTaken
	sendJoin(t, c2, srvAddr, "room_slots", 1, 2, 0x4040, id)
	hRej, pRej := recvPacket(t, c2, 2*time.Second)
	if hRej.Type != PktJoinReject {
		t.Fatalf("expected Reject, got %d", hRej.Type)
	}
	rejPayload, _ := UnmarshalJoinRejectPayload(pRej)
	if rejPayload.RejectCode != RejectSlotTaken {
		t.Fatalf("expected RejectSlotTaken, got %d", rejPayload.RejectCode)
	}

	// C2 joins with auto slot -> succeeds, match starts
	sendJoin(t, c2, srvAddr, "room_slots", 0, 2, 0x4041, id)
	recvPacket(t, c1, 2*time.Second) // c1 MatchStart
	hStart2, _ := recvPacket(t, c2, 2*time.Second)
	if hStart2.Type != PktMatchStart {
		t.Fatalf("c2 expected MatchStart, got %d", hStart2.Type)
	}

	// C3 tries to join active room -> rejected with RejectMatchInProgress
	sendJoin(t, c3, srvAddr, "room_slots", 0, 2, 0x5050, id)
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
	sendJoin(t, c1, srvAddr, "persist_finish", 0, 2, 0xA1, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, srvAddr, "persist_finish", 0, 2, 0xA2, id)
	hStart1, _ := recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	sessionID := hStart1.SessionID

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
	sendJoin(t, c1, srvAddr, "room_rejoin", 0, 2, 0xB1, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, srvAddr, "room_rejoin", 0, 2, 0xB2, id)
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
	sendJoin(t, c1, srvAddr, "room_rejoin", 0, 2, 0xB3, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, srvAddr, "room_rejoin", 0, 2, 0xB4, id)
	hStart2, _ := recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	newSessionID := hStart2.SessionID
	if newSessionID == oldSessionID {
		t.Fatal("expected new session ID to differ from old session ID")
	}

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
	sendJoin(t, c1, srvAddr, "anti_spoof", 0, 2, 0xC1, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, srvAddr, "anti_spoof", 0, 2, 0xC2, id)
	hStart1, _ := recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	sessionID := hStart1.SessionID

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

	// Test Settings mismatch
	sendJoin(t, c1, srvAddr, "room_settings", 0, 2, 0x1, baseID)
	recvPacket(t, c1, 2*time.Second)
	badSettings := baseID
	badSettings.Settings ^= 0xFF
	sendJoin(t, c2, srvAddr, "room_settings", 0, 2, 0x2, badSettings)
	h, p := recvPacket(t, c2, 2*time.Second)
	if h.Type != PktJoinReject {
		t.Fatalf("expected reject, got %d", h.Type)
	}
	rej, _ := UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectSettingsMismatch {
		t.Fatalf("expected RejectSettingsMismatch, got %d", rej.RejectCode)
	}

	// Test RomCRC mismatch
	sendJoin(t, c1, srvAddr, "room_romcrc", 0, 2, 0x3, baseID)
	recvPacket(t, c1, 2*time.Second)
	badRom := baseID
	badRom.RomCRC[0] ^= 0xFF
	sendJoin(t, c2, srvAddr, "room_romcrc", 0, 2, 0x4, badRom)
	h, p = recvPacket(t, c2, 2*time.Second)
	rej, _ = UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectRomCrcMismatch {
		t.Fatalf("expected RejectRomCrcMismatch, got %d", rej.RejectCode)
	}

	// Test EEPROM CRC mismatch
	sendJoin(t, c1, srvAddr, "room_eeprom", 0, 2, 0x5, baseID)
	recvPacket(t, c1, 2*time.Second)
	badEeprom := baseID
	badEeprom.EepromCRC ^= 0xFF
	sendJoin(t, c2, srvAddr, "room_eeprom", 0, 2, 0x6, badEeprom)
	h, p = recvPacket(t, c2, 2*time.Second)
	rej, _ = UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectEepromCrcMismatch {
		t.Fatalf("expected RejectEepromCrcMismatch, got %d", rej.RejectCode)
	}

	// Test Initial CRC mismatch
	sendJoin(t, c1, srvAddr, "room_initial", 0, 2, 0x7, baseID)
	recvPacket(t, c1, 2*time.Second)
	badInitial := baseID
	badInitial.InitialCRC ^= 0xFF
	sendJoin(t, c2, srvAddr, "room_initial", 0, 2, 0x8, badInitial)
	h, p = recvPacket(t, c2, 2*time.Second)
	rej, _ = UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectInitialCrcMismatch {
		t.Fatalf("expected RejectInitialCrcMismatch, got %d", rej.RejectCode)
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
	sendJoin(t, c1, srvAddr, "room_reval", 0, 2, 0x7777, id)
	recvPacket(t, c1, 2*time.Second)
	sendJoin(t, c2, srvAddr, "room_reval", 0, 2, 0x8888, id)
	recvPacket(t, c1, 2*time.Second)
	recvPacket(t, c2, 2*time.Second)

	// c1 attempts nonce migration with tampered identity
	tamperedID := id
	tamperedID.BuildHash[0] ^= 0xEE
	sendJoin(t, c1, srvAddr, "room_reval", 0, 2, 0x7777, tamperedID)
	h, p := recvPacket(t, c1, 2*time.Second)
	if h.Type != PktJoinReject {
		t.Fatalf("expected reject on tampered identity during migration, got %d", h.Type)
	}
	rej, _ := UnmarshalJoinRejectPayload(p)
	if rej.RejectCode != RejectInvalidIdentity {
		t.Fatalf("expected RejectInvalidIdentity, got %d", rej.RejectCode)
	}
}
