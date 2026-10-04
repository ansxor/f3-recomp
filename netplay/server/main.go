package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	var (
		addrFlag      = flag.String("addr", "127.0.0.1:9000", "UDP listen address")
		portFlag      = flag.Int("port", 0, "UDP listen port (overrides addr port if non-zero)")
		rttFlag       = flag.Duration("rtt", 0, "Simulated round-trip time (delay = rtt / 2)")
		delayFlag     = flag.Duration("delay", 0, "Simulated one-way network delay")
		jitterFlag    = flag.Duration("jitter", 0, "Simulated network jitter")
		lossFlag      = flag.Float64("loss", 0.0, "Simulated packet loss rate (0.0 to 1.0)")
		reorderFlag   = flag.Float64("reorder", 0.0, "Simulated packet reorder rate (0.0 to 1.0)")
		duplicateFlag = flag.Float64("duplicate", 0.0, "Simulated packet duplication rate (0.0 to 1.0)")
		dupAliasFlag  = flag.Float64("dup", 0.0, "Alias for -duplicate")
		seedFlag      = flag.Int64("seed", 0, "Deterministic PRNG seed for network impairment (0 for randomized)")
	)
	flag.Parse()

	addr := *addrFlag
	if *portFlag > 0 {
		addr = fmt.Sprintf("127.0.0.1:%d", *portFlag)
	}

	delay := *delayFlag
	if delay == 0 && *rttFlag > 0 {
		delay = *rttFlag / 2
	}

	dup := *duplicateFlag
	if dup == 0.0 && *dupAliasFlag > 0.0 {
		dup = *dupAliasFlag
	}

	impConfig := ImpairmentConfig{
		Delay:     delay,
		Jitter:    *jitterFlag,
		Loss:      *lossFlag,
		Duplicate: dup,
		Reorder:   *reorderFlag,
		Seed:      *seedFlag,
	}

	srvConfig := ServerConfig{
		Addr:       addr,
		Impairment: impConfig,
	}

	server, err := NewServer(srvConfig)
	if err != nil {
		log.Fatalf("Failed to start server: %v", err)
	}
	defer server.Close()

	log.Printf("F3 Netplay Relay Server listening on %s", server.LocalAddr())
	if impConfig.Delay > 0 || impConfig.Jitter > 0 || impConfig.Loss > 0 || impConfig.Duplicate > 0 || impConfig.Reorder > 0 {
		log.Printf("Network impairment enabled: delay=%v jitter=%v loss=%.1f%% dup=%.1f%% reorder=%.1f%% seed=%d",
			impConfig.Delay, impConfig.Jitter, impConfig.Loss*100, impConfig.Duplicate*100, impConfig.Reorder*100, impConfig.Seed)
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	<-sigCh

	log.Printf("Shutting down server...")
}
