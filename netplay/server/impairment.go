package main

import (
	"container/heap"
	"math/rand/v2"
	"net"
	"sync"
	"time"
)

type ImpairmentConfig struct {
	Delay     time.Duration // Base one-way delay
	Jitter    time.Duration // Max jitter variation
	Loss      float64       // Loss probability [0.0, 1.0]
	Duplicate float64       // Duplication probability [0.0, 1.0]
	Reorder   float64       // Reorder probability [0.0, 1.0]
	Seed      int64         // Deterministic seed (0 for non-deterministic)
}

type scheduledPacket struct {
	deliveryTime   time.Time
	scheduledDelay time.Duration
	seq            uint64
	addr           *net.UDPAddr
	data           []byte
	index          int
}

type packetHeap []*scheduledPacket

func (h packetHeap) Len() int { return len(h) }
func (h packetHeap) Less(i, j int) bool {
	if h[i].deliveryTime.Equal(h[j].deliveryTime) {
		return h[i].seq < h[j].seq
	}
	return h[i].deliveryTime.Before(h[j].deliveryTime)
}
func (h packetHeap) Swap(i, j int) {
	h[i], h[j] = h[j], h[i]
	h[i].index = i
	h[j].index = j
}
func (h *packetHeap) Push(x any) {
	n := len(*h)
	item := x.(*scheduledPacket)
	item.index = n
	*h = append(*h, item)
}
func (h *packetHeap) Pop() any {
	old := *h
	n := len(old)
	item := old[n-1]
	old[n-1] = nil
	item.index = -1
	*h = old[:n-1]
	return item
}

type Impairer struct {
	config ImpairmentConfig
	sendFn func(addr *net.UDPAddr, data []byte) error

	mu     sync.Mutex
	rng    *rand.Rand
	seq    uint64
	queue  packetHeap
	wakeCh chan struct{}
	stopCh chan struct{}
	wg     sync.WaitGroup

	// Metrics
	TotalSent       uint64
	TotalDropped    uint64
	TotalDuplicated uint64
	TotalReordered  uint64
}

func NewImpairer(cfg ImpairmentConfig, sendFn func(addr *net.UDPAddr, data []byte) error) *Impairer {
	var pcg *rand.PCG
	if cfg.Seed != 0 {
		pcg = rand.NewPCG(uint64(cfg.Seed), uint64(cfg.Seed)^0x5DEECE66D)
	} else {
		now := uint64(time.Now().UnixNano())
		pcg = rand.NewPCG(now, now^0x9E3779B97F4A7C15)
	}
	imp := &Impairer{
		config: cfg,
		sendFn: sendFn,
		rng:    rand.New(pcg),
		wakeCh: make(chan struct{}, 1),
		stopCh: make(chan struct{}),
	}
	heap.Init(&imp.queue)

	imp.wg.Add(1)
	go imp.worker()

	return imp
}

const MaxImpairmentQueue = 2048

func (imp *Impairer) IsEnabled() bool {
	return imp.config.Delay > 0 || imp.config.Jitter > 0 || imp.config.Loss > 0 ||
		imp.config.Duplicate > 0 || imp.config.Reorder > 0
}

func (imp *Impairer) Send(addr *net.UDPAddr, data []byte) {
	if !imp.IsEnabled() {
		_ = imp.sendFn(addr, data)
		imp.mu.Lock()
		imp.TotalSent++
		imp.mu.Unlock()
		return
	}

	imp.mu.Lock()
	defer imp.mu.Unlock()

	// Bounded queue: prevent memory exhaustion under severe delay or packet flood
	if len(imp.queue) >= MaxImpairmentQueue {
		imp.TotalDropped++
		return
	}
	// Check packet loss
	if imp.config.Loss > 0 && imp.rng.Float64() < imp.config.Loss {
		imp.TotalDropped++
		return
	}

	now := time.Now()
	delay := imp.config.Delay

	// Calculate jitter: uniform in [-Jitter, +Jitter], clamped at >= 0
	if imp.config.Jitter > 0 {
		jitterSpan := int64(imp.config.Jitter) * 2
		j := time.Duration(imp.rng.Int64N(jitterSpan+1)) - imp.config.Jitter
		delay += j
		if delay < 0 {
			delay = 0
		}
	}

	// Check reorder (add additional delay to this packet)
	reordered := false
	if imp.config.Reorder > 0 && imp.rng.Float64() < imp.config.Reorder {
		extra := imp.config.Delay + imp.config.Jitter
		if extra == 0 {
			extra = 20 * time.Millisecond
		}
		delay += extra
		reordered = true
		imp.TotalReordered++
	}

	// Check duplicate
	duplicate := false
	if imp.config.Duplicate > 0 && imp.rng.Float64() < imp.config.Duplicate {
		duplicate = true
		imp.TotalDuplicated++
	}

	// Make copy of data
	pktCopy := make([]byte, len(data))
	copy(pktCopy, data)

	imp.seq++
	item := &scheduledPacket{
		deliveryTime:   now.Add(delay),
		scheduledDelay: delay,
		seq:            imp.seq,
		addr:           addr,
		data:           pktCopy,
	}
	heap.Push(&imp.queue, item)

	if duplicate && len(imp.queue) < MaxImpairmentQueue {
		dupCopy := make([]byte, len(data))
		copy(dupCopy, data)
		dupDelay := delay + 5*time.Millisecond
		imp.seq++
		dupItem := &scheduledPacket{
			deliveryTime:   now.Add(dupDelay),
			scheduledDelay: dupDelay,
			seq:            imp.seq,
			addr:           addr,
			data:           dupCopy,
		}
		heap.Push(&imp.queue, dupItem)
	}

	_ = reordered
	imp.notifyWorker()
}

func (imp *Impairer) notifyWorker() {
	select {
	case imp.wakeCh <- struct{}{}:
	default:
	}
}

func (imp *Impairer) worker() {
	defer imp.wg.Done()

	for {
		imp.mu.Lock()
		now := time.Now()
		for len(imp.queue) > 0 {
			earliest := imp.queue[0]
			if earliest.deliveryTime.After(now) {
				break
			}
			item := heap.Pop(&imp.queue).(*scheduledPacket)
			imp.TotalSent++
			imp.mu.Unlock()

			_ = imp.sendFn(item.addr, item.data)

			imp.mu.Lock()
			now = time.Now()
		}

		var waitDuration time.Duration
		if len(imp.queue) > 0 {
			waitDuration = imp.queue[0].deliveryTime.Sub(now)
			if waitDuration < 0 {
				waitDuration = 0
			}
		} else {
			waitDuration = 10 * time.Second
		}
		imp.mu.Unlock()

		timer := time.NewTimer(waitDuration)
		select {
		case <-imp.stopCh:
			timer.Stop()
			return
		case <-imp.wakeCh:
			timer.Stop()
		case <-timer.C:
		}
	}
}

func (imp *Impairer) Close() {
	close(imp.stopCh)
	imp.notifyWorker()
	imp.wg.Wait()

	// Drain any remaining packets
	imp.mu.Lock()
	for len(imp.queue) > 0 {
		item := heap.Pop(&imp.queue).(*scheduledPacket)
		_ = imp.sendFn(item.addr, item.data)
	}
	imp.mu.Unlock()
}
