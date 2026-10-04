package main

import (
	"bytes"
	"encoding/hex"
	"testing"
)

func sampleIdentity() Identity {
	var id Identity
	for i := range 7 {
		id.RomCRC[i] = uint32(0x11223344 + i*0x01010101)
	}
	for i := range 32 {
		id.BuildHash[i] = byte(i * 7)
	}
	id.Settings, id.EepromCRC, id.InitialCRC = 0xDEADBEEF, 0xCAFEBABE, 0x12345678
	return id
}

func TestHeaderInvalid(t *testing.T) {
	if _, err := UnmarshalHeader(make([]byte, HeaderSize-1)); err == nil {
		t.Fatal("accepted truncated header")
	}
	for _, header := range []Header{{Magic: 0x1234, Version: ProtocolVersion}, {Magic: Magic, Version: 99}} {
		if _, err := UnmarshalHeader(header.Marshal()); err == nil {
			t.Fatal("accepted invalid magic or unsupported version")
		}
	}
}

func gameWire(t *testing.T) []byte {
	t.Helper()
	// Published big-endian GameData layout, independent of the encoder.
	data, err := hex.DecodeString("0000003c0000003bffffffff010203040506070800010000003a00020100008000010000003c112233440000003c55667788")
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestGameDataWireCompatibility(t *testing.T) {
	wire := gameWire(t)
	decoded, err := UnmarshalGameDataPayload(wire)
	if err != nil {
		t.Fatal(err)
	}
	if decoded.SimulatedFrame != 60 || decoded.AckFrame != 59 || decoded.AckChecksumFrame != 0xffffffff ||
		decoded.PingID != 0x01020304 || decoded.PongID != 0x05060708 || decoded.InputStartFrame != 58 ||
		len(decoded.Inputs) != 2 || decoded.Inputs[0] != 0x100 || decoded.Inputs[1] != 0x80 ||
		len(decoded.Checksums) != 1 || decoded.Checksums[0] != (ChecksumItem{60, 0x11223344}) ||
		decoded.Flags != FlagFinishReq || decoded.FinishFrame != 60 || decoded.FinishCRC != 0x55667788 {
		t.Fatalf("wire fields decoded incorrectly: %+v", decoded)
	}
	encoded := (GameDataPayload{SimulatedFrame: 60, AckFrame: 59, AckChecksumFrame: 0xffffffff,
		PingID: 0x01020304, PongID: 0x05060708, Flags: FlagFinishReq, InputStartFrame: 58,
		Inputs: []uint16{0x100, 0x80}, Checksums: []ChecksumItem{{60, 0x11223344}},
		FinishFrame: 60, FinishCRC: 0x55667788}).Marshal()
	if !bytes.Equal(encoded, wire) {
		t.Fatalf("incompatible wire encoding: %x", encoded)
	}
}

func TestGameDataMalformed(t *testing.T) {
	wire := gameWire(t)
	for size := range len(wire) {
		if _, err := UnmarshalGameDataPayload(wire[:size]); err == nil {
			t.Fatalf("accepted truncation at byte %d", size)
		}
	}
	cases := map[string]func([]byte) []byte{
		"trailer":            func(b []byte) []byte { return append(b, 0) },
		"unknown flags":      func(b []byte) []byte { b[21] |= 4; return b },
		"invalid input bits": func(b []byte) []byte { b[28] = 0x80; return b },
		"input count":        func(b []byte) []byte { b[26], b[27] = 1, 1; return b },
		"frame overflow": func(b []byte) []byte {
			for i := 22; i < 26; i++ {
				b[i] = 0xff
			}
			return b
		},
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			bad := mutate(append([]byte(nil), wire...))
			if _, err := UnmarshalGameDataPayload(bad); err == nil {
				t.Fatal("accepted malformed packet")
			}
		})
	}
}

func TestInputCountLimit(t *testing.T) {
	for _, count := range []int{128, 129, 256} {
		packet := make([]byte, 30+2*count)
		packet[25] = 2
		packet[26], packet[27] = byte(count>>8), byte(count)
		_, err := UnmarshalGameDataPayload(packet)
		if (err == nil) != (count <= 128) {
			t.Errorf("input count %d: client wire limit is 128, parser error=%v", count, err)
		}
	}
}
