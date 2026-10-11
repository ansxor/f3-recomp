package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"net"
	"net/netip"
	"time"
)

const (
	udpMagic       = "N3P1"
	udpRatePerSec  = 300
	udpBurst       = 300
	maxRateBuckets = 4096
	udpReadBufSize = 2048
)

// sign returns the first 16 bytes of HMAC-SHA256(session key, msg).
func (sess *session) sign(msg []byte) []byte {
	m := hmac.New(sha256.New, sess.key[:])
	m.Write(msg)
	return m.Sum(nil)[:udpTagLen]
}

// packet builds a tagged datagram for player and kind.
func (sess *session) packet(player, kind byte, body []byte) []byte {
	b := make([]byte, 0, udpHeaderLen+len(body)+udpTagLen)
	b = append(b, udpMagic...)
	b = binary.BigEndian.AppendUint64(b, sess.id)
	b = append(b, player, kind)
	b = append(b, body...)
	return append(b, sess.sign(b)...)
}

func (s *Server) udpLoop() {
	buf := make([]byte, udpReadBufSize)
	lim := newRateLimiter()
	for {
		n, src, err := s.udp.ReadFromUDPAddrPort(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		src = netip.AddrPortFrom(src.Addr().Unmap(), src.Port())
		if !lim.allow(src, time.Now()) {
			continue
		}
		var e effects
		s.mu.Lock()
		s.handleUDPLocked(buf[:n], src, &e)
		s.mu.Unlock()
		s.flush(&e)
	}
}

// handleUDPLocked drops silently on bad magic, size, player, kind, unknown
// session, or bad tag. Only authenticated packets with bad bodies are logged.
func (s *Server) handleUDPLocked(pkt []byte, src netip.AddrPort, e *effects) {
	if len(pkt) < udpHeaderLen+udpTagLen || len(pkt) > maxUDPPacket {
		return
	}
	if string(pkt[:4]) != udpMagic {
		return
	}
	sid := binary.BigEndian.Uint64(pkt[4:12])
	player := pkt[12]
	kind := pkt[13]
	if player > 1 || (kind != kindRegister && kind != kindInputs) {
		return
	}
	sess := s.sessions[sid]
	if sess == nil || sess.ended {
		return
	}
	msg := pkt[:len(pkt)-udpTagLen]
	if !hmac.Equal(sess.sign(msg), pkt[len(pkt)-udpTagLen:]) {
		return
	}
	body := msg[udpHeaderLen:]
	p := int(player)
	switch kind {
	case kindRegister:
		s.onRegisterLocked(sess, p, src, body, e)
	case kindInputs:
		s.onInputsLocked(sess, p, body, pkt, e)
	}
}

func (s *Server) onRegisterLocked(sess *session, p int, src netip.AddrPort, body []byte, e *effects) {
	if len(body) != 0 {
		sess.protoErr("register from player %d: body must be empty", p)
		return
	}
	sess.udpAddr[p] = src
	sess.registered[p] = true
	e.udp = append(e.udp, outPacket{data: sess.packet(byte(p), kindRegister, nil), to: src})
	s.maybeGoLocked(sess)
}

// onInputsLocked validates the whole packet before it changes the log, so a
// rejected packet leaves no partial state. A valid packet is forwarded
// byte-for-byte to the other player's registered address, including duplicates.
func (s *Server) onInputsLocked(sess *session, p int, body []byte, pkt []byte, e *effects) {
	if !sess.registered[p] {
		return
	}
	if len(body) < 5 {
		sess.protoErr("inputs from player %d: body too short", p)
		return
	}
	first := binary.BigEndian.Uint32(body[0:4])
	n := int(body[4])
	if n < 1 || n > maxInputs || len(body) != 5+2*n {
		sess.protoErr("inputs from player %d: bad count %d", p, n)
		return
	}
	if uint64(first)+uint64(n) > maxSessionFrames {
		sess.protoErr("inputs from player %d: frame %d out of range", p, first)
		return
	}
	lg := sess.inputs[p]
	for i := range n {
		f := int(first) + i
		v := binary.BigEndian.Uint16(body[5+2*i:])
		if f < len(lg) && lg[f] >= 0 && uint16(lg[f]) != v {
			sess.protoErr("inputs from player %d: conflicting input at frame %d", p, f)
			return
		}
	}
	for len(lg) < int(first)+n {
		lg = append(lg, -1)
	}
	for i := range n {
		lg[int(first)+i] = int32(binary.BigEndian.Uint16(body[5+2*i:]))
	}
	sess.inputs[p] = lg

	peer := 1 - p
	if sess.registered[peer] {
		e.udp = append(e.udp, outPacket{data: append([]byte(nil), pkt...), to: sess.udpAddr[peer]})
	}
}

// rateLimiter is a per-source token bucket. It is used only by the UDP read
// loop, so it needs no lock.
type rateLimiter struct {
	buckets   map[netip.AddrPort]*bucket
	lastPrune time.Time
}

type bucket struct {
	tokens float64
	last   time.Time
}

func newRateLimiter() *rateLimiter {
	return &rateLimiter{buckets: make(map[netip.AddrPort]*bucket)}
}

func (l *rateLimiter) allow(src netip.AddrPort, now time.Time) bool {
	if now.Sub(l.lastPrune) >= time.Second {
		l.prune(now)
		l.lastPrune = now
	}
	b := l.buckets[src]
	if b == nil {
		if len(l.buckets) >= maxRateBuckets {
			return false
		}
		b = &bucket{tokens: udpBurst, last: now}
		l.buckets[src] = b
	}
	elapsed := now.Sub(b.last).Seconds()
	b.tokens = min(udpBurst, b.tokens+elapsed*udpRatePerSec)
	b.last = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// prune drops buckets idle for over a second. Such a bucket has refilled to
// full burst, so removing it changes nothing.
func (l *rateLimiter) prune(now time.Time) {
	for k, b := range l.buckets {
		if now.Sub(b.last) > time.Second {
			delete(l.buckets, k)
		}
	}
}
