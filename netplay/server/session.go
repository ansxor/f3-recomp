package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

const (
	// maxSessionFrames bounds frame numbers for inputs and CRC reports (2^20 frames).
	maxSessionFrames = 1 << 20

	maxCRCPending       = 256
	maxRecordedMismatch = 256
	maxRecordedErrors   = 256
)

// session is one match. All fields are guarded by Server.mu. Once ended, no
// handler mutates it, so snapshot output can be written after unlock.
type session struct {
	id         uint64
	key        [32]byte
	seed       uint64
	ident      identity
	names      [2]string
	inputDelay uint8
	room       *room
	clients    [2]*client

	started   time.Time
	endedAt   time.Time
	ended     bool
	endReason string
	endedBy   int

	udpAddr      [2]netip.AddrPort
	registered   [2]bool
	goSent       bool
	syncRequired bool   // room requires the host's SYNC_BLOB before GO
	syncSeen     bool   // the host's SYNC_BLOB was accepted and relayed
	syncBlob     []byte // relayed blob, recorded in the log

	inputs          [2][]int32 // frame -> input, -1 for missing
	protoErrs       []string
	protoErrDropped int

	crcPend       map[uint32]*crcEntry
	crcCompared   int
	crcMismatches []mismatchRec
	crcMismatchN  int
	crcEvicted    int
	desyncSent    bool

	results [2]*resultRec
	ack     *ackRec

	chat        []chatRec
	chatDropped [2]int
}

type crcEntry struct {
	has [2]bool
	crc [2]uint32
}

type desyncInfo struct {
	frame, crc1, crc2 uint32
}

type resultRec struct {
	Frame    uint32 `json:"frame"`
	Winner   uint8  `json:"winner"`
	FinalCRC uint32 `json:"final_crc"`
}

type ackRec struct {
	Agreed bool  `json:"agreed"`
	Winner uint8 `json:"winner"`
}

type mismatchRec struct {
	Frame uint32 `json:"frame"`
	CRC1  uint32 `json:"crc_p1"`
	CRC2  uint32 `json:"crc_p2"`
}

// chatRec is one accepted CHAT line. TMs counts milliseconds from session start.
type chatRec struct {
	Player uint8  `json:"player"`
	TMs    int64  `json:"t_ms"`
	Text   string `json:"text"`
}

func newSession(id uint64, r *room, host, joiner *client) *session {
	s := &session{
		id:           id,
		ident:        r.id,
		names:        [2]string{host.name, joiner.name},
		inputDelay:   r.inputDelay,
		syncRequired: r.syncRequired,
		room:         r,
		clients:      [2]*client{host, joiner},
		started:      time.Now(),
		crcPend:      make(map[uint32]*crcEntry),
	}
	mustRand(s.key[:])
	var seed [8]byte
	mustRand(seed[:])
	s.seed = beUint64(seed[:])
	return s
}

func (s *session) end(reason string, endedBy int) {
	s.ended = true
	s.endedAt = time.Now()
	s.endReason = reason
	s.endedBy = endedBy
}

// recordCRC stores one CRC report. A match returns desync info on the first
// mismatch of the session; later mismatches are logged only.
func (s *session) recordCRC(role int, frame, crc uint32) *desyncInfo {
	e := s.crcPend[frame]
	if e == nil {
		if len(s.crcPend) >= maxCRCPending {
			s.evictLowestCRC()
		}
		e = &crcEntry{}
		s.crcPend[frame] = e
	}
	if e.has[role] {
		return nil // first report per player per frame wins
	}
	e.has[role] = true
	e.crc[role] = crc
	if !e.has[0] || !e.has[1] {
		return nil
	}
	delete(s.crcPend, frame)
	s.crcCompared++
	if e.crc[0] == e.crc[1] {
		return nil
	}
	s.crcMismatchN++
	if len(s.crcMismatches) < maxRecordedMismatch {
		s.crcMismatches = append(s.crcMismatches, mismatchRec{Frame: frame, CRC1: e.crc[0], CRC2: e.crc[1]})
	}
	if s.desyncSent {
		return nil
	}
	s.desyncSent = true
	return &desyncInfo{frame: frame, crc1: e.crc[0], crc2: e.crc[1]}
}

func (s *session) evictLowestCRC() {
	var lowest uint32
	first := true
	for f := range s.crcPend {
		if first || f < lowest {
			lowest = f
			first = false
		}
	}
	delete(s.crcPend, lowest)
	s.crcEvicted++
}

