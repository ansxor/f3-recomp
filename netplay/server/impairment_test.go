package main

import (
	"container/heap"
	"net"
	"testing"
	"time"
)

func awaitDelivery(t *testing.T, delivered <-chan time.Time) time.Time {
	t.Helper()
	select {
	case at := <-delivered:
		return at
	case <-time.After(2 * time.Second):
		t.Fatal("scheduled packet not delivered")
		return time.Time{}
	}
}

func TestImpairmentDelay(t *testing.T) {
	delivered := make(chan time.Time, 1)
	imp := NewImpairer(ImpairmentConfig{Delay: 40 * time.Millisecond, Seed: 12345},
		func(_ *net.UDPAddr, _ []byte) error { delivered <- time.Now(); return nil })
	defer imp.Close()
	start := time.Now()
	imp.Send(&net.UDPAddr{}, []byte("hello"))
	if elapsed := awaitDelivery(t, delivered).Sub(start); elapsed < 40*time.Millisecond {
		t.Fatalf("packet delivered before its deadline: %v", elapsed)
	}
}

func TestImpairmentLoss(t *testing.T) {
	delivered := make(chan time.Time, 1000)
	imp := NewImpairer(ImpairmentConfig{Loss: .30, Seed: 42},
		func(_ *net.UDPAddr, _ []byte) error { delivered <- time.Now(); return nil })
	defer imp.Close()
	for range 1000 {
		imp.Send(&net.UDPAddr{}, []byte("ping"))
	}
	imp.mu.Lock()
	expected := 1000 - int(imp.TotalDropped)
	imp.mu.Unlock()
	if expected < 620 || expected > 780 {
		t.Fatalf("unexpected delivery count for seeded 30%% loss: %d/1000", expected)
	}
	for range expected {
		awaitDelivery(t, delivered)
	}
}

func TestImpairmentDuplicate(t *testing.T) {
	delivered := make(chan time.Time, 40)
	imp := NewImpairer(ImpairmentConfig{Duplicate: 1, Seed: 999},
		func(_ *net.UDPAddr, _ []byte) error { delivered <- time.Now(); return nil })
	defer imp.Close()
	for range 20 {
		imp.Send(&net.UDPAddr{}, []byte("dup"))
	}
	for range 40 {
		awaitDelivery(t, delivered)
	}
	imp.mu.Lock()
	defer imp.mu.Unlock()
	if imp.TotalSent != 40 || imp.TotalDuplicated != 20 || len(imp.queue) != 0 {
		t.Fatalf("duplicate delivery totals: sent=%d duplicated=%d queued=%d",
			imp.TotalSent, imp.TotalDuplicated, len(imp.queue))
	}
}

func TestHeapDeadlineOrder(t *testing.T) {
	// Close deadlines must remain totally ordered; a tolerance-based comparator
	// can let a later packet block one whose deadline has already passed.
	base := time.Unix(0, 0)
	queue := packetHeap{}
	for i, delay := range []time.Duration{2500, 1000, 1900, 1000} {
		heap.Push(&queue, &scheduledPacket{deliveryTime: base.Add(delay * time.Microsecond),
			scheduledDelay: time.Duration(10-i) * time.Millisecond, seq: uint64(i)})
	}
	previous := base
	var seq uint64
	for queue.Len() != 0 {
		next := heap.Pop(&queue).(*scheduledPacket)
		if next.deliveryTime.Before(previous) || (next.deliveryTime.Equal(previous) && next.seq < seq) {
			t.Fatal("packet deadline ordering is not transitive and stable")
		}
		previous, seq = next.deliveryTime, next.seq
	}
}