func (s *session) protoErr(format string, args ...any) {
	if len(s.protoErrs) >= maxRecordedErrors {
		s.protoErrDropped++
		return
	}
	s.protoErrs = append(s.protoErrs, fmt.Sprintf(format, args...))
}

// snapshot builds the log record. Call it after the session has ended.
func (s *session) snapshot() *logFile {
	l := &logFile{
		SessionID:      fmt.Sprintf("%016x", s.id),
		StartedAt:      s.started.UTC().Format(time.RFC3339Nano),
		EndedAt:        s.endedAt.UTC().Format(time.RFC3339Nano),
		GameID:         s.ident.gameID,
		BuildHash:      hex.EncodeToString(s.ident.buildHash[:]),
		ROMCRCs:        s.ident.romCRCs,
		Seed:           fmt.Sprintf("%016x", s.seed),
		InputDelay:     s.inputDelay,
		PlayerNames:    s.names,
		Inputs:         [2]inputLog{s.inputs[0], s.inputs[1]},
		Results:        s.results,
		ResultAck:      s.ack,
		ProtocolErrors: s.protoErrs,
		EndReason:      s.endReason,
		EndedBy:        s.endedBy,
	}
	l.ProtocolErrorsDropped = s.protoErrDropped
	if s.syncBlob != nil {
		h := hex.EncodeToString(s.syncBlob)
		l.SyncBlob = &h
	}
	if l.ProtocolErrors == nil {
		l.ProtocolErrors = []string{}
	}
	l.CRC = crcLog{
		Compared:      s.crcCompared,
		MismatchCount: s.crcMismatchN,
		Mismatches:    s.crcMismatches,
		Unmatched:     len(s.crcPend),
		Evicted:       s.crcEvicted,
	}
	if l.CRC.Mismatches == nil {
		l.CRC.Mismatches = []mismatchRec{}
	}
	l.Chat = s.chat
	if l.Chat == nil {
		l.Chat = []chatRec{}
	}
	l.ChatDropped = s.chatDropped
	return l
}

// logFile is the per-session JSON record written to <logdir>/<session_id>.json.
type logFile struct {
	SessionID             string             `json:"session_id"`
	StartedAt             string             `json:"started_at"`
	EndedAt               string             `json:"ended_at"`
	GameID                string             `json:"game_id"`
	BuildHash             string             `json:"build_hash"`
	ROMCRCs               [numROMCRCs]uint32 `json:"rom_crcs"`
	Seed                  string             `json:"seed"`
	InputDelay            uint8              `json:"input_delay"`
	PlayerNames           [2]string          `json:"player_names"`
	Inputs                [2]inputLog        `json:"inputs"`
	CRC                   crcLog             `json:"crc"`
	Results               [2]*resultRec      `json:"results"`
	ResultAck             *ackRec            `json:"result_ack"`
	SyncBlob              *string            `json:"sync_blob"`
	ProtocolErrors        []string           `json:"protocol_errors"`
	ProtocolErrorsDropped int                `json:"protocol_errors_dropped"`
	EndReason             string             `json:"end_reason"`
	EndedBy               int                `json:"ended_by"`
	Chat                  []chatRec          `json:"chat"`
	ChatDropped           [2]int             `json:"chat_dropped"`
}

type crcLog struct {
	Compared      int           `json:"compared"`
	MismatchCount int           `json:"mismatch_count"`
	Mismatches    []mismatchRec `json:"mismatches"`
	Unmatched     int           `json:"unmatched"`
	Evicted       int           `json:"evicted"`
}

// inputLog is a dense array from frame 0. Missing frames are null.
type inputLog []int32

func (l inputLog) MarshalJSON() ([]byte, error) {
	buf := []byte{'['}
	for i, v := range l {
		if i > 0 {
			buf = append(buf, ',')
		}
		if v < 0 {
			buf = append(buf, "null"...)
		} else {
			buf = strconv.AppendInt(buf, int64(v), 10)
		}
	}
	return append(buf, ']'), nil
}

// writeLog writes the record to dir/<session_id>.json through a temp file.
func writeLog(dir string, l *logFile) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	b, err := json.MarshalIndent(l, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(dir, l.SessionID+".json")
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, append(b, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func beUint64(b []byte) uint64 {
	var v uint64
	for _, x := range b {
		v = v<<8 | uint64(x)
	}
	return v
}
